// JetScript compiler and VM tests against a mock host (Node, Node3D, MeshInstance3D, Camera3D, Input).
#include "jetscript.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <functional>
#include <iostream>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#if defined(__linux__)
#include <pthread.h>
#endif

#if defined(__SANITIZE_ADDRESS__)
#define JS_TEST_ASAN 1
#elif defined(__has_feature)
#if __has_feature(address_sanitizer)
#define JS_TEST_ASAN 1
#endif
#endif

using namespace jetscript;

// jet_script is built with -fno-rtti, so its shared_ptr<const Script> control blocks have vtables without typeinfo.
// UBSan's vptr check (-fsanitize=undefined) would reject them.
// Instantiating the same control block here (RTTI on, linked first) makes the linker keep this TU's vtable for both.
static const std::shared_ptr<const Script> g_rtti_anchor(static_cast<Script*>(nullptr));

// ------------------------------------------------------------------ check helpers

static int g_checks = 0, g_failures = 0;

#define CHECK(cond)                                                                       \
    do {                                                                                  \
        ++g_checks;                                                                       \
        if (!(cond)) {                                                                    \
            ++g_failures;                                                                 \
            std::cerr << __FILE__ << ":" << __LINE__ << ": CHECK failed: " #cond << "\n"; \
        }                                                                                 \
    } while (0)

#define CHECK_EQ(a, b)                                                                                   \
    do {                                                                                                 \
        ++g_checks;                                                                                      \
        auto va_ = (a);                                                                                  \
        auto vb_ = (b);                                                                                  \
        if (!(va_ == vb_)) {                                                                             \
            ++g_failures;                                                                                \
            std::cerr << __FILE__ << ":" << __LINE__ << ": CHECK_EQ failed: " #a " == " #b "\n  left:  " \
                      << va_ << "\n  right: " << vb_ << "\n";                                           \
        }                                                                                                \
    } while (0)

static bool contains(const std::string& s, const std::string& part) { return s.find(part) != std::string::npos; }
static bool is_equal(Real a, Real b) { return std::fabs(a - b) < 1e-4f * (1 + std::fabs(b)); }

// ------------------------------------------------------------------ mock host

struct MockObject {
    std::string cls, name;
    ObjectId parent = 0;
    std::vector<ObjectId> children;
    Vector3 position, rotation, scale{1, 1, 1}, look_target;
    bool visible = true, current = false, queued = false;
    Color color{1, 1, 1};
    Real fov = 75, near_ = 0.05f, far_ = 4000;
};

class MockHost : public Host {
public:
    std::map<std::string, ClassInfo> classes;
    std::vector<std::string> props, methods;
    std::map<ObjectId, MockObject> objects;
    ObjectId next_id = 1;
    ObjectId input = 0;
    ObjectId root = 0;
    std::vector<std::string> out, err_out;
    std::set<std::string> pressed;
    VM* vm = nullptr;
    int get_property_calls = 0, set_property_calls = 0;

    MockHost() {
        props = {"name", "position", "rotation", "rotation_degrees", "scale", "visible", "global_position",
                 "color", "fov", "near", "far", "current"};
        methods = {"get_node", "get_node_or_null", "get_parent", "get_children", "get_child", "get_child_count",
                   "find_child", "queue_free", "translate", "rotate_x", "rotate_y", "rotate_z", "look_at", "show",
                   "hide", "is_visible_in_tree", "make_current", "is_action_pressed", "is_action_just_pressed",
                   "is_action_just_released", "get_axis", "fail_method", "detach_self", "call_script", "sum"};
        ClassInfo node;
        node.name = "Node";
        node.properties = {{"name", Type::String, true}};
        node.methods = {{"get_node", 1, 1, Type::Object},        {"get_node_or_null", 1, 1, Type::Object},
                        {"get_parent", 0, 0, Type::Object},      {"get_children", 0, 0, Type::Array},
                        {"get_child", 1, 1, Type::Object},       {"get_child_count", 0, 0, Type::Int},
                        {"find_child", 1, 1, Type::Object},      {"queue_free", 0, 0, Type::Nil},
                        {"fail_method", 0, 0, Type::Nil},        {"detach_self", 0, 0, Type::Nil},
                        {"call_script", 2, 2, Type::Nil},        {"sum", 0, -1, Type::Int}};
        classes["Node"] = node;
        ClassInfo n3;
        n3.name = "Node3D";
        n3.base = "Node";
        n3.properties = {{"position", Type::Vector3, false}, {"rotation", Type::Vector3, false},
                         {"rotation_degrees", Type::Vector3, false}, {"scale", Type::Vector3, false},
                         {"visible", Type::Bool, false}, {"global_position", Type::Vector3, false}};
        n3.methods = {{"translate", 1, 1, Type::Nil}, {"rotate_x", 1, 1, Type::Nil}, {"rotate_y", 1, 1, Type::Nil},
                      {"rotate_z", 1, 1, Type::Nil},  {"look_at", 1, 1, Type::Nil},  {"show", 0, 0, Type::Nil},
                      {"hide", 0, 0, Type::Nil},      {"is_visible_in_tree", 0, 0, Type::Bool}};
        classes["Node3D"] = n3;
        ClassInfo mesh;
        mesh.name = "MeshInstance3D";
        mesh.base = "Node3D";
        mesh.properties = {{"color", Type::Color, false}};
        classes["MeshInstance3D"] = mesh;
        ClassInfo cam;
        cam.name = "Camera3D";
        cam.base = "Node3D";
        cam.properties = {{"fov", Type::Float, false}, {"near", Type::Float, false}, {"far", Type::Float, false},
                          {"current", Type::Bool, false}};
        cam.methods = {{"make_current", 0, 0, Type::Nil}};
        classes["Camera3D"] = cam;
        ClassInfo timer;
        timer.name = "Timer";
        timer.base = "Node";
        timer.signals = {{"timeout", {}}};
        classes["Timer"] = timer;
        ClassInfo in;
        in.name = "Input";
        in.methods = {{"is_action_pressed", 1, 1, Type::Bool},
                      {"is_action_just_pressed", 1, 1, Type::Bool},
                      {"is_action_just_released", 1, 1, Type::Bool},
                      {"get_axis", 2, 2, Type::Float}};
        classes["Input"] = in;
        input = create("Input", "Input", 0);
    }

    ObjectId create(const std::string& cls, const std::string& name, ObjectId parent) {
        ObjectId id = next_id++;
        MockObject o;
        o.cls = cls;
        o.name = name;
        o.parent = parent;
        objects[id] = o;
        if (parent) objects[parent].children.push_back(id);
        return id;
    }
    MockObject& obj(ObjectId id) { return objects[id]; }

    bool inherits(const std::string& cls, const std::string& base) const {
        std::string c = cls;
        for (int i = 0; i < 16 && !c.empty(); ++i) {
            if (c == base) return true;
            auto it = classes.find(c);
            if (it == classes.end()) return false;
            c = it->second.base;
        }
        return false;
    }

    const ClassInfo* find_class(std::string_view name) const override {
        auto it = classes.find(std::string(name));
        return it == classes.end() ? nullptr : &it->second;
    }
    std::string_view class_of(ObjectId id) const override {
        auto it = objects.find(id);
        if (it == objects.end()) return std::string_view();
        return it->second.cls;
    }
    int property_id(std::string_view name) const override {
        for (size_t i = 0; i < props.size(); ++i)
            if (props[i] == name) return int(i);
        return -1;
    }
    int method_id(std::string_view name) const override {
        for (size_t i = 0; i < methods.size(); ++i)
            if (methods[i] == name) return int(i);
        return -1;
    }
    ObjectId singleton(std::string_view name) const override { return name == "Input" ? input : 0; }

    bool has_prop(const MockObject& o, const std::string& p) const {
        for (std::string c = o.cls; !c.empty();) {
            auto it = classes.find(c);
            if (it == classes.end()) return false;
            for (const PropertyInfo& pi : it->second.properties)
                if (pi.name == p) return true;
            c = it->second.base;
        }
        return false;
    }
    bool has_method(const MockObject& o, const std::string& m) const {
        for (std::string c = o.cls; !c.empty();) {
            auto it = classes.find(c);
            if (it == classes.end()) return false;
            for (const MethodInfo& mi : it->second.methods)
                if (mi.name == m) return true;
            c = it->second.base;
        }
        return false;
    }

    Status get_property(ObjectId id, int pid, Value& outv, std::string& error) override {
        ++get_property_calls;
        auto it = objects.find(id);
        if (it == objects.end()) {
            error = "Object was freed.";
            return Status::Error;
        }
        MockObject& o = it->second;
        if (pid < 0 || pid >= int(props.size()) || !has_prop(o, props[size_t(pid)])) return Status::NotFound;
        const std::string& p = props[size_t(pid)];
        const Real k = 180.0f / 3.14159265358979f;
        if (p == "name") outv = Value(o.name);
        else if (p == "position" || p == "global_position") outv = Value(o.position);
        else if (p == "rotation") outv = Value(o.rotation);
        else if (p == "rotation_degrees") outv = Value(Vector3{o.rotation.x * k, o.rotation.y * k, o.rotation.z * k});
        else if (p == "scale") outv = Value(o.scale);
        else if (p == "visible") outv = Value(o.visible);
        else if (p == "color") outv = Value(o.color);
        else if (p == "fov") outv = Value(o.fov);
        else if (p == "near") outv = Value(o.near_);
        else if (p == "far") outv = Value(o.far_);
        else if (p == "current") outv = Value(o.current);
        else return Status::NotFound;
        return Status::Ok;
    }

    Status set_property(ObjectId id, int pid, const Value& v, std::string& error) override {
        ++set_property_calls;
        auto it = objects.find(id);
        if (it == objects.end()) {
            error = "Object was freed.";
            return Status::Error;
        }
        MockObject& o = it->second;
        if (pid < 0 || pid >= int(props.size()) || !has_prop(o, props[size_t(pid)])) return Status::NotFound;
        const std::string& p = props[size_t(pid)];
        auto need = [&](Type t) {
            if (v.type() == t || (t == Type::Float && v.is_number())) return true;
            error = "Invalid type for property '" + p + "': expected " + type_name(t) + ", got " + type_name(v.type()) + ".";
            return false;
        };
        const Real k = 3.14159265358979f / 180.0f;
        if (p == "name") {
            error = "Property 'name' is read-only.";
            return Status::Error;
        } else if (p == "position" || p == "global_position") {
            if (!need(Type::Vector3)) return Status::Error;
            o.position = v.as_vector3();
        } else if (p == "rotation") {
            if (!need(Type::Vector3)) return Status::Error;
            o.rotation = v.as_vector3();
        } else if (p == "rotation_degrees") {
            if (!need(Type::Vector3)) return Status::Error;
            Vector3 d = v.as_vector3();
            o.rotation = Vector3{d.x * k, d.y * k, d.z * k};
        } else if (p == "scale") {
            if (!need(Type::Vector3)) return Status::Error;
            o.scale = v.as_vector3();
        } else if (p == "visible") {
            if (!need(Type::Bool)) return Status::Error;
            o.visible = v.as_bool();
        } else if (p == "color") {
            if (!need(Type::Color)) return Status::Error;
            o.color = v.as_color();
        } else if (p == "fov") {
            if (!need(Type::Float)) return Status::Error;
            o.fov = v.as_real();
        } else if (p == "near") {
            if (!need(Type::Float)) return Status::Error;
            o.near_ = v.as_real();
        } else if (p == "far") {
            if (!need(Type::Float)) return Status::Error;
            o.far_ = v.as_real();
        } else if (p == "current") {
            if (!need(Type::Bool)) return Status::Error;
            o.current = v.as_bool();
        } else {
            return Status::NotFound;
        }
        return Status::Ok;
    }

    ObjectId resolve_path(ObjectId from, const std::string& path) {
        ObjectId cur = from;
        size_t i = 0;
        if (!path.empty() && path[0] == '/') {
            cur = 0;
            i = 1;
            std::string rest = path.substr(1);
            if (rest.compare(0, 5, "root/") == 0) i += 5;
            else if (rest == "root") return root;
        }
        while (i <= path.size()) {
            size_t j = path.find('/', i);
            if (j == std::string::npos) j = path.size();
            std::string seg = path.substr(i, j - i);
            i = j + 1;
            if (seg.empty() || seg == ".") {
                if (j >= path.size()) break;
                continue;
            }
            if (seg == "..") {
                if (!cur || !objects.count(cur)) return 0;
                cur = objects[cur].parent;
                if (!cur) return 0;
            } else if (cur == 0) {
                if (!objects.count(root) || objects[root].name != seg) return 0;
                cur = root;
            } else {
                ObjectId found = 0;
                for (ObjectId c : objects[cur].children)
                    if (objects[c].name == seg) found = c;
                if (!found) return 0;
                cur = found;
            }
            if (j >= path.size()) break;
        }
        return cur;
    }

    ObjectId find_child_rec(ObjectId from, const std::string& name, int depth = 0) {
        if (depth > 32) return 0;
        for (ObjectId c : objects[from].children) {
            if (objects[c].name == name) return c;
            if (ObjectId r = find_child_rec(c, name, depth + 1)) return r;
        }
        return 0;
    }

    Status call_method(ObjectId id, int mid, const Value* args, int count, Value& result, std::string& error) override {
        auto it = objects.find(id);
        if (it == objects.end()) {
            error = "Object was freed.";
            return Status::Error;
        }
        if (mid < 0 || mid >= int(methods.size()) || !has_method(it->second, methods[size_t(mid)])) return Status::NotFound;
        const std::string& m = methods[size_t(mid)];
        MockObject& o = it->second;
        auto argn = [&](int n) {
            if (count == n) return true;
            error = "Method '" + m + "' expects " + std::to_string(n) + " arguments.";
            return false;
        };
        if (m == "get_node" || m == "get_node_or_null") {
            if (!argn(1)) return Status::Error;
            ObjectId r = resolve_path(id, std::string(args[0].as_string()));
            if (!r && m == "get_node") err_out.push_back("Node not found: \"" + std::string(args[0].as_string()) + "\"");
            result = Value::object(r);
        } else if (m == "get_parent") {
            result = Value::object(o.parent);
        } else if (m == "get_children") {
            result = Value::new_array();
            for (ObjectId c : o.children) result.array_push(Value::object(c));
        } else if (m == "get_child") {
            if (!argn(1)) return Status::Error;
            Int i = args[0].as_int();
            if (i < 0 || i >= Int(o.children.size())) {
                error = "Index p_index = " + std::to_string(i) + " is out of bounds.";
                return Status::Error;
            }
            result = Value::object(o.children[size_t(i)]);
        } else if (m == "get_child_count") {
            result = Value(Int(o.children.size()));
        } else if (m == "find_child") {
            if (!argn(1)) return Status::Error;
            result = Value::object(find_child_rec(id, std::string(args[0].as_string())));
        } else if (m == "queue_free") {
            o.queued = true;
        } else if (m == "translate") {
            if (!argn(1)) return Status::Error;
            Vector3 d = args[0].as_vector3();
            o.position = Vector3{o.position.x + d.x, o.position.y + d.y, o.position.z + d.z};
        } else if (m == "rotate_x" || m == "rotate_y" || m == "rotate_z") {
            if (!argn(1)) return Status::Error;
            Real a = args[0].as_real();
            if (m == "rotate_x") o.rotation.x += a;
            else if (m == "rotate_y") o.rotation.y += a;
            else o.rotation.z += a;
        } else if (m == "look_at") {
            if (!argn(1)) return Status::Error;
            o.look_target = args[0].as_vector3();
        } else if (m == "show" || m == "hide") {
            o.visible = m == "show";
        } else if (m == "is_visible_in_tree") {
            bool v = true;
            for (ObjectId c = id; c; c = objects[c].parent) v = v && objects[c].visible;
            result = Value(v);
        } else if (m == "make_current") {
            o.current = true;
        } else if (m == "is_action_pressed" || m == "is_action_just_pressed" || m == "is_action_just_released") {
            if (!argn(1)) return Status::Error;
            bool p = pressed.count(std::string(args[0].as_string())) > 0;
            result = Value(m == "is_action_just_released" ? !p : p);
        } else if (m == "get_axis") {
            if (!argn(2)) return Status::Error;
            Real a = (pressed.count(std::string(args[1].as_string())) ? 1.0f : 0.0f) -
                     (pressed.count(std::string(args[0].as_string())) ? 1.0f : 0.0f);
            result = Value(a);
        } else if (m == "fail_method") {
            error = "fail_method always fails.";
            return Status::Error;
        } else if (m == "detach_self") {
            if (vm) vm->detach(id);
        } else if (m == "call_script") {
            if (!argn(2)) return Status::Error;
            Value r;
            if (vm && !vm->call(args[0].as_object(), args[1].as_string(), nullptr, 0, &r)) {
                error = "nested call failed";
                return Status::Error;
            }
            result = r;
        } else if (m == "sum") {
            Int s = 0;
            for (int i = 0; i < count; ++i) s += args[i].as_int();
            result = Value(s);
        } else {
            return Status::NotFound;
        }
        return Status::Ok;
    }

    std::string describe(ObjectId id) const override {
        auto it = objects.find(id);
        if (it == objects.end()) return "<Freed Object>";
        return it->second.name + ":<" + it->second.cls + "#" + std::to_string(id) + ">";
    }
    void print(std::string_view text, bool is_error) override {
        (is_error ? err_out : out).push_back(std::string(text));
    }
};

