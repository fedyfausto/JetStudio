#pragma once
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// JetScript (docs/jetscript.md). No exceptions, RTTI or Godot types, so it builds for the editor, player and firmware.
namespace jetscript{

    using Int = int64_t;
    // ESP32-S3 has a single precision FPU.
    using Real = float;
    // Host object handle; 0 is never a valid object.
    using ObjectId = uint32_t;

    struct Vector3{ Real x = 0, y = 0, z = 0; };
    struct Vector2{ Real x = 0, y = 0; };
    struct Color{ Real r = 0, g = 0, b = 0; };

    enum class Type : uint8_t{ Nil, Bool, Int, Float, String, Vector3, Color, Array, Dictionary, Object, Vector2, Callable, Signal };
    const char* type_name(Type);

    namespace detail{ struct Heap; }

    // Cheap to copy: String, Array and Dictionary are refcounted, and containers are shared by reference like in GDScript.
    // Cycles (an array that contains itself) are never freed; they leak until broken, e.g. with clear().
    // Refcounts are not atomic, so use Values, Scripts and VMs from one thread only. VMs on that thread can share a Script.
    class Value{
        public:
            Value() = default;
            Value(bool);
            Value(int);
            Value(Int);
            Value(Real);
            Value(double);
            Value(const Vector3&);
            Value(const Vector2&);
            Value(const Color&);
            Value(const char*);
            Value(std::string_view);
            Value(const std::string&);
            static Value object(ObjectId);
            // Godot's Callable (an object's method) and Signal (an object's signal), by name.
            static Value callable(ObjectId, std::string_view method);
            static Value signal(ObjectId, std::string_view name);
            static Value newArray();
            static Value newDictionary();
            Value(const Value&);
            Value(Value&&) noexcept;
            Value& operator=(const Value&);
            Value& operator=(Value&&) noexcept;
            ~Value();

            Type type() const{ return type_; }
            bool isNil() const{ return type_ == Type::Nil; }
            bool isNumber() const{ return type_ == Type::Int || type_ == Type::Float; }
            // GDScript truthiness
            bool truthy() const;
            // false unless Bool
            bool asBool() const;
            // Int, or Float truncated; 0 otherwise
            Int asInt() const;
            // Float, or Int converted; 0 otherwise
            Real asReal() const;
            // zero vector unless Vector3
            Vector3 asVector3() const;
            // zero vector unless Vector2
            Vector2 asVector2() const;
            // black unless Color
            Color asColor() const;
            // empty unless String
            std::string_view asString() const;
            // 0 unless Object
            ObjectId asObject() const;
            // Callable and Signal: their object, 0 otherwise
            ObjectId boundObject() const;
            // and their method or signal name
            std::string_view boundName() const;

            // Array helpers for hosts; they do nothing on other types.
            size_t arraySize() const;
            Value arrayGet(size_t index) const;
            void arrayPush(const Value&);

            // same text as str(x); objects use Host::describe
            std::string to_string() const;
            // GDScript ==, with int/float numeric equality
            bool operator==(const Value&) const;

        private:
            friend struct detail::Heap;
            // 16 bytes on 32 and 64 bit targets: tag, one float (Float, Vector3.x, Color.r) and an 8 byte payload.
            // Payload: Int, bool, ObjectId, the other two vector/color components, or the heap object (null for an empty string).
            // Callable and Signal keep their object's id in the float's place and the name as a string in the payload.
            union Payload{ Int i; void* p; ObjectId o; bool b; Real f[2]; };
            Type type_ = Type::Nil;
            Real f_ = 0;
            Payload u_{};
    };

    struct PropertyInfo{ std::string name; Type type = Type::Nil; bool read_only = false; };
    // max_args < 0: variadic.
    struct MethodInfo{ std::string name; int min_args = 0, max_args = 0; Type returnType = Type::Nil; };
    struct SignalInfo{ std::string name; std::vector<std::string> arguments; };
    struct ClassInfo{
        // base is empty for the root class
        std::string name, base;
        std::vector<PropertyInfo> properties;
        std::vector<MethodInfo> methods;
        // the ones the engine emits
        std::vector<SignalInfo> signals;
        // Tween.TRANS_SINE...
        std::vector<std::pair<std::string, Int>> constants;
    };

    enum class Status : uint8_t{ Ok, NotFound, Error };

    class Host;
    // Whether the ClassInfo of the object's class or of one of its bases lists this signal (or method).
    bool classLists(const Host& host, ObjectId id, std::string_view name, bool signal);

    // Implemented by the engine, always called on the thread that runs the VM.
    class Host{
        public:
            virtual ~Host() = default;
            // Used by `extends`, `is` and the compile-time identifier checks.
            virtual const ClassInfo* findClass(std::string_view name) const = 0;
            // Empty when the object does not exist.
            virtual std::string_view classOf(ObjectId) const = 0;
            // Member name ids, the same for every class (-1 if unknown). The VM looks names up once, at compile/attach time.
            virtual int propertyId(std::string_view name) const = 0;
            virtual int methodId(std::string_view name) const = 0;
            // Global singletons such as "Input"; 0 when unknown.
            virtual ObjectId singleton(std::string_view name) const = 0;
            // Return NotFound when the object has no such member, Error (with a message) for bad values or arguments.
            virtual Status getProperty(ObjectId, int property, Value& out, std::string& error) = 0;
            virtual Status setProperty(ObjectId, int property, const Value& value, std::string& error) = 0;
            virtual Status callMethod(ObjectId, int method, const Value* args, int count,
                                       Value& result, std::string& error) = 0;
            // An engine object's own signals and methods (connect() and Callables check them). By default they are looked up
            // in the ClassInfo, which a host may build only for the compiler: a device host should answer from its own tables.
            virtual bool has_signal(ObjectId id, std::string_view name) const{ return classLists(*this, id, name, true); }
            virtual bool has_method(ObjectId id, std::string_view name) const{ return classLists(*this, id, name, false); }
            // Text for str(object), e.g. "Cube:<MeshInstance3D#3>".
            virtual std::string describe(ObjectId) const = 0;
            // load()/preload(): the resource at path as an object, 0 when there is none.
            virtual ObjectId resource(std::string_view path) const{ (void)path; return 0; }
            // print()/printerr(): one call per line, without the trailing newline.
            virtual void print(std::string_view text, bool isError) = 0;
    };

