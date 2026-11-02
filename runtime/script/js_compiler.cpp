#include "js_parser.hpp"

#include <algorithm>
#include <cmath>

namespace jetscript{

    namespace detail{

        namespace{

            struct RType{
                // annotation given
                bool present = false;
                bool ok = true;
                bool variant = false;
                bool is_void = false;
                Type type = Type::Nil;
                // engine class name for Object annotations
                std::string cls;
                bool typed() const{ return present && ok && !variant && !is_void; }
            };

            struct Local{
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

            struct LoopCtx{
                size_t continueTarget = 0;
                std::vector<size_t> breaks;
            };

            struct FuncState{
                FunctionData* fd = nullptr;
                std::vector<Local> locals;
                std::vector<size_t> scopes;
                uint32_t nextSlot = 0, maxSlots = 0;
                int depth = 0, maxDepth = 0;
                std::vector<LoopCtx> loops;
                RType ret;
                int curLine = 0;
            };

            struct ConstInfo{
                const ConstDecl* decl = nullptr;
                Value value;
                // 0 unresolved, 1 resolving, 2 resolved, 3 failed
                int state = 0;
                int line = 0, col = 0;
                uint32_t name = 0;
            };

            struct EnumInfo{
                uint32_t name = 0;
                Value dict;
                std::vector<std::pair<uint32_t, Int>> values;
                int line = 0;
            };

            enum class CS : uint8_t{ Var, Const, Enum, Func, Signal };
            struct ClassSym{
                CS kind;
                uint32_t index;
                int line;
            };

            struct WB{
                Op op;
                uint32_t arg;
                int delta;
            };

            bool isContainer(const Value& v){ return v.type() == Type::Array || v.type() == Type::Dictionary; }

            // Levels of nonempty arrays/dictionaries: the bytecode loader reads at most kMaxValueDepth.
            int valueHeight(const Value& v){
                int height = 0;

                if(v.type() == Type::Array){
                    for(const Value& item : Heap::arr(v)->items) height = std::max(height, valueHeight(item) + 1);
                } else if(v.type() == Type::Dictionary){
                    for(const auto& e : Heap::dict(v)->entries)
                        height = std::max(height, std::max(valueHeight(e.key), valueHeight(e.value)) + 1);
                }

                return height;
            }

            SType stypeOf(const Value& v){ return SType(v.type()); }

            class Compiler{
                public:
                    Compiler(Ast& ast, const Host& host, std::vector<Diagnostic>& errors, std::vector<Diagnostic>& warnings)
                        : ast_(ast), host_(host), errors_(errors), warnings_(warnings){}

                    void run(ScriptData& sd);

                private:
                    void error(int line, int col, std::string msg);
                    void warning(int line, int col, std::string msg);
                    const std::string& nm(uint32_t id) const{ return ast_.name(id); }

                    void setupBase();
                    void registerSymbols();
                    void resolveEnums();
                    bool resolveConst(uint32_t index, Value& out);
                    RType resolveType(const TypeRef& t);
                    const PropertyInfo* findProp(const std::string& name) const;
                    const MethodInfo* findMethod(const std::string& name) const;
                    bool findSignal(const std::string& name) const;
                    bool classConstant(const std::string& cls, const std::string& name, Value& out) const;
                    void emitBound(const std::string& name, bool signal);
                    bool class_exists(const std::string& name) const;
                    void buildMetadata();

                    void beginFunction(FunctionData& fd, int line);
                    void endFunction();
                    void compileInit(bool onready);
                    void compileFunction(const FuncDecl& f, FunctionData& fd);
                    void pushScope();
                    void popScope();
                    const Local* findLocal(uint32_t name) const;
                    Local& declareLocal(uint32_t name, int line, int col, bool withSlot = true);
                    uint32_t allocHidden(uint32_t count);

                    size_t pc() const{ return fs_->fd->code.size(); }
                    void adjust(int d);
                    void emit(Op op, uint32_t arg, int delta);
                    void emitWord(uint32_t w){ fs_->fd->code.push_back(w); }
                    size_t emitJump(Op op, int delta);
                    void patch(size_t at){ patchTo(at, pc()); }
                    void patchTo(size_t at, size_t target);
                    void patchAll(std::vector<size_t>& list){
                        for(size_t at : list) patch(at);

                        list.clear();
                    }
                    void emitJumpBack(size_t target);
                    void emitConst(const Value& v);
                    uint32_t add_const(const Value& v);
                    uint32_t nameEntry(const std::string& name);
                    void emitConvert(const RType& rt, uint32_t ctx, uint32_t param, SType src, int line, int col, const std::string& what);

                    // statements (recursive path: compileBlock -> compileStmt -> compileIf/... -> compileBlock)
                    void compileBlock(const std::vector<Stmt*>& body);
                    void compileStmt(const Stmt* s);
                    JS_NOINLINE void compileSimpleStmt(const Stmt* s);
                    JS_NOINLINE void compileVar(const Stmt* s);
                    JS_NOINLINE void compileLocalConst(const Stmt* s);
                    JS_NOINLINE void compileAssign(const Stmt* s);
                    JS_NOINLINE bool assignSimple(const Stmt* s);
                    JS_NOINLINE void compileIf(const Stmt* s);
                    JS_NOINLINE void compileWhile(const Stmt* s);
                    JS_NOINLINE void compileFor(const Stmt* s);
                    JS_NOINLINE void forBegin(const Stmt* s, size_t& top);
                    JS_NOINLINE void forEnd(const Stmt* s, size_t top);
                    JS_NOINLINE void compileMatch(const Stmt* s);
                    JS_NOINLINE size_t matchPatterns(const MatchBranch& br, uint32_t tmp, bool& unconditional);
                    JS_NOINLINE void compileReturn(const Stmt* s);
                    void condJump(Node* n, bool when, std::vector<size_t>& out);
                    JS_NOINLINE int constTruth(Node* n);
                    JS_NOINLINE bool ltLocalJump(Node* n, std::vector<size_t>& out);

                    // expressions (recursive path: compileExpr -> compile_xxx -> compileExpr)
                    SType compileExpr(Node* n);
                    JS_NOINLINE bool emitFolded(Node* n, SType& type);
                    JS_NOINLINE SType emitLiteral(Node* n);
                    JS_NOINLINE SType emitNilError(Node* n);
                    JS_NOINLINE SType compileIdent(Node* n);
                    SType compileLogic(Node* n);
                    SType compileTernary(Node* n);
                    SType compileMember(Node* n);
                    JS_NOINLINE bool memberStatic(Node* n, SType& type);
                    JS_NOINLINE SType emitGetNamed(Node* n, SType baseType);
                    SType compileCall(Node* n);
                    JS_NOINLINE bool planCall(Node* n, uint32_t& word0, uint32_t& word1, SType& ret, uint8_t& retmode);
                    SType compileMethodCall(Node* n);
                    JS_NOINLINE int methodStatic(Node* n);
                    JS_NOINLINE SType emitIsAs(Node* n);
                    JS_NOINLINE SType emitGetNode(Node* n);
                    SType pushLvalueBase(Node* e, std::vector<WB>& wbs);
                    JS_NOINLINE bool lvalueStatic(Node* e, std::vector<WB>& wbs, SType& type);
                    // static type checks run the VM's own operators on sample values
                    JS_NOINLINE SType checkBinary(BinOp op, SType a, SType b, int line, int col);
                    JS_NOINLINE SType checkUnary(UnOp op, SType a, int line, int col);
                    JS_NOINLINE SType checkIndex(SType base, SType index, int line, int col);
                    JS_NOINLINE SType checkMethod(Node* n, SType receiver);
                    JS_NOINLINE SType checkFieldSet(Node* target, SType base, SType value, int op, int line, int col);
                    JS_NOINLINE void checkPropAssign(const PropertyInfo* p, SType value, int line, int col);
                    JS_NOINLINE void checkCallArgs(Node* n, uint32_t fi, const std::vector<SType>* types);
                    Local* findLocalMut(uint32_t name);
                    bool tryConst(Node* n, Value& out, bool containers = false);
                    bool constEval(Node* n, Value& out, bool containers);
                    JS_NOINLINE bool constIdent(Node* n, Value& out);
                    JS_NOINLINE int constStaticMember(Node* n, Value& out);
                    JS_NOINLINE bool constField(Node* n, Value& base);
                    JS_NOINLINE bool constUnary(Node* n, Value& v);
                    JS_NOINLINE bool constBinary(Node* n, Value& a, const Value& b);
                    JS_NOINLINE bool constCall(Node* n, Value& out, bool containers);
                    JS_NOINLINE bool constCallInvoke(Node* n, std::vector<Value>& args, Value& out);
                    JS_NOINLINE int constCallCheck(Node* n);
                    JS_NOINLINE bool constContainer(Node* n, Value& out);
                    bool isBuiltinCall(uint32_t name) const;
                    bool namesType(uint32_t name, Type& t, bool& is_class) const;

                    Ast& ast_;
                    const Host& host_;
                    std::vector<Diagnostic>& errors_;
                    std::vector<Diagnostic>& warnings_;
                    ScriptData* sd_ = nullptr;
                    std::vector<const ClassInfo*> chain_;
                    std::unordered_map<uint32_t, ClassSym> syms_;
                    std::vector<ConstInfo> consts_;
                    // constants being resolved through other constants, capped like nesting
                    int constDepth_ = 0;
                    // a constant array/dictionary was nested deeper than bytecode allows
                    bool tooDeep_ = false;
                    // constant pool index by type and bits, see add_const()
                    std::unordered_map<std::string, uint32_t> constIndex_;
                    std::vector<EnumInfo> enums_;
                    std::vector<RType> varTypes_;
                    std::vector<SType> varStype_;
                    // static type of each member initializer (export inference)
                    std::vector<SType> initStype_;
                    std::vector<RType> funcRet_;
                    std::vector<std::vector<RType>> funcParams_;
                    std::vector<uint16_t> funcNreq_;
                    std::unordered_map<std::string, uint32_t> nameEntries_;
                    FuncState* fs_ = nullptr;
                    int getNodeMid_ = -1;
            };

            void Compiler::error(int line, int col, std::string msg){
                if(int(errors_.size()) >= kMaxErrors) return;

                for(const Diagnostic& d : errors_)
                    if(d.line == line && d.message == msg) return;

                Diagnostic d;
                d.line = line;
                d.column = col;
                d.message = std::move(msg);
                errors_.push_back(std::move(d));
            }

            void Compiler::warning(int line, int col, std::string msg){
                if(int(warnings_.size()) >= kMaxErrors) return;

                Diagnostic d;
                d.line = line;
                d.column = col;
                d.message = std::move(msg);
                warnings_.push_back(std::move(d));
            }

            void Compiler::setupBase(){
                std::string base = "Node";

                if(ast_.hasExtends) base = nm(ast_.extendsName);

                const ClassInfo* ci = host_.findClass(base);

                if(!ci){
                    error(ast_.extendsLine, ast_.extendsCol, "Could not find base class \"" + base + "\".");
                    sd_->baseClass = base;
                    return;
                }

                sd_->baseClass = ci->name.empty() ? base : ci->name;

                for(int guard = 0; ci && guard < 64; ++guard){
                    chain_.push_back(ci);

                    if(ci->base.empty()) break;

                    ci = host_.findClass(ci->base);
                }

                getNodeMid_ = host_.methodId("get_node");
            }

            const PropertyInfo* Compiler::findProp(const std::string& name) const{
                for(const ClassInfo* c : chain_)
                    for(const PropertyInfo& p : c->properties)
                        if(p.name == name) return &p;

                return nullptr;
            }

            const MethodInfo* Compiler::findMethod(const std::string& name) const{
                for(const ClassInfo* c : chain_)
                    for(const MethodInfo& m : c->methods)
                        if(m.name == name) return &m;

                return nullptr;
            }

            // Tween.TRANS_SINE: the class's constants or its bases'.
            bool Compiler::classConstant(const std::string& cls, const std::string& name, Value& out) const{
                const ClassInfo* ci = host_.findClass(cls);

                for(int guard = 0; ci && guard < 64; ++guard){
                    for(const auto& c : ci->constants)
                        if(c.first == name){
                            out = Value(c.second);
                            return true;
                        }

                    ci = ci->base.empty() ? nullptr : host_.findClass(ci->base);
                }

                return false;
            }

            bool Compiler::findSignal(const std::string& name) const{
                for(const ClassInfo* c : chain_)
                    for(const SignalInfo& sig : c->signals)
                        if(sig.name == name) return true;

                return false;
            }

