#include "js_internal.hpp"

#include <algorithm>
#include <unordered_map>

namespace jetscript{

    namespace detail{

        struct NameCache{
            // ScriptData::serial of the target script (0 = empty)
            uint32_t serial = 0;
            // 0 none, 1 var, 2 const, 3 func
            uint8_t kind = 0;
            uint32_t index = 0;
        };

        // Per-VM data of a compiled script: inline caches for dynamic member access (by name table index) and singletons.
        struct ScriptRt{
            uint32_t serial = 0;
            uint32_t users = 0;
            std::vector<NameCache> cache;
            std::vector<ObjectId> singletons;
        };

        struct Instance{
            ObjectId id = 0;
            std::shared_ptr<const Script> script;
            const ScriptData* sd = nullptr;
            ScriptRt* rt = nullptr;
            std::vector<Value> members;
            // frames currently executing on this instance
            uint32_t busy = 0;
            // detached while running
            bool dead = false;
        };

        struct Frame{
            const FunctionData* fn;
            Instance* self;
            // return address (caller frames) / faulting instruction (innermost after an error)
            uint32_t pc;
            uint32_t bp;
            // result replaces the receiver slot below the arguments
            bool receiver;
        };

        // A signal of `source` calls `method` on `target` (both names are Strings).
        struct Connection{
            ObjectId source = 0, target = 0;
            Value signal, method;
            bool one_shot = false;
        };
        constexpr size_t kMaxConnections = 4096;

        // Frames live on the heap, so script-to-script calls never recurse on the C++ stack.
        struct Exec{
            std::vector<Value> stack;
            std::vector<Frame> frames;
        };

    }

    using namespace detail;

    bool classLists(const Host& host, ObjectId id, std::string_view name, bool signal){
        std::string_view c = host.classOf(id);

        for(int guard = 0; guard < 64 && !c.empty(); ++guard){
            const ClassInfo* ci = host.findClass(c);

            if(!ci) return false;

            if(signal){
                for(const SignalInfo& s : ci->signals)
                    if(s.name == name) return true;
            } else {
                for(const MethodInfo& m : ci->methods)
                    if(m.name == name) return true;
            }

            c = ci->base;
        }

        return false;
    }

    struct VMState{
        explicit VMState(Host& h) : host(h){
            bctx.host = &h;
            bctx.rng = &rng;
        }
        ~VMState();

        Host& host;
        std::unordered_map<ObjectId, Instance*> instances;
        std::unordered_map<uint32_t, std::unique_ptr<ScriptRt>> rts;
        std::vector<Instance*> zombies;
        std::vector<RuntimeError> errors;
        std::vector<std::unique_ptr<Exec>> execs;
        int depth = 0;
        uint32_t limit = kDefaultInstructionLimit;
        int64_t budget = 0;
        Rng rng;
        BuiltinCtx bctx;
        std::vector<Connection> connections;

        Instance* find(ObjectId id) const{
            auto it = instances.find(id);
            return it == instances.end() ? nullptr : it->second;
        }
        void destroyInstance(Instance* inst);
        void removeInstance(ObjectId id);
        void collectZombies();
        ScriptRt* runtimeFor(const ScriptData* sd);
        // false when the error list is full and the message was dropped
        bool record(std::string message, const Instance* inst, const char* function);
        bool invoke(Instance* inst, const FunctionData* fn, const Value* args, int argc, Value* result);
        bool execute(Exec& ex, Value* result);
        void unwind(Exec& ex, const std::string& message);

        std::string typeDesc(const Value& v) const;
        bool is_class(ObjectId id, std::string_view cls) const;
        const NameCache& resolve(Instance* self, uint32_t ne, const Instance* target);
        bool get_named(Instance* self, uint32_t ne, const Value& base, Value& out, std::string& err);
        bool set_named(Instance* self, uint32_t ne, Value& base, const Value& v, std::string& err);
        bool get_index(const Value& base, const Value& idx, Value& out, std::string& err);
        bool setIndex(Value& base, const Value& idx, const Value& v, std::string& err);
        bool setVar(Instance* inst, uint32_t index, const Value& v, std::string& err);

        // Signals (VMState::emit and friends below)
        bool classHas(ObjectId id, std::string_view name, bool signal) const;
        bool has_signal(ObjectId id, std::string_view name) const;
        bool has_method(ObjectId id, std::string_view name) const;
        std::string boundText(ObjectId id, std::string_view name) const;
        int findConnection(ObjectId source, std::string_view signal, ObjectId target, std::string_view method) const;
        bool connect(ObjectId source, const Value& signal, ObjectId target, const Value& method, Int flags, std::string& err);
        bool disconnect(ObjectId source, std::string_view signal, ObjectId target, std::string_view method, std::string& err);
        void emit(ObjectId source, std::string_view signal, const Value* args, int argc);
        bool callBound(ObjectId target, std::string_view method, const Value* args, int argc, Value& out, std::string& err);
        bool signalCall(int sm, std::string_view name, const Value& recv, const Value* args, int argc, Value& out,
                         std::string& err);
    };

    VMState::~VMState(){
        for(auto& kv : instances) destroyInstance(kv.second);

        instances.clear();

        for(Instance* z : zombies) destroyInstance(z);

        zombies.clear();
    }

    void VMState::destroyInstance(Instance* inst){
        if(inst->rt && --inst->rt->users == 0) rts.erase(inst->rt->serial);

        delete inst;
    }

    void VMState::removeInstance(ObjectId id){
        auto it = instances.find(id);

        if(it == instances.end()) return;

        Instance* inst = it->second;
        instances.erase(it);

        if(inst->busy > 0){
            inst->dead = true;
            zombies.push_back(inst);
        } else {
            destroyInstance(inst);
        }
    }

    void VMState::collectZombies(){
        size_t keep = 0;

        for(size_t i = 0; i < zombies.size(); ++i){
            if(zombies[i]->busy == 0) destroyInstance(zombies[i]);
            else zombies[keep++] = zombies[i];
        }

        zombies.resize(keep);
    }

    ScriptRt* VMState::runtimeFor(const ScriptData* sd){
        auto it = rts.find(sd->serial);

        if(it != rts.end()) return it->second.get();

        std::unique_ptr<ScriptRt> rt(new ScriptRt());
        rt->serial = sd->serial;
        rt->cache.resize(sd->names.size());
        rt->singletons.assign(sd->names.size(), 0);
        ScriptRt* p = rt.get();
        rts.emplace(sd->serial, std::move(rt));
        return p;
    }

    bool VMState::record(std::string message, const Instance* inst, const char* function){
        if(errors.size() >= 64) return false;

        RuntimeError e;
        e.message = std::move(message);

        if(inst){
            e.path = inst->sd->path;
            e.object = inst->id;
        }

        e.function = function ? function : "";
        errors.push_back(std::move(e));
        return true;
    }

    std::string VMState::typeDesc(const Value& v) const{
        if(v.type() == Type::Nil) return "null instance";

        if(v.type() == Type::Object){
            std::string_view c = host.classOf(Heap::o(v));

            if(c.empty()) return "previously freed";

            return std::string(c);
        }

        return type_name(v.type());
    }

    bool VMState::is_class(ObjectId id, std::string_view cls) const{
        std::string_view c = host.classOf(id);

        for(int guard = 0; guard < 64 && !c.empty(); ++guard){
            if(c == cls) return true;

            const ClassInfo* ci = host.findClass(c);

            if(!ci || ci->base.empty()) return false;

            c = ci->base;
        }

        return false;
    }

    static bool isValueType(const Value& v){
        return v.type() == Type::Vector3 || v.type() == Type::Vector2 || v.type() == Type::Color || v.type() == Type::String;
    }

    static std::string keyText(const Value& k){
        std::string s;
        appendValue(s, k, nullptr, 0, false);
        return s;
    }

    const NameCache& VMState::resolve(Instance* self, uint32_t ne, const Instance* target){
        NameCache& c = self->rt->cache[ne];

        if(c.serial != target->sd->serial){
            const SymEntry* s = target->sd->findSymbol(self->sd->names[ne].name);
            c.serial = target->sd->serial;
            c.kind = s ? uint8_t(uint8_t(s->kind) + 1) : 0;
            c.index = s ? s->index : 0;
        }

        return c;
    }