    struct Diagnostic{ int line = 0, column = 0; std::string message; };

    struct ExportInfo{
        std::string name;
        Type type = Type::Nil;
        Value default_value;
        bool hasRange = false;
        Real min = 0, max = 0, step = 0;
        int line = 0;
    };
    struct MemberInfo{ std::string name; Type type = Type::Nil; int line = 0; bool exported = false, onready = false, constant = false; };
    struct FunctionInfo{ std::string name; int line = 0; std::vector<std::string> arguments; };

    // see js_internal.hpp
    struct ScriptData;

    // Immutable, shared by every node that uses the same file.
    class Script{
        public:
            explicit Script(std::unique_ptr<ScriptData>);
            ~Script();
            const std::string& path() const;
            // "Node" when `extends` is omitted
            const std::string& baseClass() const;
            const std::vector<ExportInfo>& exports() const;
            // variables, constants and enum values
            const std::vector<MemberInfo>& members() const;
            const std::vector<FunctionInfo>& functions() const;
            const std::vector<SignalInfo>& signals() const;
            bool hasFunction(std::string_view name) const;
            const ScriptData& data() const{ return *data_; }
        private:
            std::unique_ptr<ScriptData> data_;
    };

    struct CompileResult{
        // null when errors is not empty
        std::shared_ptr<const Script> script;
        std::vector<Diagnostic> errors;
        std::vector<Diagnostic> warnings;
    };

    // `path` (e.g. "res://player.jet") is only used in diagnostics and runtime errors.
    // Bad input never crashes, and nesting depth is capped so compiling fits in the small stack of a microcontroller task.
#ifndef JETSCRIPT_NO_COMPILER
    CompileResult compile(std::string_view source, std::string path, const Host& host);
#endif

    // Little endian, versioned and CRC checked, so scripts compiled on the PC run on devices built without the compiler.
    // Those builds define JETSCRIPT_NO_COMPILER and leave out js_lexer, js_parser and js_compiler.
    std::vector<uint8_t> saveBytecode(const Script& script);
    // Only accepts bytecode from the same JetScript build; bad data just gives errors.
    // Host member ids are looked up again by name.
    CompileResult loadBytecode(const uint8_t* data, size_t size, const Host& host);

    struct StackFrame{ std::string path, function; int line = 0; };
    struct RuntimeError{
        std::string path, function, message;
        int line = 0;
        ObjectId object = 0;
        // innermost first
        std::vector<StackFrame> stack;
    };

    struct VMState;

    class VM{
        public:
            explicit VM(Host& host);
            ~VM();
            VM(const VM&) = delete;
            VM& operator=(const VM&) = delete;

            // Creates the script instance and runs the member initializers, except @onready ones.
            // The base class must be the object's class or an ancestor, otherwise it records a RuntimeError and returns false.
            bool attach(ObjectId object, std::shared_ptr<const Script> script);
            void detach(ObjectId object);
            void clear();
            bool hasInstance(ObjectId object) const;
            const Script* scriptOf(ObjectId object) const;

            // Script member variables by name (scene overrides of @export values, debugger).
            bool setMember(ObjectId object, std::string_view name, const Value& value);
            bool getMember(ObjectId object, std::string_view name, Value& out) const;

            // Runs @onready initializers, then _ready() if defined.
            bool ready(ObjectId object);
            // Calls _process(delta) if defined. Returns false on runtime error.
            bool process(ObjectId object, Real delta);
            // Errors are recorded; false if the value is not a Callable.
            bool callCallable(const Value& callable, const Value* args = nullptr, int count = 0, Value* result = nullptr);
            // Calls any script function. A missing function is not an error (returns true).
            bool call(ObjectId object, std::string_view function, const Value* args = nullptr,
                      int count = 0, Value* result = nullptr);

            // Godot's signals. Connections live here, not in the objects, so a signal nobody listens to costs one check.
            // flags: 0 or Godot's CONNECT_ONE_SHOT (4); CONNECT_PERSIST (2) is accepted and means nothing at run time.
            bool connect(ObjectId source, std::string_view signal, ObjectId target, std::string_view method, uint32_t flags,
                         std::string& error);
            bool is_connected(ObjectId source, std::string_view signal, ObjectId target, std::string_view method) const;
            bool has_connections(ObjectId source) const;
            // For the engine's own signals (an area's body_entered, a Timer's timeout): calls what is connected, in order.
            // The signal is not checked, and errors in the callees are recorded like those of any other call.
            void emit_signal(ObjectId source, std::string_view signal, const Value* args = nullptr, int count = 0);
            // Drops the connections from and to an object that no longer exists. clear() drops them all.
            void forget(ObjectId object);

            // Errors recorded since the last call, oldest first.
            std::vector<RuntimeError> takeErrors();
            bool hasErrors() const;

            // Instruction budget per top-level call (attach/ready/process/call), 5,000,000 by default; 0 disables it.
            // Only counted where execution can repeat: backward jumps (by loop length) and calls.
            void setInstructionLimit(uint32_t limit);
            // For randf()/randi(); tests use it to get repeatable results.
            void seed(uint64_t value);

        private:
            std::unique_ptr<VMState> state_;
    };

}