std::ostream& operator<<(std::ostream& os, const Value& v) { return os << v.to_string() << " (" << type_name(v.type()) << ")"; }
std::ostream& operator<<(std::ostream& os, Type t) { return os << type_name(t); }

// ------------------------------------------------------------------ fixture helpers

struct Fixture {
    MockHost host;
    VM vm;
    ObjectId world, cube, camera, player, label, target;
    Fixture() : vm(host) {
        host.vm = &vm;
        world = host.create("Node3D", "World", 0);
        host.root = world;
        cube = host.create("MeshInstance3D", "Cube", world);
        camera = host.create("Camera3D", "Camera", world);
        player = host.create("Node3D", "Player", world);
        label = host.create("Node", "Label", player);
        target = host.create("Node3D", "Target", world);
        vm.seed(1234);
    }
};

static std::string dump_errors(const std::vector<Diagnostic>& errs) {
    std::string s;
    for (const Diagnostic& d : errs) s += "  " + std::to_string(d.line) + ":" + std::to_string(d.column) + ": " + d.message + "\n";
    return s;
}

static std::shared_ptr<const Script> compile_ok(const Host& host, const std::string& src, const char* path = "res://test.jet") {
    CompileResult r = compile(src, path, host);
    if (!r.errors.empty()) {
        ++g_failures;
        std::cerr << "unexpected compile errors for:\n" << src << "\n" << dump_errors(r.errors);
    }
    ++g_checks;
    return r.script;
}

static std::vector<Diagnostic> compile_errors(const Host& host, const std::string& src) {
    CompileResult r = compile(src, "res://bad.jet", host);
    if (r.script && !r.errors.empty()) ++g_failures; // script must be null when errors exist
    return r.errors;
}

static std::string first_error(const Host& host, const std::string& src) {
    auto errs = compile_errors(host, src);
    return errs.empty() ? std::string("<no error>") : errs[0].message;
}

static std::string dump_runtime(const std::vector<RuntimeError>& errs) {
    std::string s;
    for (const RuntimeError& e : errs) {
        s += "  " + e.path + ":" + std::to_string(e.line) + " in " + e.function + ": " + e.message + "\n";
        for (const StackFrame& f : e.stack) s += "    at " + f.function + " (" + f.path + ":" + std::to_string(f.line) + ")\n";
    }
    return s;
}

// Runs `func f():` with the given body (lines without indentation; a tab is added).
static Value run_body(Fixture& fx, const std::string& body, const std::string& header = "extends Node3D\n",
                      ObjectId on = 0) {
    std::string src = header + "func f():\n";
    std::istringstream in(body);
    std::string line;
    while (std::getline(in, line)) src += "\t" + line + "\n";
    auto script = compile_ok(fx.host, src);
    if (!script) return Value("<compile error>");
    ObjectId obj = on ? on : fx.player;
    if (!fx.vm.attach(obj, script)) {
        std::cerr << "attach failed\n" << dump_runtime(fx.vm.take_errors());
        return Value("<attach error>");
    }
    Value r;
    if (!fx.vm.call(obj, "f", nullptr, 0, &r)) {
        std::cerr << "runtime error in:\n" << src << dump_runtime(fx.vm.take_errors());
        ++g_failures;
        return Value("<runtime error>");
    }
    fx.vm.detach(obj);
    return r;
}

static Value ev(Fixture& fx, const std::string& expr) { return run_body(fx, "return " + expr); }
static std::string evs(Fixture& fx, const std::string& expr) { return ev(fx, expr).to_string(); }

static RuntimeError run_error(Fixture& fx, const std::string& src, const char* fn = "f", ObjectId on = 0) {
    RuntimeError none;
    none.message = "<no error>";
    auto script = compile_ok(fx.host, src, "res://err.jet");
    if (!script) return none;
    ObjectId obj = on ? on : fx.player;
    fx.vm.take_errors();
    if (!fx.vm.attach(obj, script)) {
        auto errs = fx.vm.take_errors();
        return errs.empty() ? none : errs[0];
    }
    bool ok = fx.vm.call(obj, fn);
    auto errs = fx.vm.take_errors();
    fx.vm.detach(obj);
    if (ok || errs.empty()) return none;
    return errs[0];
}

// ------------------------------------------------------------------ tests

static void test_values() {
    CHECK(sizeof(Value) <= 16);
    Value a(Int(5)), b(2.5), s("hello"), v(Vector3{1, 2, 3});
    CHECK(a.type() == Type::Int && a.as_int() == 5 && a.as_real() == 5.0f);
    CHECK(b.type() == Type::Float && b.as_int() == 2);
    CHECK(s.as_string() == "hello");
    CHECK(v.as_vector3().y == 2);
    Value arr = Value::new_array();
    arr.array_push(a);
    arr.array_push(s);
    Value arr2 = arr; // shared reference
    arr2.array_push(Value(true));
    CHECK_EQ(arr.array_size(), size_t(3));
    CHECK_EQ(arr.to_string(), std::string("[5, \"hello\", true]"));
    CHECK(Value(Int(1)) == Value(1.0));
    CHECK(!(Value("1") == Value(Int(1))));
    CHECK_EQ(Value(2.0).to_string(), std::string("2.0"));
    CHECK_EQ(Value(0.1).to_string(), std::string("0.1"));
    CHECK_EQ(Value(Real(1.0f / 3.0f)).to_string(), std::string("0.33333334"));
    CHECK_EQ(Value(1e20).to_string(), std::string("100000000000000000000.0"));
    CHECK_EQ(Value(1e25).to_string(), std::string("1e+25"));
    CHECK_EQ(Value(-0.5).to_string(), std::string("-0.5"));
    CHECK_EQ(Value(Vector3{1, 2, 3}).to_string(), std::string("(1.0, 2.0, 3.0)"));
    CHECK_EQ(Value(Color{1, 0.5f, 0}).to_string(), std::string("(1, 0.5, 0, 1)"));
    CHECK_EQ(Value().to_string(), std::string("<null>"));
    Value moved = std::move(s);
    CHECK(s.is_nil() && moved.as_string() == "hello");
    Value e("");
    CHECK(e.type() == Type::String && e.as_string().empty() && !e.truthy());
    CHECK(std::string(type_name(Type::Dictionary)) == "Dictionary");
}

static void test_lexer(Fixture& fx) {
    // spaces instead of tabs, nested blocks, comments at odd indentation, blank lines with whitespace
    auto s1 = compile_ok(fx.host,
                         "extends Node3D\n"
                         "# comment\n"
                         "func f():\n"
                         "    var x = 0\n"
                         "      # odd comment\n"
                         "    \n"
                         "    for i in 3:\n"
                         "        if i == 1:\n"
                         "            x += 10\n"
                         "        else:\n"
                         "            x += 1 # trailing\n"
                         "    return x\n");
    if (s1) {
        fx.vm.attach(fx.player, s1);
        Value r;
        CHECK(fx.vm.call(fx.player, "f", nullptr, 0, &r));
        CHECK_EQ(r.as_int(), Int(12));
    }
    // CRLF line endings + tabs
    {
        std::string src = "extends Node3D\r\nfunc f():\r\n\tvar a = 1\r\n\tif a == 1:\r\n\t\ta = 2\r\n\treturn a\r\n";
        auto s = compile_ok(fx.host, src);
        if (s) {
            fx.vm.attach(fx.player, s);
            Value r;
            CHECK(fx.vm.call(fx.player, "f", nullptr, 0, &r));
            CHECK_EQ(r.as_int(), Int(2));
        }
    }
    // one-line blocks, semicolons, continuation inside brackets and with backslash
    CHECK_EQ(run_body(fx, "var t = 0\nfor i in 4: t += i\nif t == 6: return t * 2\nreturn -1").as_int(), Int(12));
    CHECK_EQ(run_body(fx, "var a = 1; var b = 2; a += b\nreturn a").as_int(), Int(3));
    CHECK_EQ(run_body(fx, "var arr = [\n1,\n\t\t2,\n  3,\n]\nreturn arr.size()").as_int(), Int(3));
    CHECK_EQ(run_body(fx, "var d = {\n\"a\": 1, # comment inside\n\"b\": 2\n}\nreturn d[\"b\"]").as_int(), Int(2));
    CHECK_EQ(run_body(fx, "var x = 1 + \\\n2 + \\\n3\nreturn x").as_int(), Int(6));
    CHECK_EQ(run_body(fx, "var x = max(1,\n5,\n3)\nreturn x").as_int(), Int(5));
    CHECK_EQ(run_body(fx, "var i = 0\nwhile i < 5: i += 1\nreturn i").as_int(), Int(5));
    {
        auto s = compile_ok(fx.host, "extends Node\nfunc g(): return 42\nfunc h(x): return x + 1\n");
        if (s) {
            fx.vm.attach(fx.label, s);
            Value r;
            Value arg(Int(1));
            CHECK(fx.vm.call(fx.label, "g", nullptr, 0, &r) && r.as_int() == 42);
            CHECK(fx.vm.call(fx.label, "h", &arg, 1, &r) && r.as_int() == 2);
            fx.vm.detach(fx.label);
        }
    }
    // numeric literals
    CHECK_EQ(ev(fx, "0xFF").as_int(), Int(255));
    CHECK_EQ(ev(fx, "0b1010").as_int(), Int(10));
    CHECK_EQ(ev(fx, "1_000_000").as_int(), Int(1000000));
    CHECK_EQ(ev(fx, "1.5").as_real(), 1.5f);
    CHECK_EQ(ev(fx, ".5").as_real(), 0.5f);
    CHECK_EQ(ev(fx, "1e3").as_real(), 1000.0f);
    CHECK_EQ(ev(fx, "2.5e-2").as_real(), 0.025f);
    CHECK(ev(fx, "1e3").type() == Type::Float);
    CHECK(ev(fx, "3.").type() == Type::Float);
    // strings
    CHECK_EQ(evs(fx, "\"a\\tb\\n\"").size(), size_t(4));
    CHECK_EQ(evs(fx, "'it\\'s'"), std::string("it's"));
    CHECK_EQ(evs(fx, "\"\\u00e9\""), std::string("\xc3\xa9"));
    CHECK_EQ(evs(fx, "\"\\\"q\\\"\""), std::string("\"q\""));
    CHECK_EQ(ev(fx, "\"\\u00e9t\\u00e9\".length()").as_int(), Int(3));
    CHECK_EQ(run_body(fx, "var s = \"\"\"line1\nline2\"\"\"\nreturn s.split(\"\\n\").size()").as_int(), Int(2));
    // UTF-8 BOM
    {
        std::string src = "\xEF\xBB\xBF" "extends Node\nfunc f():\n\treturn 7\n";
        auto s = compile_ok(fx.host, src);
        CHECK(s != nullptr);
    }
}

static void test_operators(Fixture& fx) {
    CHECK_EQ(ev(fx, "2 + 3 * 4").as_int(), Int(14));
    CHECK_EQ(ev(fx, "(2 + 3) * 4").as_int(), Int(20));
    CHECK_EQ(ev(fx, "2 ** 3 ** 2").as_int(), Int(64)); // left associative like Godot
    CHECK_EQ(ev(fx, "-2 ** 2").as_int(), Int(4));      // unary binds tighter (spec table)
    CHECK_EQ(ev(fx, "2 ** 10").as_int(), Int(1024));
    CHECK_EQ(ev(fx, "2.0 ** 0.5").as_real(), std::sqrt(2.0f));
    CHECK_EQ(ev(fx, "7 / 2").as_int(), Int(3));
    CHECK_EQ(ev(fx, "-7 / 2").as_int(), Int(-3));
    CHECK_EQ(ev(fx, "7 % 3").as_int(), Int(1));
    CHECK_EQ(ev(fx, "-7 % 3").as_int(), Int(-1));
    CHECK_EQ(ev(fx, "7.0 / 2").as_real(), 3.5f);
    CHECK_EQ(ev(fx, "7.5 % 2").as_real(), 1.5f);
    CHECK(ev(fx, "7 / 2.0").type() == Type::Float);
    CHECK_EQ(ev(fx, "1 << 4").as_int(), Int(16));
    CHECK_EQ(ev(fx, "0xF0 >> 4").as_int(), Int(15));
    CHECK_EQ(ev(fx, "-16 >> 2").as_int(), Int(-4));
    CHECK_EQ(ev(fx, "5 & 3").as_int(), Int(1));
    CHECK_EQ(ev(fx, "5 | 3").as_int(), Int(7));
    CHECK_EQ(ev(fx, "5 ^ 3").as_int(), Int(6));
    CHECK_EQ(ev(fx, "~5").as_int(), Int(-6));
    CHECK_EQ(ev(fx, "1 | 2 == 3").as_bool(), true);
    CHECK_EQ(ev(fx, "2 + 3 << 1").as_int(), Int(10));
    CHECK_EQ(ev(fx, "-(3)").as_int(), Int(-3));
    CHECK_EQ(ev(fx, "+3").as_int(), Int(3));
    CHECK_EQ(ev(fx, "9223372036854775807 + 1").as_int(), INT64_MIN); // wraps, no UB
    CHECK_EQ(ev(fx, "-9223372036854775807 - 2").as_int(), INT64_MAX);
    // The 32 bit fast paths must give the 64 bit results.
    CHECK_EQ(ev(fx, "-2147483648 / -1").as_int(), Int(2147483648));
    CHECK_EQ(ev(fx, "-2147483648 % -1").as_int(), Int(0));
    CHECK_EQ(ev(fx, "-2147483649 / 2").as_int(), Int(-1073741824));
    CHECK_EQ(ev(fx, "(1 << 40) / 3").as_int(), (Int(1) << 40) / 3);
    CHECK_EQ(ev(fx, "(1 << 40) % 7").as_int(), (Int(1) << 40) % 7);
    CHECK_EQ(ev(fx, "-7 % -3").as_int(), Int(-1));
    CHECK_EQ(run_body(fx, "var a = 1 << 40\nvar b = 3\nreturn a / b").as_int(), (Int(1) << 40) / 3);
    CHECK_EQ(run_body(fx, "var a = 3000000000\nreturn a * 0.5").as_real(), 1.5e9f);
    CHECK(ev(fx, "16777216 == 16777216.0 and 16777217 != 16777216.0 and 16777217 > 16777216.0").as_bool());
    CHECK(ev(fx, "-3 < -2.5 and 2.5 < 3 and not (1 < NAN) and not (1 >= NAN) and not (1 == NAN)").as_bool());
    CHECK_EQ(ev(fx, "int(-2147483648.0)").as_int(), Int(-2147483648LL));
    CHECK_EQ(ev(fx, "int(3e9)").as_int(), Int(3000000000LL));
    CHECK_EQ(ev(fx, "int(1e30)").as_int(), INT64_MAX);
    CHECK_EQ(ev(fx, "int(-1e30)").as_int(), INT64_MIN);
    CHECK_EQ(ev(fx, "int(NAN)").as_int(), Int(0));
    CHECK_EQ(ev(fx, "floori(-2.5) + ceili(2.5) + roundi(2.5)").as_int(), Int(-3 + 3 + 3));
    CHECK_EQ(run_body(fx, "var a = [3, 1.5, 2, -1e9, 20000000]\na.sort()\nreturn str(a)").to_string(),
             std::string("[-1000000000.0, 1.5, 2, 3, 20000000]"));
    CHECK(ev(fx, "1 == 1.0").as_bool());
    CHECK(ev(fx, "1 != 2").as_bool());
    CHECK(ev(fx, "1 < 2.5").as_bool());
    CHECK(ev(fx, "2 >= 2").as_bool());
    CHECK(!ev(fx, "3 <= 2").as_bool());
    CHECK(ev(fx, "\"abc\" < \"abd\"").as_bool());
    CHECK(ev(fx, "\"a\" == \"a\"").as_bool());
    CHECK(!ev(fx, "\"1\" == 1").as_bool());
    CHECK(ev(fx, "not 1 == 2").as_bool());
    CHECK(ev(fx, "!false").as_bool());
    CHECK(!ev(fx, "true and false").as_bool());
    CHECK(ev(fx, "true or false").as_bool());
    CHECK(ev(fx, "false || true && true").as_bool());
    CHECK(ev(fx, "1 + 2 == 3 and 2 * 2 == 4").as_bool());
    CHECK(ev(fx, "1 < 2 == true").as_bool());
    CHECK_EQ(ev(fx, "\"yes\" if 2 > 1 else \"no\"").to_string(), std::string("yes"));
    CHECK_EQ(ev(fx, "1 if false else 2 if false else 3").as_int(), Int(3));
    CHECK(ev(fx, "3 in [1, 2, 3]").as_bool());
    CHECK(ev(fx, "4 not in [1, 2, 3]").as_bool());
    CHECK(ev(fx, "\"b\" in \"abc\"").as_bool());
    CHECK(ev(fx, "\"k\" in {\"k\": 1}").as_bool());
    CHECK(!ev(fx, "\"z\" in {\"k\": 1}").as_bool());
    CHECK(ev(fx, "1 is int").as_bool());
    CHECK(!ev(fx, "1 is float").as_bool());
    CHECK(ev(fx, "1.0 is float").as_bool());
    CHECK(ev(fx, "\"s\" is String").as_bool());
    CHECK(ev(fx, "Vector3() is Vector3").as_bool());
    CHECK(ev(fx, "Color.RED is Color").as_bool());
    CHECK(ev(fx, "[] is Array and {} is Dictionary").as_bool());
    CHECK(ev(fx, "true is bool").as_bool());
    CHECK(ev(fx, "self is Node3D and self is Node").as_bool());
    CHECK(!ev(fx, "self is MeshInstance3D").as_bool());
    CHECK(ev(fx, "$\"../Cube\" is MeshInstance3D").as_bool());
    CHECK(ev(fx, "1 is not String").as_bool());
    CHECK(!ev(fx, "null is Node").as_bool());
    // short-circuit: the right side would fail
    CHECK(!run_body(fx, "var n = null\nreturn n != null and n.position.x > 0").as_bool());
    CHECK(run_body(fx, "var n = null\nreturn n == null or n.position.x > 0").as_bool());
    // strings and arrays
    CHECK_EQ(evs(fx, "\"ab\" + \"cd\""), std::string("abcd"));
    CHECK_EQ(evs(fx, "[1, 2] + [3]"), std::string("[1, 2, 3]"));
    CHECK_EQ(evs(fx, "\"%d-%s\" % [5, \"x\"]"), std::string("5-x"));
    CHECK_EQ(evs(fx, "\"v=%d\" % 7"), std::string("v=7"));
    // vectors and colors
    CHECK_EQ(evs(fx, "Vector3(1, 2, 3) + Vector3(1, 1, 1)"), std::string("(2.0, 3.0, 4.0)"));
    CHECK_EQ(evs(fx, "Vector3(1, 2, 3) * 2"), std::string("(2.0, 4.0, 6.0)"));
    CHECK_EQ(evs(fx, "2 * Vector3(1, 2, 3)"), std::string("(2.0, 4.0, 6.0)"));
    CHECK_EQ(evs(fx, "Vector3(2, 4, 6) / 2"), std::string("(1.0, 2.0, 3.0)"));
    CHECK_EQ(evs(fx, "-Vector3(1, -2, 3)"), std::string("(-1.0, 2.0, -3.0)"));
    CHECK_EQ(evs(fx, "Vector3(1, 2, 3) * Vector3(2, 2, 2)"), std::string("(2.0, 4.0, 6.0)"));
    CHECK_EQ(evs(fx, "Vector3(1, 2, 3) - Vector3(1, 2, 3)"), std::string("(0.0, 0.0, 0.0)"));
    CHECK_EQ(evs(fx, "Color(0.5, 0.5, 0.5) * 2"), std::string("(1, 1, 1, 1)"));
    CHECK_EQ(evs(fx, "Color(1, 1, 1) - Color(0.5, 0, 0.25)"), std::string("(0.5, 1, 0.75, 1)"));
    CHECK(ev(fx, "Vector3(1, 2, 3) == Vector3(1, 2, 3)").as_bool());

    // Vector2: Godot's 2D conventions (y down)
    CHECK_EQ(evs(fx, "Vector2(1, 2) + Vector2(3, 4)"), std::string("(4.0, 6.0)"));
    CHECK_EQ(evs(fx, "Vector2(1, 2) * 2"), std::string("(2.0, 4.0)"));
    CHECK_EQ(evs(fx, "2 * Vector2(1, 2)"), std::string("(2.0, 4.0)"));
    CHECK_EQ(evs(fx, "Vector2(4, 2) / Vector2(2, 2)"), std::string("(2.0, 1.0)"));
    CHECK_EQ(evs(fx, "-Vector2(1, -2)"), std::string("(-1.0, 2.0)"));
    CHECK_EQ(evs(fx, "Vector2.UP"), std::string("(0.0, -1.0)"));
    CHECK_EQ(evs(fx, "Vector2()"), std::string("(0.0, 0.0)"));
    CHECK_EQ(evs(fx, "typeof(Vector2(1, 2))"), std::string("Vector2"));
    CHECK_EQ(evs(fx, "Vector2(3, 4).normalized()"), std::string("(0.6, 0.8)"));
    CHECK(ev(fx, "Vector2.RIGHT.rotated(PI / 2).is_equal_approx(Vector2.DOWN)").as_bool());
    CHECK_EQ(evs(fx, "Vector2(1, 2).orthogonal()"), std::string("(2.0, -1.0)"));
    CHECK_EQ(evs(fx, "Vector2(0, 0).move_toward(Vector2(10, 0), 3)"), std::string("(3.0, 0.0)"));
    CHECK_EQ(evs(fx, "clamp(Vector2(5, -5), Vector2(0, 0), Vector2(2, 2))"), std::string("(2.0, 0.0)"));
    CHECK_EQ(evs(fx, "lerp(Vector2(0, 0), Vector2(2, 4), 0.5)"), std::string("(1.0, 2.0)"));
    CHECK_EQ(evs(fx, "abs(Vector2(-1, 2))"), std::string("(1.0, 2.0)"));
    CHECK(std::fabs(ev(fx, "Vector2(3, 4).length()").as_real() - 5) < 1e-5f);
    CHECK(std::fabs(ev(fx, "Vector2(0, 1).angle()").as_real() - 3.14159265f / 2) < 1e-5f);
    CHECK(std::fabs(ev(fx, "Vector2(1, 0).angle_to(Vector2(0, 1))").as_real() - 3.14159265f / 2) < 1e-5f);
    CHECK(std::fabs(ev(fx, "Vector2(1, 0).dot(Vector2(0, 1))").as_real()) < 1e-6f);
    CHECK(ev(fx, "Vector2(1, 2) == Vector2(1, 2)").as_bool());
    CHECK(!ev(fx, "Vector2(1, 2) == Vector3(1, 2, 0)").as_bool());
    CHECK(ev(fx, "Vector2(1, 2) is Vector2").as_bool());
    CHECK_EQ(run_body(fx, "var v := Vector2(1, 2)\nv.x += 5\nv[1] = 7\nreturn v").to_string(), std::string("(6.0, 7.0)"));
    CHECK_EQ(run_body(fx, "var v := Vector2(1, 2)\nreturn v.y").to_string(), std::string("2.0"));
    CHECK_EQ(run_body(fx, "const V := Vector2(2, 3) * 2\nreturn V.x + V.y").to_string(), std::string("10.0"));
    CHECK_EQ(run_body(fx, "var d := {Vector2(1, 1): \"a\"}\nreturn d[Vector2(1, 1)]").to_string(), std::string("a"));
    CHECK(ev(fx, "[1, [2]] == [1, [2]]").as_bool());
    CHECK(ev(fx, "{\"a\": 1} == {\"a\": 1.0}").as_bool());
    CHECK(ev(fx, "1.0 / 0 == INF").as_bool());
    CHECK(ev(fx, "is_nan(0.0 / 0.0)").as_bool());
}