    bool VMState::setVar(Instance* inst, uint32_t index, const Value& v, std::string& err){
        const VarData& vd = inst->sd->vars[index];

        if(!vd.typed){
            inst->members[index] = v;
            return true;
        }

        Value c = v;
        bool ok = convertTo(vd.type, c);

        if(ok && !vd.class_name.empty() && c.type() == Type::Object && !is_class(Heap::o(c), vd.class_name)) ok = false;

        if(!ok){
            err = std::string("Trying to assign value of type '") + typeDesc(v) + "' to a variable of type '" +
                  (vd.class_name.empty() ? std::string(type_name(vd.type)) : vd.class_name) + "'.";
            return false;
        }

        inst->members[index] = std::move(c);
        return true;
    }

    bool VMState::get_named(Instance* self, uint32_t ne, const Value& base, Value& out, std::string& err){
        const NameEntry& e = self->sd->names[ne];

        switch(base.type()){
            case Type::Vector3:
                if(e.field >= 0 && e.field <= 2){
                    out = Value(Heap::comp(base, e.field));
                    return true;
                }
                break;
            case Type::Vector2:
                if(e.field >= 0 && e.field <= 1){
                    out = Value(Heap::comp(base, e.field));
                    return true;
                }
                break;
            case Type::Color:
                if(e.field >= 3){
                    out = Value(e.field == 6 ? Real(1) : Heap::comp(base, e.field - 3));
                    return true;
                }
                break;
            case Type::Dictionary:{
                const DictObj* d = Heap::dict(base);
                int i = d->findStr(e.name);

                if(i >= 0){
                    out = d->entries[size_t(i)].value;
                    return true;
                }
                break;
            }
            case Type::Object:{
                ObjectId id = Heap::o(base);

                if(Instance* inst = find(id)){
                    const NameCache& c = resolve(self, ne, inst);

                    if(c.kind == 1){
                        out = inst->members[c.index];
                        return true;
                    }

                    if(c.kind == 2){
                        out = inst->sd->consts[c.index].second;
                        return true;
                    }

                    if(c.kind == 3 || c.kind == 4){
                        out = c.kind == 3 ? Value::callable(id, e.name) : Value::signal(id, e.name);
                        return true;
                    }
                }

                if(e.pid >= 0){
                    err.clear();
                    Status st = host.getProperty(id, e.pid, out, err);

                    if(st == Status::Ok) return true;

                    if(st == Status::Error){
                        if(err.empty()) err = "Error reading property '" + e.name + "'.";

                        return false;
                    }
                }

                //$Timer.timeout, $Enemy.queue_free
                if(classHas(id, e.name, true)){
                    out = Value::signal(id, e.name);
                    return true;
                }

                if(e.mid >= 0 && classHas(id, e.name, false)){
                    out = Value::callable(id, e.name);
                    return true;
                }
                break;
            }
            default: break;
        }

        err = "Invalid access to property or key '" + e.name + "' on a base object of type '" + typeDesc(base) + "'.";
        return false;
    }

    bool VMState::set_named(Instance* self, uint32_t ne, Value& base, const Value& v, std::string& err){
        const NameEntry& e = self->sd->names[ne];

        switch(base.type()){
            case Type::Vector3:
                if(e.field >= 0 && e.field <= 2 && v.isNumber()){
                    Heap::setComp(base, e.field, v.asReal());
                    return true;
                }
                break;
            case Type::Vector2:
                if(e.field >= 0 && e.field <= 1 && v.isNumber()){
                    Heap::setComp(base, e.field, v.asReal());
                    return true;
                }
                break;
            case Type::Color:
                if(e.field >= 3 && v.isNumber()){
                    //alpha is ignored
                    if(e.field != 6) Heap::setComp(base, e.field - 3, v.asReal());

                    return true;
                }
                break;
            case Type::Dictionary:{
                DictObj* d = Heap::dict(base);

                if(d->read_only){
                    err = "Dictionary is in read-only state.";
                    return false;
                }

                d->set(Value(e.name), v);
                return true;
            }
            case Type::Object:{
                ObjectId id = Heap::o(base);

                if(Instance* inst = find(id)){
                    const NameCache& c = resolve(self, ne, inst);

                    if(c.kind == 1) return setVar(inst, c.index, v, err);

                    if(c.kind == 2){
                        err = "Cannot assign a new value to a constant ('" + e.name + "').";
                        return false;
                    }
                }

                if(e.pid >= 0){
                    err.clear();
                    Status st = host.setProperty(id, e.pid, v, err);

                    if(st == Status::Ok) return true;

                    if(st == Status::Error){
                        if(err.empty()) err = "Error assigning property '" + e.name + "'.";

                        return false;
                    }
                }
                break;
            }
            default: break;
        }

        err = "Invalid assignment of property or key '" + e.name + "' with value of type '" + typeDesc(v) +
              "' on a base object of type '" + typeDesc(base) + "'.";
        return false;
    }

    static bool indexInt(const Value& idx, Int& out){
        if(idx.type() != Type::Int) return false;

        out = Heap::i(idx);
        return true;
    }

    bool VMState::get_index(const Value& base, const Value& idx, Value& out, std::string& err){
        Int i = 0;

        switch(base.type()){
            case Type::Array:{
                const auto& items = Heap::arr(base)->items;

                if(!indexInt(idx, i)) break;

                Int n = Int(items.size());
                Int j = i < 0 ? i + n : i;

                if(j < 0 || j >= n){
                    err = "Out of bounds get index '" + formatInt(i) + "' (on base: 'Array').";
                    return false;
                }

                out = items[size_t(j)];
                return true;
            }
            case Type::Dictionary:{
                const DictObj* d = Heap::dict(base);
                int k = d->find(idx);

                if(k < 0){
                    err = "Invalid access to key '" + keyText(idx) + "' on a base object of type 'Dictionary'.";
                    return false;
                }

                out = d->entries[size_t(k)].value;
                return true;
            }
            case Type::String:{
                if(!indexInt(idx, i)) break;

                std::string_view s = Heap::sv(base);
                Int n = Heap::str(base) ? Int(Heap::str(base)->ncp) : 0;
                Int j = i < 0 ? i + n : i;

                if(j < 0 || j >= n){
                    err = "Out of bounds get index '" + formatInt(i) + "' (on base: 'String').";
                    return false;
                }

                size_t a = utf8Offset(s, j), b = utf8Offset(s, j + 1);
                out = Value(s.substr(a, b - a));
                return true;
            }
            case Type::Vector3:
            case Type::Vector2:
            case Type::Color:{
                int f = -1;

                if(idx.type() == Type::String){
                    f = findBuiltinField(Heap::sv(idx));

                    if(base.type() == Type::Vector3 && f > 2) f = -1;

                    if(base.type() == Type::Vector2 && f > 1) f = -1;

                    if(base.type() == Type::Color) f = f >= 3 ? f - 3 : -1;
                } else if(indexInt(idx, i)){
                    Int n = base.type() == Type::Vector3 ? 3 : base.type() == Type::Vector2 ? 2 : 4;

                    if(i < 0 || i >= n){
                        err = "Out of bounds get index '" + formatInt(i) + "' (on base: '" + type_name(base.type()) + "').";
                        return false;
                    }

                    f = int(i);
                }

                if(f < 0) break;

                out = Value(f == 3 ? Real(1) : Heap::comp(base, f));
                return true;
            }
            case Type::Object:{
                if(idx.type() != Type::String) break;

                std::string_view name = Heap::sv(idx);
                ObjectId id = Heap::o(base);

                if(Instance* inst = find(id)){
                    if(const SymEntry* s = inst->sd->findSymbol(name)){
                        if(s->kind == SymKind::Var){ out = inst->members[s->index]; return true; }

                        if(s->kind == SymKind::Const){ out = inst->sd->consts[s->index].second; return true; }
                    }
                }

                int pid = host.propertyId(name);

                if(pid >= 0){
                    err.clear();
                    Status st = host.getProperty(id, pid, out, err);

                    if(st == Status::Ok) return true;

                    if(st == Status::Error) return false;
                }
                break;
            }
            default: break;
        }

        if(base.type() == Type::Array || base.type() == Type::String ||
            ((base.type() == Type::Vector3 || base.type() == Type::Vector2 || base.type() == Type::Color) && idx.type() != Type::String)){
            err = std::string("Invalid index type '") + type_name(idx.type()) + "' for a base of type '" + type_name(base.type()) + "'.";
            return false;
        }

        err = "Invalid access to property or key '" + keyText(idx) + "' on a base object of type '" + typeDesc(base) + "'.";
        return false;
    }

