#include "script_host.hpp"
#include "physics.hpp"
#include <algorithm>
#include <cmath>

namespace jetstudio {
namespace {
using jetscript::ClassInfo;
using jetscript::ObjectId;
using jetscript::Status;
using jetscript::Type;
using jetscript::Value;

enum Property : int {
    Name, Position, Rotation, RotationDegrees, Scale, Visible, GlobalPosition,
    MeshColor, Fov, Near, Far, Current, LightColor, LightEnergy, BackgroundColor, AmbientColor,
    MeshAlpha, UvOffset, UvScale, PerspectiveCorrect, TextureFilter, Text, Size,
    Velocity, LinearVelocity, AngularVelocity, Mass, GravityScale, Freeze, Sleeping, FloorMaxAngle, Monitoring, Disabled,
    CollisionLayer, CollisionMask, Friction, Bounce,
    Emitting, Amount, Lifetime, OneShot, Explosiveness, LifetimeRandomness, Direction, Spread, Gravity,
    InitialVelocityMin, InitialVelocityMax, DampingMin, DampingMax, CurrentScene,
    Frame, Hframes, Vframes, FlipH, FlipV, PixelSize, Modulate, ZIndex, Centered, Offset, Animation, SpeedScale, Zoom, Enabled,
    OneWayCollision, WaitTime, Autostart, Paused, TimeLeft, PropertyCount
};
const char* const property_names[PropertyCount] = {
    "name", "position", "rotation", "rotation_degrees", "scale", "visible", "global_position",
    "color", "fov", "near", "far", "current", "light_color", "light_energy", "background_color", "ambient_color",
    "alpha", "uv_offset", "uv_scale", "perspective_correct", "texture_filter", "text", "size",
    "velocity", "linear_velocity", "angular_velocity", "mass", "gravity_scale", "freeze", "sleeping", "floor_max_angle", "monitoring",
    "disabled", "collision_layer", "collision_mask", "friction", "bounce",
    "emitting", "amount", "lifetime", "one_shot", "explosiveness", "lifetime_randomness", "direction", "spread", "gravity",
    "initial_velocity_min", "initial_velocity_max", "damping_min", "damping_max", "current_scene",
    "frame", "hframes", "vframes", "flip_h", "flip_v", "pixel_size", "modulate", "z_index", "centered", "offset", "animation",
    "speed_scale", "zoom", "enabled", "one_way_collision", "wait_time", "autostart", "paused", "time_left"};

enum Method : int {
    GetNode, GetNodeOrNull, GetParent, GetChildren, GetChild, GetChildCount, FindChild, QueueFree,
    Translate, RotateX, RotateY, RotateZ, LookAt, Show, Hide, IsVisibleInTree, MakeCurrent,
    IsActionPressed, IsActionJustPressed, IsActionJustReleased, GetAxis,
    MoveAndSlide, IsOnFloor, IsOnWall, IsOnCeiling, GetFloorNormal, ApplyCentralImpulse, ApplyImpulse, ApplyCentralForce,
    GetOverlappingBodies, HasOverlappingBodies, OverlapsBody, Restart, GetTree, ChangeSceneToFile, ReloadCurrentScene,
    AddChild, RemoveChild, Instantiate, Rotate, Play, Stop, IsPlaying, GetOverlappingAreas, HasOverlappingAreas, OverlapsArea,
    Start, IsStopped,
    MethodCount
};
const char* const method_names[MethodCount] = {
    "get_node", "get_node_or_null", "get_parent", "get_children", "get_child", "get_child_count", "find_child", "queue_free",
    "translate", "rotate_x", "rotate_y", "rotate_z", "look_at", "show", "hide", "is_visible_in_tree", "make_current",
    "is_action_pressed", "is_action_just_pressed", "is_action_just_released", "get_axis",
    "move_and_slide", "is_on_floor", "is_on_wall", "is_on_ceiling", "get_floor_normal", "apply_central_impulse", "apply_impulse",
    "apply_central_force", "get_overlapping_bodies", "has_overlapping_bodies", "overlaps_body", "restart", "get_tree",
    "change_scene_to_file", "reload_current_scene", "add_child", "remove_child", "instantiate", "rotate", "play", "stop", "is_playing",
    "get_overlapping_areas", "has_overlapping_areas", "overlaps_area", "start", "is_stopped"};

const std::vector<ClassInfo>& class_list() {
    static const std::vector<ClassInfo> classes = [] {
        std::vector<ClassInfo> c;
        auto add = [&](const char* name, const char* base, std::vector<jetscript::PropertyInfo> properties,
                       std::vector<jetscript::MethodInfo> methods, std::vector<jetscript::SignalInfo> signals = {}) {
            c.push_back({name, base, std::move(properties), std::move(methods), std::move(signals)});
        };
        const std::vector<jetscript::SignalInfo> area_signals = {
            {"body_entered", {"body"}}, {"body_exited", {"body"}}, {"area_entered", {"area"}}, {"area_exited", {"area"}}};
        add("Node", "", {{"name", Type::String, true}},
            {{"get_node", 1, 1, Type::Object}, {"get_node_or_null", 1, 1, Type::Object}, {"get_parent", 0, 0, Type::Object},
             {"get_children", 0, 0, Type::Array}, {"get_child", 1, 1, Type::Object}, {"get_child_count", 0, 0, Type::Int},
             {"find_child", 1, 1, Type::Object}, {"queue_free", 0, 0, Type::Nil}, {"get_tree", 0, 0, Type::Object},
             {"add_child", 1, 2, Type::Nil}, {"remove_child", 1, 1, Type::Nil},
             // run by the VM, listed for the editor's completion
             {"connect", 2, 3, Type::Int}, {"disconnect", 2, 2, Type::Nil}, {"is_connected", 2, 2, Type::Bool},
             {"emit_signal", 1, -1, Type::Int}, {"has_signal", 1, 1, Type::Bool}});
        add("Node3D", "Node",
            {{"position", Type::Vector3}, {"rotation", Type::Vector3}, {"rotation_degrees", Type::Vector3}, {"scale", Type::Vector3},
             {"visible", Type::Bool}, {"global_position", Type::Vector3}},
            {{"translate", 1, 1}, {"rotate_x", 1, 1}, {"rotate_y", 1, 1}, {"rotate_z", 1, 1}, {"look_at", 1, 2},
             {"show", 0, 0}, {"hide", 0, 0}, {"is_visible_in_tree", 0, 0, Type::Bool}});
        add("MeshInstance3D", "Node3D", {{"color", Type::Color}, {"alpha", Type::Float}, {"uv_offset", Type::Vector3},
            {"uv_scale", Type::Vector3}, {"perspective_correct", Type::Bool}, {"texture_filter", Type::Int}}, {});
        add("Camera3D", "Node3D", {{"fov", Type::Float}, {"near", Type::Float}, {"far", Type::Float}, {"current", Type::Bool}},
            {{"make_current", 0, 0}});
        add("DirectionalLight3D", "Node3D", {{"light_color", Type::Color}, {"light_energy", Type::Float}}, {});
        add("WorldEnvironment", "Node", {{"background_color", Type::Color}, {"ambient_color", Type::Color}}, {});
        // Controls: position and size are screen pixels (x, y)
        add("Control", "Node", {{"position", Type::Vector2}, {"size", Type::Vector2}, {"visible", Type::Bool}, {"alpha", Type::Float}},
            {{"show", 0, 0}, {"hide", 0, 0}});
        add("Label", "Control", {{"text", Type::String}, {"color", Type::Color}}, {});
        add("TextureRect", "Control", {{"color", Type::Color}}, {});
        add("ColorRect", "Control", {{"color", Type::Color}}, {});
        // Physics, with Godot's class hierarchy
        add("CollisionObject3D", "Node3D", {{"collision_layer", Type::Int}, {"collision_mask", Type::Int}}, {});
        add("PhysicsBody3D", "CollisionObject3D", {}, {});
        add("StaticBody3D", "PhysicsBody3D", {{"friction", Type::Float}, {"bounce", Type::Float}}, {});
        add("CharacterBody3D", "PhysicsBody3D", {{"velocity", Type::Vector3}, {"floor_max_angle", Type::Float}},
            {{"move_and_slide", 0, 0, Type::Bool}, {"is_on_floor", 0, 0, Type::Bool}, {"is_on_wall", 0, 0, Type::Bool},
             {"is_on_ceiling", 0, 0, Type::Bool}, {"get_floor_normal", 0, 0, Type::Vector3}});
        add("RigidBody3D", "PhysicsBody3D",
            {{"linear_velocity", Type::Vector3}, {"angular_velocity", Type::Vector3}, {"mass", Type::Float}, {"gravity_scale", Type::Float},
             {"freeze", Type::Bool}, {"sleeping", Type::Bool}, {"friction", Type::Float}, {"bounce", Type::Float}},
            {{"apply_central_impulse", 1, 1}, {"apply_impulse", 1, 2}, {"apply_central_force", 1, 1}});
        add("Area3D", "CollisionObject3D", {{"monitoring", Type::Bool}},
            {{"get_overlapping_bodies", 0, 0, Type::Array}, {"has_overlapping_bodies", 0, 0, Type::Bool}, {"overlaps_body", 1, 1, Type::Bool},
             {"get_overlapping_areas", 0, 0, Type::Array}, {"has_overlapping_areas", 0, 0, Type::Bool}, {"overlaps_area", 1, 1, Type::Bool}},
            area_signals);
        add("CollisionShape3D", "Node3D", {{"disabled", Type::Bool}}, {});
        add("CPUParticles3D", "Node3D",
            {{"emitting", Type::Bool}, {"amount", Type::Int}, {"lifetime", Type::Float}, {"one_shot", Type::Bool},
             {"explosiveness", Type::Float}, {"lifetime_randomness", Type::Float}, {"direction", Type::Vector3}, {"spread", Type::Float},
             {"gravity", Type::Vector3}, {"initial_velocity_min", Type::Float}, {"initial_velocity_max", Type::Float},
             {"damping_min", Type::Float}, {"damping_max", Type::Float}, {"color", Type::Color}, {"alpha", Type::Float}},
            {{"restart", 0, 0}}, {{"finished", {}}});
        add("Sprite3D", "Node3D",
            {{"frame", Type::Int}, {"hframes", Type::Int}, {"vframes", Type::Int}, {"flip_h", Type::Bool}, {"flip_v", Type::Bool},
             {"pixel_size", Type::Float}, {"modulate", Type::Color}}, {});
        add("PackedScene", "", {}, {{"instantiate", 0, 0, Type::Object}});
        // 2D, Godot's classes: positions in canvas pixels (y down), rotation in radians, clockwise.
        add("Node2D", "Node",
            {{"position", Type::Vector2}, {"rotation", Type::Float}, {"rotation_degrees", Type::Float}, {"scale", Type::Vector2},
             {"global_position", Type::Vector2}, {"visible", Type::Bool}, {"z_index", Type::Int}, {"modulate", Type::Color},
             {"alpha", Type::Float}},
            {{"translate", 1, 1}, {"rotate", 1, 1}, {"look_at", 1, 1}, {"show", 0, 0}, {"hide", 0, 0},
             {"is_visible_in_tree", 0, 0, Type::Bool}});
        add("Sprite2D", "Node2D",
            {{"frame", Type::Int}, {"hframes", Type::Int}, {"vframes", Type::Int}, {"flip_h", Type::Bool}, {"flip_v", Type::Bool},
             {"centered", Type::Bool}, {"offset", Type::Vector2}}, {});
        add("AnimatedSprite2D", "Node2D",
            {{"frame", Type::Int}, {"flip_h", Type::Bool}, {"flip_v", Type::Bool}, {"centered", Type::Bool}, {"offset", Type::Vector2},
             {"animation", Type::String}, {"speed_scale", Type::Float}},
            {{"play", 0, 1}, {"stop", 0, 0}, {"is_playing", 0, 0, Type::Bool}}, {{"animation_finished", {}}});
        add("Camera2D", "Node2D", {{"offset", Type::Vector2}, {"zoom", Type::Vector2}, {"enabled", Type::Bool}},
            {{"make_current", 0, 0}});

        // 2D physics: the same bodies in pixels, y down.
        add("CollisionObject2D", "Node2D", {{"collision_layer", Type::Int}, {"collision_mask", Type::Int}}, {});
        add("PhysicsBody2D", "CollisionObject2D", {}, {});
        add("StaticBody2D", "PhysicsBody2D", {{"friction", Type::Float}, {"bounce", Type::Float}}, {});
        add("CharacterBody2D", "PhysicsBody2D", {{"velocity", Type::Vector2}, {"floor_max_angle", Type::Float}},
            {{"move_and_slide", 0, 0, Type::Bool}, {"is_on_floor", 0, 0, Type::Bool}, {"is_on_wall", 0, 0, Type::Bool},
             {"is_on_ceiling", 0, 0, Type::Bool}, {"get_floor_normal", 0, 0, Type::Vector2}});
        add("RigidBody2D", "PhysicsBody2D",
            {{"linear_velocity", Type::Vector2}, {"angular_velocity", Type::Float}, {"mass", Type::Float}, {"gravity_scale", Type::Float},
             {"freeze", Type::Bool}, {"sleeping", Type::Bool}, {"friction", Type::Float}, {"bounce", Type::Float}},
            {{"apply_central_impulse", 1, 1}, {"apply_impulse", 1, 2}, {"apply_central_force", 1, 1}});
        add("Area2D", "CollisionObject2D", {{"monitoring", Type::Bool}},
            {{"get_overlapping_bodies", 0, 0, Type::Array}, {"has_overlapping_bodies", 0, 0, Type::Bool}, {"overlaps_body", 1, 1, Type::Bool},
             {"get_overlapping_areas", 0, 0, Type::Array}, {"has_overlapping_areas", 0, 0, Type::Bool}, {"overlaps_area", 1, 1, Type::Bool}},
            area_signals);
        add("CollisionShape2D", "Node2D", {{"disabled", Type::Bool}, {"one_way_collision", Type::Bool}}, {});
        add("Timer", "Node",
            {{"wait_time", Type::Float}, {"one_shot", Type::Bool}, {"autostart", Type::Bool}, {"paused", Type::Bool},
             {"time_left", Type::Float, true}},
            {{"start", 0, 1}, {"stop", 0, 0}, {"is_stopped", 0, 0, Type::Bool}}, {{"timeout", {}}});
        add("SceneTree", "", {{"current_scene", Type::Object, true}},
            {{"change_scene_to_file", 1, 1, Type::Int}, {"reload_current_scene", 0, 0, Type::Int}});
        add("Input", "", {},
            {{"is_action_pressed", 1, 1, Type::Bool}, {"is_action_just_pressed", 1, 1, Type::Bool},
             {"is_action_just_released", 1, 1, Type::Bool}, {"get_axis", 2, 2, Type::Float}});
        return c;
    }();
    return classes;
}

jetscript::Color to_color(uint32_t c) { return {((c >> 16) & 255) / 255.0f, ((c >> 8) & 255) / 255.0f, (c & 255) / 255.0f}; }
uint32_t from_color(jetscript::Color c) {
    auto channel = [](float v) { return uint32_t(std::lround(std::clamp(std::isfinite(v) ? v : 0.0f, 0.0f, 1.0f) * 255)); };
    return channel(c.r) << 16 | channel(c.g) << 8 | channel(c.b);
}
Vec3 to_vec(jetscript::Vector3 v) { return {v.x, v.y, v.z}; }
jetscript::Vector3 from_vec(Vec3 v) { return {v.x, v.y, v.z}; }

bool has_property(NodeType type, int p) {
    switch (p) {
    case Name: return true;
    case Position: case Visible: return is_node3d(type) || is_control(type) || is_node2d(type);
    case Rotation: case RotationDegrees: case Scale: case GlobalPosition: return is_node3d(type) || is_node2d(type);
    case MeshColor: return type == NodeType::MeshInstance3D || type == NodeType::CPUParticles3D || is_control(type);
    case MeshAlpha: return type == NodeType::MeshInstance3D || type == NodeType::CPUParticles3D || is_control(type) || is_node2d(type);
    case ZIndex: return is_node2d(type);
    case Centered: return type == NodeType::Sprite2D || type == NodeType::AnimatedSprite2D;
    case Offset: return type == NodeType::Sprite2D || type == NodeType::AnimatedSprite2D || type == NodeType::Camera2D;
    case Animation: case SpeedScale: return type == NodeType::AnimatedSprite2D;
    case Zoom: case Enabled: return type == NodeType::Camera2D;
    case UvOffset: case UvScale: case PerspectiveCorrect: case TextureFilter: return type == NodeType::MeshInstance3D;
    case Text: return type == NodeType::Label;
    case Size: return is_control(type);
    case Fov: case Near: case Far: case Current: return type == NodeType::Camera3D;
    case LightColor: case LightEnergy: return type == NodeType::DirectionalLight3D;
    case BackgroundColor: case AmbientColor: return type == NodeType::WorldEnvironment;
    case Velocity: case FloorMaxAngle: return physics::is_character(type);
    case LinearVelocity: case AngularVelocity: case Mass: case GravityScale: case Freeze: case Sleeping: return physics::is_rigid(type);
    case Friction: case Bounce:
        return physics::is_rigid(type) || type == NodeType::StaticBody3D || type == NodeType::StaticBody2D;
    case Monitoring: return physics::is_area(type);
    case Disabled: return physics::is_collision_shape(type);
    case OneWayCollision: return type == NodeType::CollisionShape2D;
    case CollisionLayer: case CollisionMask: return physics::is_collision_object(type);
    case OneShot: return type == NodeType::CPUParticles3D || type == NodeType::Timer;
    case WaitTime: case Autostart: case Paused: case TimeLeft: return type == NodeType::Timer;
    case Emitting: case Amount: case Lifetime: case Explosiveness: case LifetimeRandomness: case Direction:
    case Spread: case Gravity: case InitialVelocityMin: case InitialVelocityMax: case DampingMin: case DampingMax:
        return type == NodeType::CPUParticles3D;
    case Frame: case FlipH: case FlipV: return type == NodeType::Sprite3D || type == NodeType::Sprite2D || type == NodeType::AnimatedSprite2D;
    case Hframes: case Vframes: return type == NodeType::Sprite3D || type == NodeType::Sprite2D;
    case PixelSize: return type == NodeType::Sprite3D;
    case Modulate: return type == NodeType::Sprite3D || is_node2d(type);
    }
    return false;
}
bool node_has_method(NodeType type, int m) {
    if (m <= QueueFree || m == GetTree || m == AddChild || m == RemoveChild) return true;
    if (m == Show || m == Hide) return is_node3d(type) || is_control(type) || is_node2d(type);
    if (m == Translate || m == LookAt || m == IsVisibleInTree || m == Rotate) return is_node3d(type) ? m != Rotate : is_node2d(type);
    if (m <= IsVisibleInTree) return is_node3d(type);
    if (m == MakeCurrent) return type == NodeType::Camera3D || type == NodeType::Camera2D;
    if (m == Play || m == IsPlaying) return type == NodeType::AnimatedSprite2D;
    if (m == Stop) return type == NodeType::AnimatedSprite2D || type == NodeType::Timer;
    if (m == Start || m == IsStopped) return type == NodeType::Timer;
    if (m >= MoveAndSlide && m <= GetFloorNormal) return physics::is_character(type);
    if (m >= ApplyCentralImpulse && m <= ApplyCentralForce) return physics::is_rigid(type);
    if ((m >= GetOverlappingBodies && m <= OverlapsBody) || (m >= GetOverlappingAreas && m <= OverlapsArea)) return physics::is_area(type);
    if (m == Restart) return type == NodeType::CPUParticles3D;
    return false;
}
} // namespace

ScriptHost::ScriptHost(Runtime* runtime) : runtime_(runtime) {}

const std::vector<jetscript::ClassInfo>& ScriptHost::classes() const { return class_list(); }

const jetscript::ClassInfo* ScriptHost::find_class(std::string_view name) const {
    for (const auto& c : class_list()) if (c.name == name) return &c;
    return nullptr;
}

// load()/preload(): a scene of the package as a PackedScene, by path.
ObjectId ScriptHost::resource(std::string_view path) const {
    if (!runtime_) return 0;
    if (path == runtime_->description_.path) return SceneIdBase;
    const auto& scenes = runtime_->description_.scenes;
    for (size_t i = 0; i < scenes.size(); ++i)
        if (scenes[i].path == path) return SceneIdBase + 1 + ObjectId(i);
    return 0;
}

std::string_view ScriptHost::scene_path(ObjectId id) const {
    if (!runtime_ || id < SceneIdBase || id >= AutoloadIdBase) return {};
    const size_t i = id - SceneIdBase;
    const auto& d = runtime_->description_;
    if (i == 0) return d.path;
    return i <= d.scenes.size() ? std::string_view(d.scenes[i - 1].path) : std::string_view();
}

std::string_view ScriptHost::class_of(ObjectId id) const {
    if (id == InputId) return "Input";
    if (id == TreeId) return "SceneTree";
    if (!scene_path(id).empty()) return "PackedScene";
    if (!runtime_) return {};
    const int index = runtime_->index_of(id);
    if (index < 0 || runtime_->nodes_[index].freed) return {};
    return node_type_name(runtime_->nodes_[index].d.type);
}

int ScriptHost::property_id(std::string_view name) const {
    for (int i = 0; i < PropertyCount; ++i) if (name == property_names[i]) return i;
    return -1;
}
int ScriptHost::method_id(std::string_view name) const {
    for (int i = 0; i < MethodCount; ++i) if (name == method_names[i]) return i;
    return -1;
}
ObjectId ScriptHost::singleton(std::string_view name) const {
    if (name == "Input") return InputId;
    if (runtime_) {
        const auto& autoloads = runtime_->description_.autoloads;
        for (size_t i = 0; i < autoloads.size(); ++i)
            if (autoloads[i].name == name) return AutoloadIdBase + uint32_t(i);
    }
    for (size_t i = 0; i < autoload_names_.size(); ++i)
        if (autoload_names_[i] == name) return AutoloadIdBase + uint32_t(i);
    return 0;
}

// From the node types, so the device never builds the class list (it's for the compiler).
bool ScriptHost::has_signal(ObjectId id, std::string_view name) const {
    if (!runtime_) return false;
    const int index = runtime_->index_of(id);
    if (index < 0 || runtime_->nodes_[index].freed) return false;
    const NodeType type = runtime_->nodes_[index].d.type;
    if (physics::is_area(type))
        return name == "body_entered" || name == "body_exited" || name == "area_entered" || name == "area_exited";
    if (type == NodeType::Timer) return name == "timeout";
    if (type == NodeType::AnimatedSprite2D) return name == "animation_finished";
    if (type == NodeType::CPUParticles3D) return name == "finished";
    return false;
}

bool ScriptHost::has_method(ObjectId id, std::string_view name) const {
    const int m = method_id(name);
    if (m < 0) return false;
    if (id == InputId) return m >= IsActionPressed && m <= GetAxis;
    if (id == TreeId) return m == ChangeSceneToFile || m == ReloadCurrentScene;
    if (!scene_path(id).empty()) return m == Instantiate;
    if (!runtime_) return false;
    const int index = runtime_->index_of(id);
    return index >= 0 && !runtime_->nodes_[index].freed && node_has_method(runtime_->nodes_[index].d.type, m);
}

std::string ScriptHost::describe(ObjectId id) const {
    if (id == InputId) return "<Input>";
    if (id == TreeId) return "<SceneTree>";
    if (!scene_path(id).empty()) return "<PackedScene:" + std::string(scene_path(id)) + ">";
    if (runtime_) {
        const int index = runtime_->index_of(id);
        if (index >= 0 && !runtime_->nodes_[index].freed) {
            const auto& d = runtime_->nodes_[index].d;
            return d.name + ":<" + node_type_name(d.type) + "#" + std::to_string(d.id) + ">";
        }
    }
    return "<Freed Object>";
}

void ScriptHost::print(std::string_view text, bool is_error) {
    if (runtime_) runtime_->log(std::string(text), is_error);
}

// Built from the authored values, which scripts may have just changed.
static Transform global_of(const Runtime::Nodes& nodes, int index) {
    Transform result;
    std::vector<int> chain;
    for (int i = index; i >= 0 && is_node3d(nodes[i].d.type); i = nodes[i].parent) chain.push_back(i);
    for (auto it = chain.rbegin(); it != chain.rend(); ++it) {
        const auto& d = nodes[*it].d;
        result = result * local_transform(d.position, d.rotation, d.scale);
    }
    return result;
}
static Transform parent_global_of(const Runtime::Nodes& nodes, int index) {
    const int parent = nodes[index].parent;
    return parent >= 0 && is_node3d(nodes[parent].d.type) ? global_of(nodes, parent) : Transform{};
}

// 2D: the canvas transform through the 2D parents, from the authored values.
static Transform local_2d(const NodeDesc& d) {
    return local_transform({d.position.x, d.position.y, 0}, {0, 0, d.rotation.z}, {d.scale.x, d.scale.y, 1});
}
static Transform global_2d(const Runtime::Nodes& nodes, int index) {
    Transform result;
    std::vector<int> chain;
    for (int i = index; i >= 0 && is_node2d(nodes[i].d.type); i = nodes[i].parent) chain.push_back(i);
    for (auto it = chain.rbegin(); it != chain.rend(); ++it) result = result * local_2d(nodes[*it].d);
    return result;
}
static Transform parent_global_2d(const Runtime::Nodes& nodes, int index) {
    const int parent = nodes[index].parent;
    return parent >= 0 && is_node2d(nodes[parent].d.type) ? global_2d(nodes, parent) : Transform{};
}
static jetscript::Vector2 to2(Vec3 v) { return {v.x, v.y}; }

Status ScriptHost::get_property(ObjectId id, int p, Value& out, std::string& error) {
    if (!runtime_ || id == InputId || !scene_path(id).empty()) return Status::NotFound;
    if (id == TreeId) {
        if (p != CurrentScene) return Status::NotFound;
        out = runtime_->nodes_.empty() ? Value() : Value::object(runtime_->nodes_[0].d.id);
        return Status::Ok;
    }
    const int index = runtime_->index_of(id);
    if (index < 0 || runtime_->nodes_[index].freed) { error = "Attempt to access a previously freed instance."; return Status::Error; }
    const auto& live = runtime_->nodes_[index];
    const auto& d = live.d;
    if (p < 0 || p >= PropertyCount || !has_property(d.type, p)) return Status::NotFound;
    if (is_node2d(d.type)) {
        switch (p) {
        case Position: out = Value(to2(d.position)); return Status::Ok;
        case Rotation: out = Value(radians(d.rotation.z)); return Status::Ok;
        case RotationDegrees: out = Value(d.rotation.z); return Status::Ok;
        case Scale: out = Value(to2(d.scale)); return Status::Ok;
        case GlobalPosition: out = Value(to2(global_2d(runtime_->nodes_, index).origin)); return Status::Ok;
        case ZIndex: out = Value(jetscript::Int(d.z_index)); return Status::Ok;
        case Centered: out = Value(d.centered); return Status::Ok;
        case Offset: out = Value(jetscript::Vector2{d.offset[0], d.offset[1]}); return Status::Ok;
        case Animation: out = Value(d.more().animation); return Status::Ok;
        case SpeedScale: out = Value(d.more().speed_scale); return Status::Ok;
        case Zoom: out = Value(jetscript::Vector2{d.more().zoom[0], d.more().zoom[1]}); return Status::Ok;
        case Enabled: out = Value(d.more().current); return Status::Ok;
        case Velocity: case LinearVelocity: out = Value(to2(live.motion().velocity)); return Status::Ok;
        case AngularVelocity: out = Value(live.motion().angular.z); return Status::Ok;
        case OneWayCollision: out = Value(d.more().one_way); return Status::Ok;
        default: break;
        }
    }
    switch (p) {
    case Name: out = Value(d.name); break;
    case Velocity: case LinearVelocity: out = Value(from_vec(live.motion().velocity)); break;
    case AngularVelocity: out = Value(from_vec(live.motion().angular)); break;
    case Mass: out = Value(d.more().mass); break;
    case GravityScale: out = Value(d.more().gravity_scale); break;
    case Freeze: out = Value(d.more().freeze); break;
    case Sleeping: out = Value(live.motion().sleeping); break;
    case FloorMaxAngle: out = Value(radians(d.more().floor_max_angle)); break; // Godot: radians
    case Monitoring: out = Value(d.more().monitoring); break;
    case Disabled: out = Value(d.more().disabled); break;
    case CollisionLayer: out = Value(jetscript::Int(d.more().collision_layer)); break;
    case CollisionMask: out = Value(jetscript::Int(d.more().collision_mask)); break;
    case Friction: out = Value(d.more().friction); break;
    case Bounce: out = Value(d.more().bounce); break;
    case Emitting: out = Value(d.more().emitting); break;
    case Amount: out = Value(jetscript::Int(d.more().amount)); break;
    case Lifetime: out = Value(d.more().lifetime); break;
    case OneShot: out = Value(d.more().one_shot); break;
    case Explosiveness: out = Value(d.more().explosiveness); break;
    case LifetimeRandomness: out = Value(d.more().lifetime_randomness); break;
    case Direction: out = Value(from_vec(d.more().direction)); break;
    case Spread: out = Value(d.more().spread); break;
    case Gravity: out = Value(from_vec(d.more().gravity)); break;
    case InitialVelocityMin: out = Value(d.more().velocity_min); break;
    case InitialVelocityMax: out = Value(d.more().velocity_max); break;
    case DampingMin: out = Value(d.more().damping_min); break;
    case DampingMax: out = Value(d.more().damping_max); break;
    case Position: out = is_control(d.type) ? Value(to2(d.position)) : Value(from_vec(d.position)); break;
    case Rotation: out = Value(jetscript::Vector3{radians(d.rotation.x), radians(d.rotation.y), radians(d.rotation.z)}); break;
    case RotationDegrees: out = Value(from_vec(d.rotation)); break;
    case Scale: out = Value(from_vec(d.scale)); break;
    case Visible: out = Value(d.visible); break;
    case GlobalPosition: out = Value(from_vec(global_of(runtime_->nodes_, index).origin)); break;
    case MeshColor: out = Value(to_color(d.color)); break;
    case Fov: out = Value(d.more().fov); break;
    case Near: out = Value(d.more().near); break;
    case Far: out = Value(d.more().far); break;
    case Current: out = Value(d.more().current); break;
    case LightColor: out = Value(to_color(d.more().light_color)); break;
    case LightEnergy: out = Value(d.more().energy); break;
    case BackgroundColor: out = Value(to_color(d.more().background)); break;
    case AmbientColor: out = Value(to_color(d.more().ambient)); break;
    case MeshAlpha: out = Value(d.alpha); break;
    case UvOffset: out = Value(jetscript::Vector3{d.uv_offset[0], d.uv_offset[1], 0}); break;
    case UvScale: out = Value(jetscript::Vector3{d.uv_scale[0], d.uv_scale[1], 1}); break;
    case PerspectiveCorrect: out = Value(d.perspective); break;
    case TextureFilter: out = Value(jetscript::Int(d.bilinear ? 1 : 0)); break; // Godot: 0 nearest, 1 linear
    case Text: out = Value(d.more().text); break;
    case Size: out = Value(to2(d.more().size)); break;
    case Frame: out = Value(jetscript::Int(d.frame)); break;
    case Hframes: out = Value(jetscript::Int(d.hframes)); break;
    case Vframes: out = Value(jetscript::Int(d.vframes)); break;
    case FlipH: out = Value(d.flip_h); break;
    case FlipV: out = Value(d.flip_v); break;
    case PixelSize: out = Value(d.pixel_size); break;
    case Modulate: out = Value(to_color(d.color)); break;
    case WaitTime: out = Value(d.more().lifetime); break;
    case Autostart: out = Value(d.more().autoplay); break;
    case Paused: out = Value(d.more().paused); break;
    case TimeLeft: out = Value(d.more().playing ? std::max(live.cycle, 0.0f) : 0.0f); break;
    }
    return Status::Ok;
}

Status ScriptHost::set_property(ObjectId id, int p, const Value& v, std::string& error) {
    if (!runtime_ || id == InputId || id == TreeId || !scene_path(id).empty()) return Status::NotFound;
    const int index = runtime_->index_of(id);
    if (index < 0 || runtime_->nodes_[index].freed) { error = "Attempt to access a previously freed instance."; return Status::Error; }
    auto& live = runtime_->nodes_[index];
    auto& d = live.d;
    if (p < 0 || p >= PropertyCount || !has_property(d.type, p)) return Status::NotFound;
    auto expect = [&](Type type) {
        const bool ok = v.type() == type || (type == Type::Float && v.type() == Type::Int) || (type == Type::Bool && v.type() == Type::Int);
        if (!ok) error = std::string("Invalid assignment of property '") + property_names[p] + "' with value of type '"
            + jetscript::type_name(v.type()) + "' (expected '" + jetscript::type_name(type) + "').";
        return ok;
    };
    auto vector = [&](Vec3& target, float bound, float factor = 1) {
        if (!expect(Type::Vector3)) return false;
        const Vec3 value = to_vec(v.as_vector3()) * factor;
        if (!value.finite() || std::abs(value.x) > bound || std::abs(value.y) > bound || std::abs(value.z) > bound) {
            error = std::string("Value of '") + property_names[p] + "' is not finite or out of range.";
            return false;
        }
        target = value;
        return true;
    };
    // Controls: Vector2 like Godot, or a Vector3 whose z is ignored (scripts written before Vector2).
    auto plane = [&](Vec3& target, float bound) {
        Vec3 value;
        if (v.type() == Type::Vector3) value = to_vec(v.as_vector3());
        else if (expect(Type::Vector2)) value = {v.as_vector2().x, v.as_vector2().y, 0};
        else return false;
        if (!value.finite() || std::abs(value.x) > bound || std::abs(value.y) > bound) {
            error = std::string("Value of '") + property_names[p] + "' is not finite or out of range.";
            return false;
        }
        target = {value.x, value.y, 0};
        error.clear();
        return true;
    };
    auto number = [&](float& target, float low, float high) {
        if (!expect(Type::Float)) return false;
        const float value = v.as_real();
        if (!std::isfinite(value)) { error = std::string("Value of '") + property_names[p] + "' is not finite."; return false; }
        target = std::clamp(value, low, high);
        return true;
    };
    auto layers = [&](uint32_t& target) {
        if (v.type() != Type::Int || v.as_int() < 0 || v.as_int() > 0xffffffffLL) {
            error = std::string("'") + property_names[p] + "' must be an int bitmask (layers 1-32).";
            return false;
        }
        target = uint32_t(v.as_int());
        return true;
    };
    if (is_node2d(d.type)) {
        jetscript::Vector2 w;
        auto vector2 = [&](float bound) {
            if (v.type() == Type::Vector3) w = {v.as_vector3().x, v.as_vector3().y};
            else if (expect(Type::Vector2)) w = v.as_vector2();
            else return false;
            if (!std::isfinite(w.x) || !std::isfinite(w.y) || std::abs(w.x) > bound || std::abs(w.y) > bound) {
                error = std::string("Value of '") + property_names[p] + "' is not finite or out of range.";
                return false;
            }
            return true;
        };
        bool done = true, ok = true;
        switch (p) {
        case Position: ok = vector2(100000); if (ok) { d.position.x = w.x; d.position.y = w.y; } break;
        case Rotation: {
            float radians_value = 0;
            ok = number(radians_value, -1e6f, 1e6f);
            if (ok) d.rotation.z = radians_value * 180.0f / Pi;
            break;
        }
        case RotationDegrees: ok = number(d.rotation.z, -1e6f, 1e6f); break;
        case Scale:
            ok = vector2(1000);
            if (ok && (std::abs(w.x) < 0.001f || std::abs(w.y) < 0.001f)) { error = "Scale components must not be zero."; ok = false; }
            if (ok) { d.scale.x = w.x; d.scale.y = w.y; }
            break;
        case GlobalPosition:
            ok = vector2(100000);
            if (ok) {
                const Vec3 local = parent_global_2d(runtime_->nodes_, index).inverse().xform({w.x, w.y, 0});
                d.position.x = local.x;
                d.position.y = local.y;
            }
            break;
        case ZIndex:
            ok = v.type() == Type::Int && v.as_int() >= -4096 && v.as_int() <= 4096;
            if (!ok) error = "z_index must be an int between -4096 and 4096.";
            else d.z_index = int16_t(v.as_int());
            break;
        case Centered: ok = expect(Type::Bool); if (ok) d.centered = v.truthy(); break;
        case Offset: ok = vector2(100000); if (ok) { d.offset[0] = w.x; d.offset[1] = w.y; } break;
        case Animation: {
            ok = expect(Type::String);
            if (!ok) break;
            // Switching keeps it playing (from the new animation's start) or stopped, as in Godot.
            auto& e = d.extras();
            const bool was_playing = e.playing;
            e.playing = false;
            ok = runtime_->play_animation(live, v.as_string());
            if (!ok) error = "There is no animation with name '" + std::string(v.as_string()) + "'.";
            e.playing = ok && was_playing;
            break;
        }
        case SpeedScale: ok = number(d.extras().speed_scale, -100, 100); break;
        case Zoom:
            ok = vector2(100);
            if (ok && (w.x < 0.01f || w.y < 0.01f)) { error = "Camera2D zoom must be positive."; ok = false; }
            if (ok) { d.extras().zoom[0] = w.x; d.extras().zoom[1] = w.y; }
            break;
        case Enabled: ok = expect(Type::Bool); if (ok) d.extras().current = v.truthy(); break;
        case Velocity: case LinearVelocity:
            ok = vector2(100000);
            if (ok) live.edit_motion().velocity = {w.x, w.y, 0};
            if (ok && p == LinearVelocity) runtime_->wake(live);
            break;
        case AngularVelocity: {
            float spin = 0;
            ok = number(spin, -1000, 1000);
            if (ok) { live.edit_motion().angular = {0, 0, spin}; runtime_->wake(live); }
            break;
        }
        case OneWayCollision: ok = expect(Type::Bool); if (ok) d.extras().one_way = v.truthy(); runtime_->physics_->built = false; break;
        default: done = false;
        }
        if (done) return ok ? Status::Ok : Status::Error;
    }
    bool ok = true;
    switch (p) {
    case Name: error = "Property 'name' is read-only in JetScript."; return Status::Error;
    case Velocity: ok = vector(live.edit_motion().velocity, 1000); break;
    case LinearVelocity: ok = vector(live.edit_motion().velocity, 1000); if (ok) runtime_->wake(live); break;
    case AngularVelocity: ok = vector(live.edit_motion().angular, 1000); if (ok) runtime_->wake(live); break;
    case Mass: ok = number(d.extras().mass, 0.001f, 100000); break;
    case GravityScale: ok = number(d.extras().gravity_scale, -100, 100); break;
    case Freeze: ok = expect(Type::Bool); if (ok) { d.extras().freeze = v.truthy(); runtime_->wake(live); } break;
    case Sleeping: ok = expect(Type::Bool); if (ok) { live.edit_motion().sleeping = v.truthy(); live.edit_motion().still_time = 0; } break;
    case FloorMaxAngle: {
        float angle = 0;
        ok = number(angle, 0, radians(89));
        if (ok) d.extras().floor_max_angle = degrees(angle);
        break;
    }
    case Monitoring: ok = expect(Type::Bool); if (ok) d.extras().monitoring = v.truthy(); break;
    case Disabled: ok = expect(Type::Bool); if (ok) d.extras().disabled = v.truthy(); runtime_->physics_->built = false; break;
    case CollisionLayer: ok = layers(d.extras().collision_layer); runtime_->physics_->built = false; break;
    case CollisionMask: ok = layers(d.extras().collision_mask); runtime_->physics_->built = false; break;
    case Friction: ok = number(d.extras().friction, 0, 100); break;
    case Bounce: ok = number(d.extras().bounce, 0, 1); break;
    case Emitting:
        ok = expect(Type::Bool);
        if (ok && v.truthy() && !d.more().emitting) { live.cycle = 0; live.emitted = 0; } // a new cycle, as in Godot
        if (ok) d.extras().emitting = v.truthy();
        break;
    case Amount:
        if (v.type() != Type::Int || v.as_int() < 1 || v.as_int() > MaxParticles) {
            error = "'amount' must be an int between 1 and 200."; ok = false;
        } else {
            d.extras().amount = uint16_t(v.as_int());
        }
        break;
    case Lifetime: ok = number(d.extras().lifetime, 0.01f, 600); break;
    case OneShot: ok = expect(Type::Bool); if (ok) d.extras().one_shot = v.truthy(); break;
    case WaitTime: ok = number(d.extras().lifetime, 0.001f, 100000); break;
    case Autostart: ok = expect(Type::Bool); if (ok) d.extras().autoplay = v.truthy(); break;
    case Paused: ok = expect(Type::Bool); if (ok) d.extras().paused = v.truthy(); break;
    case TimeLeft: error = "Property 'time_left' is read-only."; return Status::Error;
    case Explosiveness: ok = number(d.extras().explosiveness, 0, 1); break;
    case LifetimeRandomness: ok = number(d.extras().lifetime_randomness, 0, 1); break;
    case Direction: ok = vector(d.extras().direction, 1000); break;
    case Spread: ok = number(d.extras().spread, 0, 180); break;
    case Gravity: ok = vector(d.extras().gravity, 1000); break;
    case InitialVelocityMin: ok = number(d.extras().velocity_min, 0, 1000); break;
    case InitialVelocityMax: ok = number(d.extras().velocity_max, 0, 1000); break;
    case DampingMin: ok = number(d.extras().damping_min, 0, 1000); break;
    case DampingMax: ok = number(d.extras().damping_max, 0, 1000); break;
    case Position: ok = is_control(d.type) ? plane(d.position, 10000) : vector(d.position, 10000); break;
    case Rotation: ok = vector(d.rotation, 1e6f, 180.0f / Pi); break;
    case RotationDegrees: ok = vector(d.rotation, 1e6f); break;
    case Scale: {
        Vec3 s;
        ok = vector(s, 1000);
        if (ok && (std::abs(s.x) < 0.001f || std::abs(s.y) < 0.001f || std::abs(s.z) < 0.001f)) {
            error = "Scale components must not be zero."; ok = false;
        }
        if (ok) d.scale = s;
        break;
    }
    case Visible: ok = expect(Type::Bool); if (ok) d.visible = v.truthy(); break;
    case GlobalPosition: {
        Vec3 global;
        ok = vector(global, 10000);
        if (ok) d.position = parent_global_of(runtime_->nodes_, index).inverse().xform(global);
        break;
    }
    case MeshColor: ok = expect(Type::Color); if (ok) d.color = from_color(v.as_color()); break;
    case Fov: ok = number(d.extras().fov, 1, 179); break;
    case Near: ok = number(d.extras().near, 0.001f, d.extras().far * 0.999f); break;
    case Far: ok = number(d.extras().far, d.extras().near * 1.001f, 10000); break;
    case Current:
        ok = expect(Type::Bool);
        if (ok && v.truthy()) for (auto& n : runtime_->nodes_) if (n.d.type == NodeType::Camera3D) n.d.extras().current = false;
        if (ok) d.extras().current = v.truthy();
        break;
    case LightColor: ok = expect(Type::Color); if (ok) d.extras().light_color = from_color(v.as_color()); break;
    case LightEnergy: ok = number(d.extras().energy, 0, 16); break;
    case BackgroundColor: ok = expect(Type::Color); if (ok) d.extras().background = from_color(v.as_color()); break;
    case AmbientColor: ok = expect(Type::Color); if (ok) d.extras().ambient = from_color(v.as_color()); break;
    case MeshAlpha: ok = number(d.alpha, 0, 1); break;
    case UvOffset: case UvScale: {
        // Only x and y are used. Godot's uv1_offset/uv1_scale are Vector3 too.
        Vec3 uv;
        ok = vector(uv, 1000);
        float* target = p == UvOffset ? d.uv_offset : d.uv_scale;
        if (ok) { target[0] = uv.x; target[1] = uv.y; }
        break;
    }
    case PerspectiveCorrect: ok = expect(Type::Bool); if (ok) d.perspective = v.truthy(); break;
    case Text:
        ok = expect(Type::String);
        if (ok && v.as_string().size() > MaxText) { error = "Label text is limited to 512 bytes."; ok = false; }
        if (ok) d.extras().text = std::string(v.as_string());
        break;
    case Size: {
        Vec3 size;
        ok = plane(size, 4096);
        if (ok && (size.x < 0 || size.y < 0)) { error = "Size must not be negative."; ok = false; }
        if (ok) d.extras().size = size;
        break;
    }
    case TextureFilter:
        ok = v.type() == Type::Int && (v.as_int() == 0 || v.as_int() == 1);
        if (!ok) error = "texture_filter must be 0 (nearest) or 1 (linear).";
        else d.bilinear = v.as_int() == 1;
        break;
    case Frame: {
        const jetscript::Int frames = jetscript::Int(d.hframes) * d.vframes;
        ok = v.type() == Type::Int && v.as_int() >= 0 && v.as_int() < frames;
        if (!ok) error = "Index p_frame = " + v.to_string() + " is out of bounds (vframes * hframes = " + std::to_string(frames) + ").";
        else d.frame = uint16_t(v.as_int());
        break;
    }
    case Hframes: case Vframes: {
        ok = v.type() == Type::Int && v.as_int() >= 1 && v.as_int() <= 256;
        if (!ok) { error = std::string(property_names[p]) + " must be an int between 1 and 256."; break; }
        (p == Hframes ? d.hframes : d.vframes) = uint16_t(v.as_int());
        if (d.frame >= d.hframes * d.vframes) d.frame = 0; // as Godot does
        break;
    }
    case FlipH: ok = expect(Type::Bool); if (ok) d.flip_h = v.truthy(); break;
    case FlipV: ok = expect(Type::Bool); if (ok) d.flip_v = v.truthy(); break;
    case PixelSize: ok = number(d.pixel_size, 0.0001f, 100); break;
    case Modulate: ok = expect(Type::Color); if (ok) d.color = from_color(v.as_color()); break;
    }
    return ok ? Status::Ok : Status::Error;
}

Status ScriptHost::call_method(ObjectId id, int m, const Value* args, int count, Value& result, std::string& error) {
    result = Value();
    if (!runtime_) { error = "No running scene."; return Status::Error; }
    auto arg_string = [&](int i, std::string_view& out) {
        if (i >= count || args[i].type() != Type::String) {
            error = std::string("Invalid argument for '") + method_names[m] + "': expected String.";
            return false;
        }
        out = args[i].as_string();
        return true;
    };
    if (id == TreeId) {
        if (m == ChangeSceneToFile) { // returns OK (0) or ERR_CANT_OPEN (19), as in Godot
            std::string_view path;
            if (!arg_string(0, path)) return Status::Error;
            const bool ok = runtime_->change_scene(path);
            if (!ok) runtime_->log("change_scene_to_file(): the package has no scene \"" + std::string(path) + "\".", true);
            result = Value(jetscript::Int(ok ? 0 : 19));
            return Status::Ok;
        }
        if (m == ReloadCurrentScene) {
            result = Value(jetscript::Int(runtime_->change_scene(runtime_->current_scene()) ? 0 : 19));
            return Status::Ok;
        }
        return Status::NotFound;
    }
    if (!scene_path(id).empty()) {
        if (m != Instantiate) return Status::NotFound;
        const uint32_t root = runtime_->instantiate(scene_path(id), error);
        if (!root) return Status::Error;
        result = Value::object(root);
        return Status::Ok;
    }
    if (id == InputId) {
        if (m < IsActionPressed || m > GetAxis) return Status::NotFound;
        auto bit = [&](int i, uint32_t& out) {
            std::string_view name;
            if (!arg_string(i, name)) return false;
            const int index = runtime_->action_index(name);
            out = index >= 0 ? 1u << index : 0;
            if (!out) error = "The InputMap action \"" + std::string(name) + "\" doesn't exist.";
            return out != 0;
        };
        uint32_t a = 0, b = 0;
        if (!bit(0, a) || (m == GetAxis && !bit(1, b))) return Status::Error;
        const uint32_t now = runtime_->input_;
        const uint32_t before = runtime_->in_physics_ ? runtime_->physics_previous_input_ : runtime_->previous_input_;
        switch (m) {
        case IsActionPressed: result = Value((now & a) != 0); break;
        case IsActionJustPressed: result = Value((now & a) && !(before & a)); break;
        case IsActionJustReleased: result = Value(!(now & a) && (before & a)); break;
        case GetAxis: result = Value(float((now & b) ? 1 : 0) - float((now & a) ? 1 : 0)); break;
        }
        return Status::Ok;
    }
    auto& nodes = runtime_->nodes_;
    const int index = runtime_->index_of(id);
    if (index < 0 || nodes[index].freed) { error = "Attempt to call a method on a previously freed instance."; return Status::Error; }
    auto& node = nodes[index];
    if (m < 0 || m >= MethodCount || !node_has_method(node.d.type, m)) return Status::NotFound;
    auto object = [&](int i) { return Value::object(nodes[i].d.id); };
    if (is_node2d(node.d.type) && (m == Translate || m == LookAt)) {
        if (count < 1 || args[0].type() != Type::Vector2) {
            error = std::string("Invalid argument for '") + method_names[m] + "': expected Vector2.";
            return Status::Error;
        }
        const auto target = args[0].as_vector2();
        if (m == Translate) {
            node.d.position.x += target.x;
            node.d.position.y += target.y;
            return Status::Ok;
        }
        // look_at(): +x towards the point, whatever the parents' rotation.
        const Transform parent = parent_global_2d(nodes, index);
        const Vec3 here = global_2d(nodes, index).origin;
        const float angle = std::atan2(target.y - here.y, target.x - here.x);
        const float parent_angle = std::atan2(parent.basis.m[1][0], parent.basis.m[0][0]);
        node.d.rotation.z = (angle - parent_angle) * 180.0f / Pi;
        return Status::Ok;
    }
    auto find_child = [&](int parent, std::string_view name) {
        for (int child : nodes[parent].children) if (!nodes[child].freed && nodes[child].d.name == name) return child;
        return -1;
    };
    auto resolve = [&](std::string_view path) {
        int current = index;
        size_t start = 0;
        if (!path.empty() && path[0] == '/') {
            // Absolute: "/root/World/Child" or "/World/Child".
            path.remove_prefix(1);
            if (path.substr(0, 4) == "root" && (path.size() == 4 || path[4] == '/')) path.remove_prefix(std::min<size_t>(path.size(), 5));
            const size_t end = path.find('/');
            current = -1;
            if (path.substr(0, end) == nodes[0].d.name) current = 0;
            else
                for (size_t i = 0; i < nodes.size(); ++i)
                    if (nodes[i].d.id >= AutoloadIdBase && nodes[i].d.name == path.substr(0, end)) current = int(i); // "/root/Game"
            if (current < 0) return -1;
            start = end == std::string_view::npos ? path.size() : end + 1;
        }
        while (start < path.size()) {
            size_t end = path.find('/', start);
            if (end == std::string_view::npos) end = path.size();
            const auto part = path.substr(start, end - start);
            if (part == "..") current = nodes[current].parent;
            else if (!part.empty() && part != ".") current = find_child(current, part);
            if (current < 0 || nodes[current].freed) return -1;
            start = end + 1;
        }
        return current;
    };
    auto number = [&](int i, float& out) {
        if (i >= count || !args[i].is_number() || !std::isfinite(args[i].as_real())) {
            error = std::string("Invalid argument for '") + method_names[m] + "': expected a finite number.";
            return false;
        }
        out = args[i].as_real();
        return true;
    };
    auto rotate = [&](Vec3 axis) {
        float angle;
        if (!number(0, angle)) return false;
        const Vec3 r = node.d.rotation;
        const Basis current = Basis::from_euler({radians(r.x), radians(r.y), radians(r.z)});
        const Vec3 e = (Basis::axis_angle(axis, angle) * current).euler();
        node.d.rotation = {degrees(e.x), degrees(e.y), degrees(e.z)};
        return true;
    };
    switch (m) {
    case GetNode: case GetNodeOrNull: {
        std::string_view path;
        if (!arg_string(0, path)) return Status::Error;
        const int found = resolve(path);
        if (found >= 0) result = object(found);
        else if (m == GetNode) {
            error = "Node not found: \"" + std::string(path) + "\" (relative to \"" + node.d.name + "\").";
            return Status::Error;
        }
        return Status::Ok;
    }
    case GetParent: if (node.parent >= 0) result = object(node.parent); return Status::Ok;
    case GetChildren:
        result = Value::new_array();
        for (int child : node.children) if (!nodes[child].freed) result.array_push(object(child));
        return Status::Ok;
    case GetChildCount: {
        jetscript::Int total = 0;
        for (int child : node.children) total += nodes[child].freed ? 0 : 1;
        result = Value(total);
        return Status::Ok;
    }
    case GetChild: {
        if (count < 1 || args[0].type() != Type::Int) { error = "Invalid argument for 'get_child': expected int."; return Status::Error; }
        std::vector<int> live;
        for (int child : node.children) if (!nodes[child].freed) live.push_back(child);
        jetscript::Int i = args[0].as_int();
        if (i < 0) i += jetscript::Int(live.size());
        if (i < 0 || i >= jetscript::Int(live.size())) { error = "Index p_index = " + std::to_string(args[0].as_int()) + " is out of bounds."; return Status::Error; }
        result = object(live[size_t(i)]);
        return Status::Ok;
    }
    case FindChild: {
        std::string_view name;
        if (!arg_string(0, name)) return Status::Error;
        std::vector<int> stack(node.children.rbegin(), node.children.rend());
        while (!stack.empty()) {
            const int i = stack.back();
            stack.pop_back();
            if (nodes[i].freed) continue;
            if (nodes[i].d.name == name) { result = object(i); break; }
            stack.insert(stack.end(), nodes[i].children.rbegin(), nodes[i].children.rend());
        }
        return Status::Ok;
    }
    case QueueFree: node.free_requested = true; runtime_->pending_free_ = true; return Status::Ok;
    case Play: {
        std::string_view name;
        if (count >= 1 && !arg_string(0, name)) return Status::Error;
        if (!runtime_->play_animation(node, name)) {
            error = name.empty() ? "AnimatedSprite2D has no animations: set them in the Inspector." :
                                   "There is no animation with name '" + std::string(name) + "'.";
            return Status::Error;
        }
        return Status::Ok;
    }
    case Stop:
        node.d.extras().playing = false;
        if (node.d.type == NodeType::Timer) {
            node.cycle = 0;
            node.d.extras().autoplay = false; // as Godot's stop()
        }
        return Status::Ok;
    case Start: {
        float wait_time = -1;
        if (count >= 1) {
            if (!args[0].is_number() || !std::isfinite(args[0].as_real()) || args[0].as_real() > 100000) {
                error = "Invalid argument for 'start': expected the time in seconds."; return Status::Error;
            }
            wait_time = args[0].as_real();
        }
        runtime_->start_timer(node, wait_time);
        return Status::Ok;
    }
    case IsStopped: result = Value(!node.d.more().playing); return Status::Ok;
    case IsPlaying: result = Value(node.d.more().playing); return Status::Ok;
    case Rotate:
        if (count < 1 || !args[0].is_number()) { error = "Invalid argument for 'rotate': expected float (radians)."; return Status::Error; }
        node.d.rotation.z += args[0].as_real() * 180.0f / Pi;
        return Status::Ok;
    case AddChild:
    case RemoveChild: {
        const int child = count >= 1 && args[0].type() == Type::Object ? runtime_->index_of(args[0].as_object()) : -1;
        if (child < 0 || nodes[child].freed) {
            error = std::string("Invalid argument for '") + method_names[m] + "': expected a node.";
            return Status::Error;
        }
        // add_child() runs _ready(), which may add nodes and move nodes_: nothing of node is used after it.
        const bool ok = m == AddChild ? runtime_->add_child(index, child, error) : runtime_->remove_child(index, child, error);
        return ok ? Status::Ok : Status::Error;
    }
    case Translate: {
        if (count < 1 || args[0].type() != Type::Vector3) { error = "Invalid argument for 'translate': expected Vector3."; return Status::Error; }
        const Basis local = local_transform({}, node.d.rotation, node.d.scale).basis;
        const Vec3 moved = node.d.position + local.xform(to_vec(args[0].as_vector3()));
        if (!moved.finite()) { error = "Resulting position is not finite."; return Status::Error; }
        node.d.position = moved;
        return Status::Ok;
    }
    case RotateX: return rotate({1, 0, 0}) ? Status::Ok : Status::Error;
    case RotateY: return rotate({0, 1, 0}) ? Status::Ok : Status::Error;
    case RotateZ: return rotate({0, 0, 1}) ? Status::Ok : Status::Error;
    case LookAt: {
        if (count < 1 || args[0].type() != Type::Vector3 || (count > 1 && args[1].type() != Type::Vector3)) {
            error = "Invalid arguments for 'look_at': expected Vector3 target and optional Vector3 up."; return Status::Error;
        }
        const Vec3 up = count > 1 ? to_vec(args[1].as_vector3()) : Vec3{0, 1, 0};
        const Transform global = global_of(nodes, index);
        const Vec3 z = (global.origin - to_vec(args[0].as_vector3())).normalized(); // -Z faces the target
        const Vec3 x = up.cross(z).normalized();
        if (z.length() == 0 || x.length() == 0) {
            error = "The target position and the node position are the same, or the up vector is aligned with the direction.";
            return Status::Error;
        }
        Basis look;
        look.set_column(0, x); look.set_column(1, z.cross(x)); look.set_column(2, z);
        const Basis parent = parent_global_of(nodes, index).basis.orthonormalized();
        const Vec3 e = (parent.transposed() * look).euler();
        node.d.rotation = {degrees(e.x), degrees(e.y), degrees(e.z)};
        return Status::Ok;
    }
    case Show: node.d.visible = true; return Status::Ok;
    case Hide: node.d.visible = false; return Status::Ok;
    case IsVisibleInTree: {
        bool visible = true;
        for (int i = index; i >= 0 && is_node3d(nodes[i].d.type); i = nodes[i].parent) visible = visible && nodes[i].d.visible;
        result = Value(visible);
        return Status::Ok;
    }
    case MakeCurrent:
        for (auto& n : nodes) if (n.d.type == node.d.type) n.d.extras().current = false;
        node.d.extras().current = true;
        return Status::Ok;
    case MoveAndSlide: result = Value(runtime_->move_and_slide(index)); return Status::Ok;
    case IsOnFloor: result = Value(node.motion().on_floor); return Status::Ok;
    case IsOnWall: result = Value(node.motion().on_wall); return Status::Ok;
    case IsOnCeiling: result = Value(node.motion().on_ceiling); return Status::Ok;
    case GetFloorNormal: {
        const Vec3 normal = node.motion().on_floor ? node.motion().floor_normal : Vec3{};
        result = is_node2d(node.d.type) ? Value(to2(normal)) : Value(from_vec(normal));
        return Status::Ok;
    }
    case ApplyCentralImpulse: case ApplyImpulse: case ApplyCentralForce: {
        // 2D bodies take a Vector2.
        const bool flat = is_node2d(node.d.type);
        const Type wanted = flat ? Type::Vector2 : Type::Vector3;
        const Vec3 value = args[0].type() != wanted ? Vec3{}
            : flat ? Vec3{args[0].as_vector2().x, args[0].as_vector2().y, 0} : to_vec(args[0].as_vector3());
        if (args[0].type() != wanted || !value.finite()) {
            error = std::string("Invalid argument for '") + method_names[m] + "': expected " + jetscript::type_name(wanted) + ".";
            return Status::Error;
        }
        if (m == ApplyCentralForce) node.edit_motion().force = node.edit_motion().force + value;
        else node.edit_motion().velocity = node.edit_motion().velocity + value * (1.0f / std::max(node.d.more().mass, 0.001f)); // arcade: no torque
        runtime_->wake(node);
        return Status::Ok;
    }
    case GetOverlappingBodies:
        result = Value::new_array();
        for (uint32_t body : node.motion().overlaps) result.array_push(Value::object(body));
        return Status::Ok;
    case HasOverlappingBodies: result = Value(!node.motion().overlaps.empty()); return Status::Ok;
    case OverlapsBody: {
        if (args[0].type() != Type::Object) { error = "Invalid argument for 'overlaps_body': expected a body."; return Status::Error; }
        result = Value(std::find(node.motion().overlaps.begin(), node.motion().overlaps.end(), args[0].as_object()) != node.motion().overlaps.end());
        return Status::Ok;
    }
    case GetOverlappingAreas:
        result = Value::new_array();
        for (uint32_t area : node.motion().areas) result.array_push(Value::object(area));
        return Status::Ok;
    case HasOverlappingAreas: result = Value(!node.motion().areas.empty()); return Status::Ok;
    case OverlapsArea: {
        if (args[0].type() != Type::Object) { error = "Invalid argument for 'overlaps_area': expected an area."; return Status::Error; }
        result = Value(std::find(node.motion().areas.begin(), node.motion().areas.end(), args[0].as_object()) != node.motion().areas.end());
        return Status::Ok;
    }
    case Restart: runtime_->restart_particles(node); return Status::Ok;
    case GetTree: result = Value::object(TreeId); return Status::Ok;
    }
    return Status::NotFound;
}

} // namespace jetstudio
