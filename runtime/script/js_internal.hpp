#pragma once
#include "jetscript.hpp"

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <new>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace jetscript{
    namespace detail{

        // parser: block + expression nesting
        constexpr int kMaxNesting = 64;
        // script -> script call frames
        constexpr int kMaxCallDepth = 128;
        // host -> VM nested entries
        constexpr int kMaxReentry = 8;
        // str() / == / duplicate() of nested containers
        constexpr int kMaxStringifyDepth = 16;
        // node budget for deep == and duplicate(true); sub-containers can be shared
        constexpr int kMaxDeepNodes = 100000;
        constexpr uint32_t kDefaultInstructionLimit = 5000000;
        // constant arrays/dictionaries in bytecode: deepest element the loader reads
        constexpr int kMaxValueDepth = 16;
        // diagnostics per compile
        constexpr int kMaxErrors = 32;
        constexpr uint32_t kMaxLocals = 4096;
#if defined(ESP_PLATFORM) || defined(ARDUINO)
        // a microcontroller runs out of RAM long before the desktop limits: a script error is better than an abort
        constexpr size_t kMaxStringBytes = 64u * 1024u;
        constexpr size_t kMaxArrayItems = 64u * 1024u;
        constexpr uint32_t kMaxStackValues = 1u << 14;
#else
        // also bounds str() of huge/shared structures
        constexpr size_t kMaxStringBytes = 1u * 1024u * 1024u;
        constexpr size_t kMaxArrayItems = 1u * 1024u * 1024u;
        // VM value stack hard cap
        constexpr uint32_t kMaxStackValues = 1u << 18;
#endif

        enum : uint8_t{ kStr = 0, kArr = 1, kDict = 2 };

        struct HeapObj{
            uint32_t rc = 1;
            uint8_t kind = kStr;
            // constant Arrays/Dictionaries
            bool read_only = false;
        };

        // Immutable UTF-8 string; characters follow the header, NUL terminated.
        struct StrObj : HeapObj{
            // bytes
            uint32_t len = 0;
            // code points
            uint32_t ncp = 0;
            // 0 = not computed yet
            uint32_t hash = 0;
            char* chars(){ return reinterpret_cast<char*>(this) + sizeof(StrObj); }
            const char* chars() const{ return reinterpret_cast<const char*>(this) + sizeof(StrObj); }
            std::string_view view() const{ return std::string_view(chars(), len); }
            bool ascii() const{ return ncp == len; }
        };

        struct ContainerObj : HeapObj{
            // deferred destruction list, so freeing nested containers does not recurse
            ContainerObj* nextDead = nullptr;
        };

        struct ArrObj : ContainerObj{
            std::vector<Value> items;
        };

        struct DictEntry{
            Value key, value;
        };

        // Keeps insertion order. Small dictionaries scan linearly, bigger ones use an open addressing index over `entries`.
        struct DictObj : ContainerObj{
            std::vector<DictEntry> entries;
            // 0 = empty, otherwise entry index + 1
            std::vector<uint32_t> table;
            int find(const Value& key) const;
            int findStr(std::string_view key) const;
            Value* lookup(const Value& key);
            void set(const Value& key, const Value& value);
            bool erase(const Value& key);
            void clear();
            void rebuild();
        };

        uint32_t hash_bytes(const char* data, size_t len);
        uint32_t hashValue(const Value& v);
        bool key_equal(const Value& a, const Value& b);

        // Raw access to Value internals for the hot paths.
        struct Heap{
            // String, Array, Dictionary, and the name of a Callable or Signal.
            static bool is_heap(Type t){ return ((0x1990u >> unsigned(t)) & 1u) != 0; }
            static Type type(const Value& v){ return v.type_; }
            static Int i(const Value& v){ return v.u_.i; }
            static Real f(const Value& v){ return v.f_; }
            static bool b(const Value& v){ return v.u_.b; }
            static ObjectId o(const Value& v){ return v.u_.o; }
            static void* p(const Value& v){ return v.u_.p; }
            static Vector3 vec(const Value& v){ Vector3 r; r.x = v.f_; r.y = v.u_.f[0]; r.z = v.u_.f[1]; return r; }
            static Vector2 vec2(const Value& v){ Vector2 r; r.x = v.f_; r.y = v.u_.f[0]; return r; }
            static Color col(const Value& v){ Color r; r.r = v.f_; r.g = v.u_.f[0]; r.b = v.u_.f[1]; return r; }
            static Real comp(const Value& v, int k){ return k == 0 ? v.f_ : v.u_.f[k - 1]; }
            static void setComp(Value& v, int k, Real x){ if(k == 0) v.f_ = x; else v.u_.f[k - 1] = x; }
            static StrObj* str(const Value& v){ return static_cast<StrObj*>(v.u_.p); }
            static bool isBound(Type t){ return t == Type::Callable || t == Type::Signal; }
            static ObjectId boundObject(const Value& v){
                ObjectId o;
                std::memcpy(&o, &v.f_, sizeof o);
                return o;
            }
            static std::string_view boundName(const Value& v){ return static_cast<StrObj*>(v.u_.p)->view(); }
            // `slot` must hold no heap reference; `name` is a non-empty String.
            static void initBound(Value& slot, Type t, ObjectId o, const Value& name){
                retain(name);
                slot.type_ = t;
                std::memcpy(&slot.f_, &o, sizeof o);
                slot.u_.p = name.u_.p;
            }
            static ArrObj* arr(const Value& v){ return static_cast<ArrObj*>(v.u_.p); }
            static DictObj* dict(const Value& v){ return static_cast<DictObj*>(v.u_.p); }
            static std::string_view sv(const Value& v){
                if(v.type_ != Type::String || !v.u_.p) return std::string_view();

                return static_cast<StrObj*>(v.u_.p)->view();
            }

            static void destroy(HeapObj* h);
            static void retain(const Value& v){
                if(is_heap(v.type_) && v.u_.p) ++static_cast<HeapObj*>(v.u_.p)->rc;
            }
            // Releases the payload and leaves the value Nil.
            static void release(Value& v){
                if(is_heap(v.type_) && v.u_.p){
                    HeapObj* h = static_cast<HeapObj*>(v.u_.p);

                    if(--h->rc == 0) destroy(h);
                }

                v.type_ = Type::Nil;
            }
            static void rawCopy(Value& d, const Value& s){ d.type_ = s.type_; d.f_ = s.f_; d.u_ = s.u_; }
            static void assign(Value& d, const Value& s){
                retain(s);
                HeapObj* old = (is_heap(d.type_) && d.u_.p) ? static_cast<HeapObj*>(d.u_.p) : nullptr;
                rawCopy(d, s);

                if(old && --old->rc == 0) destroy(old);
            }
            static void moveAssign(Value& d, Value& s){
                if(&d == &s) return;

                release(d);
                rawCopy(d, s);
                s.type_ = Type::Nil;
            }
            // `slot` must be Nil (VM stack invariant).
            static void pushCopy(Value& slot, const Value& s){ retain(s); rawCopy(slot, s); }
            static void pushMove(Value& slot, Value& s){ rawCopy(slot, s); s.type_ = Type::Nil; }

            static void setNil(Value& v){ release(v); }
            static void setBool(Value& v, bool x){ release(v); v.type_ = Type::Bool; v.u_.b = x; }
            static void setInt(Value& v, Int x){ release(v); v.type_ = Type::Int; v.u_.i = x; }
            static void setFloat(Value& v, Real x){ release(v); v.type_ = Type::Float; v.f_ = x; }
            static void setVec(Value& v, const Vector3& a){ release(v); v.type_ = Type::Vector3; v.f_ = a.x; v.u_.f[0] = a.y; v.u_.f[1] = a.z; }
            // A Vector2 keeps its z at 0, so code that treats vectors as three components works on it too.
            static void setVec2(Value& v, const Vector2& a){ release(v); v.type_ = Type::Vector2; v.f_ = a.x; v.u_.f[0] = a.y; v.u_.f[1] = 0; }
            static void setColor(Value& v, const Color& a){ release(v); v.type_ = Type::Color; v.f_ = a.r; v.u_.f[0] = a.g; v.u_.f[1] = a.b; }
            static void setObject(Value& v, ObjectId o){ release(v); v.type_ = Type::Object; v.u_.o = o; }
            // Adopts one reference of `h` (may be null for the empty string).
            static void setHeap(Value& v, Type t, HeapObj* h){ release(v); v.type_ = t; v.u_.p = h; }
            // Unchecked in-place updates of an already typed value.
            static void putInt(Value& v, Int x){ v.u_.i = x; }
            static void putFloat(Value& v, Real x){ v.f_ = x; }
            // Initializers for slots known to hold no heap reference (VM stack above sp, scalars).
            static void initBool(Value& v, bool x){ v.type_ = Type::Bool; v.u_.b = x; }
            static void initInt(Value& v, Int x){ v.type_ = Type::Int; v.u_.i = x; }
            static void initFloat(Value& v, Real x){ v.type_ = Type::Float; v.f_ = x; }
            static void initObject(Value& v, ObjectId o){ v.type_ = Type::Object; v.u_.o = o; }
            static bool is_scalar(const Value& v){ return !is_heap(v.type_) || !v.u_.p; }

            static Value make(Type t, HeapObj* h){ Value v; v.type_ = t; v.u_.p = h; return v; }
        };

        // null when len == 0
        StrObj* newString(const char* data, size_t len);
        // uninitialized characters, len > 0
        StrObj* allocString(size_t len);
        Value makeString(std::string_view s);
        Value makeArray(std::vector<Value>&& items);
        inline bool isStr(const Value& v){ return v.type() == Type::String; }
        uint32_t utf8Count(const char* s, size_t len);
        // byte offset of code point (clamped)
        size_t utf8Offset(std::string_view s, Int cpIndex);
        void utf8Append(std::string& out, uint32_t cp);

        // Ints keep GDScript's 64 bits, but the ESP32-S3 converts and divides only 32 bit ints in hardware:
        // 64 bit ones and doubles go through libgcc, so these take the 32 bit path whenever the value fits.
        inline bool fits32(Int v){ return v == Int(int32_t(v)); }
        inline Real intToReal(Int v){ return fits32(v) ? Real(int32_t(v)) : Real(v); }
        // saturating, NaN -> 0
        Int realToInt(double x);
        // same, without double
        Int realToInt(Real x);
        // Number as a float for comparisons: exact for floats and for ints up to 2^24, the float mantissa.
        inline bool exactReal(const Value& v, Real& out){
            if(v.type() == Type::Float){ out = Heap::f(v); return true; }
            const Int i = Heap::i(v);

            if(i < -(Int(1) << 24) || i > (Int(1) << 24)) return false;

            out = Real(int32_t(i));
            return true;
        }
        std::string formatInt(Int v);
        // Godot-like str(float): shortest round-trip, always a decimal part
        std::string formatReal(Real v);
        void appendFixed(std::string& out, double v, int decimals);
        void appendValue(std::string& out, const Value& v, const Host* host, int depth, bool quoted);
        std::string stringify(const Value& v, const Host* host);
        // host used by Value::to_string() for objects
        const Host* defaultHost();
        void setDefaultHost(const Host* h, bool set);

        enum class BinOp : uint8_t{
            Add, Sub, Mul, Div, Mod, Pow, Shl, Shr, BitAnd, BitOr, BitXor,
            Eq, Ne, Lt, Le, Gt, Ge, In, NotIn, Count
        };
        enum class UnOp : uint8_t{ Neg, Pos, BitNot, Not };
        const char* binopName(BinOp op);
        const char* unopName(UnOp op);
        bool binary_op(BinOp op, const Value& a, const Value& b, Value& out, std::string& err);
        bool unary_op(UnOp op, const Value& a, Value& out, std::string& err);
        bool valuesEqual(const Value& a, const Value& b, int depth = 0);
        bool valuesEqualBudget(const Value& a, const Value& b, int depth, int& budget);
        // match: same type and ==
        bool strictEqual(const Value& a, const Value& b);
        bool truthy(const Value& v);
        bool formatPercent(std::string_view fmt, const Value& args, std::string& out, std::string& err);
        // Typed variable conversion: int/float convert, other types must match.
        bool convertTo(Type t, Value& v);
        Value typeDefault(Type t);
        bool validDictKey(const Value& k);
        Value deepCopy(const Value& v, bool deep, int depth = 0);
        Value deepCopyBudget(const Value& v, bool deep, int depth, int& budget);

        struct Rng{
            uint64_t state = 0x853c49e6748fea9bULL;
            uint64_t inc = 0xda3e39cb94b95bdbULL;
            uint32_t next();
            void seed(uint64_t s);
            // [0, 1)
            double unit();
        };

        struct BuiltinCtx{
            // null while constant folding
            Host* host = nullptr;
            Rng* rng = nullptr;
            std::string error;
        };

        // Signals and Callables, run by the VM itself: Godot's Object methods for signals, then Signal's and Callable's.
        enum SignalMethod : int8_t{
            // on objects
            SM_connect, SM_disconnect, SM_is_connected, SM_emit_signal, SM_has_signal,
            SM_emit, SM_get_name, SM_get_object, SM_is_null, SM_call, SM_callv, SM_get_method, SM_is_valid,
            SM_COUNT
        };
        // -1 if none
        int findSignalMethod(std::string_view name);
        const char* signalMethodName(int id);

        // Static type tag used by the compiler: -1 unknown/Variant, otherwise a Type.
        using SType = int8_t;
        constexpr SType kUnknown = -1;

        enum : uint8_t{ kPure = 1, kRetNumeric = 2, kRetFirst = 4 };
        using BuiltinFn = bool (*)(BuiltinCtx&, const Value*, int, Value&);
        struct BuiltinFunc{
            const char* name;
            // max < 0: variadic
            int8_t min_args, max_args;
            SType ret;
            uint8_t flags;
            BuiltinFn fn;
        };
        const BuiltinFunc* builtinFuncs();
        int builtinFuncCount();
        int findBuiltinFunc(std::string_view name);

        // -1 if no builtin type has it
        int findBuiltinMethod(std::string_view name);
        const char* builtinMethodName(int id);
        // 0 ok, 1 no such method for this type, 2 error (ctx.error)
        int callBuiltinMethod(BuiltinCtx& ctx, int id, const Value& self, const Value* args, int argc, Value& out);
        // 0..2 xyz, 3..6 rgba, -1
        int findBuiltinField(std::string_view name);
        // For the compiler's type checks.
        bool builtinMethodExists(Type receiver, int id);
        // kUnknown when it depends on the arguments
        SType builtinMethodReturn(Type receiver, int id);
        // representative value of a type
        Value sampleValue(Type t);
        bool builtinTypeConstant(Type t, std::string_view name, Value& out);
        bool builtinGlobalConstant(std::string_view name, Value& out);
        // Builtin type names usable in annotations / `is` (int, float, ...). Returns false if unknown.
        bool builtinTypeFromName(std::string_view name, Type& out, bool& variant);
        bool parseColorString(std::string_view s, Color& out);
        // Arguments and result of a signal method on a receiver type (Object, Signal or Callable); false if it has none.
        bool signalMethodInfo(int id, Type receiver, int& min_args, int& max_args, SType& ret);

    }

    namespace detail{

        enum class Op : uint8_t{
            Nop,
            PushNil, PushTrue, PushFalse, PushInt, PushConst, PushSelf,
            Pop, Dup, Dup2,
            LoadLocal, StoreLocal,
            LoadMember, StoreMember,
            LoadSelfProp, StoreSelfProp, WbSelfProp,
            LoadSingleton,
            GetNamed, SetNamed, WbNamed,
            GetIndex, SetIndex, WbIndex,
            // same order as BinOp
            Add, Sub, Mul, Div, Mod, Pow, Shl, Shr, BitAnd, BitOr, BitXor,
            Eq, Ne, Lt, Le, Gt, Ge, In, NotIn,
            Neg, Pos, BitNot, Not, ToBool,
            IsType, IsClass, AsType, AsClass, Convert, CheckClass,
            Jump, JumpIfFalse, JumpIfTrue,
            ForPrep, ForNext, RangePrep, RangeNext,
            CallScript, CallSelf, CallNamed, CallBuiltin,
            MakeArray, MakeDict,
            MatchEq,
            // a Callable (arg bit 0 clear) or Signal of self, named by const[arg >> 1]
            PushBound,
            Return, ReturnNil,
            // superinstructions
            // local[a] += imm (int fast path)  : word1 = imm
            AddLocalInt,
            // if !(local[a] < const[k]) jump   : word1 = k, word2 = offset
            LtLocalJump,
            Count
        };

        inline uint32_t enc(Op op, uint32_t a = 0){ return uint32_t(op) | (a << 8); }
        inline Op opOf(uint32_t w){ return Op(w & 0xFFu); }
        inline uint32_t argOf(uint32_t w){ return w >> 8; }
        inline int32_t sargOf(uint32_t w){ return int32_t((w >> 8) ^ 0x800000u) - 0x800000; }
        inline uint32_t senc(int32_t v){ return uint32_t(v) & 0xFFFFFFu; }

        // Convert operand: bits 0-7 Type, bits 8-9 context (0 assign, 1 argument, 2 return)
        enum : uint32_t{ kConvAssign = 0, kConvArg = 1, kConvReturn = 2 };

        struct NameEntry{
            std::string name;
            // Host::propertyId
            int pid = -1;
            // Host::methodId
            int mid = -1;
            // builtin field (x y z r g b a)
            int8_t field = -1;
            // builtin method id
            int16_t bmethod = -1;
            // signal method id (SM_...)
            int8_t smethod = -1;
        };

        struct LineEntry{ uint32_t pc; int32_t line; };

        struct FunctionData{
            std::string name;
            int line = 0;
            uint16_t nparams = 0, nrequired = 0;
            uint32_t nlocals = 0;
            // locals + operand stack
            uint32_t frameSize = 0;
            std::vector<uint32_t> code;
            // entries[argc - nrequired]: start pc
            std::vector<uint32_t> entries;
            std::vector<LineEntry> lines;
            int lineAt(uint32_t pc) const;
        };

        struct VarData{
            std::string name;
            Type type = Type::Nil;
            // int/float conversions and type checks on assignment
            bool typed = false;
            // engine class annotation (checked on assignment)
            std::string class_name;
            // value before initializers run
            Value initial;
        };

        enum class SymKind : uint8_t{ Var, Const, Func, Signal };
        struct SymEntry{ std::string name; SymKind kind; uint32_t index; };

    }

    struct ScriptData{
        std::string path;
        std::string baseClass = "Node";
        std::vector<ExportInfo> exports;
        std::vector<MemberInfo> members;
        std::vector<FunctionInfo> functions;
        std::vector<SignalInfo> signals;

        // user functions, then hidden initializers
        std::vector<detail::FunctionData> funcs;
        std::vector<detail::VarData> vars;
        std::vector<Value> constants;
        // named constants (dynamic access)
        std::vector<std::pair<std::string, Value>> consts;
        // member names used by dynamic access
        std::vector<detail::NameEntry> names;
        // sorted by name: vars, consts, functions, signals
        std::vector<detail::SymEntry> symbols;
        int initFn = -1, onreadyFn = -1, readyFn = -1, processFn = -1;
        uint32_t serial = 0;

        const detail::SymEntry* findSymbol(std::string_view name) const;
    };

    namespace detail{
        // Unique id of each loaded ScriptData (VM caches are keyed by it).
        uint32_t nextScriptSerial();
    }

}