    bool VMState::setIndex(Value& base, const Value& idx, const Value& v, std::string& err){
        Int i = 0;

        switch(base.type()){
            case Type::Array:{
                ArrObj* a = Heap::arr(base);

                if(!indexInt(idx, i)) break;

                if(a->read_only){
                    err = "Array is in read-only state.";
                    return false;
                }

                Int n = Int(a->items.size());
                Int j = i < 0 ? i + n : i;

                if(j < 0 || j >= n){
                    err = "Out of bounds set index '" + formatInt(i) + "' (on base: 'Array').";
                    return false;
                }

                a->items[size_t(j)] = v;
                return true;
            }
            case Type::Dictionary:{
                DictObj* d = Heap::dict(base);

                if(d->read_only){
                    err = "Dictionary is in read-only state.";
                    return false;
                }

                if(!validDictKey(idx)){
                    err = std::string("Invalid key type '") + type_name(idx.type()) + "' for a Dictionary.";
                    return false;
                }

                d->set(idx, v);
                return true;
            }
            case Type::String:{
                if(!indexInt(idx, i)) break;

                if(v.type() != Type::String){
                    err = std::string("Invalid assignment of a value of type '") + type_name(v.type()) + "' to a String character.";
                    return false;
                }

                std::string_view s = Heap::sv(base);
                Int n = Heap::str(base) ? Int(Heap::str(base)->ncp) : 0;
                Int j = i < 0 ? i + n : i;

                if(j < 0 || j >= n){
                    err = "Out of bounds set index '" + formatInt(i) + "' (on base: 'String').";
                    return false;
                }

                size_t a = utf8Offset(s, j), b = utf8Offset(s, j + 1);
                std::string r;
                r.reserve(s.size() + Heap::sv(v).size());
                r.append(s.data(), a);
                r += Heap::sv(v);
                r.append(s.data() + b, s.size() - b);
                base = Value(r);
                return true;
            }
            case Type::Vector3:
            case Type::Vector2:
            case Type::Color:{
                int f = -1;

                if(idx.type() == Type::String){
                    f = findBuiltinField(Heap::sv(idx));

                    if(base.type() == Type::Vector3 && f > 2) f = -1;

                    if(base.type() == Type::Vector2 && f > 1) f = -1;

                    if(base.type() == Type::Color) f = f >= 3 ? f - 3 : -1;
                } else if(indexInt(idx, i)){
                    Int n = base.type() == Type::Vector3 ? 3 : base.type() == Type::Vector2 ? 2 : 4;

                    if(i < 0 || i >= n){
                        err = "Out of bounds set index '" + formatInt(i) + "' (on base: '" + type_name(base.type()) + "').";
                        return false;
                    }

                    f = int(i);
                }

                if(f < 0 || !v.isNumber()) break;

                if(f != 3) Heap::setComp(base, f, v.asReal());

                return true;
            }
            case Type::Object:{
                if(idx.type() != Type::String) break;

                std::string_view name = Heap::sv(idx);
                ObjectId id = Heap::o(base);

                if(Instance* inst = find(id)){
                    if(const SymEntry* s = inst->sd->findSymbol(name)){
                        if(s->kind == SymKind::Var) return setVar(inst, s->index, v, err);
                    }
                }

                int pid = host.propertyId(name);

                if(pid >= 0){
                    err.clear();
                    Status st = host.setProperty(id, pid, v, err);

                    if(st == Status::Ok) return true;

                    if(st == Status::Error) return false;
                }
                break;
            }
            default: break;
        }

        err = "Invalid assignment of property or key '" + keyText(idx) + "' with value of type '" + typeDesc(v) +
              "' on a base object of type '" + typeDesc(base) + "'.";
        return false;
    }

    bool VMState::classHas(ObjectId id, std::string_view name, bool signal) const{
        return signal ? host.has_signal(id, name) : host.has_method(id, name);
    }

    bool VMState::has_signal(ObjectId id, std::string_view name) const{
        if(Instance* inst = find(id)){
            const SymEntry* s = inst->sd->findSymbol(name);

            if(s && s->kind == SymKind::Signal) return true;
        }

        return classHas(id, name, true);
    }

    bool VMState::has_method(ObjectId id, std::string_view name) const{
        if(Instance* inst = find(id)){
            const SymEntry* s = inst->sd->findSymbol(name);

            if(s && s->kind == SymKind::Func) return true;
        }

        return classHas(id, name, false);
    }

    std::string VMState::boundText(ObjectId id, std::string_view name) const{
        return host.describe(id) + "::" + std::string(name);
    }

    int VMState::findConnection(ObjectId source, std::string_view signal, ObjectId target, std::string_view method) const{
        for(size_t i = 0; i < connections.size(); ++i){
            const Connection& c = connections[i];

            if(c.source == source && c.target == target && Heap::sv(c.signal) == signal && Heap::sv(c.method) == method)
                return int(i);
        }

        return -1;
    }

    bool VMState::connect(ObjectId source, const Value& signal, ObjectId target, const Value& method, Int flags, std::string& err){
        const std::string_view sig = Heap::sv(signal), name = Heap::sv(method);
        const std::string_view cls = host.classOf(source);

        if(cls.empty()){
            err = "Attempt to connect signal '" + std::string(sig) + "' of a previously freed instance.";
            return false;
        }

        if(!has_signal(source, sig)){
            err = "In Object of type '" + std::string(cls) + "': Attempt to connect nonexistent signal '" + std::string(sig) +
                  "' to callable '" + boundText(target, name) + "'.";
            return false;
        }

        if(flags & ~Int(2 | 4)){
            err = (flags & 1) ? "CONNECT_DEFERRED is not supported yet in JetScript: the callable runs when the signal is emitted."
                              : "Only the connect() flags CONNECT_ONE_SHOT and CONNECT_PERSIST are supported in JetScript.";
            return false;
        }

        if(host.classOf(target).empty()){
            err = "Cannot connect to '" + std::string(sig) + "': the callable's object was freed.";
            return false;
        }

        if(!has_method(target, name)){
            err = "Cannot connect to '" + std::string(sig) + "': '" + host.describe(target) + "' has no method '" + std::string(name) + "'.";
            return false;
        }

        if(findConnection(source, sig, target, name) >= 0){
            err = "Signal '" + std::string(sig) + "' is already connected to given callable '" + boundText(target, name) + "' in that object.";
            return false;
        }

        if(connections.size() >= kMaxConnections){
            err = "Too many signal connections (maximum " + formatInt(Int(kMaxConnections)) + ").";
            return false;
        }

        Connection c;
        c.source = source;
        c.target = target;
        c.signal = signal;
        c.method = method;
        c.one_shot = (flags & 4) != 0;
        connections.push_back(std::move(c));
        return true;
    }

    bool VMState::disconnect(ObjectId source, std::string_view signal, ObjectId target, std::string_view method, std::string& err){
        const int i = findConnection(source, signal, target, method);

        if(i < 0){
            err = "Attempt to disconnect a nonexistent connection from '" + host.describe(source) + "'. Signal: '" +
                  std::string(signal) + "', callable: '" + boundText(target, method) + "'.";
            return false;
        }

        connections.erase(connections.begin() + i);
        return true;
    }

    // Callees are gathered first: they may connect and disconnect while the signal is being emitted.
    void VMState::emit(ObjectId source, std::string_view signal, const Value* args, int argc){
        std::vector<std::pair<ObjectId, Value>> calls;
        size_t keep = 0;

        for(size_t i = 0; i < connections.size(); ++i){
            Connection& c = connections[i];
            const bool match = c.source == source && Heap::sv(c.signal) == signal;

            if(match) calls.emplace_back(c.target, c.method);

            if(match && c.one_shot) continue;

            if(keep != i) connections[keep] = std::move(c);

            ++keep;
        }

        connections.resize(keep);

        for(const auto& call : calls){
            Value out;
            std::string err;
            const std::string_view method = Heap::sv(call.second);

            if(!callBound(call.first, method, args, argc, out, err))
                record("Error calling from signal '" + std::string(signal) + "' to callable '" + boundText(call.first, method) +
                           "': " + err,
                       find(call.first), Heap::str(call.second)->chars());
        }
    }