static void test_statements(Fixture& fx) {
    CHECK_EQ(run_body(fx, "var x = 5\nif x > 10:\n\treturn 1\nelif x > 3:\n\treturn 2\nelse:\n\treturn 3").as_int(), Int(2));
    CHECK_EQ(run_body(fx, "var x = 0\nif x:\n\treturn 1\nelif x == 0:\n\treturn 4\nreturn 9").as_int(), Int(4));
    CHECK_EQ(run_body(fx, "var s = 0\nvar i = 0\nwhile true:\n\ti += 1\n\tif i % 2 == 0:\n\t\tcontinue\n\tif i > 9:\n\t\tbreak\n\ts += i\nreturn s").as_int(), Int(25));
    CHECK_EQ(run_body(fx, "var s = 0\nfor i in range(1, 10, 3):\n\ts += i\nreturn s").as_int(), Int(12));
    CHECK_EQ(run_body(fx, "var s = 0\nfor i in range(5, 0, -2):\n\ts += i\nreturn s").as_int(), Int(9));
    CHECK_EQ(run_body(fx, "var s = []\nfor x in [3, \"a\", 1.5]:\n\ts.append(typeof(x))\nreturn str(s)").to_string(),
             std::string("[\"int\", \"String\", \"float\"]"));
    CHECK_EQ(run_body(fx, "var s = \"\"\nfor k in {\"b\": 1, \"a\": 2}:\n\ts += k\nreturn s").to_string(), std::string("ba"));
    CHECK_EQ(run_body(fx, "var s = []\nfor c in \"h\\u00e9!\":\n\ts.append(c)\nreturn s.size()").as_int(), Int(3));
    CHECK_EQ(run_body(fx, "var n = 0\nfor i in 4:\n\tfor j in 3:\n\t\tif j == 2:\n\t\t\tbreak\n\t\tn += 1\nreturn n").as_int(), Int(8));
    CHECK_EQ(run_body(fx, "var a = [1, 2, 3]\nvar s = 0\nfor v in a:\n\ts += v\n\tif v == 1:\n\t\ta.append(10)\nreturn s").as_int(), Int(16));
    CHECK_EQ(run_body(fx, "var n = 3\nvar r = 0\nfor i in n:\n\tr += i\nreturn r").as_int(), Int(3));
    CHECK_EQ(run_body(fx, "var r = 0\nfor i: int in [1.5, 2.5]:\n\tr += i\nreturn r").as_int(), Int(3));
    CHECK_EQ(run_body(fx, "pass\nreturn").type(), Type::Nil);
    CHECK_EQ(run_body(fx, "const K = 3\nvar a := K * 2\nreturn a").as_int(), Int(6));
    // locals are reset when a block is re-entered
    CHECK_EQ(run_body(fx, "var out = []\nfor i in 2:\n\tvar v\n\tout.append(v)\n\tv = 5\nreturn str(out)").to_string(),
             std::string("[<null>, <null>]"));
    // compound assignment operators
    CHECK_EQ(run_body(fx, "var x = 10\nx += 5\nx -= 3\nx *= 2\nx /= 4\nx %= 4\nreturn x").as_int(), Int(2));
    CHECK_EQ(run_body(fx, "var x = 6\nx &= 3\nx |= 8\nx ^= 1\nx <<= 2\nx >>= 1\nreturn x").as_int(), Int(22));
    CHECK_EQ(run_body(fx, "var x = 2\nx **= 3\nreturn x").as_int(), Int(8));
}

static void test_match(Fixture& fx) {
    std::string header = "extends Node3D\nenum State { IDLE, RUN = 5, JUMP }\nconst SEVEN = 7\n";
    auto m = [&](const std::string& v) {
        return run_body(fx,
                        "match " + v + ":\n"
                        "\tState.IDLE, State.RUN:\n\t\treturn \"idle_or_run\"\n"
                        "\tSEVEN:\n\t\treturn \"seven\"\n"
                        "\t\"txt\":\n\t\treturn \"text\"\n"
                        "\t1.5:\n\t\treturn \"float\"\n"
                        "\t_:\n\t\treturn \"other\"",
                        header)
            .to_string();
    };
    CHECK_EQ(m("0"), std::string("idle_or_run"));
    CHECK_EQ(m("5"), std::string("idle_or_run"));
    CHECK_EQ(m("State.JUMP"), std::string("other"));
    CHECK_EQ(m("7"), std::string("seven"));
    CHECK_EQ(m("7.0"), std::string("other")); // match is type strict like Godot
    CHECK_EQ(m("\"txt\""), std::string("text"));
    CHECK_EQ(m("1.5"), std::string("float"));
    CHECK_EQ(run_body(fx, "match 42:\n\t1:\n\t\treturn 0\n\tvar x:\n\t\treturn x + 1", header).as_int(), Int(43));
    CHECK_EQ(run_body(fx, "var r = 0\nmatch 3:\n\t1, 2:\n\t\tr = 1\n\t3, 4:\n\t\tr = 2\nreturn r", header).as_int(), Int(2));
    CHECK_EQ(run_body(fx, "var r = -1\nmatch 9:\n\t1:\n\t\tr = 1\nreturn r", header).as_int(), Int(-1));
    CHECK_EQ(run_body(fx, "return State.JUMP", header).as_int(), Int(6));
    CHECK_EQ(run_body(fx, "return State.keys().size()", header).as_int(), Int(3));
    CHECK_EQ(run_body(fx, "return State[\"RUN\"]", header).as_int(), Int(5));
    CHECK_EQ(run_body(fx, "return A + B * 10", "extends Node\nenum { A, B }\n", fx.label).as_int(), Int(10));
}

static void test_typed(Fixture& fx) {
    CHECK_EQ(run_body(fx, "var i: int = 2.7\nreturn i").as_int(), Int(2));
    CHECK(run_body(fx, "var i: int = 2.7\nreturn i").type() == Type::Int);
    CHECK(run_body(fx, "var f: float = 3\nreturn f").type() == Type::Float);
    CHECK_EQ(run_body(fx, "var i: int = 0\ni = 5.9\nreturn i").as_int(), Int(5));
    CHECK(run_body(fx, "var x := 1.0\nx = 2\nreturn x").type() == Type::Float);
    CHECK(run_body(fx, "var x := 1\nx = 2.5\nreturn x").type() == Type::Int);
    CHECK(run_body(fx, "var x = 1\nx = 2.5\nreturn x").type() == Type::Float);
    CHECK(run_body(fx, "var v: Vector3\nreturn v").type() == Type::Vector3);
    CHECK(run_body(fx, "var a: Array\na.append(1)\nreturn a.size()").as_int() == 1);
    CHECK(run_body(fx, "var n: Node3D = $\"../Cube\"\nreturn n.name").to_string() == "Cube");
    CHECK_EQ(run_body(fx, "return h(3)", "extends Node3D\nfunc h(x: float) -> int:\n\treturn x * 1.5\n").as_int(), Int(4));
    CHECK(run_body(fx, "return h(3)", "extends Node3D\nfunc h(x: float):\n\treturn x\n").type() == Type::Float);
    CHECK_EQ(run_body(fx, "counter = 3.9\nreturn counter", "extends Node3D\nvar counter: int = 0\n").as_int(), Int(3));
    CHECK_EQ(run_body(fx, "return x + y", "extends Node3D\nvar x := 1.5\nvar y: float = 2\n").as_real(), 3.5f);
    CHECK_EQ(run_body(fx, "return k(1) + k(1, 2) + k(1, 2, 3.5)",
                      "extends Node3D\nfunc k(a, b := 10, c: int = 100):\n\treturn a + b + c\n").as_int(),
             Int(111 + 103 + 6));
    CHECK_EQ(run_body(fx, "return (2.9 as int) + (\"\" as String).length()").as_int(), Int(2));
    CHECK(run_body(fx, "return self as MeshInstance3D").is_nil());
    CHECK(run_body(fx, "return self as Node").type() == Type::Object);
    // runtime type errors
    auto e = run_error(fx, "extends Node3D\nvar i: int = 0\nfunc f():\n\tvar s = \"x\"\n\ti = s\n");
    CHECK(contains(e.message, "Trying to assign value of type 'String' to a variable of type 'int'"));
    CHECK_EQ(e.line, 5);
    e = run_error(fx, "extends Node3D\nfunc f():\n\tvar a = \"a\"\n\th(a)\nfunc h(x: int):\n\treturn x\n");
    CHECK(contains(e.message, "Cannot convert argument 1 from String to int"));
    e = run_error(fx, "extends Node3D\nfunc f():\n\tvar n: Camera3D = $\"../Cube\"\n");
    CHECK(contains(e.message, "variable of type 'Camera3D'"));
}

