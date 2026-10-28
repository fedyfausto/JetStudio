#include "js_internal.hpp"

namespace jetscript {
namespace detail {
namespace {

// Header: "JSBC", u32 format version, u32 fingerprint, u32 payload size, u32 CRC-32 of the payload. Little endian.
constexpr uint32_t kMagic = 0x4342534a; // "JSBC"
constexpr uint32_t kFormat = 2;
constexpr uint32_t kTypeCount = uint32_t(Type::Signal) + 1; // the last Type
constexpr int kMaxValueDepth = 16;
constexpr size_t kMaxText = 1u << 20;

uint32_t crc32(const uint8_t* data, size_t size) {
    uint32_t crc = 0xFFFFFFFFu;
    for (size_t i = 0; i < size; ++i) {
        crc ^= data[i];
        for (int k = 0; k < 8; ++k) crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
    }
    return ~crc;
}

void fnv(uint32_t& h, const void* data, size_t size) {
    const uint8_t* p = static_cast<const uint8_t*>(data);
    for (size_t i = 0; i < size; ++i) h = (h ^ p[i]) * 16777619u;
}

// Changes whenever opcodes or builtin tables change, so bytecode from another JetScript build is refused, not misread.
uint32_t fingerprint() {
    uint32_t h = 2166136261u;
    const char tag[] = "JetScript bytecode";
    fnv(h, tag, sizeof(tag));
    const uint32_t ops = uint32_t(Op::Count), types = kTypeCount;
    fnv(h, &ops, sizeof(ops));
    fnv(h, &types, sizeof(types));
    const BuiltinFunc* funcs = builtin_funcs();
    for (int i = 0; i < builtin_func_count(); ++i) {
        fnv(h, funcs[i].name, std::strlen(funcs[i].name) + 1);
        const int8_t arity[2] = {funcs[i].min_args, funcs[i].max_args};
        fnv(h, arity, sizeof(arity));
    }
    return h;
}

struct Writer {
    std::vector<uint8_t> out;
    void u8(uint8_t v) { out.push_back(v); }
    void u16(uint16_t v) { u8(uint8_t(v)); u8(uint8_t(v >> 8)); }
    void u32(uint32_t v) { for (int i = 0; i < 4; ++i) out.push_back(uint8_t(v >> (8 * i))); }
    void i32(int32_t v) { u32(uint32_t(v)); }
    void u64(uint64_t v) { u32(uint32_t(v)); u32(uint32_t(v >> 32)); }
    void f32(Real f) { uint32_t v; std::memcpy(&v, &f, 4); u32(v); }
    void str(std::string_view s) { u32(uint32_t(s.size())); out.insert(out.end(), s.begin(), s.end()); }
    void value(const Value& v) {
        const Type t = v.type();
        u8(uint8_t(t));
        switch (t) {
        case Type::Bool: u8(Heap::b(v) ? 1 : 0); break;
        case Type::Int: u64(uint64_t(Heap::i(v))); break;
        case Type::Float: f32(Heap::f(v)); break;
        case Type::String: str(Heap::sv(v)); break;
        case Type::Vector3: case Type::Color: for (int k = 0; k < 3; ++k) f32(Heap::comp(v, k)); break;
        case Type::Vector2: f32(Heap::comp(v, 0)); f32(Heap::comp(v, 1)); break;
        case Type::Array: {
            const ArrObj* a = Heap::arr(v);
            u8(a->read_only ? 1 : 0);
            u32(uint32_t(a->items.size()));
            for (const Value& item : a->items) value(item);
            break;
        }
        case Type::Dictionary: {
            const DictObj* d = Heap::dict(v);
            u8(d->read_only ? 1 : 0);
            u32(uint32_t(d->entries.size()));
            for (const DictEntry& e : d->entries) { value(e.key); value(e.value); }
            break;
        }
        default: break; // Nil (objects never appear in compiled constants)
        }
    }
};

struct Reader {
    const uint8_t* p;
    size_t left;
    bool ok = true;
    bool need(size_t n) {
        if (!ok || left < n) ok = false;
        return ok;
    }
    uint8_t u8() { if (!need(1)) return 0; const uint8_t v = *p++; --left; return v; }
    uint16_t u16() { const uint16_t a = u8(); return uint16_t(a | uint16_t(u8()) << 8); }
    uint32_t u32() {
        if (!need(4)) return 0;
        const uint32_t v = uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24;
        p += 4; left -= 4;
        return v;
    }
    int32_t i32() { return int32_t(u32()); }
    uint64_t u64() { const uint64_t lo = u32(); return lo | uint64_t(u32()) << 32; }
    Real f32() { const uint32_t v = u32(); Real f; std::memcpy(&f, &v, 4); return f; }
    std::string str() {
        const uint32_t n = u32();
        if (n > kMaxText || !need(n)) { ok = false; return {}; }
        std::string s(reinterpret_cast<const char*>(p), n);
        p += n; left -= n;
        return s;
    }
    // Element counts can never exceed the remaining bytes (each takes at least one).
    uint32_t count() {
        const uint32_t n = u32();
        if (n > left) ok = false;
        return ok ? n : 0;
    }
    Value value(int depth = 0) {
        const uint8_t t = u8();
        if (!ok || t >= kTypeCount || depth > kMaxValueDepth) { ok = false; return {}; }
        switch (Type(t)) {
        case Type::Nil: return {};
        case Type::Bool: { const uint8_t b = u8(); if (b > 1) ok = false; return Value(b == 1); }
        case Type::Int: return Value(Int(u64()));
        case Type::Float: return Value(f32());
        case Type::String: return Value(str());
        case Type::Vector3: { const Real x = f32(), y = f32(); return Value(Vector3{x, y, f32()}); }
        case Type::Vector2: { const Real x = f32(); return Value(Vector2{x, f32()}); }
        case Type::Color: { const Real r = f32(), g = f32(); return Value(Color{r, g, f32()}); }
        case Type::Array: {
            const bool read_only = u8() == 1;
            const uint32_t n = count();
            Value a = Value::new_array();
            for (uint32_t i = 0; i < n && ok; ++i) Heap::arr(a)->items.push_back(value(depth + 1));
            Heap::arr(a)->read_only = read_only;
            return a;
        }
        case Type::Dictionary: {
            const bool read_only = u8() == 1;
            const uint32_t n = count();
            Value d = Value::new_dictionary();
            for (uint32_t i = 0; i < n && ok; ++i) {
                Value key = value(depth + 1), item = value(depth + 1);
                if (!ok || !valid_dict_key(key)) { ok = false; break; }
                Heap::dict(d)->set(key, item);
            }
            Heap::dict(d)->read_only = read_only;
            return d;
        }
        default: ok = false; return {};
        }
    }
};

// Instruction length in 32-bit words.
uint32_t length_of(Op op) {
    switch (op) {
    case Op::ForNext: case Op::RangeNext: case Op::LtLocalJump: return 3;
    case Op::CallScript: case Op::CallNamed: case Op::CallSelf: case Op::AddLocalInt: return 2;
    default: return 1;
    }
}

bool verify_function(const ScriptData& sd, const FunctionData& f, std::string& why) {
    const uint32_t size = uint32_t(f.code.size());
    const uint32_t nconst = uint32_t(sd.constants.size()), nnames = uint32_t(sd.names.size());
    const uint32_t nvars = uint32_t(sd.vars.size()), nfuncs = uint32_t(sd.funcs.size());
    auto fail = [&](const char* what, uint32_t pc) {
        why = "function '" + f.name + "' at " + std::to_string(pc) + ": " + what;
        return false;
    };
    if (f.nrequired > f.nparams || f.nlocals < f.nparams || f.nlocals > kMaxLocals || f.frame_size < f.nlocals + 2
        || f.frame_size > 65535 + kMaxLocals || f.entries.size() != size_t(f.nparams - f.nrequired) + 1 || size == 0)
        return fail("invalid frame layout", 0);
    std::vector<uint8_t> start(size, 0);
    for (uint32_t pc = 0; pc < size;) {
        const Op op = op_of(f.code[pc]);
        if (uint32_t(op) >= uint32_t(Op::Count)) return fail("unknown instruction", pc);
        start[pc] = 1;
        const uint32_t n = length_of(op);
        if (pc + n > size) return fail("truncated instruction", pc);
        pc += n;
    }
    for (uint32_t entry : f.entries)
        if (entry >= size || !start[entry]) return fail("invalid entry point", entry);
    auto target = [&](uint32_t pc, int64_t offset) {
        const int64_t t = int64_t(pc) + offset;
        return t >= 0 && t < int64_t(size) && start[size_t(t)];
    };
    Op last = Op::Nop;
    for (uint32_t pc = 0; pc < size; pc += length_of(op_of(f.code[pc]))) {
        const uint32_t ins = f.code[pc], a = arg_of(ins);
        const Op op = last = op_of(ins);
        const uint32_t w1 = length_of(op) > 1 ? f.code[pc + 1] : 0, w2 = length_of(op) > 2 ? f.code[pc + 2] : 0;
        bool good = true;
        switch (op) {
        case Op::PushConst: good = a < nconst; break;
        case Op::LoadLocal: case Op::StoreLocal: case Op::AddLocalInt: good = a < f.nlocals; break;
        case Op::LoadMember: case Op::StoreMember: good = a < nvars; break;
        case Op::LoadSelfProp: case Op::StoreSelfProp: case Op::WbSelfProp: case Op::LoadSingleton:
        case Op::GetNamed: case Op::SetNamed: case Op::WbNamed: case Op::IsClass: case Op::AsClass:
            good = a < nnames; break;
        case Op::CheckClass: good = (a & 0xFFFFFu) < nnames; break;
        case Op::IsType: case Op::AsType: case Op::Convert: good = (a & 0xFFu) < kTypeCount; break;
        case Op::Jump: case Op::JumpIfFalse: case Op::JumpIfTrue: good = target(pc, sarg_of(ins)); break;
        case Op::ForPrep: good = a + 1 < f.nlocals; break;
        case Op::ForNext: good = a + 1 < f.nlocals && w1 < f.nlocals && target(pc, int32_t(w2)); break;
        case Op::RangePrep: good = (a & 0xFFFFu) + 2 < f.nlocals && (a >> 16) >= 1 && (a >> 16) <= 3; break;
        case Op::RangeNext: good = a + 2 < f.nlocals && w1 < f.nlocals && target(pc, int32_t(w2)); break;
        case Op::CallScript:
            good = w1 < nfuncs && a >= sd.funcs[w1].nrequired && a <= sd.funcs[w1].nparams; break;
        case Op::CallNamed: case Op::CallSelf: good = w1 < nnames; break;
        case Op::CallBuiltin: {
            const uint32_t index = a >> 8;
            const int argc = int(a & 0xFFu);
            good = index < uint32_t(builtin_func_count());
            if (good) {
                const BuiltinFunc& bf = builtin_funcs()[index];
                good = argc >= bf.min_args && (bf.max_args < 0 || argc <= bf.max_args);
            }
            break;
        }
        case Op::LtLocalJump: good = a < f.nlocals && w1 < nconst && target(pc, int32_t(w2)); break;
        case Op::PushBound:
            good = (a >> 1) < nconst && sd.constants[a >> 1].type() == Type::String && !sd.constants[a >> 1].as_string().empty();
            break;
        default: break;
        }
        if (!good) return fail("operand out of range", pc);
    }
    // Execution must never run past the end of the code.
    if (last != Op::Return && last != Op::ReturnNil && last != Op::Jump) return fail("missing return", size - 1);
    return true;
}

// The VM trusts bytecode, so every operand index and jump target is checked here.
// Operand stack depth (frame_size) is taken from the compiler as is; the CRC catches corrupted files.
bool verify(const ScriptData& sd, std::string& why) {
    const int nfuncs = int(sd.funcs.size());
    for (int index : {sd.init_fn, sd.onready_fn, sd.ready_fn, sd.process_fn})
        if (index < -1 || index >= nfuncs) { why = "invalid callback index"; return false; }
    for (size_t i = 0; i < sd.symbols.size(); ++i) {
        const SymEntry& s = sd.symbols[i];
        if (i > 0 && s.name < sd.symbols[i - 1].name) { why = "symbols not sorted"; return false; }
        const size_t limit = s.kind == SymKind::Var     ? sd.vars.size()
                           : s.kind == SymKind::Const   ? sd.consts.size()
                           : s.kind == SymKind::Signal  ? sd.signals.size()
                                                        : sd.funcs.size();
        if (uint8_t(s.kind) > uint8_t(SymKind::Signal) || s.index >= limit) { why = "invalid symbol " + s.name; return false; }
    }
    for (const VarData& v : sd.vars)
        if (uint32_t(v.type) >= kTypeCount) { why = "invalid variable type"; return false; }
    for (const FunctionData& f : sd.funcs)
        if (!verify_function(sd, f, why)) return false;
    return true;
}

} // namespace
} // namespace detail

std::vector<uint8_t> save_bytecode(const Script& script) {
    using namespace detail;
    const ScriptData& sd = script.data();
    Writer w;
    w.str(sd.path);
    w.str(sd.base_class);
    for (int index : {sd.init_fn, sd.onready_fn, sd.ready_fn, sd.process_fn}) w.i32(index);
    w.u32(uint32_t(sd.constants.size()));
    for (const Value& v : sd.constants) w.value(v);
    w.u32(uint32_t(sd.consts.size()));
    for (const auto& c : sd.consts) { w.str(c.first); w.value(c.second); }
    w.u32(uint32_t(sd.names.size()));
    for (const NameEntry& n : sd.names) w.str(n.name);
    w.u32(uint32_t(sd.vars.size()));
    for (const VarData& v : sd.vars) {
        w.str(v.name); w.u8(uint8_t(v.type)); w.u8(v.typed ? 1 : 0); w.str(v.class_name); w.value(v.initial);
    }
    w.u32(uint32_t(sd.symbols.size()));
    for (const SymEntry& s : sd.symbols) { w.str(s.name); w.u8(uint8_t(s.kind)); w.u32(s.index); }
    w.u32(uint32_t(sd.funcs.size()));
    for (const FunctionData& f : sd.funcs) {
        w.str(f.name); w.i32(f.line); w.u16(f.nparams); w.u16(f.nrequired); w.u32(f.nlocals); w.u32(f.frame_size);
        w.u32(uint32_t(f.code.size()));
        for (uint32_t word : f.code) w.u32(word);
        w.u32(uint32_t(f.entries.size()));
        for (uint32_t e : f.entries) w.u32(e);
        w.u32(uint32_t(f.lines.size()));
        for (const LineEntry& l : f.lines) { w.u32(l.pc); w.i32(l.line); }
    }
    w.u32(uint32_t(sd.exports.size()));
    for (const ExportInfo& e : sd.exports) {
        w.str(e.name); w.u8(uint8_t(e.type)); w.value(e.default_value); w.u8(e.has_range ? 1 : 0);
        w.f32(e.min); w.f32(e.max); w.f32(e.step); w.i32(e.line);
    }
    w.u32(uint32_t(sd.members.size()));
    for (const MemberInfo& m : sd.members) {
        w.str(m.name); w.u8(uint8_t(m.type)); w.i32(m.line);
        w.u8(uint8_t((m.exported ? 1 : 0) | (m.onready ? 2 : 0) | (m.constant ? 4 : 0)));
    }
    w.u32(uint32_t(sd.functions.size()));
    for (const FunctionInfo& f : sd.functions) {
        w.str(f.name); w.i32(f.line); w.u32(uint32_t(f.arguments.size()));
        for (const std::string& a : f.arguments) w.str(a);
    }
    w.u32(uint32_t(sd.signals.size()));
    for (const SignalInfo& sig : sd.signals) {
        w.str(sig.name); w.u32(uint32_t(sig.arguments.size()));
        for (const std::string& a : sig.arguments) w.str(a);
    }
    Writer file;
    file.u32(kMagic); file.u32(kFormat); file.u32(fingerprint());
    file.u32(uint32_t(w.out.size())); file.u32(crc32(w.out.data(), w.out.size()));
    file.out.insert(file.out.end(), w.out.begin(), w.out.end());
    return file.out;
}

CompileResult load_bytecode(const uint8_t* data, size_t size, const Host& host) {
    using namespace detail;
    CompileResult result;
    auto fail = [&](std::string message) {
        result.errors.push_back({0, 0, "Invalid JetScript bytecode: " + message});
        return result;
    };
    Reader head{data, data ? size : 0};
    if (head.u32() != kMagic) return fail("bad header.");
    if (head.u32() != kFormat) return fail("unsupported format version.");
    if (head.u32() != fingerprint()) return fail("compiled by a different JetScript version, rebuild the project.");
    const uint32_t length = head.u32(), crc = head.u32();
    if (!head.ok || length != head.left) return fail("truncated or with trailing data.");
    if (crc32(head.p, length) != crc) return fail("checksum mismatch (corrupted data).");
    Reader r{head.p, length};
    std::unique_ptr<ScriptData> sd(new ScriptData());
    sd->path = r.str();
    sd->base_class = r.str();
    sd->init_fn = r.i32(); sd->onready_fn = r.i32(); sd->ready_fn = r.i32(); sd->process_fn = r.i32();
    for (uint32_t i = 0, n = r.count(); i < n && r.ok; ++i) sd->constants.push_back(r.value());
    for (uint32_t i = 0, n = r.count(); i < n && r.ok; ++i) {
        std::string name = r.str();
        sd->consts.emplace_back(std::move(name), r.value());
    }
    for (uint32_t i = 0, n = r.count(); i < n && r.ok; ++i) {
        // Host ids belong to the running engine: resolve them again by name.
        NameEntry e;
        e.name = r.str();
        e.pid = host.property_id(e.name);
        e.mid = host.method_id(e.name);
        e.field = int8_t(find_builtin_field(e.name));
        e.bmethod = int16_t(find_builtin_method(e.name));
        e.smethod = int8_t(find_signal_method(e.name));
        sd->names.push_back(std::move(e));
    }
    for (uint32_t i = 0, n = r.count(); i < n && r.ok; ++i) {
        VarData v;
        v.name = r.str(); v.type = Type(r.u8()); v.typed = r.u8() == 1; v.class_name = r.str(); v.initial = r.value();
        sd->vars.push_back(std::move(v));
    }
    for (uint32_t i = 0, n = r.count(); i < n && r.ok; ++i) {
        SymEntry s;
        s.name = r.str(); s.kind = SymKind(r.u8()); s.index = r.u32();
        sd->symbols.push_back(std::move(s));
    }
    for (uint32_t i = 0, n = r.count(); i < n && r.ok; ++i) {
        FunctionData f;
        f.name = r.str(); f.line = r.i32(); f.nparams = r.u16(); f.nrequired = r.u16(); f.nlocals = r.u32(); f.frame_size = r.u32();
        for (uint32_t k = 0, m = r.count(); k < m && r.ok; ++k) f.code.push_back(r.u32());
        for (uint32_t k = 0, m = r.count(); k < m && r.ok; ++k) f.entries.push_back(r.u32());
        for (uint32_t k = 0, m = r.count(); k < m && r.ok; ++k) {
            LineEntry l;
            l.pc = r.u32(); l.line = r.i32();
            f.lines.push_back(l);
        }
        sd->funcs.push_back(std::move(f));
    }
    for (uint32_t i = 0, n = r.count(); i < n && r.ok; ++i) {
        ExportInfo e;
        e.name = r.str(); e.type = Type(r.u8()); e.default_value = r.value(); e.has_range = r.u8() == 1;
        e.min = r.f32(); e.max = r.f32(); e.step = r.f32(); e.line = r.i32();
        if (uint32_t(e.type) >= kTypeCount) r.ok = false;
        sd->exports.push_back(std::move(e));
    }
    for (uint32_t i = 0, n = r.count(); i < n && r.ok; ++i) {
        MemberInfo m;
        m.name = r.str(); m.type = Type(r.u8()); m.line = r.i32();
        const uint8_t flags = r.u8();
        m.exported = flags & 1; m.onready = (flags & 2) != 0; m.constant = (flags & 4) != 0;
        if (uint32_t(m.type) >= kTypeCount) r.ok = false;
        sd->members.push_back(std::move(m));
    }
    for (uint32_t i = 0, n = r.count(); i < n && r.ok; ++i) {
        FunctionInfo f;
        f.name = r.str(); f.line = r.i32();
        for (uint32_t k = 0, m = r.count(); k < m && r.ok; ++k) f.arguments.push_back(r.str());
        sd->functions.push_back(std::move(f));
    }
    for (uint32_t i = 0, n = r.count(); i < n && r.ok; ++i) {
        SignalInfo sig;
        sig.name = r.str();
        for (uint32_t k = 0, m = r.count(); k < m && r.ok; ++k) sig.arguments.push_back(r.str());
        sd->signals.push_back(std::move(sig));
    }
    if (!r.ok || r.left) return fail("malformed payload.");
    std::string why;
    if (!verify(*sd, why)) return fail(why);
    sd->serial = next_script_serial();
    result.script = std::shared_ptr<const Script>(new Script(std::move(sd)));
    return result;
}

} // namespace jetscript