            // A Callable or Signal of self: `_on_timeout`, `health_changed`, `queue_free`.
            void Compiler::emitBound(const std::string& name, bool signal){
                emit(Op::PushBound, (add_const(Value(name)) << 1) | (signal ? 1u : 0u), 1);
            }

            bool Compiler::class_exists(const std::string& name) const{ return host_.findClass(name) != nullptr; }

            void Compiler::registerSymbols(){
                auto add = [&](uint32_t name, CS kind, uint32_t index, int line, int col){
                    auto it = syms_.find(name);

                    if(it != syms_.end()){
                        error(line, col, "The member \"" + nm(name) + "\" already exists in this script (line " +
                                             formatInt(it->second.line) + ").");
                        return;
                    }

                    if(kind == CS::Var && findProp(nm(name))){
                        error(line, col, "The member \"" + nm(name) + "\" already exists in parent class " + sd_->baseClass + ".");
                    }

                    if(kind == CS::Func && findMethod(nm(name)) && nm(name)[0] != '_')
                        warning(line, col, "The method \"" + nm(name) + "()\" overrides a method of the engine class; the engine will not call it.");
                    syms_[name] = ClassSym{kind, index, line};
                };

                for(size_t i = 0; i < ast_.vars.size(); ++i) add(ast_.vars[i].name, CS::Var, uint32_t(i), ast_.vars[i].line, ast_.vars[i].col);

                for(size_t i = 0; i < ast_.consts.size(); ++i){
                    ConstInfo ci;
                    ci.decl = &ast_.consts[i];
                    ci.line = ast_.consts[i].line;
                    ci.col = ast_.consts[i].col;
                    ci.name = ast_.consts[i].name;
                    consts_.push_back(ci);
                    add(ast_.consts[i].name, CS::Const, uint32_t(consts_.size() - 1), ci.line, ci.col);
                }

                for(size_t i = 0; i < ast_.funcs.size(); ++i) add(ast_.funcs[i].name, CS::Func, uint32_t(i), ast_.funcs[i].line, ast_.funcs[i].col);

                for(size_t i = 0; i < ast_.signals.size(); ++i){
                    const SignalDecl& sig = ast_.signals[i];

                    if(findSignal(nm(sig.name)))
                        error(sig.line, sig.col, "The signal \"" + nm(sig.name) + "\" already exists in parent class " + sd_->baseClass + ".");
                    add(sig.name, CS::Signal, uint32_t(i), sig.line, sig.col);
                }

                //a named enum becomes one symbol, the values of an unnamed one become constants (resolved later)
                for(size_t i = 0; i < ast_.enums.size(); ++i){
                    const EnumDecl& e = ast_.enums[i];
                    EnumInfo info;
                    info.name = e.name;
                    info.line = e.line;
                    enums_.push_back(info);

                    if(e.name){
                        add(e.name, CS::Enum, uint32_t(i), e.line, e.col);
                    } else {
                        for(size_t k = 0; k < e.values.size(); ++k){
                            ConstInfo ci;
                            //resolved by resolveEnums
                            ci.state = 3;
                            ci.line = e.positions[k].first;
                            ci.col = e.positions[k].second;
                            ci.name = e.values[k].first;
                            consts_.push_back(ci);
                            add(e.values[k].first, CS::Const, uint32_t(consts_.size() - 1), ci.line, ci.col);
                        }
                    }
                }
            }

            void Compiler::resolveEnums(){
                size_t unnamedConst = ast_.consts.size();

                for(size_t i = 0; i < ast_.enums.size(); ++i){
                    const EnumDecl& e = ast_.enums[i];
                    Value dict = Value::newDictionary();
                    Int next = 0;

                    for(size_t k = 0; k < e.values.size(); ++k){
                        Int v = next;

                        if(e.values[k].second){
                            Value cv;

                            if(constEval(e.values[k].second, cv, false) && cv.type() == Type::Int){
                                v = cv.asInt();
                            } else {
                                error(e.positions[k].first, e.positions[k].second,
                                      "Enum values must be constant integers (\"" + nm(e.values[k].first) + "\").");
                            }
                        }

                        next = Int(uint64_t(v) + 1);
                        enums_[i].values.emplace_back(e.values[k].first, v);

                        if(Heap::dict(dict)->findStr(nm(e.values[k].first)) >= 0)
                            error(e.positions[k].first, e.positions[k].second, "Duplicate enum key \"" + nm(e.values[k].first) + "\".");
                        Heap::dict(dict)->set(Value(nm(e.values[k].first)), Value(v));

                        if(!e.name){
                            ConstInfo& ci = consts_[unnamedConst++];
                            ci.value = Value(v);
                            ci.state = 2;
                        }
                    }

                    Heap::dict(dict)->read_only = true;
                    enums_[i].dict = dict;
                }
            }

            bool Compiler::resolveConst(uint32_t index, Value& out){
                ConstInfo& ci = consts_[index];

                if(ci.state == 2){
                    out = ci.value;
                    return true;
                }

                if(ci.state == 1){
                    error(ci.line, ci.col, "Could not resolve constant \"" + nm(ci.name) + "\": cyclic reference.");
                    ci.state = 3;
                    return false;
                }

                if(ci.state == 3) return false;

                if(constDepth_ >= kMaxNesting){
                    error(ci.line, ci.col, "Could not resolve constant \"" + nm(ci.name) + "\": it depends on more than " +
                                               std::to_string(kMaxNesting) + " other constants in a chain.");
                    ci.state = 3;
                    return false;
                }

                ci.state = 1;
                Value v;
                ++constDepth_;
                tooDeep_ = false;
                const bool ok = ci.decl && constEval(ci.decl->init, v, true);
                --constDepth_;

                if(!ok){
                    if(ci.decl && tooDeep_){
                        error(ci.line, ci.col, "Constant \"" + nm(ci.name) + "\" nests arrays or dictionaries more than " +
                                                   std::to_string(kMaxValueDepth) + " levels deep.");
                    } else if(ci.decl){
                        error(ci.line, ci.col, "Assigned value for constant \"" + nm(ci.name) + "\" is not a constant expression.");
                    }

                    ci.state = 3;
                    return false;
                }

                RType rt = resolveType(ci.decl->type);

                if(rt.typed() && rt.type != Type::Object && !convertTo(rt.type, v)){
                    error(ci.line, ci.col, std::string("Cannot assign a value of type \"") + type_name(v.type()) + "\" to constant \"" +
                                               nm(ci.name) + "\" with specified type \"" + type_name(rt.type) + "\".");
                }

                if(v.type() == Type::Array) Heap::arr(v)->read_only = true;

                if(v.type() == Type::Dictionary) Heap::dict(v)->read_only = true;

                ci.value = v;
                ci.state = 2;
                out = v;
                return true;
            }

            RType Compiler::resolveType(const TypeRef& t){
                RType r;

                if(!t.present) return r;

                r.present = true;
                const std::string& name = nm(t.name);

                if(name == "void"){
                    r.is_void = true;
                    return r;
                }

                bool variant = false;
                Type bt;

                if(builtinTypeFromName(name, bt, variant)){
                    r.type = bt;
                    r.variant = variant;

                    if(bt == Type::Object && !variant) r.cls = "Object";

                    return r;
                }

                auto it = syms_.find(t.name);

                if(it != syms_.end() && it->second.kind == CS::Enum){
                    r.type = Type::Int;
                    return r;
                }

                if(class_exists(name)){
                    r.type = Type::Object;
                    r.cls = name;
                    return r;
                }

                error(t.line, t.col, "Could not find type \"" + name + "\" in the current scope.");
                r.ok = false;
                r.variant = true;
                return r;
            }

            void Compiler::beginFunction(FunctionData& fd, int line){
                fs_ = new FuncState();
                fs_->fd = &fd;
                fs_->curLine = line;
                fd.line = line;
            }

            void Compiler::endFunction(){
                FunctionData& fd = *fs_->fd;
                fd.nlocals = fs_->maxSlots;
                fd.frameSize = fs_->maxSlots + uint32_t(fs_->maxDepth) + 2;

                if(fd.frameSize > 65535) error(fd.line, 1, "Function \"" + fd.name + "\" is too large.");

                delete fs_;
                fs_ = nullptr;
            }

            void Compiler::pushScope(){ fs_->scopes.push_back(fs_->locals.size()); }

            void Compiler::popScope(){
                size_t mark = fs_->scopes.back();
                fs_->scopes.pop_back();

                for(size_t i = mark; i < fs_->locals.size(); ++i){
                    const Local& l = fs_->locals[i];

                    if(l.warn_unused && !l.used && l.name && nm(l.name)[0] != '_')
                        warning(l.line, l.col, "The local " + std::string(l.is_const ? "constant" : "variable") + " \"" + nm(l.name) +
                                                   "\" is declared but never used in the block. If this is intended, prefix it with an underscore: \"_" +
                                                   nm(l.name) + "\".");
                }

                fs_->locals.resize(mark);
                uint32_t top = 0;

                for(const Local& l : fs_->locals)
                    if(!l.is_const && l.slot + 1 > top) top = l.slot + 1;

                //slots of the closed scope (including hidden ones) are reused
                fs_->nextSlot = top;
            }

            const Local* Compiler::findLocal(uint32_t name) const{
                if(!fs_) return nullptr;

                for(size_t i = fs_->locals.size(); i-- > 0;)
                    if(fs_->locals[i].name == name) return &fs_->locals[i];

                return nullptr;
            }

            Local* Compiler::findLocalMut(uint32_t name){
                if(!fs_) return nullptr;

                for(size_t i = fs_->locals.size(); i-- > 0;)
                    if(fs_->locals[i].name == name){
                        fs_->locals[i].used = true;
                        return &fs_->locals[i];
                    }

                return nullptr;
            }

            uint32_t Compiler::allocHidden(uint32_t count){
                uint32_t s = fs_->nextSlot;
                fs_->nextSlot += count;

                if(fs_->nextSlot > fs_->maxSlots) fs_->maxSlots = fs_->nextSlot;

                //reserve with an unnamed local so popScope keeps track of the slot
                for(uint32_t i = 0; i < count; ++i){
                    Local l;
                    l.name = 0;
                    l.slot = s + i;
                    fs_->locals.push_back(l);
                }

                return s;
            }

            Local& Compiler::declareLocal(uint32_t name, int line, int col, bool withSlot){
                if(const Local* prev = findLocal(name)){
                    error(line, col, "There is already a variable named \"" + nm(name) + "\" declared in this function (line " +
                                         formatInt(prev->line) + ").");
                } else {
                    auto it = syms_.find(name);

                    if(it != syms_.end() && it->second.kind == CS::Var)
                        warning(line, col, "The local variable \"" + nm(name) + "\" is shadowing an already-declared member variable (line " +
                                               formatInt(it->second.line) + ").");
                }

                Local l;
                l.name = name;
                l.slot = withSlot ? fs_->nextSlot++ : 0;
                l.is_const = !withSlot;
                l.line = line;
                l.col = col;

                if(fs_->nextSlot > fs_->maxSlots) fs_->maxSlots = fs_->nextSlot;

                if(fs_->maxSlots > kMaxLocals) error(line, col, "Too many local variables in function.");

                fs_->locals.push_back(l);
                return fs_->locals.back();
            }

            void Compiler::adjust(int d){
                fs_->depth += d;

                //only after errors
                if(fs_->depth < 0) fs_->depth = 0;

                if(fs_->depth > fs_->maxDepth) fs_->maxDepth = fs_->depth;
            }

            void Compiler::emit(Op op, uint32_t arg, int delta){
                FunctionData& fd = *fs_->fd;

                if(fd.lines.empty() || fd.lines.back().line != fs_->curLine){
                    if(!fd.lines.empty() && fd.lines.back().pc == fd.code.size()) fd.lines.back().line = fs_->curLine;
                    else fd.lines.push_back(LineEntry{uint32_t(fd.code.size()), fs_->curLine});
                }

                fd.code.push_back(enc(op, arg & 0xFFFFFFu));
                adjust(delta);
            }

            size_t Compiler::emitJump(Op op, int delta){
                size_t at = pc();
                emit(op, 0, delta);
                return at;
            }