static void test_strings(Fixture& fx) {
    CHECK_EQ(evs(fx, "\"%s scored %d (%.2f%%)\" % [\"Ann\", 42, 99.5]"), std::string("Ann scored 42 (99.50%)"));
    CHECK_EQ(evs(fx, "\"[%5d|%-4s|%05.1f|%x|%X|%+d]\" % [42, \"ab\", 3.14159, 255, 255, 3]"),
             std::string("[   42|ab  |003.1|ff|FF|+3]"));
    CHECK_EQ(evs(fx, "\"%s\" % [[1, 2]]"), std::string("[1, 2]"));
    CHECK_EQ(evs(fx, "\"%i%%\" % 50"), std::string("50%"));
    CHECK_EQ(evs(fx, "\"%.3f\" % 2"), std::string("2.000"));
    CHECK_EQ(evs(fx, "\"%f\" % 1.5"), std::string("1.500000"));
    CHECK_EQ(evs(fx, "\"%.0f\" % 2.5"), std::string("2"));
    CHECK_EQ(evs(fx, "\"%.2f\" % -0.001"), std::string("0.00"));
    CHECK_EQ(evs(fx, "\"%c\" % 65"), std::string("A"));
    CHECK_EQ(evs(fx, "\"%v\" % Vector3(1, 2, 3)"), std::string("(1.0, 2.0, 3.0)"));
    CHECK_EQ(evs(fx, "\"Hello\".length()"), std::string("5"));
    CHECK_EQ(evs(fx, "\"Hello\".to_upper() + \"Hello\".to_lower()"), std::string("HELLOhello"));
    CHECK(ev(fx, "\"Hello\".begins_with(\"He\") and \"Hello\".ends_with(\"lo\") and \"Hello\".contains(\"ell\")").as_bool());
    CHECK_EQ(ev(fx, "\"Hello\".find(\"l\")").as_int(), Int(2));
    CHECK_EQ(ev(fx, "\"Hello\".find(\"z\")").as_int(), Int(-1));
    CHECK_EQ(evs(fx, "\"Hello\".substr(1, 3)"), std::string("ell"));
    CHECK_EQ(evs(fx, "\"Hello\".substr(2)"), std::string("llo"));
    CHECK_EQ(evs(fx, "\"a,b,,c\".split(\",\")"), std::string("[\"a\", \"b\", \"\", \"c\"]"));
    CHECK_EQ(evs(fx, "\"a,b,,c\".split(\",\", false)"), std::string("[\"a\", \"b\", \"c\"]"));
    CHECK_EQ(evs(fx, "\"  pad \\t\".strip_edges()"), std::string("pad"));
    CHECK_EQ(evs(fx, "\"a-b-c\".replace(\"-\", \"+\")"), std::string("a+b+c"));
    CHECK_EQ(ev(fx, "\"42\".to_int() + \"-7x\".to_int()").as_int(), Int(35));
    CHECK_EQ(ev(fx, "\"2.5\".to_float()").as_real(), 2.5f);
    CHECK(ev(fx, "\"\".is_empty() and not \"a\".is_empty()").as_bool());
    CHECK_EQ(evs(fx, "\"Hello\"[1]"), std::string("e"));
    CHECK_EQ(evs(fx, "\"Hello\"[-1]"), std::string("o"));
    CHECK_EQ(evs(fx, "\", \".join([1, \"a\", 2.0])"), std::string("1, a, 2.0"));
    CHECK_EQ(evs(fx, "\"ab\".repeat(3)"), std::string("ababab"));
    CHECK_EQ(evs(fx, "\"Hello\".left(2) + \"Hello\".right(2)"), std::string("Helo"));
    CHECK_EQ(run_body(fx, "var s = \"cat\"\ns[0] = \"b\"\nreturn s").to_string(), std::string("bat"));
    // str() formatting like Godot 4
    CHECK_EQ(evs(fx, "str(2.0)"), std::string("2.0"));
    CHECK_EQ(evs(fx, "str(0.1)"), std::string("0.1"));
    CHECK_EQ(evs(fx, "str(1.0 / 3.0)"), std::string("0.33333334"));
    CHECK_EQ(evs(fx, "str(100.0)"), std::string("100.0"));
    CHECK_EQ(evs(fx, "str(-2.5e-7)"), std::string("-2.5e-7"));
    CHECK_EQ(evs(fx, "str(12345678.0)"), std::string("12345678.0"));
    CHECK_EQ(evs(fx, "str(Vector3(1, 2, 3))"), std::string("(1.0, 2.0, 3.0)"));
    CHECK_EQ(evs(fx, "str(Vector3(0.5, -1.25, 1e-3))"), std::string("(0.5, -1.25, 0.001)"));
    CHECK_EQ(evs(fx, "str([1, \"a\"])"), std::string("[1, \"a\"]"));
    CHECK_EQ(evs(fx, "str({\"a\": 1, 2: \"b\"})"), std::string("{ \"a\": 1, 2: \"b\" }"));
    CHECK_EQ(evs(fx, "str({})"), std::string("{  }"));
    CHECK_EQ(evs(fx, "str(null, true, 3)"), std::string("<null>true3"));
    CHECK_EQ(evs(fx, "str(INF) + str(-INF)"), std::string("inf-inf"));
    CHECK_EQ(evs(fx, "str(self)"), std::string("Player:<Node3D#" + std::to_string(fx.player) + ">"));
    CHECK_EQ(evs(fx, "str(1, \" \", 2.5)"), std::string("1 2.5"));
}

static void test_types_methods(Fixture& fx) {
    // Vector3
    CHECK_EQ(ev(fx, "Vector3(3, 4, 0).length()").as_real(), 5.0f);
    CHECK_EQ(ev(fx, "Vector3(3, 4, 0).length_squared()").as_real(), 25.0f);
    CHECK_EQ(evs(fx, "Vector3(0, 0, 5).normalized()"), std::string("(0.0, 0.0, 1.0)"));
    CHECK_EQ(evs(fx, "Vector3().normalized()"), std::string("(0.0, 0.0, 0.0)"));
    CHECK_EQ(ev(fx, "Vector3(1, 2, 3).dot(Vector3(4, 5, 6))").as_real(), 32.0f);
    CHECK_EQ(evs(fx, "Vector3(1, 0, 0).cross(Vector3(0, 1, 0))"), std::string("(0.0, 0.0, 1.0)"));
    CHECK_EQ(ev(fx, "Vector3(1, 1, 1).distance_to(Vector3(1, 1, 3))").as_real(), 2.0f);
    CHECK_EQ(ev(fx, "Vector3(1, 1, 1).distance_squared_to(Vector3(1, 1, 3))").as_real(), 4.0f);
    CHECK_EQ(evs(fx, "Vector3(0, 0, 0).lerp(Vector3(2, 4, 6), 0.5)"), std::string("(1.0, 2.0, 3.0)"));
    CHECK_EQ(evs(fx, "Vector3(0, 0, 0).move_toward(Vector3(10, 0, 0), 3)"), std::string("(3.0, 0.0, 0.0)"));
    CHECK_EQ(evs(fx, "Vector3(0, 0, 0).move_toward(Vector3(1, 0, 0), 3)"), std::string("(1.0, 0.0, 0.0)"));
    CHECK_EQ(evs(fx, "Vector3(-1, 2, -3).abs()"), std::string("(1.0, 2.0, 3.0)"));
    CHECK(ev(fx, "Vector3(1, 0, 0).rotated(Vector3.UP, PI / 2).is_equal_approx(Vector3(0, 0, -1))").as_bool());
    CHECK(is_equal(ev(fx, "Vector3(1, 0, 0).angle_to(Vector3(0, 1, 0))").as_real(), 1.5707964f));
    CHECK(ev(fx, "Vector3(0.000001, 0, 0).is_zero_approx()").as_bool());
    CHECK_EQ(evs(fx, "Vector3.FORWARD"), std::string("(0.0, 0.0, -1.0)"));
    CHECK_EQ(evs(fx, "Vector3.ZERO + Vector3.ONE + Vector3.UP + Vector3.DOWN + Vector3.LEFT + Vector3.RIGHT + Vector3.BACK"),
             std::string("(1.0, 1.0, 2.0)"));
    CHECK_EQ(ev(fx, "Vector3(1, 2, 3).y").as_real(), 2.0f);
    CHECK_EQ(ev(fx, "Vector3(1, 2, 3)[2]").as_real(), 3.0f);
    // Color
    CHECK_EQ(evs(fx, "Color(\"#ff8800\")"), std::string("(1, 0.5333, 0, 1)"));
    CHECK_EQ(evs(fx, "Color(\"ff8800\").to_html()"), std::string("ff8800ff"));
    CHECK_EQ(evs(fx, "Color(1, 0.5, 0).to_html(false)"), std::string("ff8000"));
    CHECK_EQ(evs(fx, "Color(1, 0, 0, 0.5)"), std::string("(1, 0, 0, 1)"));
    CHECK_EQ(ev(fx, "Color(0.2, 0.4, 0.6).a").as_real(), 1.0f);
    CHECK_EQ(ev(fx, "Color(0.2, 0.4, 0.6).g").as_real(), 0.4f);
    CHECK_EQ(evs(fx, "Color.BLACK.lerp(Color.WHITE, 0.5)"), std::string("(0.5, 0.5, 0.5, 1)"));
    CHECK_EQ(evs(fx, "Color.ORANGE"), std::string("(1, 0.6471, 0, 1)"));
    CHECK(ev(fx, "Color.YELLOW == Color(1, 1, 0)").as_bool());
    CHECK(ev(fx, "Color.CYAN + Color.MAGENTA == Color(1, 1, 2)").as_bool());
    CHECK(ev(fx, "Color.GRAY.r > 0.7 and Color.GREEN.g == 1 and Color.BLUE.b == 1").as_bool());
    // Array
    CHECK_EQ(run_body(fx, "var a = [3, 1, 2]\na.sort()\nreturn str(a)").to_string(), std::string("[1, 2, 3]"));
    CHECK_EQ(run_body(fx, "var a = [\"b\", \"c\", \"a\"]\na.sort()\na.reverse()\nreturn str(a)").to_string(),
             std::string("[\"c\", \"b\", \"a\"]"));
    CHECK_EQ(run_body(fx,
                      "var a = []\na.append(1)\na.push_back(2)\na.push_front(0)\na.insert(1, 9)\n"
                      "var x = a.pop_back()\nvar y = a.pop_front()\na.remove_at(0)\nreturn str([a, x, y])")
                 .to_string(),
             std::string("[[1], 2, 0]"));
    CHECK_EQ(run_body(fx, "var a = [1, 2, 3, 2]\na.erase(2)\nreturn str(a) + str(a.has(3)) + str(a.find(2)) + str(a.size())").to_string(),
             std::string("[1, 3, 2]true23"));
    CHECK_EQ(run_body(fx, "var a = [5, 6]\nreturn str(a.front()) + str(a.back()) + str(a.is_empty()) + str([].is_empty())").to_string(),
             std::string("56falsetrue"));
    CHECK_EQ(run_body(fx, "var a = [1, [2]]\nvar b = a.duplicate()\nvar c = a.duplicate(true)\nb[0] = 9\nc[1].append(3)\n"
                          "return str(a) + str(b) + str(c)").to_string(),
             std::string("[1, [2]][9, [2]][1, [2, 3]]"));
    CHECK_EQ(run_body(fx, "var a = [1, 2]\na.clear()\nreturn a.size()").as_int(), Int(0));
    CHECK_EQ(run_body(fx, "var a = [1, 2, 3]\nvar b = a\nb.append(4)\nreturn a.size()").as_int(), Int(4)); // reference
    CHECK_EQ(run_body(fx, "var a = [1, 2, 3]\nreturn a[-1] + a[0]").as_int(), Int(4));
    CHECK(run_body(fx, "seed(5)\nvar p = [7, 8, 9].pick_random()\nreturn p in [7, 8, 9]").as_bool());
    CHECK_EQ(evs(fx, "[1, 2, 3, 4].slice(1, 3)"), std::string("[2, 3]"));
    CHECK_EQ(ev(fx, "[3, 9, 2].max() + [3, 9, 2].min()").as_int(), Int(11));
    // Dictionary
    CHECK_EQ(run_body(fx, "var d = {\"a\": 1, 2: \"b\"}\nd[\"c\"] = 3\nd.erase(2)\nreturn str(d.keys()) + str(d.values()) + str(d.size())").to_string(),
             std::string("[\"a\", \"c\"][1, 3]2"));
    CHECK_EQ(run_body(fx, "var d = {\"a\": 1}\nreturn str(d.get(\"a\")) + str(d.get(\"z\")) + str(d.get(\"z\", 5)) + str(d.has(\"a\"))").to_string(),
             std::string("1<null>5true"));
    CHECK_EQ(run_body(fx, "var d = {a = 1, b = 2}\nd.b += 5\nreturn d.a + d[\"b\"]").as_int(), Int(8));
    CHECK_EQ(run_body(fx, "var d = {}\nfor i in 50:\n\td[i] = i * i\nd.erase(10)\nreturn d[49] + d.size()").as_int(), Int(2401 + 49));
    CHECK_EQ(run_body(fx, "var d = {Vector3(1, 2, 3): \"v\", 1.5: \"f\", true: \"t\"}\nreturn d[Vector3(1, 2, 3)] + d[1.5] + d[true]").to_string(),
             std::string("vft"));
    CHECK_EQ(run_body(fx, "var d = {\"x\": [1]}\nvar e = d.duplicate()\ne[\"y\"] = 2\nd.clear()\nreturn str(d.is_empty()) + str(e.size())").to_string(),
             std::string("true2"));
}

static void test_builtins(Fixture& fx) {
    CHECK_EQ(ev(fx, "abs(-3)").as_int(), Int(3));
    CHECK(ev(fx, "abs(-3)").type() == Type::Int);
    CHECK_EQ(ev(fx, "abs(-2.5)").as_real(), 2.5f);
    CHECK_EQ(ev(fx, "sign(-4)").as_int(), Int(-1));
    CHECK_EQ(ev(fx, "floor(2.7) + ceil(2.2) + round(2.5)").as_real(), 8.0f);
    CHECK(ev(fx, "floor(2.7)").type() == Type::Float);
    CHECK_EQ(ev(fx, "floori(2.7)").as_int(), Int(2));
    CHECK_EQ(ev(fx, "sqrt(16)").as_real(), 4.0f);
    CHECK_EQ(ev(fx, "pow(2, 8)").as_real(), 256.0f);
    CHECK(is_equal(ev(fx, "sin(PI / 2) + cos(0) + tan(0)").as_real(), 2.0f));
    CHECK(is_equal(ev(fx, "atan2(1, 1)").as_real(), 0.785398f));
    CHECK(is_equal(ev(fx, "exp(1)").as_real(), 2.7182817f));
    CHECK(is_equal(ev(fx, "log(exp(2))").as_real(), 2.0f));
    CHECK(is_equal(ev(fx, "asin(1) + acos(1) + atan(0)").as_real(), 1.5707964f));
    CHECK(is_equal(ev(fx, "deg_to_rad(180)").as_real(), 3.1415927f));
    CHECK(is_equal(ev(fx, "rad_to_deg(PI)").as_real(), 180.0f));
    CHECK_EQ(ev(fx, "min(3, 1, 2)").as_int(), Int(1));
    CHECK_EQ(ev(fx, "max(3, 1.5)").as_real(), 3.0f);
    CHECK(ev(fx, "max(3, 1.5)").type() == Type::Float);
    CHECK_EQ(ev(fx, "clamp(15, 0, 10)").as_int(), Int(10));
    CHECK_EQ(ev(fx, "clampf(-1.5, 0.0, 1.0)").as_real(), 0.0f);
    CHECK_EQ(ev(fx, "clampi(5, 7, 9)").as_int(), Int(7));
    CHECK_EQ(ev(fx, "lerp(0, 10, 0.25)").as_real(), 2.5f);
    CHECK_EQ(evs(fx, "lerp(Vector3(), Vector3(2, 2, 2), 0.5)"), std::string("(1.0, 1.0, 1.0)"));
    CHECK_EQ(ev(fx, "lerpf(1, 3, 0.5)").as_real(), 2.0f);
    CHECK_EQ(ev(fx, "inverse_lerp(0, 10, 2.5)").as_real(), 0.25f);
    CHECK_EQ(ev(fx, "move_toward(0, 10, 3)").as_real(), 3.0f);
    CHECK_EQ(ev(fx, "move_toward(9, 10, 3)").as_real(), 10.0f);
    CHECK(is_equal(ev(fx, "wrapf(370.0, 0.0, 360.0)").as_real(), 10.0f));
    CHECK_EQ(ev(fx, "wrapi(-1, 0, 5)").as_int(), Int(4));
    CHECK_EQ(ev(fx, "fmod(7.5, 2)").as_real(), 1.5f);
    CHECK_EQ(ev(fx, "fposmod(-1.0, 3.0)").as_real(), 2.0f);
    CHECK_EQ(ev(fx, "posmod(-1, 3)").as_int(), Int(2));
    CHECK(is_equal(ev(fx, "snapped(0.37, 0.25)").as_real(), 0.25f));
    CHECK_EQ(ev(fx, "snapped(13, 5)").as_int(), Int(15));
    CHECK(ev(fx, "is_equal_approx(0.1 + 0.2, 0.3) and is_zero_approx(0.000001)").as_bool());
    CHECK_EQ(ev(fx, "len(\"abc\") + len([1, 2]) + len({1: 2})").as_int(), Int(6));
    CHECK_EQ(evs(fx, "range(3)"), std::string("[0, 1, 2]"));
    CHECK_EQ(evs(fx, "range(2, 5)"), std::string("[2, 3, 4]"));
    CHECK_EQ(evs(fx, "range(10, 0, -4)"), std::string("[10, 6, 2]"));
    CHECK_EQ(ev(fx, "int(\"12\") + int(3.9) + int(true)").as_int(), Int(16));
    CHECK_EQ(ev(fx, "float(\"1.5\") + float(2)").as_real(), 3.5f);
    CHECK(ev(fx, "bool(1) and not bool(0) and bool(\"x\") and not bool(\"\")").as_bool());
    CHECK_EQ(evs(fx, "typeof(1) + typeof(1.0) + typeof(\"\") + typeof(null) + typeof(self) + typeof([])"),
             std::string("intfloatStringNilObjectArray"));
    CHECK(ev(fx, "is_instance_valid(self) and not is_instance_valid(null)").as_bool());
    CHECK_EQ(ev(fx, "TAU / PI").as_real(), 2.0f);
    CHECK(ev(fx, "INF > 1e30 and NAN != NAN").as_bool());
    // deterministic random numbers
    Value r1 = run_body(fx, "seed(42)\nreturn [randi(), randf(), randi_range(1, 6), randf_range(-1, 1)]");
    Value r2 = run_body(fx, "seed(42)\nreturn [randi(), randf(), randi_range(1, 6), randf_range(-1, 1)]");
    CHECK(r1 == r2);
    CHECK(run_body(fx, "seed(1)\nfor i in 200:\n\tvar r = randi_range(1, 6)\n\tif r < 1 or r > 6:\n\t\treturn false\n"
                       "\tvar f = randf()\n\tif f < 0 or f >= 1:\n\t\treturn false\nreturn true")
              .as_bool());
    // print / printerr go through the host
    fx.host.out.clear();
    fx.host.err_out.clear();
    run_body(fx, "print(\"a\", 1, 2.0, [3])\nprinterr(\"bad\")\nprints(1, 2)\nprintt(1, 2)");
    CHECK(fx.host.out.size() == 3 && fx.host.out[0] == "a12.0[3]" && fx.host.out[1] == "1 2" && fx.host.out[2] == "1\t2");
    CHECK(fx.host.err_out.size() == 1 && fx.host.err_out[0] == "bad");
}