    // A script function's errors are recorded by invoke(); an engine method's come back in err.
    bool VMState::callBound(ObjectId target, std::string_view method, const Value* args, int argc, Value& out, std::string& err){
        if(Instance* inst = find(target)){
            const SymEntry* s = inst->sd->findSymbol(method);

            if(s && s->kind == SymKind::Func){
                invoke(inst, &inst->sd->funcs[s->index], args, argc, &out);
                return true;
            }
        }

        if(host.classOf(target).empty()){
            err = "Attempt to call function '" + std::string(method) + "' on a previously freed instance.";
            return false;
        }

        const int mid = host.methodId(method);
        err.clear();
        const Status st = mid >= 0 ? host.callMethod(target, mid, args, argc, out, err) : Status::NotFound;

        if(st == Status::Ok) return true;

        if(st == Status::NotFound || err.empty())
            err = "Invalid call. Nonexistent function '" + std::string(method) + "' in base '" + typeDesc(Value::object(target)) + "'.";
        return false;
    }

    // Godot's connect(), disconnect(), is_connected(), emit_signal() and has_signal() on objects,
    // and the methods of Signal and Callable values.
    bool VMState::signalCall(int sm, std::string_view name, const Value& recv, const Value* args, int argc, Value& out,
                              std::string& err){
        const Type rt = recv.type();
        const std::string base = type_name(rt);
        int lo = 0, hi = 0;
        SType ret = kUnknown;

        if(!signalMethodInfo(sm, rt, lo, hi, ret)){
            err = "Invalid call. Nonexistent function '" + std::string(name) + "' in base '" + base + "'.";
            return false;
        }

        if(argc < lo || (hi >= 0 && argc > hi)){
            err = "Invalid call to function '" + std::string(name) + "' in base '" + base + "'. Expected " +
                  formatInt(argc < lo ? lo : hi) + " arguments.";
            return false;
        }

        auto badArg = [&](int i, const char* expected){
            err = "Invalid type in function '" + std::string(name) + "' in base '" + base + "'. Cannot convert argument " +
                  formatInt(i + 1) + " from " + type_name(args[i].type()) + " to " + expected + ".";
            return false;
        };

        auto nameOf = [](const Value& bound){
            Value text = Heap::make(Type::String, Heap::str(bound));
            Heap::retain(text);
            return text;
        };

        //the signal the call is about (source and name), then its callable argument (target and method).
        ObjectId source = 0, target = 0;
        Value signal, method;
        int next = 0;

        if(rt == Type::Object){
            source = Heap::o(recv);

            if(args[0].type() != Type::String || Heap::sv(args[0]).empty()) return badArg(0, "StringName");

            signal = args[0];
            next = 1;
        } else if(rt == Type::Signal){
            source = Heap::boundObject(recv);
            signal = nameOf(recv);
        } else {
            target = Heap::boundObject(recv);
            method = nameOf(recv);
        }

        if(sm == SM_connect || sm == SM_disconnect || sm == SM_is_connected){
            if(args[next].type() != Type::Callable) return badArg(next, "Callable");

            target = Heap::boundObject(args[next]);
            method = nameOf(args[next]);
        }

        if(sm <= SM_emit && host.classOf(source).empty()){
            err = "Attempt to call function '" + std::string(name) + "' on a previously freed instance.";
            return false;
        }

        switch(sm){
            case SM_connect:{
                Int flags = 0;

                if(argc > next + 1){
                    if(args[next + 1].type() != Type::Int) return badArg(next + 1, "int");

                    flags = Heap::i(args[next + 1]);
                }

                if(!connect(source, signal, target, method, flags, err)) return false;

                out = Value(Int(0));
                return true;
            }
            case SM_disconnect: return disconnect(source, Heap::sv(signal), target, Heap::sv(method), err);
            case SM_is_connected: out = Value(findConnection(source, Heap::sv(signal), target, Heap::sv(method)) >= 0); return true;
            case SM_has_signal: out = Value(has_signal(source, Heap::sv(signal))); return true;
            case SM_emit_signal:
            case SM_emit:
                if(!has_signal(source, Heap::sv(signal))){
                    err = "Can not emit non-existing signal \"" + std::string(Heap::sv(signal)) + "\".";
                    return false;
                }

                if(!connections.empty()) emit(source, Heap::sv(signal), args + next, argc - next);

                if(sm == SM_emit_signal) out = Value(Int(0));

                return true;
            case SM_get_name: out = signal; return true;
            case SM_get_method: out = method; return true;
            case SM_get_object:{
                const ObjectId id = rt == Type::Signal ? source : target;
                out = host.classOf(id).empty() ? Value() : Value::object(id);
                return true;
            }
            case SM_is_null: out = Value(false); return true;
            case SM_is_valid: out = Value(!host.classOf(target).empty() && has_method(target, Heap::sv(method))); return true;
            case SM_call: return callBound(target, Heap::sv(method), args, argc, out, err);
            case SM_callv:{
                if(args[0].type() != Type::Array) return badArg(0, "Array");

                //the callee may change the array
                const Value list = args[0];
                const std::vector<Value> items = Heap::arr(list)->items;
                return callBound(target, Heap::sv(method), items.data(), int(items.size()), out, err);
            }
            default: break;
        }

        err = "Invalid call. Nonexistent function '" + std::string(name) + "' in base '" + base + "'.";
        return false;
    }

    bool VMState::invoke(Instance* inst, const FunctionData* fn, const Value* args, int argc, Value* result){
        if(depth >= kMaxReentry){
            record("Too many nested script calls from the engine (maximum " + formatInt(kMaxReentry) + ").", inst, fn->name.c_str());
            return false;
        }

        if(argc < fn->nrequired || argc > fn->nparams){
            record("Invalid call to function '" + fn->name + "' in base '" + inst->sd->baseClass + " (" + inst->sd->path +
                       ")'. Expected " + formatInt(argc < fn->nrequired ? fn->nrequired : fn->nparams) + " arguments.",
                   inst, fn->name.c_str());
            return false;
        }

        if(depth == 0) budget = limit ? int64_t(limit) : INT64_MAX;

        if(int(execs.size()) <= depth) execs.emplace_back(new Exec());

        Exec& ex = *execs[size_t(depth)];
        ++depth;
        //nested entries (signals, _ready() from add_child()) usually run short callbacks: their stack starts smaller.
        size_t need = size_t(fn->frameSize) + 1;
        const size_t initial = depth == 1 ? 256 : 64;

        if(ex.stack.size() < need) ex.stack.resize(need < initial ? initial : need);

        for(int i = 0; i < argc; ++i) ex.stack[size_t(i)] = args[i];

        Frame f;
        f.fn = fn;
        f.self = inst;
        f.pc = fn->entries[size_t(argc - fn->nrequired)];
        f.bp = 0;
        f.receiver = false;
        ex.frames.push_back(f);
        ++inst->busy;
        bool ok = execute(ex, result);
        --depth;

        if(depth == 0) collectZombies();

        return ok;
    }

    void VMState::unwind(Exec& ex, const std::string& message){
        RuntimeError e;
        e.message = message;

        for(size_t i = ex.frames.size(); i-- > 0;){
            const Frame& f = ex.frames[i];
            StackFrame sf;
            sf.path = f.self->sd->path;
            sf.function = f.fn->name;
            uint32_t pc = f.pc;

            //caller frames store the return address
            if(i + 1 != ex.frames.size() && pc > 0) --pc;

            sf.line = f.fn->lineAt(pc);
            e.stack.push_back(std::move(sf));
        }

        if(!e.stack.empty()){
            e.path = e.stack.front().path;
            e.function = e.stack.front().function;
            e.line = e.stack.front().line;
            e.object = ex.frames.back().self->id;
        }

        if(errors.size() < 64) errors.push_back(std::move(e));

        for(Value& v : ex.stack) Heap::release(v);

        for(const Frame& f : ex.frames) --f.self->busy;

        ex.frames.clear();
    }

    static Real numOf(const Value& v){ return v.type() == Type::Int ? intToReal(Heap::i(v)) : Heap::f(v); }