            void Compiler::patchTo(size_t at, size_t target){
                uint32_t& w = fs_->fd->code[at];
                int32_t off = int32_t(target) - int32_t(at);

                if(opOf(w) == Op::LtLocalJump){
                    fs_->fd->code[at + 2] = uint32_t(off);
                    return;
                }

                //the offset is a signed 24-bit operand
                if(off < -(1 << 23) || off >= (1 << 23)){
                    error(fs_->fd->line, 1, "Function \"" + fs_->fd->name + "\" is too large.");
                    return;
                }

                w = enc(opOf(w), senc(off));
            }

            void Compiler::emitJumpBack(size_t target){
                size_t at = pc();
                emit(Op::Jump, 0, 0);
                patchTo(at, target);
            }

            uint32_t Compiler::add_const(const Value& v){
                //scalars, strings and containers are found by key: a linear scan per constant was quadratic
                std::string key(1, char(v.type()));
                bool keyed = true;

                switch(v.type()){
                    case Type::Bool: key += Heap::b(v) ? '1' : '0'; break;
                    case Type::Int:{
                        const Int n = Heap::i(v);
                        key.append(reinterpret_cast<const char*>(&n), sizeof n);
                        break;
                    }
                    case Type::Float: case Type::Vector2: case Type::Vector3: case Type::Color:{
                        const int n = v.type() == Type::Float ? 1 : v.type() == Type::Vector2 ? 2 : 3;

                        for(int k = 0; k < n; ++k){
                            const Real c = Heap::comp(v, k);
                            key.append(reinterpret_cast<const char*>(&c), sizeof c);
                        }
                        break;
                    }
                    case Type::String: key += Heap::sv(v); break;
                    case Type::Array: case Type::Dictionary:{
                        const void* p = Heap::p(v);
                        key.append(reinterpret_cast<const char*>(&p), sizeof p);
                        break;
                    }
                    default: keyed = false; break;
                }

                if(keyed){
                    auto found = constIndex_.find(key);

                    if(found != constIndex_.end()) return found->second;

                    sd_->constants.push_back(v);
                    constIndex_.emplace(std::move(key), uint32_t(sd_->constants.size() - 1));
                    return uint32_t(sd_->constants.size() - 1);
                }

                for(size_t i = 0; i < sd_->constants.size(); ++i){
                    const Value& c = sd_->constants[i];

                    if(!isContainer(c) && strictEqual(c, v) && !(v.type() == Type::Float && std::signbit(Heap::f(v)) != std::signbit(Heap::f(c))))
                        return uint32_t(i);

                    if(isContainer(c) && Heap::p(c) == Heap::p(v)) return uint32_t(i);
                }

                sd_->constants.push_back(v);
                return uint32_t(sd_->constants.size() - 1);
            }

            void Compiler::emitConst(const Value& v){
                switch(v.type()){
                    case Type::Nil: emit(Op::PushNil, 0, 1); return;
                    case Type::Bool: emit(Heap::b(v) ? Op::PushTrue : Op::PushFalse, 0, 1); return;
                    case Type::Int:
                        if(Heap::i(v) >= -0x800000 && Heap::i(v) < 0x800000){
                            emit(Op::PushInt, senc(int32_t(Heap::i(v))), 1);
                            return;
                        }
                        break;
                    default: break;
                }

                emit(Op::PushConst, add_const(v), 1);
            }

            uint32_t Compiler::nameEntry(const std::string& name){
                auto it = nameEntries_.find(name);

                if(it != nameEntries_.end()) return it->second;

                NameEntry e;
                e.name = name;
                e.pid = host_.propertyId(name);
                e.mid = host_.methodId(name);
                e.field = int8_t(findBuiltinField(name));
                e.bmethod = int16_t(findBuiltinMethod(name));
                e.smethod = int8_t(findSignalMethod(name));
                uint32_t id = uint32_t(sd_->names.size());
                sd_->names.push_back(std::move(e));
                nameEntries_[name] = id;
                return id;
            }

            static bool staticCompatible(SType src, const RType& rt){
                if(src == kUnknown || !rt.typed()) return true;

                Type s = Type(src), t = rt.type;

                if(s == t) return true;

                if((t == Type::Int || t == Type::Float) && (s == Type::Int || s == Type::Float)) return true;

                if(t == Type::Object && s == Type::Nil) return true;

                return false;
            }

            void Compiler::emitConvert(const RType& rt, uint32_t ctx, uint32_t param, SType src, int line, int col,
                                        const std::string& what){
                if(!rt.typed()) return;

                if(!staticCompatible(src, rt)){
                    std::string tn = rt.type == Type::Object ? rt.cls : std::string(type_name(rt.type));

                    if(ctx == kConvReturn)
                        error(line, col, std::string("Cannot return a value of type \"") + type_name(Type(src)) +
                                             "\" as \"" + tn + "\".");
                    else if(what.empty())
                        error(line, col, std::string("Cannot assign a value of type \"") + type_name(Type(src)) + "\" as \"" + tn + "\".");
                    else
                        error(line, col, std::string("Cannot assign a value of type \"") + type_name(Type(src)) + "\" to " + what +
                                             " with specified type \"" + tn + "\".");
                    return;
                }

                if(rt.type == Type::Object){
                    if(src == SType(Type::Nil)) return;

                    if(rt.cls.empty() || rt.cls == "Object") emit(Op::Convert, uint32_t(Type::Object) | (ctx << 8) | (param << 10), 0);
                    else emit(Op::CheckClass, nameEntry(rt.cls) | (ctx << 20), 0);
                    return;
                }

                if(src == SType(rt.type)) return;

                emit(Op::Convert, uint32_t(rt.type) | (ctx << 8) | ((param & 0x3FFFu) << 10), 0);
            }

            bool Compiler::isBuiltinCall(uint32_t name) const{
                if(findLocal(name)) return false;

                auto it = syms_.find(name);

                if(it != syms_.end()) return false;

                if(findMethod(nm(name))) return false;

                return findBuiltinFunc(nm(name)) >= 0 || nm(name) == "String";
            }

            bool Compiler::namesType(uint32_t name, Type& t, bool& is_class) const{
                if(findLocal(name) || syms_.count(name) || findProp(nm(name))) return false;

                //e.g. `Input` is an object, not a type
                if(host_.singleton(nm(name)) != 0) return false;

                bool variant = false;
                is_class = false;

                if(builtinTypeFromName(nm(name), t, variant)) return !variant;

                if(class_exists(nm(name))){
                    t = Type::Object;
                    is_class = true;
                    return true;
                }

                return false;
            }

            bool Compiler::tryConst(Node* n, Value& out, bool containers){
                if(!n) return false;

                if(n->kind == NK::Literal){
                    out = n->value;
                    return true;
                }

                if(containers) return constEval(n, out, true);

                if(n->cstate == 2) return false;

                if(n->cstate == 1){
                    out = n->value;
                    return true;
                }

                bool ok = constEval(n, out, false);

                if(ok && isContainer(out) && !static_cast<HeapObj*>(Heap::p(out))->read_only) ok = false;

                n->cstate = ok ? 1 : 2;

                if(ok) n->value = out;

                return ok;
            }

            bool Compiler::constIdent(Node* n, Value& out){
                if(const Local* l = findLocalMut(n->name)){
                    if(!l->is_const) return false;

                    out = l->cval;
                    return true;
                }

                auto it = syms_.find(n->name);

                if(it != syms_.end()){
                    if(it->second.kind == CS::Const) return resolveConst(it->second.index, out);

                    if(it->second.kind == CS::Enum){
                        out = enums_[it->second.index].dict;
                        return !out.isNil();
                    }

                    return false;
                }

                if(findProp(nm(n->name))) return false;

                return builtinGlobalConstant(nm(n->name), out);
            }

            // 1: constant found, 0: not constant, -1: not a static member (evaluate the base)
            int Compiler::constStaticMember(Node* n, Value& out){
                Node* b = n->a;

                if(b->kind != NK::Ident || findLocal(b->name)) return -1;

                auto it = syms_.find(b->name);

                if(it != syms_.end() && it->second.kind == CS::Enum){
                    for(const auto& kv : enums_[it->second.index].values)
                        if(kv.first == n->name){
                            out = Value(kv.second);
                            return 1;
                        }

                    return 0;
                }

                Type t;
                bool is_class = false;

                if(it == syms_.end() && namesType(b->name, t, is_class)){
                    if(is_class) return classConstant(nm(b->name), nm(n->name), out) ? 1 : 0;

                    return builtinTypeConstant(t, nm(n->name), out) ? 1 : 0;
                }

                return -1;
            }

            bool Compiler::constField(Node* n, Value& base){
                int f = findBuiltinField(nm(n->name));

                if((base.type() == Type::Vector3 && f >= 0 && f <= 2) || (base.type() == Type::Vector2 && f >= 0 && f <= 1)){
                    base = Value(Heap::comp(base, f));
                    return true;
                }

                if(base.type() == Type::Color && f >= 3){
                    base = Value(f == 6 ? Real(1) : Heap::comp(base, f - 3));
                    return true;
                }

                return false;
            }

            bool Compiler::constUnary(Node* n, Value& v){
                std::string err;
                Value r;

                if(!unary_op(UnOp(n->op), v, r, err)) return false;

                v = r;
                return true;
            }

            bool Compiler::constBinary(Node* n, Value& a, const Value& b){
                if(isContainer(a) || isContainer(b)) return false;

                std::string err;
                Value r;

                if(!binary_op(BinOp(n->op), a, b, r, err)) return false;

                a = r;
                return true;
            }

            // Returns the builtin index for a foldable call, or -1.
            int Compiler::constCallCheck(Node* n){
                if(!isBuiltinCall(n->name)) return -1;

                int id = nm(n->name) == "String" ? findBuiltinFunc("str") : findBuiltinFunc(nm(n->name));

                if(id < 0) return -1;

                const BuiltinFunc& bf = builtinFuncs()[id];

                if(!(bf.flags & kPure)) return -1;

                int argc = int(n->list.size());

                if(argc < bf.min_args || (bf.max_args >= 0 && argc > bf.max_args)) return -1;

                return id;
            }

            bool Compiler::constCallInvoke(Node* n, std::vector<Value>& args, Value& out){
                int id = constCallCheck(n);

                if(id < 0) return false;

                BuiltinCtx ctx;
                Value r;

                if(!builtinFuncs()[id].fn(ctx, args.data(), int(args.size()), r)) return false;

                if(isContainer(r)) return false;

                out = r;
                return true;
            }

            bool Compiler::constCall(Node* n, Value& out, bool containers){
                if(constCallCheck(n) < 0) return false;

                std::vector<Value> args(n->list.size());

                for(size_t i = 0; i < n->list.size(); ++i)
                    if(!constEval(n->list[i], args[i], containers) || isContainer(args[i])) return false;

                return constCallInvoke(n, args, out);
            }

            bool Compiler::constContainer(Node* n, Value& out){
                if(n->kind == NK::ArrayLit){
                    std::vector<Value> items(n->list.size());

                    for(size_t i = 0; i < n->list.size(); ++i)
                        if(!constEval(n->list[i], items[i], true)) return false;

                    out = makeArray(std::move(items));
                    Heap::arr(out)->read_only = true;

                    //a deeper constant would not load back from bytecode
                    if(valueHeight(out) > kMaxValueDepth){ tooDeep_ = true; return false; }

                    return true;
                }

                Value d = Value::newDictionary();

                for(size_t i = 0; i + 1 < n->list.size(); i += 2){
                    Value k, v;

                    if(!constEval(n->list[i], k, true) || !constEval(n->list[i + 1], v, true)) return false;

                    if(!validDictKey(k)) return false;

                    Heap::dict(d)->set(k, v);
                }

                Heap::dict(d)->read_only = true;
                out = d;

                if(valueHeight(out) > kMaxValueDepth){ tooDeep_ = true; return false; }

                return true;
            }

            bool Compiler::constEval(Node* n, Value& out, bool containers){
                if(!n) return false;

                switch(n->kind){
                    case NK::Literal: out = n->value; return true;
                    case NK::Ident: return constIdent(n, out);
                    case NK::Member:{
                        int r = constStaticMember(n, out);

                        if(r >= 0) return r == 1;

                        if(!constEval(n->a, out, containers)) return false;

                        return constField(n, out);
                    }
                    case NK::Unary: return constEval(n->a, out, containers) && constUnary(n, out);
                    case NK::Not:
                        if(!constEval(n->a, out, containers)) return false;

                        out = Value(!truthy(out));
                        return true;
                    case NK::Binary:{
                        Value b;
                        return constEval(n->a, out, containers) && constEval(n->b, b, containers) && constBinary(n, out, b);
                    }
                    case NK::And:
                    case NK::Or:{
                        Value b;

                        if(!constEval(n->a, out, containers) || !constEval(n->b, b, containers)) return false;

                        bool r = n->kind == NK::And ? (truthy(out) && truthy(b)) : (truthy(out) || truthy(b));
                        out = Value(r);
                        return true;
                    }
                    case NK::Ternary:{
                        Value c;

                        if(!constEval(n->b, c, containers)) return false;

                        return constEval(truthy(c) ? n->a : n->c, out, containers);
                    }
                    case NK::Call: return constCall(n, out, containers);
                    case NK::ArrayLit:
                    case NK::DictLit: return containers && constContainer(n, out);
                    default: return false;
                }
            }