static void test_writeback(Fixture& fx) {
    MockObject& p = fx.host.obj(fx.player);
    p.position = Vector3{0, 0, 0};
    run_body(fx, "position.x += 1\nposition.y = 2\nposition.z -= 3\nreturn null");
    CHECK(p.position.x == 1 && p.position.y == 2 && p.position.z == -3);
    run_body(fx, "self.position.x *= 10\nreturn null");
    CHECK(p.position.x == 10);
    run_body(fx, "rotation_degrees.y += 90\nreturn null");
    CHECK(std::fabs(p.rotation.y - 1.5707964f) < 1e-5f);
    run_body(fx, "$\"../Cube\".position.y = 2\nget_node(\"../Cube\").position.z += 5\nreturn null");
    CHECK(fx.host.obj(fx.cube).position.y == 2 && fx.host.obj(fx.cube).position.z == 5);
    run_body(fx, "self.color.r = 0.5\ncolor.g -= 1\nreturn null", "extends MeshInstance3D\n", fx.cube);
    CHECK(fx.host.obj(fx.cube).color.r == 0.5f && fx.host.obj(fx.cube).color.g == 0.0f && fx.host.obj(fx.cube).color.b == 1.0f);
    CHECK_EQ(run_body(fx, "var arr = [Vector3(), Vector3()]\narr[0].x = 1\narr[1].z += 2\nreturn str(arr)").to_string(),
             std::string("[(1.0, 0.0, 0.0), (0.0, 0.0, 2.0)]"));
    CHECK_EQ(run_body(fx, "var d = {\"v\": Vector3()}\nd.v.y = 3\nd[\"v\"].x = 1\nreturn str(d)").to_string(),
             std::string("{ \"v\": (1.0, 3.0, 0.0) }"));
    CHECK_EQ(run_body(fx, "var nested = [[Vector3()]]\nnested[0][0].z = 7\nreturn nested[0][0].z").as_real(), 7.0f);
    CHECK_EQ(run_body(fx, "var v = Vector3(1, 2, 3)\nv.x = 9\nv[1] = 8\nreturn str(v)").to_string(), std::string("(9.0, 8.0, 3.0)"));
    CHECK_EQ(run_body(fx, "pos.x += 4\nreturn pos.x", "extends Node3D\nvar pos := Vector3(1, 0, 0)\n").as_real(), 5.0f);
    run_body(fx, "get_children()[0].name\nvar t = $\"../Target\"\nt.position.x = 11\nt.scale *= 2\nreturn null");
    CHECK(fx.host.obj(fx.target).position.x == 11 && fx.host.obj(fx.target).scale.y == 2);
    run_body(fx, "visible = false\nreturn null");
    CHECK(!p.visible);
    p.visible = true;
    run_body(fx, "translate(Vector3(1, 1, 1))\nrotate_y(0.5)\nreturn null");
    CHECK(p.position.y == 3 && std::fabs(p.rotation.y - (1.5707964f + 0.5f)) < 1e-5f);
    // read-only property
    CHECK(contains(first_error(fx.host, "extends Node\nfunc f():\n\tname = \"x\"\n"), "read-only"));
}

static void test_node_paths(Fixture& fx) {
    CHECK_EQ(evs(fx, "$Label.name"), std::string("Label"));
    CHECK_EQ(evs(fx, "$\"Label\".name"), std::string("Label"));
    CHECK_EQ(evs(fx, "$\"../Cube\".name"), std::string("Cube"));
    CHECK_EQ(evs(fx, "$\"/root/World/Cube\".name"), std::string("Cube"));
    CHECK_EQ(evs(fx, "$\"/World/Camera\".name"), std::string("Camera"));
    CHECK_EQ(evs(fx, "$../Target.name"), std::string("Target"));
    CHECK_EQ(evs(fx, "get_parent().name"), std::string("World"));
    CHECK_EQ(ev(fx, "get_parent().get_child_count()").as_int(), Int(4));
    CHECK_EQ(evs(fx, "get_parent().get_child(0).name"), std::string("Cube"));
    CHECK_EQ(evs(fx, "get_parent().find_child(\"Label\").name"), std::string("Label"));
    CHECK(ev(fx, "get_node_or_null(\"Nope\") == null").as_bool());
    // a node with a space in its name
    ObjectId spaced = fx.host.create("Node", "my node", fx.player);
    CHECK_EQ(evs(fx, "$\"my node\".name"), std::string("my node"));
    CHECK(spaced != 0);
    // A/B paths
    ObjectId deep = fx.host.create("Node3D", "Deep", fx.label);
    CHECK_EQ(evs(fx, "$Label/Deep.name"), std::string("Deep"));
    CHECK(deep != 0);
    // Input singleton
    fx.host.pressed.insert("ui_right");
    CHECK(ev(fx, "Input.is_action_pressed(\"ui_right\") and not Input.is_action_pressed(\"ui_left\")").as_bool());
    CHECK_EQ(ev(fx, "Input.get_axis(\"ui_left\", \"ui_right\")").as_real(), 1.0f);
    fx.host.pressed.clear();
    // queue_free goes to the host
    run_body(fx, "queue_free()\nreturn null", "extends Node\n", fx.label);
    CHECK(fx.host.obj(fx.label).queued);
    // variadic host method
    CHECK_EQ(ev(fx, "sum(1, 2, 3, 4)").as_int(), Int(10));
}

static void test_script_to_script(Fixture& fx) {
    auto enemy = compile_ok(fx.host,
                            "extends Node3D\n"
                            "const MAX_HEALTH = 100\n"
                            "var health := MAX_HEALTH\n"
                            "var hits := 0\n"
                            "var last_attacker = null\n"
                            "func take_damage(amount: int, from = null) -> int:\n"
                            "\thealth -= amount\n"
                            "\thits += 1\n"
                            "\tlast_attacker = from\n"
                            "\treturn health\n"
                            "func is_dead():\n"
                            "\treturn health <= 0\n",
                            "res://enemy.jet");
    auto player = compile_ok(fx.host,
                             "extends Node3D\n"
                             "var target\n"
                             "var log = []\n"
                             "func _ready():\n"
                             "\ttarget = $\"../Target\"\n"
                             "func attack():\n"
                             "\tvar left = target.take_damage(30, self)\n"
                             "\tlog.append(left)\n"
                             "\tlog.append(target.health)\n"
                             "\tlog.append(target.MAX_HEALTH)\n"
                             "\ttarget.hits += 10\n"
                             "\ttarget.position.x = 4\n"
                             "\treturn target.is_dead()\n"
                             "func set_health():\n"
                             "\ttarget.health = 2.9\n"
                             "\treturn target.health\n",
                             "res://player.jet");
    if (!enemy || !player) return;
    CHECK(fx.vm.attach(fx.target, enemy));
    CHECK(fx.vm.attach(fx.player, player));
    CHECK(fx.vm.ready(fx.target));
    CHECK(fx.vm.ready(fx.player));
    Value r;
    CHECK(fx.vm.call(fx.player, "attack", nullptr, 0, &r));
    CHECK(r.type() == Type::Bool && !r.as_bool());
    Value logv;
    CHECK(fx.vm.get_member(fx.player, "log", logv));
    CHECK_EQ(logv.to_string(), std::string("[70, 70, 100]"));
    Value hits, attacker;
    CHECK(fx.vm.get_member(fx.target, "hits", hits) && hits.as_int() == 11);
    CHECK(fx.vm.get_member(fx.target, "last_attacker", attacker) && attacker.as_object() == fx.player);
    CHECK(fx.host.obj(fx.target).position.x == 4);
    CHECK(fx.vm.call(fx.player, "set_health", nullptr, 0, &r));
    CHECK(r.type() == Type::Int && r.as_int() == 2); // typed member converted on dynamic assignment
    for (int i = 0; i < 3; ++i) fx.vm.call(fx.player, "attack", nullptr, 0, &r);
    CHECK(r.as_bool());
    // wrong argument count on a dynamic script call -> runtime error with stack
    auto caller = compile_ok(fx.host, "extends Node\nfunc f():\n\tvar t = $\"../Target\"\n\tt.take_damage()\n", "res://caller.jet");
    if (caller) {
        ObjectId c = fx.host.create("Node", "Caller", fx.world);
        fx.vm.attach(c, caller);
        CHECK(!fx.vm.call(c, "f"));
        auto errs = fx.vm.take_errors();
        CHECK(errs.size() == 1 && contains(errs[0].message, "Invalid call to function 'take_damage'") &&
              contains(errs[0].message, "Expected 1 arguments") && errs[0].line == 4);
    }
    // get_member / set_member
    CHECK(fx.vm.set_member(fx.target, "health", Value(Real(55.7f))));
    Value h;
    CHECK(fx.vm.get_member(fx.target, "health", h) && h.type() == Type::Int && h.as_int() == 55);
    CHECK(!fx.vm.set_member(fx.target, "health", Value("text")));
    CHECK(!fx.vm.set_member(fx.target, "nope", Value(Int(1))));
    CHECK(!fx.vm.set_member(fx.target, "MAX_HEALTH", Value(Int(1))));
    Value mh;
    CHECK(fx.vm.get_member(fx.target, "MAX_HEALTH", mh) && mh.as_int() == 100);
    CHECK(fx.vm.script_of(fx.target) == enemy.get());
    CHECK(fx.vm.call(fx.target, "missing_function")); // not an error
    fx.vm.detach(fx.target);
    fx.vm.detach(fx.player);
    CHECK(!fx.vm.has_instance(fx.target));
}

static void test_ready_order(Fixture& fx) {
    // Engine helper: children are readied before parents (Godot order); _process parents first.
    std::vector<ObjectId> order;
    std::function<void(ObjectId)> post = [&](ObjectId id) {
        for (ObjectId c : fx.host.obj(id).children) post(c);
        order.push_back(id);
    };
    auto parent_script = compile_ok(fx.host,
                                    "extends Node3D\n"
                                    "@onready var child_value = $Label.value\n"
                                    "@onready var label = $Label\n"
                                    "var early = 1\n"
                                    "func _ready():\n"
                                    "\tprint(\"parent ready \", child_value, \" \", label.name)\n"
                                    "func _process(delta):\n"
                                    "\tprint(\"parent process \", delta)\n");
    auto child_script = compile_ok(fx.host,
                                   "extends Node\n"
                                   "var value = 0\n"
                                   "func _ready():\n"
                                   "\tvalue = 42\n"
                                   "\tprint(\"child ready\")\n"
                                   "func _process(delta):\n"
                                   "\tprint(\"child process\")\n");
    if (!parent_script || !child_script) return;
    CHECK(fx.vm.attach(fx.player, parent_script));
    CHECK(fx.vm.attach(fx.label, child_script));
    Value cv;
    CHECK(fx.vm.get_member(fx.player, "child_value", cv) && cv.is_nil()); // @onready not run by attach
    fx.host.out.clear();
    post(fx.player);
    for (ObjectId id : order) CHECK(fx.vm.ready(id));
    CHECK(fx.host.out.size() == 2 && fx.host.out[0] == "child ready" && fx.host.out[1] == "parent ready 42 Label");
    fx.host.out.clear();
    CHECK(fx.vm.process(fx.player, 0.5f));
    CHECK(fx.vm.process(fx.label, 0.5f));
    CHECK(fx.host.out.size() == 2 && fx.host.out[0] == "parent process 0.5" && fx.host.out[1] == "child process");
    fx.vm.detach(fx.player);
    fx.vm.detach(fx.label);
    // attach to an incompatible class
    auto cam_script = compile_ok(fx.host, "extends Camera3D\nfunc _ready():\n\tfov = 60\n");
    if (cam_script) {
        CHECK(!fx.vm.attach(fx.cube, cam_script));
        auto errs = fx.vm.take_errors();
        CHECK(errs.size() == 1 && errs[0].message ==
                                      "Script inherits from native type 'Camera3D', so it can't be assigned to an object of type 'MeshInstance3D'.");
        CHECK(fx.vm.attach(fx.camera, cam_script));
        CHECK(fx.vm.ready(fx.camera));
        CHECK(fx.host.obj(fx.camera).fov == 60);
        fx.vm.detach(fx.camera);
    }
    // a Node3D script can go on a MeshInstance3D (derived class)
    auto n3 = compile_ok(fx.host, "extends Node3D\nfunc _process(delta):\n\tposition.x += delta\n");
    if (n3) {
        CHECK(fx.vm.attach(fx.cube, n3));
        fx.host.obj(fx.cube).position.x = 0;
        CHECK(fx.vm.process(fx.cube, 0.25f));
        CHECK(fx.host.obj(fx.cube).position.x == 0.25f);
        fx.vm.detach(fx.cube);
    }
    CHECK(!fx.vm.attach(9999, n3));
    fx.vm.take_errors();
}

static void test_exports(Fixture& fx) {
    auto s = compile_ok(fx.host,
                        "extends MeshInstance3D\n"
                        "const BASE = 2.0\n"
                        "@export var speed := 90.0\n"
                        "@export_range(0, 10, 0.5) var jump := 2.0\n"
                        "@export_range(1, 5) var lives: int = 3\n"
                        "@export var title = \"Hero\"\n"
                        "@export var tint := Color(1, 0.5, 0)\n"
                        "@export var offset := Vector3(1, 2, 3)\n"
                        "@export var enabled: bool\n"
                        "@export var scaled := BASE * 3\n"
                        "@export var dynamic: float = sin(0.5) + randf()\n"
                        "@export\n"
                        "var count: int = 7.9\n"
                        "var hidden = 1\n"
                        "@onready var later = 5\n");
    if (!s) return;
    const auto& ex = s->exports();
    CHECK_EQ(ex.size(), size_t(10));
    if (ex.size() == 10) {
        CHECK(ex[0].name == "speed" && ex[0].type == Type::Float && ex[0].default_value.as_real() == 90.0f && !ex[0].has_range);
        CHECK(ex[1].name == "jump" && ex[1].has_range && ex[1].min == 0 && ex[1].max == 10 && ex[1].step == 0.5f);
        CHECK(ex[2].name == "lives" && ex[2].type == Type::Int && ex[2].default_value.as_int() == 3 && ex[2].step == 1);
        CHECK(ex[3].name == "title" && ex[3].type == Type::String && ex[3].default_value.as_string() == "Hero");
        CHECK(ex[4].type == Type::Color && ex[4].default_value.as_color().g == 0.5f);
        CHECK(ex[5].type == Type::Vector3 && ex[5].default_value.as_vector3().z == 3);
        CHECK(ex[6].type == Type::Bool && ex[6].default_value.type() == Type::Bool && !ex[6].default_value.as_bool());
        CHECK(ex[7].type == Type::Float && ex[7].default_value.as_real() == 6.0f);
        CHECK(ex[8].type == Type::Float && ex[8].default_value.as_real() == 0.0f); // non-constant -> type default
        CHECK(ex[9].name == "count" && ex[9].type == Type::Int && ex[9].default_value.as_int() == 7 && ex[9].line == 13);
        CHECK(ex[1].line == 4);
    }
    const auto& members = s->members();
    CHECK(members.size() == 13);
    if (members.size() == 13) {
        CHECK(members[0].name == "BASE" && members[0].constant && members[0].type == Type::Float);
        CHECK(members[1].name == "speed" && members[1].exported && members[1].type == Type::Float);
        CHECK(members[11].name == "hidden" && !members[11].exported);
        CHECK(members[12].name == "later" && members[12].onready);
    }
    CHECK(s->base_class() == "MeshInstance3D");
    CHECK(s->path() == "res://test.jet");
    // scene overrides after attach, then initializers do not overwrite them
    CHECK(fx.vm.attach(fx.cube, s));
    CHECK(fx.vm.set_member(fx.cube, "speed", Value(Int(10))));
    Value v;
    CHECK(fx.vm.get_member(fx.cube, "speed", v) && v.type() == Type::Float && v.as_real() == 10.0f);
    CHECK(fx.vm.get_member(fx.cube, "count", v) && v.as_int() == 7);
    fx.vm.detach(fx.cube);
    // function metadata
    auto f = compile_ok(fx.host, "extends Node\nfunc _ready():\n\tpass\nfunc move(dx, dy := 1):\n\tpass\n");
    if (f) {
        CHECK(f->functions().size() == 2 && f->functions()[1].name == "move" && f->functions()[1].arguments.size() == 2 &&
              f->functions()[1].line == 4);
        CHECK(f->has_function("move") && f->has_function("_ready") && !f->has_function("nope"));
        CHECK(f->base_class() == "Node");
    }
}

struct BadCase {
    const char* src;
    int line, col;
    const char* message;
};

