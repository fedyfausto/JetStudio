#pragma once
// JetScript (docs/jetscript.md). No exceptions, RTTI or Godot types, so it builds for the editor, player and firmware.
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace jetscript {

using Int = int64_t;
using Real = float; // ESP32-S3 has a single precision FPU.
using ObjectId = uint32_t; // Host object handle; 0 is never a valid object.

struct Vector3 { Real x = 0, y = 0, z = 0; };
struct Color { Real r = 0, g = 0, b = 0; };

enum class Type : uint8_t { Nil, Bool, Int, Float, String, Vector3, Color, Array, Dictionary, Object };
const char* type_name(Type);

namespace detail { struct Heap; }

// Cheap to copy: String, Array and Dictionary are refcounted, and containers are shared by reference like in GDScript.
// Cycles (an array that contains itself) are never freed; they leak until broken, e.g. with clear().
// Refcounts aren't atomic, so use Values, Scripts and VMs from one thread only. VMs on that thread can share a Script.
class Value {
public:
    Value() = default;
    Value(bool);
    Value(int);
    Value(Int);
    Value(Real);
    Value(double);
    Value(const Vector3&);
    Value(const Color&);
    Value(const char*);
    Value(std::string_view);
    Value(const std::string&);
    static Value object(ObjectId);
    static Value new_array();
    static Value new_dictionary();
    Value(const Value&);
    Value(Value&&) noexcept;
    Value& operator=(const Value&);
    Value& operator=(Value&&) noexcept;
    ~Value();

    Type type() const { return type_; }
    bool is_nil() const { return type_ == Type::Nil; }
    bool is_number() const { return type_ == Type::Int || type_ == Type::Float; }
    bool truthy() const;              // GDScript truthiness
    bool as_bool() const;             // false unless Bool
    Int as_int() const;               // Int, or Float truncated; 0 otherwise
    Real as_real() const;             // Float, or Int converted; 0 otherwise
    Vector3 as_vector3() const;       // zero vector unless Vector3
    Color as_color() const;           // black unless Color
    std::string_view as_string() const; // empty unless String
    ObjectId as_object() const;       // 0 unless Object

    // Array helpers for hosts; they do nothing on other types.
    size_t array_size() const;
    Value array_get(size_t index) const;
    void array_push(const Value&);

    std::string to_string() const;    // same text as str(x); objects use Host::describe
    bool operator==(const Value&) const; // GDScript ==, with int/float numeric equality

private:
    friend struct detail::Heap;
    // 16 bytes on 32 and 64 bit targets: tag, one float (Float, Vector3.x, Color.r) and an 8 byte payload.
    // Payload: Int, bool, ObjectId, the other two vector/color components, or the heap object (null for an empty string).
    union Payload { Int i; void* p; ObjectId o; bool b; Real f[2]; };
    Type type_ = Type::Nil;
    Real f_ = 0;
    Payload u_{};
};

struct PropertyInfo { std::string name; Type type = Type::Nil; bool read_only = false; };
// max_args < 0: variadic.
struct MethodInfo { std::string name; int min_args = 0, max_args = 0; Type return_type = Type::Nil; };
struct ClassInfo {
    std::string name, base; // base is empty for the root class
    std::vector<PropertyInfo> properties;
    std::vector<MethodInfo> methods;
};

enum class Status : uint8_t { Ok, NotFound, Error };

// Implemented by the engine, always called on the thread that runs the VM.
class Host {
public:
    virtual ~Host() = default;
    // Used by `extends`, `is` and the compile-time identifier checks.
    virtual const ClassInfo* find_class(std::string_view name) const = 0;
    // Empty when the object doesn't exist.
    virtual std::string_view class_of(ObjectId) const = 0;
    // Member name ids, the same for every class (-1 if unknown). The VM looks names up once, at compile/attach time.
    virtual int property_id(std::string_view name) const = 0;
    virtual int method_id(std::string_view name) const = 0;
    // Global singletons such as "Input"; 0 when unknown.
    virtual ObjectId singleton(std::string_view name) const = 0;
    // Return NotFound when the object has no such member, Error (with a message) for bad values or arguments.
    virtual Status get_property(ObjectId, int property, Value& out, std::string& error) = 0;
    virtual Status set_property(ObjectId, int property, const Value& value, std::string& error) = 0;
    virtual Status call_method(ObjectId, int method, const Value* args, int count,
                               Value& result, std::string& error) = 0;
    // Text for str(object), e.g. "Cube:<MeshInstance3D#3>".
    virtual std::string describe(ObjectId) const = 0;
    // print()/printerr(): one call per line, without the trailing newline.
    virtual void print(std::string_view text, bool is_error) = 0;
};

struct Diagnostic { int line = 0, column = 0; std::string message; };

struct ExportInfo {
    std::string name;
    Type type = Type::Nil;
    Value default_value;
    bool has_range = false;
    Real min = 0, max = 0, step = 0;
    int line = 0;
};
struct MemberInfo { std::string name; Type type = Type::Nil; int line = 0; bool exported = false, onready = false, constant = false; };
struct FunctionInfo { std::string name; int line = 0; std::vector<std::string> arguments; };

struct ScriptData; // see js_internal.hpp

// Immutable, shared by every node that uses the same file.
class Script {
public:
    explicit Script(std::unique_ptr<ScriptData>);
    ~Script();
    const std::string& path() const;
    const std::string& base_class() const;       // "Node" when `extends` is omitted
    const std::vector<ExportInfo>& exports() const;
    const std::vector<MemberInfo>& members() const; // variables, constants and enum values
    const std::vector<FunctionInfo>& functions() const;
    bool has_function(std::string_view name) const;
    const ScriptData& data() const { return *data_; }
private:
    std::unique_ptr<ScriptData> data_;
};

struct CompileResult {
    std::shared_ptr<const Script> script; // null when errors is not empty
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
std::vector<uint8_t> save_bytecode(const Script& script);
// Only accepts bytecode from the same JetScript build; bad data just gives errors.
// Host member ids are looked up again by name.
CompileResult load_bytecode(const uint8_t* data, size_t size, const Host& host);

struct StackFrame { std::string path, function; int line = 0; };
struct RuntimeError {
    std::string path, function, message;
    int line = 0;
    ObjectId object = 0;
    std::vector<StackFrame> stack; // innermost first
};

struct VMState;

class VM {
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
    bool has_instance(ObjectId object) const;
    const Script* script_of(ObjectId object) const;

    // Script member variables by name (scene overrides of @export values, debugger).
    bool set_member(ObjectId object, std::string_view name, const Value& value);
    bool get_member(ObjectId object, std::string_view name, Value& out) const;

    // Runs @onready initializers, then _ready() if defined.
    bool ready(ObjectId object);
    // Calls _process(delta) if defined. Returns false on runtime error.
    bool process(ObjectId object, Real delta);
    // Calls any script function. A missing function is not an error (returns true).
    bool call(ObjectId object, std::string_view function, const Value* args = nullptr,
              int count = 0, Value* result = nullptr);

    // Errors recorded since the last call, oldest first.
    std::vector<RuntimeError> take_errors();
    bool has_errors() const;

    // Instruction budget per top-level call (attach/ready/process/call), 5,000,000 by default; 0 disables it.
    // Only counted where execution can repeat: backward jumps (by loop length) and calls.
    void set_instruction_limit(uint32_t limit);
    // For randf()/randi(); tests use it to get repeatable results.
    void seed(uint64_t value);

private:
    std::unique_ptr<VMState> state_;
};

} // namespace jetscript