            void Compiler::compileBlock(const std::vector<Stmt*>& body){
                pushScope();

                for(const Stmt* s : body) compileStmt(s);

                popScope();
            }

            void Compiler::compileStmt(const Stmt* s){
                fs_->curLine = s->line;
                fs_->depth = 0;

                switch(s->kind){
                    case SK::If: compileIf(s); return;
                    case SK::While: compileWhile(s); return;
                    case SK::For: compileFor(s); return;
                    case SK::Match: compileMatch(s); return;
                    default: compileSimpleStmt(s); return;
                }
            }

            void Compiler::compileSimpleStmt(const Stmt* s){
                switch(s->kind){
                    case SK::Expr:{
                        Node* e = s->a;
                        bool effect = e && (e->kind == NK::Call || e->kind == NK::MethodCall || e->kind == NK::GetNode);

                        if(e && !effect && e->kind != NK::Bad)
                            warning(s->line, s->col, "Standalone expression (the line may have no effect).");
                        compileExpr(e);
                        emit(Op::Pop, 0, -1);
                        return;
                    }
                    case SK::Var: compileVar(s); return;
                    case SK::Const: compileLocalConst(s); return;
                    case SK::Assign: compileAssign(s); return;
                    case SK::Pass: return;
                    case SK::Break:
                        //reported by the parser
                        if(fs_->loops.empty()) return;

                        fs_->loops.back().breaks.push_back(emitJump(Op::Jump, 0));
                        return;
                    case SK::Continue:
                        if(fs_->loops.empty()) return;

                        emitJumpBack(fs_->loops.back().continueTarget);
                        return;
                    case SK::Return: compileReturn(s); return;
                    default: return;
                }
            }

            void Compiler::compileVar(const Stmt* s){
                RType rt = resolveType(s->type);

                if(rt.is_void) error(s->type.line, s->type.col, "\"void\" can not be used as a variable type.");

                SType st = kUnknown;

                if(s->a){
                    st = compileExpr(s->a);

                    if(s->infer && st == SType(Type::Nil))
                        error(s->line, s->col, "Cannot infer the type of \"" + nm(s->name) + "\" variable because the value is \"null\".");
                } else if(rt.typed()){
                    if(rt.type == Type::Array) emit(Op::MakeArray, 0, 1);
                    else if(rt.type == Type::Dictionary) emit(Op::MakeDict, 0, 1);
                    else emitConst(typeDefault(rt.type));
                    st = SType(rt.type);
                } else {
                    emit(Op::PushNil, 0, 1);
                }

                fs_->curLine = s->line;

                if(rt.typed()) emitConvert(rt, kConvAssign, 0, st, s->line, s->col, "variable \"" + nm(s->name) + "\"");

                if(s->infer && !rt.present && st != kUnknown && st != SType(Type::Nil) && st != SType(Type::Object)){
                    rt.present = true;
                    rt.type = Type(st);
                }

                Local& l = declareLocal(s->name, s->line, s->col);
                l.rt = rt;
                l.stype = rt.typed() ? SType(rt.type) : kUnknown;
                l.warn_unused = true;
                emit(Op::StoreLocal, l.slot, -1);
            }

            void Compiler::compileLocalConst(const Stmt* s){
                Value v;

                if(!constEval(s->a, v, true)){
                    error(s->line, s->col, "Assigned value for constant \"" + nm(s->name) + "\" is not a constant expression.");
                    v = Value();
                }

                //constants do not use a slot
                Local& l = declareLocal(s->name, s->line, s->col, false);
                l.cval = v;
                l.stype = stypeOf(v);
                l.warn_unused = true;
            }

            static SType numericResult(SType a, SType b){
                if(a == SType(Type::Int) && b == SType(Type::Int)) return SType(Type::Int);

                bool an = a == SType(Type::Int) || a == SType(Type::Float);
                bool bn = b == SType(Type::Int) || b == SType(Type::Float);

                if(an && bn) return SType(Type::Float);

                return kUnknown;
            }

            static SType binaryResult(BinOp op, SType ta, SType tb){
                switch(op){
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
                        if(ta == SType(Type::String)) return SType(Type::String);

                        return numericResult(ta, tb);
                    case BinOp::Add:
                        if(ta == SType(Type::String) && tb == SType(Type::String)) return SType(Type::String);

                        if(ta == SType(Type::Array) && tb == SType(Type::Array)) return SType(Type::Array);

                        [[fallthrough]];
                    case BinOp::Sub:
                    case BinOp::Mul:
                    case BinOp::Div:
                    case BinOp::Pow:{
                        bool va = ta == SType(Type::Vector3) || ta == SType(Type::Vector2) || ta == SType(Type::Color);
                        bool vb = tb == SType(Type::Vector3) || tb == SType(Type::Vector2) || tb == SType(Type::Color);

                        if(va && (tb == ta || numericResult(tb, tb) != kUnknown)) return ta;

                        if(vb && numericResult(ta, ta) != kUnknown && op == BinOp::Mul) return tb;

                        return numericResult(ta, tb);
                    }
                    case BinOp::Count: break;
                }

                return kUnknown;
            }