static void test_compile_errors(Fixture& fx) {
    const BadCase cases[] = {
        {"extends Node\nfunc f():\n\tprint(foo)\n", 3, 8, "Identifier \"foo\" not declared in the current scope."},
        {"extends Node\nfunc f():\n\tbar()\n", 3, 2, "Function \"bar()\" not found in base self."},
        {"extends Node\nfunc f(a, b):\n\tpass\nfunc g():\n\tf(1)\n", 5, 2, "Too few arguments for \"f()\" call"},
        {"extends Node\nfunc f(a):\n\tpass\nfunc g():\n\tf(1, 2)\n", 5, 2, "Too many arguments for \"f()\" call"},
        {"extends Node\nfunc f():\n\tsqrt(1, 2)\n", 3, 2, "Too many arguments for \"sqrt()\" call"},
        {"extends Node\nfunc f():\n\tif true\n\t\tpass\n", 3, 9, "Expected \":\" after \"if\" condition."},
        {"extends Node\nfunc f():\n\tvar x = (1 + 2\n", 4, 1, "Unclosed \"(\""},
        {"extends Node\nfunc f():\n\tvar s = \"abc\n", 3, 10, "Unterminated string."},
        {"extends Node\nfunc f():\n\tvar s = \"\\q\"\n", 3, 10, "Invalid escape in string."},
        {"extends Node\nfunc f():\n\tif true:\n\tpass\n", 4, 2, "Expected indented block after \"if\"."},
        {"extends Node\nfunc f():\n\t\tpass\n\t  pass\n", 4, 1, "Mixed use of tabs and spaces"},
        {"extends Node\nfunc f():\n        pass\n    pass\n", 4, 5, "Unindent doesn't match the previous indentation level."},
        {"extends Node\nfunc f():\n\tsignal hit\n", 3, 2, "Signals can only be declared at the top level of the script."},
        {"extends Node\nsignal hit\nfunc f():\n\thit()\n", 4, 2, "\"hit\" is a signal: emit it with \"hit.emit()\"."},
        {"extends Node\nvar hit\nsignal hit\n", 3, 8, "The member \"hit\" already exists in this script"},
        {"extends Node\nsignal hit(a = 1)\n", 2, 14, "Signal parameters can't have default values."},
        {"extends Node\nsignal hit\nfunc f():\n\thit = 1\n", 4, 2, "Cannot assign a new value to a signal."},
        {"extends Node\nsignal hit\nfunc f():\n\thit.call()\n", 4, 6, "Function \"call()\" not found in base Signal."},
        {"extends Node\nfunc f():\n\tf.emit()\n", 3, 4, "Function \"emit()\" not found in base Callable."},
        {"extends Node\nsignal hit\nfunc f():\n\thit.connect()\n", 4, 6, "Invalid call to function \"connect()\" in base Signal"},
        {"extends Timer\nsignal timeout\n", 2, 8, "The signal \"timeout\" already exists in parent class Timer."},
        {"extends Node\nfunc f():\n\tawait g()\n", 3, 2, "\"await\" is not supported yet in JetScript."},
        {"extends Node\nclass_name Foo\n", 2, 1, "\"class_name\" is not supported yet in JetScript."},
        {"extends Node\nclass Inner:\n\tpass\n", 2, 1, "Inner classes (\"class\") are not supported yet in JetScript."},
        {"extends Node\nstatic func s():\n\tpass\n", 2, 1, "Static functions and variables (\"static\") are not supported yet"},
        {"extends Node\nvar x = preload\n", 2, 9, "\"preload\" needs a path: preload(\"res://scene.jetscene\")."},
        {"extends Node\nfunc f():\n\tvar l = func(): return 1\n", 3, 10, "Lambdas are not supported yet in JetScript."},
        {"extends Node\nfunc _ready():\n\tsuper()\n", 3, 2, "\"super\" is not supported yet in JetScript."},
        {"extends Node\nfunc f():\n\tbreakpoint\n", 3, 2, "\"breakpoint\" is not supported yet in JetScript."},
        {"extends Node\nfunc f():\n\tyield()\n", 3, 2, "\"yield\" is not supported"},
        {"extends Nope\n", 1, 9, "Could not find base class \"Nope\"."},
        {"extends Node\nvar a\nvar a\n", 3, 5, "The member \"a\" already exists in this script"},
        {"extends Node\nconst K = 1\nfunc f():\n\tK = 2\n", 4, 2, "Cannot assign a new value to a constant."},
        {"extends Node\nfunc f():\n\tvar x: Foo\n", 3, 9, "Could not find type \"Foo\" in the current scope."},
        {"extends Node\nfunc f():\n\tvar x: int = \"s\"\n", 3, 6, "Cannot assign a value of type \"String\""},
        {"extends Node\nfunc f():\n\tbreak\n", 3, 2, "\"break\" can only be used inside a loop."},
        {"extends Node\nfunc f():\n\tvar a = 1\n\tvar a = 2\n", 4, 6, "There is already a variable named \"a\""},
        {"extends Node\nfunc f() -> void:\n\treturn 1\n", 3, 2, "A void function cannot return a value."},
        {"extends Node\nvar x = 1\nextends Node3D\n", 3, 1, "\"extends\" can only be used once."},
        {"extends Node\nconst C = randf()\n", 2, 7, "isn't a constant expression"},
        {"extends Node\n@export var arr = []\n", 2, 13, "Export type \"Array\" is not supported"},
        {"extends Node\n@tool\n", 2, 1, "Annotation \"@tool\" is not supported yet in JetScript."},
        {"extends Node\nfunc f():\n\tvar a: Array[int] = []\n", 3, 14, "Typed arrays are not supported yet in JetScript."},
        {"extends Node\nvar x: int:\n\tset(v):\n\t\tx = v\n", 2, 11, "setters and getters"},
        {"extends Node\nfunc f():\n\tvar x = 0x\n", 3, 10, "Invalid numeric notation."},
        {"extends Node\nfunc f():\n\tvar x = 99999999999999999999\n", 3, 10, "Integer literal is too large."},
        {"extends Node\nfunc f():\n\t1 +\n", 3, 5, "Expected expression"},
        {"extends Node\nfunc f():\n\tposition.x = 1\n", 3, 2, "Identifier \"position\" not declared"},
        {"extends Node3D\nfunc f():\n\tvar c = Vector3.NOPE\n", 3, 18, "Cannot find member \"NOPE\" in base \"Vector3\"."},
        {"extends Node\nfunc f():\n\tvar x = print\n", 3, 10, "Built-in functions can't be used as Callables yet"},
        {"extends Node\nfunc f():\n\tvar x = 1\n\tx()\n", 4, 2, "\"x\" is a variable and can't be called"},
        {"extends Node\nfunc f(a = 1, b):\n\tpass\n", 2, 15, "Cannot have mandatory parameters after optional parameters."},
        {"extends Node\nenum E { A = \"x\" }\n", 2, 10, "Enum values must be constant integers"},
        {"extends Node\nconst A = B\nconst B = A\n", 2, 7, "cyclic reference"},
        {"extends Node\nfunc f():\n\t$Child.x = (\n", 4, 1, "Unclosed \"(\""},
        {"extends Node\nfunc f():\n\tvar x = )\n", 3, 10, "Closing \")\" doesn't have an opening counterpart."},
        {"extends Node\n\tvar x\n", 2, 2, "Unexpected indentation."},
        {"extends Node\nfunc f():\n\tvar x = [1, 2\n\tvar y\n", 4, 2, "Expected closing \"]\" after array elements."},
        {"extends Node\nfunc f():\n\tmatch 1:\n\t\t[1, 2]:\n\t\t\tpass\n", 4, 3, "Array and dictionary patterns are not supported yet"},
        {"extends Node\nfunc f():\n\tvar x = 1 +* 2\n", 3, 13, "Expected expression"},
        {"extends Node\nfunc f():\n\tif x = 1:\n\t\tpass\n", 3, 7, "Expected \":\" after \"if\" condition."},
        {"extends Node\nfunc f():\n\t@onready var x = 1\n", 3, 2, "is not allowed inside a function"},
        {"extends Node\nfunc f():\n\tvar v = Node3D.new()\n", 3, 10, "Static calls on engine classes"},
        {"extends Node\nfunc f():\n\tvar v = 1 is Nope\n", 3, 15, "Could not find type \"Nope\""},
        {"extends Node\nfunc f():\n\t$X\n\tvar q = $\n", 4, 10, "Expected node path after \"$\"."},
        {"extends Node\nvar §x = 1\n", 2, 5, "Invalid character"},
    };
    int index = 0;
    for (const BadCase& c : cases) {
        ++index;
        auto errs = compile_errors(fx.host, c.src);
        ++g_checks;
        if (errs.empty()) {
            ++g_failures;
            std::cerr << "expected compile error #" << index << " for:\n" << c.src;
            continue;
        }
        const Diagnostic& d = errs[0];
        if (!contains(d.message, c.message) || d.line != c.line || d.column != c.col) {
            ++g_failures;
            std::cerr << "compile error case #" << index << " mismatch:\n" << c.src << "expected " << c.line << ":" << c.col
                      << " " << c.message << "\ngot:\n" << dump_errors(errs);
        }
    }
    // warnings do not prevent compilation
    CompileResult w = compile("extends Node\nvar x = 1\nfunc f():\n\tvar x = 2\n\tx + 1\n", "res://w.jet", fx.host);
    CHECK(w.script != nullptr && w.errors.empty() && w.warnings.size() == 2);
    // several errors are collected
    auto many = compile_errors(fx.host, "extends Node\nfunc f():\n\tprint(a)\n\tprint(b)\n\tprint(c)\n");
    CHECK(many.size() == 3 && many[2].line == 5);
}

struct TypeCase {
    const char* body; // function body lines (tab-indented by the helper)
    int line, col;    // expected position (0: no error expected)
    const char* message;
};

static void test_static_types(Fixture& fx) {
    const TypeCase cases[] = {
        // the reported case: compound assignment on a Vector3 host property
        {"var a = 1\nrotation_degrees += 1", 4, 2, "Invalid operands \"Vector3\" and \"int\" for \"+\" operator."},
        {"rotation_degrees.y += 1", 0, 0, ""},
        {"position += Vector3.UP * delta", 0, 0, ""},
        {"position -= Vector3(1, 2, 3) * 0.5", 0, 0, ""},
        {"scale *= 2", 0, 0, ""},
        {"position = 3", 3, 2, "Cannot assign a value of type \"int\" as \"Vector3\"."},
        {"self.position = \"x\"", 3, 2, "Cannot assign a value of type \"String\" as \"Vector3\"."},
        {"visible = 1", 0, 0, ""},
        {"var x: int = \"a\"", 3, 6, "Cannot assign a value of type \"String\" to variable \"x\" with specified type \"int\"."},
        {"var x: int = 1\nx = Vector3()", 4, 2, "Cannot assign a value of type \"Vector3\" as \"int\"."},
        {"var x: float = 1\nx = 2", 0, 0, ""},
        {"var v := Vector3()\nv += 1", 4, 2, "Invalid operands \"Vector3\" and \"int\" for \"+\" operator."},
        {"var v := Vector3()\nv *= 2.5\nv /= 2", 0, 0, ""},
        {"var s := \"a\"\nvar t = s + 1", 4, 12, "Invalid operands \"String\" and \"int\" for \"+\" operator."},
        {"var t = \"a\" + 1", 3, 14, "Invalid operands \"String\" and \"int\" for \"+\" operator."},
        {"var t = 1 + 2.5 + position.x", 0, 0, ""},
        {"var t = Vector3(1, 2, 3) + 1", 3, 27, "Invalid operands \"Vector3\" and \"int\" for \"+\" operator."},
        {"var t = Vector3.ONE * 2.0 + position", 0, 0, ""},
        {"var t = position.length() + 1", 0, 0, ""},
        {"var t = position.normalized() + 1", 3, 32, "Invalid operands \"Vector3\" and \"int\""},
        {"var t = position.lenght()", 3, 19, "Function \"lenght()\" not found in base Vector3."},
        {"var t = position.w", 3, 19, "Cannot find member \"w\" in base \"Vector3\"."},
        {"var t = -\"a\"", 3, 10, "Invalid operand of type \"String\" for unary operator \"-\"."},
        {"var t = scale + rotation", 0, 0, ""},
        {"var t = sin(1.0) + Vector3()", 3, 19, "Invalid operands \"float\" and \"Vector3\""},
        {"var t = str(1) + 2", 3, 17, "Invalid operands \"String\" and \"int\""},
        {"var t = len([1]) * 2", 0, 0, ""},
        {"var n := 5\nvar t = n.x", 4, 12, "Cannot find member \"x\" in base \"int\"."},
        {"var n := 5\nvar t = n[0]", 4, 11, "Cannot use subscript operator on a base of type \"int\"."},
        {"var a := [1, 2]\na += 3", 4, 2, "Invalid operands \"Array\" and \"int\""},
        {"var a := [1, 2]\na += [3]\nvar d := {}\nd[\"k\"] = 1", 0, 0, ""},
        {"for i in Vector3():\n\tpass", 3, 11, "Unable to iterate on value of type \"Vector3\"."},
        {"var t = \"%d\" % [1]", 0, 0, ""},
        {"var t = 1 if delta else \"x\"\nt += 1", 0, 0, ""},
        {"var u = delta + 1\nvar w = u.foo + Vector3()", 0, 0, ""},
        {"take(\"x\")", 3, 7, "Invalid argument for \"take()\" function: argument 1 should be \"int\" but is \"String\"."},
        {"take(2.5)", 0, 0, ""},
        {"var c := Color(1, 0, 0)\nc.r = \"x\"", 4, 2, "Cannot assign a value of type \"String\" as \"float\"."},
        {"position.x = \"x\"", 3, 2, "Cannot assign a value of type \"String\" as \"float\"."},
        {"var t = self + 1", 3, 15, "Invalid operands \"Object\" and \"int\""},
        {"var t = $\"../Cube\".position + 1", 0, 0, ""},
        {"var t = Input.get_axis(\"a\", \"b\") * position", 0, 0, ""},
        {"var t = 3 in \"abc\"", 3, 12, "Invalid operands \"int\" and \"String\" for \"in\" operator."},
        {"var t = PI * 2 + TAU", 0, 0, ""},
    };
    int index = 0;
    for (const TypeCase& c : cases) {
        ++index;
        std::string src = "extends Node3D\nfunc f(delta):\n";
        std::istringstream in(c.body);
        std::string line;
        while (std::getline(in, line)) src += "\t" + line + "\n";
        src += "func take(amount: int):\n\tpass\n";
        CompileResult r = compile(src, "res://types.jet", fx.host);
        ++g_checks;
        if (c.line == 0) {
            if (!r.errors.empty()) {
                ++g_failures;
                std::cerr << "type case #" << index << " should compile:\n" << src << dump_errors(r.errors);
            }
            continue;
        }
        if (r.errors.empty() || r.script || !contains(r.errors[0].message, c.message) || r.errors[0].line != c.line ||
            r.errors[0].column != c.col) {
            ++g_failures;
            std::cerr << "type case #" << index << " mismatch:\n" << src << "expected " << c.line << ":" << c.col << " "
                      << c.message << "\ngot:\n" << dump_errors(r.errors);
        }
    }
    // warnings: unused local variable, integer division
    CompileResult w = compile("extends Node\nfunc f(a: int, b: int):\n\tvar unused = 1\n\tvar _ok = 2\n\treturn a / b\n",
                              "res://warn.jet", fx.host);
    CHECK(w.script != nullptr && w.errors.empty());
    bool unused = false, division = false;
    for (const Diagnostic& d : w.warnings) {
        if (contains(d.message, "\"unused\" is declared but never used") && d.line == 3) unused = true;
        if (contains(d.message, "Integer division") && d.line == 5) division = true;
        CHECK(!contains(d.message, "_ok"));
    }
    CHECK(unused && division);
}