    bool VMState::execute(Exec& ex, Value* result){
        Frame* fr = &ex.frames.back();
        const FunctionData* fn = fr->fn;
        const uint32_t* code = fn->code.data();
        const uint32_t* pc = code + fr->pc;
        const uint32_t* ip = pc;
        Value* S = ex.stack.data();
        Value* bp = S + fr->bp;
        Value* sp = bp + fn->nlocals;
        Instance* self = fr->self;
        const ScriptData* sd = self->sd;
        const Value* K = sd->constants.data();
        std::string err;

#define JS_LOAD_FRAME()                       \
        do{                                      \
            fr = &ex.frames.back();               \
            fn = fr->fn;                          \
            code = fn->code.data();               \
            pc = code + fr->pc;                   \
            bp = S + fr->bp;                      \
            self = fr->self;                      \
            sd = self->sd;                        \
            K = sd->constants.data();             \
        } while(0)
#define JS_FAIL(msg)    \
        do{                \
            err = (msg);    \
            goto fail;      \
        } while(0)

        for(;;){
            ip = pc;
            uint32_t ins = *pc++;

            switch(opOf(ins)){
                case Op::Nop: break;
                case Op::PushNil: ++sp; break;
                case Op::PushTrue: Heap::initBool(*sp++, true); break;
                case Op::PushFalse: Heap::initBool(*sp++, false); break;
                case Op::PushInt: Heap::initInt(*sp++, sargOf(ins)); break;
                case Op::PushConst: Heap::pushCopy(*sp++, K[argOf(ins)]); break;
                case Op::PushSelf: Heap::initObject(*sp++, self->id); break;
                case Op::PushBound:{
                    const uint32_t a = argOf(ins);
                    Heap::initBound(*sp++, (a & 1) ? Type::Signal : Type::Callable, self->id, K[a >> 1]);
                    break;
                }
                case Op::Pop: Heap::release(*--sp); break;
                case Op::Dup:
                    Heap::pushCopy(*sp, sp[-1]);
                    ++sp;
                    break;
                case Op::Dup2:
                    Heap::pushCopy(sp[0], sp[-2]);
                    Heap::pushCopy(sp[1], sp[-1]);
                    sp += 2;
                    break;
                case Op::LoadLocal: Heap::pushCopy(*sp++, bp[argOf(ins)]); break;
                case Op::StoreLocal: Heap::moveAssign(bp[argOf(ins)], *--sp); break;
                case Op::LoadMember: Heap::pushCopy(*sp++, self->members[argOf(ins)]); break;
                case Op::StoreMember: Heap::moveAssign(self->members[argOf(ins)], *--sp); break;
                case Op::LoadSelfProp:{
                    const NameEntry& e = sd->names[argOf(ins)];
                    Value out;
                    err.clear();
                    Status st = host.getProperty(self->id, e.pid, out, err);

                    if(st != Status::Ok){
                        if(st == Status::NotFound || err.empty())
                            err = "Invalid access to property or key '" + e.name + "' on a base object of type '" +
                                  typeDesc(Value::object(self->id)) + "'.";
                        goto fail;
                    }

                    Heap::pushMove(*sp++, out);
                    break;
                }
                case Op::StoreSelfProp:
                case Op::WbSelfProp:{
                    const NameEntry& e = sd->names[argOf(ins)];

                    if(opOf(ins) == Op::StoreSelfProp || isValueType(sp[-1])){
                        err.clear();
                        Status st = host.setProperty(self->id, e.pid, sp[-1], err);

                        if(st != Status::Ok){
                            if(st == Status::NotFound || err.empty())
                                err = "Invalid assignment of property or key '" + e.name + "' with value of type '" +
                                      typeDesc(sp[-1]) + "' on a base object of type '" + typeDesc(Value::object(self->id)) + "'.";
                            goto fail;
                        }
                    }

                    Heap::release(*--sp);
                    break;
                }
                case Op::LoadSingleton:{
                    uint32_t n = argOf(ins);
                    ObjectId id = self->rt->singletons[n];

                    if(!id){
                        id = host.singleton(sd->names[n].name);
                        self->rt->singletons[n] = id;
                    }

                    if(!id) JS_FAIL("Singleton '" + sd->names[n].name + "' is not available.");

                    Heap::initObject(*sp++, id);
                    break;
                }
                case Op::GetNamed:{
                    Value& base = sp[-1];
                    const NameEntry& e = sd->names[argOf(ins)];

                    if(base.type() == Type::Vector3 && e.field >= 0 && e.field <= 2){
                        Heap::initFloat(base, Heap::comp(base, e.field));
                        break;
                    }

                    Value out;

                    if(!get_named(self, argOf(ins), base, out, err)) goto fail;

                    Heap::moveAssign(base, out);
                    break;
                }
                case Op::SetNamed:{
                    Value& base = sp[-2];
                    const NameEntry& e = sd->names[argOf(ins)];

                    if(base.type() == Type::Vector3 && e.field >= 0 && e.field <= 2 && sp[-1].isNumber()){
                        Heap::setComp(base, e.field, numOf(sp[-1]));
                    } else if(!set_named(self, argOf(ins), base, sp[-1], err)){
                        goto fail;
                    }

                    Heap::release(*--sp);
                    break;
                }
                case Op::WbNamed:
                    if(isValueType(sp[-1]) && !set_named(self, argOf(ins), sp[-2], sp[-1], err)) goto fail;

                    Heap::release(*--sp);
                    break;
                case Op::GetIndex:{
                    Value out;

                    if(!get_index(sp[-2], sp[-1], out, err)) goto fail;

                    Heap::release(*--sp);
                    Heap::moveAssign(sp[-1], out);
                    break;
                }
                case Op::SetIndex:
                    if(!setIndex(sp[-3], sp[-2], sp[-1], err)) goto fail;

                    Heap::release(*--sp);
                    Heap::release(*--sp);
                    break;
                case Op::WbIndex:
                    if(isValueType(sp[-1]) && !setIndex(sp[-3], sp[-2], sp[-1], err)) goto fail;

                    Heap::release(*--sp);
                    Heap::release(*--sp);
                    break;

#define JS_ARITH(OPNAME, BINOP, INT_EXPR, REAL_EXPR)                                         \
            case Op::OPNAME: {                                                                       \
                Value& a = sp[-2];                                                                   \
                const Value& b = sp[-1];                                                             \
                Type ta = a.type(), tb = b.type();                                                   \
                if(ta == Type::Int && tb == Type::Int){                                            \
                    uint64_t x = uint64_t(Heap::i(a)), y = uint64_t(Heap::i(b));                     \
                    Heap::putInt(a, Int(INT_EXPR));                                                 \
                } else if((ta == Type::Float || ta == Type::Int) && (tb == Type::Float || tb == Type::Int)){ \
                    Real x = numOf(a), y = numOf(b);                                               \
                    Heap::initFloat(a, REAL_EXPR);                                                  \
                } else {                                                                             \
                    Value r;                                                                         \
                    if(!binary_op(BINOP, a, b, r, err)) goto fail;                                  \
                    Heap::moveAssign(a, r);                                                         \
                }                                                                                    \
                Heap::release(*--sp);                                                                \
                break;                                                                               \
            }

                    JS_ARITH(Add, BinOp::Add, x + y, x + y)
                    JS_ARITH(Sub, BinOp::Sub, x - y, x - y)
                    JS_ARITH(Mul, BinOp::Mul, x * y, x * y)
#undef JS_ARITH

#define JS_CMP(OPNAME, BINOP, CMP)                                                           \
            case Op::OPNAME: {                                                                       \
                Value& a = sp[-2];                                                                   \
                const Value& b = sp[-1];                                                             \
                Type ta = a.type(), tb = b.type();                                                   \
                bool r;                                                                              \
                if(ta == Type::Int && tb == Type::Int){                                            \
                    r = Heap::i(a) CMP Heap::i(b);                                                   \
                } else if(ta == Type::Float && tb == Type::Float){                                 \
                    r = Heap::f(a) CMP Heap::f(b);                                                   \
                } else {                                                                             \
                    Value rv;                                                                        \
                    if(!binary_op(BINOP, a, b, rv, err)) goto fail;                                 \
                    r = truthy(rv);                                                                  \
                }                                                                                    \
                Heap::release(*--sp);                                                                \
                Heap::setBool(sp[-1], r);                                                           \
                break;                                                                               \
            }

                    JS_CMP(Lt, BinOp::Lt, <)
                    JS_CMP(Le, BinOp::Le, <=)
                    JS_CMP(Gt, BinOp::Gt, >)
                    JS_CMP(Ge, BinOp::Ge, >=)
                    JS_CMP(Eq, BinOp::Eq, ==)
                    JS_CMP(Ne, BinOp::Ne, !=)
#undef JS_CMP

                case Op::Div:{
                    Value& a = sp[-2];
                    const Value& b = sp[-1];

                    if(a.type() == Type::Float && b.type() == Type::Float){
                        Heap::putFloat(a, Heap::f(a) / Heap::f(b));
                    } else {
                        Value r;

                        if(!binary_op(BinOp::Div, a, b, r, err)) goto fail;

                        Heap::moveAssign(a, r);
                    }

                    Heap::release(*--sp);
                    break;
                }
                case Op::Mod:
                case Op::Pow:
                case Op::Shl:
                case Op::Shr:
                case Op::BitAnd:
                case Op::BitOr:
                case Op::BitXor:
                case Op::In:
                case Op::NotIn:{
                    Value r;
                    BinOp bop = BinOp(int(opOf(ins)) - int(Op::Add));

                    if(!binary_op(bop, sp[-2], sp[-1], r, err)) goto fail;

                    Heap::release(*--sp);
                    Heap::moveAssign(sp[-1], r);
                    break;
                }
                case Op::Neg:{
                    Value& a = sp[-1];

                    if(a.type() == Type::Int){
                        Heap::putInt(a, Int(0 - uint64_t(Heap::i(a))));
                    } else if(a.type() == Type::Float){
                        Heap::putFloat(a, -Heap::f(a));
                    } else {
                        Value r;

                        if(!unary_op(UnOp::Neg, a, r, err)) goto fail;

                        Heap::moveAssign(a, r);
                    }
                    break;
                }
                case Op::Pos:
                case Op::BitNot:{
                    Value r;

                    if(!unary_op(opOf(ins) == Op::Pos ? UnOp::Pos : UnOp::BitNot, sp[-1], r, err)) goto fail;

                    Heap::moveAssign(sp[-1], r);
                    break;
                }
                case Op::Not: Heap::setBool(sp[-1], !truthy(sp[-1])); break;
                case Op::ToBool: Heap::setBool(sp[-1], truthy(sp[-1])); break;
                case Op::IsType: Heap::setBool(sp[-1], sp[-1].type() == Type(argOf(ins) & 0xFFu)); break;
                case Op::IsClass:{
                    bool r = sp[-1].type() == Type::Object && is_class(Heap::o(sp[-1]), sd->names[argOf(ins)].name);
                    Heap::setBool(sp[-1], r);
                    break;
                }
                case Op::AsType:{
                    Type t = Type(argOf(ins) & 0xFFu);

                    if(!convertTo(t, sp[-1]))
                        JS_FAIL("Invalid cast: could not convert value of type '" + typeDesc(sp[-1]) + "' to '" + type_name(t) + "'.");
                    break;
                }
                case Op::AsClass:
                    if(sp[-1].type() != Type::Object || !is_class(Heap::o(sp[-1]), sd->names[argOf(ins)].name))
                        Heap::release(sp[-1]);
                    break;
                case Op::Convert:{
                    uint32_t a = argOf(ins);
                    Type t = Type(a & 0xFFu);
                    uint32_t ctx = (a >> 8) & 3u;

                    if(!convertTo(t, sp[-1]) || (t == Type::Object && sp[-1].type() != Type::Object && !sp[-1].isNil())){
                        if(ctx == kConvArg)
                            JS_FAIL("Invalid type in function '" + fn->name + "'. Cannot convert argument " + formatInt(Int(a >> 10) + 1) +
                                    " from " + typeDesc(sp[-1]) + " to " + type_name(t) + ".");

                        if(ctx == kConvReturn)
                            JS_FAIL("Trying to return value of type '" + typeDesc(sp[-1]) + "' from a function whose return type is '" +
                                    type_name(t) + "'.");
                        JS_FAIL("Trying to assign value of type '" + typeDesc(sp[-1]) + "' to a variable of type '" + type_name(t) + "'.");
                    }
                    break;
                }
                case Op::CheckClass:{
                    uint32_t a = argOf(ins);
                    const std::string& cls = sd->names[a & 0xFFFFFu].name;
                    const Value& v = sp[-1];

                    if(!v.isNil() && !(v.type() == Type::Object && is_class(Heap::o(v), cls))){
                        if((a >> 20) == kConvReturn)
                            JS_FAIL("Trying to return value of type '" + typeDesc(v) + "' from a function whose return type is '" + cls + "'.");
                        JS_FAIL("Trying to assign value of type '" + typeDesc(v) + "' to a variable of type '" + cls + "'.");
                    }
                    break;
                }
                case Op::Jump:{
                    int32_t off = sargOf(ins);

                    if(off <= 0){
                        budget -= int64_t(1 - off);

                        if(budget < 0)
                            JS_FAIL("Script instruction limit exceeded (" + formatInt(Int(limit)) +
                                    " instructions in one call): possible infinite loop.");
                    }

                    pc = ip + off;
                    break;
                }
                case Op::JumpIfFalse:
                case Op::JumpIfTrue:{
                    const Value& c = sp[-1];
                    bool t = c.type() == Type::Bool ? Heap::b(c) : truthy(c);
                    Heap::release(*--sp);

                    if(t == (opOf(ins) == Op::JumpIfTrue)) pc = ip + sargOf(ins);

                    break;
                }
                case Op::ForPrep:{
                    uint32_t slot = argOf(ins);
                    Value& it = sp[-1];
                    Type t = it.type();

                    if(t != Type::Array && t != Type::Dictionary && t != Type::String && t != Type::Int)
                        JS_FAIL("Unable to iterate on object of type '" + typeDesc(it) + "'.");
                    Heap::moveAssign(bp[slot], *--sp);
                    Heap::setInt(bp[slot + 1], 0);
                    break;
                }
                case Op::ForNext:{
                    uint32_t slot = argOf(ins);
                    Value& it = bp[slot];
                    Value& idxv = bp[slot + 1];
                    Value& var = bp[pc[0]];

                    //ForPrep put the index there; bytecode that skips it is rejected here, not read as an int
                    if(idxv.type() != Type::Int) JS_FAIL("Invalid bytecode.");

                    Int idx = Heap::i(idxv);
                    bool more = false;

                    switch(it.type()){
                        case Type::Array:{
                            const auto& items = Heap::arr(it)->items;

                            if(idx < Int(items.size())){
                                var = items[size_t(idx)];
                                Heap::putInt(idxv, idx + 1);
                                more = true;
                            }
                            break;
                        }
                        case Type::Dictionary:{
                            const auto& entries = Heap::dict(it)->entries;

                            if(idx < Int(entries.size())){
                                var = entries[size_t(idx)].key;
                                Heap::putInt(idxv, idx + 1);
                                more = true;
                            }
                            break;
                        }
                        case Type::String:{
                            std::string_view s = Heap::sv(it);

                            if(idx < Int(s.size())){
                                size_t n = 1;

                                while(size_t(idx) + n < s.size() && (static_cast<unsigned char>(s[size_t(idx) + n]) & 0xC0u) == 0x80u) ++n;

                                var = Value(s.substr(size_t(idx), n));
                                Heap::putInt(idxv, idx + Int(n));
                                more = true;
                            }
                            break;
                        }
                        case Type::Int:
                            if(idx < Heap::i(it)){
                                Heap::setInt(var, idx);
                                Heap::putInt(idxv, idx + 1);
                                more = true;
                            }
                            break;
                        default: break;
                    }

                    pc = more ? ip + 3 : ip + int32_t(pc[1]);
                    break;
                }
                case Op::RangePrep:{
                    uint32_t slot = argOf(ins) & 0xFFFFu;
                    int argc = int(argOf(ins) >> 16);
                    Int v[3] = {0, 0, 1};

                    for(int k = 0; k < argc; ++k){
                        const Value& a = sp[k - argc];

                        if(!a.isNumber())
                            JS_FAIL(std::string("Invalid type in utility function 'range'. Cannot convert argument ") +
                                    formatInt(k + 1) + " from " + typeDesc(a) + " to int.");
                        v[argc == 1 ? 1 : k] = a.asInt();
                    }

                    if(v[2] == 0) JS_FAIL("Step argument is zero!");

                    for(int k = 0; k < argc; ++k) Heap::release(*--sp);

                    Heap::setInt(bp[slot], v[0]);
                    Heap::setInt(bp[slot + 1], v[1]);
                    Heap::setInt(bp[slot + 2], v[2]);
                    break;
                }
                case Op::RangeNext:{
                    uint32_t slot = argOf(ins);

                    if(bp[slot].type() != Type::Int || bp[slot + 1].type() != Type::Int || bp[slot + 2].type() != Type::Int)
                        JS_FAIL("Invalid bytecode.");

                    Int cur = Heap::i(bp[slot]), end = Heap::i(bp[slot + 1]), step = Heap::i(bp[slot + 2]);

                    if(step > 0 ? cur < end : cur > end){
                        Value& var = bp[pc[0]];

                        if(var.type() == Type::Int) Heap::putInt(var, cur);
                        else Heap::setInt(var, cur);
                        Int next = Int(uint64_t(cur) + uint64_t(step));

                        //overflow: stop
                        if((step > 0) != (next > cur)) next = end;

                        Heap::putInt(bp[slot], next);
                        pc = ip + 3;
                    } else {
                        pc = ip + int32_t(pc[1]);
                    }
                    break;
                }
                case Op::CallScript:
                case Op::CallNamed:{
                    int argc = int(argOf(ins));
                    uint32_t operand = *pc++;
                    const FunctionData* callee = nullptr;
                    Instance* target = self;
                    bool receiver = false;

                    if(opOf(ins) == Op::CallScript){
                        callee = &sd->funcs[operand];
                    } else {
                        Value& recv = sp[-argc - 1];
                        const NameEntry& e = sd->names[operand];

                        if(recv.type() == Type::Object){
                            ObjectId id = Heap::o(recv);
                            Instance* inst = find(id);

                            if(inst){
                                const NameCache& c = resolve(self, operand, inst);

                                if(c.kind == 3){
                                    callee = &inst->sd->funcs[c.index];
                                    target = inst;
                                    receiver = true;

                                    if(argc < callee->nrequired || argc > callee->nparams)
                                        JS_FAIL("Invalid call to function '" + e.name + "' in base '" + typeDesc(recv) + " (" +
                                                inst->sd->path + ")'. Expected " +
                                                formatInt(argc < callee->nrequired ? callee->nrequired : callee->nparams) + " arguments.");
                                }
                            }

                            if(!callee && e.smethod >= 0 && e.smethod <= SM_has_signal){
                                Value out;

                                if(!signalCall(e.smethod, e.name, recv, sp - argc, argc, out, err)) goto fail;

                                for(int k = 0; k <= argc; ++k) Heap::release(*--sp);

                                Heap::pushMove(*sp++, out);
                                break;
                            }

                            if(!callee){
                                Status st = Status::NotFound;
                                Value out;
                                err.clear();

                                if(e.mid >= 0) st = host.callMethod(id, e.mid, sp - argc, argc, out, err);

                                if(st != Status::Ok){
                                    if(st == Status::NotFound || err.empty())
                                        err = "Invalid call. Nonexistent function '" + e.name + "' in base '" + typeDesc(recv) + "'.";
                                    goto fail;
                                }

                                for(int k = 0; k <= argc; ++k) Heap::release(*--sp);

                                Heap::pushMove(*sp++, out);
                                break;
                            }
                        } else if(recv.type() == Type::Nil){
                            JS_FAIL("Cannot call method '" + e.name + "' on a null value.");
                        } else if(Heap::isBound(recv.type())){
                            Value out;

                            if(!signalCall(e.smethod, e.name, recv, sp - argc, argc, out, err)) goto fail;

                            for(int k = 0; k <= argc; ++k) Heap::release(*--sp);

                            Heap::pushMove(*sp++, out);
                            break;
                        } else {
                            Value out;
                            bctx.error.clear();
                            int r = callBuiltinMethod(bctx, e.bmethod, recv, sp - argc, argc, out);

                            if(r == 1)
                                JS_FAIL("Invalid call. Nonexistent function '" + e.name + "' in base '" + typeDesc(recv) + "'.");

                            if(r == 2) JS_FAIL(bctx.error);

                            for(int k = 0; k <= argc; ++k) Heap::release(*--sp);

                            Heap::pushMove(*sp++, out);
                            break;
                        }
                    }

                    //push a script frame
                    if(ex.frames.size() >= size_t(kMaxCallDepth))
                        JS_FAIL("Stack overflow (stack size: " + formatInt(kMaxCallDepth) + "). Check for infinite recursion in your script.");

                    if(--budget < 0)
                        JS_FAIL("Script instruction limit exceeded (" + formatInt(Int(limit)) + " instructions in one call): possible infinite loop.");
                    fr->pc = uint32_t(pc - code);
                    size_t newBp = size_t((sp - argc) - S);
                    size_t need = newBp + callee->frameSize + 1;

                    if(need > ex.stack.size()){
                        if(need > kMaxStackValues)
                            JS_FAIL("Stack overflow (value stack exhausted). Check for infinite recursion in your script.");
                        size_t spI = size_t(sp - S);
                        size_t grow = ex.stack.size() * 2;
                        ex.stack.resize(grow > need ? (grow > kMaxStackValues ? kMaxStackValues : grow) : need);
                        S = ex.stack.data();
                        sp = S + spI;
                    }

                    Frame nf;
                    nf.fn = callee;
                    nf.self = target;
                    nf.pc = callee->entries[size_t(argc - callee->nrequired)];
                    nf.bp = uint32_t(newBp);
                    nf.receiver = receiver;
                    ex.frames.push_back(nf);
                    ++target->busy;
                    JS_LOAD_FRAME();
                    sp = bp + fn->nlocals;
                    break;
                }
                case Op::CallSelf:{
                    int argc = int(argOf(ins));
                    const NameEntry& e = sd->names[*pc++];
                    Value out;

                    if(e.smethod >= 0 && e.smethod <= SM_has_signal){
                        if(!signalCall(e.smethod, e.name, Value::object(self->id), sp - argc, argc, out, err)) goto fail;

                        for(int k = 0; k < argc; ++k) Heap::release(*--sp);

                        Heap::pushMove(*sp++, out);
                        break;
                    }

                    err.clear();
                    Status st = e.mid >= 0 ? host.callMethod(self->id, e.mid, sp - argc, argc, out, err) : Status::NotFound;

                    if(st != Status::Ok){
                        if(st == Status::NotFound || err.empty())
                            err = "Invalid call. Nonexistent function '" + e.name + "' in base '" + typeDesc(Value::object(self->id)) + "'.";
                        goto fail;
                    }

                    for(int k = 0; k < argc; ++k) Heap::release(*--sp);

                    Heap::pushMove(*sp++, out);
                    break;
                }
                case Op::CallBuiltin:{
                    int argc = int(argOf(ins) & 0xFFu);
                    const BuiltinFunc& bf = builtinFuncs()[argOf(ins) >> 8];
                    Value out;
                    bctx.error.clear();

                    if(!bf.fn(bctx, sp - argc, argc, out)) JS_FAIL(bctx.error);

                    for(int k = 0; k < argc; ++k) Heap::release(*--sp);

                    Heap::pushMove(*sp++, out);
                    break;
                }
                case Op::MakeArray:{
                    uint32_t n = argOf(ins);
                    std::vector<Value> items(n);

                    for(uint32_t k = 0; k < n; ++k) Heap::moveAssign(items[k], sp[int(k) - int(n)]);

                    sp -= n;
                    Value arr = makeArray(std::move(items));
                    Heap::pushMove(*sp++, arr);
                    break;
                }
                case Op::MakeDict:{
                    uint32_t n = argOf(ins);
                    Value d = Value::newDictionary();
                    DictObj* dict = Heap::dict(d);
                    Value* base = sp - 2 * int(n);

                    for(uint32_t k = 0; k < n; ++k){
                        if(!validDictKey(base[2 * k]))
                            JS_FAIL(std::string("Invalid key type '") + type_name(base[2 * k].type()) + "' for a Dictionary.");
                        dict->set(base[2 * k], base[2 * k + 1]);
                    }

                    for(uint32_t k = 0; k < 2 * n; ++k) Heap::release(*--sp);

                    Heap::pushMove(*sp++, d);
                    break;
                }
                case Op::MatchEq:{
                    bool r = strictEqual(sp[-2], sp[-1]);
                    Heap::release(*--sp);
                    Heap::setBool(sp[-1], r);
                    break;
                }
                case Op::Return:
                case Op::ReturnNil:{
                    Value ret;

                    if(opOf(ins) == Op::Return) Heap::moveAssign(ret, *--sp);

                    for(Value* p = bp; p < sp; ++p) Heap::release(*p);

                    Frame done = *fr;
                    ex.frames.pop_back();
                    --done.self->busy;

                    if(ex.frames.empty()){
                        if(result) *result = std::move(ret);

                        return true;
                    }

                    JS_LOAD_FRAME();
                    Value* dst = S + done.bp;

                    if(done.receiver){
                        --dst;
                        Heap::release(*dst);
                    }

                    Heap::pushMove(*dst, ret);
                    sp = dst + 1;
                    break;
                }
                case Op::AddLocalInt:{
                    Value& l = bp[argOf(ins)];
                    int32_t imm = int32_t(*pc++);

                    if(l.type() == Type::Int){
                        Heap::putInt(l, Int(uint64_t(Heap::i(l)) + uint64_t(Int(imm))));
                    } else if(l.type() == Type::Float){
                        Heap::putFloat(l, Heap::f(l) + Real(imm));
                    } else {
                        Value r;

                        if(!binary_op(BinOp::Add, l, Value(Int(imm)), r, err)) goto fail;

                        Heap::moveAssign(l, r);
                    }
                    break;
                }
                case Op::LtLocalJump:{
                    const Value& l = bp[argOf(ins)];
                    const Value& c = K[pc[0]];
                    bool lt;

                    if(l.type() == Type::Int && c.type() == Type::Int){
                        lt = Heap::i(l) < Heap::i(c);
                    } else if(l.type() == Type::Float && c.type() == Type::Float){
                        lt = Heap::f(l) < Heap::f(c);
                    } else {
                        Value r;

                        if(!binary_op(BinOp::Lt, l, c, r, err)) goto fail;

                        lt = truthy(r);
                    }

                    pc = lt ? ip + 3 : ip + int32_t(pc[1]);
                    break;
                }
                case Op::Count:
                default: JS_FAIL("Invalid bytecode.");
            }
        }

    fail:
        fr->pc = uint32_t(ip - code);
        (void)sp;
        unwind(ex, err);
        return false;
#undef JS_LOAD_FRAME
#undef JS_FAIL
    }

