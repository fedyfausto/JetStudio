#include "js_internal.hpp"

#include <unordered_map>

namespace jetscript {

namespace detail {

struct NameCache {
    uint32_t serial = 0; // ScriptData::serial of the target script (0 = empty)
    uint8_t kind = 0;    // 0 none, 1 var, 2 const, 3 func
    uint32_t index = 0;
};

// Per-VM data of a compiled script: inline caches for dynamic member access (by name table index) and singletons.
struct ScriptRt {
    uint32_t serial = 0;
    uint32_t users = 0;
    std::vector<NameCache> cache;
    std::vector<ObjectId> singletons;
};

struct Instance {
    ObjectId id = 0;
    std::shared_ptr<const Script> script;
    const ScriptData* sd = nullptr;
    ScriptRt* rt = nullptr;
    std::vector<Value> members;
    uint32_t busy = 0; // frames currently executing on this instance
    bool dead = false; // detached while running
};

struct Frame {
    const FunctionData* fn;
    Instance* self;
    uint32_t pc; // return address (caller frames) / faulting instruction (innermost after an error)
    uint32_t bp;
    bool receiver; // result replaces the receiver slot below the arguments
};

// Frames live on the heap, so script-to-script calls never recurse on the C++ stack.
struct Exec {
    std::vector<Value> stack;
    std::vector<Frame> frames;
};

} // namespace detail

using namespace detail;

struct VMState {
    explicit VMState(Host& h) : host(h) {
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

    Instance* find(ObjectId id) const {
        auto it = instances.find(id);
        return it == instances.end() ? nullptr : it->second;
    }
    void destroy_instance(Instance* inst);
    void remove_instance(ObjectId id);
    void collect_zombies();
    ScriptRt* runtime_for(const ScriptData* sd);
    void record(std::string message, const Instance* inst, const char* function);
    bool invoke(Instance* inst, const FunctionData* fn, const Value* args, int argc, Value* result);
    bool execute(Exec& ex, Value* result);
    void unwind(Exec& ex, const std::string& message);

    std::string type_desc(const Value& v) const;
    bool is_class(ObjectId id, std::string_view cls) const;
    const NameCache& resolve(Instance* self, uint32_t ne, const Instance* target);
    bool get_named(Instance* self, uint32_t ne, const Value& base, Value& out, std::string& err);
    bool set_named(Instance* self, uint32_t ne, Value& base, const Value& v, std::string& err);
    bool get_index(const Value& base, const Value& idx, Value& out, std::string& err);
    bool set_index(Value& base, const Value& idx, const Value& v, std::string& err);
    bool set_var(Instance* inst, uint32_t index, const Value& v, std::string& err);
};

VMState::~VMState() {
    for (auto& kv : instances) destroy_instance(kv.second);
    instances.clear();
    for (Instance* z : zombies) destroy_instance(z);
    zombies.clear();
}

void VMState::destroy_instance(Instance* inst) {
    if (inst->rt && --inst->rt->users == 0) rts.erase(inst->rt->serial);
    delete inst;
}

void VMState::remove_instance(ObjectId id) {
    auto it = instances.find(id);
    if (it == instances.end()) return;
    Instance* inst = it->second;
    instances.erase(it);
    if (inst->busy > 0) {
        inst->dead = true;
        zombies.push_back(inst);
    } else {
        destroy_instance(inst);
    }
}

void VMState::collect_zombies() {
    size_t keep = 0;
    for (size_t i = 0; i < zombies.size(); ++i) {
        if (zombies[i]->busy == 0) destroy_instance(zombies[i]);
        else zombies[keep++] = zombies[i];
    }
    zombies.resize(keep);
}

ScriptRt* VMState::runtime_for(const ScriptData* sd) {
    auto it = rts.find(sd->serial);
    if (it != rts.end()) return it->second.get();
    std::unique_ptr<ScriptRt> rt(new ScriptRt());
    rt->serial = sd->serial;
    rt->cache.resize(sd->names.size());
    rt->singletons.assign(sd->names.size(), 0);
    ScriptRt* p = rt.get();
    rts.emplace(sd->serial, std::move(rt));
    return p;
}

void VMState::record(std::string message, const Instance* inst, const char* function) {
    if (errors.size() >= 64) return;
    RuntimeError e;
    e.message = std::move(message);
    if (inst) {
        e.path = inst->sd->path;
        e.object = inst->id;
    }
    e.function = function ? function : "";
    errors.push_back(std::move(e));
}

std::string VMState::type_desc(const Value& v) const {
    if (v.type() == Type::Nil) return "null instance";
    if (v.type() == Type::Object) {
        std::string_view c = host.class_of(Heap::o(v));
        if (c.empty()) return "previously freed";
        return std::string(c);
    }
    return type_name(v.type());
}

bool VMState::is_class(ObjectId id, std::string_view cls) const {
    std::string_view c = host.class_of(id);
    for (int guard = 0; guard < 64 && !c.empty(); ++guard) {
        if (c == cls) return true;
        const ClassInfo* ci = host.find_class(c);
        if (!ci || ci->base.empty()) return false;
        c = ci->base;
    }
    return false;
}

static bool is_value_type(const Value& v) {
    return v.type() == Type::Vector3 || v.type() == Type::Color || v.type() == Type::String;
}

static std::string key_text(const Value& k) {
    std::string s;
    append_value(s, k, nullptr, 0, false);
    return s;
}

const NameCache& VMState::resolve(Instance* self, uint32_t ne, const Instance* target) {
    NameCache& c = self->rt->cache[ne];
    if (c.serial != target->sd->serial) {
        const SymEntry* s = target->sd->find_symbol(self->sd->names[ne].name);
        c.serial = target->sd->serial;
        c.kind = s ? uint8_t(uint8_t(s->kind) + 1) : 0;
        c.index = s ? s->index : 0;
    }
    return c;
}

bool VMState::set_var(Instance* inst, uint32_t index, const Value& v, std::string& err) {
    const VarData& vd = inst->sd->vars[index];
    if (!vd.typed) {
        inst->members[index] = v;
        return true;
    }
    Value c = v;
    bool ok = convert_to(vd.type, c);
    if (ok && !vd.class_name.empty() && c.type() == Type::Object && !is_class(Heap::o(c), vd.class_name)) ok = false;
    if (!ok) {
        err = std::string("Trying to assign value of type '") + type_desc(v) + "' to a variable of type '" +
              (vd.class_name.empty() ? std::string(type_name(vd.type)) : vd.class_name) + "'.";
        return false;
    }
    inst->members[index] = std::move(c);
    return true;
}

bool VMState::get_named(Instance* self, uint32_t ne, const Value& base, Value& out, std::string& err) {
    const NameEntry& e = self->sd->names[ne];
    switch (base.type()) {
    case Type::Vector3:
        if (e.field >= 0 && e.field <= 2) {
            out = Value(Heap::comp(base, e.field));
            return true;
        }
        break;
    case Type::Color:
        if (e.field >= 3) {
            out = Value(e.field == 6 ? Real(1) : Heap::comp(base, e.field - 3));
            return true;
        }
        break;
    case Type::Dictionary: {
        const DictObj* d = Heap::dict(base);
        int i = d->find_str(e.name);
        if (i >= 0) {
            out = d->entries[size_t(i)].value;
            return true;
        }
        break;
    }
    case Type::Object: {
        ObjectId id = Heap::o(base);
        if (Instance* inst = find(id)) {
            const NameCache& c = resolve(self, ne, inst);
            if (c.kind == 1) {
                out = inst->members[c.index];
                return true;
            }
            if (c.kind == 2) {
                out = inst->sd->consts[c.index].second;
                return true;
            }
        }
        if (e.pid >= 0) {
            err.clear();
            Status st = host.get_property(id, e.pid, out, err);
            if (st == Status::Ok) return true;
            if (st == Status::Error) {
                if (err.empty()) err = "Error reading property '" + e.name + "'.";
                return false;
            }
        }
        break;
    }
    default: break;
    }
    err = "Invalid access to property or key '" + e.name + "' on a base object of type '" + type_desc(base) + "'.";
    return false;
}

bool VMState::set_named(Instance* self, uint32_t ne, Value& base, const Value& v, std::string& err) {
    const NameEntry& e = self->sd->names[ne];
    switch (base.type()) {
    case Type::Vector3:
        if (e.field >= 0 && e.field <= 2 && v.is_number()) {
            Heap::set_comp(base, e.field, v.as_real());
            return true;
        }
        break;
    case Type::Color:
        if (e.field >= 3 && v.is_number()) {
            if (e.field != 6) Heap::set_comp(base, e.field - 3, v.as_real()); // alpha is ignored
            return true;
        }
        break;
    case Type::Dictionary: {
        DictObj* d = Heap::dict(base);
        if (d->read_only) {
            err = "Dictionary is in read-only state.";
            return false;
        }
        d->set(Value(e.name), v);
        return true;
    }
    case Type::Object: {
        ObjectId id = Heap::o(base);
        if (Instance* inst = find(id)) {
            const NameCache& c = resolve(self, ne, inst);
            if (c.kind == 1) return set_var(inst, c.index, v, err);
            if (c.kind == 2) {
                err = "Cannot assign a new value to a constant ('" + e.name + "').";
                return false;
            }
        }
        if (e.pid >= 0) {
            err.clear();
            Status st = host.set_property(id, e.pid, v, err);
            if (st == Status::Ok) return true;
            if (st == Status::Error) {
                if (err.empty()) err = "Error assigning property '" + e.name + "'.";
                return false;
            }
        }
        break;
    }
    default: break;
    }
    err = "Invalid assignment of property or key '" + e.name + "' with value of type '" + type_desc(v) +
          "' on a base object of type '" + type_desc(base) + "'.";
    return false;
}

static bool index_int(const Value& idx, Int& out) {
    if (idx.type() != Type::Int) return false;
    out = Heap::i(idx);
    return true;
}

bool VMState::get_index(const Value& base, const Value& idx, Value& out, std::string& err) {
    Int i = 0;
    switch (base.type()) {
    case Type::Array: {
        const auto& items = Heap::arr(base)->items;
        if (!index_int(idx, i)) break;
        Int n = Int(items.size());
        Int j = i < 0 ? i + n : i;
        if (j < 0 || j >= n) {
            err = "Out of bounds get index '" + format_int(i) + "' (on base: 'Array').";
            return false;
        }
        out = items[size_t(j)];
        return true;
    }
    case Type::Dictionary: {
        const DictObj* d = Heap::dict(base);
        int k = d->find(idx);
        if (k < 0) {
            err = "Invalid access to key '" + key_text(idx) + "' on a base object of type 'Dictionary'.";
            return false;
        }
        out = d->entries[size_t(k)].value;
        return true;
    }
    case Type::String: {
        if (!index_int(idx, i)) break;
        std::string_view s = Heap::sv(base);
        Int n = Heap::str(base) ? Int(Heap::str(base)->ncp) : 0;
        Int j = i < 0 ? i + n : i;
        if (j < 0 || j >= n) {
            err = "Out of bounds get index '" + format_int(i) + "' (on base: 'String').";
            return false;
        }
        size_t a = utf8_offset(s, j), b = utf8_offset(s, j + 1);
        out = Value(s.substr(a, b - a));
        return true;
    }
    case Type::Vector3:
    case Type::Color: {
        int f = -1;
        if (idx.type() == Type::String) {
            f = find_builtin_field(Heap::sv(idx));
            if (base.type() == Type::Vector3 && f > 2) f = -1;
            if (base.type() == Type::Color) f = f >= 3 ? f - 3 : -1;
        } else if (index_int(idx, i)) {
            Int n = base.type() == Type::Vector3 ? 3 : 4;
            if (i < 0 || i >= n) {
                err = "Out of bounds get index '" + format_int(i) + "' (on base: '" + type_name(base.type()) + "').";
                return false;
            }
            f = int(i);
        }
        if (f < 0) break;
        out = Value(f == 3 ? Real(1) : Heap::comp(base, f));
        return true;
    }
    case Type::Object: {
        if (idx.type() != Type::String) break;
        std::string_view name = Heap::sv(idx);
        ObjectId id = Heap::o(base);
        if (Instance* inst = find(id)) {
            if (const SymEntry* s = inst->sd->find_symbol(name)) {
                if (s->kind == SymKind::Var) { out = inst->members[s->index]; return true; }
                if (s->kind == SymKind::Const) { out = inst->sd->consts[s->index].second; return true; }
            }
        }
        int pid = host.property_id(name);
        if (pid >= 0) {
            err.clear();
            Status st = host.get_property(id, pid, out, err);
            if (st == Status::Ok) return true;
            if (st == Status::Error) return false;
        }
        break;
    }
    default: break;
    }
    if (base.type() == Type::Array || base.type() == Type::String ||
        ((base.type() == Type::Vector3 || base.type() == Type::Color) && idx.type() != Type::String)) {
        err = std::string("Invalid index type '") + type_name(idx.type()) + "' for a base of type '" + type_name(base.type()) + "'.";
        return false;
    }
    err = "Invalid access to property or key '" + key_text(idx) + "' on a base object of type '" + type_desc(base) + "'.";
    return false;
}

bool VMState::set_index(Value& base, const Value& idx, const Value& v, std::string& err) {
    Int i = 0;
    switch (base.type()) {
    case Type::Array: {
        ArrObj* a = Heap::arr(base);
        if (!index_int(idx, i)) break;
        if (a->read_only) {
            err = "Array is in read-only state.";
            return false;
        }
        Int n = Int(a->items.size());
        Int j = i < 0 ? i + n : i;
        if (j < 0 || j >= n) {
            err = "Out of bounds set index '" + format_int(i) + "' (on base: 'Array').";
            return false;
        }
        a->items[size_t(j)] = v;
        return true;
    }
    case Type::Dictionary: {
        DictObj* d = Heap::dict(base);
        if (d->read_only) {
            err = "Dictionary is in read-only state.";
            return false;
        }
        if (!valid_dict_key(idx)) {
            err = std::string("Invalid key type '") + type_name(idx.type()) + "' for a Dictionary.";
            return false;
        }
        d->set(idx, v);
        return true;
    }
    case Type::String: {
        if (!index_int(idx, i)) break;
        if (v.type() != Type::String) {
            err = std::string("Invalid assignment of a value of type '") + type_name(v.type()) + "' to a String character.";
            return false;
        }
        std::string_view s = Heap::sv(base);
        Int n = Heap::str(base) ? Int(Heap::str(base)->ncp) : 0;
        Int j = i < 0 ? i + n : i;
        if (j < 0 || j >= n) {
            err = "Out of bounds set index '" + format_int(i) + "' (on base: 'String').";
            return false;
        }
        size_t a = utf8_offset(s, j), b = utf8_offset(s, j + 1);
        std::string r;
        r.reserve(s.size() + Heap::sv(v).size());
        r.append(s.data(), a);
        r += Heap::sv(v);
        r.append(s.data() + b, s.size() - b);
        base = Value(r);
        return true;
    }
    case Type::Vector3:
    case Type::Color: {
        int f = -1;
        if (idx.type() == Type::String) {
            f = find_builtin_field(Heap::sv(idx));
            if (base.type() == Type::Vector3 && f > 2) f = -1;
            if (base.type() == Type::Color) f = f >= 3 ? f - 3 : -1;
        } else if (index_int(idx, i)) {
            Int n = base.type() == Type::Vector3 ? 3 : 4;
            if (i < 0 || i >= n) {
                err = "Out of bounds set index '" + format_int(i) + "' (on base: '" + type_name(base.type()) + "').";
                return false;
            }
            f = int(i);
        }
        if (f < 0 || !v.is_number()) break;
        if (f != 3) Heap::set_comp(base, f, v.as_real());
        return true;
    }
    case Type::Object: {
        if (idx.type() != Type::String) break;
        std::string_view name = Heap::sv(idx);
        ObjectId id = Heap::o(base);
        if (Instance* inst = find(id)) {
            if (const SymEntry* s = inst->sd->find_symbol(name)) {
                if (s->kind == SymKind::Var) return set_var(inst, s->index, v, err);
            }
        }
        int pid = host.property_id(name);
        if (pid >= 0) {
            err.clear();
            Status st = host.set_property(id, pid, v, err);
            if (st == Status::Ok) return true;
            if (st == Status::Error) return false;
        }
        break;
    }
    default: break;
    }
    err = "Invalid assignment of property or key '" + key_text(idx) + "' with value of type '" + type_desc(v) +
          "' on a base object of type '" + type_desc(base) + "'.";
    return false;
}

bool VMState::invoke(Instance* inst, const FunctionData* fn, const Value* args, int argc, Value* result) {
    if (depth >= kMaxReentry) {
        record("Too many nested script calls from the engine (maximum " + format_int(kMaxReentry) + ").", inst, fn->name.c_str());
        return false;
    }
    if (argc < fn->nrequired || argc > fn->nparams) {
        record("Invalid call to function '" + fn->name + "' in base '" + inst->sd->base_class + " (" + inst->sd->path +
                   ")'. Expected " + format_int(argc < fn->nrequired ? fn->nrequired : fn->nparams) + " arguments.",
               inst, fn->name.c_str());
        return false;
    }
    if (depth == 0) budget = limit ? int64_t(limit) : INT64_MAX;
    if (int(execs.size()) <= depth) execs.emplace_back(new Exec());
    Exec& ex = *execs[size_t(depth)];
    ++depth;
    size_t need = size_t(fn->frame_size) + 1;
    if (ex.stack.size() < need) ex.stack.resize(need < 256 ? 256 : need);
    for (int i = 0; i < argc; ++i) ex.stack[size_t(i)] = args[i];
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
    if (depth == 0) collect_zombies();
    return ok;
}

void VMState::unwind(Exec& ex, const std::string& message) {
    RuntimeError e;
    e.message = message;
    for (size_t i = ex.frames.size(); i-- > 0;) {
        const Frame& f = ex.frames[i];
        StackFrame sf;
        sf.path = f.self->sd->path;
        sf.function = f.fn->name;
        uint32_t pc = f.pc;
        if (i + 1 != ex.frames.size() && pc > 0) --pc; // caller frames store the return address
        sf.line = f.fn->line_at(pc);
        e.stack.push_back(std::move(sf));
    }
    if (!e.stack.empty()) {
        e.path = e.stack.front().path;
        e.function = e.stack.front().function;
        e.line = e.stack.front().line;
        e.object = ex.frames.back().self->id;
    }
    if (errors.size() < 64) errors.push_back(std::move(e));
    for (Value& v : ex.stack) Heap::release(v);
    for (const Frame& f : ex.frames) --f.self->busy;
    ex.frames.clear();
}

static Real num_of(const Value& v) { return v.type() == Type::Int ? Real(Heap::i(v)) : Heap::f(v); }

bool VMState::execute(Exec& ex, Value* result) {
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
    do {                                      \
        fr = &ex.frames.back();               \
        fn = fr->fn;                          \
        code = fn->code.data();               \
        pc = code + fr->pc;                   \
        bp = S + fr->bp;                      \
        self = fr->self;                      \
        sd = self->sd;                        \
        K = sd->constants.data();             \
    } while (0)
#define JS_FAIL(msg)    \
    do {                \
        err = (msg);    \
        goto fail;      \
    } while (0)

    for (;;) {
        ip = pc;
        uint32_t ins = *pc++;
        switch (op_of(ins)) {
        case Op::Nop: break;
        case Op::PushNil: ++sp; break;
        case Op::PushTrue: Heap::init_bool(*sp++, true); break;
        case Op::PushFalse: Heap::init_bool(*sp++, false); break;
        case Op::PushInt: Heap::init_int(*sp++, sarg_of(ins)); break;
        case Op::PushConst: Heap::push_copy(*sp++, K[arg_of(ins)]); break;
        case Op::PushSelf: Heap::init_object(*sp++, self->id); break;
        case Op::Pop: Heap::release(*--sp); break;
        case Op::Dup:
            Heap::push_copy(*sp, sp[-1]);
            ++sp;
            break;
        case Op::Dup2:
            Heap::push_copy(sp[0], sp[-2]);
            Heap::push_copy(sp[1], sp[-1]);
            sp += 2;
            break;
        case Op::LoadLocal: Heap::push_copy(*sp++, bp[arg_of(ins)]); break;
        case Op::StoreLocal: Heap::move_assign(bp[arg_of(ins)], *--sp); break;
        case Op::LoadMember: Heap::push_copy(*sp++, self->members[arg_of(ins)]); break;
        case Op::StoreMember: Heap::move_assign(self->members[arg_of(ins)], *--sp); break;
        case Op::LoadSelfProp: {
            const NameEntry& e = sd->names[arg_of(ins)];
            Value out;
            err.clear();
            Status st = host.get_property(self->id, e.pid, out, err);
            if (st != Status::Ok) {
                if (st == Status::NotFound || err.empty())
                    err = "Invalid access to property or key '" + e.name + "' on a base object of type '" +
                          type_desc(Value::object(self->id)) + "'.";
                goto fail;
            }
            Heap::push_move(*sp++, out);
            break;
        }
        case Op::StoreSelfProp:
        case Op::WbSelfProp: {
            const NameEntry& e = sd->names[arg_of(ins)];
            if (op_of(ins) == Op::StoreSelfProp || is_value_type(sp[-1])) {
                err.clear();
                Status st = host.set_property(self->id, e.pid, sp[-1], err);
                if (st != Status::Ok) {
                    if (st == Status::NotFound || err.empty())
                        err = "Invalid assignment of property or key '" + e.name + "' with value of type '" +
                              type_desc(sp[-1]) + "' on a base object of type '" + type_desc(Value::object(self->id)) + "'.";
                    goto fail;
                }
            }
            Heap::release(*--sp);
            break;
        }
        case Op::LoadSingleton: {
            uint32_t n = arg_of(ins);
            ObjectId id = self->rt->singletons[n];
            if (!id) {
                id = host.singleton(sd->names[n].name);
                self->rt->singletons[n] = id;
            }
            if (!id) JS_FAIL("Singleton '" + sd->names[n].name + "' is not available.");
            Heap::init_object(*sp++, id);
            break;
        }
        case Op::GetNamed: {
            Value& base = sp[-1];
            const NameEntry& e = sd->names[arg_of(ins)];
            if (base.type() == Type::Vector3 && e.field >= 0 && e.field <= 2) {
                Heap::init_float(base, Heap::comp(base, e.field));
                break;
            }
            Value out;
            if (!get_named(self, arg_of(ins), base, out, err)) goto fail;
            Heap::move_assign(base, out);
            break;
        }
        case Op::SetNamed: {
            Value& base = sp[-2];
            const NameEntry& e = sd->names[arg_of(ins)];
            if (base.type() == Type::Vector3 && e.field >= 0 && e.field <= 2 && sp[-1].is_number()) {
                Heap::set_comp(base, e.field, num_of(sp[-1]));
            } else if (!set_named(self, arg_of(ins), base, sp[-1], err)) {
                goto fail;
            }
            Heap::release(*--sp);
            break;
        }
        case Op::WbNamed:
            if (is_value_type(sp[-1]) && !set_named(self, arg_of(ins), sp[-2], sp[-1], err)) goto fail;
            Heap::release(*--sp);
            break;
        case Op::GetIndex: {
            Value out;
            if (!get_index(sp[-2], sp[-1], out, err)) goto fail;
            Heap::release(*--sp);
            Heap::move_assign(sp[-1], out);
            break;
        }
        case Op::SetIndex:
            if (!set_index(sp[-3], sp[-2], sp[-1], err)) goto fail;
            Heap::release(*--sp);
            Heap::release(*--sp);
            break;
        case Op::WbIndex:
            if (is_value_type(sp[-1]) && !set_index(sp[-3], sp[-2], sp[-1], err)) goto fail;
            Heap::release(*--sp);
            Heap::release(*--sp);
            break;

#define JS_ARITH(OPNAME, BINOP, INT_EXPR, REAL_EXPR)                                         \
    case Op::OPNAME: {                                                                       \
        Value& a = sp[-2];                                                                   \
        const Value& b = sp[-1];                                                             \
        Type ta = a.type(), tb = b.type();                                                   \
        if (ta == Type::Int && tb == Type::Int) {                                            \
            uint64_t x = uint64_t(Heap::i(a)), y = uint64_t(Heap::i(b));                     \
            Heap::put_int(a, Int(INT_EXPR));                                                 \
        } else if ((ta == Type::Float || ta == Type::Int) && (tb == Type::Float || tb == Type::Int)) { \
            Real x = num_of(a), y = num_of(b);                                               \
            Heap::init_float(a, REAL_EXPR);                                                  \
        } else {                                                                             \
            Value r;                                                                         \
            if (!binary_op(BINOP, a, b, r, err)) goto fail;                                  \
            Heap::move_assign(a, r);                                                         \
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
        if (ta == Type::Int && tb == Type::Int) {                                            \
            r = Heap::i(a) CMP Heap::i(b);                                                   \
        } else if (ta == Type::Float && tb == Type::Float) {                                 \
            r = Heap::f(a) CMP Heap::f(b);                                                   \
        } else {                                                                             \
            Value rv;                                                                        \
            if (!binary_op(BINOP, a, b, rv, err)) goto fail;                                 \
            r = truthy(rv);                                                                  \
        }                                                                                    \
        Heap::release(*--sp);                                                                \
        Heap::set_bool(sp[-1], r);                                                           \
        break;                                                                               \
    }
            JS_CMP(Lt, BinOp::Lt, <)
            JS_CMP(Le, BinOp::Le, <=)
            JS_CMP(Gt, BinOp::Gt, >)
            JS_CMP(Ge, BinOp::Ge, >=)
            JS_CMP(Eq, BinOp::Eq, ==)
            JS_CMP(Ne, BinOp::Ne, !=)
#undef JS_CMP

        case Op::Div: {
            Value& a = sp[-2];
            const Value& b = sp[-1];
            if (a.type() == Type::Float && b.type() == Type::Float) {
                Heap::put_float(a, Heap::f(a) / Heap::f(b));
            } else {
                Value r;
                if (!binary_op(BinOp::Div, a, b, r, err)) goto fail;
                Heap::move_assign(a, r);
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
        case Op::NotIn: {
            Value r;
            BinOp bop = BinOp(int(op_of(ins)) - int(Op::Add));
            if (!binary_op(bop, sp[-2], sp[-1], r, err)) goto fail;
            Heap::release(*--sp);
            Heap::move_assign(sp[-1], r);
            break;
        }
        case Op::Neg: {
            Value& a = sp[-1];
            if (a.type() == Type::Int) {
                Heap::put_int(a, Int(0 - uint64_t(Heap::i(a))));
            } else if (a.type() == Type::Float) {
                Heap::put_float(a, -Heap::f(a));
            } else {
                Value r;
                if (!unary_op(UnOp::Neg, a, r, err)) goto fail;
                Heap::move_assign(a, r);
            }
            break;
        }
        case Op::Pos:
        case Op::BitNot: {
            Value r;
            if (!unary_op(op_of(ins) == Op::Pos ? UnOp::Pos : UnOp::BitNot, sp[-1], r, err)) goto fail;
            Heap::move_assign(sp[-1], r);
            break;
        }
        case Op::Not: Heap::set_bool(sp[-1], !truthy(sp[-1])); break;
        case Op::ToBool: Heap::set_bool(sp[-1], truthy(sp[-1])); break;
        case Op::IsType: Heap::set_bool(sp[-1], sp[-1].type() == Type(arg_of(ins) & 0xFFu)); break;
        case Op::IsClass: {
            bool r = sp[-1].type() == Type::Object && is_class(Heap::o(sp[-1]), sd->names[arg_of(ins)].name);
            Heap::set_bool(sp[-1], r);
            break;
        }
        case Op::AsType: {
            Type t = Type(arg_of(ins) & 0xFFu);
            if (!convert_to(t, sp[-1]))
                JS_FAIL("Invalid cast: could not convert value of type '" + type_desc(sp[-1]) + "' to '" + type_name(t) + "'.");
            break;
        }
        case Op::AsClass:
            if (sp[-1].type() != Type::Object || !is_class(Heap::o(sp[-1]), sd->names[arg_of(ins)].name))
                Heap::release(sp[-1]);
            break;
        case Op::Convert: {
            uint32_t a = arg_of(ins);
            Type t = Type(a & 0xFFu);
            uint32_t ctx = (a >> 8) & 3u;
            if (!convert_to(t, sp[-1]) || (t == Type::Object && sp[-1].type() != Type::Object && !sp[-1].is_nil())) {
                if (ctx == kConvArg)
                    JS_FAIL("Invalid type in function '" + fn->name + "'. Cannot convert argument " + format_int(Int(a >> 10) + 1) +
                            " from " + type_desc(sp[-1]) + " to " + type_name(t) + ".");
                if (ctx == kConvReturn)
                    JS_FAIL("Trying to return value of type '" + type_desc(sp[-1]) + "' from a function whose return type is '" +
                            type_name(t) + "'.");
                JS_FAIL("Trying to assign value of type '" + type_desc(sp[-1]) + "' to a variable of type '" + type_name(t) + "'.");
            }
            break;
        }
        case Op::CheckClass: {
            uint32_t a = arg_of(ins);
            const std::string& cls = sd->names[a & 0xFFFFFu].name;
            const Value& v = sp[-1];
            if (!v.is_nil() && !(v.type() == Type::Object && is_class(Heap::o(v), cls))) {
                if ((a >> 20) == kConvReturn)
                    JS_FAIL("Trying to return value of type '" + type_desc(v) + "' from a function whose return type is '" + cls + "'.");
                JS_FAIL("Trying to assign value of type '" + type_desc(v) + "' to a variable of type '" + cls + "'.");
            }
            break;
        }
        case Op::Jump: {
            int32_t off = sarg_of(ins);
            if (off <= 0) {
                budget -= int64_t(1 - off);
                if (budget < 0)
                    JS_FAIL("Script instruction limit exceeded (" + format_int(Int(limit)) +
                            " instructions in one call): possible infinite loop.");
            }
            pc = ip + off;
            break;
        }
        case Op::JumpIfFalse:
        case Op::JumpIfTrue: {
            const Value& c = sp[-1];
            bool t = c.type() == Type::Bool ? Heap::b(c) : truthy(c);
            Heap::release(*--sp);
            if (t == (op_of(ins) == Op::JumpIfTrue)) pc = ip + sarg_of(ins);
            break;
        }
        case Op::ForPrep: {
            uint32_t slot = arg_of(ins);
            Value& it = sp[-1];
            Type t = it.type();
            if (t != Type::Array && t != Type::Dictionary && t != Type::String && t != Type::Int)
                JS_FAIL("Unable to iterate on object of type '" + type_desc(it) + "'.");
            Heap::move_assign(bp[slot], *--sp);
            Heap::set_int(bp[slot + 1], 0);
            break;
        }
        case Op::ForNext: {
            uint32_t slot = arg_of(ins);
            Value& it = bp[slot];
            Value& idxv = bp[slot + 1];
            Value& var = bp[pc[0]];
            Int idx = Heap::i(idxv);
            bool more = false;
            switch (it.type()) {
            case Type::Array: {
                const auto& items = Heap::arr(it)->items;
                if (idx < Int(items.size())) {
                    var = items[size_t(idx)];
                    Heap::put_int(idxv, idx + 1);
                    more = true;
                }
                break;
            }
            case Type::Dictionary: {
                const auto& entries = Heap::dict(it)->entries;
                if (idx < Int(entries.size())) {
                    var = entries[size_t(idx)].key;
                    Heap::put_int(idxv, idx + 1);
                    more = true;
                }
                break;
            }
            case Type::String: {
                std::string_view s = Heap::sv(it);
                if (idx < Int(s.size())) {
                    size_t n = 1;
                    while (size_t(idx) + n < s.size() && (static_cast<unsigned char>(s[size_t(idx) + n]) & 0xC0u) == 0x80u) ++n;
                    var = Value(s.substr(size_t(idx), n));
                    Heap::put_int(idxv, idx + Int(n));
                    more = true;
                }
                break;
            }
            case Type::Int:
                if (idx < Heap::i(it)) {
                    Heap::set_int(var, idx);
                    Heap::put_int(idxv, idx + 1);
                    more = true;
                }
                break;
            default: break;
            }
            pc = more ? ip + 3 : ip + int32_t(pc[1]);
            break;
        }
        case Op::RangePrep: {
            uint32_t slot = arg_of(ins) & 0xFFFFu;
            int argc = int(arg_of(ins) >> 16);
            Int v[3] = {0, 0, 1};
            for (int k = 0; k < argc; ++k) {
                const Value& a = sp[k - argc];
                if (!a.is_number())
                    JS_FAIL(std::string("Invalid type in utility function 'range'. Cannot convert argument ") +
                            format_int(k + 1) + " from " + type_desc(a) + " to int.");
                v[argc == 1 ? 1 : k] = a.as_int();
            }
            if (v[2] == 0) JS_FAIL("Step argument is zero!");
            for (int k = 0; k < argc; ++k) Heap::release(*--sp);
            Heap::set_int(bp[slot], v[0]);
            Heap::set_int(bp[slot + 1], v[1]);
            Heap::set_int(bp[slot + 2], v[2]);
            break;
        }
        case Op::RangeNext: {
            uint32_t slot = arg_of(ins);
            Int cur = Heap::i(bp[slot]), end = Heap::i(bp[slot + 1]), step = Heap::i(bp[slot + 2]);
            if (step > 0 ? cur < end : cur > end) {
                Value& var = bp[pc[0]];
                if (var.type() == Type::Int) Heap::put_int(var, cur);
                else Heap::set_int(var, cur);
                Int next = Int(uint64_t(cur) + uint64_t(step));
                if ((step > 0) != (next > cur)) next = end; // overflow: stop
                Heap::put_int(bp[slot], next);
                pc = ip + 3;
            } else {
                pc = ip + int32_t(pc[1]);
            }
            break;
        }
        case Op::CallScript:
        case Op::CallNamed: {
            int argc = int(arg_of(ins));
            uint32_t operand = *pc++;
            const FunctionData* callee = nullptr;
            Instance* target = self;
            bool receiver = false;
            if (op_of(ins) == Op::CallScript) {
                callee = &sd->funcs[operand];
            } else {
                Value& recv = sp[-argc - 1];
                const NameEntry& e = sd->names[operand];
                if (recv.type() == Type::Object) {
                    ObjectId id = Heap::o(recv);
                    Instance* inst = find(id);
                    if (inst) {
                        const NameCache& c = resolve(self, operand, inst);
                        if (c.kind == 3) {
                            callee = &inst->sd->funcs[c.index];
                            target = inst;
                            receiver = true;
                            if (argc < callee->nrequired || argc > callee->nparams)
                                JS_FAIL("Invalid call to function '" + e.name + "' in base '" + type_desc(recv) + " (" +
                                        inst->sd->path + ")'. Expected " +
                                        format_int(argc < callee->nrequired ? callee->nrequired : callee->nparams) + " arguments.");
                        }
                    }
                    if (!callee) {
                        Status st = Status::NotFound;
                        Value out;
                        err.clear();
                        if (e.mid >= 0) st = host.call_method(id, e.mid, sp - argc, argc, out, err);
                        if (st != Status::Ok) {
                            if (st == Status::NotFound || err.empty())
                                err = "Invalid call. Nonexistent function '" + e.name + "' in base '" + type_desc(recv) + "'.";
                            goto fail;
                        }
                        for (int k = 0; k <= argc; ++k) Heap::release(*--sp);
                        Heap::push_move(*sp++, out);
                        break;
                    }
                } else if (recv.type() == Type::Nil) {
                    JS_FAIL("Cannot call method '" + e.name + "' on a null value.");
                } else {
                    Value out;
                    bctx.error.clear();
                    int r = call_builtin_method(bctx, e.bmethod, recv, sp - argc, argc, out);
                    if (r == 1)
                        JS_FAIL("Invalid call. Nonexistent function '" + e.name + "' in base '" + type_desc(recv) + "'.");
                    if (r == 2) JS_FAIL(bctx.error);
                    for (int k = 0; k <= argc; ++k) Heap::release(*--sp);
                    Heap::push_move(*sp++, out);
                    break;
                }
            }
            // push a script frame
            if (ex.frames.size() >= size_t(kMaxCallDepth))
                JS_FAIL("Stack overflow (stack size: " + format_int(kMaxCallDepth) + "). Check for infinite recursion in your script.");
            if (--budget < 0)
                JS_FAIL("Script instruction limit exceeded (" + format_int(Int(limit)) + " instructions in one call): possible infinite loop.");
            fr->pc = uint32_t(pc - code);
            size_t new_bp = size_t((sp - argc) - S);
            size_t need = new_bp + callee->frame_size + 1;
            if (need > ex.stack.size()) {
                if (need > kMaxStackValues)
                    JS_FAIL("Stack overflow (value stack exhausted). Check for infinite recursion in your script.");
                size_t sp_i = size_t(sp - S);
                size_t grow = ex.stack.size() * 2;
                ex.stack.resize(grow > need ? (grow > kMaxStackValues ? kMaxStackValues : grow) : need);
                S = ex.stack.data();
                sp = S + sp_i;
            }
            Frame nf;
            nf.fn = callee;
            nf.self = target;
            nf.pc = callee->entries[size_t(argc - callee->nrequired)];
            nf.bp = uint32_t(new_bp);
            nf.receiver = receiver;
            ex.frames.push_back(nf);
            ++target->busy;
            JS_LOAD_FRAME();
            sp = bp + fn->nlocals;
            break;
        }
        case Op::CallSelf: {
            int argc = int(arg_of(ins));
            const NameEntry& e = sd->names[*pc++];
            Value out;
            err.clear();
            Status st = e.mid >= 0 ? host.call_method(self->id, e.mid, sp - argc, argc, out, err) : Status::NotFound;
            if (st != Status::Ok) {
                if (st == Status::NotFound || err.empty())
                    err = "Invalid call. Nonexistent function '" + e.name + "' in base '" + type_desc(Value::object(self->id)) + "'.";
                goto fail;
            }
            for (int k = 0; k < argc; ++k) Heap::release(*--sp);
            Heap::push_move(*sp++, out);
            break;
        }
        case Op::CallBuiltin: {
            int argc = int(arg_of(ins) & 0xFFu);
            const BuiltinFunc& bf = builtin_funcs()[arg_of(ins) >> 8];
            Value out;
            bctx.error.clear();
            if (!bf.fn(bctx, sp - argc, argc, out)) JS_FAIL(bctx.error);
            for (int k = 0; k < argc; ++k) Heap::release(*--sp);
            Heap::push_move(*sp++, out);
            break;
        }
        case Op::MakeArray: {
            uint32_t n = arg_of(ins);
            std::vector<Value> items(n);
            for (uint32_t k = 0; k < n; ++k) Heap::move_assign(items[k], sp[int(k) - int(n)]);
            sp -= n;
            Value arr = make_array(std::move(items));
            Heap::push_move(*sp++, arr);
            break;
        }
        case Op::MakeDict: {
            uint32_t n = arg_of(ins);
            Value d = Value::new_dictionary();
            DictObj* dict = Heap::dict(d);
            Value* base = sp - 2 * int(n);
            for (uint32_t k = 0; k < n; ++k) {
                if (!valid_dict_key(base[2 * k]))
                    JS_FAIL(std::string("Invalid key type '") + type_name(base[2 * k].type()) + "' for a Dictionary.");
                dict->set(base[2 * k], base[2 * k + 1]);
            }
            for (uint32_t k = 0; k < 2 * n; ++k) Heap::release(*--sp);
            Heap::push_move(*sp++, d);
            break;
        }
        case Op::MatchEq: {
            bool r = strict_equal(sp[-2], sp[-1]);
            Heap::release(*--sp);
            Heap::set_bool(sp[-1], r);
            break;
        }
        case Op::Return:
        case Op::ReturnNil: {
            Value ret;
            if (op_of(ins) == Op::Return) Heap::move_assign(ret, *--sp);
            for (Value* p = bp; p < sp; ++p) Heap::release(*p);
            Frame done = *fr;
            ex.frames.pop_back();
            --done.self->busy;
            if (ex.frames.empty()) {
                if (result) *result = std::move(ret);
                return true;
            }
            JS_LOAD_FRAME();
            Value* dst = S + done.bp;
            if (done.receiver) {
                --dst;
                Heap::release(*dst);
            }
            Heap::push_move(*dst, ret);
            sp = dst + 1;
            break;
        }
        case Op::AddLocalInt: {
            Value& l = bp[arg_of(ins)];
            int32_t imm = int32_t(*pc++);
            if (l.type() == Type::Int) {
                Heap::put_int(l, Int(uint64_t(Heap::i(l)) + uint64_t(Int(imm))));
            } else if (l.type() == Type::Float) {
                Heap::put_float(l, Heap::f(l) + Real(imm));
            } else {
                Value r;
                if (!binary_op(BinOp::Add, l, Value(Int(imm)), r, err)) goto fail;
                Heap::move_assign(l, r);
            }
            break;
        }
        case Op::LtLocalJump: {
            const Value& l = bp[arg_of(ins)];
            const Value& c = K[pc[0]];
            bool lt;
            if (l.type() == Type::Int && c.type() == Type::Int) {
                lt = Heap::i(l) < Heap::i(c);
            } else if (l.type() == Type::Float && c.type() == Type::Float) {
                lt = Heap::f(l) < Heap::f(c);
            } else {
                Value r;
                if (!binary_op(BinOp::Lt, l, c, r, err)) goto fail;
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

VM::VM(Host& host) : state_(new VMState(host)) { set_default_host(&host, true); }

VM::~VM() {
    set_default_host(&state_->host, false);
    state_.reset();
}

bool VM::attach(ObjectId object, std::shared_ptr<const Script> script) {
    VMState& s = *state_;
    if (!script) {
        s.record("Cannot attach a null script.", nullptr, "");
        return false;
    }
    const ScriptData& sd = script->data();
    std::string_view cls = s.host.class_of(object);
    if (cls.empty()) {
        s.record("Cannot attach script '" + sd.path + "': object #" + format_int(Int(object)) + " does not exist.", nullptr, "");
        s.errors.back().path = sd.path;
        s.errors.back().object = object;
        return false;
    }
    if (!s.is_class(object, sd.base_class)) {
        s.record("Script inherits from native type '" + sd.base_class + "', so it can't be assigned to an object of type '" +
                     std::string(cls) + "'.",
                 nullptr, "");
        s.errors.back().path = sd.path;
        s.errors.back().object = object;
        return false;
    }
    s.remove_instance(object);
    Instance* inst = new Instance();
    inst->id = object;
    inst->script = script;
    inst->sd = &sd;
    inst->rt = s.runtime_for(&sd);
    ++inst->rt->users;
    inst->members.reserve(sd.vars.size());
    for (const VarData& v : sd.vars) {
        if (v.typed && (v.type == Type::Array || v.type == Type::Dictionary)) inst->members.push_back(type_default(v.type));
        else inst->members.push_back(v.initial);
    }
    s.instances[object] = inst;
    if (sd.init_fn >= 0 && !s.invoke(inst, &sd.funcs[size_t(sd.init_fn)], nullptr, 0, nullptr)) {
        s.remove_instance(object);
        return false;
    }
    return true;
}

void VM::detach(ObjectId object) { state_->remove_instance(object); }

void VM::clear() {
    std::vector<ObjectId> ids;
    ids.reserve(state_->instances.size());
    for (const auto& kv : state_->instances) ids.push_back(kv.first);
    for (ObjectId id : ids) state_->remove_instance(id);
}

bool VM::has_instance(ObjectId object) const { return state_->find(object) != nullptr; }

const Script* VM::script_of(ObjectId object) const {
    Instance* inst = state_->find(object);
    return inst ? inst->script.get() : nullptr;
}

bool VM::set_member(ObjectId object, std::string_view name, const Value& value) {
    Instance* inst = state_->find(object);
    if (!inst) return false;
    const SymEntry* sym = inst->sd->find_symbol(name);
    if (!sym || sym->kind != SymKind::Var) return false;
    std::string err;
    return state_->set_var(inst, sym->index, value, err);
}

bool VM::get_member(ObjectId object, std::string_view name, Value& out) const {
    Instance* inst = state_->find(object);
    if (!inst) return false;
    const SymEntry* sym = inst->sd->find_symbol(name);
    if (!sym) return false;
    if (sym->kind == SymKind::Var) {
        out = inst->members[sym->index];
        return true;
    }
    if (sym->kind == SymKind::Const) {
        out = inst->sd->consts[sym->index].second;
        return true;
    }
    return false;
}

bool VM::ready(ObjectId object) {
    VMState& s = *state_;
    Instance* inst = s.find(object);
    if (!inst) return true;
    const ScriptData& sd = *inst->sd;
    if (sd.onready_fn >= 0 && sd.funcs[size_t(sd.onready_fn)].code.size() > 1 &&
        !s.invoke(inst, &sd.funcs[size_t(sd.onready_fn)], nullptr, 0, nullptr))
        return false;
    inst = s.find(object); // the instance may have been detached by the initializers
    if (!inst || inst->sd->ready_fn < 0) return true;
    return s.invoke(inst, &inst->sd->funcs[size_t(inst->sd->ready_fn)], nullptr, 0, nullptr);
}

bool VM::process(ObjectId object, Real delta) {
    VMState& s = *state_;
    Instance* inst = s.find(object);
    if (!inst || inst->sd->process_fn < 0) return true;
    const FunctionData& fn = inst->sd->funcs[size_t(inst->sd->process_fn)];
    Value arg(delta);
    return s.invoke(inst, &fn, &arg, fn.nparams >= 1 ? 1 : 0, nullptr);
}

bool VM::call(ObjectId object, std::string_view function, const Value* args, int count, Value* result) {
    VMState& s = *state_;
    if (result) *result = Value();
    Instance* inst = s.find(object);
    if (!inst) return true;
    const SymEntry* sym = inst->sd->find_symbol(function);
    if (!sym || sym->kind != SymKind::Func) return true;
    if (count < 0 || (count > 0 && !args)) count = 0;
    return s.invoke(inst, &inst->sd->funcs[sym->index], args, count, result);
}

std::vector<RuntimeError> VM::take_errors() {
    std::vector<RuntimeError> out;
    out.swap(state_->errors);
    return out;
}

bool VM::has_errors() const { return !state_->errors.empty(); }

void VM::set_instruction_limit(uint32_t limit) { state_->limit = limit; }

void VM::seed(uint64_t value) { state_->rng.seed(value); }

} // namespace jetscript