            // Assignment targets known at compile time: locals, member variables, self properties and constants.
            // Returns false when `e` has to be evaluated at runtime.
            bool Compiler::lvalueStatic(Node* e, std::vector<WB>& wbs, SType& type){
                uint32_t name = 0;
                bool selfMember = false;
                type = kUnknown;

                if(e->kind == NK::Ident){
                    name = e->name;
                } else if(e->kind == NK::Member && e->a->kind == NK::Self){
                    name = e->name;
                    selfMember = true;
                } else if(e->kind == NK::Member && e->a->kind == NK::Ident){
                    Type t;
                    bool is_class = false;
                    auto it = syms_.find(e->a->name);

                    if(!findLocal(e->a->name) && ((it != syms_.end() && it->second.kind == CS::Enum) ||
                                                    (it == syms_.end() && namesType(e->a->name, t, is_class)))){
                        error(e->line, e->col, "Cannot assign a new value to a constant.");
                        emit(Op::PushNil, 0, 1);
                        wbs.push_back(WB{Op::Pop, 0, -1});
                        return true;
                    }

                    return false;
                } else {
                    return false;
                }

                if(!selfMember){
                    if(const Local* l = findLocalMut(name)){
                        if(l->is_const){
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

                if(it != syms_.end()){
                    if(it->second.kind == CS::Var){
                        emit(Op::LoadMember, it->second.index, 1);
                        wbs.push_back(WB{Op::StoreMember, it->second.index, -1});
                        type = varStype_[it->second.index];
                        return true;
                    }

                    error(e->line, e->col, "Cannot assign a new value to a constant.");
                    emit(Op::PushNil, 0, 1);
                    wbs.push_back(WB{Op::Pop, 0, -1});
                    return true;
                }

                if(const PropertyInfo* p = findProp(nm(name))){
                    uint32_t ne = nameEntry(p->name);

                    if(sd_->names[ne].pid >= 0){
                        emit(Op::LoadSelfProp, ne, 1);
                        wbs.push_back(WB{p->read_only ? Op::Pop : Op::WbSelfProp, ne, -1});
                        type = p->type == Type::Nil ? kUnknown : SType(p->type);
                        return true;
                    }
                }

                if(selfMember){
                    emit(Op::PushSelf, 0, 1);
                    emit(Op::Dup, 0, 1);
                    emit(Op::GetNamed, nameEntry(nm(name)), 0);
                    wbs.push_back(WB{Op::WbNamed, nameEntry(nm(name)), -1});
                    wbs.insert(wbs.end() - 1, WB{Op::Pop, 0, -1});
                    return true;
                }

                return false;
            }

            // Pushes the value the target modifies, leaving its containers on the stack, and records the write-back steps.
            // Returns the static type.
            SType Compiler::pushLvalueBase(Node* e, std::vector<WB>& wbs){
                fs_->curLine = e->line;
                SType t = kUnknown;

                if(lvalueStatic(e, wbs, t)) return t;

                if(e->kind == NK::Member){
                    SType bt = pushLvalueBase(e->a, wbs);
                    fs_->curLine = e->line;
                    uint32_t ne = nameEntry(nm(e->name));
                    emit(Op::Dup, 0, 1);
                    SType mt = emitGetNamed(e, bt);
                    wbs.push_back(WB{Op::WbNamed, ne, -1});
                    return mt;
                }

                if(e->kind == NK::Index){
                    SType bt = pushLvalueBase(e->a, wbs);
                    SType it = compileExpr(e->b);
                    fs_->curLine = e->line;
                    emit(Op::Dup2, 0, 2);
                    emit(Op::GetIndex, 0, -1);
                    wbs.push_back(WB{Op::WbIndex, 0, -2});
                    return checkIndex(bt, it, e->line, e->col);
                }

                t = compileExpr(e);
                wbs.push_back(WB{Op::Pop, 0, -1});
                return t;
            }

            // `x = v` and `x op= v` on locals, member variables and self properties; false if the target is not a plain name.
            bool Compiler::assignSimple(const Stmt* s){
                Node* t = s->a;
                int op = s->op;
                bool selfOnly = t->kind == NK::Member && t->a->kind == NK::Self;

                if(t->kind != NK::Ident && !selfOnly) return false;

                uint32_t name = t->name;
                //the right-hand side, or `target op rhs` for compound assignment
                auto value = [&](Op load, uint32_t arg, SType target) -> SType {
                    if(!op) return compileExpr(s->b);

                    emit(load, arg, 1);
                    SType rhs = compileExpr(s->b);
                    fs_->curLine = s->line;
                    emit(Op(int(Op::Add) + op - 1), 0, -1);
                    return checkBinary(BinOp(op - 1), target, rhs, s->line, s->col);
                };

                if(!selfOnly){
                    if(const Local* l = findLocalMut(name)){
                        if(l->is_const){
                            error(t->line, t->col, "Cannot assign a new value to a constant.");
                            return true;
                        }

                        uint32_t slot = l->slot;
                        RType rt = l->rt;
                        SType target = l->stype;
                        Value cv;

                        if(op == int(BinOp::Add) + 1 && tryConst(s->b, cv) && cv.type() == Type::Int && Heap::i(cv) >= INT32_MIN &&
                            Heap::i(cv) <= INT32_MAX && (target == kUnknown || target == SType(Type::Int) || target == SType(Type::Float))){
                            //`i += 1` fast path
                            emit(Op::AddLocalInt, slot, 0);
                            emitWord(uint32_t(int32_t(Heap::i(cv))));
                            return true;
                        }

                        SType st = value(Op::LoadLocal, slot, target);
                        fs_->curLine = s->line;
                        emitConvert(rt, kConvAssign, 0, st, s->line, s->col, std::string());
                        emit(Op::StoreLocal, slot, -1);
                        return true;
                    }
                }

                auto it = syms_.find(name);

                if(it != syms_.end()){
                    if(it->second.kind != CS::Var){
                        error(t->line, t->col, it->second.kind == CS::Func     ? "Cannot assign a new value to a function."
                                               : it->second.kind == CS::Signal ? "Cannot assign a new value to a signal."
                                                                               : "Cannot assign a new value to a constant.");
                        return true;
                    }

                    uint32_t idx = it->second.index;
                    SType st = value(Op::LoadMember, idx, varStype_[idx]);
                    fs_->curLine = s->line;
                    emitConvert(varTypes_[idx], kConvAssign, 0, st, s->line, s->col, std::string());
                    emit(Op::StoreMember, idx, -1);
                    return true;
                }

                if(const PropertyInfo* p = findProp(nm(name))){
                    if(p->read_only){
                        error(t->line, t->col, "Cannot assign a new value to a read-only property \"" + p->name + "\".");
                        return true;
                    }

                    uint32_t ne = nameEntry(p->name);

                    if(sd_->names[ne].pid >= 0){
                        SType st = value(Op::LoadSelfProp, ne, p->type == Type::Nil ? kUnknown : SType(p->type));
                        fs_->curLine = s->line;
                        checkPropAssign(p, st, s->line, s->col);
                        emit(Op::StoreSelfProp, ne, -1);
                        return true;
                    }
                }

                //self.<unknown>: dynamic assignment on self
                if(selfOnly) return false;

                Value tmp;

                if(findMethod(nm(name)) || findBuiltinFunc(nm(name)) >= 0)
                    error(t->line, t->col, "Cannot assign a new value to a function.");
                else if(builtinGlobalConstant(nm(name), tmp))
                    error(t->line, t->col, "Cannot assign a new value to a constant.");
                else
                    error(t->line, t->col, "Identifier \"" + nm(name) + "\" not declared in the current scope.");
                return true;
            }

            void Compiler::compileAssign(const Stmt* s){
                Node* t = s->a;
                //0 '=', else BinOp + 1
                int op = s->op;

                if(!t || t->kind == NK::Bad) return;

                if(assignSimple(s)) return;

                std::vector<WB> wbs;

                if(t->kind == NK::Member){
                    uint32_t ne = nameEntry(nm(t->name));
                    SType bt = kUnknown;

                    if(t->a->kind == NK::Self){
                        emit(Op::PushSelf, 0, 1);
                        wbs.push_back(WB{Op::Pop, 0, -1});
                        bt = SType(Type::Object);
                    } else {
                        bt = pushLvalueBase(t->a, wbs);
                    }

                    fs_->curLine = s->line;
                    SType current = kUnknown;

                    if(op){
                        emit(Op::Dup, 0, 1);
                        current = emitGetNamed(t, bt);
                    }

                    SType vt = compileExpr(s->b);
                    fs_->curLine = s->line;

                    if(op) emit(Op(int(Op::Add) + op - 1), 0, -1);

                    checkFieldSet(t, bt, op ? checkBinary(BinOp(op - 1), current, vt, s->line, s->col) : vt, op, s->line, s->col);
                    emit(Op::SetNamed, ne, -1);
                } else if(t->kind == NK::Index){
                    SType bt = pushLvalueBase(t->a, wbs);
                    SType it = compileExpr(t->b);
                    fs_->curLine = s->line;
                    SType current = checkIndex(bt, it, s->line, s->col);

                    if(op){
                        emit(Op::Dup2, 0, 2);
                        emit(Op::GetIndex, 0, -1);
                        SType vt = compileExpr(s->b);
                        fs_->curLine = s->line;
                        emit(Op(int(Op::Add) + op - 1), 0, -1);
                        checkBinary(BinOp(op - 1), current, vt, s->line, s->col);
                    } else {
                        compileExpr(s->b);
                    }

                    fs_->curLine = s->line;
                    emit(Op::SetIndex, 0, -2);
                } else {
                    error(t->line, t->col, "Cannot assign to this expression.");
                    return;
                }

                for(size_t i = wbs.size(); i-- > 0;) emit(wbs[i].op, wbs[i].arg, wbs[i].delta);
            }

            // Only fully known types are checked; unknown (Variant) values always stay dynamic.
            static bool known(SType t){ return t != kUnknown; }

            SType Compiler::checkBinary(BinOp op, SType a, SType b, int line, int col){
                if(!known(a) || !known(b)) return binaryResult(op, a, b);

                //formatting: checked at runtime
                if(op == BinOp::Mod && a == SType(Type::String)) return SType(Type::String);

                if(op == BinOp::Div && a == SType(Type::Int) && b == SType(Type::Int))
                    warning(line, col, "Integer division. Decimal part will be discarded.");
                Value x = sampleValue(Type(a)), y = sampleValue(Type(b)), r;
                std::string err;

                if(!binary_op(op, x, y, r, err)){
                    error(line, col, std::string("Invalid operands \"") + type_name(Type(a)) + "\" and \"" + type_name(Type(b)) +
                                         "\" for \"" + binopName(op) + "\" operator.");
                    return kUnknown;
                }

                return stypeOf(r);
            }

            SType Compiler::checkUnary(UnOp op, SType a, int line, int col){
                if(!known(a)) return op == UnOp::BitNot ? SType(Type::Int) : kUnknown;

                Value x = sampleValue(Type(a)), r;
                std::string err;

                if(!unary_op(op, x, r, err)){
                    error(line, col, std::string("Invalid operand of type \"") + type_name(Type(a)) + "\" for unary operator \"" +
                                         unopName(op) + "\".");
                    return kUnknown;
                }

                return stypeOf(r);
            }

            SType Compiler::checkIndex(SType base, SType index, int line, int col){
                if(!known(base)) return kUnknown;

                Type b = Type(base);

                switch(b){
                    case Type::Array:
                    case Type::Dictionary:
                    case Type::Object: return kUnknown;
                    case Type::String:
                    case Type::Vector3:
                    case Type::Vector2:
                    case Type::Color:
                        if(known(index) && index != SType(Type::Int) && index != SType(Type::String)){
                            error(line, col, std::string("Invalid index type \"") + type_name(Type(index)) + "\" for a base of type \"" +
                                                 type_name(b) + "\".");
                            return kUnknown;
                        }

                        if(b == Type::String) return index == SType(Type::Int) ? SType(Type::String) : kUnknown;

                        return SType(Type::Float);
                    default:
                        error(line, col, std::string("Cannot use subscript operator on a base of type \"") + type_name(b) + "\".");
                        return kUnknown;
                }
            }

            SType Compiler::checkMethod(Node* n, SType receiver){
                if(!known(receiver) || receiver == SType(Type::Object)) return kUnknown;

                const std::string& name = nm(n->name);
                Type t = Type(receiver);

                if(t == Type::Nil){
                    error(n->line, n->col, "Cannot call method \"" + name + "()\" on a null value.");
                    return kUnknown;
                }

                if(t == Type::Callable || t == Type::Signal){
                    int lo = 0, hi = 0;
                    SType ret = kUnknown;

                    if(!signalMethodInfo(findSignalMethod(name), t, lo, hi, ret)){
                        error(n->line, n->col, "Function \"" + name + "()\" not found in base " + type_name(t) + ".");
                        return kUnknown;
                    }

                    const int argc = int(n->list.size());

                    if(argc < lo || (hi >= 0 && argc > hi))
                        error(n->line, n->col, std::string("Invalid call to function \"") + name + "()\" in base " + type_name(t) + ". Expected " +
                                                   formatInt(argc < lo ? lo : hi) + " arguments.");
                    return ret;
                }

                int id = findBuiltinMethod(name);

                if(!builtinMethodExists(t, id)){
                    error(n->line, n->col, "Function \"" + name + "()\" not found in base " + type_name(t) + ".");
                    return kUnknown;
                }

                return builtinMethodReturn(t, id);
            }

            SType Compiler::checkFieldSet(Node* target, SType base, SType value, int op, int line, int col){
                (void)op;

                if(!known(base)) return kUnknown;

                Type b = Type(base);

                if(b == Type::Vector3 || b == Type::Vector2 || b == Type::Color){
                    int f = findBuiltinField(nm(target->name));
                    bool ok = b == Type::Vector3 ? (f >= 0 && f <= 2) : b == Type::Vector2 ? (f >= 0 && f <= 1) : f >= 3;

                    //reported by emitGetNamed / runtime
                    if(!ok) return kUnknown;

                    if(known(value) && value != SType(Type::Int) && value != SType(Type::Float))
                        error(line, col, std::string("Cannot assign a value of type \"") + type_name(Type(value)) + "\" as \"float\".");
                    return SType(Type::Float);
                }

                if(b != Type::Object && b != Type::Dictionary)
                    error(target->line, target->col, "Cannot find member \"" + nm(target->name) + "\" in base \"" + type_name(b) + "\".");
                return kUnknown;
            }

            void Compiler::checkPropAssign(const PropertyInfo* p, SType value, int line, int col){
                if(!known(value) || p->type == Type::Nil) return;

                Type v = Type(value), t = p->type;

                if(v == t) return;

                bool ok;

                switch(t){
                    case Type::Int:
                    case Type::Float:
                    case Type::Bool: ok = v == Type::Int || v == Type::Float || v == Type::Bool; break;
                    case Type::Object: ok = v == Type::Nil; break;
                    //x and y taken, for scripts written before Vector2
                    case Type::Vector2: ok = v == Type::Vector3; break;
                    default: ok = false; break;
                }

                if(!ok)
                    error(line, col, std::string("Cannot assign a value of type \"") + type_name(v) + "\" as \"" + type_name(t) + "\".");
            }

            void Compiler::checkCallArgs(Node* n, uint32_t fi, const std::vector<SType>* types){
                const std::vector<RType>& params = funcParams_[fi];

                for(size_t i = 0; i < types->size() && i < params.size(); ++i){
                    const RType& rt = params[i];
                    SType st = (*types)[i];

                    if(!rt.typed() || !known(st) || staticCompatible(st, rt)) continue;

                    std::string tn = rt.type == Type::Object ? rt.cls : std::string(type_name(rt.type));
                    error(n->list[i]->line, n->list[i]->col, "Invalid argument for \"" + nm(n->name) + "()\" function: argument " +
                                                                formatInt(Int(i + 1)) + " should be \"" + tn + "\" but is \"" +
                                                                type_name(Type(st)) + "\".");
                }
            }

            int Compiler::constTruth(Node* n){
                Value cv;

                if(!tryConst(n, cv)) return -1;

                return truthy(cv) ? 1 : 0;
            }

            // `local < constant` as a fused compare-and-jump.
            bool Compiler::ltLocalJump(Node* n, std::vector<size_t>& out){
                if(n->kind != NK::Binary || BinOp(n->op) != BinOp::Lt || n->a->kind != NK::Ident) return false;

                const Local* l = findLocalMut(n->a->name);
                Value k;

                if(!l || l->is_const || !tryConst(n->b, k) || !k.isNumber()) return false;

                size_t at = pc();
                emit(Op::LtLocalJump, l->slot, 0);
                emitWord(add_const(k));
                emitWord(0);
                out.push_back(at);
                return true;
            }

            void Compiler::condJump(Node* n, bool when, std::vector<size_t>& out){
                int truth = constTruth(n);

                if(truth >= 0){
                    if((truth == 1) == when) out.push_back(emitJump(Op::Jump, 0));

                    return;
                }

                fs_->curLine = n->line;

                if(n->kind == NK::Not){
                    condJump(n->a, !when, out);
                    return;
                }

                if(n->kind == NK::And || n->kind == NK::Or){
                    if((n->kind == NK::And) != when){
                        //and/false or or/true: either operand decides
                        condJump(n->a, when, out);
                        condJump(n->b, when, out);
                    } else {
                        std::vector<size_t> skip;
                        condJump(n->a, !when, skip);
                        condJump(n->b, when, out);
                        patchAll(skip);
                    }

                    return;
                }

                if(!when && ltLocalJump(n, out)) return;

                compileExpr(n);
                out.push_back(emitJump(when ? Op::JumpIfTrue : Op::JumpIfFalse, -1));
            }

            void Compiler::compileIf(const Stmt* s){
                std::vector<size_t> ends;
                std::vector<size_t> next;
                condJump(s->a, false, next);
                compileBlock(s->body);

                if(!s->elifs.empty() || !s->orelse.empty()) ends.push_back(emitJump(Op::Jump, 0));

                for(size_t i = 0; i < s->elifs.size(); ++i){
                    patchAll(next);
                    const Stmt* e = s->elifs[i];
                    fs_->curLine = e->line;
                    fs_->depth = 0;
                    condJump(e->a, false, next);
                    compileBlock(e->body);

                    if(i + 1 < s->elifs.size() || !s->orelse.empty()) ends.push_back(emitJump(Op::Jump, 0));
                }

                patchAll(next);

                if(!s->orelse.empty()) compileBlock(s->orelse);

                patchAll(ends);
            }

            void Compiler::compileWhile(const Stmt* s){
                size_t top = pc();
                std::vector<size_t> exits;
                condJump(s->a, false, exits);
                fs_->loops.push_back(LoopCtx{top, {}});
                compileBlock(s->body);
                fs_->curLine = s->line;
                emitJumpBack(top);
                patchAll(exits);
                patchAll(fs_->loops.back().breaks);
                fs_->loops.pop_back();
            }

            void Compiler::forBegin(const Stmt* s, size_t& top){
                pushScope();
                RType rt = resolveType(s->type);
                Node* it = s->a;
                bool range = false;
                uint32_t argc = 0;

                if(it && it->kind == NK::Call && nm(it->name) == "range" && isBuiltinCall(it->name) && !it->list.empty() &&
                    it->list.size() <= 3){
                    range = true;
                    argc = uint32_t(it->list.size());

                    for(Node* a : it->list) compileExpr(a);
                } else {
                    SType st = compileExpr(it);

                    if(st == SType(Type::Int)){
                        range = true;
                        argc = 1;
                    } else if(st != kUnknown && st != SType(Type::Array) && st != SType(Type::Dictionary) && st != SType(Type::String)){
                        error(it->line, it->col, std::string("Unable to iterate on value of type \"") + type_name(Type(st)) + "\".");
                    }
                }

                fs_->curLine = s->line;
                uint32_t state;

                if(range){
                    state = allocHidden(3);
                    emit(Op::RangePrep, state | (argc << 16), -int(argc));
                } else {
                    state = allocHidden(2);
                    emit(Op::ForPrep, state, -1);
                }

                Local& var = declareLocal(s->name, s->line, s->col);
                var.rt = rt;
                var.stype = rt.typed() ? SType(rt.type) : (range ? SType(Type::Int) : kUnknown);
                uint32_t varSlot = var.slot;
                top = pc();
                emit(range ? Op::RangeNext : Op::ForNext, state, 0);
                emitWord(varSlot);
                //exit offset, patched by forEnd
                emitWord(0);

                if(rt.typed()){
                    emit(Op::LoadLocal, varSlot, 1);
                    emitConvert(rt, kConvAssign, 0, range ? SType(Type::Int) : kUnknown, s->line, s->col,
                                 "variable \"" + nm(s->name) + "\"");
                    emit(Op::StoreLocal, varSlot, -1);
                }

                fs_->loops.push_back(LoopCtx{top, {}});
            }

            void Compiler::forEnd(const Stmt* s, size_t top){
                fs_->curLine = s->line;
                emitJumpBack(top);
                fs_->fd->code[top + 2] = uint32_t(int32_t(pc()) - int32_t(top));
                patchAll(fs_->loops.back().breaks);
                fs_->loops.pop_back();
                popScope();
            }

            void Compiler::compileFor(const Stmt* s){
                size_t top = 0;
                forBegin(s, top);
                compileBlock(s->body);
                forEnd(s, top);
            }

            // Emits the tests of one match branch and returns the jump to the next one, or SIZE_MAX if a pattern always matches.
            // Bindings go into the current scope.
            size_t Compiler::matchPatterns(const MatchBranch& br, uint32_t tmp, bool& unconditional){
                std::vector<size_t> toBody;
                unconditional = false;

                for(Node* p : br.patterns){
                    if(!p) continue;

                    fs_->curLine = p->line;
                    fs_->depth = 0;

                    if(p->kind == NK::Wildcard){
                        toBody.push_back(emitJump(Op::Jump, 0));
                        unconditional = true;
                        break;
                    }

                    if(p->kind == NK::Binding){
                        if(br.patterns.size() > 1) error(p->line, p->col, "Cannot use a variable bind with multiple patterns.");

                        Local& l = declareLocal(p->name, p->line, p->col);
                        emit(Op::LoadLocal, tmp, 1);
                        emit(Op::StoreLocal, l.slot, -1);
                        toBody.push_back(emitJump(Op::Jump, 0));
                        unconditional = true;
                        break;
                    }

                    emit(Op::LoadLocal, tmp, 1);
                    compileExpr(p);
                    fs_->curLine = p->line;
                    emit(Op::MatchEq, 0, -1);
                    toBody.push_back(emitJump(Op::JumpIfTrue, -1));
                }

                size_t skip = SIZE_MAX;

                if(!unconditional) skip = emitJump(Op::Jump, 0);

                patchAll(toBody);
                return skip;
            }

            void Compiler::compileMatch(const Stmt* s){
                pushScope();
                compileExpr(s->a);
                uint32_t tmp = allocHidden(1);
                emit(Op::StoreLocal, tmp, -1);
                std::vector<size_t> ends;

                for(const MatchBranch& br : s->branches){
                    pushScope();
                    bool unconditional = false;
                    size_t skip = matchPatterns(br, tmp, unconditional);
                    compileBlock(br.body);
                    ends.push_back(emitJump(Op::Jump, 0));

                    if(skip != SIZE_MAX) patch(skip);

                    popScope();
                }

                patchAll(ends);
                popScope();
            }

            void Compiler::compileReturn(const Stmt* s){
                if(!s->a){
                    if(fs_->ret.typed())
                        error(s->line, s->col, "A function with return type \"" +
                                                   std::string(fs_->ret.type == Type::Object ? fs_->ret.cls : type_name(fs_->ret.type)) +
                                                   "\" must return a value.");
                    emit(Op::ReturnNil, 0, 0);
                    return;
                }

                if(fs_->ret.is_void){
                    error(s->line, s->col, "A void function cannot return a value.");
                    return;
                }

                SType st = compileExpr(s->a);
                fs_->curLine = s->line;
                emitConvert(fs_->ret, kConvReturn, 0, st, s->line, s->col, "return value");
                emit(Op::Return, 0, -1);
            }

            bool Compiler::emitFolded(Node* n, SType& type){
                Value cv;

                if(!tryConst(n, cv)) return false;

                emitConst(cv);
                type = stypeOf(cv);
                return true;
            }

            SType Compiler::emitLiteral(Node* n){
                emitConst(n->value);
                return stypeOf(n->value);
            }

            SType Compiler::emitNilError(Node* n){
                if(n && (n->kind == NK::Wildcard || n->kind == NK::Binding))
                    error(n->line, n->col, "Patterns can only be used in \"match\" branches.");
                emit(Op::PushNil, 0, 1);
                return kUnknown;
            }

            SType Compiler::compileExpr(Node* n){
                if(!n || n->kind == NK::Bad) return emitNilError(n);

                fs_->curLine = n->line;

                if(n->kind != NK::Literal && n->kind != NK::Ident && n->cstate != 2){
                    SType t = kUnknown;

                    if(emitFolded(n, t)) return t;
                }

                switch(n->kind){
                    case NK::Literal: return emitLiteral(n);
                    case NK::Ident: return compileIdent(n);
                    case NK::Self: emit(Op::PushSelf, 0, 1); return SType(Type::Object);
                    case NK::Unary:{
                        SType t = compileExpr(n->a);
                        fs_->curLine = n->line;
                        UnOp op = UnOp(n->op);
                        emit(op == UnOp::Neg ? Op::Neg : (op == UnOp::Pos ? Op::Pos : Op::BitNot), 0, 0);
                        return checkUnary(op, t, n->line, n->col);
                    }
                    case NK::Not:
                        compileExpr(n->a);
                        emit(Op::Not, 0, 0);
                        return SType(Type::Bool);
                    case NK::Binary:{
                        SType ta = compileExpr(n->a);
                        SType tb = compileExpr(n->b);
                        fs_->curLine = n->line;
                        emit(Op(int(Op::Add) + n->op), 0, -1);
                        return checkBinary(BinOp(n->op), ta, tb, n->line, n->col);
                    }
                    case NK::And:
                    case NK::Or: return compileLogic(n);
                    case NK::Ternary: return compileTernary(n);
                    case NK::Is:
                    case NK::As:
                        compileExpr(n->a);
                        return emitIsAs(n);
                    case NK::Member: return compileMember(n);
                    case NK::Index:{
                        SType bt = compileExpr(n->a);
                        SType it = compileExpr(n->b);
                        fs_->curLine = n->line;
                        emit(Op::GetIndex, 0, -1);
                        return checkIndex(bt, it, n->line, n->col);
                    }
                    case NK::Call: return compileCall(n);
                    case NK::MethodCall: return compileMethodCall(n);
                    case NK::ArrayLit:
                        for(Node* x : n->list) compileExpr(x);

                        fs_->curLine = n->line;
                        emit(Op::MakeArray, uint32_t(n->list.size()), 1 - int(n->list.size()));
                        return SType(Type::Array);
                    case NK::DictLit:
                        for(Node* x : n->list) compileExpr(x);

                        fs_->curLine = n->line;
                        emit(Op::MakeDict, uint32_t(n->list.size() / 2), 1 - int(n->list.size()));
                        return SType(Type::Dictionary);
                    case NK::GetNode: return emitGetNode(n);
                    default: return emitNilError(n);
                }
            }

            SType Compiler::compileLogic(Node* n){
                bool isAnd = n->kind == NK::And;
                int base = fs_->depth;
                compileExpr(n->a);
                size_t j1 = emitJump(isAnd ? Op::JumpIfFalse : Op::JumpIfTrue, -1);
                compileExpr(n->b);
                emit(Op::ToBool, 0, 0);
                size_t j2 = emitJump(Op::Jump, 0);
                patch(j1);
                fs_->depth = base;
                emit(isAnd ? Op::PushFalse : Op::PushTrue, 0, 1);
                patch(j2);
                return SType(Type::Bool);
            }

            SType Compiler::compileTernary(Node* n){
                int base = fs_->depth;
                std::vector<size_t> toElse;
                condJump(n->b, false, toElse);
                SType ta = compileExpr(n->a);
                size_t end = emitJump(Op::Jump, 0);
                patchAll(toElse);
                fs_->depth = base;
                SType tb = compileExpr(n->c);
                patch(end);
                return ta == tb ? ta : kUnknown;
            }

            SType Compiler::emitGetNode(Node* n){
                if(getNodeMid_ < 0 || !findMethod("get_node")){
                    error(n->line, n->col, "\"$\" requires the base class to have a get_node() method.");
                    emit(Op::PushNil, 0, 1);
                    return kUnknown;
                }

                emitConst(n->value);
                emit(Op::CallSelf, 1, 0);
                emitWord(nameEntry("get_node"));
                return SType(Type::Object);
            }

            SType Compiler::compileIdent(Node* n){
                uint32_t name = n->name;

                if(const Local* l = findLocalMut(name)){
                    if(l->is_const){
                        emitConst(l->cval);
                        return stypeOf(l->cval);
                    }

                    emit(Op::LoadLocal, l->slot, 1);
                    return l->stype;
                }

                auto it = syms_.find(name);

                if(it != syms_.end()){
                    switch(it->second.kind){
                        case CS::Var:
                            emit(Op::LoadMember, it->second.index, 1);
                            return varStype_[it->second.index];
                        case CS::Const:{
                            Value v;

                            if(!resolveConst(it->second.index, v)){
                                emit(Op::PushNil, 0, 1);
                                return kUnknown;
                            }

                            emitConst(v);
                            return stypeOf(v);
                        }
                        case CS::Enum:
                            emitConst(enums_[it->second.index].dict);
                            return SType(Type::Dictionary);
                        case CS::Func:
                            emitBound(nm(name), false);
                            return SType(Type::Callable);
                        case CS::Signal:
                            emitBound(nm(name), true);
                            return SType(Type::Signal);
                    }
                }

                const std::string& s = nm(name);

                if(const PropertyInfo* p = findProp(s)){
                    uint32_t ne = nameEntry(p->name);

                    if(sd_->names[ne].pid >= 0){
                        emit(Op::LoadSelfProp, ne, 1);
                    } else {
                        emit(Op::PushSelf, 0, 1);
                        emit(Op::GetNamed, ne, 0);
                    }

                    return p->type == Type::Nil ? kUnknown : SType(p->type);
                }

                if(findMethod(s)){
                    emitBound(s, false);
                    return SType(Type::Callable);
                }

                if(findSignal(s)){
                    emitBound(s, true);
                    return SType(Type::Signal);
                }

                Value cv;

                if(builtinGlobalConstant(s, cv)){
                    emitConst(cv);
                    return stypeOf(cv);
                }

                if(host_.singleton(s) != 0){
                    emit(Op::LoadSingleton, nameEntry(s), 1);
                    return SType(Type::Object);
                }

                Type t;
                bool variant = false;

                if(builtinTypeFromName(s, t, variant) || class_exists(s)){
                    error(n->line, n->col, "\"" + s + "\" is a type and can not be used as a value.");
                    emit(Op::PushNil, 0, 1);
                    return kUnknown;
                }

                if(findBuiltinFunc(s) >= 0){
                    error(n->line, n->col, "Built-in functions can not be used as Callables yet in JetScript; call \"" + s + "()\" instead.");
                    emit(Op::PushNil, 0, 1);
                    return kUnknown;
                }

                error(n->line, n->col, "Identifier \"" + s + "\" not declared in the current scope.");
                emit(Op::PushNil, 0, 1);
                return kUnknown;
            }

            // Self members/properties, enum values and builtin type constants do not need the base evaluated.
            // Returns true when it emitted code.
            bool Compiler::memberStatic(Node* n, SType& type){
                Node* b = n->a;
                const std::string& mname = nm(n->name);

                if(b->kind == NK::Self){
                    auto it = syms_.find(n->name);

                    if(it != syms_.end()){
                        if(it->second.kind == CS::Var){
                            emit(Op::LoadMember, it->second.index, 1);
                            type = varStype_[it->second.index];
                            return true;
                        }

                        Node tmp;
                        tmp.kind = NK::Ident;
                        tmp.name = n->name;
                        tmp.line = n->line;
                        tmp.col = n->col;
                        type = compileIdent(&tmp);
                        return true;
                    }

                    if(const PropertyInfo* p = findProp(mname)){
                        uint32_t ne = nameEntry(p->name);

                        if(sd_->names[ne].pid >= 0){
                            emit(Op::LoadSelfProp, ne, 1);
                            type = p->type == Type::Nil ? kUnknown : SType(p->type);
                            return true;
                        }
                    }

                    if(findMethod(mname) || findSignal(mname)){
                        const bool signal = !findMethod(mname);
                        emitBound(mname, signal);
                        type = SType(signal ? Type::Signal : Type::Callable);
                        return true;
                    }

                    return false;
                }

                if(b->kind != NK::Ident || findLocal(b->name)) return false;

                auto it = syms_.find(b->name);

                if(it != syms_.end() && it->second.kind == CS::Enum){
                    for(const auto& kv : enums_[it->second.index].values)
                        if(kv.first == n->name){
                            emitConst(Value(kv.second));
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

                if(it == syms_.end() && namesType(b->name, t, is_class)){
                    Value cv;
                    type = kUnknown;

                    if((!is_class && builtinTypeConstant(t, mname, cv)) || (is_class && classConstant(nm(b->name), mname, cv))){
                        emitConst(cv);
                        type = stypeOf(cv);
                        return true;
                    }

                    if(is_class)
                        error(n->line, n->col, "Static access on engine classes (\"" + nm(b->name) + "." + mname +
                                                   "\") is not supported yet in JetScript.");
                    else
                        error(n->line, n->col, "Cannot find member \"" + mname + "\" in base \"" + nm(b->name) + "\".");
                    emit(Op::PushNil, 0, 1);
                    return true;
                }

                return false;
            }

            SType Compiler::emitGetNamed(Node* n, SType bt){
                fs_->curLine = n->line;
                const std::string& mname = nm(n->name);
                uint32_t ne = nameEntry(mname);
                emit(Op::GetNamed, ne, 0);
                int f = sd_->names[ne].field;

                if((bt == SType(Type::Vector3) && f >= 0 && f <= 2) || (bt == SType(Type::Vector2) && f >= 0 && f <= 1) ||
                    (bt == SType(Type::Color) && f >= 3))
                    return SType(Type::Float);

                if(bt != kUnknown && bt != SType(Type::Object) && bt != SType(Type::Dictionary))
                    error(n->line, n->col, "Cannot find member \"" + mname + "\" in base \"" + type_name(Type(bt)) + "\".");
                return kUnknown;
            }

            SType Compiler::compileMember(Node* n){
                SType t = kUnknown;

                if(memberStatic(n, t)) return t;

                if(n->a->kind == NK::Self){
                    emit(Op::PushSelf, 0, 1);
                    return emitGetNamed(n, kUnknown);
                }

                SType bt = compileExpr(n->a);
                return emitGetNamed(n, bt);
            }

            // Looks `name(args)` up as script function, then base class method, then builtin, and fills the instruction words.
            // Returns false after reporting an error.
            bool Compiler::planCall(Node* n, uint32_t& word0, uint32_t& word1, SType& ret, uint8_t& retmode){
                const std::string& name = nm(n->name);
                int argc = int(n->list.size());
                ret = kUnknown;
                retmode = 0;
                auto arity = [&](int lo, int hi){
                    if(argc < lo){
                        error(n->line, n->col, "Too few arguments for \"" + name + "()\" call. Expected at least " + formatInt(lo) +
                                                   " but received " + formatInt(argc) + ".");
                        return false;
                    }

                    if(hi >= 0 && argc > hi){
                        error(n->line, n->col, "Too many arguments for \"" + name + "()\" call. Expected at most " + formatInt(hi) +
                                                   " but received " + formatInt(argc) + ".");
                        return false;
                    }

                    return true;
                };

                if(findLocal(n->name)){
                    error(n->line, n->col, "\"" + name + "\" is a variable and can not be called; a Callable is called with \"" + name + ".call()\".");
                    return false;
                }

                auto it = syms_.find(n->name);

                if(it != syms_.end()){
                    if(it->second.kind == CS::Signal){
                        error(n->line, n->col, "\"" + name + "\" is a signal: emit it with \"" + name + ".emit()\".");
                        return false;
                    }

                    if(it->second.kind != CS::Func){
                        error(n->line, n->col, "\"" + name + "\" is not a function and can not be called.");
                        return false;
                    }

                    uint32_t fi = it->second.index;

                    if(!arity(funcNreq_[fi], int(ast_.funcs[fi].params.size()))) return false;

                    word0 = enc(Op::CallScript, uint32_t(argc));
                    word1 = fi;
                    const RType& r = funcRet_[fi];
                    ret = r.typed() ? SType(r.type) : kUnknown;
                    return true;
                }

                //connect(), emit_signal()... run by the VM on any object
                int lo = 0, hi = 0;
                SType sret = kUnknown;

                if(signalMethodInfo(findSignalMethod(name), Type::Object, lo, hi, sret)){
                    if(!arity(lo, hi)) return false;

                    word0 = enc(Op::CallSelf, uint32_t(argc));
                    word1 = nameEntry(name);
                    ret = sret;
                    return true;
                }

                if(const MethodInfo* m = findMethod(name)){
                    if(!arity(m->min_args, m->max_args)) return false;

                    uint32_t ne = nameEntry(name);

                    if(sd_->names[ne].mid < 0){
                        error(n->line, n->col, "Function \"" + name + "()\" is not available from the engine.");
                        return false;
                    }

                    word0 = enc(Op::CallSelf, uint32_t(argc));
                    word1 = ne;
                    ret = m->returnType == Type::Nil ? kUnknown : SType(m->returnType);
                    return true;
                }

                int id = name == "String" ? findBuiltinFunc("str") : findBuiltinFunc(name);

                if(id >= 0){
                    const BuiltinFunc& bf = builtinFuncs()[id];

                    if(name == "String" ? !arity(1, 1) : !arity(bf.min_args, bf.max_args)) return false;

                    if(name == "Vector3" && argc != 0 && argc != 1 && argc != 3){
                        error(n->line, n->col, "No constructor of \"Vector3\" matches the given arguments (expected (), (Vector3) or (x, y, z)).");
                        return false;
                    }

                    word0 = enc(Op::CallBuiltin, uint32_t(argc) | (uint32_t(id) << 8));
                    word1 = UINT32_MAX;
                    ret = bf.ret;
                    retmode = (bf.flags & kRetFirst) ? (name == "lerp" ? 3 : 1) : ((bf.flags & kRetNumeric) ? 2 : 0);
                    return true;
                }

                if(class_exists(name)){
                    error(n->line, n->col, "Creating engine objects (\"" + name + "\") is not supported yet in JetScript.");
                    return false;
                }

                error(n->line, n->col, "Function \"" + name + "()\" not found in base self.");
                return false;
            }

            SType Compiler::compileCall(Node* n){
                uint32_t w0 = 0, w1 = 0;
                SType ret = kUnknown;
                uint8_t retmode = 0;

                if(!planCall(n, w0, w1, ret, retmode)){
                    emit(Op::PushNil, 0, 1);
                    return kUnknown;
                }

                SType first = kUnknown, numeric = SType(Type::Int);
                //argument types for typed script function parameters
                std::vector<SType>* types = nullptr;

                if(opOf(w0) == Op::CallScript && !funcParams_[w1].empty()) types = new std::vector<SType>();

                for(size_t i = 0; i < n->list.size(); ++i){
                    SType t = compileExpr(n->list[i]);

                    if(types) types->push_back(t);

                    if(i == 0) first = t;

                    if(t == SType(Type::Float) && numeric != kUnknown) numeric = SType(Type::Float);
                    else if(t != SType(Type::Int) && t != SType(Type::Float)) numeric = kUnknown;
                }

                if(types){
                    checkCallArgs(n, w1, types);
                    delete types;
                }

                fs_->curLine = n->line;
                int argc = int(n->list.size());
                emit(opOf(w0), argOf(w0), 1 - argc);

                if(w1 != UINT32_MAX) emitWord(w1);

                switch(retmode){
                    case 1: return first;
                    case 2: return numeric;
                    case 3: return first == SType(Type::Int) ? SType(Type::Float) : first;
                    default: return ret;
                }
            }

            // 0: dynamic call, 1: call on self resolved statically, 2: error emitted
            int Compiler::methodStatic(Node* n){
                Node* recv = n->a;
                const std::string& name = nm(n->name);

                if(recv->kind == NK::Self){
                    auto it = syms_.find(n->name);

                    if((it != syms_.end() && it->second.kind == CS::Func) || (it == syms_.end() && findMethod(name))) return 1;

                    return 0;
                }

                if(recv->kind == NK::Ident && !findLocal(recv->name) && !syms_.count(recv->name)){
                    Type t;
                    bool is_class = false;

                    if(namesType(recv->name, t, is_class)){
                        if(is_class)
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

            SType Compiler::compileMethodCall(Node* n){
                int kind = methodStatic(n);

                if(kind == 2) return kUnknown;

                if(kind == 1){
                    //self.f(...) == f(...): the node is reinterpreted as a plain call
                    NK saved = n->kind;
                    n->kind = NK::Call;
                    SType t = compileCall(n);
                    n->kind = saved;
                    return t;
                }

                SType rt = compileExpr(n->a);

                for(Node* a : n->list) compileExpr(a);

                fs_->curLine = n->line;
                int argc = int(n->list.size());
                emit(Op::CallNamed, uint32_t(argc), -argc);
                emitWord(nameEntry(nm(n->name)));
                return checkMethod(n, rt);
            }

            SType Compiler::emitIsAs(Node* n){
                fs_->curLine = n->line;
                const std::string& tn = nm(n->type.name);
                bool isAs = n->kind == NK::As;
                Type t;
                bool variant = false;
                SType result = SType(Type::Bool);

                if(!n->type.present){
                    //reported by the parser
                    return kUnknown;
                } else if(builtinTypeFromName(tn, t, variant) && !variant){
                    emit(isAs ? Op::AsType : Op::IsType, uint32_t(t), 0);

                    if(isAs) result = SType(t);
                } else if(!variant && class_exists(tn)){
                    emit(isAs ? Op::AsClass : Op::IsClass, nameEntry(tn), 0);

                    if(isAs) result = SType(Type::Object);
                } else {
                    auto it = syms_.find(n->type.name);

                    if(it != syms_.end() && it->second.kind == CS::Enum){
                        emit(isAs ? Op::AsType : Op::IsType, uint32_t(Type::Int), 0);

                        if(isAs) result = SType(Type::Int);
                    } else {
                        error(n->type.line, n->type.col, "Could not find type \"" + tn + "\" in the current scope.");
                        return kUnknown;
                    }
                }

                if(n->negate && !isAs) emit(Op::Not, 0, 0);

                return result;
            }

            void Compiler::compileInit(bool onready){
                FunctionData& fd = sd_->funcs[size_t(onready ? sd_->onreadyFn : sd_->initFn)];
                fd.name = onready ? "@implicit_ready" : "@implicit_new";
                beginFunction(fd, 1);
                fd.entries.push_back(0);
                pushScope();

                for(size_t i = 0; i < ast_.vars.size(); ++i){
                    const VarDecl& v = ast_.vars[i];

                    if(v.onready != onready || !v.init) continue;

                    fs_->curLine = v.line;
                    fs_->depth = 0;
                    SType st = compileExpr(v.init);
                    initStype_[i] = st;
                    fs_->curLine = v.line;
                    RType& rt = varTypes_[i];

                    if(rt.typed()) emitConvert(rt, kConvAssign, 0, st, v.line, v.col, "variable \"" + nm(v.name) + "\"");

                    if(v.infer && !rt.present){
                        if(st == SType(Type::Nil))
                            error(v.line, v.col, "Cannot infer the type of \"" + nm(v.name) + "\" variable because the value is \"null\".");

                        if(st != kUnknown && st != SType(Type::Nil) && st != SType(Type::Object)){
                            rt.present = true;
                            rt.type = Type(st);
                        }
                    }

                    if(varStype_[i] == kUnknown && rt.typed()) varStype_[i] = SType(rt.type);

                    emit(Op::StoreMember, uint32_t(i), -1);
                }

                emit(Op::ReturnNil, 0, 0);
                popScope();
                endFunction();
            }

            void Compiler::compileFunction(const FuncDecl& f, FunctionData& fd){
                beginFunction(fd, f.line);
                fd.name = nm(f.name);
                fd.nparams = uint16_t(f.params.size());
                size_t fi = size_t(&f - &ast_.funcs[0]);
                fd.nrequired = funcNreq_[fi];
                fs_->ret = funcRet_[fi];
                pushScope();
                std::vector<RType> ptypes;

                for(size_t k = 0; k < f.params.size(); ++k){
                    const Param& p = f.params[k];
                    RType rt = funcParams_[fi][k];

                    for(const Local& l : fs_->locals)
                        if(l.name == p.name) error(p.line, p.col, "Parameter \"" + nm(p.name) + "\" is declared more than once.");

                    Local l;
                    l.name = p.name;
                    l.slot = fs_->nextSlot++;
                    l.line = p.line;
                    l.rt = rt;
                    l.stype = rt.typed() ? SType(rt.type) : kUnknown;
                    fs_->locals.push_back(l);
                    ptypes.push_back(rt);
                }

                fs_->maxSlots = fs_->nextSlot;

                for(size_t k = fd.nrequired; k < f.params.size(); ++k){
                    fd.entries.push_back(uint32_t(pc()));
                    const Param& p = f.params[k];
                    fs_->curLine = p.line;
                    fs_->depth = 0;
                    SType st = compileExpr(p.def);

                    if(p.infer && !ptypes[k].present && st != kUnknown && st != SType(Type::Nil) && st != SType(Type::Object)){
                        ptypes[k].present = true;
                        ptypes[k].type = Type(st);
                        fs_->locals[k].rt = ptypes[k];
                        fs_->locals[k].stype = st;
                    }

                    emit(Op::StoreLocal, uint32_t(k), -1);
                }

                fd.entries.push_back(uint32_t(pc()));

                for(size_t k = 0; k < f.params.size(); ++k){
                    if(!ptypes[k].typed()) continue;

                    fs_->curLine = f.params[k].line;
                    fs_->depth = 0;
                    emit(Op::LoadLocal, uint32_t(k), 1);
                    emitConvert(ptypes[k], kConvArg, uint32_t(k), kUnknown, f.params[k].line, f.params[k].col,
                                 "parameter \"" + nm(f.params[k].name) + "\"");
                    emit(Op::StoreLocal, uint32_t(k), -1);
                }

                for(const Stmt* s : f.body) compileStmt(s);

                fs_->curLine = f.line;
                emit(Op::ReturnNil, 0, 0);
                popScope();
                endFunction();
            }

            void Compiler::buildMetadata(){
                for(const ClassItem& item : ast_.order){
                    switch(item.kind){
                        case ItemKind::Var:{
                            const VarDecl& v = ast_.vars[item.index];
                            const RType& rt = varTypes_[item.index];
                            MemberInfo mi;
                            mi.name = nm(v.name);
                            SType hint = varStype_[item.index] != kUnknown ? varStype_[item.index] : initStype_[item.index];
                            mi.type = rt.typed() ? rt.type : (hint == kUnknown ? Type::Nil : Type(hint));
                            mi.line = v.line;
                            mi.exported = v.exported;
                            mi.onready = v.onready;
                            sd_->members.push_back(mi);

                            if(v.exported){
                                ExportInfo ei;
                                ei.name = mi.name;
                                ei.line = v.line;
                                Value init;
                                bool constant = v.init && tryConst(v.init, init);
                                Type t = mi.type;

                                if(t == Type::Nil && constant) t = init.type();

                                ei.type = t;
                                bool exportable = t == Type::Bool || t == Type::Int || t == Type::Float || t == Type::String ||
                                                  t == Type::Vector3 || t == Type::Vector2 || t == Type::Color;

                                if(!exportable){
                                    if(t == Type::Nil)
                                        error(v.line, v.col, "Cannot infer the type of exported variable \"" + mi.name +
                                                                 "\": add a type annotation or a constant default value.");
                                    else
                                        error(v.line, v.col, std::string("Export type \"") +
                                                                 (t == Type::Object && !rt.cls.empty() ? rt.cls : std::string(type_name(t))) +
                                                                 "\" is not supported (exportable: bool, int, float, String, Vector2, Vector3, Color).");
                                }

                                if(constant && convertTo(t, init)) ei.default_value = init;
                                else ei.default_value = typeDefault(t);

                                if(v.hasRange){
                                    if(t != Type::Int && t != Type::Float)
                                        error(v.line, v.col, "\"@export_range\" can only be used on int or float variables.");
                                    Real vals[3] = {0, 0, t == Type::Int ? 1.0f : 0.001f};

                                    for(size_t k = 0; k < v.rangeArgs.size() && k < 3; ++k){
                                        Value a;

                                        if(!tryConst(v.rangeArgs[k], a) || !a.isNumber())
                                            error(v.line, v.col, "\"@export_range\" arguments must be constant numbers.");
                                        else vals[k] = a.asReal();
                                    }

                                    ei.hasRange = true;
                                    ei.min = vals[0];
                                    ei.max = vals[1];
                                    ei.step = vals[2];
                                }

                                sd_->exports.push_back(std::move(ei));
                            }
                            break;
                        }
                        case ItemKind::Const:{
                            const ConstDecl& c = ast_.consts[item.index];
                            MemberInfo mi;
                            mi.name = nm(c.name);
                            Value v;

                            if(resolveConst(uint32_t(item.index), v)) mi.type = v.type();

                            mi.line = c.line;
                            mi.constant = true;
                            sd_->members.push_back(mi);
                            sd_->consts.emplace_back(mi.name, v);
                            break;
                        }
                        case ItemKind::Enum:{
                            const EnumInfo& e = enums_[item.index];

                            if(e.name){
                                MemberInfo mi;
                                mi.name = nm(e.name);
                                mi.type = Type::Dictionary;
                                mi.line = e.line;
                                mi.constant = true;
                                sd_->members.push_back(mi);
                                sd_->consts.emplace_back(mi.name, e.dict);
                            }

                            for(size_t k = 0; k < e.values.size(); ++k){
                                MemberInfo mi;
                                mi.name = e.name ? nm(e.name) + "." + nm(e.values[k].first) : nm(e.values[k].first);
                                mi.type = Type::Int;
                                mi.line = ast_.enums[item.index].positions[k].first;
                                mi.constant = true;
                                sd_->members.push_back(mi);

                                if(!e.name) sd_->consts.emplace_back(mi.name, Value(e.values[k].second));
                            }
                            break;
                        }
                        case ItemKind::Func:{
                            const FuncDecl& f = ast_.funcs[item.index];
                            FunctionInfo fi;
                            fi.name = nm(f.name);
                            fi.line = f.line;

                            for(const Param& p : f.params) fi.arguments.push_back(nm(p.name));

                            sd_->functions.push_back(std::move(fi));
                            break;
                        }

                        //below, in declaration order
                        case ItemKind::Signal: break;
                    }
                }

                for(size_t i = 0; i < ast_.vars.size(); ++i){
                    VarData vd;
                    vd.name = nm(ast_.vars[i].name);
                    const RType& rt = varTypes_[i];
                    vd.typed = rt.typed();
                    vd.type = rt.typed() ? rt.type : Type::Nil;

                    if(rt.typed() && rt.type == Type::Object && rt.cls != "Object") vd.class_name = rt.cls;

                    if(vd.typed && !isContainer(typeDefault(vd.type))) vd.initial = typeDefault(vd.type);

                    sd_->vars.push_back(std::move(vd));
                }

                for(size_t i = 0; i < sd_->vars.size(); ++i) sd_->symbols.push_back(SymEntry{sd_->vars[i].name, SymKind::Var, uint32_t(i)});

                for(size_t i = 0; i < sd_->consts.size(); ++i) sd_->symbols.push_back(SymEntry{sd_->consts[i].first, SymKind::Const, uint32_t(i)});

                for(size_t i = 0; i < ast_.funcs.size(); ++i) sd_->symbols.push_back(SymEntry{nm(ast_.funcs[i].name), SymKind::Func, uint32_t(i)});

                for(size_t i = 0; i < ast_.signals.size(); ++i){
                    SignalInfo info;
                    info.name = nm(ast_.signals[i].name);

                    for(uint32_t a : ast_.signals[i].params) info.arguments.push_back(nm(a));

                    sd_->signals.push_back(std::move(info));
                    sd_->symbols.push_back(SymEntry{sd_->signals.back().name, SymKind::Signal, uint32_t(i)});
                }

                std::sort(sd_->symbols.begin(), sd_->symbols.end(), [](const SymEntry& a, const SymEntry& b){ return a.name < b.name; });
            }

            void Compiler::run(ScriptData& sd){
                sd_ = &sd;
                setupBase();
                registerSymbols();
                resolveEnums();

                for(uint32_t i = 0; i < uint32_t(ast_.consts.size()); ++i){
                    Value v;
                    resolveConst(i, v);
                }

                for(const VarDecl& v : ast_.vars){
                    RType rt = resolveType(v.type);

                    if(rt.is_void) error(v.type.line, v.type.col, "\"void\" can not be used as a variable type.");

                    varTypes_.push_back(rt);
                    varStype_.push_back(rt.typed() ? SType(rt.type) : kUnknown);
                    initStype_.push_back(kUnknown);
                }

                for(const FuncDecl& f : ast_.funcs){
                    funcRet_.push_back(resolveType(f.ret));
                    std::vector<RType> ptypes;

                    for(const Param& p : f.params){
                        ptypes.push_back(resolveType(p.type));

                        if(ptypes.back().is_void) error(p.line, p.col, "\"void\" can not be used as a parameter type.");
                    }

                    funcParams_.push_back(std::move(ptypes));
                    uint16_t nreq = uint16_t(f.params.size());
                    bool seenDefault = false;

                    for(size_t k = 0; k < f.params.size(); ++k){
                        if(f.params[k].def){
                            if(!seenDefault) nreq = uint16_t(k);

                            seenDefault = true;
                        } else if(seenDefault){
                            error(f.params[k].line, f.params[k].col, "Cannot have mandatory parameters after optional parameters.");
                        }
                    }

                    funcNreq_.push_back(nreq);

                    if(f.params.size() > 255) error(f.line, f.col, "Too many parameters (the maximum is 255).");
                }

                sd.funcs.resize(ast_.funcs.size() + 2);
                sd.initFn = int(ast_.funcs.size());
                sd.onreadyFn = int(ast_.funcs.size() + 1);
                compileInit(false);
                compileInit(true);

                for(size_t i = 0; i < ast_.funcs.size(); ++i) compileFunction(ast_.funcs[i], sd.funcs[i]);

                for(size_t i = 0; i < ast_.funcs.size(); ++i){
                    if(sd.funcs[i].name == "_ready") sd.readyFn = int(i);

                    if(sd.funcs[i].name == "_process") sd.processFn = int(i);
                }

                buildMetadata();
            }

        }
    }

    CompileResult compile(std::string_view source, std::string path, const Host& host){
        CompileResult result;
        detail::Ast ast;
        {
            detail::Parser parser(source, ast, result.errors);
            parser.parseScript();
        }

        if(!result.errors.empty()) return result;

        std::unique_ptr<ScriptData> sd(new ScriptData());
        sd->path = std::move(path);
        sd->serial = detail::nextScriptSerial();
        {
            detail::Compiler compiler(ast, host, result.errors, result.warnings);
            compiler.run(*sd);
        }

        if(!result.errors.empty()) return result;

        //plain `new` rather than make_shared, so the control block type is one users can name too.
        //UBSan's vptr check needs that across -fno-rtti code (see tests/script_tests.cpp).
        result.script = std::shared_ptr<const Script>(new Script(std::move(sd)));
        return result;
    }

}