    VM::VM(Host& host) : state_(new VMState(host)){ setDefaultHost(&host, true); }

    VM::~VM(){
        setDefaultHost(&state_->host, false);
        state_.reset();
    }

    bool VM::attach(ObjectId object, std::shared_ptr<const Script> script){
        VMState& s = *state_;

        if(!script){
            s.record("Cannot attach a null script.", nullptr, "");
            return false;
        }

        const ScriptData& sd = script->data();
        std::string_view cls = s.host.classOf(object);

        if(cls.empty()){
            if(s.record("Cannot attach script '" + sd.path + "': object #" + formatInt(Int(object)) + " does not exist.", nullptr, "")){
                s.errors.back().path = sd.path;
                s.errors.back().object = object;
            }

            return false;
        }

        if(!s.is_class(object, sd.baseClass)){
            const std::string message = "Script inherits from native type '" + sd.baseClass +
                "', so it can not be assigned to an object of type '" + std::string(cls) + "'.";

            if(s.record(message, nullptr, "")){
                s.errors.back().path = sd.path;
                s.errors.back().object = object;
            }

            return false;
        }

        s.removeInstance(object);
        Instance* inst = new Instance();
        inst->id = object;
        inst->script = script;
        inst->sd = &sd;
        inst->rt = s.runtimeFor(&sd);
        ++inst->rt->users;
        inst->members.reserve(sd.vars.size());

        for(const VarData& v : sd.vars){
            if(v.typed && (v.type == Type::Array || v.type == Type::Dictionary)) inst->members.push_back(typeDefault(v.type));
            else inst->members.push_back(v.initial);
        }

        s.instances[object] = inst;

        if(sd.initFn >= 0 && !s.invoke(inst, &sd.funcs[size_t(sd.initFn)], nullptr, 0, nullptr)){
            s.removeInstance(object);
            return false;
        }

        return true;
    }

