#include "js_parser.hpp"

#include <algorithm>
#include <cmath>

namespace jetscript {

namespace detail {

namespace {

struct RType {
    bool present = false;  // annotation given
    bool ok = true;
    bool variant = false;
    bool is_void = false;
    Type type = Type::Nil;
    std::string cls;       // engine class name for Object annotations
    bool typed() const { return present && ok && !variant && !is_void; }
};

struct Local {
    uint32_t name = 0;
    uint32_t slot = 0;
    SType stype = kUnknown;
    RType rt;
    bool is_const = false;
    bool used = false;
    bool warn_unused = false;
    Value cval;
    int line = 0, col = 0;
};

struct LoopCtx {
    size_t continue_target = 0;
    std::vector<size_t> breaks;
};

struct FuncState {
    FunctionData* fd = nullptr;
    std::vector<Local> locals;
    std::vector<size_t> scopes;
    uint32_t next_slot = 0, max_slots = 0;
    int depth = 0, max_depth = 0;
    std::vector<LoopCtx> loops;
    RType ret;
    int cur_line = 0;
};

struct ConstInfo {
    const ConstDecl* decl = nullptr;
    Value value;
    int state = 0; // 0 unresolved, 1 resolving, 2 resolved, 3 failed
    int line = 0, col = 0;
    uint32_t name = 0;
};

struct EnumInfo {
    uint32_t name = 0;
    Value dict;
    std::vector<std::pair<uint32_t, Int>> values;
    int line = 0;
};

enum class CS : uint8_t { Var, Const, Enum, Func, Signal };
struct ClassSym {
    CS kind;
    uint32_t index;
    int line;
};

struct WB {
    Op op;
    uint32_t arg;
    int delta;
};

bool is_container(const Value& v) { return v.type() == Type::Array || v.type() == Type::Dictionary; }

SType stype_of(const Value& v) { return SType(v.type()); }

class Compiler {
public:
    Compiler(Ast& ast, const Host& host, std::vector<Diagnostic>& errors, std::vector<Diagnostic>& warnings)
        : ast_(ast), host_(host), errors_(errors), warnings_(warnings) {}

    void run(ScriptData& sd);

private:
    void error(int line, int col, std::string msg);
    void warning(int line, int col, std::string msg);
    const std::string& nm(uint32_t id) const { return ast_.name(id); }

    void setup_base();
    void register_symbols();
    void resolve_enums();
    bool resolve_const(uint32_t index, Value& out);
    RType resolve_type(const TypeRef& t);
    const PropertyInfo* find_prop(const std::string& name) const;
    const MethodInfo* find_method(const std::string& name) const;
    bool find_signal(const std::string& name) const;
    bool class_constant(const std::string& cls, const std::string& name, Value& out) const;
    void emit_bound(const std::string& name, bool signal);
    bool class_exists(const std::string& name) const;
    void build_metadata();

    void begin_function(FunctionData& fd, int line);
    void end_function();
    void compile_init(bool onready);
    void compile_function(const FuncDecl& f, FunctionData& fd);
    void push_scope();
    void pop_scope();
    const Local* find_local(uint32_t name) const;
    Local& declare_local(uint32_t name, int line, int col, bool with_slot = true);
    uint32_t alloc_hidden(uint32_t count);

    size_t pc() const { return fs_->fd->code.size(); }
    void adjust(int d);
    void emit(Op op, uint32_t arg, int delta);
    void emit_word(uint32_t w) { fs_->fd->code.push_back(w); }
    size_t emit_jump(Op op, int delta);
    void patch(size_t at) { patch_to(at, pc()); }
    void patch_to(size_t at, size_t target);
    void patch_all(std::vector<size_t>& list) {
        for (size_t at : list) patch(at);
        list.clear();
    }
    void emit_jump_back(size_t target);
    void emit_const(const Value& v);
    uint32_t add_const(const Value& v);
    uint32_t name_entry(const std::string& name);
    void emit_convert(const RType& rt, uint32_t ctx, uint32_t param, SType src, int line, int col, const std::string& what);

    // statements (recursive path: compile_block -> compile_stmt -> compile_if/... -> compile_block)
    void compile_block(const std::vector<Stmt*>& body);
    void compile_stmt(const Stmt* s);
    JS_NOINLINE void compile_simple_stmt(const Stmt* s);
    JS_NOINLINE void compile_var(const Stmt* s);
    JS_NOINLINE void compile_local_const(const Stmt* s);
    JS_NOINLINE void compile_assign(const Stmt* s);
    JS_NOINLINE bool assign_simple(const Stmt* s);
    JS_NOINLINE void compile_if(const Stmt* s);
    JS_NOINLINE void compile_while(const Stmt* s);
    JS_NOINLINE void compile_for(const Stmt* s);
    JS_NOINLINE void for_begin(const Stmt* s, size_t& top);
    JS_NOINLINE void for_end(const Stmt* s, size_t top);
    JS_NOINLINE void compile_match(const Stmt* s);
    JS_NOINLINE size_t match_patterns(const MatchBranch& br, uint32_t tmp, bool& unconditional);
    JS_NOINLINE void compile_return(const Stmt* s);
    void cond_jump(Node* n, bool when, std::vector<size_t>& out);
    JS_NOINLINE int const_truth(Node* n);
    JS_NOINLINE bool lt_local_jump(Node* n, std::vector<size_t>& out);

    // expressions (recursive path: compile_expr -> compile_xxx -> compile_expr)
    SType compile_expr(Node* n);
    JS_NOINLINE bool emit_folded(Node* n, SType& type);
    JS_NOINLINE SType emit_literal(Node* n);
    JS_NOINLINE SType emit_nil_error(Node* n);
    JS_NOINLINE SType compile_ident(Node* n);
    SType compile_logic(Node* n);
    SType compile_ternary(Node* n);
    SType compile_member(Node* n);
    JS_NOINLINE bool member_static(Node* n, SType& type);
    JS_NOINLINE SType emit_get_named(Node* n, SType base_type);
    SType compile_call(Node* n);
    JS_NOINLINE bool plan_call(Node* n, uint32_t& word0, uint32_t& word1, SType& ret, uint8_t& retmode);
    SType compile_method_call(Node* n);
    JS_NOINLINE int method_static(Node* n);
    JS_NOINLINE SType emit_is_as(Node* n);
    JS_NOINLINE SType emit_get_node(Node* n);
    SType push_lvalue_base(Node* e, std::vector<WB>& wbs);
    JS_NOINLINE bool lvalue_static(Node* e, std::vector<WB>& wbs, SType& type);
    // static type checks run the VM's own operators on sample values
    JS_NOINLINE SType check_binary(BinOp op, SType a, SType b, int line, int col);
    JS_NOINLINE SType check_unary(UnOp op, SType a, int line, int col);
    JS_NOINLINE SType check_index(SType base, SType index, int line, int col);
    JS_NOINLINE SType check_method(Node* n, SType receiver);
    JS_NOINLINE SType check_field_set(Node* target, SType base, SType value, int op, int line, int col);
    JS_NOINLINE void check_prop_assign(const PropertyInfo* p, SType value, int line, int col);
    JS_NOINLINE void check_call_args(Node* n, uint32_t fi, const std::vector<SType>* types);
    Local* find_local_mut(uint32_t name);
    bool try_const(Node* n, Value& out, bool containers = false);
    bool const_eval(Node* n, Value& out, bool containers);
    JS_NOINLINE bool const_ident(Node* n, Value& out);
    JS_NOINLINE int const_static_member(Node* n, Value& out);
    JS_NOINLINE bool const_field(Node* n, Value& base);
    JS_NOINLINE bool const_unary(Node* n, Value& v);
    JS_NOINLINE bool const_binary(Node* n, Value& a, const Value& b);
    JS_NOINLINE bool const_call(Node* n, Value& out, bool containers);
    JS_NOINLINE bool const_call_invoke(Node* n, std::vector<Value>& args, Value& out);
    JS_NOINLINE int const_call_check(Node* n);
    JS_NOINLINE bool const_container(Node* n, Value& out);
    bool is_builtin_call(uint32_t name) const;
    bool names_type(uint32_t name, Type& t, bool& is_class) const;