static void test_runtime_errors(Fixture& fx) {
    RuntimeError e = run_error(fx, "extends Node\nfunc f():\n\tvar n = null\n\tvar p = n.position\n");
    CHECK_EQ(e.message, std::string("Invalid access to property or key 'position' on a base object of type 'null instance'."));
    CHECK_EQ(e.line, 4);
    CHECK_EQ(e.function, std::string("f"));
    CHECK_EQ(e.path, std::string("res://err.jet"));
    CHECK_EQ(e.object, fx.player);
    e = run_error(fx, "extends Node\nfunc f():\n\tvar n = null\n\tn.foo()\n");
    CHECK_EQ(e.message, std::string("Cannot call method 'foo' on a null value."));
    e = run_error(fx, "extends Node\nfunc f():\n\tvar a = 0\n\tvar b = 10 / a\n");
    CHECK_EQ(e.message, std::string("Division by zero error in operator '/'."));
    CHECK_EQ(e.line, 4);
    e = run_error(fx, "extends Node\nfunc f():\n\tvar a = 0\n\tvar b = 10 % a\n");
    CHECK_EQ(e.message, std::string("Modulo by zero error in operator '%'."));
    e = run_error(fx, "extends Node\nfunc f():\n\tvar a = [1, 2]\n\treturn a[5]\n");
    CHECK_EQ(e.message, std::string("Out of bounds get index '5' (on base: 'Array')."));
    e = run_error(fx, "extends Node\nfunc f():\n\tvar a = [1, 2]\n\ta[-3] = 0\n");
    CHECK(contains(e.message, "Out of bounds set index '-3'"));
    e = run_error(fx, "extends Node\nfunc f():\n\tvar d = {}\n\treturn d[\"missing\"]\n");
    CHECK(contains(e.message, "Invalid access to key 'missing'"));
    e = run_error(fx, "extends Node3D\nfunc f():\n\tvar c = $\"../Cube\"\n\tc.explode()\n");
    CHECK_EQ(e.message, std::string("Invalid call. Nonexistent function 'explode' in base 'MeshInstance3D'."));
    e = run_error(fx, "extends Node3D\nfunc f():\n\tvar c = $\"../Camera\"\n\treturn c.color\n");
    CHECK_EQ(e.message, std::string("Invalid access to property or key 'color' on a base object of type 'Camera3D'."));
    e = run_error(fx, "extends Node3D\nfunc f():\n\tvar c = $\"../Camera\"\n\tc.wings = 2\n");
    CHECK(contains(e.message, "Invalid assignment of property or key 'wings' with value of type 'int' on a base object of type 'Camera3D'."));
    e = run_error(fx, "extends Node\nfunc f():\n\tvar a = \"a\"\n\treturn a + 1\n");
    CHECK_EQ(e.message, std::string("Invalid operands 'String' and 'int' in operator '+'."));
    e = run_error(fx, "extends Node\nfunc f():\n\tvar v = Vector3()\n\treturn v.nope()\n");
    CHECK_EQ(e.message, std::string("Invalid call. Nonexistent function 'nope' in base 'Vector3'."));
    e = run_error(fx, "extends Node\nfunc f():\n\treturn [].insert(5, 1)\n");
    CHECK(contains(e.message, "Out of bounds index 5 in insert()"));
    e = run_error(fx, "extends Node\nfunc f():\n\treturn \"abc\".substr()\n");
    CHECK(contains(e.message, "Invalid call to function 'substr' in base 'String'. Expected 1 arguments."));
    e = run_error(fx, "extends Node\nfunc f():\n\treturn sqrt(\"x\")\n");
    CHECK(contains(e.message, "Cannot convert argument 1 from String to float"));
    e = run_error(fx, "extends Node\nfunc f():\n\tfail_method()\n");
    CHECK_EQ(e.message, std::string("fail_method always fails."));
    e = run_error(fx, "extends Node\nconst A = [1, 2]\nfunc f():\n\tA.append(3)\n");
    CHECK_EQ(e.message, std::string("Array is in read-only state."));
    e = run_error(fx, "extends Node\nfunc f():\n\tassert(1 > 2, \"math\")\n");
    CHECK_EQ(e.message, std::string("Assertion failed: math"));
    e = run_error(fx, "extends Node\nfunc f():\n\tvar n = 1.5\n\tfor x in n:\n\t\tpass\n");
    CHECK(contains(e.message, "Unable to iterate on object of type 'float'"));
    e = run_error(fx, "extends Node\nfunc f():\n\tvar s = \"%d\" % \"x\"\n");
    CHECK(contains(e.message, "a number is required"));
    e = run_error(fx, "extends Node\nfunc f():\n\tvar d = {[1]: 2}\n");
    CHECK(contains(e.message, "Invalid key type 'Array'"));
    e = run_error(fx, "extends Node3D\nfunc f():\n\tvar p = 5\n\tposition = p\n");
    CHECK(contains(e.message, "Invalid type for property 'position'"));
    // stack trace across script functions, innermost first
    e = run_error(fx,
                  "extends Node\n"
                  "func f():\n"
                  "\tvar x = 1\n"
                  "\tg(x)\n"
                  "func g(v):\n"
                  "\th(v, 0)\n"
                  "func h(a, b):\n"
                  "\treturn a / b\n");
    CHECK_EQ(e.message, std::string("Division by zero error in operator '/'."));
    CHECK_EQ(e.line, 8);
    CHECK_EQ(e.function, std::string("h"));
    CHECK(e.stack.size() == 3);
    if (e.stack.size() == 3) {
        CHECK(e.stack[0].function == "h" && e.stack[0].line == 8);
        CHECK(e.stack[1].function == "g" && e.stack[1].line == 6);
        CHECK(e.stack[2].function == "f" && e.stack[2].line == 4);
        CHECK(e.stack[2].path == "res://err.jet");
    }
    // a failing _process reports and the VM stays usable
    auto s = compile_ok(fx.host, "extends Node\nvar t = 0\nfunc _process(delta):\n\tt += 1\n\tif t == 2:\n\t\tvar z = null\n\t\tz.x = 1\n");
    if (s) {
        fx.vm.attach(fx.label, s);
        CHECK(fx.vm.process(fx.label, 0.1f));
        CHECK(!fx.vm.process(fx.label, 0.1f));
        CHECK(fx.vm.has_errors());
        auto errs = fx.vm.take_errors();
        CHECK(errs.size() == 1 && errs[0].line == 7 && errs[0].function == "_process");
        CHECK(!fx.vm.has_errors());
        CHECK(fx.vm.process(fx.label, 0.1f));
        fx.vm.detach(fx.label);
    }
    // failure inside a member initializer makes attach fail
    auto bad_init = compile_ok(fx.host, "extends Node\nvar a = [1][3]\n");
    if (bad_init) {
        CHECK(!fx.vm.attach(fx.label, bad_init));
        auto errs = fx.vm.take_errors();
        CHECK(errs.size() == 1 && errs[0].function == "@implicit_new" && errs[0].line == 2);
        CHECK(!fx.vm.has_instance(fx.label));
    }
    // calling with the wrong number of arguments from the engine
    auto two = compile_ok(fx.host, "extends Node\nfunc two(a, b):\n\treturn a + b\n");
    if (two) {
        fx.vm.attach(fx.label, two);
        Value args[3] = {Value(Int(1)), Value(Int(2)), Value(Int(3))};
        CHECK(!fx.vm.call(fx.label, "two", args, 1));
        CHECK(!fx.vm.call(fx.label, "two", args, 3));
        Value r;
        CHECK(fx.vm.call(fx.label, "two", args, 2, &r) && r.as_int() == 3);
        auto errs = fx.vm.take_errors();
        CHECK(errs.size() == 2 && contains(errs[0].message, "Expected 2 arguments"));
        fx.vm.detach(fx.label);
    }
}

static void test_limits(Fixture& fx) {
    // instruction budget
    RuntimeError e = run_error(fx, "extends Node\nfunc f():\n\twhile true:\n\t\tpass\n");
    CHECK(contains(e.message, "possible infinite loop"));
    CHECK_EQ(e.line, 3);
    e = run_error(fx, "extends Node\nfunc f():\n\tvar i = 0\n\twhile i >= 0:\n\t\ti += 1\n");
    CHECK(contains(e.message, "possible infinite loop"));
    fx.vm.set_instruction_limit(1000);
    e = run_error(fx, "extends Node\nfunc f():\n\tfor i in 100000:\n\t\tpass\n");
    CHECK(contains(e.message, "instruction limit exceeded") || contains(e.message, "possible infinite loop"));
    fx.vm.set_instruction_limit(5000000);
    CHECK_EQ(run_body(fx, "var s = 0\nfor i in 1000:\n\ts += i\nreturn s").as_int(), Int(499500));
    // recursion
    e = run_error(fx, "extends Node\nfunc f():\n\treturn rec(0)\nfunc rec(n):\n\treturn rec(n + 1)\n");
    CHECK(contains(e.message, "Stack overflow"));
    CHECK_EQ(e.line, 5);
    CHECK(e.stack.size() == 128);
    CHECK_EQ(run_body(fx, "return fib(15)", "extends Node3D\nfunc fib(n):\n\treturn n if n < 2 else fib(n - 1) + fib(n - 2)\n").as_int(),
             Int(610));
    CHECK_EQ(run_body(fx, "return depth(100)", "extends Node3D\nfunc depth(n):\n\treturn 0 if n == 0 else 1 + depth(n - 1)\n").as_int(),
             Int(100));
    // mutual recursion between two script instances is bounded too
    auto ping = compile_ok(fx.host, "extends Node\nvar other\nfunc ping(n):\n\treturn other.ping(n + 1)\n");
    if (ping) {
        ObjectId a = fx.host.create("Node", "A", fx.world), b = fx.host.create("Node", "B", fx.world);
        fx.vm.attach(a, ping);
        fx.vm.attach(b, ping);
        fx.vm.set_member(a, "other", Value::object(b));
        fx.vm.set_member(b, "other", Value::object(a));
        Value arg(Int(0));
        CHECK(!fx.vm.call(a, "ping", &arg, 1));
        auto errs = fx.vm.take_errors();
        CHECK(errs.size() == 1 && contains(errs[0].message, "Stack overflow"));
        fx.vm.detach(a);
        fx.vm.detach(b);
    }
    // nesting depth
    std::string deep = "extends Node\nfunc f():\n\treturn ";
    for (int i = 0; i < 100; ++i) deep += "(";
    deep += "1";
    for (int i = 0; i < 100; ++i) deep += ")";
    deep += "\n";
    CHECK(contains(first_error(fx.host, deep), "nesting depth"));
    std::string ok_nest = "extends Node\nfunc f():\n\treturn ";
    for (int i = 0; i < 40; ++i) ok_nest += "(";
    ok_nest += "1";
    for (int i = 0; i < 40; ++i) ok_nest += ")";
    ok_nest += "\n";
    CHECK(compile(ok_nest, "x", fx.host).errors.empty());
    std::string blocks = "extends Node\nfunc f():\n";
    for (int i = 0; i < 80; ++i) blocks += std::string(size_t(i + 1), '\t') + "if true:\n";
    blocks += std::string(81, '\t') + "pass\n";
    CHECK(contains(first_error(fx.host, blocks), "nesting depth"));
    std::string chain = "extends Node\nfunc f():\n\treturn 1";
    for (int i = 0; i < 200; ++i) chain += " + 1";
    chain += "\n";
    CHECK(contains(first_error(fx.host, chain), "too complex"));
    std::string unary = "extends Node\nfunc f():\n\treturn ";
    for (int i = 0; i < 500; ++i) unary += "-";
    unary += "1\n";
    CHECK(contains(first_error(fx.host, unary), "nesting depth"));
    std::string arrays = "extends Node\nvar a = ";
    for (int i = 0; i < 500; ++i) arrays += "[";
    CHECK(!compile_errors(fx.host, arrays).empty());
    // deeply nested runtime data: destruction and printing must not recurse unboundedly
    CHECK_EQ(run_body(fx, "var a = []\nfor i in 100000:\n\ta = [a]\nvar s = str(a)\nreturn s.length() > 0").as_bool(), true);
    // cycles are allowed but leak under refcounting, so break this one
    CHECK_EQ(run_body(fx, "var a = [1]\na.append(a)\nvar s = str(a)\nvar b = a == a and s.length() > 0\na.clear()\nreturn b").as_bool(), true);
    CHECK_EQ(run_body(fx, "var d = {}\nfor i in 50000:\n\td = {\"n\": d}\nreturn d.size()").as_int(), Int(1));
}

static void test_reentrancy(Fixture& fx) {
    // the host detaches the running instance; execution finishes safely
    auto s = compile_ok(fx.host, "extends Node\nvar v = 1\nfunc f():\n\tdetach_self()\n\tv += 1\n\treturn v\n");
    if (s) {
        fx.vm.attach(fx.label, s);
        Value r;
        CHECK(fx.vm.call(fx.label, "f", nullptr, 0, &r) && r.as_int() == 2);
        CHECK(!fx.vm.has_instance(fx.label));
    }
    // the host calls back into the VM from inside a host method
    auto a = compile_ok(fx.host, "extends Node\nfunc f():\n\treturn call_script($\"../../Target\", \"g\") + 1\n");
    auto b = compile_ok(fx.host, "extends Node3D\nfunc g():\n\treturn 41\n");
    if (a && b) {
        fx.vm.attach(fx.label, a);
        fx.vm.attach(fx.target, b);
        Value r;
        CHECK(fx.vm.call(fx.label, "f", nullptr, 0, &r) && r.as_int() == 42);
        fx.vm.detach(fx.label);
        fx.vm.detach(fx.target);
    }
    // re-attaching replaces the instance, clear() removes everything
    auto c = compile_ok(fx.host, "extends Node\nvar k = 3\n");
    if (c) {
        fx.vm.attach(fx.label, c);
        fx.vm.set_member(fx.label, "k", Value(Int(9)));
        fx.vm.attach(fx.label, c);
        Value k;
        CHECK(fx.vm.get_member(fx.label, "k", k) && k.as_int() == 3);
        fx.vm.clear();
        CHECK(!fx.vm.has_instance(fx.label));
    }
}

static const char* kFuzzSources[] = {
    ("extends Node3D\n@export var speed := 90.0\n@export_range(0, 10, 0.5) var jump := 2.0\nvar elapsed := 0.0\n"
    "@onready var target = $\"../Target\"\nfunc _ready():\n\tprint(\"Hello from \", name)\n"
    "func _process(delta):\n\telapsed += delta\n\trotation_degrees.y += speed * delta\n"
    "\tposition.y = sin(elapsed * 2.0) * 0.5\n\tif Input.is_action_pressed(\"ui_right\"):\n\t\tposition.x += 2.0 * delta\n"),
    ("extends Node\nenum State { IDLE, RUN = 5, JUMP }\nconst MAX = 10.0\nvar d = {\"a\": [1, 2, {\"b\": Vector3(1, 2, 3)}]}\n"
    "func f(x, y := 2, z: float = 1.0) -> int:\n\tmatch x:\n\t\tState.IDLE, 7:\n\t\t\treturn 1\n\t\tvar q:\n\t\t\treturn q\n"
    "\tfor i in range(3):\n\t\twhile i < 2: i += 1\n\treturn int(\"%d %s\" % [x, y])\n"),
    ("extends Node3D\nfunc g():\n\tvar a = [Vector3(), Vector3()]\n\ta[0].x += 1; a[1].y = 2\n\t$\"../Cube\".position.z = 3\n"
    "\tvar s = \"\"\"multi\nline\"\"\" + 'q\\n' + \"\\u00e9\"\n\treturn s if not a.is_empty() else null\n"),
};