    void VM::detach(ObjectId object){ state_->removeInstance(object); }

    void VM::clear(){
        state_->connections.clear();
        std::vector<ObjectId> ids;
        ids.reserve(state_->instances.size());

        for(const auto& kv : state_->instances) ids.push_back(kv.first);

        for(ObjectId id : ids) state_->removeInstance(id);
    }

    bool VM::hasInstance(ObjectId object) const{ return state_->find(object) != nullptr; }

    const Script* VM::scriptOf(ObjectId object) const{
        Instance* inst = state_->find(object);
        return inst ? inst->script.get() : nullptr;
    }

    bool VM::setMember(ObjectId object, std::string_view name, const Value& value){
        Instance* inst = state_->find(object);

        if(!inst) return false;

        const SymEntry* sym = inst->sd->findSymbol(name);

        if(!sym || sym->kind != SymKind::Var) return false;

        std::string err;
        return state_->setVar(inst, sym->index, value, err);
    }

    bool VM::getMember(ObjectId object, std::string_view name, Value& out) const{
        Instance* inst = state_->find(object);

        if(!inst) return false;

        const SymEntry* sym = inst->sd->findSymbol(name);

        if(!sym) return false;

        if(sym->kind == SymKind::Var){
            out = inst->members[sym->index];
            return true;
        }

        if(sym->kind == SymKind::Const){
            out = inst->sd->consts[sym->index].second;
            return true;
        }

        return false;
    }