    Ast& ast_;
    const Host& host_;
    std::vector<Diagnostic>& errors_;
    std::vector<Diagnostic>& warnings_;
    ScriptData* sd_ = nullptr;
    std::vector<const ClassInfo*> chain_;
    std::unordered_map<uint32_t, ClassSym> syms_;
    std::vector<ConstInfo> consts_;
    std::vector<EnumInfo> enums_;
    std::vector<RType> var_types_;
    std::vector<SType> var_stype_;
    std::vector<SType> init_stype_; // static type of each member initializer (export inference)
    std::vector<RType> func_ret_;
    std::vector<std::vector<RType>> func_params_;
    std::vector<uint16_t> func_nreq_;
    std::unordered_map<std::string, uint32_t> name_entries_;
    FuncState* fs_ = nullptr;
    int get_node_mid_ = -1;
};

void Compiler::error(int line, int col, std::string msg) {
    if (int(errors_.size()) >= kMaxErrors) return;
    for (const Diagnostic& d : errors_)
        if (d.line == line && d.message == msg) return;
    Diagnostic d;
    d.line = line;
    d.column = col;
    d.message = std::move(msg);
    errors_.push_back(std::move(d));
}

void Compiler::warning(int line, int col, std::string msg) {
    if (int(warnings_.size()) >= kMaxErrors) return;
    Diagnostic d;
    d.line = line;
    d.column = col;
    d.message = std::move(msg);
    warnings_.push_back(std::move(d));
}

void Compiler::setup_base() {
    std::string base = "Node";
    if (ast_.has_extends) base = nm(ast_.extends_name);
    const ClassInfo* ci = host_.find_class(base);
    if (!ci) {
        error(ast_.extends_line, ast_.extends_col, "Could not find base class \"" + base + "\".");
        sd_->base_class = base;
        return;
    }
    sd_->base_class = ci->name.empty() ? base : ci->name;
    for (int guard = 0; ci && guard < 64; ++guard) {
        chain_.push_back(ci);
        if (ci->base.empty()) break;
        ci = host_.find_class(ci->base);
    }
    get_node_mid_ = host_.method_id("get_node");
}

const PropertyInfo* Compiler::find_prop(const std::string& name) const {
    for (const ClassInfo* c : chain_)
        for (const PropertyInfo& p : c->properties)
            if (p.name == name) return &p;
    return nullptr;
}

const MethodInfo* Compiler::find_method(const std::string& name) const {
    for (const ClassInfo* c : chain_)
        for (const MethodInfo& m : c->methods)
            if (m.name == name) return &m;
    return nullptr;
}

// Tween.TRANS_SINE: the class's constants or its bases'.
bool Compiler::class_constant(const std::string& cls, const std::string& name, Value& out) const {
    const ClassInfo* ci = host_.find_class(cls);
    for (int guard = 0; ci && guard < 64; ++guard) {
        for (const auto& c : ci->constants)
            if (c.first == name) {
                out = Value(c.second);
                return true;
            }
        ci = ci->base.empty() ? nullptr : host_.find_class(ci->base);
    }
    return false;
}

bool Compiler::find_signal(const std::string& name) const {
    for (const ClassInfo* c : chain_)
        for (const SignalInfo& sig : c->signals)
            if (sig.name == name) return true;
    return false;
}

// A Callable or Signal of self: `_on_timeout`, `health_changed`, `queue_free`.
void Compiler::emit_bound(const std::string& name, bool signal) {
    emit(Op::PushBound, (add_const(Value(name)) << 1) | (signal ? 1u : 0u), 1);
}

bool Compiler::class_exists(const std::string& name) const { return host_.find_class(name) != nullptr; }

void Compiler::register_symbols() {
    auto add = [&](uint32_t name, CS kind, uint32_t index, int line, int col) {
        auto it = syms_.find(name);
        if (it != syms_.end()) {
            error(line, col, "The member \"" + nm(name) + "\" already exists in this script (line " +
                                 format_int(it->second.line) + ").");
            return;
        }
        if (kind == CS::Var && find_prop(nm(name))) {
            error(line, col, "The member \"" + nm(name) + "\" already exists in parent class " + sd_->base_class + ".");
        }
        if (kind == CS::Func && find_method(nm(name)) && nm(name)[0] != '_')
            warning(line, col, "The method \"" + nm(name) + "()\" overrides a method of the engine class; the engine will not call it.");
        syms_[name] = ClassSym{kind, index, line};
    };
    for (size_t i = 0; i < ast_.vars.size(); ++i) add(ast_.vars[i].name, CS::Var, uint32_t(i), ast_.vars[i].line, ast_.vars[i].col);
    for (size_t i = 0; i < ast_.consts.size(); ++i) {
        ConstInfo ci;
        ci.decl = &ast_.consts[i];
        ci.line = ast_.consts[i].line;
        ci.col = ast_.consts[i].col;
        ci.name = ast_.consts[i].name;
        consts_.push_back(ci);
        add(ast_.consts[i].name, CS::Const, uint32_t(consts_.size() - 1), ci.line, ci.col);
    }
    for (size_t i = 0; i < ast_.funcs.size(); ++i) add(ast_.funcs[i].name, CS::Func, uint32_t(i), ast_.funcs[i].line, ast_.funcs[i].col);
    for (size_t i = 0; i < ast_.signals.size(); ++i) {
        const SignalDecl& sig = ast_.signals[i];
        if (find_signal(nm(sig.name)))
            error(sig.line, sig.col, "The signal \"" + nm(sig.name) + "\" already exists in parent class " + sd_->base_class + ".");
        add(sig.name, CS::Signal, uint32_t(i), sig.line, sig.col);
    }
    // a named enum becomes one symbol, the values of an unnamed one become constants (resolved later)
    for (size_t i = 0; i < ast_.enums.size(); ++i) {
        const EnumDecl& e = ast_.enums[i];
        EnumInfo info;
        info.name = e.name;
        info.line = e.line;
        enums_.push_back(info);
        if (e.name) {
            add(e.name, CS::Enum, uint32_t(i), e.line, e.col);
        } else {
            for (size_t k = 0; k < e.values.size(); ++k) {
                ConstInfo ci;
                ci.state = 3; // resolved by resolve_enums
                ci.line = e.positions[k].first;
                ci.col = e.positions[k].second;
                ci.name = e.values[k].first;
                consts_.push_back(ci);
                add(e.values[k].first, CS::Const, uint32_t(consts_.size() - 1), ci.line, ci.col);
            }
        }
    }
}

void Compiler::resolve_enums() {
    size_t unnamed_const = ast_.consts.size();
    for (size_t i = 0; i < ast_.enums.size(); ++i) {
        const EnumDecl& e = ast_.enums[i];
        Value dict = Value::new_dictionary();
        Int next = 0;
        for (size_t k = 0; k < e.values.size(); ++k) {
            Int v = next;
            if (e.values[k].second) {
                Value cv;
                if (const_eval(e.values[k].second, cv, false) && cv.type() == Type::Int) {
                    v = cv.as_int();
                } else {
                    error(e.positions[k].first, e.positions[k].second,
                          "Enum values must be constant integers (\"" + nm(e.values[k].first) + "\").");
                }
            }
            next = Int(uint64_t(v) + 1);
            enums_[i].values.emplace_back(e.values[k].first, v);
            if (Heap::dict(dict)->find_str(nm(e.values[k].first)) >= 0)
                error(e.positions[k].first, e.positions[k].second, "Duplicate enum key \"" + nm(e.values[k].first) + "\".");
            Heap::dict(dict)->set(Value(nm(e.values[k].first)), Value(v));
            if (!e.name) {
                ConstInfo& ci = consts_[unnamed_const++];
                ci.value = Value(v);
                ci.state = 2;
            }
        }
        Heap::dict(dict)->read_only = true;
        enums_[i].dict = dict;
    }
}

bool Compiler::resolve_const(uint32_t index, Value& out) {
    ConstInfo& ci = consts_[index];
    if (ci.state == 2) {
        out = ci.value;
        return true;
    }
    if (ci.state == 1) {
        error(ci.line, ci.col, "Could not resolve constant \"" + nm(ci.name) + "\": cyclic reference.");
        ci.state = 3;
        return false;
    }
    if (ci.state == 3) return false;
    ci.state = 1;
    Value v;
    if (!ci.decl || !const_eval(ci.decl->init, v, true)) {
        if (ci.decl)
            error(ci.line, ci.col, "Assigned value for constant \"" + nm(ci.name) + "\" isn't a constant expression.");
        ci.state = 3;
        return false;
    }
    RType rt = resolve_type(ci.decl->type);
    if (rt.typed() && rt.type != Type::Object && !convert_to(rt.type, v)) {
        error(ci.line, ci.col, std::string("Cannot assign a value of type \"") + type_name(v.type()) + "\" to constant \"" +
                                   nm(ci.name) + "\" with specified type \"" + type_name(rt.type) + "\".");
    }
    if (v.type() == Type::Array) Heap::arr(v)->read_only = true;
    if (v.type() == Type::Dictionary) Heap::dict(v)->read_only = true;
    ci.value = v;
    ci.state = 2;
    out = v;
    return true;
}

RType Compiler::resolve_type(const TypeRef& t) {
    RType r;
    if (!t.present) return r;
    r.present = true;
    const std::string& name = nm(t.name);
    if (name == "void") {
        r.is_void = true;
        return r;
    }
    bool variant = false;
    Type bt;
    if (builtin_type_from_name(name, bt, variant)) {
        r.type = bt;
        r.variant = variant;
        if (bt == Type::Object && !variant) r.cls = "Object";
        return r;
    }
    auto it = syms_.find(t.name);
    if (it != syms_.end() && it->second.kind == CS::Enum) {
        r.type = Type::Int;
        return r;
    }
    if (class_exists(name)) {
        r.type = Type::Object;
        r.cls = name;
        return r;
    }
    error(t.line, t.col, "Could not find type \"" + name + "\" in the current scope.");
    r.ok = false;
    r.variant = true;
    return r;
}

void Compiler::begin_function(FunctionData& fd, int line) {
    fs_ = new FuncState();
    fs_->fd = &fd;
    fs_->cur_line = line;
    fd.line = line;
}

void Compiler::end_function() {
    FunctionData& fd = *fs_->fd;
    fd.nlocals = fs_->max_slots;
    fd.frame_size = fs_->max_slots + uint32_t(fs_->max_depth) + 2;
    if (fd.frame_size > 65535) error(fd.line, 1, "Function \"" + fd.name + "\" is too large.");
    delete fs_;
    fs_ = nullptr;
}

void Compiler::push_scope() { fs_->scopes.push_back(fs_->locals.size()); }

void Compiler::pop_scope() {
    size_t mark = fs_->scopes.back();
    fs_->scopes.pop_back();
    for (size_t i = mark; i < fs_->locals.size(); ++i) {
        const Local& l = fs_->locals[i];
        if (l.warn_unused && !l.used && l.name && nm(l.name)[0] != '_')
            warning(l.line, l.col, "The local " + std::string(l.is_const ? "constant" : "variable") + " \"" + nm(l.name) +
                                       "\" is declared but never used in the block. If this is intended, prefix it with an underscore: \"_" +
                                       nm(l.name) + "\".");
    }
    fs_->locals.resize(mark);
    uint32_t top = 0;
    for (const Local& l : fs_->locals)
        if (!l.is_const && l.slot + 1 > top) top = l.slot + 1;
    fs_->next_slot = top; // slots of the closed scope (including hidden ones) are reused
}

const Local* Compiler::find_local(uint32_t name) const {
    if (!fs_) return nullptr;
    for (size_t i = fs_->locals.size(); i-- > 0;)
        if (fs_->locals[i].name == name) return &fs_->locals[i];
    return nullptr;
}

Local* Compiler::find_local_mut(uint32_t name) {
    if (!fs_) return nullptr;
    for (size_t i = fs_->locals.size(); i-- > 0;)
        if (fs_->locals[i].name == name) {
            fs_->locals[i].used = true;
            return &fs_->locals[i];
        }
    return nullptr;
}

uint32_t Compiler::alloc_hidden(uint32_t count) {
    uint32_t s = fs_->next_slot;
    fs_->next_slot += count;
    if (fs_->next_slot > fs_->max_slots) fs_->max_slots = fs_->next_slot;
    // reserve with an unnamed local so pop_scope keeps track of the slot
    for (uint32_t i = 0; i < count; ++i) {
        Local l;
        l.name = 0;
        l.slot = s + i;
        fs_->locals.push_back(l);
    }
    return s;
}

Local& Compiler::declare_local(uint32_t name, int line, int col, bool with_slot) {
    if (const Local* prev = find_local(name)) {
        error(line, col, "There is already a variable named \"" + nm(name) + "\" declared in this function (line " +
                             format_int(prev->line) + ").");
    } else {
        auto it = syms_.find(name);
        if (it != syms_.end() && it->second.kind == CS::Var)
            warning(line, col, "The local variable \"" + nm(name) + "\" is shadowing an already-declared member variable (line " +
                                   format_int(it->second.line) + ").");
    }
    Local l;
    l.name = name;
    l.slot = with_slot ? fs_->next_slot++ : 0;
    l.is_const = !with_slot;
    l.line = line;
    l.col = col;
    if (fs_->next_slot > fs_->max_slots) fs_->max_slots = fs_->next_slot;
    if (fs_->max_slots > kMaxLocals) error(line, col, "Too many local variables in function.");
    fs_->locals.push_back(l);
    return fs_->locals.back();
}

void Compiler::adjust(int d) {
    fs_->depth += d;
    if (fs_->depth < 0) fs_->depth = 0; // only after errors
    if (fs_->depth > fs_->max_depth) fs_->max_depth = fs_->depth;
}

void Compiler::emit(Op op, uint32_t arg, int delta) {
    FunctionData& fd = *fs_->fd;
    if (fd.lines.empty() || fd.lines.back().line != fs_->cur_line) {
        if (!fd.lines.empty() && fd.lines.back().pc == fd.code.size()) fd.lines.back().line = fs_->cur_line;
        else fd.lines.push_back(LineEntry{uint32_t(fd.code.size()), fs_->cur_line});
    }
    fd.code.push_back(enc(op, arg & 0xFFFFFFu));
    adjust(delta);
}

size_t Compiler::emit_jump(Op op, int delta) {
    size_t at = pc();
    emit(op, 0, delta);
    return at;
}

void Compiler::patch_to(size_t at, size_t target) {
    uint32_t& w = fs_->fd->code[at];
    int32_t off = int32_t(target) - int32_t(at);
    if (op_of(w) == Op::LtLocalJump) fs_->fd->code[at + 2] = uint32_t(off);
    else w = enc(op_of(w), senc(off));
}

void Compiler::emit_jump_back(size_t target) {
    size_t at = pc();
    emit(Op::Jump, 0, 0);
    patch_to(at, target);
}

uint32_t Compiler::add_const(const Value& v) {
    for (size_t i = 0; i < sd_->constants.size(); ++i) {
        const Value& c = sd_->constants[i];
        if (!is_container(c) && strict_equal(c, v) && !(v.type() == Type::Float && std::signbit(Heap::f(v)) != std::signbit(Heap::f(c))))
            return uint32_t(i);
        if (is_container(c) && Heap::p(c) == Heap::p(v)) return uint32_t(i);
    }
    sd_->constants.push_back(v);
    return uint32_t(sd_->constants.size() - 1);
}

void Compiler::emit_const(const Value& v) {
    switch (v.type()) {
    case Type::Nil: emit(Op::PushNil, 0, 1); return;
    case Type::Bool: emit(Heap::b(v) ? Op::PushTrue : Op::PushFalse, 0, 1); return;
    case Type::Int:
        if (Heap::i(v) >= -0x800000 && Heap::i(v) < 0x800000) {
            emit(Op::PushInt, senc(int32_t(Heap::i(v))), 1);
            return;
        }
        break;
    default: break;
    }
    emit(Op::PushConst, add_const(v), 1);
}

uint32_t Compiler::name_entry(const std::string& name) {
    auto it = name_entries_.find(name);
    if (it != name_entries_.end()) return it->second;
    NameEntry e;
    e.name = name;
    e.pid = host_.property_id(name);
    e.mid = host_.method_id(name);
    e.field = int8_t(find_builtin_field(name));
    e.bmethod = int16_t(find_builtin_method(name));
    e.smethod = int8_t(find_signal_method(name));
    uint32_t id = uint32_t(sd_->names.size());
    sd_->names.push_back(std::move(e));
    name_entries_[name] = id;
    return id;
}

static bool static_compatible(SType src, const RType& rt) {
    if (src == kUnknown || !rt.typed()) return true;
    Type s = Type(src), t = rt.type;
    if (s == t) return true;
    if ((t == Type::Int || t == Type::Float) && (s == Type::Int || s == Type::Float)) return true;
    if (t == Type::Object && s == Type::Nil) return true;
    return false;
}

void Compiler::emit_convert(const RType& rt, uint32_t ctx, uint32_t param, SType src, int line, int col,
                            const std::string& what) {
    if (!rt.typed()) return;
    if (!static_compatible(src, rt)) {
        std::string tn = rt.type == Type::Object ? rt.cls : std::string(type_name(rt.type));
        if (ctx == kConvReturn)
            error(line, col, std::string("Cannot return a value of type \"") + type_name(Type(src)) +
                                 "\" as \"" + tn + "\".");
        else if (what.empty())
            error(line, col, std::string("Cannot assign a value of type \"") + type_name(Type(src)) + "\" as \"" + tn + "\".");
        else
            error(line, col, std::string("Cannot assign a value of type \"") + type_name(Type(src)) + "\" to " + what +
                                 " with specified type \"" + tn + "\".");
        return;
    }
    if (rt.type == Type::Object) {
        if (src == SType(Type::Nil)) return;
        if (rt.cls.empty() || rt.cls == "Object") emit(Op::Convert, uint32_t(Type::Object) | (ctx << 8) | (param << 10), 0);
        else emit(Op::CheckClass, name_entry(rt.cls) | (ctx << 20), 0);
        return;
    }
    if (src == SType(rt.type)) return; // statically known to match
    emit(Op::Convert, uint32_t(rt.type) | (ctx << 8) | ((param & 0x3FFFu) << 10), 0);
}

bool Compiler::is_builtin_call(uint32_t name) const {
    if (find_local(name)) return false;
    auto it = syms_.find(name);
    if (it != syms_.end()) return false;
    if (find_method(nm(name))) return false;
    return find_builtin_func(nm(name)) >= 0 || nm(name) == "String";
}

bool Compiler::names_type(uint32_t name, Type& t, bool& is_class) const {
    if (find_local(name) || syms_.count(name) || find_prop(nm(name))) return false;
    if (host_.singleton(nm(name)) != 0) return false; // e.g. `Input` is an object, not a type
    bool variant = false;
    is_class = false;
    if (builtin_type_from_name(nm(name), t, variant)) return !variant;
    if (class_exists(nm(name))) {
        t = Type::Object;
        is_class = true;
        return true;
    }
    return false;
}

bool Compiler::try_const(Node* n, Value& out, bool containers) {
    if (!n) return false;
    if (n->kind == NK::Literal) {
        out = n->value;
        return true;
    }
    if (containers) return const_eval(n, out, true);
    if (n->cstate == 2) return false;
    if (n->cstate == 1) {
        out = n->value;
        return true;
    }
    bool ok = const_eval(n, out, false);
    if (ok && is_container(out) && !static_cast<HeapObj*>(Heap::p(out))->read_only) ok = false;
    n->cstate = ok ? 1 : 2;
    if (ok) n->value = out;
    return ok;
}

bool Compiler::const_ident(Node* n, Value& out) {
    if (const Local* l = find_local_mut(n->name)) {
        if (!l->is_const) return false;
        out = l->cval;
        return true;
    }
    auto it = syms_.find(n->name);
    if (it != syms_.end()) {
        if (it->second.kind == CS::Const) return resolve_const(it->second.index, out);
        if (it->second.kind == CS::Enum) {
            out = enums_[it->second.index].dict;
            return !out.is_nil();
        }
        return false;
    }
    if (find_prop(nm(n->name))) return false;
    return builtin_global_constant(nm(n->name), out);
}

// 1: constant found, 0: not constant, -1: not a static member (evaluate the base)
int Compiler::const_static_member(Node* n, Value& out) {
    Node* b = n->a;
    if (b->kind != NK::Ident || find_local(b->name)) return -1;
    auto it = syms_.find(b->name);
    if (it != syms_.end() && it->second.kind == CS::Enum) {
        for (const auto& kv : enums_[it->second.index].values)
            if (kv.first == n->name) {
                out = Value(kv.second);
                return 1;
            }
        return 0;
    }
    Type t;
    bool is_class = false;
    if (it == syms_.end() && names_type(b->name, t, is_class)) {
        if (is_class) return class_constant(nm(b->name), nm(n->name), out) ? 1 : 0;
        return builtin_type_constant(t, nm(n->name), out) ? 1 : 0;
    }
    return -1;
}

bool Compiler::const_field(Node* n, Value& base) {
    int f = find_builtin_field(nm(n->name));
    if ((base.type() == Type::Vector3 && f >= 0 && f <= 2) || (base.type() == Type::Vector2 && f >= 0 && f <= 1)) {
        base = Value(Heap::comp(base, f));
        return true;
    }
    if (base.type() == Type::Color && f >= 3) {
        base = Value(f == 6 ? Real(1) : Heap::comp(base, f - 3));
        return true;
    }
    return false;
}

bool Compiler::const_unary(Node* n, Value& v) {
    std::string err;
    Value r;
    if (!unary_op(UnOp(n->op), v, r, err)) return false;
    v = r;
    return true;
}

bool Compiler::const_binary(Node* n, Value& a, const Value& b) {
    if (is_container(a) || is_container(b)) return false;
    std::string err;
    Value r;
    if (!binary_op(BinOp(n->op), a, b, r, err)) return false;
    a = r;
    return true;
}

// Returns the builtin index for a foldable call, or -1.
int Compiler::const_call_check(Node* n) {
    if (!is_builtin_call(n->name)) return -1;
    int id = nm(n->name) == "String" ? find_builtin_func("str") : find_builtin_func(nm(n->name));
    if (id < 0) return -1;
    const BuiltinFunc& bf = builtin_funcs()[id];
    if (!(bf.flags & kPure)) return -1;
    int argc = int(n->list.size());
    if (argc < bf.min_args || (bf.max_args >= 0 && argc > bf.max_args)) return -1;
    return id;
}

bool Compiler::const_call_invoke(Node* n, std::vector<Value>& args, Value& out) {
    int id = const_call_check(n);
    if (id < 0) return false;
    BuiltinCtx ctx;
    Value r;
    if (!builtin_funcs()[id].fn(ctx, args.data(), int(args.size()), r)) return false;
    if (is_container(r)) return false;
    out = r;
    return true;
}

bool Compiler::const_call(Node* n, Value& out, bool containers) {
    if (const_call_check(n) < 0) return false;
    std::vector<Value> args(n->list.size());
    for (size_t i = 0; i < n->list.size(); ++i)
        if (!const_eval(n->list[i], args[i], containers) || is_container(args[i])) return false;
    return const_call_invoke(n, args, out);
}

bool Compiler::const_container(Node* n, Value& out) {
    if (n->kind == NK::ArrayLit) {
        std::vector<Value> items(n->list.size());
        for (size_t i = 0; i < n->list.size(); ++i)
            if (!const_eval(n->list[i], items[i], true)) return false;
        out = make_array(std::move(items));
        Heap::arr(out)->read_only = true;
        return true;
    }
    Value d = Value::new_dictionary();
    for (size_t i = 0; i + 1 < n->list.size(); i += 2) {
        Value k, v;
        if (!const_eval(n->list[i], k, true) || !const_eval(n->list[i + 1], v, true)) return false;
        if (!valid_dict_key(k)) return false;
        Heap::dict(d)->set(k, v);
    }
    Heap::dict(d)->read_only = true;
    out = d;
    return true;
}

bool Compiler::const_eval(Node* n, Value& out, bool containers) {
    if (!n) return false;
    switch (n->kind) {
    case NK::Literal: out = n->value; return true;
    case NK::Ident: return const_ident(n, out);
    case NK::Member: {
        int r = const_static_member(n, out);
        if (r >= 0) return r == 1;
        if (!const_eval(n->a, out, containers)) return false;
        return const_field(n, out);
    }
    case NK::Unary: return const_eval(n->a, out, containers) && const_unary(n, out);
    case NK::Not:
        if (!const_eval(n->a, out, containers)) return false;
        out = Value(!truthy(out));
        return true;
    case NK::Binary: {
        Value b;
        return const_eval(n->a, out, containers) && const_eval(n->b, b, containers) && const_binary(n, out, b);
    }
    case NK::And:
    case NK::Or: {
        Value b;
        if (!const_eval(n->a, out, containers) || !const_eval(n->b, b, containers)) return false;
        bool r = n->kind == NK::And ? (truthy(out) && truthy(b)) : (truthy(out) || truthy(b));
        out = Value(r);
        return true;
    }
    case NK::Ternary: {
        Value c;
        if (!const_eval(n->b, c, containers)) return false;
        return const_eval(truthy(c) ? n->a : n->c, out, containers);
    }
    case NK::Call: return const_call(n, out, containers);
    case NK::ArrayLit:
    case NK::DictLit: return containers && const_container(n, out);
    default: return false;
    }
}

void Compiler::compile_block(const std::vector<Stmt*>& body) {
    push_scope();
    for (const Stmt* s : body) compile_stmt(s);
    pop_scope();
}

void Compiler::compile_stmt(const Stmt* s) {
    fs_->cur_line = s->line;
    fs_->depth = 0;
    switch (s->kind) {
    case SK::If: compile_if(s); return;
    case SK::While: compile_while(s); return;
    case SK::For: compile_for(s); return;
    case SK::Match: compile_match(s); return;
    default: compile_simple_stmt(s); return;
    }
}

void Compiler::compile_simple_stmt(const Stmt* s) {
    switch (s->kind) {
    case SK::Expr: {
        Node* e = s->a;
        bool effect = e && (e->kind == NK::Call || e->kind == NK::MethodCall || e->kind == NK::GetNode);
        if (e && !effect && e->kind != NK::Bad)
            warning(s->line, s->col, "Standalone expression (the line may have no effect).");
        compile_expr(e);
        emit(Op::Pop, 0, -1);
        return;
    }
    case SK::Var: compile_var(s); return;
    case SK::Const: compile_local_const(s); return;
    case SK::Assign: compile_assign(s); return;
    case SK::Pass: return;
    case SK::Break:
        if (fs_->loops.empty()) return; // reported by the parser
        fs_->loops.back().breaks.push_back(emit_jump(Op::Jump, 0));
        return;
    case SK::Continue:
        if (fs_->loops.empty()) return;
        emit_jump_back(fs_->loops.back().continue_target);
        return;
    case SK::Return: compile_return(s); return;
    default: return;
    }
}

void Compiler::compile_var(const Stmt* s) {
    RType rt = resolve_type(s->type);
    if (rt.is_void) error(s->type.line, s->type.col, "\"void\" can't be used as a variable type.");
    SType st = kUnknown;
    if (s->a) {
        st = compile_expr(s->a);
        if (s->infer && st == SType(Type::Nil))
            error(s->line, s->col, "Cannot infer the type of \"" + nm(s->name) + "\" variable because the value is \"null\".");
    } else if (rt.typed()) {
        if (rt.type == Type::Array) emit(Op::MakeArray, 0, 1);
        else if (rt.type == Type::Dictionary) emit(Op::MakeDict, 0, 1);
        else emit_const(type_default(rt.type));
        st = SType(rt.type);
    } else {
        emit(Op::PushNil, 0, 1);
    }
    fs_->cur_line = s->line;
    if (rt.typed()) emit_convert(rt, kConvAssign, 0, st, s->line, s->col, "variable \"" + nm(s->name) + "\"");
    if (s->infer && !rt.present && st != kUnknown && st != SType(Type::Nil) && st != SType(Type::Object)) {
        rt.present = true;
        rt.type = Type(st);
    }
    Local& l = declare_local(s->name, s->line, s->col);
    l.rt = rt;
    l.stype = rt.typed() ? SType(rt.type) : kUnknown;
    l.warn_unused = true;
    emit(Op::StoreLocal, l.slot, -1);
}

void Compiler::compile_local_const(const Stmt* s) {
    Value v;
    if (!const_eval(s->a, v, true)) {
        error(s->line, s->col, "Assigned value for constant \"" + nm(s->name) + "\" isn't a constant expression.");
        v = Value();
    }
    Local& l = declare_local(s->name, s->line, s->col, false); // constants do not use a slot
    l.cval = v;
    l.stype = stype_of(v);
    l.warn_unused = true;
}

static SType numeric_result(SType a, SType b) {
    if (a == SType(Type::Int) && b == SType(Type::Int)) return SType(Type::Int);
    bool an = a == SType(Type::Int) || a == SType(Type::Float);
    bool bn = b == SType(Type::Int) || b == SType(Type::Float);
    if (an && bn) return SType(Type::Float);
    return kUnknown;
}

static SType binary_result(BinOp op, SType ta, SType tb) {
    switch (op) {
    case BinOp::Eq:
    case BinOp::Ne:
    case BinOp::Lt:
    case BinOp::Le:
    case BinOp::Gt:
    case BinOp::Ge:
    case BinOp::In:
    case BinOp::NotIn: return SType(Type::Bool);
    case BinOp::Shl:
    case BinOp::Shr:
    case BinOp::BitAnd:
    case BinOp::BitOr:
    case BinOp::BitXor: return (ta == SType(Type::Int) && tb == SType(Type::Int)) ? SType(Type::Int) : kUnknown;
    case BinOp::Mod:
        if (ta == SType(Type::String)) return SType(Type::String);
        return numeric_result(ta, tb);
    case BinOp::Add:
        if (ta == SType(Type::String) && tb == SType(Type::String)) return SType(Type::String);
        if (ta == SType(Type::Array) && tb == SType(Type::Array)) return SType(Type::Array);
        [[fallthrough]];
    case BinOp::Sub:
    case BinOp::Mul:
    case BinOp::Div:
    case BinOp::Pow: {
        bool va = ta == SType(Type::Vector3) || ta == SType(Type::Vector2) || ta == SType(Type::Color);
        bool vb = tb == SType(Type::Vector3) || tb == SType(Type::Vector2) || tb == SType(Type::Color);
        if (va && (tb == ta || numeric_result(tb, tb) != kUnknown)) return ta;
        if (vb && numeric_result(ta, ta) != kUnknown && op == BinOp::Mul) return tb;
        return numeric_result(ta, tb);
    }
    case BinOp::Count: break;
    }
    return kUnknown;
}

// Assignment targets known at compile time: locals, member variables, self properties and constants.
// Returns false when `e` has to be evaluated at runtime.
bool Compiler::lvalue_static(Node* e, std::vector<WB>& wbs, SType& type) {
    uint32_t name = 0;
    bool self_member = false;
    type = kUnknown;
    if (e->kind == NK::Ident) {
        name = e->name;
    } else if (e->kind == NK::Member && e->a->kind == NK::Self) {
        name = e->name;
        self_member = true;
    } else if (e->kind == NK::Member && e->a->kind == NK::Ident) {
        Type t;
        bool is_class = false;
        auto it = syms_.find(e->a->name);
        if (!find_local(e->a->name) && ((it != syms_.end() && it->second.kind == CS::Enum) ||
                                        (it == syms_.end() && names_type(e->a->name, t, is_class)))) {
            error(e->line, e->col, "Cannot assign a new value to a constant.");
            emit(Op::PushNil, 0, 1);
            wbs.push_back(WB{Op::Pop, 0, -1});
            return true;
        }
        return false;
    } else {
        return false;
    }
    if (!self_member) {
        if (const Local* l = find_local_mut(name)) {
            if (l->is_const) {
                error(e->line, e->col, "Cannot assign a new value to a constant.");
                emit(Op::PushNil, 0, 1);
                wbs.push_back(WB{Op::Pop, 0, -1});
                return true;
            }
            emit(Op::LoadLocal, l->slot, 1);
            wbs.push_back(WB{Op::StoreLocal, l->slot, -1});
            type = l->stype;
            return true;
        }
    }
    auto it = syms_.find(name);
    if (it != syms_.end()) {
        if (it->second.kind == CS::Var) {
            emit(Op::LoadMember, it->second.index, 1);
            wbs.push_back(WB{Op::StoreMember, it->second.index, -1});
            type = var_stype_[it->second.index];
            return true;
        }
        error(e->line, e->col, "Cannot assign a new value to a constant.");
        emit(Op::PushNil, 0, 1);
        wbs.push_back(WB{Op::Pop, 0, -1});
        return true;
    }
    if (const PropertyInfo* p = find_prop(nm(name))) {
        uint32_t ne = name_entry(p->name);
        if (sd_->names[ne].pid >= 0) {
            emit(Op::LoadSelfProp, ne, 1);
            wbs.push_back(WB{p->read_only ? Op::Pop : Op::WbSelfProp, ne, -1});
            type = p->type == Type::Nil ? kUnknown : SType(p->type);
            return true;
        }
    }
    if (self_member) {
        emit(Op::PushSelf, 0, 1);
        emit(Op::Dup, 0, 1);
        emit(Op::GetNamed, name_entry(nm(name)), 0);
        wbs.push_back(WB{Op::WbNamed, name_entry(nm(name)), -1});
        wbs.insert(wbs.end() - 1, WB{Op::Pop, 0, -1});
        return true;
    }
    return false;
}

// Pushes the value the target modifies, leaving its containers on the stack, and records the write-back steps.
// Returns the static type.
SType Compiler::push_lvalue_base(Node* e, std::vector<WB>& wbs) {
    fs_->cur_line = e->line;
    SType t = kUnknown;
    if (lvalue_static(e, wbs, t)) return t;
    if (e->kind == NK::Member) {
        SType bt = push_lvalue_base(e->a, wbs);
        fs_->cur_line = e->line;
        uint32_t ne = name_entry(nm(e->name));
        emit(Op::Dup, 0, 1);
        SType mt = emit_get_named(e, bt);
        wbs.push_back(WB{Op::WbNamed, ne, -1});
        return mt;
    }
    if (e->kind == NK::Index) {
        SType bt = push_lvalue_base(e->a, wbs);
        SType it = compile_expr(e->b);
        fs_->cur_line = e->line;
        emit(Op::Dup2, 0, 2);
        emit(Op::GetIndex, 0, -1);
        wbs.push_back(WB{Op::WbIndex, 0, -2});
        return check_index(bt, it, e->line, e->col);
    }
    t = compile_expr(e);
    wbs.push_back(WB{Op::Pop, 0, -1});
    return t;
}

// `x = v` and `x op= v` on locals, member variables and self properties; false if the target isn't a plain name.
bool Compiler::assign_simple(const Stmt* s) {
    Node* t = s->a;
    int op = s->op;
    bool self_only = t->kind == NK::Member && t->a->kind == NK::Self;
    if (t->kind != NK::Ident && !self_only) return false;
    uint32_t name = t->name;
    // the right-hand side, or `target op rhs` for compound assignment
    auto value = [&](Op load, uint32_t arg, SType target) -> SType {
        if (!op) return compile_expr(s->b);
        emit(load, arg, 1);
        SType rhs = compile_expr(s->b);
        fs_->cur_line = s->line;
        emit(Op(int(Op::Add) + op - 1), 0, -1);
        return check_binary(BinOp(op - 1), target, rhs, s->line, s->col);
    };
    if (!self_only) {
        if (const Local* l = find_local_mut(name)) {
            if (l->is_const) {
                error(t->line, t->col, "Cannot assign a new value to a constant.");
                return true;
            }
            uint32_t slot = l->slot;
            RType rt = l->rt;
            SType target = l->stype;
            Value cv;
            if (op == int(BinOp::Add) + 1 && try_const(s->b, cv) && cv.type() == Type::Int && Heap::i(cv) >= INT32_MIN &&
                Heap::i(cv) <= INT32_MAX && (target == kUnknown || target == SType(Type::Int) || target == SType(Type::Float))) {
                emit(Op::AddLocalInt, slot, 0); // `i += 1` fast path
                emit_word(uint32_t(int32_t(Heap::i(cv))));
                return true;
            }
            SType st = value(Op::LoadLocal, slot, target);
            fs_->cur_line = s->line;
            emit_convert(rt, kConvAssign, 0, st, s->line, s->col, std::string());
            emit(Op::StoreLocal, slot, -1);
            return true;
        }
    }
    auto it = syms_.find(name);
    if (it != syms_.end()) {
        if (it->second.kind != CS::Var) {
            error(t->line, t->col, it->second.kind == CS::Func     ? "Cannot assign a new value to a function."
                                   : it->second.kind == CS::Signal ? "Cannot assign a new value to a signal."
                                                                   : "Cannot assign a new value to a constant.");
            return true;
        }
        uint32_t idx = it->second.index;
        SType st = value(Op::LoadMember, idx, var_stype_[idx]);
        fs_->cur_line = s->line;
        emit_convert(var_types_[idx], kConvAssign, 0, st, s->line, s->col, std::string());
        emit(Op::StoreMember, idx, -1);
        return true;
    }
    if (const PropertyInfo* p = find_prop(nm(name))) {
        if (p->read_only) {
            error(t->line, t->col, "Cannot assign a new value to a read-only property \"" + p->name + "\".");
            return true;
        }
        uint32_t ne = name_entry(p->name);
        if (sd_->names[ne].pid >= 0) {
            SType st = value(Op::LoadSelfProp, ne, p->type == Type::Nil ? kUnknown : SType(p->type));
            fs_->cur_line = s->line;
            check_prop_assign(p, st, s->line, s->col);
            emit(Op::StoreSelfProp, ne, -1);
            return true;
        }
    }
    if (self_only) return false; // self.<unknown>: dynamic assignment on self
    Value tmp;
    if (find_method(nm(name)) || find_builtin_func(nm(name)) >= 0)
        error(t->line, t->col, "Cannot assign a new value to a function.");
    else if (builtin_global_constant(nm(name), tmp))
        error(t->line, t->col, "Cannot assign a new value to a constant.");
    else
        error(t->line, t->col, "Identifier \"" + nm(name) + "\" not declared in the current scope.");
    return true;
}

void Compiler::compile_assign(const Stmt* s) {
    Node* t = s->a;
    int op = s->op; // 0 '=', else BinOp + 1
    if (!t || t->kind == NK::Bad) return;
    if (assign_simple(s)) return;
    std::vector<WB> wbs;
    if (t->kind == NK::Member) {
        uint32_t ne = name_entry(nm(t->name));
        SType bt = kUnknown;
        if (t->a->kind == NK::Self) {
            emit(Op::PushSelf, 0, 1);
            wbs.push_back(WB{Op::Pop, 0, -1});
            bt = SType(Type::Object);
        } else {
            bt = push_lvalue_base(t->a, wbs);
        }
        fs_->cur_line = s->line;
        SType current = kUnknown;
        if (op) {
            emit(Op::Dup, 0, 1);
            current = emit_get_named(t, bt);
        }
        SType vt = compile_expr(s->b);
        fs_->cur_line = s->line;
        if (op) emit(Op(int(Op::Add) + op - 1), 0, -1);
        check_field_set(t, bt, op ? check_binary(BinOp(op - 1), current, vt, s->line, s->col) : vt, op, s->line, s->col);
        emit(Op::SetNamed, ne, -1);
    } else if (t->kind == NK::Index) {
        SType bt = push_lvalue_base(t->a, wbs);
        SType it = compile_expr(t->b);
        fs_->cur_line = s->line;
        SType current = check_index(bt, it, s->line, s->col);
        if (op) {
            emit(Op::Dup2, 0, 2);
            emit(Op::GetIndex, 0, -1);
            SType vt = compile_expr(s->b);
            fs_->cur_line = s->line;
            emit(Op(int(Op::Add) + op - 1), 0, -1);
            check_binary(BinOp(op - 1), current, vt, s->line, s->col);
        } else {
            compile_expr(s->b);
        }
        fs_->cur_line = s->line;
        emit(Op::SetIndex, 0, -2);
    } else {
        error(t->line, t->col, "Cannot assign to this expression.");
        return;
    }
    for (size_t i = wbs.size(); i-- > 0;) emit(wbs[i].op, wbs[i].arg, wbs[i].delta);
}

// Only fully known types are checked; unknown (Variant) values always stay dynamic.
static bool known(SType t) { return t != kUnknown; }

SType Compiler::check_binary(BinOp op, SType a, SType b, int line, int col) {
    if (!known(a) || !known(b)) return binary_result(op, a, b);
    if (op == BinOp::Mod && a == SType(Type::String)) return SType(Type::String); // formatting: checked at runtime
    if (op == BinOp::Div && a == SType(Type::Int) && b == SType(Type::Int))
        warning(line, col, "Integer division. Decimal part will be discarded.");
    Value x = sample_value(Type(a)), y = sample_value(Type(b)), r;
    std::string err;
    if (!binary_op(op, x, y, r, err)) {
        error(line, col, std::string("Invalid operands \"") + type_name(Type(a)) + "\" and \"" + type_name(Type(b)) +
                             "\" for \"" + binop_name(op) + "\" operator.");
        return kUnknown;
    }
    return stype_of(r);
}

SType Compiler::check_unary(UnOp op, SType a, int line, int col) {
    if (!known(a)) return op == UnOp::BitNot ? SType(Type::Int) : kUnknown;
    Value x = sample_value(Type(a)), r;
    std::string err;
    if (!unary_op(op, x, r, err)) {
        error(line, col, std::string("Invalid operand of type \"") + type_name(Type(a)) + "\" for unary operator \"" +
                             unop_name(op) + "\".");
        return kUnknown;
    }
    return stype_of(r);
}

SType Compiler::check_index(SType base, SType index, int line, int col) {
    if (!known(base)) return kUnknown;
    Type b = Type(base);
    switch (b) {
    case Type::Array:
    case Type::Dictionary:
    case Type::Object: return kUnknown;
    case Type::String:
    case Type::Vector3:
    case Type::Vector2:
    case Type::Color:
        if (known(index) && index != SType(Type::Int) && index != SType(Type::String)) {
            error(line, col, std::string("Invalid index type \"") + type_name(Type(index)) + "\" for a base of type \"" +
                                 type_name(b) + "\".");
            return kUnknown;
        }
        if (b == Type::String) return index == SType(Type::Int) ? SType(Type::String) : kUnknown;
        return SType(Type::Float);
    default:
        error(line, col, std::string("Cannot use subscript operator on a base of type \"") + type_name(b) + "\".");
        return kUnknown;
    }
}

SType Compiler::check_method(Node* n, SType receiver) {
    if (!known(receiver) || receiver == SType(Type::Object)) return kUnknown;
    const std::string& name = nm(n->name);
    Type t = Type(receiver);
    if (t == Type::Nil) {
        error(n->line, n->col, "Cannot call method \"" + name + "()\" on a null value.");
        return kUnknown;
    }
    if (t == Type::Callable || t == Type::Signal) {
        int lo = 0, hi = 0;
        SType ret = kUnknown;
        if (!signal_method_info(find_signal_method(name), t, lo, hi, ret)) {
            error(n->line, n->col, "Function \"" + name + "()\" not found in base " + type_name(t) + ".");
            return kUnknown;
        }
        const int argc = int(n->list.size());
        if (argc < lo || (hi >= 0 && argc > hi))
            error(n->line, n->col, std::string("Invalid call to function \"") + name + "()\" in base " + type_name(t) + ". Expected " +
                                       format_int(argc < lo ? lo : hi) + " arguments.");
        return ret;
    }
    int id = find_builtin_method(name);
    if (!builtin_method_exists(t, id)) {
        error(n->line, n->col, "Function \"" + name + "()\" not found in base " + type_name(t) + ".");
        return kUnknown;
    }
    return builtin_method_return(t, id);
}

SType Compiler::check_field_set(Node* target, SType base, SType value, int op, int line, int col) {
    (void)op;
    if (!known(base)) return kUnknown;
    Type b = Type(base);
    if (b == Type::Vector3 || b == Type::Vector2 || b == Type::Color) {
        int f = find_builtin_field(nm(target->name));
        bool ok = b == Type::Vector3 ? (f >= 0 && f <= 2) : b == Type::Vector2 ? (f >= 0 && f <= 1) : f >= 3;
        if (!ok) return kUnknown; // reported by emit_get_named / runtime
        if (known(value) && value != SType(Type::Int) && value != SType(Type::Float))
            error(line, col, std::string("Cannot assign a value of type \"") + type_name(Type(value)) + "\" as \"float\".");
        return SType(Type::Float);
    }
    if (b != Type::Object && b != Type::Dictionary)
        error(target->line, target->col, "Cannot find member \"" + nm(target->name) + "\" in base \"" + type_name(b) + "\".");
    return kUnknown;
}

void Compiler::check_prop_assign(const PropertyInfo* p, SType value, int line, int col) {
    if (!known(value) || p->type == Type::Nil) return;
    Type v = Type(value), t = p->type;
    if (v == t) return;
    bool ok;
    switch (t) {
    case Type::Int:
    case Type::Float:
    case Type::Bool: ok = v == Type::Int || v == Type::Float || v == Type::Bool; break;
    case Type::Object: ok = v == Type::Nil; break;
    case Type::Vector2: ok = v == Type::Vector3; break; // x and y taken, for scripts written before Vector2
    default: ok = false; break;
    }
    if (!ok)
        error(line, col, std::string("Cannot assign a value of type \"") + type_name(v) + "\" as \"" + type_name(t) + "\".");
}

void Compiler::check_call_args(Node* n, uint32_t fi, const std::vector<SType>* types) {
    const std::vector<RType>& params = func_params_[fi];
    for (size_t i = 0; i < types->size() && i < params.size(); ++i) {
        const RType& rt = params[i];
        SType st = (*types)[i];
        if (!rt.typed() || !known(st) || static_compatible(st, rt)) continue;
        std::string tn = rt.type == Type::Object ? rt.cls : std::string(type_name(rt.type));
        error(n->list[i]->line, n->list[i]->col, "Invalid argument for \"" + nm(n->name) + "()\" function: argument " +
                                                    format_int(Int(i + 1)) + " should be \"" + tn + "\" but is \"" +
                                                    type_name(Type(st)) + "\".");
    }
}

int Compiler::const_truth(Node* n) {
    Value cv;
    if (!try_const(n, cv)) return -1;
    return truthy(cv) ? 1 : 0;
}

// `local < constant` as a fused compare-and-jump.
bool Compiler::lt_local_jump(Node* n, std::vector<size_t>& out) {
    if (n->kind != NK::Binary || BinOp(n->op) != BinOp::Lt || n->a->kind != NK::Ident) return false;
    const Local* l = find_local_mut(n->a->name);
    Value k;
    if (!l || l->is_const || !try_const(n->b, k) || !k.is_number()) return false;
    size_t at = pc();
    emit(Op::LtLocalJump, l->slot, 0);
    emit_word(add_const(k));
    emit_word(0);
    out.push_back(at);
    return true;
}

void Compiler::cond_jump(Node* n, bool when, std::vector<size_t>& out) {
    int truth = const_truth(n);
    if (truth >= 0) {
        if ((truth == 1) == when) out.push_back(emit_jump(Op::Jump, 0));
        return;
    }
    fs_->cur_line = n->line;
    if (n->kind == NK::Not) {
        cond_jump(n->a, !when, out);
        return;
    }
    if (n->kind == NK::And || n->kind == NK::Or) {
        if ((n->kind == NK::And) != when) {
            // and/false or or/true: either operand decides
            cond_jump(n->a, when, out);
            cond_jump(n->b, when, out);
        } else {
            std::vector<size_t> skip;
            cond_jump(n->a, !when, skip);
            cond_jump(n->b, when, out);
            patch_all(skip);
        }
        return;
    }
    if (!when && lt_local_jump(n, out)) return;
    compile_expr(n);
    out.push_back(emit_jump(when ? Op::JumpIfTrue : Op::JumpIfFalse, -1));
}

void Compiler::compile_if(const Stmt* s) {
    std::vector<size_t> ends;
    std::vector<size_t> next;
    cond_jump(s->a, false, next);
    compile_block(s->body);
    if (!s->elifs.empty() || !s->orelse.empty()) ends.push_back(emit_jump(Op::Jump, 0));
    for (size_t i = 0; i < s->elifs.size(); ++i) {
        patch_all(next);
        const Stmt* e = s->elifs[i];
        fs_->cur_line = e->line;
        fs_->depth = 0;
        cond_jump(e->a, false, next);
        compile_block(e->body);
        if (i + 1 < s->elifs.size() || !s->orelse.empty()) ends.push_back(emit_jump(Op::Jump, 0));
    }
    patch_all(next);
    if (!s->orelse.empty()) compile_block(s->orelse);
    patch_all(ends);
}

void Compiler::compile_while(const Stmt* s) {
    size_t top = pc();
    std::vector<size_t> exits;
    cond_jump(s->a, false, exits);
    fs_->loops.push_back(LoopCtx{top, {}});
    compile_block(s->body);
    fs_->cur_line = s->line;
    emit_jump_back(top);
    patch_all(exits);
    patch_all(fs_->loops.back().breaks);
    fs_->loops.pop_back();
}

void Compiler::for_begin(const Stmt* s, size_t& top) {
    push_scope();
    RType rt = resolve_type(s->type);
    Node* it = s->a;
    bool range = false;
    uint32_t argc = 0;
    if (it && it->kind == NK::Call && nm(it->name) == "range" && is_builtin_call(it->name) && !it->list.empty() &&
        it->list.size() <= 3) {
        range = true;
        argc = uint32_t(it->list.size());
        for (Node* a : it->list) compile_expr(a);
    } else {
        SType st = compile_expr(it);
        if (st == SType(Type::Int)) {
            range = true;
            argc = 1;
        } else if (st != kUnknown && st != SType(Type::Array) && st != SType(Type::Dictionary) && st != SType(Type::String)) {
            error(it->line, it->col, std::string("Unable to iterate on value of type \"") + type_name(Type(st)) + "\".");
        }
    }
    fs_->cur_line = s->line;
    uint32_t state;
    if (range) {
        state = alloc_hidden(3);
        emit(Op::RangePrep, state | (argc << 16), -int(argc));
    } else {
        state = alloc_hidden(2);
        emit(Op::ForPrep, state, -1);
    }
    Local& var = declare_local(s->name, s->line, s->col);
    var.rt = rt;
    var.stype = rt.typed() ? SType(rt.type) : (range ? SType(Type::Int) : kUnknown);
    uint32_t var_slot = var.slot;
    top = pc();
    emit(range ? Op::RangeNext : Op::ForNext, state, 0);
    emit_word(var_slot);
    emit_word(0); // exit offset, patched by for_end
    if (rt.typed()) {
        emit(Op::LoadLocal, var_slot, 1);
        emit_convert(rt, kConvAssign, 0, range ? SType(Type::Int) : kUnknown, s->line, s->col,
                     "variable \"" + nm(s->name) + "\"");
        emit(Op::StoreLocal, var_slot, -1);
    }
    fs_->loops.push_back(LoopCtx{top, {}});
}

void Compiler::for_end(const Stmt* s, size_t top) {
    fs_->cur_line = s->line;
    emit_jump_back(top);
    fs_->fd->code[top + 2] = uint32_t(int32_t(pc()) - int32_t(top));
    patch_all(fs_->loops.back().breaks);
    fs_->loops.pop_back();
    pop_scope();
}

void Compiler::compile_for(const Stmt* s) {
    size_t top = 0;
    for_begin(s, top);
    compile_block(s->body);
    for_end(s, top);
}

// Emits the tests of one match branch and returns the jump to the next one, or SIZE_MAX if a pattern always matches.
// Bindings go into the current scope.
size_t Compiler::match_patterns(const MatchBranch& br, uint32_t tmp, bool& unconditional) {
    std::vector<size_t> to_body;
    unconditional = false;
    for (Node* p : br.patterns) {
        if (!p) continue;
        fs_->cur_line = p->line;
        fs_->depth = 0;
        if (p->kind == NK::Wildcard) {
            to_body.push_back(emit_jump(Op::Jump, 0));
            unconditional = true;
            break;
        }
        if (p->kind == NK::Binding) {
            if (br.patterns.size() > 1) error(p->line, p->col, "Cannot use a variable bind with multiple patterns.");
            Local& l = declare_local(p->name, p->line, p->col);
            emit(Op::LoadLocal, tmp, 1);
            emit(Op::StoreLocal, l.slot, -1);
            to_body.push_back(emit_jump(Op::Jump, 0));
            unconditional = true;
            break;
        }
        emit(Op::LoadLocal, tmp, 1);
        compile_expr(p);
        fs_->cur_line = p->line;
        emit(Op::MatchEq, 0, -1);
        to_body.push_back(emit_jump(Op::JumpIfTrue, -1));
    }
    size_t skip = SIZE_MAX;
    if (!unconditional) skip = emit_jump(Op::Jump, 0);
    patch_all(to_body);
    return skip;
}

void Compiler::compile_match(const Stmt* s) {
    push_scope();
    compile_expr(s->a);
    uint32_t tmp = alloc_hidden(1);
    emit(Op::StoreLocal, tmp, -1);
    std::vector<size_t> ends;
    for (const MatchBranch& br : s->branches) {
        push_scope();
        bool unconditional = false;
        size_t skip = match_patterns(br, tmp, unconditional);
        compile_block(br.body);
        ends.push_back(emit_jump(Op::Jump, 0));
        if (skip != SIZE_MAX) patch(skip);
        pop_scope();
    }
    patch_all(ends);
    pop_scope();
}

void Compiler::compile_return(const Stmt* s) {
    if (!s->a) {
        if (fs_->ret.typed())
            error(s->line, s->col, "A function with return type \"" +
                                       std::string(fs_->ret.type == Type::Object ? fs_->ret.cls : type_name(fs_->ret.type)) +
                                       "\" must return a value.");
        emit(Op::ReturnNil, 0, 0);
        return;
    }
    if (fs_->ret.is_void) {
        error(s->line, s->col, "A void function cannot return a value.");
        return;
    }
    SType st = compile_expr(s->a);
    fs_->cur_line = s->line;
    emit_convert(fs_->ret, kConvReturn, 0, st, s->line, s->col, "return value");
    emit(Op::Return, 0, -1);
}

bool Compiler::emit_folded(Node* n, SType& type) {
    Value cv;
    if (!try_const(n, cv)) return false;
    emit_const(cv);
    type = stype_of(cv);
    return true;
}

SType Compiler::emit_literal(Node* n) {
    emit_const(n->value);
    return stype_of(n->value);
}

SType Compiler::emit_nil_error(Node* n) {
    if (n && (n->kind == NK::Wildcard || n->kind == NK::Binding))
        error(n->line, n->col, "Patterns can only be used in \"match\" branches.");
    emit(Op::PushNil, 0, 1);
    return kUnknown;
}

SType Compiler::compile_expr(Node* n) {
    if (!n || n->kind == NK::Bad) return emit_nil_error(n);
    fs_->cur_line = n->line;
    if (n->kind != NK::Literal && n->kind != NK::Ident && n->cstate != 2) {
        SType t = kUnknown;
        if (emit_folded(n, t)) return t;
    }
    switch (n->kind) {
    case NK::Literal: return emit_literal(n);
    case NK::Ident: return compile_ident(n);
    case NK::Self: emit(Op::PushSelf, 0, 1); return SType(Type::Object);
    case NK::Unary: {
        SType t = compile_expr(n->a);
        fs_->cur_line = n->line;
        UnOp op = UnOp(n->op);
        emit(op == UnOp::Neg ? Op::Neg : (op == UnOp::Pos ? Op::Pos : Op::BitNot), 0, 0);
        return check_unary(op, t, n->line, n->col);
    }
    case NK::Not:
        compile_expr(n->a);
        emit(Op::Not, 0, 0);
        return SType(Type::Bool);
    case NK::Binary: {
        SType ta = compile_expr(n->a);
        SType tb = compile_expr(n->b);
        fs_->cur_line = n->line;
        emit(Op(int(Op::Add) + n->op), 0, -1);
        return check_binary(BinOp(n->op), ta, tb, n->line, n->col);
    }
    case NK::And:
    case NK::Or: return compile_logic(n);
    case NK::Ternary: return compile_ternary(n);
    case NK::Is:
    case NK::As:
        compile_expr(n->a);
        return emit_is_as(n);
    case NK::Member: return compile_member(n);
    case NK::Index: {
        SType bt = compile_expr(n->a);
        SType it = compile_expr(n->b);
        fs_->cur_line = n->line;
        emit(Op::GetIndex, 0, -1);
        return check_index(bt, it, n->line, n->col);
    }
    case NK::Call: return compile_call(n);
    case NK::MethodCall: return compile_method_call(n);
    case NK::ArrayLit:
        for (Node* x : n->list) compile_expr(x);
        fs_->cur_line = n->line;
        emit(Op::MakeArray, uint32_t(n->list.size()), 1 - int(n->list.size()));
        return SType(Type::Array);
    case NK::DictLit:
        for (Node* x : n->list) compile_expr(x);
        fs_->cur_line = n->line;
        emit(Op::MakeDict, uint32_t(n->list.size() / 2), 1 - int(n->list.size()));
        return SType(Type::Dictionary);
    case NK::GetNode: return emit_get_node(n);
    default: return emit_nil_error(n);
    }
}

SType Compiler::compile_logic(Node* n) {
    bool is_and = n->kind == NK::And;
    int base = fs_->depth;
    compile_expr(n->a);
    size_t j1 = emit_jump(is_and ? Op::JumpIfFalse : Op::JumpIfTrue, -1);
    compile_expr(n->b);
    emit(Op::ToBool, 0, 0);
    size_t j2 = emit_jump(Op::Jump, 0);
    patch(j1);
    fs_->depth = base;
    emit(is_and ? Op::PushFalse : Op::PushTrue, 0, 1);
    patch(j2);
    return SType(Type::Bool);
}

SType Compiler::compile_ternary(Node* n) {
    int base = fs_->depth;
    std::vector<size_t> to_else;
    cond_jump(n->b, false, to_else);
    SType ta = compile_expr(n->a);
    size_t end = emit_jump(Op::Jump, 0);
    patch_all(to_else);
    fs_->depth = base;
    SType tb = compile_expr(n->c);
    patch(end);
    return ta == tb ? ta : kUnknown;
}

SType Compiler::emit_get_node(Node* n) {
    if (get_node_mid_ < 0 || !find_method("get_node")) {
        error(n->line, n->col, "\"$\" requires the base class to have a get_node() method.");
        emit(Op::PushNil, 0, 1);
        return kUnknown;
    }
    emit_const(n->value);
    emit(Op::CallSelf, 1, 0);
    emit_word(name_entry("get_node"));
    return SType(Type::Object);
}

SType Compiler::compile_ident(Node* n) {
    uint32_t name = n->name;
    if (const Local* l = find_local_mut(name)) {
        if (l->is_const) {
            emit_const(l->cval);
            return stype_of(l->cval);
        }
        emit(Op::LoadLocal, l->slot, 1);
        return l->stype;
    }
    auto it = syms_.find(name);
    if (it != syms_.end()) {
        switch (it->second.kind) {
        case CS::Var:
            emit(Op::LoadMember, it->second.index, 1);
            return var_stype_[it->second.index];
        case CS::Const: {
            Value v;
            if (!resolve_const(it->second.index, v)) {
                emit(Op::PushNil, 0, 1);
                return kUnknown;
            }
            emit_const(v);
            return stype_of(v);
        }
        case CS::Enum:
            emit_const(enums_[it->second.index].dict);
            return SType(Type::Dictionary);
        case CS::Func:
            emit_bound(nm(name), false);
            return SType(Type::Callable);
        case CS::Signal:
            emit_bound(nm(name), true);
            return SType(Type::Signal);
        }
    }
    const std::string& s = nm(name);
    if (const PropertyInfo* p = find_prop(s)) {
        uint32_t ne = name_entry(p->name);
        if (sd_->names[ne].pid >= 0) {
            emit(Op::LoadSelfProp, ne, 1);
        } else {
            emit(Op::PushSelf, 0, 1);
            emit(Op::GetNamed, ne, 0);
        }
        return p->type == Type::Nil ? kUnknown : SType(p->type);
    }
    if (find_method(s)) {
        emit_bound(s, false);
        return SType(Type::Callable);
    }
    if (find_signal(s)) {
        emit_bound(s, true);
        return SType(Type::Signal);
    }
    Value cv;
    if (builtin_global_constant(s, cv)) {
        emit_const(cv);
        return stype_of(cv);
    }
    if (host_.singleton(s) != 0) {
        emit(Op::LoadSingleton, name_entry(s), 1);
        return SType(Type::Object);
    }
    Type t;
    bool variant = false;
    if (builtin_type_from_name(s, t, variant) || class_exists(s)) {
        error(n->line, n->col, "\"" + s + "\" is a type and can't be used as a value.");
        emit(Op::PushNil, 0, 1);
        return kUnknown;
    }
    if (find_builtin_func(s) >= 0) {
        error(n->line, n->col, "Built-in functions can't be used as Callables yet in JetScript; call \"" + s + "()\" instead.");
        emit(Op::PushNil, 0, 1);
        return kUnknown;
    }
    error(n->line, n->col, "Identifier \"" + s + "\" not declared in the current scope.");
    emit(Op::PushNil, 0, 1);
    return kUnknown;
}

// Self members/properties, enum values and builtin type constants don't need the base evaluated.
// Returns true when it emitted code.
bool Compiler::member_static(Node* n, SType& type) {
    Node* b = n->a;
    const std::string& mname = nm(n->name);
    if (b->kind == NK::Self) {
        auto it = syms_.find(n->name);
        if (it != syms_.end()) {
            if (it->second.kind == CS::Var) {
                emit(Op::LoadMember, it->second.index, 1);
                type = var_stype_[it->second.index];
                return true;
            }
            Node tmp;
            tmp.kind = NK::Ident;
            tmp.name = n->name;
            tmp.line = n->line;
            tmp.col = n->col;
            type = compile_ident(&tmp);
            return true;
        }
        if (const PropertyInfo* p = find_prop(mname)) {
            uint32_t ne = name_entry(p->name);
            if (sd_->names[ne].pid >= 0) {
                emit(Op::LoadSelfProp, ne, 1);
                type = p->type == Type::Nil ? kUnknown : SType(p->type);
                return true;
            }
        }
        if (find_method(mname) || find_signal(mname)) {
            const bool signal = !find_method(mname);
            emit_bound(mname, signal);
            type = SType(signal ? Type::Signal : Type::Callable);
            return true;
        }
        return false;
    }
    if (b->kind != NK::Ident || find_local(b->name)) return false;
    auto it = syms_.find(b->name);
    if (it != syms_.end() && it->second.kind == CS::Enum) {
        for (const auto& kv : enums_[it->second.index].values)
            if (kv.first == n->name) {
                emit_const(Value(kv.second));
                type = SType(Type::Int);
                return true;
            }
        error(n->line, n->col, "Cannot find member \"" + mname + "\" in base \"" + nm(b->name) + "\".");
        emit(Op::PushNil, 0, 1);
        type = kUnknown;
        return true;
    }
    Type t;
    bool is_class = false;
    if (it == syms_.end() && names_type(b->name, t, is_class)) {
        Value cv;
        type = kUnknown;
        if ((!is_class && builtin_type_constant(t, mname, cv)) || (is_class && class_constant(nm(b->name), mname, cv))) {
            emit_const(cv);
            type = stype_of(cv);
            return true;
        }
        if (is_class)
            error(n->line, n->col, "Static access on engine classes (\"" + nm(b->name) + "." + mname +
                                       "\") is not supported yet in JetScript.");
        else
            error(n->line, n->col, "Cannot find member \"" + mname + "\" in base \"" + nm(b->name) + "\".");
        emit(Op::PushNil, 0, 1);
        return true;
    }
    return false;
}

SType Compiler::emit_get_named(Node* n, SType bt) {
    fs_->cur_line = n->line;
    const std::string& mname = nm(n->name);
    uint32_t ne = name_entry(mname);
    emit(Op::GetNamed, ne, 0);
    int f = sd_->names[ne].field;
    if ((bt == SType(Type::Vector3) && f >= 0 && f <= 2) || (bt == SType(Type::Vector2) && f >= 0 && f <= 1) ||
        (bt == SType(Type::Color) && f >= 3))
        return SType(Type::Float);
    if (bt != kUnknown && bt != SType(Type::Object) && bt != SType(Type::Dictionary))
        error(n->line, n->col, "Cannot find member \"" + mname + "\" in base \"" + type_name(Type(bt)) + "\".");
    return kUnknown;
}

SType Compiler::compile_member(Node* n) {
    SType t = kUnknown;
    if (member_static(n, t)) return t;
    if (n->a->kind == NK::Self) {
        emit(Op::PushSelf, 0, 1);
        return emit_get_named(n, kUnknown);
    }
    SType bt = compile_expr(n->a);
    return emit_get_named(n, bt);
}

// Looks `name(args)` up as script function, then base class method, then builtin, and fills the instruction words.
// Returns false after reporting an error.
bool Compiler::plan_call(Node* n, uint32_t& word0, uint32_t& word1, SType& ret, uint8_t& retmode) {
    const std::string& name = nm(n->name);
    int argc = int(n->list.size());
    ret = kUnknown;
    retmode = 0;
    auto arity = [&](int lo, int hi) {
        if (argc < lo) {
            error(n->line, n->col, "Too few arguments for \"" + name + "()\" call. Expected at least " + format_int(lo) +
                                       " but received " + format_int(argc) + ".");
            return false;
        }
        if (hi >= 0 && argc > hi) {
            error(n->line, n->col, "Too many arguments for \"" + name + "()\" call. Expected at most " + format_int(hi) +
                                       " but received " + format_int(argc) + ".");
            return false;
        }
        return true;
    };
    if (find_local(n->name)) {
        error(n->line, n->col, "\"" + name + "\" is a variable and can't be called; a Callable is called with \"" + name + ".call()\".");
        return false;
    }
    auto it = syms_.find(n->name);
    if (it != syms_.end()) {
        if (it->second.kind == CS::Signal) {
            error(n->line, n->col, "\"" + name + "\" is a signal: emit it with \"" + name + ".emit()\".");
            return false;
        }
        if (it->second.kind != CS::Func) {
            error(n->line, n->col, "\"" + name + "\" is not a function and can't be called.");
            return false;
        }
        uint32_t fi = it->second.index;
        if (!arity(func_nreq_[fi], int(ast_.funcs[fi].params.size()))) return false;
        word0 = enc(Op::CallScript, uint32_t(argc));
        word1 = fi;
        const RType& r = func_ret_[fi];
        ret = r.typed() ? SType(r.type) : kUnknown;
        return true;
    }
    // connect(), emit_signal()... run by the VM on any object
    int lo = 0, hi = 0;
    SType sret = kUnknown;
    if (signal_method_info(find_signal_method(name), Type::Object, lo, hi, sret)) {
        if (!arity(lo, hi)) return false;
        word0 = enc(Op::CallSelf, uint32_t(argc));
        word1 = name_entry(name);
        ret = sret;
        return true;
    }
    if (const MethodInfo* m = find_method(name)) {
        if (!arity(m->min_args, m->max_args)) return false;
        uint32_t ne = name_entry(name);
        if (sd_->names[ne].mid < 0) {
            error(n->line, n->col, "Function \"" + name + "()\" is not available from the engine.");
            return false;
        }
        word0 = enc(Op::CallSelf, uint32_t(argc));
        word1 = ne;
        ret = m->return_type == Type::Nil ? kUnknown : SType(m->return_type);
        return true;
    }
    int id = name == "String" ? find_builtin_func("str") : find_builtin_func(name);
    if (id >= 0) {
        const BuiltinFunc& bf = builtin_funcs()[id];
        if (name == "String" ? !arity(1, 1) : !arity(bf.min_args, bf.max_args)) return false;
        if (name == "Vector3" && argc != 0 && argc != 1 && argc != 3) {
            error(n->line, n->col, "No constructor of \"Vector3\" matches the given arguments (expected (), (Vector3) or (x, y, z)).");
            return false;
        }
        word0 = enc(Op::CallBuiltin, uint32_t(argc) | (uint32_t(id) << 8));
        word1 = UINT32_MAX;
        ret = bf.ret;
        retmode = (bf.flags & kRetFirst) ? (name == "lerp" ? 3 : 1) : ((bf.flags & kRetNumeric) ? 2 : 0);
        return true;
    }
    if (class_exists(name)) {
        error(n->line, n->col, "Creating engine objects (\"" + name + "\") is not supported yet in JetScript.");
        return false;
    }
    error(n->line, n->col, "Function \"" + name + "()\" not found in base self.");
    return false;
}

SType Compiler::compile_call(Node* n) {
    uint32_t w0 = 0, w1 = 0;
    SType ret = kUnknown;
    uint8_t retmode = 0;
    if (!plan_call(n, w0, w1, ret, retmode)) {
        emit(Op::PushNil, 0, 1);
        return kUnknown;
    }
    SType first = kUnknown, numeric = SType(Type::Int);
    std::vector<SType>* types = nullptr; // argument types for typed script function parameters
    if (op_of(w0) == Op::CallScript && !func_params_[w1].empty()) types = new std::vector<SType>();
    for (size_t i = 0; i < n->list.size(); ++i) {
        SType t = compile_expr(n->list[i]);
        if (types) types->push_back(t);
        if (i == 0) first = t;
        if (t == SType(Type::Float) && numeric != kUnknown) numeric = SType(Type::Float);
        else if (t != SType(Type::Int) && t != SType(Type::Float)) numeric = kUnknown;
    }
    if (types) {
        check_call_args(n, w1, types);
        delete types;
    }
    fs_->cur_line = n->line;
    int argc = int(n->list.size());
    emit(op_of(w0), arg_of(w0), 1 - argc);
    if (w1 != UINT32_MAX) emit_word(w1);
    switch (retmode) {
    case 1: return first;
    case 2: return numeric;
    case 3: return first == SType(Type::Int) ? SType(Type::Float) : first;
    default: return ret;
    }
}

// 0: dynamic call, 1: call on self resolved statically, 2: error emitted
int Compiler::method_static(Node* n) {
    Node* recv = n->a;
    const std::string& name = nm(n->name);
    if (recv->kind == NK::Self) {
        auto it = syms_.find(n->name);
        if ((it != syms_.end() && it->second.kind == CS::Func) || (it == syms_.end() && find_method(name))) return 1;
        return 0;
    }
    if (recv->kind == NK::Ident && !find_local(recv->name) && !syms_.count(recv->name)) {
        Type t;
        bool is_class = false;
        if (names_type(recv->name, t, is_class)) {
            if (is_class)
                error(recv->line, recv->col, "Static calls on engine classes (\"" + nm(recv->name) + "." + name +
                                                 "()\") are not supported yet in JetScript.");
            else
                error(recv->line, recv->col, "Static function \"" + name + "()\" not found in base \"" + nm(recv->name) + "\".");
            emit(Op::PushNil, 0, 1);
            return 2;
        }
    }
    return 0;
}

SType Compiler::compile_method_call(Node* n) {
    int kind = method_static(n);
    if (kind == 2) return kUnknown;
    if (kind == 1) {
        // self.f(...) == f(...): the node is reinterpreted as a plain call
        NK saved = n->kind;
        n->kind = NK::Call;
        SType t = compile_call(n);
        n->kind = saved;
        return t;
    }
    SType rt = compile_expr(n->a);
    for (Node* a : n->list) compile_expr(a);
    fs_->cur_line = n->line;
    int argc = int(n->list.size());
    emit(Op::CallNamed, uint32_t(argc), -argc);
    emit_word(name_entry(nm(n->name)));
    return check_method(n, rt);
}

SType Compiler::emit_is_as(Node* n) {
    fs_->cur_line = n->line;
    const std::string& tn = nm(n->type.name);
    bool is_as = n->kind == NK::As;
    Type t;
    bool variant = false;
    SType result = SType(Type::Bool);
    if (!n->type.present) {
        return kUnknown; // reported by the parser
    } else if (builtin_type_from_name(tn, t, variant) && !variant) {
        emit(is_as ? Op::AsType : Op::IsType, uint32_t(t), 0);
        if (is_as) result = SType(t);
    } else if (!variant && class_exists(tn)) {
        emit(is_as ? Op::AsClass : Op::IsClass, name_entry(tn), 0);
        if (is_as) result = SType(Type::Object);
    } else {
        auto it = syms_.find(n->type.name);
        if (it != syms_.end() && it->second.kind == CS::Enum) {
            emit(is_as ? Op::AsType : Op::IsType, uint32_t(Type::Int), 0);
            if (is_as) result = SType(Type::Int);
        } else {
            error(n->type.line, n->type.col, "Could not find type \"" + tn + "\" in the current scope.");
            return kUnknown;
        }
    }
    if (n->negate && !is_as) emit(Op::Not, 0, 0);
    return result;
}

void Compiler::compile_init(bool onready) {
    FunctionData& fd = sd_->funcs[size_t(onready ? sd_->onready_fn : sd_->init_fn)];
    fd.name = onready ? "@implicit_ready" : "@implicit_new";
    begin_function(fd, 1);
    fd.entries.push_back(0);
    push_scope();
    for (size_t i = 0; i < ast_.vars.size(); ++i) {
        const VarDecl& v = ast_.vars[i];
        if (v.onready != onready || !v.init) continue;
        fs_->cur_line = v.line;
        fs_->depth = 0;
        SType st = compile_expr(v.init);
        init_stype_[i] = st;
        fs_->cur_line = v.line;
        RType& rt = var_types_[i];
        if (rt.typed()) emit_convert(rt, kConvAssign, 0, st, v.line, v.col, "variable \"" + nm(v.name) + "\"");
        if (v.infer && !rt.present) {
            if (st == SType(Type::Nil))
                error(v.line, v.col, "Cannot infer the type of \"" + nm(v.name) + "\" variable because the value is \"null\".");
            if (st != kUnknown && st != SType(Type::Nil) && st != SType(Type::Object)) {
                rt.present = true;
                rt.type = Type(st);
            }
        }
        if (var_stype_[i] == kUnknown && rt.typed()) var_stype_[i] = SType(rt.type);
        emit(Op::StoreMember, uint32_t(i), -1);
    }
    emit(Op::ReturnNil, 0, 0);
    pop_scope();
    end_function();
}

void Compiler::compile_function(const FuncDecl& f, FunctionData& fd) {
    begin_function(fd, f.line);
    fd.name = nm(f.name);
    fd.nparams = uint16_t(f.params.size());
    size_t fi = size_t(&f - &ast_.funcs[0]);
    fd.nrequired = func_nreq_[fi];
    fs_->ret = func_ret_[fi];
    push_scope();
    std::vector<RType> ptypes;
    for (size_t k = 0; k < f.params.size(); ++k) {
        const Param& p = f.params[k];
        RType rt = func_params_[fi][k];
        for (const Local& l : fs_->locals)
            if (l.name == p.name) error(p.line, p.col, "Parameter \"" + nm(p.name) + "\" is declared more than once.");
        Local l;
        l.name = p.name;
        l.slot = fs_->next_slot++;
        l.line = p.line;
        l.rt = rt;
        l.stype = rt.typed() ? SType(rt.type) : kUnknown;
        fs_->locals.push_back(l);
        ptypes.push_back(rt);
    }
    fs_->max_slots = fs_->next_slot;
    for (size_t k = fd.nrequired; k < f.params.size(); ++k) {
        fd.entries.push_back(uint32_t(pc()));
        const Param& p = f.params[k];
        fs_->cur_line = p.line;
        fs_->depth = 0;
        SType st = compile_expr(p.def);
        if (p.infer && !ptypes[k].present && st != kUnknown && st != SType(Type::Nil) && st != SType(Type::Object)) {
            ptypes[k].present = true;
            ptypes[k].type = Type(st);
            fs_->locals[k].rt = ptypes[k];
            fs_->locals[k].stype = st;
        }
        emit(Op::StoreLocal, uint32_t(k), -1);
    }
    fd.entries.push_back(uint32_t(pc()));
    for (size_t k = 0; k < f.params.size(); ++k) {
        if (!ptypes[k].typed()) continue;
        fs_->cur_line = f.params[k].line;
        fs_->depth = 0;
        emit(Op::LoadLocal, uint32_t(k), 1);
        emit_convert(ptypes[k], kConvArg, uint32_t(k), kUnknown, f.params[k].line, f.params[k].col,
                     "parameter \"" + nm(f.params[k].name) + "\"");
        emit(Op::StoreLocal, uint32_t(k), -1);
    }
    for (const Stmt* s : f.body) compile_stmt(s);
    fs_->cur_line = f.line;
    emit(Op::ReturnNil, 0, 0);
    pop_scope();
    end_function();
}

void Compiler::build_metadata() {
    for (const ClassItem& item : ast_.order) {
        switch (item.kind) {
        case ItemKind::Var: {
            const VarDecl& v = ast_.vars[item.index];
            const RType& rt = var_types_[item.index];
            MemberInfo mi;
            mi.name = nm(v.name);
            SType hint = var_stype_[item.index] != kUnknown ? var_stype_[item.index] : init_stype_[item.index];
            mi.type = rt.typed() ? rt.type : (hint == kUnknown ? Type::Nil : Type(hint));
            mi.line = v.line;
            mi.exported = v.exported;
            mi.onready = v.onready;
            sd_->members.push_back(mi);
            if (v.exported) {
                ExportInfo ei;
                ei.name = mi.name;
                ei.line = v.line;
                Value init;
                bool constant = v.init && try_const(v.init, init);
                Type t = mi.type;
                if (t == Type::Nil && constant) t = init.type();
                ei.type = t;
                bool exportable = t == Type::Bool || t == Type::Int || t == Type::Float || t == Type::String ||
                                  t == Type::Vector3 || t == Type::Vector2 || t == Type::Color;
                if (!exportable) {
                    if (t == Type::Nil)
                        error(v.line, v.col, "Cannot infer the type of exported variable \"" + mi.name +
                                                 "\": add a type annotation or a constant default value.");
                    else
                        error(v.line, v.col, std::string("Export type \"") +
                                                 (t == Type::Object && !rt.cls.empty() ? rt.cls : std::string(type_name(t))) +
                                                 "\" is not supported (exportable: bool, int, float, String, Vector2, Vector3, Color).");
                }
                if (constant && convert_to(t, init)) ei.default_value = init;
                else ei.default_value = type_default(t);
                if (v.has_range) {
                    if (t != Type::Int && t != Type::Float)
                        error(v.line, v.col, "\"@export_range\" can only be used on int or float variables.");
                    Real vals[3] = {0, 0, t == Type::Int ? 1.0f : 0.001f};
                    for (size_t k = 0; k < v.range_args.size() && k < 3; ++k) {
                        Value a;
                        if (!try_const(v.range_args[k], a) || !a.is_number())
                            error(v.line, v.col, "\"@export_range\" arguments must be constant numbers.");
                        else vals[k] = a.as_real();
                    }
                    ei.has_range = true;
                    ei.min = vals[0];
                    ei.max = vals[1];
                    ei.step = vals[2];
                }
                sd_->exports.push_back(std::move(ei));
            }
            break;
        }
        case ItemKind::Const: {
            const ConstDecl& c = ast_.consts[item.index];
            MemberInfo mi;
            mi.name = nm(c.name);
            Value v;
            if (resolve_const(uint32_t(item.index), v)) mi.type = v.type();
            mi.line = c.line;
            mi.constant = true;
            sd_->members.push_back(mi);
            sd_->consts.emplace_back(mi.name, v);
            break;
        }
        case ItemKind::Enum: {
            const EnumInfo& e = enums_[item.index];
            if (e.name) {
                MemberInfo mi;
                mi.name = nm(e.name);
                mi.type = Type::Dictionary;
                mi.line = e.line;
                mi.constant = true;
                sd_->members.push_back(mi);
                sd_->consts.emplace_back(mi.name, e.dict);
            }
            for (size_t k = 0; k < e.values.size(); ++k) {
                MemberInfo mi;
                mi.name = e.name ? nm(e.name) + "." + nm(e.values[k].first) : nm(e.values[k].first);
                mi.type = Type::Int;
                mi.line = ast_.enums[item.index].positions[k].first;
                mi.constant = true;
                sd_->members.push_back(mi);
                if (!e.name) sd_->consts.emplace_back(mi.name, Value(e.values[k].second));
            }
            break;
        }
        case ItemKind::Func: {
            const FuncDecl& f = ast_.funcs[item.index];
            FunctionInfo fi;
            fi.name = nm(f.name);
            fi.line = f.line;
            for (const Param& p : f.params) fi.arguments.push_back(nm(p.name));
            sd_->functions.push_back(std::move(fi));
            break;
        }
        case ItemKind::Signal: break; // below, in declaration order
        }
    }

    for (size_t i = 0; i < ast_.vars.size(); ++i) {
        VarData vd;
        vd.name = nm(ast_.vars[i].name);
        const RType& rt = var_types_[i];
        vd.typed = rt.typed();
        vd.type = rt.typed() ? rt.type : Type::Nil;
        if (rt.typed() && rt.type == Type::Object && rt.cls != "Object") vd.class_name = rt.cls;
        if (vd.typed && !is_container(type_default(vd.type))) vd.initial = type_default(vd.type);
        sd_->vars.push_back(std::move(vd));
    }

    for (size_t i = 0; i < sd_->vars.size(); ++i) sd_->symbols.push_back(SymEntry{sd_->vars[i].name, SymKind::Var, uint32_t(i)});
    for (size_t i = 0; i < sd_->consts.size(); ++i) sd_->symbols.push_back(SymEntry{sd_->consts[i].first, SymKind::Const, uint32_t(i)});
    for (size_t i = 0; i < ast_.funcs.size(); ++i) sd_->symbols.push_back(SymEntry{nm(ast_.funcs[i].name), SymKind::Func, uint32_t(i)});
    for (size_t i = 0; i < ast_.signals.size(); ++i) {
        SignalInfo info;
        info.name = nm(ast_.signals[i].name);
        for (uint32_t a : ast_.signals[i].params) info.arguments.push_back(nm(a));
        sd_->signals.push_back(std::move(info));
        sd_->symbols.push_back(SymEntry{sd_->signals.back().name, SymKind::Signal, uint32_t(i)});
    }
    std::sort(sd_->symbols.begin(), sd_->symbols.end(), [](const SymEntry& a, const SymEntry& b) { return a.name < b.name; });
}

void Compiler::run(ScriptData& sd) {
    sd_ = &sd;
    setup_base();
    register_symbols();
    resolve_enums();
    for (uint32_t i = 0; i < uint32_t(ast_.consts.size()); ++i) {
        Value v;
        resolve_const(i, v);
    }

    for (const VarDecl& v : ast_.vars) {
        RType rt = resolve_type(v.type);
        if (rt.is_void) error(v.type.line, v.type.col, "\"void\" can't be used as a variable type.");
        var_types_.push_back(rt);
        var_stype_.push_back(rt.typed() ? SType(rt.type) : kUnknown);
        init_stype_.push_back(kUnknown);
    }

    for (const FuncDecl& f : ast_.funcs) {
        func_ret_.push_back(resolve_type(f.ret));
        std::vector<RType> ptypes;
        for (const Param& p : f.params) {
            ptypes.push_back(resolve_type(p.type));
            if (ptypes.back().is_void) error(p.line, p.col, "\"void\" can't be used as a parameter type.");
        }
        func_params_.push_back(std::move(ptypes));
        uint16_t nreq = uint16_t(f.params.size());
        bool seen_default = false;
        for (size_t k = 0; k < f.params.size(); ++k) {
            if (f.params[k].def) {
                if (!seen_default) nreq = uint16_t(k);
                seen_default = true;
            } else if (seen_default) {
                error(f.params[k].line, f.params[k].col, "Cannot have mandatory parameters after optional parameters.");
            }
        }
        func_nreq_.push_back(nreq);
        if (f.params.size() > 255) error(f.line, f.col, "Too many parameters (the maximum is 255).");
    }
    sd.funcs.resize(ast_.funcs.size() + 2);
    sd.init_fn = int(ast_.funcs.size());
    sd.onready_fn = int(ast_.funcs.size() + 1);
    compile_init(false);
    compile_init(true);
    for (size_t i = 0; i < ast_.funcs.size(); ++i) compile_function(ast_.funcs[i], sd.funcs[i]);
    for (size_t i = 0; i < ast_.funcs.size(); ++i) {
        if (sd.funcs[i].name == "_ready") sd.ready_fn = int(i);
        if (sd.funcs[i].name == "_process") sd.process_fn = int(i);
    }
    build_metadata();
}

} // namespace
} // namespace detail

CompileResult compile(std::string_view source, std::string path, const Host& host) {
    CompileResult result;
    detail::Ast ast;
    {
        detail::Parser parser(source, ast, result.errors);
        parser.parse_script();
    }
    if (!result.errors.empty()) return result;
    std::unique_ptr<ScriptData> sd(new ScriptData());
    sd->path = std::move(path);
    sd->serial = detail::next_script_serial();
    {
        detail::Compiler compiler(ast, host, result.errors, result.warnings);
        compiler.run(*sd);
    }
    if (!result.errors.empty()) return result;
    // Plain `new` rather than make_shared, so the control block type is one users can name too.
    // UBSan's vptr check needs that across -fno-rtti code (see tests/script_tests.cpp).
    result.script = std::shared_ptr<const Script>(new Script(std::move(sd)));
    return result;
}

} // namespace jetscript