static void test_signals(Fixture& fx) {
    auto emitter = compile_ok(fx.host,
                              "extends Node3D\n"
                              "signal health_changed(old_value, new_value: int)\n"
                              "signal died\n"
                              "var health := 3\n"
                              "func hit():\n"
                              "\tvar old = health\n"
                              "\thealth -= 1\n"
                              "\thealth_changed.emit(old, health)\n"
                              "\tif health == 0:\n"
                              "\t\temit_signal(\"died\")\n",
                              "res://emitter.jet");
    auto listener = compile_ok(fx.host,
                               "extends Node3D\n"
                               "var log = []\n"
                               "var player\n"
                               "func _ready():\n"
                               "\tplayer = $\"../Player\"\n"
                               "\tplayer.health_changed.connect(_on_health)\n"
                               "\tplayer.connect(\"died\", _on_died, CONNECT_ONE_SHOT)\n"
                               "\t$\"../Timer\".timeout.connect(_on_timeout)\n"
                               "func _on_health(a, b):\n"
                               "\tlog.append([a, b])\n"
                               "func _on_died():\n"
                               "\tlog.append(\"died\")\n"
                               "func _on_timeout():\n"
                               "\tlog.append(\"tick\")\n"
                               "func state():\n"
                               "\treturn [player.is_connected(\"died\", _on_died), player.health_changed.is_connected(_on_health),\n"
                               "\t\tplayer.has_signal(\"died\"), player.has_signal(\"nope\"), has_signal(\"died\")]\n"
                               "func drop():\n"
                               "\tplayer.health_changed.disconnect(_on_health)\n",
                               "res://listener.jet");
    if (!emitter || !listener) return;
    CHECK_EQ(emitter->signals().size(), size_t(2));
    CHECK(emitter->signals()[0].name == "health_changed" && emitter->signals()[0].arguments.size() == 2 &&
          emitter->signals()[0].arguments[1] == "new_value");
    const ObjectId timer = fx.host.create("Timer", "Timer", fx.world);
    CHECK(fx.vm.attach(fx.player, emitter));
    CHECK(fx.vm.attach(fx.target, listener));
    CHECK(fx.vm.ready(fx.target));
    CHECK(fx.vm.has_connections(fx.player) && fx.vm.has_connections(timer) && !fx.vm.has_connections(fx.target));
    CHECK(fx.vm.is_connected(fx.player, "died", fx.target, "_on_died"));
    for (int i = 0; i < 3; ++i) CHECK(fx.vm.call(fx.player, "hit"));
    fx.vm.emit_signal(timer, "timeout");
    Value log, state;
    CHECK(fx.vm.get_member(fx.target, "log", log));
    CHECK_EQ(log.to_string(), std::string("[[3, 2], [2, 1], [1, 0], \"died\", \"tick\"]"));
    CHECK(fx.vm.call(fx.target, "state", nullptr, 0, &state));
    CHECK_EQ(state.to_string(), std::string("[false, true, true, false, false]")); // one shot: gone after the first
    CHECK(fx.vm.call(fx.target, "drop"));
    CHECK(fx.vm.call(fx.player, "hit"));
    CHECK(fx.vm.get_member(fx.target, "log", log) && log.array_size() == 5);
    fx.vm.forget(fx.target);
    CHECK(!fx.vm.has_connections(timer));
    fx.vm.emit_signal(timer, "timeout");
    CHECK(fx.vm.get_member(fx.target, "log", log) && log.array_size() == 5);
    CHECK(fx.vm.take_errors().empty());

    // The engine connects too; an error in one callee doesn't stop the others.
    std::string error;
    CHECK(fx.vm.connect(timer, "timeout", fx.target, "_on_timeout", 0, error));
    CHECK(!fx.vm.connect(timer, "timeout", fx.target, "_on_timeout", 0, error) && contains(error, "already connected"));
    CHECK(!fx.vm.connect(timer, "nope", fx.target, "_on_timeout", 0, error) && contains(error, "nonexistent signal 'nope'"));
    CHECK(!fx.vm.connect(timer, "timeout", fx.target, "missing", 0, error) && contains(error, "has no method 'missing'"));
    CHECK(fx.vm.connect(timer, "timeout", fx.target, "drop", 0, error)); // drop() fails: nothing is connected any more
    CHECK(fx.vm.connect(timer, "timeout", fx.target, "queue_free", 0, error));
    fx.vm.emit_signal(timer, "timeout");
    auto errs = fx.vm.take_errors();
    CHECK(errs.size() == 1 && contains(errs[0].message, "nonexistent connection"));
    CHECK(fx.vm.get_member(fx.target, "log", log) && log.array_size() == 6);
    CHECK(fx.host.obj(fx.target).queued);
    fx.vm.detach(fx.player);
    fx.vm.detach(fx.target);
    fx.vm.clear();
    CHECK(!fx.vm.has_connections(timer));

    // Callables and Signals as values.
    CHECK_EQ(evs(fx, "sum.call(1, 2, 3)"), std::string("6"));
    CHECK_EQ(evs(fx, "Callable(self, \"sum\").callv([4, 5])"), std::string("9"));
    CHECK_EQ(evs(fx, "[sum.get_method(), sum.get_object() == self, sum.is_valid(), sum.is_null(), sum is Callable]"),
             std::string("[\"sum\", true, true, false, true]"));
    CHECK_EQ(evs(fx, "[typeof(sum), sum == Callable(self, \"sum\"), sum != queue_free, str(sum)]"),
             std::string("[\"Callable\", true, true, \"Player:<Node3D#5>::sum\"]"));
    CHECK_EQ(evs(fx, "{sum: 1}.has(Callable(self, \"sum\"))"), std::string("true"));
    CHECK_EQ(evs(fx, "Callable(self, \"nope\").is_valid()"), std::string("false"));
    CHECK_EQ(run_body(fx, "return [hit.get_name(), hit.get_object() == self, hit is Signal, typeof(hit)]",
                      "extends Node3D\nsignal hit\n").to_string(),
             std::string("[\"hit\", true, true, \"Signal\"]"));
    CHECK_EQ(run_body(fx, "return twice.call(4)", "extends Node3D\nfunc twice(x):\n\treturn x * 2\n").as_int(), Int(8));
    CHECK_EQ(run_body(fx, "var c: Callable = twice\nvar s: Signal = hit\nreturn [c.call(1), s.get_name()]",
                      "extends Node3D\nsignal hit\nfunc twice(x):\n\treturn x * 2\n").to_string(),
             std::string("[2, \"hit\"]"));
    CHECK_EQ(run_body(fx, "return $Label.queue_free.get_object() == $Label").to_string(), std::string("true"));

    // Runtime errors.
    auto err_of = [&](const std::string& body, const std::string& header = "extends Node3D\nsignal hit\n") {
        std::string src = header + "func f():\n";
        std::istringstream in(body);
        std::string line;
        while (std::getline(in, line)) src += "\t" + line + "\n";
        fx.vm.forget(fx.player); // connections outlive the script instance, as they belong to the node
        return run_error(fx, src).message;
    };
    CHECK(contains(err_of("emit_signal(\"nope\")"), "Can't emit non-existing signal \"nope\"."));
    CHECK(contains(err_of("connect(\"hit\", f, CONNECT_DEFERRED)"), "CONNECT_DEFERRED is not supported yet"));
    CHECK(contains(err_of("connect(\"hit\", f)\nconnect(\"hit\", f)"), "is already connected to given callable"));
    CHECK(contains(err_of("hit.disconnect(f)"), "Attempt to disconnect a nonexistent connection"));
    CHECK(contains(err_of("connect(\"hit\", 1)"), "Cannot convert argument 2 from int to Callable."));
    CHECK(contains(err_of("var c = Callable(self, \"nope\")\nc.call()"), "Nonexistent function 'nope'"));
    CHECK(contains(err_of("var s = hit\ns.call()"), "Nonexistent function 'call' in base 'Signal'"));
    fx.vm.clear();

    // A callee that emits again, and signals in saved bytecode.
    auto chain = compile_ok(fx.host,
                            "extends Node3D\n"
                            "signal ping(n)\n"
                            "var total = 0\n"
                            "func _ready():\n"
                            "\tping.connect(_on_ping)\n"
                            "func _on_ping(n):\n"
                            "\ttotal += n\n"
                            "\tif n > 1:\n"
                            "\t\tping.emit(n - 1)\n"
                            "func start():\n"
                            "\tping.emit(4)\n"
                            "\treturn total\n",
                            "res://chain.jet");
    if (!chain) return;
    std::vector<uint8_t> bytes = save_bytecode(*chain);
    CompileResult loaded = load_bytecode(bytes.data(), bytes.size(), fx.host);
    CHECK(loaded.script && loaded.errors.empty());
    if (!loaded.script) return;
    CHECK(loaded.script->signals().size() == 1 && loaded.script->signals()[0].arguments.size() == 1);
    CHECK(fx.vm.attach(fx.cube, loaded.script) && fx.vm.ready(fx.cube));
    Value total;
    CHECK(fx.vm.call(fx.cube, "start", nullptr, 0, &total));
    CHECK_EQ(total.as_int(), Int(10)); // 4 + 3 + 2 + 1
    CHECK(fx.vm.take_errors().empty());
    fx.vm.detach(fx.cube);
    fx.vm.clear();
}

static void test_fuzz(Fixture& fx) {
    size_t compiled = 0;
    // every prefix of every sample
    for (const char* src : kFuzzSources) {
        std::string s(src);
        for (size_t n = 0; n <= s.size(); ++n) {
            CompileResult r = compile(std::string_view(s.data(), n), "res://fuzz.jet", fx.host);
            if (r.script) ++compiled;
            for (const Diagnostic& d : r.errors) {
                CHECK(d.line >= 1);
                CHECK(d.column >= 1);
            }
        }
    }
    // random byte mutations, insertions and deletions (deterministic LCG)
    uint64_t state = 0x1234567ULL;
    auto rnd = [&]() {
        state = state * 6364136223846793005ULL + 1442695040888963407ULL;
        return uint32_t(state >> 33);
    };
    const char alphabet[] = "()[]{}:;,.$@\"'\\\t\n #=+-*/%<>!&|^~0123456789abcxyz_ifelsewhileformatchvarfuncreturn\r\x80\xff";
    int runs = 0;
    for (int iter = 0; iter < 3000; ++iter) {
        std::string s = kFuzzSources[size_t(iter) % 3];
        int edits = 1 + int(rnd() % 6);
        for (int k = 0; k < edits && !s.empty(); ++k) {
            size_t pos = rnd() % s.size();
            switch (rnd() % 4) {
            case 0: s[pos] = alphabet[rnd() % (sizeof(alphabet) - 1)]; break;
            case 1: s.insert(pos, 1, alphabet[rnd() % (sizeof(alphabet) - 1)]); break;
            case 2: s.erase(pos, 1 + rnd() % 8); break;
            default: s.insert(pos, s.substr(rnd() % s.size(), rnd() % 16)); break;
            }
        }
        CompileResult r = compile(s, "res://fuzz.jet", fx.host);
        if (r.script) {
            ++compiled;
            // run whatever compiled, with a tight budget
            ObjectId o = fx.host.create("Node3D", "F", fx.world);
            fx.vm.set_instruction_limit(20000);
            if (fx.vm.attach(o, r.script)) {
                fx.vm.ready(o);
                fx.vm.process(o, 0.016f);
                for (const FunctionInfo& fi : r.script->functions()) {
                    Value args[3] = {Value(Int(1)), Value(2.5), Value("x")};
                    fx.vm.call(o, fi.name, args, int(fi.arguments.size() > 3 ? 3 : fi.arguments.size()));
                }
                ++runs;
            }
            fx.vm.take_errors();
            fx.vm.detach(o);
            fx.vm.set_instruction_limit(5000000);
        }
    }
    // pure garbage
    for (int iter = 0; iter < 500; ++iter) {
        std::string s;
        size_t n = rnd() % 200;
        for (size_t k = 0; k < n; ++k) s.push_back(char(rnd() & 0xFF));
        compile(s, "res://garbage.jet", fx.host);
    }
    CHECK(compiled > 0);
    std::cout << "fuzz: " << compiled << " inputs compiled, " << runs << " executed\n";
}

#if defined(__linux__) && !defined(JS_TEST_ASAN)
// Runs `job` on a thread with a painted stack and returns the peak bytes used.
// glibc keeps its thread descriptor and TLS at the top of that stack, so an empty job's baseline is reported separately.
struct StackJob {
    std::function<void()> fn;
};
static void* run_stack_job(void* p) {
    static_cast<StackJob*>(p)->fn();
    return nullptr;
}
static size_t peak_stack(std::function<void()> fn, size_t size = 256 * 1024) {
    void* mem = nullptr;
    if (posix_memalign(&mem, 65536, size) != 0) return 0;
    std::memset(mem, 0xA5, size);
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstack(&attr, mem, size);
    StackJob job{std::move(fn)};
    pthread_t th;
    bool ok = pthread_create(&th, &attr, run_stack_job, &job) == 0;
    if (ok) pthread_join(th, nullptr);
    pthread_attr_destroy(&attr);
    const unsigned char* p = static_cast<const unsigned char*>(mem);
    size_t untouched = 0;
    while (untouched < size && p[untouched] == 0xA5) ++untouched;
    std::free(mem);
    return ok ? size - untouched : 0;
}
static bool run_with_stack(std::function<void()> fn, size_t stack) {
    StackJob job{std::move(fn)};
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, stack);
    pthread_t th;
    bool ok = pthread_create(&th, &attr, run_stack_job, &job) == 0;
    if (ok) pthread_join(th, nullptr);
    pthread_attr_destroy(&attr);
    return ok;
}
#endif

static void test_small_stack(Fixture& fx) {
#if defined(__linux__) && !defined(JS_TEST_ASAN)
    // worst-case inputs right below the nesting limit (64)
    std::vector<std::pair<const char*, std::string>> inputs;
    std::string s = "extends Node\nfunc f():\n\treturn ";
    for (int i = 0; i < 61; ++i) s += "(-";
    s += "1" + std::string(61, ')') + "\n";
    inputs.emplace_back("parentheses", s);
    s = "extends Node\nfunc f():\n\treturn ";
    for (int i = 0; i < 62; ++i) s += "[";
    s += std::string(62, ']') + "\n";
    inputs.emplace_back("arrays", s);
    s = "extends Node\nfunc f():\n\treturn ";
    for (int i = 0; i < 31; ++i) s += "max(1, ";
    s += "1" + std::string(31, ')') + "\n";
    inputs.emplace_back("calls", s);
    s = "extends Node\nfunc f():\n\tvar a = []\n\treturn ";
    for (int i = 0; i < 31; ++i) s += "a.find(";
    s += "1" + std::string(31, ')') + "\n";
    inputs.emplace_back("method calls", s);
    s = "extends Node\nfunc f():\n\treturn 1";
    for (int i = 0; i < 61; ++i) s += " + 1";
    inputs.emplace_back("operator chain", s + "\n");
    s = "extends Node\nfunc f():\n\tvar x = 0\n";
    for (int i = 0; i < 58; ++i) s += std::string(size_t(i + 1), '\t') + "if x == 1 and not x:\n";
    s += std::string(59, '\t') + "x = [1, {\"a\": (2 + 3)}]\n";
    inputs.emplace_back("blocks", s);
    s = "extends Node\nfunc f():\n\treturn ";
    for (int i = 0; i < 31; ++i) s += "1 if false else ";
    inputs.emplace_back("ternaries", s + "2\n");
    s = "extends Node\nfunc f():\n\treturn ";
    for (int i = 0; i < 5000; ++i) s += "([{";
    inputs.emplace_back("over the limit", s);
    inputs.emplace_back("example script", kFuzzSources[0]);
    size_t base = peak_stack([&]() { compile("", "res://empty.jet", fx.host); });
    size_t worst = 0;
    for (auto& in : inputs) {
        bool ok = false;
        size_t used = peak_stack([&]() {
            CompileResult r = compile(in.second, "res://stack.jet", fx.host);
            ok = std::string(in.first) == "over the limit" ? !r.errors.empty() : r.errors.empty();
        });
        if (!ok) {
            ++g_failures;
            std::cerr << "stack input '" << in.first << "' did not compile as expected\n";
        }
        worst = std::max(worst, used - base);
        std::printf("stack: compile %-15s %6zu bytes\n", in.first, used - base);
    }
    // VM execution: recursion is on the heap; str()/== recurse over nested containers (bounded)
    auto script = compile("extends Node\nfunc rec(n):\n\treturn 0 if n == 0 else 1 + rec(n - 1)\n"
                          "func f():\n\tvar a = []\n\tfor i in 12:\n\t\ta = [a, {\"k\": a}]\n"
                          "\tvar s = str(a)\n\tvar d = a.duplicate(true)\n\tvar e = a == d\n"
                          "\treturn rec(120) + s.length() + int(e) + (\"%5.2f %s\" % [1.5, a]).length()\n",
                          "res://vm.jet", fx.host)
                      .script;
    CHECK(script != nullptr);
    size_t vm_used = 0;
    if (script) {
        fx.vm.attach(fx.label, script);
        Value r;
        bool ok = false;
        vm_used = peak_stack([&]() { ok = fx.vm.call(fx.label, "f", nullptr, 0, &r); }) - base;
        CHECK(ok && r.as_int() > 120);
        if (!ok) std::cerr << dump_runtime(fx.vm.take_errors());
        fx.vm.detach(fx.label);
        std::printf("stack: VM call (recursion 120, nested str/==/duplicate) %zu bytes\n", vm_used);
    }
    CHECK(worst < 24 * 1024);
    CHECK(vm_used < 16 * 1024);
    // and the real thing: a 32 KiB thread stack with a guard page
    for (auto& in : inputs) CHECK(run_with_stack([&]() { compile(in.second, "res://stack.jet", fx.host); }, 32 * 1024));
    std::printf("stack: worst-case compile %zu bytes, thread baseline %zu bytes (x86-64, this build)\n", worst, base);
#else
    (void)fx;
#endif
}

static double seconds_since(std::chrono::steady_clock::time_point t0) {
    return std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
}

static void test_timing(Fixture& fx) {
    auto s = compile_ok(fx.host,
                        "extends Node3D\n"
                        "var speed := 2.0\n"
                        "func bench_while():\n"
                        "\tvar x := 0.0\n"
                        "\tvar i := 0\n"
                        "\twhile i < 1000000:\n"
                        "\t\tx += i * 0.5\n"
                        "\t\ti += 1\n"
                        "\treturn x\n"
                        "func bench_for():\n"
                        "\tvar x := 0.0\n"
                        "\tfor i in range(1000000):\n"
                        "\t\tx = x * 0.999 + sin(0.001 * i)\n"
                        "\treturn x\n"
                        "func _process(delta):\n"
                        "\tposition.x += speed * delta\n"
                        "\trotation_degrees.y += 90.0 * delta\n");
    if (!s) return;
    fx.vm.attach(fx.player, s);
    fx.vm.set_instruction_limit(100000000);
    Value r;
    auto t0 = std::chrono::steady_clock::now();
    CHECK(fx.vm.call(fx.player, "bench_while", nullptr, 0, &r));
    double t_while = seconds_since(t0);
    CHECK(r.as_real() > 2.4e11f && r.as_real() < 2.6e11f);
    t0 = std::chrono::steady_clock::now();
    CHECK(fx.vm.call(fx.player, "bench_for", nullptr, 0, &r));
    double t_for = seconds_since(t0);
    fx.host.obj(fx.player).position = Vector3{0, 0, 0};
    t0 = std::chrono::steady_clock::now();
    for (int i = 0; i < 100000; ++i) fx.vm.process(fx.player, 0.01f);
    double t_proc = seconds_since(t0);
    CHECK(std::fabs(fx.host.obj(fx.player).position.x - 2000.0f) < 5.0f);
    fx.vm.set_instruction_limit(5000000);
    fx.vm.detach(fx.player);
    std::printf("timing: 1M-iteration while loop (float math) %.1f ms (%.1f ns/iter)\n", t_while * 1e3, t_while * 1e3);
    std::printf("timing: 1M-iteration for range loop with sin() %.1f ms\n", t_for * 1e3);
    std::printf("timing: 100k _process calls (position.x += speed * delta; rotation_degrees.y += ...) %.1f ms (%.2f us/call)\n",
                t_proc * 1e3, t_proc * 1e6 / 100000.0);
}

int main() {
    std::cout << "JetScript tests (sizeof(Value) = " << sizeof(Value) << ")\n";
    test_values();
    {
        Fixture fx;
        test_lexer(fx);
        test_operators(fx);
        test_statements(fx);
        test_match(fx);
        test_typed(fx);
        test_strings(fx);
        test_types_methods(fx);
        test_builtins(fx);
        test_writeback(fx);
        test_node_paths(fx);
    }
    {
        Fixture fx;
        test_script_to_script(fx);
        test_ready_order(fx);
        test_exports(fx);
        test_compile_errors(fx);
        test_static_types(fx);
        test_runtime_errors(fx);
        test_limits(fx);
        test_reentrancy(fx);
    }
    {
        Fixture fx;
        test_signals(fx);
    }
    {
        Fixture fx;
        test_fuzz(fx);
        test_small_stack(fx);
        test_timing(fx);
    }
    if (g_failures) {
        std::cout << "FAIL: " << g_failures << " of " << g_checks << " checks failed\n";
        return 1;
    }
    std::cout << "PASS: " << g_checks << " checks\n";
    return 0;
}