    bool VM::ready(ObjectId object){
        VMState& s = *state_;
        Instance* inst = s.find(object);

        if(!inst) return true;

        const ScriptData& sd = *inst->sd;

        if(sd.onreadyFn >= 0 && sd.funcs[size_t(sd.onreadyFn)].code.size() > 1 &&
            !s.invoke(inst, &sd.funcs[size_t(sd.onreadyFn)], nullptr, 0, nullptr))
            return false;
        //the instance may have been detached by the initializers
        inst = s.find(object);

        if(!inst || inst->sd->readyFn < 0) return true;

        return s.invoke(inst, &inst->sd->funcs[size_t(inst->sd->readyFn)], nullptr, 0, nullptr);
    }

    bool VM::process(ObjectId object, Real delta){
        VMState& s = *state_;
        Instance* inst = s.find(object);

        if(!inst || inst->sd->processFn < 0) return true;

        const FunctionData& fn = inst->sd->funcs[size_t(inst->sd->processFn)];
        Value arg(delta);
        return s.invoke(inst, &fn, &arg, fn.nparams >= 1 ? 1 : 0, nullptr);
    }

    bool VM::call(ObjectId object, std::string_view function, const Value* args, int count, Value* result){
        VMState& s = *state_;

        if(result) *result = Value();

        Instance* inst = s.find(object);

        if(!inst) return true;

        const SymEntry* sym = inst->sd->findSymbol(function);

        if(!sym || sym->kind != SymKind::Func) return true;

        if(count < 0 || (count > 0 && !args)) count = 0;

        return s.invoke(inst, &inst->sd->funcs[sym->index], args, count, result);
    }

    bool VM::connect(ObjectId source, std::string_view signal, ObjectId target, std::string_view method, uint32_t flags,
                     std::string& error){
        return state_->connect(source, Value(signal), target, Value(method), Int(flags), error);
    }

    bool VM::is_connected(ObjectId source, std::string_view signal, ObjectId target, std::string_view method) const{
        return state_->findConnection(source, signal, target, method) >= 0;
    }

    bool VM::has_connections(ObjectId source) const{
        for(const Connection& c : state_->connections)
            if(c.source == source) return true;

        return false;
    }

    void VM::emit_signal(ObjectId source, std::string_view signal, const Value* args, int count){
        if(state_->connections.empty()) return;

        if(count < 0 || (count > 0 && !args)) count = 0;

        state_->emit(source, signal, args, count);
    }

    void VM::forget(ObjectId object){
        auto& list = state_->connections;
        list.erase(std::remove_if(list.begin(), list.end(),
                                  [&](const Connection& c){ return c.source == object || c.target == object; }),
                   list.end());
    }

    bool VM::callCallable(const Value& callable, const Value* args, int count, Value* result){
        VMState& s = *state_;

        if(result) *result = Value();

        if(callable.type() != Type::Callable) return false;

        if(count < 0 || (count > 0 && !args)) count = 0;

        Value out;
        std::string err;
        const std::string_view method = Heap::boundName(callable);

        if(!s.callBound(Heap::boundObject(callable), method, args, count, out, err)){
            s.record("Error calling '" + s.boundText(Heap::boundObject(callable), method) + "': " + err,
                     s.find(Heap::boundObject(callable)), Heap::str(callable)->chars());
            return false;
        }

        if(result) *result = std::move(out);

        return true;
    }

    std::vector<RuntimeError> VM::takeErrors(){
        std::vector<RuntimeError> out;
        out.swap(state_->errors);
        return out;
    }

    bool VM::hasErrors() const{ return !state_->errors.empty(); }

    void VM::setInstructionLimit(uint32_t limit){ state_->limit = limit; }

    void VM::seed(uint64_t value){ state_->rng.seed(value); }

}
