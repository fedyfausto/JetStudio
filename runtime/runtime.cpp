#include "runtime.hpp"
#include "physics.hpp"
#include "script_host.hpp"
#include "Primitives.hpp"
#include "EnvironmentMapping.hpp"
#include "ParticleSystem.hpp"
#include "font_data.hpp"
#include <algorithm>
#include <cstdlib>
#include <cctype>
#include <cmath>
#include <unordered_set>
#include <set>

namespace jetstudio {
namespace {
constexpr float JetUnits = 100; // 1 m = 100 Jet units
// Godot (right-handed, -Z forward) to Jet (left-handed, +Z forward).
::Vector3 to_jet(Vec3 v) {
    return {int32_t(std::lround(v.x * JetUnits)), int32_t(std::lround(v.y * JetUnits)), int32_t(std::lround(-v.z * JetUnits))};
}
bool bounded(Vec3 v, float bound) {
    return v.finite() && std::abs(v.x) <= bound && std::abs(v.y) <= bound && std::abs(v.z) <= bound;
}
bool valid_scale(Vec3 s) {
    for (int i = 0; i < 3; ++i) if (!std::isfinite(s[i]) || std::abs(s[i]) < 0.001f || std::abs(s[i]) > 1000) return false;
    return true;
}
Renderer::Color color_of(uint32_t c) { return {uint8_t(c >> 16), uint8_t(c >> 8), uint8_t(c)}; }
bool valid_name(const std::string& name) {
    if (name.empty() || name.size() > 80) return false;
    for (char c : name) if (c == '/' || c == ':' || c == '"' || c == '%' || c == '@' || c == '.' || uint8_t(c) < 32) return false;
    return true;
}
int degrees_int(float radians_value) {
    int d = int(std::lround(degrees(radians_value))) % 360;
    return d < 0 ? d + 360 : d;
}
bool same(Vec3 a, Vec3 b) { return std::abs(a.x - b.x) < 1e-4f && std::abs(a.y - b.y) < 1e-4f && std::abs(a.z - b.z) < 1e-4f; }

// Profile points are {radius, y, normal radius, normal y}, revolved around Y.
Renderer::Object* lathe(const std::vector<std::array<float, 4>>& profile, int segments, Renderer::Material* material) {
    auto* mesh = new Renderer::Object();
    for (size_t p = 0; p < profile.size(); ++p)
        for (int s = 0; s <= segments; ++s) {
            const float angle = 2 * Pi * float(s) / float(segments), c = std::cos(angle), sn = std::sin(angle);
            const auto& q = profile[p];
            Renderer::Object::Vertex vertex;
            vertex.position = {int32_t(std::lround(q[0] * c)), int32_t(std::lround(q[1])), int32_t(std::lround(q[0] * sn))};
            vertex.normal = {int32_t(std::lround(q[2] * c * FIXED_POINT_SCALE)), int32_t(std::lround(q[3] * FIXED_POINT_SCALE)),
                             int32_t(std::lround(q[2] * sn * FIXED_POINT_SCALE))};
            vertex.uv = {int32_t(FIXED_POINT_SCALE * s / segments), int32_t(FIXED_POINT_SCALE * p / (profile.size() - 1))};
            mesh->addVertex(vertex);
        }
    const int row = segments + 1;
    for (size_t p = 0; p + 1 < profile.size(); ++p)
        for (int s = 0; s < segments; ++s) {
            const uint16_t a = uint16_t(p * row + s), b = uint16_t(a + 1), c = uint16_t(a + row), d = uint16_t(c + 1);
            if (profile[p][0] > 0) mesh->addTriangle(a, b, c, material);
            if (profile[p + 1][0] > 0) mesh->addTriangle(b, d, c, material);
        }
    return mesh;
}

// Z is mirrored, so the winding is reversed too to keep faces pointing outwards in Jet's left-handed space.
Renderer::Object* make_object(const std::vector<MeshVertex>& vertices, const std::vector<uint16_t>& indices, Renderer::Material* material) {
    auto* mesh = new Renderer::Object();
    mesh->vertices.reserve(vertices.size());
    for (const auto& v : vertices) {
        Renderer::Object::Vertex out;
        out.position = {int32_t(std::lround(v.position.x * JetUnits)), int32_t(std::lround(v.position.y * JetUnits)),
                        int32_t(std::lround(-v.position.z * JetUnits))};
        const Vec3 n = v.normal.normalized();
        out.normal = {int32_t(std::lround(n.x * FIXED_POINT_SCALE)), int32_t(std::lround(n.y * FIXED_POINT_SCALE)),
                      int32_t(std::lround(-n.z * FIXED_POINT_SCALE))};
        out.uv = {int32_t(std::lround(v.u * FIXED_POINT_SCALE)), int32_t(std::lround(v.v * FIXED_POINT_SCALE))};
        mesh->vertices.push_back(out);
    }
    for (size_t i = 0; i + 2 < indices.size(); i += 3) mesh->addTriangle(indices[i], indices[i + 2], indices[i + 1], material);
    return mesh;
}

// Godot's QuadMesh: 1x1 m facing +Z, one-sided unless cull is Disabled.
Renderer::Object* quad(Renderer::Material* material) {
    const std::vector<MeshVertex> v = {
        {{-0.5f, 0.5f, 0}, {0, 0, 1}, 0, 0}, {{0.5f, 0.5f, 0}, {0, 0, 1}, 1, 0},
        {{0.5f, -0.5f, 0}, {0, 0, 1}, 1, 1}, {{-0.5f, -0.5f, 0}, {0, 0, 1}, 0, 1}};
    return make_object(v, {0, 3, 2, 0, 2, 1}, material);
}

// Godot's TorusMesh: inner radius 0.5 m, outer 1 m.
Renderer::Object* torus(int segments, Renderer::Material* material) {
    const int rings = std::max(3, segments), sides = std::max(4, segments / 2);
    const float major = 0.75f, minor = 0.25f;
    std::vector<MeshVertex> v;
    std::vector<uint16_t> idx;
    for (int i = 0; i <= rings; ++i) {
        const float a = 2 * Pi * float(i) / float(rings);
        const Vec3 radial{std::cos(a), 0, -std::sin(a)};
        for (int j = 0; j <= sides; ++j) {
            const float t = 2 * Pi * float(j) / float(sides);
            const Vec3 normal = radial * std::cos(t) + Vec3{0, std::sin(t), 0};
            v.push_back({radial * major + normal * minor, normal, float(i) / float(rings), float(j) / float(sides)});
        }
    }
    const int row = sides + 1;
    for (int i = 0; i < rings; ++i)
        for (int j = 0; j < sides; ++j) {
            const uint16_t a = uint16_t(i * row + j), b = uint16_t(a + 1), c = uint16_t(a + row), d = uint16_t(c + 1);
            idx.insert(idx.end(), {a, c, b, b, c, d});
        }
    return make_object(v, idx, material);
}

// Godot's CylinderMesh: radius 0.5 m, height 2 m. Jet's createCylinder mixes windings and breaks back-face culling.
Renderer::Object* cylinder(int segments, Renderer::Material* material) {
    const std::vector<std::array<float, 4>> profile = {
        {0, 100, 0, 1}, {50, 100, 0, 1}, {50, 100, 1, 0}, {50, -100, 1, 0}, {50, -100, 0, -1}, {0, -100, 0, -1}};
    return lathe(profile, segments, material);
}

// Godot's CapsuleMesh: radius 0.5 m, total height 2 m (Jet's own capsule has broken normals).
Renderer::Object* capsule(int segments, Renderer::Material* material) {
    const float radius = 50, half = 50;
    const int rings = std::max(2, segments / 4);
    std::vector<std::array<float, 4>> profile;
    for (int i = 0; i <= rings; ++i) {
        const float a = Pi * 0.5f * float(i) / float(rings);
        profile.push_back({radius * std::sin(a), half + radius * std::cos(a), std::sin(a), std::cos(a)});
    }
    for (int i = 0; i <= rings; ++i) {
        const float a = Pi * 0.5f + Pi * 0.5f * float(i) / float(rings);
        profile.push_back({radius * std::sin(a), -half + radius * std::cos(a), std::sin(a), std::cos(a)});
    }
    return lathe(profile, segments, material);
}
} // namespace

const char* node_type_name(NodeType type) {
    switch (type) {
    case NodeType::Node: return "Node";
    case NodeType::Node3D: return "Node3D";
    case NodeType::MeshInstance3D: return "MeshInstance3D";
    case NodeType::Camera3D: return "Camera3D";
    case NodeType::DirectionalLight3D: return "DirectionalLight3D";
    case NodeType::WorldEnvironment: return "WorldEnvironment";
    case NodeType::Label: return "Label";
    case NodeType::TextureRect: return "TextureRect";
    case NodeType::ColorRect: return "ColorRect";
    case NodeType::StaticBody3D: return "StaticBody3D";
    case NodeType::CharacterBody3D: return "CharacterBody3D";
    case NodeType::RigidBody3D: return "RigidBody3D";
    case NodeType::Area3D: return "Area3D";
    case NodeType::CollisionShape3D: return "CollisionShape3D";
    case NodeType::CPUParticles3D: return "CPUParticles3D";
    case NodeType::Sprite3D: return "Sprite3D";
    case NodeType::Control: return "Control";
    case NodeType::Node2D: return "Node2D";
    case NodeType::Sprite2D: return "Sprite2D";
    case NodeType::AnimatedSprite2D: return "AnimatedSprite2D";
    case NodeType::Camera2D: return "Camera2D";
    case NodeType::StaticBody2D: return "StaticBody2D";
    case NodeType::CharacterBody2D: return "CharacterBody2D";
    case NodeType::RigidBody2D: return "RigidBody2D";
    case NodeType::Area2D: return "Area2D";
    case NodeType::CollisionShape2D: return "CollisionShape2D";
    case NodeType::Timer: return "Timer";
    case NodeType::AnimationPlayer: return "AnimationPlayer";
    }
    return "Node";
}
bool is_control(NodeType type) {
    return type == NodeType::Control || type == NodeType::Label || type == NodeType::TextureRect || type == NodeType::ColorRect;
}
// Weak: a firmware may define its own (see cold_alloc in runtime.hpp).
__attribute__((weak)) void* cold_alloc(size_t bytes) { return std::malloc(bytes); }
__attribute__((weak)) void cold_free(void* p) { std::free(p); }

bool uses_extras(NodeType type) {
    return type != NodeType::Node && type != NodeType::Node3D && type != NodeType::MeshInstance3D && type != NodeType::Sprite3D
        && type != NodeType::Node2D && type != NodeType::Sprite2D;
}
bool is_node2d(NodeType type) {
    return type == NodeType::Node2D || type == NodeType::Sprite2D || type == NodeType::AnimatedSprite2D || type == NodeType::Camera2D
        || physics::is_flat(type);
}
bool is_node3d(NodeType type) {
    return type != NodeType::Node && type != NodeType::WorldEnvironment && type != NodeType::Timer
        && type != NodeType::AnimationPlayer && !is_control(type) && !is_node2d(type);
}
uint16_t rgb565(uint32_t c) { return uint16_t(((c >> 19) & 31) << 11 | ((c >> 10) & 63) << 5 | ((c >> 3) & 31)); }

bool validate(const Description& d, std::string& error) {
    error.clear();
    auto fail = [&](std::string message) { error = std::move(message); return false; };
    if (d.width < MinScreenSide || d.height < MinScreenSide || d.width > MaxScreenWidth || d.height > MaxScreenHeight
        || d.width % 2 || d.height % 2)
        return fail("Unsupported resolution: " + std::to_string(d.width) + "x" + std::to_string(d.height) + " (even sides, from "
                    + std::to_string(MinScreenSide) + " to " + std::to_string(MaxScreenWidth) + "x" + std::to_string(MaxScreenHeight) + ").");
    if (d.actions.size() > MaxActions) return fail("At most 32 actions in the Input Map.");
    for (size_t i = 0; i < d.actions.size(); ++i) {
        const auto& action = d.actions[i];
        bool ok = !action.name.empty() && action.name.size() <= 64 && action.keys.size() <= MaxActionKeys;
        for (char c : action.name) ok = ok && (std::isalnum(uint8_t(c)) || c == '_');
        for (const auto& key : action.keys) ok = ok && !key.empty() && key.size() <= 32;
        for (size_t j = 0; j < i; ++j) ok = ok && d.actions[j].name != action.name;
        if (!ok) return fail("Invalid action in the Input Map: \"" + action.name + "\" (letters, digits and _; unique names).");
    }
    if (d.scripts.size() > MaxScripts) return fail("At most 64 scripts per scene.");
    size_t script_bytes = 0;
    for (const auto& script : d.scripts) {
        if (script.path.rfind("res://", 0) != 0 || script.path.size() > 512) return fail("Invalid script path: " + script.path);
        script_bytes += script.source.size() + script.bytecode.size();
    }
    if (script_bytes > MaxScriptBytes) return fail("Scripts exceed 256 KiB in total.");
    if (d.textures.size() > MaxTextures) return fail("At most 64 textures per scene.");
    size_t texture_bytes = 0;
    for (const auto& t : d.textures) {
        const size_t count = size_t(t.width) * t.height;
        const bool indexed = !t.palette.empty();
        if (!t.width || !t.height || t.width > MaxTextureSide || t.height > MaxTextureSide
            || (indexed ? ((!t.packed_indices && t.indices.size() != count) || t.palette.size() > 256 || !t.pixels.empty())
                        : (t.pixels.size() != count || !t.indices.empty() || t.packed_indices)))
            return fail("Invalid texture: " + t.path);
        if (indexed)
            for (size_t i = 0; i < count; ++i) if (t.index_data()[i] >= t.palette.size()) return fail("Invalid texture: " + t.path);
        texture_bytes += t.bytes();
    }
    if (texture_bytes > MaxTextureBytes) return fail("Textures exceed 4 MiB in total.");
    auto texture_ok = [&](int32_t i) { return i >= -1 && (i < 0 || size_t(i) < d.textures.size()); };
    if (d.meshes.size() > MaxMeshAssets) return fail("At most 128 models per scene.");
    size_t total_vertices = 0;
    for (const auto& m : d.meshes) {
        total_vertices += m.vertices.size();
        if (m.vertices.empty() || m.vertices.size() > MaxMeshVertices || m.indices.empty() || m.indices.size() % 3)
            return fail("Invalid model: " + m.path);
        for (uint16_t i : m.indices) if (i >= m.vertices.size()) return fail("Invalid model: " + m.path);
        for (const auto& v : m.vertices)
            if (!bounded(v.position, 1000) || !v.normal.finite() || !std::isfinite(v.u) || !std::isfinite(v.v) || std::abs(v.u) > 1000
                || std::abs(v.v) > 1000) return fail("Model with invalid coordinates: " + m.path);
    }
    if (total_vertices > MaxSceneVertices) return fail("Models exceed 262144 vertices in total.");
    auto check_connections = [&](const std::vector<NodeDesc>& nodes, const std::vector<SceneConnection>& connections) -> bool {
        if (connections.size() > MaxConnections) return fail("At most 512 signal connections per scene.");
        auto identifier = [](const std::string& s) {
            if (s.empty() || s.size() > 80 || std::isdigit(uint8_t(s[0]))) return false;
            for (char c : s) if (!std::isalnum(uint8_t(c)) && c != '_') return false;
            return true;
        };
        for (const auto& c : connections)
            if (c.from < 0 || c.to < 0 || size_t(c.from) >= nodes.size() || size_t(c.to) >= nodes.size() || !identifier(c.signal)
                || !identifier(c.method) || (c.flags & ~6u))
                return fail("Invalid signal connection: " + c.signal + " -> " + c.method + ".");
        return true;
    };
    auto check_nodes = [&](const std::vector<NodeDesc>& nodes) -> bool {
        if (nodes.empty()) return fail("The scene must have a root node.");
        if (nodes.size() > MaxNodes) return fail("At most 256 nodes per scene.");
        std::unordered_set<uint32_t> ids;
        std::unordered_set<std::string> siblings;
        size_t meshes = 0;
        for (size_t i = 0; i < nodes.size(); ++i) {
            const auto& n = nodes[i];
            if (i == 0 ? n.parent != -1 : (n.parent < 0 || size_t(n.parent) >= i)) return fail("Invalid node hierarchy.");
            if (!n.id || n.id >= InstanceIdBase || !ids.insert(n.id).second) return fail("Duplicate or null node identifier.");
            if (!valid_name(n.name)) return fail("Invalid node name: \"" + n.name + "\" (1..80 characters, without . : @ / \" %).");
            if (!siblings.insert(std::to_string(n.parent) + "/" + n.name).second) return fail("Two sibling nodes have the same name: " + n.name);
            if (uint8_t(n.type) > uint8_t(NodeType::AnimationPlayer)) return fail("Unsupported node type.");
            if (n.type == NodeType::AnimationPlayer) {
                const auto& animations = n.more().player_animations;
                if (animations.size() > MaxAnimations) return fail("At most 64 animations in " + n.name + ".");
                for (const auto& a : animations) {
                    bool ok = !a.name.empty() && a.name.size() <= 80 && std::isfinite(a.length) && a.length > 0 && a.length <= 10000
                        && a.loop <= 2 && a.tracks.size() <= MaxTracks;
                    for (const auto& t : a.tracks) {
                        ok = ok && t.path.size() <= 256 && t.property.size() <= 80 && t.interpolation <= 1 && t.keys.size() <= MaxKeys;
                        for (size_t k = 0; ok && k < t.keys.size(); ++k)
                            ok = std::isfinite(t.keys[k].time) && t.keys[k].time >= 0 && t.keys[k].time <= a.length
                                && std::isfinite(t.keys[k].transition) && (k == 0 || t.keys[k].time >= t.keys[k - 1].time)
                                && t.keys[k].method.size() <= 80 && t.keys[k].args.size() <= 8;
                    }
                    if (!ok) return fail("Invalid animation \"" + a.name + "\" in " + n.name + ".");
                }
            }
            if (n.type == NodeType::Timer && (!std::isfinite(n.more().lifetime) || n.more().lifetime < 0.001f || n.more().lifetime > 100000))
                return fail("Invalid wait time in " + n.name + " (0.001 to 100000 seconds).");
            if (n.type == NodeType::CPUParticles3D) {
                const float values[] = {n.more().lifetime, n.more().lifetime_randomness, n.more().explosiveness, n.more().spread, n.more().velocity_min, n.more().velocity_max,
                                        n.more().damping_min, n.more().damping_max, n.more().emission_radius};
                for (float v : values) if (!std::isfinite(v)) return fail("Invalid particle parameter in " + n.name + ".");
                if (n.more().amount < 1 || n.more().amount > MaxParticles || n.more().lifetime < 0.01f || n.more().lifetime > 600 || n.more().lifetime_randomness < 0
                    || n.more().lifetime_randomness > 1 || n.more().explosiveness < 0 || n.more().explosiveness > 1 || n.more().spread < 0 || n.more().spread > 180
                    || n.more().velocity_min < 0 || n.more().velocity_max < 0 || n.more().velocity_min > 1000 || n.more().velocity_max > 1000 || !bounded(n.more().direction, 1000)
                    || n.more().damping_min < 0 || n.more().damping_max < 0 || n.more().damping_min > 1000 || n.more().damping_max > 1000
                    || !bounded(n.more().gravity, 1000) || n.more().emission_radius < 0 || n.more().emission_radius > 1000 || !bounded(n.more().emission_box, 1000)
                    || n.more().emission_box.x < 0 || n.more().emission_box.y < 0 || n.more().emission_box.z < 0
                    || uint8_t(n.more().particle_style) > uint8_t(ParticleStyle::Puff) || uint8_t(n.more().emission_shape) > uint8_t(EmissionShape::Box)
                    || n.color > 0xffffff || n.more().color_end > 0xffffff || !std::isfinite(n.alpha) || n.alpha < 0 || n.alpha > 1
                    || !std::isfinite(n.more().puff_size) || !std::isfinite(n.more().puff_size_end) || n.more().puff_size < 0 || n.more().puff_size > 100
                    || n.more().puff_size_end < 0 || n.more().puff_size_end > 100)
                    return fail("Particle parameter out of range in " + n.name + " (amount 1-200, lifetime 0.01-600 s, spread 0-180).");
            }
            // Sizes in metres, or in pixels for 2D shapes (whose box depth isn't used).
            const float most = physics::is_flat(n.type) ? 100000 : 1000;
            if (physics::is_collision_shape(n.type)
                && (uint8_t(n.more().collision) > uint8_t(CollisionKind::Capsule) || !bounded(n.more().box_size, most) || n.more().box_size.x <= 0 || n.more().box_size.y <= 0
                    || (n.type == NodeType::CollisionShape3D && n.more().box_size.z <= 0) || !std::isfinite(n.more().radius) || n.more().radius <= 0
                    || n.more().radius > most || !std::isfinite(n.more().height) || n.more().height <= 0 || n.more().height > most))
                return fail("Invalid collision shape in " + n.name + ".");
            if (physics::is_collision_object(n.type)) {
                const float values[] = {n.more().mass, n.more().gravity_scale, n.more().linear_damp, n.more().angular_damp, n.more().friction, n.more().bounce, n.more().floor_max_angle};
                for (float v : values) if (!std::isfinite(v) || std::abs(v) > 100000) return fail("Invalid physics parameter in " + n.name + ".");
                if (n.more().mass <= 0 || n.more().bounce < 0 || n.more().bounce > 1 || n.more().friction < 0 || n.more().linear_damp < 0 || n.more().angular_damp < 0
                    || n.more().floor_max_angle < 0 || n.more().floor_max_angle > 89 || !bounded(n.more().linear_velocity, most) || !bounded(n.more().angular_velocity, 1000))
                    return fail("Physics parameter out of range in " + n.name + ".");
            }
            if (is_node2d(n.type)) {
                // Negative scales flip, as in Godot.
                if (!bounded(n.position, 100000) || !bounded(n.rotation, 100000) || !bounded(n.scale, 1000) || !std::isfinite(n.offset[0])
                    || !std::isfinite(n.offset[1]) || std::abs(n.offset[0]) > 100000 || std::abs(n.offset[1]) > 100000
                    || n.z_index < -4096 || n.z_index > 4096 || n.color > 0xffffff || !std::isfinite(n.alpha) || n.alpha < 0 || n.alpha > 1)
                    return fail("Invalid 2D node: " + n.name + ".");
            } else if (!bounded(n.position, is_control(n.type) ? 10000 : 1000) || !bounded(n.rotation, 100000) || !valid_scale(n.scale)) {
                return fail("Transform out of range in " + n.name + ".");
            }
            if (n.type == NodeType::Sprite2D || n.type == NodeType::AnimatedSprite2D) {
                std::vector<SpriteAnimation> animations;
                if (!texture_ok(n.texture) || n.hframes < 1 || n.vframes < 1 || n.hframes > 256 || n.vframes > 256
                    || n.frame >= n.hframes * n.vframes || n.more().animations.size() > MaxText
                    || !parse_animations(n.more().animations, n.hframes * n.vframes, animations) || !std::isfinite(n.more().speed_scale)
                    || std::abs(n.more().speed_scale) > 100)
                    return fail("Invalid sprite in " + n.name + ".");
            }
            if (n.type == NodeType::Camera2D) {
                const float* zoom = n.more().zoom;
                if (!std::isfinite(zoom[0]) || !std::isfinite(zoom[1]) || zoom[0] <= 0.01f || zoom[1] <= 0.01f || zoom[0] > 100 || zoom[1] > 100)
                    return fail("Invalid Camera2D zoom in " + n.name + ".");
            }
            if (is_control(n.type) && (n.more().text.size() > MaxText || !n.more().size.finite() || n.more().size.x < 0 || n.more().size.y < 0 || n.more().size.x > 4096
                || n.more().size.y > 4096 || n.more().text_scale < 1 || n.more().text_scale > 8 || n.color > 0xffffff || !std::isfinite(n.alpha) || n.alpha < 0
                || n.alpha > 1 || !texture_ok(n.texture))) return fail("Invalid 2D node: " + n.name + ".");
            if (n.type == NodeType::MeshInstance3D) {
                if (++meshes > MaxMeshes) return fail("At most 128 meshes per scene.");
                if (uint8_t(n.shape) > uint8_t(Shape::Mesh) || uint8_t(n.shading) > uint8_t(Shading::Water)
                    || n.color > 0xffffff || n.segments < 3 || n.segments > 48) return fail("Invalid mesh in " + n.name + ".");
                // mesh -1: no model assigned yet, the node draws nothing
                if (n.shape == Shape::Mesh && (n.mesh < -1 || (n.mesh >= 0 && size_t(n.mesh) >= d.meshes.size())))
                    return fail("Missing model for " + n.name + ".");
                if (!std::isfinite(n.diffuse) || n.diffuse < 0 || n.diffuse > 1) return fail("Invalid material in " + n.name + ".");
                const bool uv_ok = std::isfinite(n.uv_scale[0]) && std::isfinite(n.uv_scale[1]) && std::isfinite(n.uv_offset[0])
                    && std::isfinite(n.uv_offset[1]) && std::abs(n.uv_scale[0]) <= 1000 && std::abs(n.uv_scale[1]) <= 1000
                    && std::abs(n.uv_offset[0]) <= 1000 && std::abs(n.uv_offset[1]) <= 1000;
                if (!texture_ok(n.texture) || uint8_t(n.cull) > uint8_t(CullMode::Disabled) || uint8_t(n.draw_order) > uint8_t(DrawOrder::OnTop)
                    || !uv_ok || !std::isfinite(n.alpha)
                    || n.alpha < 0 || n.alpha > 1 || !std::isfinite(n.specular) || n.specular < 0 || n.specular > 1)
                    return fail("Invalid material in " + n.name + ".");
            }
            if (n.type == NodeType::Sprite3D) {
                if (++meshes > MaxMeshes) return fail("At most 128 meshes per scene.");
                if (!texture_ok(n.texture) || n.hframes < 1 || n.vframes < 1 || n.hframes > 256 || n.vframes > 256
                    || n.frame >= n.hframes * n.vframes || !std::isfinite(n.pixel_size) || n.pixel_size <= 0 || n.pixel_size > 100
                    || n.color > 0xffffff || !std::isfinite(n.alpha) || n.alpha < 0 || n.alpha > 1
                    || uint8_t(n.draw_order) > uint8_t(DrawOrder::OnTop)) return fail("Invalid sprite in " + n.name + ".");
            }
            if (n.type == NodeType::Camera3D && (!std::isfinite(n.more().fov) || n.more().fov < 1 || n.more().fov > 179 || !std::isfinite(n.more().near)
                || !std::isfinite(n.more().far) || n.more().near < 0.001f || n.more().far <= n.more().near || n.more().far > 10000)) return fail("Invalid camera in " + n.name + ".");
            if (n.type == NodeType::DirectionalLight3D && (n.more().light_color > 0xffffff || !std::isfinite(n.more().energy) || n.more().energy < 0 || n.more().energy > 16))
                return fail("Invalid light in " + n.name + ".");
            if (n.type == NodeType::WorldEnvironment && (n.more().background > 0xffffff || n.more().ambient > 0xffffff
                || uint8_t(n.more().background_mode) > uint8_t(Background::Panorama) || n.more().sky_top > 0xffffff || n.more().sky_horizon > 0xffffff
                || n.more().ground_horizon > 0xffffff || n.more().ground_bottom > 0xffffff || !texture_ok(n.more().sky_texture)))
                return fail("Invalid environment in " + n.name + ".");
            if (n.script < -1 || (n.script >= 0 && size_t(n.script) >= d.scripts.size())) return fail("Missing script for " + n.name + ".");
            for (const auto& o : n.overrides) if (o.name.empty() || o.name.size() > 80) return fail("Invalid script property in " + n.name + ".");
        }
        return true;
    };
    if (!check_nodes(d.nodes) || !check_connections(d.nodes, d.connections)) return false;

    if (d.autoloads.size() > MaxAutoloads) return fail("At most 16 autoloads.");

    for (size_t i = 0; i < d.autoloads.size(); ++i) {
        const auto& a = d.autoloads[i];
        bool ok = !a.name.empty() && a.name.size() <= 64 && !std::isdigit(uint8_t(a.name[0])) && a.name != "Input"
            && a.script >= 0 && size_t(a.script) < d.scripts.size();
        for (char c : a.name) ok = ok && (std::isalnum(uint8_t(c)) || c == '_');
        for (size_t j = 0; j < i; ++j) ok = ok && d.autoloads[j].name != a.name;
        if (!ok) return fail("Invalid autoload: \"" + a.name + "\" (a unique name of letters, digits and _, and a script).");
    }

    if (d.scenes.size() > MaxScenes) return fail("At most 64 scenes per project.");
    for (size_t i = 0; i < d.scenes.size(); ++i) {
        const auto& scene = d.scenes[i];
        bool ok = scene.path.rfind("res://", 0) == 0 && scene.path.size() <= 512 && scene.path != d.path;
        for (size_t j = 0; j < i; ++j) ok = ok && d.scenes[j].path != scene.path;
        if (!ok) return fail("Invalid or duplicate scene path: " + scene.path);
        if (!check_nodes(scene.nodes) || !check_connections(scene.nodes, scene.connections)) {
            error += " (" + scene.path + ")";
            return false;
        }
    }
    return true;
}

Runtime::Runtime() : color_(size_t(DefaultWidth / BufferColumnStep) * (DefaultHeight / BufferRowStep)),
    depth_(Z_BUFFERING ? ZBUFFER_STRIDE(DefaultWidth) * DefaultHeight : 0),
    target_(color_.data()), sun_({35, 50, 0}, {255, 255, 255}, 220), ambient_({78, 88, 110}),
    scene_(color_.data(), Z_BUFFERING ? depth_.data() : nullptr, DefaultWidth, DefaultHeight),
    physics_(std::make_unique<physics::World>()),
    host_(std::make_unique<ScriptHost>(this)), vm_(std::make_unique<jetscript::VM>(*host_)) {
    
    scene_.getRenderer()->interlacedMode = FIELD_BUFFERS != 0;
    scene_.setCamera(&camera_);
    scene_.setDirectionalLight(&sun_);
    scene_.setAmbientLight(&ambient_);
    Description empty;
    empty.nodes.push_back({});
    std::string error;
    load(empty, error);
}
Runtime::~Runtime() {
    vm_.reset();
    scene_.getObjects().clear();
}

bool Runtime::load(Description d, std::string& error) {
    if (!validate(d, error)) return false;
    pack_ = nullptr;
    pack_size_ = 0;
    playing_ = false;
    editor_view_ = true;
    vm_->clear();
    vm_->take_errors();
    description_ = std::move(d);
    scene_nodes_ = &description_.nodes;
    current_scene_ = description_.path;
    pending_scene_.clear();
    resize(description_.width, description_.height);
    return build(error);
}

int Runtime::index_of(uint32_t id) const {
    const auto found = index_.find(id);
    return found == index_.end() ? -1 : found->second;
}

namespace {
// The frame of a Sprite3D sheet as a UV window; a flip runs the window backwards.
void sprite_uv(NodeDesc& d) {
    const float w = 1.0f / float(d.hframes), h = 1.0f / float(d.vframes);
    const int col = d.frame % d.hframes, row = d.frame / d.hframes;
    d.uv_scale[0] = d.flip_h ? -w : w;
    d.uv_scale[1] = d.flip_v ? -h : h;
    d.uv_offset[0] = float(d.flip_h ? col + 1 : col) * w;
    d.uv_offset[1] = float(d.flip_v ? row + 1 : row) * h;
}
} // namespace

// Jet's primitives are symmetric under the Z mirror, so they're used as is and keep Jet's winding and UV layout.
Renderer::Object* Runtime::make_mesh(const NodeDesc& d, Renderer::Material* material) const {
    switch (d.shape) {
    case Shape::Box: return Primitives::createCube(100, 100, 100, material);
    case Shape::Sphere: return Primitives::createSphere(50, d.segments, material);
    case Shape::Plane: return Primitives::createPlane(200, 200, material);
    case Shape::Pyramid: {
        Renderer::Object* mesh = Primitives::createPyramid(100, 100, material);
        for (auto& vertex : mesh->vertices) vertex.position.y -= 50; // pivot at the centre
        // Jet's pyramid is wound inwards, flip it so it culls like the others.
        for (auto& t : mesh->triangles) std::swap(t.v2, t.v3);
        return mesh;
    }
    case Shape::Cylinder: return cylinder(d.segments, material);
    case Shape::Capsule: return capsule(d.segments, material);
    case Shape::Quad: return quad(material);
    case Shape::Torus: return torus(d.segments, material);
    case Shape::Mesh: break;
    }
    const auto& asset = description_.meshes[size_t(d.mesh)];
    return make_object(asset.vertices, asset.indices, material);
}

size_t Runtime::node_bytes() const {
    size_t bytes = nodes_.capacity() * sizeof(Live);
    for (const auto& n : nodes_) {
        if (n.d.has_extras()) bytes += sizeof(NodeExtras);
        if (n.motion_.allocated()) bytes += sizeof(Motion);
    }
    return bytes;
}

bool Runtime::load_pack(const uint8_t* data, size_t size, std::string& error, bool textures_in_place) {
    Description d;
    if (!decode_pack(data, size, d, error, textures_in_place) || !load(std::move(d), error)) return false;
    pack_ = data;
    pack_size_ = size;
    // Every scene was validated while decoding; the built one now lives in nodes_.
    std::vector<NodeDesc>().swap(description_.nodes);
    std::vector<SceneConnection>().swap(description_.connections);
    for (auto& scene : description_.scenes) {
        std::vector<NodeDesc>().swap(scene.nodes);
        std::vector<SceneConnection>().swap(scene.connections);
    }
    return true;
}

// The nodes of a scene, from the description or read again from the package.
const std::vector<NodeDesc>* Runtime::nodes_of(std::string_view path, std::vector<SceneConnection>* connections) {
    const std::vector<NodeDesc>* nodes = nullptr;
    const std::vector<SceneConnection>* links = nullptr;
    uint32_t offset = 0;
    if (path == description_.path) { nodes = &description_.nodes; links = &description_.connections; offset = description_.nodes_offset; }
    for (const auto& scene : description_.scenes)
        if (scene.path == path) { nodes = &scene.nodes; links = &scene.connections; offset = scene.offset; }
    if (!nodes || !pack_ || !nodes->empty()) {
        if (connections && links) *connections = *links;
        return nodes;
    }
    std::string error;
    std::vector<SceneConnection> read;
    if (!decode_nodes(pack_, pack_size_, offset, pack_nodes_, read, error)) return nullptr;
    if (connections) *connections = std::move(read);
    return &pack_nodes_;
}

bool Runtime::build(std::string& error) {
    error.clear();
    // The scene only borrows the objects, so drop its pointers first.
    scene_.getObjects().clear();
    objects_.clear();
    queues_reserved_ = false;
    shared_meshes_.clear();
    materials_.clear();
    textures_.clear();
    nodes_.clear();
    index_.clear();
    meshes_ = 0;
    pending_free_ = reorder_needed_ = false;
    next_instance_id_ = InstanceIdBase;
    if (!scene_nodes_) scene_nodes_ = &description_.nodes;
    const auto& scene_nodes = *scene_nodes_;

    nodes_.clear();
    nodes_.reserve(scene_nodes.size() + description_.autoloads.size());
    nodes_.resize(scene_nodes.size());
    for (size_t i = 0; i < nodes_.size(); ++i) {
        Live& n = nodes_[i];
        n.d = scene_nodes[i];
        n.parent = n.d.parent;
        if (n.parent >= 0) nodes_[n.parent].children.push_back(int32_t(i));
        index_[n.d.id] = int32_t(i);
        create_visual(int32_t(i));
    }
    scene_ids_.clear();
    if (!scene_connections_.empty())
        for (size_t i = 0; i < scene_nodes.size(); ++i) scene_ids_.push_back(nodes_[i].d.id);

    // Autoloads come after the scene, outside its tree, and keep their ids across scene changes.
    for (size_t i = 0; i < description_.autoloads.size(); ++i) {
        Live a;
        a.d.type = NodeType::Node;
        a.d.id = AutoloadIdBase + uint32_t(i);
        a.d.name = description_.autoloads[i].name;
        a.d.script = description_.autoloads[i].script;
        index_[a.d.id] = int32_t(nodes_.size());
        nodes_.push_back(std::move(a));
    }
    if (scene_nodes_ == &pack_nodes_) {
        std::vector<NodeDesc>().swap(pack_nodes_);
        scene_nodes_ = nullptr;
    }

    collect_physics();
    particles_.clear(); // created by start()
    particle_triangles_ = 0;
    update_transforms();
    apply_environment();
    apply_camera();
    return true;
}

// The Jet object, material and texture of a mesh or sprite node.
void Runtime::create_visual(int32_t index) {
    Live& n = nodes_[size_t(index)];
    if (n.d.type == NodeType::Sprite3D) {
        // A quad, unlit and double-sided like Godot's Sprite3D, the frame picked through the UVs.
        n.d.shape = Shape::Quad;
        n.d.shading = Shading::Unlit;
        n.d.cull = CullMode::Disabled;
        n.d.repeat = false;
        n.d.perspective = true;
        n.d.reflection = false;
        sprite_uv(n.d);
    } else if (n.d.type != NodeType::MeshInstance3D || (n.d.shape == Shape::Mesh && n.d.mesh < 0)) {
        return;
    }

    auto material = std::make_unique<Renderer::Material>(rgb565(n.d.color));
    // Reflections rewrite their UVs every frame, so they always keep geometry of their own.
    // The others get their shared copy at the first bake, once the world scale is known.
    Renderer::Object* mesh = n.d.reflection ? make_mesh(n.d, material.get()) : new Renderer::Object();
    mesh->calculateBoundingBox();
    n.baked_scale = {0, 0, 0}; // forces the first bake
    n.object = mesh;
    n.material = material.get();
    if (n.d.texture >= 0) {
        const auto& asset = description_.textures[size_t(n.d.texture)];
        const bool indexed = !asset.palette.empty();
        auto* data = indexed ? reinterpret_cast<uint16_t*>(const_cast<uint8_t*>(asset.index_data()))
                             : const_cast<uint16_t*>(asset.pixels.data());
        auto texture = std::make_unique<Renderer::Texture>(asset.width, asset.height, data, asset.has_alpha, asset.alpha_color,
            false, Renderer::WRAP, indexed ? const_cast<uint16_t*>(asset.palette.data()) : nullptr);
        n.texture = texture.get();
        material->diffuseMap = n.texture;
        textures_.push_back(std::move(texture));
    }

    n.applied = Applied::of(n.d);
    apply_material(n);
    scene_.addObject(mesh);
    objects_.emplace_back(mesh);
    materials_.push_back(std::move(material));
    ++meshes_;
}

// Frees what a freed node holds outside nodes_: its script, Jet object, material, texture and particles.
void Runtime::release_node(Live& n) {
    if (vm_->has_instance(n.d.id)) vm_->detach(n.d.id);
    vm_->forget(n.d.id);
    if (n.object) {
        auto& drawn = scene_.getObjects();
        drawn.erase(std::remove(drawn.begin(), drawn.end(), n.object), drawn.end());
        objects_.erase(std::find_if(objects_.begin(), objects_.end(), [&](const auto& o) { return o.get() == n.object; }));
        materials_.erase(std::find_if(materials_.begin(), materials_.end(), [&](const auto& m) { return m.get() == n.material; }));
        if (n.texture)
            textures_.erase(std::find_if(textures_.begin(), textures_.end(), [&](const auto& t) { return t.get() == n.texture; }));
        n.object = nullptr;
        n.material = nullptr;
        n.texture = nullptr;
        --meshes_;
    }
    if (n.particles >= 0) {
        particles_[size_t(n.particles)] = {};
        n.particles = -1;
    }
}

// Each shape belongs to the nearest body or area above it. Called again whenever the tree changes.
void Runtime::collect_physics() {
    shape_owners_.clear();
    collision_objects_.clear();
    for (size_t i = 0; i < nodes_.size(); ++i) {
        const Live& n = nodes_[i];
        if (!physics::is_collision_shape(n.d.type) || n.freed || !n.in_tree) continue;
        // A 2D shape belongs to a 2D body or area, a 3D one to a 3D one.
        const bool flat = physics::is_flat(n.d.type);
        int body = n.parent;
        while (body >= 0 && !(physics::is_collision_object(nodes_[size_t(body)].d.type) && physics::is_flat(nodes_[size_t(body)].d.type) == flat))
            body = nodes_[size_t(body)].parent;
        if (body < 0) continue;
        shape_owners_.emplace_back(int32_t(i), body);
        if (std::find(collision_objects_.begin(), collision_objects_.end(), body) == collision_objects_.end())
            collision_objects_.push_back(body);
    }
    has_physics_ = !shape_owners_.empty();
    physics_->built = physics_->valid = false;
}

namespace {
// Scale and UV transform baked into the vertices, so Jet's bounds match the drawn shape. Normals take the inverse scale.
void bake_vertices(std::vector<Renderer::Object::Vertex>& vertices, Vec3 s, const float* us, const float* uo) {
    if (same(s, {1, 1, 1}) && us[0] == 1 && us[1] == 1 && uo[0] == 0 && uo[1] == 0) return;
    for (auto& vertex : vertices) {
        vertex.uv.x = int32_t(std::lround(vertex.uv.x * us[0] + uo[0] * FIXED_POINT_SCALE));
        vertex.uv.y = int32_t(std::lround(vertex.uv.y * us[1] + uo[1] * FIXED_POINT_SCALE));
        vertex.position.x = int32_t(std::lround(vertex.position.x * s.x));
        vertex.position.y = int32_t(std::lround(vertex.position.y * s.y));
        vertex.position.z = int32_t(std::lround(vertex.position.z * s.z));
        auto& normal = vertex.normal;
        const double nx = normal.x / s.x, ny = normal.y / s.y, nz = normal.z / s.z;
        const double length = std::sqrt(nx * nx + ny * ny + nz * nz);
        if (length > 0) normal = {int32_t(nx / length * FIXED_POINT_SCALE), int32_t(ny / length * FIXED_POINT_SCALE), int32_t(nz / length * FIXED_POINT_SCALE)};
    }
}

float radius_of(const std::vector<Renderer::Object::Vertex>& vertices) {
    float r2 = 0;
    for (const auto& v : vertices)
        r2 = std::max(r2, float(v.position.x) * v.position.x + float(v.position.y) * v.position.y + float(v.position.z) * v.position.z);
    return std::sqrt(r2);
}
} // namespace

// Nodes with the same mesh, world scale and UVs draw one shared copy, like MeshInstance3Ds that share a Godot Mesh.
// Each keeps its own Jet object for the transform and the material, with the copy as its only instance.
void Runtime::share(Live& n, Vec3 s) {
    const auto q = [](float v) { return int32_t(std::lround(v * 10000)); };
    const NodeDesc& d = n.d;
    const bool segmented = d.shape == Shape::Sphere || d.shape == Shape::Cylinder || d.shape == Shape::Capsule || d.shape == Shape::Torus;
    // Front-face culling is a reversed winding (see apply_material), so it is part of the copy.
    const bool flip = d.cull == CullMode::Front;
    const std::array<int32_t, 11> key{int32_t(d.shape), segmented ? int32_t(d.segments) : 0, d.shape == Shape::Mesh ? d.mesh : -1,
        q(s.x), q(s.y), q(s.z), q(d.uv_scale[0]), q(d.uv_scale[1]), q(d.uv_offset[0]), q(d.uv_offset[1]), flip};
    auto& slot = shared_meshes_[key];
    Renderer::Object::SharedMesh mesh = slot.lock();
    if (!mesh) {
        std::unique_ptr<Renderer::Object> geometry(make_mesh(d, &shared_material_));
        bake_vertices(geometry->vertices, s, d.uv_scale, d.uv_offset);
        if (flip) for (auto& t : geometry->triangles) std::swap(t.v2, t.v3);
        mesh = Renderer::Object::freezeMesh(std::move(*geometry));
        slot = mesh;
    }
    n.object->addInstance(mesh, {}, n.material);
    n.object->calculateBoundingBox();
    n.flipped = flip;
    n.baked_scale = s;
    n.jet_radius = radius_of(mesh->vertices);
}

void Runtime::bake(Live& n, Vec3 s) {
    auto& object = *n.object;
    const bool first = same(n.baked_scale, {0, 0, 0});
    if (first && object.vertices.empty()) {
        share(n, s);
        return;
    }
    // A script changed the scale or the UVs: the node leaves its shared copy for geometry of its own.
    // That geometry stays unscaled in base_vertices for the next changes; most meshes never get here.
    if (!object.instances.empty()) {
        object.instances.clear();
        std::unique_ptr<Renderer::Object> fresh(make_mesh(n.d, n.material));
        object.triangles = std::move(fresh->triangles);
        if (n.d.cull == CullMode::Front) for (auto& t : object.triangles) std::swap(t.v2, t.v3);
        n.flipped = n.d.cull == CullMode::Front;
        n.base_vertices = std::move(fresh->vertices);
    } else if (n.base_vertices.empty() && !first) {
        std::unique_ptr<Renderer::Object> fresh(make_mesh(n.d, n.material));
        n.base_vertices = std::move(fresh->vertices);
    }
    if (!n.base_vertices.empty()) object.vertices = n.base_vertices;
    bake_vertices(object.vertices, s, n.d.uv_scale, n.d.uv_offset);
    object.calculateBoundingBox();
    n.baked_scale = s;
    n.jet_radius = radius_of(object.vertices);
}

namespace {
uint16_t tint565(uint16_t c, uint32_t albedo) {
    const uint32_t r = (c >> 11) & 31, g = (c >> 5) & 63, b = c & 31;
    return uint16_t(((r * ((albedo >> 16) & 255) + 127) / 255) << 11 | ((g * ((albedo >> 8) & 255) + 127) / 255) << 5
                    | ((b * (albedo & 255) + 127) / 255));
}
bool same_material(const NodeDesc& a, const Runtime::Applied& b) {
    return a.color == b.color && a.shading == b.shading && a.alpha == b.alpha && a.specular == b.specular && a.shininess == b.shininess
        && a.diffuse == b.diffuse
        && a.bilinear == b.bilinear && a.repeat == b.repeat && a.perspective == b.perspective && a.cull == b.cull && a.billboard == b.billboard;
}
bool same_uv(const NodeDesc& a, const Runtime::Applied& b) {
    return a.uv_scale[0] == b.uv_scale[0] && a.uv_scale[1] == b.uv_scale[1] && a.uv_offset[0] == b.uv_offset[0] && a.uv_offset[1] == b.uv_offset[1];
}
} // namespace

void Runtime::apply_material(Live& n) {
    static const Renderer::ShadingMode modes[] = {Renderer::ShadingMode::GOURAUD, Renderer::ShadingMode::FLAT, Renderer::ShadingMode::UNLIT,
        Renderer::ShadingMode::PHONG, Renderer::ShadingMode::ADDITIVE, Renderer::ShadingMode::WATER_REFLECT};
    const NodeDesc& d = n.d;
    auto& m = *n.material;
    m.shadingMode = modes[uint8_t(d.shading)];
    m.alpha = uint8_t(std::lround(std::clamp(d.alpha, 0.0f, 1.0f) * 255));
    m.specular = uint8_t(std::lround(std::clamp(d.specular, 0.0f, 1.0f) * 255));
    m.specularExponent = d.shininess;
    m.diffuse = uint8_t(std::lround(std::clamp(d.diffuse, 0.0f, 1.0f) * 255));
    m.perspectiveCorrect = d.perspective;
    m.color = rgb565(d.color);
    if (n.texture) {
        // Albedo multiplies the texture, as in Godot. Palette textures are tinted through the palette, which is cheap.
        // Jet only filters RGB565 data bilinearly, so bilinear filtering expands a palette texture to pixels.
        // The colour key is left alone, and a tinted colour that lands on it is nudged by one bit.
        const auto& asset = description_.textures[size_t(d.texture)];
        const bool indexed = !asset.palette.empty();
        const bool expand = indexed && d.bilinear && BILINEAR_FILTER;
        auto tint = [&](uint16_t c) {
            if (d.color == 0xffffff || (asset.has_alpha && c == asset.alpha_color)) return c;
            c = tint565(c, d.color);
            return asset.has_alpha && c == asset.alpha_color ? uint16_t(c ^ 1) : c;
        };
        if (expand) {
            std::vector<uint16_t> palette(asset.palette.size());
            for (size_t i = 0; i < palette.size(); ++i) palette[i] = tint(asset.palette[i]);
            n.tinted.resize(size_t(asset.width) * asset.height);
            for (size_t i = 0; i < n.tinted.size(); ++i) n.tinted[i] = palette[asset.index_data()[i]];
        } else if (d.color != 0xffffff) {
            const std::vector<uint16_t>& source = indexed ? asset.palette : asset.pixels;
            n.tinted.resize(source.size());
            for (size_t i = 0; i < source.size(); ++i) n.tinted[i] = tint(source[i]);
        } else {
            n.tinted.clear();
        }
        if (indexed && !expand) {
            n.texture->data = reinterpret_cast<uint16_t*>(const_cast<uint8_t*>(asset.index_data()));
            n.texture->palette = const_cast<uint16_t*>(n.tinted.empty() ? asset.palette.data() : n.tinted.data());
        } else {
            n.texture->data = const_cast<uint16_t*>(n.tinted.empty() ? asset.pixels.data() : n.tinted.data());
            n.texture->palette = nullptr;
        }
        n.texture->bilinear = d.bilinear;
        n.texture->addressMode = d.repeat ? Renderer::WRAP : Renderer::CLAMP;
        m.color = 0xffff;
    }
    auto& o = *n.object;
    // Jet's CULL_FRONTFACES draws nothing, so front culling is a reversed winding plus back-face culling.
    // A node drawing a shared copy has no triangles yet: share() picks a copy with the right winding.
    const bool flip = d.cull == CullMode::Front;
    if (flip != n.flipped && !o.triangles.empty()) {
        for (auto& t : o.triangles) std::swap(t.v2, t.v3);
        n.flipped = flip;
    }
    o.cullingMode = d.cull == CullMode::Disabled ? Renderer::CullingMode::NO_CULLING : Renderer::CullingMode::CULL_BACKFACES;
    o.isBillboard = d.billboard;
    // Only Background and OnTop get their own draw bands; translucent and additive stay depth-sorted with the rest.
    o.noWriteZBuffer = d.draw_order == DrawOrder::Background;
    o.ignoreZBuffer = d.draw_order == DrawOrder::OnTop;
    // Without a z-buffer (device) triangles in one depth bucket keep scene order, e.g. a far wheel over the bodywork.
    o.preciseDepthSort = d.draw_order == DrawOrder::Default;
    n.applied = Applied::of(d);
}

void Runtime::sync_object(Live& n) {
    if (n.d.type == NodeType::Sprite3D) sprite_uv(n.d); // a script may have changed frame or flips
    if (n.synced && n.synced_visible == n.world_visible && n.synced_world == n.world && same_material(n.d, n.applied)
        && same_uv(n.d, n.applied)) return; // nothing changed since the last frame
    n.synced = true;
    n.synced_world = n.world;
    n.synced_visible = n.world_visible;
    Vec3 scale;
    const Basis rotation = n.world.basis.orthonormalized(&scale);
    bool shown = n.world_visible;
    if (n.d.type == NodeType::Sprite3D) {
        // The quad takes the size of one frame, pixel_size metres per texel; no texture, nothing drawn.
        shown = shown && n.d.texture >= 0;
        if (n.d.texture >= 0) {
            const auto& t = description_.textures[size_t(n.d.texture)];
            scale.x *= float(t.width) / float(n.d.hframes) * n.d.pixel_size;
            scale.y *= float(t.height) / float(n.d.vframes) * n.d.pixel_size;
        }
    }
    if (!same(scale, n.baked_scale) || (n.object->vertices.empty() && n.object->instances.empty()) || !same_uv(n.d, n.applied)) {
        n.applied.uv_scale[0] = n.d.uv_scale[0]; n.applied.uv_scale[1] = n.d.uv_scale[1];
        n.applied.uv_offset[0] = n.d.uv_offset[0]; n.applied.uv_offset[1] = n.d.uv_offset[1];
        bake(n, scale);
    }
    if (!same_material(n.d, n.applied)) apply_material(n);
    const Vec3 e = rotation.mirrored_z().euler_zyx();
    n.object->setRotation(degrees_int(e.x), degrees_int(e.y), degrees_int(e.z));
    n.object->setPosition(to_jet(n.world.origin));
    n.object->enabled = shown;
}

// Physics ticks pass sync = false: world transforms only, the Jet objects are synced later.
void Runtime::update_transforms(bool sync) {
    for (auto& n : nodes_) {
        const Live* parent = n.parent >= 0 && is_node3d(nodes_[n.parent].d.type) ? &nodes_[n.parent] : nullptr;
        if (is_node3d(n.d.type)) {
            if (!n.local_valid || n.d.position != n.local_position || n.d.rotation != n.local_rotation || n.d.scale != n.local_scale) {
                n.local = local_transform(n.d.position, n.d.rotation, n.d.scale);
                n.local_position = n.d.position;
                n.local_rotation = n.d.rotation;
                n.local_scale = n.d.scale;
                n.local_valid = true;
            }
            n.world = parent ? parent->world * n.local : n.local;
            n.world_visible = n.d.visible && !n.freed && n.in_tree && (!parent || parent->world_visible);
        } else if (is_node2d(n.d.type)) {
            // A 2D node inherits from a 2D parent; under anything else it starts a canvas of its own, as in Godot.
            const Live* parent2d = n.parent >= 0 && is_node2d(nodes_[n.parent].d.type) ? &nodes_[n.parent] : nullptr;
            if (!n.local_valid || n.d.position != n.local_position || n.d.rotation != n.local_rotation || n.d.scale != n.local_scale) {
                const Vec3 position{n.d.position.x, n.d.position.y, 0}, rotation{0, 0, n.d.rotation.z}, scale{n.d.scale.x, n.d.scale.y, 1};
                n.local = local_transform(position, rotation, scale);
                n.local_position = n.d.position;
                n.local_rotation = n.d.rotation;
                n.local_scale = n.d.scale;
                n.local_valid = true;
            }
            n.world = parent2d ? parent2d->world * n.local : n.local;
            n.world_visible = n.d.visible && !n.freed && n.in_tree && (!parent2d || parent2d->world_visible);
            n.canvas_z = int16_t(std::clamp(n.d.z_index + (parent2d ? parent2d->canvas_z : 0), -4096, 4096));
        } else if (is_control(n.d.type)) {
            // As in Godot, a Control sits relative to its parent Control, and any hidden ancestor hides it.
            const bool nested = n.parent >= 0 && is_control(nodes_[n.parent].d.type);
            n.world = {};
            n.world.origin = nested ? nodes_[n.parent].world.origin + n.d.position : n.d.position;
            n.world_visible = n.d.visible && !n.freed && n.in_tree && (n.parent < 0 || nodes_[n.parent].world_visible);
        } else {
            n.world = {};
            n.world_visible = !n.freed && n.in_tree;
        }
        if (sync && n.object) sync_object(n);
    }
    transforms_stale_ = false;
}

void Runtime::apply_environment() {
    const Live* environment = nullptr;
    const Live* light = nullptr;
    for (const auto& n : nodes_) {
        if (n.freed || !n.in_tree) continue;
        if (!environment && n.d.type == NodeType::WorldEnvironment) environment = &n;
        if (!light && n.d.type == NodeType::DirectionalLight3D && n.world_visible) light = &n;
    }
    scene_.setBackcolor(rgb565(environment ? environment->d.more().background : 0x161c28));
    ambient_.color = color_of(environment ? environment->d.more().ambient : 0x4e586e);
    if (!light) { scene_.setDirectionalLight(nullptr); return; }
    // Godot lights shine along -Z; Jet wants the direction towards the light.
    const Vec3 toward = light->world.basis.orthonormalized().column(2).normalized();
    sun_.worldLightDir = {int32_t(std::lround(toward.x * FIXED_POINT_SCALE)), int32_t(std::lround(toward.y * FIXED_POINT_SCALE)),
                          int32_t(std::lround(-toward.z * FIXED_POINT_SCALE))};
    sun_.lightDir = sun_.worldLightDir;
    sun_.color = color_of(light->d.more().light_color);
    sun_.intensity = uint16_t(std::clamp(light->d.more().energy * 220.0f, 0.0f, 255.0f));
    scene_.setDirectionalLight(&sun_);
}

void Runtime::apply_camera() {
    const Live* camera = nullptr;
    if (playing_ && !editor_view_) {
        for (const auto& n : nodes_) if (!n.freed && n.in_tree && n.d.type == NodeType::Camera3D && n.d.more().current) { camera = &n; break; }
        if (!camera) for (const auto& n : nodes_) if (!n.freed && n.in_tree && n.d.type == NodeType::Camera3D) { camera = &n; break; }
    }
    Basis basis;
    Vec3 position;
    float fov = editor_fov_, near = 0.05f, far = 500;
    if (camera) {
        basis = camera->world.basis.orthonormalized();
        position = camera->world.origin;
        fov = camera->d.more().fov; near = camera->d.more().near; far = camera->d.more().far;
    } else {
        basis = Basis::from_euler(editor_rotation_);
        position = editor_position_;
    }
    view_basis_ = basis;
    view_fov_ = fov;
    // Jet's camera basis is Ry * Rx * Rz in its own (mirrored) space.
    const Vec3 e = basis.mirrored_z().euler();
    camera_.setPosition(to_jet(position));
    camera_.rotation.x = e.x;
    camera_.rotation.y = e.y;
    camera_.rotation.z = e.z;
    // Godot's FOV is vertical; Jet's projection factor is derived from the width.
    const float horizontal = 2 * std::atan(float(width_) / height_ * std::tan(radians(fov) * 0.5f));
    camera_.setFOV(degrees(horizontal), width_);
    camera_.nearPlane = std::clamp(int32_t(near * JetUnits), int32_t(5), int32_t(1000));
    camera_.farPlane = std::clamp(int32_t(far * JetUnits), camera_.nearPlane + 1, int32_t(60000));
}

void Runtime::set_editor_camera(Vec3 position, Vec3 rotation, float fov) {
    if (!position.finite() || !rotation.finite() || !std::isfinite(fov)) return;
    editor_position_ = position;
    editor_rotation_ = rotation;
    editor_fov_ = std::clamp(fov, 1.0f, 179.0f);
}

void Runtime::log(std::string text, bool error, std::string path, int line) {
    if (log_.size() >= 512) log_.erase(log_.begin(), log_.begin() + 256);
    log_.push_back({std::move(text), error, std::move(path), line});
}

std::vector<LogLine> Runtime::take_log() {
    std::vector<LogLine> out;
    out.swap(log_);
    return out;
}

void Runtime::flush_script_errors() {
    for (auto& e : vm_->take_errors()) {
        std::string where = e.path + ":" + std::to_string(e.line);
        if (!e.function.empty()) where += " in " + e.function + "()";
        log("Script error: " + e.message + "  (" + where + ")", true, e.path, e.line);
        for (size_t i = 1; i < e.stack.size(); ++i)
            log("    at " + e.stack[i].function + "() " + e.stack[i].path + ":" + std::to_string(e.stack[i].line), true,
                e.stack[i].path, e.stack[i].line);
    }
}

namespace {
// Scene overrides come from JSON, which has no Vector3 or Color.
bool coerce(const jetscript::Value& in, jetscript::Type type, jetscript::Value& out) {
    using jetscript::Type;
    const Type from = in.type();
    if (type == Type::Nil || from == type) { out = in; return true; }
    if (type == Type::Float && from == Type::Int) { out = jetscript::Value(in.as_real()); return true; }
    if (type == Type::Int && from == Type::Float && std::isfinite(in.as_real())) { out = jetscript::Value(jetscript::Int(in.as_real())); return true; }
    if (type == Type::Bool && from == Type::Int) { out = jetscript::Value(in.as_int() != 0); return true; }
    if ((type == Type::Vector3 || type == Type::Color) && from == Type::Array && in.array_size() == 3) {
        float v[3];
        for (int i = 0; i < 3; ++i) {
            const auto item = in.array_get(size_t(i));
            if (!item.is_number()) return false;
            v[i] = item.as_real();
        }
        out = type == Type::Vector3 ? jetscript::Value(jetscript::Vector3{v[0], v[1], v[2]}) : jetscript::Value(jetscript::Color{v[0], v[1], v[2]});
        return true;
    }
    if (type == Type::Vector2 && from == Type::Array && in.array_size() == 2) {
        const auto x = in.array_get(0), y = in.array_get(1);
        if (!x.is_number() || !y.is_number()) return false;
        out = jetscript::Value(jetscript::Vector2{x.as_real(), y.as_real()});
        return true;
    }
    if (type == Type::Color && from == Type::Int) {
        const auto c = uint32_t(in.as_int());
        out = jetscript::Value(jetscript::Color{((c >> 16) & 255) / 255.0f, ((c >> 8) & 255) / 255.0f, (c & 255) / 255.0f});
        return true;
    }
    return false;
}
} // namespace

bool Runtime::start(std::vector<ScriptDiagnostic>& errors) {
    errors.clear();
    std::string error;
    playing_ = false;
    vm_->clear();
    vm_->take_errors();
    kill_tweens(false);
    scene_nodes_ = nodes_of(description_.path, &scene_connections_);
    current_scene_ = description_.path;
    pending_scene_.clear();
    build(error);
    // Compile every scene's scripts now, so errors show up at Play.
    std::vector<std::shared_ptr<const jetscript::Script>> compiled(description_.scripts.size());
    // A package keeps only the scripts its scenes and autoloads use.
    std::vector<bool> used(description_.scripts.size(), pack_ != nullptr);
    for (const auto& a : description_.autoloads) used[size_t(a.script)] = true;
    for (const auto& n : description_.nodes) if (n.script >= 0) used[n.script] = true;
    for (const auto& scene : description_.scenes)
        for (const auto& n : scene.nodes) if (n.script >= 0) used[n.script] = true;
    for (size_t i = 0; i < compiled.size(); ++i) {
        if (!used[i]) continue;
        const auto& file = description_.scripts[i];
        jetscript::CompileResult result;
        if (!file.bytecode.empty()) {
            result = jetscript::load_bytecode(file.bytecode.data(), file.bytecode.size(), *host_);
        } else {
#ifndef JETSCRIPT_NO_COMPILER
            result = jetscript::compile(file.source, file.path, *host_);
#else
            result.errors.push_back({0, 0, "Script without bytecode: this runtime does not include the compiler."});
#endif
        }
        for (const auto& d : result.errors) errors.push_back({file.path, d.line, d.column, d.message});
        compiled[i] = result.script;
    }
    if (!errors.empty()) return false;
    compiled_ = std::move(compiled);
    playing_ = true;
    editor_view_ = false;
    input_ = previous_input_ = physics_previous_input_ = 0;
    start_autoloads();
    enter_scene();
    return true;
}

// Like Godot, the autoloads are ready before the first scene.
// Scripts may add nodes, which can move nodes_: loops that call them go by id, never keeping a Live&.
void Runtime::start_autoloads() {
    std::vector<uint32_t> ids;
    for (const auto& n : nodes_)
        if (n.d.id >= AutoloadIdBase && n.d.script >= 0) ids.push_back(n.d.id);
    for (uint32_t id : ids) {
        const int i = index_of(id);
        if (i >= 0 && vm_->attach(id, compiled_[size_t(nodes_[size_t(i)].d.script)])) vm_->ready(id);
    }
    flush_script_errors();
}

void Runtime::enter_scene() {
    physics_time_ = 0;
    start_particles();
    for (auto& n : nodes_)
        if (physics::is_rigid(n.d.type)) {
            n.edit_motion().velocity = n.d.more().linear_velocity;
            n.edit_motion().angular = n.d.more().angular_velocity;
        }
    std::vector<uint32_t> ids;
    for (const auto& n : nodes_)
        if (n.d.script >= 0) ids.push_back(n.d.id);
    for (uint32_t id : ids) attach_script(id); // autoloads are already running
    // As in Godot, the scene's connections are made before _ready().
    connect_scene(scene_connections_, scene_ids_);
    std::vector<SceneConnection>().swap(scene_connections_);
    std::vector<uint32_t>().swap(scene_ids_);
    flush_script_errors();
    // _ready(): children before parents, siblings in tree order, as in Godot.
    if (!nodes_.empty()) enter_tree(0);
    free_pending();
    update_transforms();
    for (int32_t i : collision_objects_) nodes_[size_t(i)].edit_motion().last_origin = nodes_[size_t(i)].world.origin;
    apply_environment();
    apply_camera();
}

// Creates the script instance (its member initializers run) and applies the exported values set in the scene.
void Runtime::attach_script(uint32_t id) {
    int i = index_of(id);
    if (i < 0 || nodes_[size_t(i)].d.script < 0 || vm_->has_instance(id)) return;
    const auto& script = compiled_[size_t(nodes_[size_t(i)].d.script)];
    if (!vm_->attach(id, script)) return;

    // The initializers may have added nodes: look the node up again.
    i = index_of(id);
    if (i < 0) return;
    const NodeDesc& d = nodes_[size_t(i)].d;
    for (const auto& o : d.overrides) {
        const auto& exports = script->exports();
        const auto found = std::find_if(exports.begin(), exports.end(), [&](const auto& e) { return e.name == o.name; });
        if (found == exports.end()) continue; // the variable was removed from the script
        jetscript::Value value;
        if (!coerce(o.value, found->type, value) || !vm_->set_member(id, o.name, value))
            log("Invalid value for " + d.name + "." + o.name, true, script->path(), found->line);
    }
}

// The nodes of a scene, copied from the description or read again from the package.
bool Runtime::scene_descs(std::string_view path, std::vector<NodeDesc>& out, std::vector<SceneConnection>& connections) const {
    const std::vector<NodeDesc>* nodes = nullptr;
    const std::vector<SceneConnection>* links = nullptr;
    uint32_t offset = 0;
    if (path == description_.path) { nodes = &description_.nodes; links = &description_.connections; offset = description_.nodes_offset; }
    for (const auto& scene : description_.scenes)
        if (scene.path == path) { nodes = &scene.nodes; links = &scene.connections; offset = scene.offset; }
    if (!nodes) return false;
    if (!nodes->empty() || !pack_) {
        out = *nodes;
        connections = *links;
        return !out.empty();
    }
    std::string error;
    return decode_nodes(pack_, pack_size_, offset, out, connections, error);
}

void Runtime::connect_scene(const std::vector<SceneConnection>& connections, const std::vector<uint32_t>& ids) {
    auto name = [&](uint32_t id) {
        const int i = index_of(id);
        return i >= 0 ? nodes_[size_t(i)].d.name : std::string("?");
    };
    for (const auto& c : connections) {
        if (size_t(c.from) >= ids.size() || size_t(c.to) >= ids.size()) continue;
        const uint32_t from = ids[size_t(c.from)], to = ids[size_t(c.to)];
        std::string error;
        if (!vm_->connect(from, c.signal, to, c.method, c.flags, error))
            log("Signal connection " + name(from) + "." + c.signal + " -> " + name(to) + "." + c.method + "(): " + error, true);
    }
}

// PackedScene.instantiate(): the scene's nodes outside the tree, with their scripts attached but not ready yet.
uint32_t Runtime::instantiate(std::string_view path, std::string& error) {
    std::vector<NodeDesc> descs;
    std::vector<SceneConnection> connections;
    if (!scene_descs(path, descs, connections)) {
        error = "Failed to instantiate \"" + std::string(path) + "\": no such scene in the package.";
        return 0;
    }

    const size_t first = nodes_.size();
    std::vector<uint32_t> ids;
    for (auto& desc : descs) {
        Live n;
        n.d = std::move(desc);
        n.d.id = next_instance_id_++;
        n.parent = n.d.parent >= 0 ? int32_t(first) + n.d.parent : -1;
        n.in_tree = false;
        const int32_t index = int32_t(nodes_.size());
        if (n.parent >= 0) nodes_[size_t(n.parent)].children.push_back(index);
        index_[n.d.id] = index;
        ids.push_back(n.d.id);
        nodes_.push_back(std::move(n));
        create_visual(index);
    }

    // Scripts once the whole subtree exists, since their initializers may look for children.
    for (uint32_t id : ids) attach_script(id);
    connect_scene(connections, ids);
    flush_script_errors();
    return ids.front();
}

// Node.add_child(): the child (with its subtree) joins the parent; if the parent is in the tree, it enters it too.
bool Runtime::add_child(int32_t parent, int32_t child, std::string& error) {
    Live& c = nodes_[size_t(child)];
    Live& p = nodes_[size_t(parent)];
    if (c.parent >= 0) {
        error = "Can't add child \"" + c.d.name + "\" to \"" + p.d.name + "\", already has a parent \"" +
                nodes_[size_t(c.parent)].d.name + "\".";
        return false;
    }
    if (c.in_tree || c.d.id >= AutoloadIdBase || child == 0) {
        error = "Can't add child \"" + c.d.name + "\": it is a scene root or an autoload.";
        return false;
    }
    for (int32_t i = parent; i >= 0; i = nodes_[size_t(i)].parent)
        if (i == child) {
            error = "Can't add child \"" + c.d.name + "\" to \"" + p.d.name + "\": it is one of its ancestors.";
            return false;
        }

    // Siblings need distinct names (paths go by name): Bullet, Bullet2, Bullet3...
    const std::string base = c.d.name;
    for (int n = 2;; ++n) {
        const bool taken = std::any_of(p.children.begin(), p.children.end(),
                                       [&](int32_t s) { return nodes_[size_t(s)].d.name == c.d.name; });
        if (!taken) break;
        c.d.name = base + std::to_string(n);
    }

    c.parent = parent;
    p.children.push_back(child);
    if (child < parent) reorder_needed_ = true;
    if (p.in_tree) enter_tree(child);
    return true;
}

// Node.remove_child(): the child stays alive outside the tree, as in Godot; add_child() can put it back.
bool Runtime::remove_child(int32_t parent, int32_t child, std::string& error) {
    Live& p = nodes_[size_t(parent)];
    const auto found = std::find(p.children.begin(), p.children.end(), child);
    if (found == p.children.end()) {
        error = "Cannot remove child \"" + nodes_[size_t(child)].d.name + "\" as it's not a child of \"" + p.d.name + "\".";
        return false;
    }
    p.children.erase(found);
    nodes_[size_t(child)].parent = -1;

    std::vector<int32_t> stack{child};
    while (!stack.empty()) {
        Live& n = nodes_[size_t(stack.back())];
        stack.pop_back();
        n.in_tree = false;
        stack.insert(stack.end(), n.children.begin(), n.children.end());
    }
    collect_physics();
    return true;
}

// A subtree joins the scene: particles, the initial velocity of rigid bodies, physics, then _ready() with
// children before parents, as in Godot.
void Runtime::enter_tree(int32_t root) {
    std::vector<uint32_t> ready;
    std::vector<std::pair<int32_t, size_t>> stack{{root, 0}};
    while (!stack.empty()) {
        auto& [index, child] = stack.back();
        Live& n = nodes_[size_t(index)];
        if (child == 0) {
            if (!n.in_tree && physics::is_rigid(n.d.type)) {
                n.edit_motion().velocity = n.d.more().linear_velocity;
                n.edit_motion().angular = n.d.more().angular_velocity;
            }
            n.in_tree = true;
            if (playing_ && n.d.type == NodeType::CPUParticles3D && n.particles < 0) start_particle(n);
            if (playing_ && n.d.type == NodeType::AnimatedSprite2D && n.d.more().autoplay && !n.d.more().playing) play_animation(n, {});
            if (playing_ && n.d.type == NodeType::Timer && n.d.more().autoplay && !n.d.more().playing) start_timer(n);
            if (playing_ && n.d.type == NodeType::AnimationPlayer && !n.d.more().animation.empty() && !n.d.more().playing)
                play_player(n, n.d.more().animation, 1, false);
        }
        if (child < n.children.size()) {
            const int32_t next = n.children[child++];
            stack.push_back({next, 0});
            continue;
        }
        if (!n.freed && vm_->has_instance(n.d.id)) ready.push_back(n.d.id);
        stack.pop_back();
    }
    collect_physics();

    // _ready() may add nodes, which can move nodes_: go by id.
    for (uint32_t id : ready) {
        const int i = index_of(id);
        if (i >= 0 && !nodes_[size_t(i)].freed) vm_->ready(id);
    }
    flush_script_errors();
}

// "idle 0-3 8, run 4-9 12, die 10-14 10 once": name, first-last frame (or one frame), frames per second, "once".
bool parse_animations(std::string_view text, int frames, std::vector<SpriteAnimation>& out) {
    out.clear();
    size_t start = 0;
    while (start <= text.size()) {
        size_t end = text.find_first_of(",\n", start);
        if (end == std::string_view::npos) end = text.size();
        std::vector<std::string_view> words;
        for (size_t i = start; i < end;) {
            while (i < end && text[i] == ' ') ++i;
            size_t j = i;
            while (j < end && text[j] != ' ') ++j;
            if (j > i) words.push_back(text.substr(i, j - i));
            i = j;
        }
        start = end + 1;
        if (words.empty()) continue;
        if (words.size() < 2 || words.size() > 4 || out.size() >= 64) return false;

        SpriteAnimation a;
        a.name = std::string(words[0]);
        const std::string range(words[1]);
        char* rest = nullptr;
        a.first = int(std::strtol(range.c_str(), &rest, 10));
        a.last = *rest == '-' ? int(std::strtol(rest + 1, &rest, 10)) : a.first;
        if (*rest || a.first < 0 || a.last < a.first || a.last >= frames) return false;
        if (words.size() >= 3) {
            const std::string fps(words[2]);
            a.fps = std::strtof(fps.c_str(), &rest);
            if (*rest || !std::isfinite(a.fps) || a.fps <= 0 || a.fps > 240) return false;
        }
        if (words.size() == 4) {
            if (words[3] != "once") return false;
            a.loop = false;
        }
        out.push_back(std::move(a));
    }
    return true;
}

// AnimatedSprite2D.play(): an empty name plays the current animation, or the first one.
bool Runtime::play_animation(Live& n, std::string_view name) {
    auto& e = n.d.extras();
    std::vector<SpriteAnimation> animations;
    parse_animations(e.animations, n.d.hframes * n.d.vframes, animations);
    if (name.empty()) name = !e.animation.empty() ? std::string_view(e.animation) : std::string_view();
    const auto found = std::find_if(animations.begin(), animations.end(), [&](const auto& a) { return name.empty() || a.name == name; });
    if (found == animations.end()) return false;
    if (e.animation != found->name || !e.playing) {
        e.animation = found->name;
        e.anim_time = 0;
        n.d.frame = uint16_t(found->first);
    }
    e.anim_first = int16_t(found->first);
    e.anim_last = int16_t(found->last);
    e.anim_fps = found->fps;
    e.anim_loop = found->loop;
    e.playing = true;
    return true;
}

// Advances the playing AnimatedSprite2Ds and Timers. A "once" animation stops on its last frame.
void Runtime::update_animations(float dt) {
    for (auto& n : nodes_) {
        if ((n.d.type != NodeType::AnimatedSprite2D && n.d.type != NodeType::Timer && n.d.type != NodeType::AnimationPlayer)
            || n.freed || !n.in_tree || !n.d.more().playing)
            continue;
        if (n.d.type == NodeType::AnimationPlayer) {
            advance_player(n, dt);
            continue;
        }
        auto& e = n.d.extras();

        // Godot's Timer: at most one timeout per frame, a repeating one keeps the time it overran by.
        if (n.d.type == NodeType::Timer) {
            if (e.paused) continue;
            n.cycle -= dt;
            if (n.cycle >= 0) continue;
            if (e.one_shot) {
                e.playing = false;
                n.cycle = 0;
            } else {
                n.cycle += e.lifetime;
            }
            defer_signal(n.d.id, "timeout");
            continue;
        }

        e.anim_time += dt * e.speed_scale;
        const int count = e.anim_last - e.anim_first + 1;
        int step = int(std::floor(e.anim_time * e.anim_fps));
        if (e.anim_loop) {
            step %= count;
            if (step < 0) step += count;
        } else if (step >= count || step < 0) {
            step = step < 0 ? 0 : count - 1;
            e.playing = false;
            defer_signal(n.d.id, "animation_finished");
        }
        n.d.frame = uint16_t(e.anim_first + step);
    }
    run_deferred();
}

void Runtime::start_timer(Live& n, float wait_time) {
    auto& e = n.d.extras();
    if (wait_time > 0) e.lifetime = wait_time;
    e.playing = true;
    n.cycle = e.lifetime;
}

// a signal nobody listens to isn't even queued
void Runtime::defer_signal(uint32_t source, const char* signal, std::vector<jetscript::Value> args) {
    if (vm_->has_connections(source)) deferred_.push_back({source, signal, {}, std::move(args)});
}

void Runtime::run_deferred() {
    if (deferred_.empty()) return;
    std::vector<Deferred> pending;
    pending.swap(deferred_);
    for (const auto& d : pending) {
        const jetscript::Value* args = d.args.empty() ? nullptr : d.args.data();
        if (d.signal) vm_->emit_signal(d.source, d.signal, args, int(d.args.size()));
        else vm_->call_callable(d.callable, args, int(d.args.size()));
    }
    flush_script_errors();
}

bool Runtime::change_scene(std::string_view path) {
    bool found = path == description_.path;
    for (const auto& scene : description_.scenes) found = found || scene.path == path;
    if (!found || !playing_) return false;
    pending_scene_ = std::string(path);
    return true;
}

void Runtime::switch_scene() {
    const std::string path = std::move(pending_scene_);
    pending_scene_.clear();
    std::vector<SceneConnection> connections;
    const std::vector<NodeDesc>* target = nodes_of(path, &connections);
    if (!target) return;
    for (const auto& n : nodes_) {
        if (n.d.id >= AutoloadIdBase) continue;
        if (vm_->has_instance(n.d.id)) vm_->detach(n.d.id);
        vm_->forget(n.d.id); // the next scene's nodes may get the same ids
    }
    kill_tweens(true);
    flush_script_errors();
    scene_nodes_ = target;
    scene_connections_ = std::move(connections);
    current_scene_ = path;
    std::string error;
    build(error);
    enter_scene();
}

void Runtime::stop() {
    if (!playing_) return;
    playing_ = false;
    vm_->clear();
    kill_tweens(false);
    flush_script_errors();
    scene_nodes_ = nodes_of(description_.path);
    current_scene_ = description_.path;
    pending_scene_.clear();
    compiled_.clear();
    std::string error;
    build(error);
    editor_view_ = true;
}

// queue_free() is deferred to the end of the frame, as in Godot. The freed nodes, their subtrees included,
// release what they hold and leave nodes_, so spawning and freeing doesn't grow it forever.
void Runtime::free_pending() {
    if (!pending_free_ && !reorder_needed_) return;
    if (pending_free_) {
        pending_free_ = false;
        std::vector<int32_t> stack;
        for (size_t i = 0; i < nodes_.size(); ++i)
            if (nodes_[i].free_requested) stack.push_back(int32_t(i));
        while (!stack.empty()) {
            Live& n = nodes_[size_t(stack.back())];
            stack.pop_back();
            if (n.freed) continue;
            n.freed = true;
            stack.insert(stack.end(), n.children.begin(), n.children.end());
        }
        for (auto& n : nodes_)
            if (n.freed) release_node(n);
    }
    compact_nodes();
}

// Drops the freed nodes and keeps parents before children (update_transforms() needs it), then fixes every index.
void Runtime::compact_nodes() {
    std::vector<int32_t> order;
    order.reserve(nodes_.size());
    if (reorder_needed_) {
        // A node went under one that comes later: walk the trees again, roots in their current order.
        std::vector<int32_t> stack;
        for (size_t i = nodes_.size(); i-- > 0;)
            if (nodes_[i].parent < 0) stack.push_back(int32_t(i));
        while (!stack.empty()) {
            const int32_t i = stack.back();
            stack.pop_back();
            if (nodes_[size_t(i)].freed) continue;
            order.push_back(i);
            const auto& children = nodes_[size_t(i)].children;
            stack.insert(stack.end(), children.rbegin(), children.rend());
        }
    } else {
        for (size_t i = 0; i < nodes_.size(); ++i)
            if (!nodes_[i].freed) order.push_back(int32_t(i));
        if (order.size() == nodes_.size()) return;
    }

    std::vector<int32_t> moved_to(nodes_.size(), -1);
    for (size_t k = 0; k < order.size(); ++k) moved_to[size_t(order[k])] = int32_t(k);
    if (reorder_needed_) {
        // In place, one cycle of the permutation at a time: a second array would need as much RAM again.
        // source[k] is the node that goes to slot k; the freed ones go last.
        std::vector<int32_t> source(order);
        for (size_t i = 0; i < nodes_.size(); ++i)
            if (moved_to[i] < 0) source.push_back(int32_t(i));
        std::vector<bool> placed(source.size(), false);
        for (size_t k = 0; k < source.size(); ++k) {
            if (placed[k]) continue;
            placed[k] = true;
            if (size_t(source[k]) == k) continue;
            Live held = std::move(nodes_[k]);
            size_t j = k;
            while (size_t(source[j]) != k) {
                nodes_[j] = std::move(nodes_[size_t(source[j])]);
                j = size_t(source[j]);
                placed[j] = true;
            }
            nodes_[j] = std::move(held);
        }
        nodes_.resize(order.size());
    } else {
        // Same order minus the freed: in place, no second array.
        for (size_t k = 0; k < order.size(); ++k)
            if (size_t(order[k]) != k) nodes_[k] = std::move(nodes_[size_t(order[k])]);
        nodes_.resize(order.size());
    }
    reorder_needed_ = false;

    index_.clear();
    for (size_t k = 0; k < nodes_.size(); ++k) {
        Live& n = nodes_[k];
        index_[n.d.id] = int32_t(k);
        n.parent = n.parent >= 0 ? moved_to[size_t(n.parent)] : -1;
        n.children.clear();
        if (n.motion_.allocated()) n.edit_motion().floor_body = -1; // found again at the next physics tick
    }
    for (size_t k = 0; k < nodes_.size(); ++k)
        if (nodes_[k].parent >= 0) nodes_[size_t(nodes_[k].parent)].children.push_back(int32_t(k));
    collect_physics();
}

void Runtime::update(float delta) {
    const float dt = std::isfinite(delta) ? std::clamp(delta, 0.0f, 0.1f) : 0.0f;
    if (playing_) {
        scene_.waterTime += dt;
        // Fixed 60 Hz physics ticks (at most 4 per frame), before _process as in Godot.
        physics_time_ += dt;
        int ticks = 0;
        while (physics_time_ >= physics::Step && ticks < 4) {
            physics_tick(physics::Step);
            physics_time_ -= physics::Step;
            ++ticks;
        }
        if (ticks == 4) physics_time_ = 0;
        free_pending();
        for (size_t i = 0; i < nodes_.size(); ++i) {
            const Live& n = nodes_[i];
            if (n.freed || !n.in_tree || n.d.script < 0) continue;
            const uint32_t id = n.d.id;
            if (vm_->has_instance(id)) vm_->process(id, dt);
        }
        flush_script_errors();
        update_tweens(dt);
        free_pending();
        update_animations(dt);
        previous_input_ = input_;
        if (!pending_scene_.empty()) switch_scene();
    }
    update_transforms();
    if (playing_) update_particles(dt); // emitters where the scripts left them
    apply_environment();
    apply_camera();
}

namespace {
uint16_t lerp565(uint32_t a, uint32_t b, float t) {
    auto channel = [&](int shift) {
        const float x = float((a >> shift) & 255), y = float((b >> shift) & 255);
        return uint32_t(std::lround(x + (y - x) * t));
    };
    return rgb565(channel(16) << 16 | channel(8) << 8 | channel(0));
}
} // namespace

// Jet clears each row with gradient_ (water reflections use it too). A panorama is drawn here and Jet's clear is skipped.
void Runtime::prepare_background() {
    const Live* env = nullptr;
    for (const auto& n : nodes_) if (!n.freed && n.in_tree && n.d.type == NodeType::WorldEnvironment) { env = &n; break; }
    gradient_.resize(size_t(height_));
    const Vec3 forward = -view_basis_.column(2);
    const float focal = (height_ * 0.5f) / std::tan(radians(view_fov_) * 0.5f);
    const float pitch = std::asin(std::clamp(forward.y, -1.0f, 1.0f));
    const Background mode = env ? env->d.more().background_mode : Background::Color;
    if (mode == Background::Color) {
        std::fill(gradient_.begin(), gradient_.end(), rgb565(env ? env->d.more().background : 0x161c28));
    } else {
        // Horizon row from the camera pitch; colours follow the angle from it.
        const float horizon = height_ * 0.5f + focal * std::tan(pitch);
        const auto& e = env->d;
        for (int y = 0; y < height_; ++y) {
            const float angle = std::atan((horizon - float(y)) / focal) / (Pi * 0.5f);
            if (angle >= 0) {
                const float t = 1 - (1 - std::min(angle, 1.0f)) * (1 - std::min(angle, 1.0f));
                gradient_[size_t(y)] = lerp565(e.more().sky_horizon, e.more().sky_top, t);
            } else {
                const float t = std::min(-angle * 4, 1.0f);
                gradient_[size_t(y)] = lerp565(e.more().ground_horizon, e.more().ground_bottom, t);
            }
        }
    }
    scene_.backgroundGradientColors = gradient_.data();
    const bool panorama = mode == Background::Panorama && env->d.more().sky_texture >= 0;
    scene_.setClearBuffer(!panorama);
    if (!panorama) return;
    // Separable: yaw depends only on the column, pitch only on the row.
    const auto& sky = description_.textures[size_t(env->d.more().sky_texture)];
    const float yaw = std::atan2(forward.x, -forward.z);
    sky_columns_.resize(size_t(width_));
    sky_rows_.resize(size_t(height_));
    for (int x = 0; x < width_; ++x) {
        float u = (yaw + std::atan((x - width_ * 0.5f) / focal)) / (2 * Pi) + 0.5f;
        u -= std::floor(u);
        sky_columns_[size_t(x)] = std::min(int32_t(u * sky.width), int32_t(sky.width) - 1);
    }
    for (int y = 0; y < height_; ++y) {
        const float elevation = pitch + std::atan((height_ * 0.5f - y) / focal);
        const float v = std::clamp(0.5f - elevation / Pi, 0.0f, 0.9999f);
        sky_rows_[size_t(y)] = int32_t(v * sky.height) * sky.width;
    }
    const bool indexed = !sky.palette.empty();
    const uint8_t* indices = sky.index_data();
    // Only the rows and columns this build stores (see buffer_width()).
    uint16_t* out = target_;
    for (int y = FIELD_BUFFERS ? next_field() : 0; y < height_; y += BufferRowStep) {
        const int32_t row = sky_rows_[size_t(y)];
        if (indexed)
            for (int x = 0; x < width_; x += BufferColumnStep) *out++ = sky.palette[indices[size_t(row + sky_columns_[size_t(x)])]];
        else
            for (int x = 0; x < width_; x += BufferColumnStep) *out++ = sky.pixels[size_t(row + sky_columns_[size_t(x)])];
    }
    std::fill(depth_.begin(), depth_.end(), uint16_t(0xFFFF));
}

// Reflection UVs depend on the view and the world normals, so they're recomputed every frame.
void Runtime::update_reflections() {
    for (auto& n : nodes_) {
        if (!n.object || !n.texture || !n.d.reflection || !n.world_visible) continue;
        const auto& r = n.object->rotation;
        const Basis rotation = Basis::rotation_z(radians(float(r.z))) * Basis::rotation_y(radians(float(r.y))) * Basis::rotation_x(radians(float(r.x)));
        const Vec3 origin{float(n.object->position.x), float(n.object->position.y), float(n.object->position.z)};
        int32_t anchor = 0;
        bool first = true;
        for (auto& v : n.object->vertices) {
            const Vec3 local{float(v.position.x), float(v.position.y), float(v.position.z)};
            const Vec3 normal = rotation.xform({float(v.normal.x), float(v.normal.y), float(v.normal.z)});
            const Vec3 world = rotation.xform(local) + origin;
            const ::Vector3 p{int32_t(std::lround(world.x)), int32_t(std::lround(world.y)), int32_t(std::lround(world.z))};
            const ::Vector3 nn{int32_t(std::lround(normal.x)), int32_t(std::lround(normal.y)), int32_t(std::lround(normal.z))};
            v.uv = Renderer::environmentReflectionUV(p, nn, camera_.position);
            // Keep each surface on one side of the panorama seam.
            if (first) { anchor = v.uv.x; first = false; }
            else v.uv.x = Renderer::unwrapEnvironmentU(v.uv.x, anchor);
        }
    }
}

namespace {
uint16_t blend565(uint16_t dst, uint16_t src, uint32_t alpha) {
    if (alpha >= 255) return src;
    const uint32_t inv = 255 - alpha;
    const uint32_t r = (((src >> 11) & 31) * alpha + ((dst >> 11) & 31) * inv) / 255;
    const uint32_t g = (((src >> 5) & 63) * alpha + ((dst >> 5) & 63) * inv) / 255;
    const uint32_t b = ((src & 31) * alpha + (dst & 31) * inv) / 255;
    return uint16_t(r << 11 | g << 5 | b);
}
// The font covers ASCII and Latin-1; anything else becomes '?'.
int glyph_index(uint32_t code) {
    if (code >= 32 && code <= 126) return int(code - 32);
    if (code >= 160 && code <= 255) return int(code - 160 + 95);
    return '?' - 32;
}
} // namespace

// The 2D nodes as seen by the current Camera2D (its position at the screen centre) or, without one, with the canvas
// origin at the top-left corner, as in Godot. Sorted by z_index, then tree order; the Controls come after them.
// The current Camera2D's view: screen = canvas * zoom + shift.
void Runtime::set_editor_canvas(bool enabled, float zoom, Vec3 origin) {
    editor_canvas_ = enabled;
    editor_canvas_zoom_ = std::isfinite(zoom) ? std::clamp(zoom, 0.05f, 64.0f) : 1;
    editor_canvas_origin_ = origin.finite() ? origin : Vec3{};
}

void Runtime::canvas_view(float& zoom_x, float& zoom_y, float& shift_x, float& shift_y) const {
    zoom_x = zoom_y = 1;
    shift_x = shift_y = 0;
    if (canvas_override()) {
        zoom_x = zoom_y = editor_canvas_zoom_;
        shift_x = -editor_canvas_origin_.x * zoom_x;
        shift_y = -editor_canvas_origin_.y * zoom_y;
        return;
    }
    for (const auto& n : nodes_)
        if (n.d.type == NodeType::Camera2D && !n.freed && n.in_tree && n.d.more().current) {
            zoom_x = n.d.more().zoom[0];
            zoom_y = n.d.more().zoom[1];
            shift_x = width_ * 0.5f - (n.world.origin.x + n.d.offset[0]) * zoom_x;
            shift_y = height_ * 0.5f - (n.world.origin.y + n.d.offset[1]) * zoom_y;
            return;
        }
}

std::vector<float> Runtime::shape_outline(uint32_t id) const {
    std::vector<float> out;
    const int index = index_of(id);
    if (index < 0 || nodes_[size_t(index)].d.type != NodeType::CollisionShape2D || nodes_[size_t(index)].freed) return out;
    const Live& n = nodes_[size_t(index)];
    float zoom_x, zoom_y, shift_x, shift_y;
    canvas_view(zoom_x, zoom_y, shift_x, shift_y);
    auto add = [&](float x, float y) {
        const Vec3 p = n.world.xform({x, y, 0});
        out.push_back(p.x * zoom_x + shift_x);
        out.push_back(p.y * zoom_y + shift_y);
    };

    // A closed polygon: the rectangle's corners, or the lower and upper half of the circle (capsule: apart).
    const NodeExtras& e = n.d.more();
    if (e.collision == CollisionKind::Box) {
        const float w = e.box_size.x * 0.5f, h = e.box_size.y * 0.5f;
        add(-w, -h); add(w, -h); add(w, h); add(-w, h);
        return out;
    }
    constexpr int Arc = 12;
    const float r = e.radius, half = e.collision == CollisionKind::Capsule ? std::max(0.0f, e.height * 0.5f - r) : 0;
    for (int i = 0; i <= Arc; ++i) add(std::cos(Pi * i / Arc) * r, std::sin(Pi * i / Arc) * r + half);
    for (int i = 0; i <= Arc; ++i) add(-std::cos(Pi * i / Arc) * r, -std::sin(Pi * i / Arc) * r - half);
    return out;
}

void Runtime::prepare_canvas() {
    float zoom_x, zoom_y, shift_x, shift_y;
    canvas_view(zoom_x, zoom_y, shift_x, shift_y);
    auto screen = [&](const Live& n, float x, float y) {
        const Vec3 p = n.world.xform({x, y, 0});
        return Vec3{p.x * zoom_x + shift_x, p.y * zoom_y + shift_y, 0};
    };

    std::vector<std::pair<int, int32_t>> order;
    for (size_t i = 0; i < nodes_.size(); ++i) {
        const Live& n = nodes_[i];
        if ((n.d.type == NodeType::Sprite2D || n.d.type == NodeType::AnimatedSprite2D) && n.world_visible && n.d.texture >= 0)
            order.emplace_back(n.canvas_z, int32_t(i));
    }
    std::stable_sort(order.begin(), order.end(), [](const auto& a, const auto& b) { return a.first < b.first; });

    for (const auto& [z, index] : order) {
        const Live& n = nodes_[size_t(index)];
        const NodeDesc& d = n.d;
        // Opacity comes down from the 2D parents, like Godot's modulate.
        float alpha = std::clamp(d.alpha, 0.0f, 1.0f);
        for (int p = n.parent; p >= 0 && is_node2d(nodes_[p].d.type); p = nodes_[p].parent) alpha *= std::clamp(nodes_[p].d.alpha, 0.0f, 1.0f);
        const auto& t = description_.textures[size_t(d.texture)];
        const int fw = t.width / d.hframes, fh = t.height / d.vframes;
        if (alpha <= 0 || fw <= 0 || fh <= 0) continue;

        OverlayItem item;
        item.type = NodeType::Sprite2D;
        item.id = d.id;
        item.texture = &t;
        item.tint = d.color;
        item.alpha = uint8_t(std::lround(alpha * 255));
        const int frame = std::min(int(d.frame), d.hframes * d.vframes - 1);
        item.frame_x = (frame % d.hframes) * fw;
        item.frame_y = (frame / d.hframes) * fh;
        item.frame_w = fw;
        item.frame_h = fh;
        item.flip_h = d.flip_h;
        item.flip_v = d.flip_v;

        // The frame's corners on the screen: a = (0, 0), b = (fw, 0), c = (0, fh).
        const float left = (d.centered ? -fw * 0.5f : 0) + d.offset[0], top = (d.centered ? -fh * 0.5f : 0) + d.offset[1];
        const Vec3 a = screen(n, left, top), b = screen(n, left + fw, top), c = screen(n, left, top + fh);
        const Vec3 across = b - a, down = c - a;
        if (std::abs(across.y) < 1e-3f && std::abs(down.x) < 1e-3f) {
            // Not rotated: a rectangle, flipped by negative scales.
            if (across.x < 0) item.flip_h = !item.flip_h;
            if (down.y < 0) item.flip_v = !item.flip_v;
            item.x = int(std::lround(std::min(a.x, b.x)));
            item.y = int(std::lround(std::min(a.y, c.y)));
            item.w = int(std::lround(std::max(a.x, b.x))) - item.x;
            item.h = int(std::lround(std::max(a.y, c.y))) - item.y;
        } else {
            // Rotated or skewed: each screen pixel finds its texel through the inverse map.
            const float det = across.x * down.y - down.x * across.y;
            if (std::abs(det) < 1e-6f) continue;
            const float m[4] = {down.y * fw / det, -down.x * fw / det, -across.y * fh / det, across.x * fh / det};
            item.ux = int32_t(std::lround(m[0] * 65536));
            item.uy = int32_t(std::lround(m[1] * 65536));
            item.vx = int32_t(std::lround(m[2] * 65536));
            item.vy = int32_t(std::lround(m[3] * 65536));
            // At pixel centres: u = m0 * (x + 0.5 - a.x) + m1 * (y + 0.5 - a.y).
            item.u0 = int64_t(std::llround((m[0] * (0.5f - a.x) + m[1] * (0.5f - a.y)) * 65536));
            item.v0 = int64_t(std::llround((m[2] * (0.5f - a.x) + m[3] * (0.5f - a.y)) * 65536));
            item.affine = true;
            const Vec3 e = b + down;
            const float x0 = std::min({a.x, b.x, c.x, e.x}), x1 = std::max({a.x, b.x, c.x, e.x});
            const float y0 = std::min({a.y, b.y, c.y, e.y}), y1 = std::max({a.y, b.y, c.y, e.y});
            item.x = int(std::floor(x0));
            item.y = int(std::floor(y0));
            item.w = int(std::ceil(x1)) - item.x;
            item.h = int(std::ceil(y1)) - item.y;
        }
        if (item.w <= 0 || item.h <= 0 || item.x >= width_ || item.y >= height_ || item.x + item.w <= 0 || item.y + item.h <= 0) continue;
        overlay_.push_back(std::move(item));
    }
}

void Runtime::prepare_overlay() {
    overlay_.clear();
    prepare_canvas();
    for (const auto& n : nodes_) {
        if (!is_control(n.d.type) || n.d.type == NodeType::Control || !n.world_visible) continue;
        // Opacity is inherited from the parent Controls, like Godot's modulate.
        float alpha = std::clamp(n.d.alpha, 0.0f, 1.0f);
        for (int p = n.parent; p >= 0 && is_control(nodes_[p].d.type); p = nodes_[p].parent) alpha *= std::clamp(nodes_[p].d.alpha, 0.0f, 1.0f);
        if (alpha <= 0) continue;
        const auto& d = n.d;
        OverlayItem item;
        item.type = d.type;
        item.id = d.id;
        // The editor's 2D view moves and zooms them with the canvas (text and images by whole pixels).
        const float zoom = canvas_override() ? editor_canvas_zoom_ : 1;
        const Vec3 at = canvas_override() ? (n.world.origin - editor_canvas_origin_) * zoom : n.world.origin;
        item.x = int(std::lround(at.x));
        item.y = int(std::lround(at.y));
        item.color = rgb565(d.color);
        item.tint = d.color;
        item.alpha = uint8_t(std::lround(alpha * 255));
        item.scale = uint8_t(std::max<int>(1, d.more().text_scale));
        const float scale = item.scale * zoom;
        if (d.type == NodeType::ColorRect) {
            item.w = int(std::lround(d.more().size.x * zoom));
            item.h = int(std::lround(d.more().size.y * zoom));
        } else if (d.type == NodeType::TextureRect) {
            if (d.texture < 0) continue;
            item.texture = &description_.textures[size_t(d.texture)];
            item.w = int(std::lround(d.more().size.x > 0 ? d.more().size.x * zoom : item.texture->width * scale));
            item.h = int(std::lround(d.more().size.y > 0 ? d.more().size.y * zoom : item.texture->height * scale));
        } else {
            // UTF-8 to glyphs, one list per line.
            item.lines.emplace_back();
            const std::string& text = d.more().text;
            size_t longest = 0;
            for (size_t i = 0; i < text.size();) {
                uint32_t code = uint8_t(text[i++]);
                if (code >= 0xC0 && code < 0xE0 && i < text.size()) code = ((code & 0x1F) << 6) | (uint8_t(text[i++]) & 0x3F);
                else if (code >= 0xE0) { while (i < text.size() && (uint8_t(text[i]) & 0xC0) == 0x80) ++i; code = '?'; }
                if (code == '\n') { item.lines.emplace_back(); continue; }
                item.lines.back().push_back(uint8_t(glyph_index(code)));
                longest = std::max(longest, item.lines.back().size());
            }
            item.columns = int(longest);
            item.w = int(std::lround(float(longest * font::Width) * scale));
            item.h = int(std::lround(float(item.lines.size() * font::Height) * scale));
            item.stretched = zoom != 1;
        }
        if (item.w <= 0 || item.h <= 0 || item.x >= width_ || item.y >= height_ || item.x + item.w <= 0 || item.y + item.h <= 0) continue;
        overlay_.push_back(std::move(item));
    }
}

void compose_overlay_row(const std::vector<OverlayItem>& items, uint16_t* row, int y, int width, bool swapped) {
    auto put = [&](int x, uint16_t c, uint32_t alpha) {
        uint16_t dst = row[x];
        if (swapped) dst = uint16_t(dst << 8 | dst >> 8);
        dst = blend565(dst, c, alpha);
        row[x] = swapped ? uint16_t(dst << 8 | dst >> 8) : dst;
    };
    for (const auto& it : items) {
        if (y < it.y || y >= it.y + it.h) continue;
        const int x0 = std::max(0, it.x), x1 = std::min(width, it.x + it.w);
        if (it.type == NodeType::ColorRect) {
            for (int x = x0; x < x1; ++x) put(x, it.color, it.alpha);
        } else if (it.type == NodeType::Sprite2D) {
            const auto& t = *it.texture;
            const uint8_t* indices = t.palette.empty() ? nullptr : t.index_data();
            auto draw = [&](int x, int u, int v) {
                if (it.flip_h) u = it.frame_w - 1 - u;
                if (it.flip_v) v = it.frame_h - 1 - v;
                const size_t i = size_t(it.frame_y + v) * t.width + size_t(it.frame_x + u);
                uint16_t c = indices ? t.palette[indices[i]] : t.pixels[i];
                if (t.has_alpha && c == t.alpha_color) return;
                if (it.tint != 0xffffff) c = tint565(c, it.tint);
                put(x, c, it.alpha);
            };
            if (!it.affine) {
                const int v = (y - it.y) * it.frame_h / it.h;
                for (int x = x0; x < x1; ++x) draw(x, (x - it.x) * it.frame_w / it.w, v);
            } else {
                int64_t u = int64_t(it.ux) * x0 + int64_t(it.uy) * y + it.u0;
                int64_t v = int64_t(it.vx) * x0 + int64_t(it.vy) * y + it.v0;
                const int64_t uw = int64_t(it.frame_w) << 16, vh = int64_t(it.frame_h) << 16;
                for (int x = x0; x < x1; ++x, u += it.ux, v += it.vx)
                    if (u >= 0 && u < uw && v >= 0 && v < vh) draw(x, int(u >> 16), int(v >> 16));
            }
        } else if (it.type == NodeType::TextureRect) {
            // Nearest sampling, modulate tints.
            const auto& t = *it.texture;
            const bool indexed = !t.palette.empty();
            const size_t ty = size_t((y - it.y) * t.height / it.h) * t.width;
            for (int x = x0; x < x1; ++x) {
                const size_t i = ty + size_t((x - it.x) * t.width / it.w);
                uint16_t c = indexed ? t.palette[t.index_data()[i]] : t.pixels[i];
                if (t.has_alpha && c == t.alpha_color) continue;
                if (it.tint != 0xffffff) c = tint565(c, it.tint);
                put(x, c, it.alpha);
            }
        } else if (it.stretched) {
            // The editor's zoomed 2D view: each pixel takes its glyph pixel, so text keeps its size at any zoom.
            const int columns = it.columns * font::Width, rows = int(it.lines.size()) * font::Height;
            const int gy_all = (y - it.y) * rows / it.h;
            const auto& glyphs = it.lines[size_t(gy_all / font::Height)];
            const int gy = gy_all % font::Height;
            for (int x = x0; x < x1; ++x) {
                const int gx_all = (x - it.x) * columns / it.w;
                const size_t g = size_t(gx_all / font::Width);
                if (g < glyphs.size() && (font::Glyphs[glyphs[g]][gy] & (0x40 >> (gx_all % font::Width)))) put(x, it.color, it.alpha);
            }
        } else {
            const int cell = font::Height * it.scale;
            const size_t line = size_t((y - it.y) / cell);
            if (line >= it.lines.size()) continue;
            const int gy = ((y - it.y) % cell) / it.scale;
            int cx = it.x;
            for (uint8_t glyph : it.lines[line]) {
                const uint8_t bits = font::Glyphs[glyph][gy];
                for (int gx = 0; bits && gx < font::Width; ++gx) {
                    if (!(bits & (0x40 >> gx))) continue;
                    for (int sx = 0; sx < it.scale; ++sx) {
                        const int x = cx + gx * it.scale + sx;
                        if (x >= 0 && x < width) put(x, it.color, it.alpha);
                    }
                }
                cx += font::Width * it.scale;
            }
        }
    }
}

// With half-width fields the device composites the overlay while sending each row instead.
void Runtime::draw_overlay() {
    prepare_overlay();
#if !HALF_WIDTH_BUFFERS && !FIELD_BUFFERS
    int top = height_, bottom = 0;
    for (const auto& it : overlay_) {
        top = std::min(top, std::max(0, it.y));
        bottom = std::max(bottom, std::min(height_, it.y + it.h));
    }
    for (int y = top; y < bottom; ++y) compose_overlay_row(overlay_, target_ + size_t(y) * width_, y, width_, false);
#endif
}

int Runtime::pick_overlay(int x, int y) const {
    // The 2D view moved and zoomed them: as drawn, topmost first.
    if (canvas_override()) {
        for (auto it = overlay_.rbegin(); it != overlay_.rend(); ++it)
            if (x >= it->x && y >= it->y && x < it->x + it->w && y < it->y + it->h) return int(it->id);
        return -1;
    }
    for (size_t i = nodes_.size(); i-- > 0;) {
        const Live* it = &nodes_[i];
        const auto& d = it->d;
        if (!is_control(d.type) || d.type == NodeType::Control || !it->world_visible) continue;
        int w = int(d.more().size.x), h = int(d.more().size.y);
        if (d.type == NodeType::Label) {
            int lines = 1, longest = 0, current = 0;
            for (char ch : d.more().text) {
                if (ch == '\n') { ++lines; current = 0; continue; }
                if ((uint8_t(ch) & 0xC0) != 0x80) longest = std::max(longest, ++current);
            }
            w = longest * font::Width * d.more().text_scale;
            h = lines * font::Height * d.more().text_scale;
        } else if (d.type == NodeType::TextureRect && d.texture >= 0) {
            const auto& t = description_.textures[size_t(d.texture)];
            if (w <= 0) w = t.width * d.more().text_scale;
            if (h <= 0) h = t.height * d.more().text_scale;
        }
        const Vec3& p = it->world.origin;
        if (x >= p.x && y >= p.y && x < p.x + w && y < p.y + h) return int(d.id);
    }
    // Then the 2D sprites as last drawn, topmost first (rotated ones by their bounds).
    for (auto it = overlay_.rbegin(); it != overlay_.rend(); ++it)
        if (it->type == NodeType::Sprite2D && x >= it->x && y >= it->y && x < it->x + it->w && y < it->y + it->h) return int(it->id);
    return -1;
}

size_t MemoryReport::largest_scene() const {
    size_t most = 0;
    for (const auto& s : scenes) most = std::max(most, s.total());
    return most;
}

namespace {
// 32-bit sizes (arm-none-eabi, firmware config) plus newlib's 8 bytes per allocation.
constexpr size_t Block = 8;
constexpr size_t LiveBytes = 504, ExtrasBytes = 364 + Block, MotionBytes = 96 + Block, DescBytes = 184;
constexpr size_t ObjectBytes = 188 + Block, MaterialBytes = 32 + Block, TextureBytes = 44 + Block, InstanceBytes = 68 + Block;
constexpr size_t VertexBytes = 36, TriangleBytes = 16, QueueBytesPerTriangle = 129;
constexpr size_t ParticleBytes = 40, EmitterBytes = 16 + Block, DrawerBytes = MaxParticles * 36 + 12 + Block;
constexpr size_t OverlayBytes = 104, AssetBytes = 72;
// The runtime, Jet's scene and rasteriser state and the VM before any content (fitted to a Nucleo-F746ZG's free heap).
constexpr size_t EngineBytes = 26 * 1024;
} // namespace

MemoryReport Runtime::estimate_memory(bool textures_in_place) const {
    MemoryReport report;
    report.engine = EngineBytes;
    // The device draws half-width interlaced fields, whatever this build draws.
    report.field = size_t(width_ / 2) * size_t(height_ / 2) * 2;
    report.frames = report.field + 2 * size_t(width_) * 2;
    for (const auto& t : description_.textures) {
        const size_t texels = size_t(t.width) * t.height;
        report.textures += AssetBytes + t.palette.size() * 2 + (t.palette.empty() ? texels * 2 : textures_in_place ? 0 : texels);
    }

    // Compiled scripts: about twice their bytecode; instances count with their nodes.
    std::vector<size_t> instance_bytes(description_.scripts.size(), 64);
#ifndef JETSCRIPT_NO_COMPILER
    for (size_t i = 0; i < description_.scripts.size(); ++i) {
        const auto& file = description_.scripts[i];
        const auto result = file.bytecode.empty() ? jetscript::compile(file.source, file.path, *host_)
                                                  : jetscript::load_bytecode(file.bytecode.data(), file.bytecode.size(), *host_);
        if (!result.script) continue;
        report.scripts += jetscript::save_bytecode(*result.script).size() * 2 + 256;
        instance_bytes[i] += result.script->members().size() * 16;
    }
#endif

    auto scene = [&](const std::string& path, const std::vector<NodeDesc>& nodes) {
        MemoryEstimate e;
        e.scene = path;
        e.loading = nodes.size() * DescBytes;
        size_t triangles = 0;
        bool drawer = false; // Jet's pool that draws the streaks, once
        std::set<std::array<int32_t, 5>> shared;
        for (const auto& n : nodes) {
            e.nodes += LiveBytes;
            if (uses_extras(n.type)) e.nodes += ExtrasBytes + n.more().text.size() + n.more().animations.size();
            if (physics::is_collision_object(n.type)) e.nodes += MotionBytes;
            if (n.script >= 0 && size_t(n.script) < instance_bytes.size()) e.nodes += instance_bytes[size_t(n.script)];
            if (is_control(n.type) || n.type == NodeType::Sprite2D || n.type == NodeType::AnimatedSprite2D) e.nodes += OverlayBytes;
            if (n.type == NodeType::CPUParticles3D) {
                e.particles += EmitterBytes + size_t(n.more().amount) * ParticleBytes;
                drawer = drawer || n.more().particle_style != ParticleStyle::Puff;
            }
            const bool mesh = n.type == NodeType::Sprite3D || (n.type == NodeType::MeshInstance3D && !(n.shape == Shape::Mesh && n.mesh < 0));
            if (!mesh) continue;

            // One Jet object per node; identical meshes (shape, detail, model and size) share their geometry.
            NodeDesc d = n;
            if (d.type == NodeType::Sprite3D) d.shape = Shape::Quad;
            std::unique_ptr<Renderer::Object> geometry(make_mesh(d, nullptr));
            const size_t tris = geometry ? geometry->triangles.size() : 0;
            triangles += tris;
            e.meshes += ObjectBytes + MaterialBytes + InstanceBytes + (n.texture >= 0 ? TextureBytes : 0);
            // Reflective meshes keep geometry of their own.
            const std::array<int32_t, 5> key{int32_t(d.shape), d.segments, d.mesh, int32_t(std::lround(n.scale.x * 100)),
                                              int32_t(std::lround(n.scale.y * 100 + n.scale.z * 10000))};
            if (geometry && (d.reflection || shared.insert(key).second))
                e.meshes += ObjectBytes + geometry->vertices.size() * VertexBytes + tris * TriangleBytes + 2 * Block;
        }
        e.triangles = (triangles / 2 + 32) * QueueBytesPerTriangle;
        if (drawer) e.particles += DrawerBytes;
        for (const auto& a : description_.autoloads) e.nodes += LiveBytes + (a.script >= 0 ? instance_bytes[size_t(a.script)] : 0);
        const size_t count = nodes.size() + description_.autoloads.size(); // in blocks of 8
        e.nodes += ((count + 7) / 8 * 8 - count) * LiveBytes;
        report.scenes.push_back(std::move(e));
    };
    scene(description_.path, description_.nodes);
    for (const auto& s : description_.scenes) scene(s.path, s.nodes);
    return report;
}

bool Runtime::drawn_rect(uint32_t id, int& x, int& y, int& w, int& h) const {
    for (const auto& it : overlay_)
        if (it.id == id) {
            x = it.x;
            y = it.y;
            w = it.w;
            h = it.h;
            return true;
        }
    return false;
}

// Jet's per-frame queues would grow while drawing, each time holding the old and the doubled buffer for a moment:
// reserve them once per scene instead. Back faces and what's out of view never reach them, so half of the scene's
// triangles is usually plenty (hidden nodes count, they have geometry too); a busier frame still grows them.
void Runtime::reserve_triangle_queues() {
    size_t triangles = 0;
    for (const auto& o : objects_) {
        triangles += o->triangles.size();
#if JET_MESH_INSTANCING
        for (const auto& instance : o->instances) triangles += instance.mesh->triangles.size();
#endif
    }
    scene_.reserveTriangles(triangles / 2 + 32);
    queues_reserved_ = true;
}

void Runtime::render() {
    if (canvas_override()) {
        // The editor's 2D view: the canvas on the scene's background colour, without the 3D world.
        uint32_t background = 0x161c28;
        for (const auto& n : nodes_)
            if (n.d.type == NodeType::WorldEnvironment && !n.freed && n.in_tree) { background = n.d.more().background; break; }
        std::fill(target_, target_ + size_t(buffer_width()) * buffer_rows(), rgb565(background));
        particle_triangles_ = 0;
        draw_overlay();
        return;
    }
    apply_camera();
    prepare_background();
    update_reflections();
    if (!queues_reserved_) reserve_triangle_queues();
    scene_.render(executor_);
    render_particles(); // after the raster bands joined, as JetExamples' RenderEffects
    draw_overlay();
}

void Runtime::set_input(uint32_t pressed) { input_ = pressed; }

int Runtime::action_index(std::string_view name) const {
    for (size_t i = 0; i < description_.actions.size(); ++i)
        if (description_.actions[i].name == name) return int(i);
    return -1;
}

std::vector<InputAction> default_actions() {
    return {{"ui_left", {"Left", "A"}}, {"ui_right", {"Right", "D"}}, {"ui_up", {"Up", "W"}}, {"ui_down", {"Down", "S"}},
            {"ui_accept", {"Space", "Enter"}}, {"ui_cancel", {"Escape", "Backspace"}}};
}

void Runtime::resize(int width, int height) {
    if (width == width_ && height == height_) return;
    width_ = width;
    height_ = height;
    color_.assign(size_t(buffer_width()) * buffer_rows(), 0);
    depth_.assign(Z_BUFFERING ? size_t(ZBUFFER_STRIDE(width)) * height : 0, 0);
    target_ = color_.data();
    scene_.resize(color_.data(), Z_BUFFERING ? depth_.data() : nullptr, width, height);
}

void Runtime::set_framebuffer(uint16_t* buffer) {
    target_ = buffer ? buffer : color_.data();
    scene_.setFramebuffer(target_);
}

int Runtime::pick(int x, int y) {
    if (x < 0 || y < 0 || x >= width_ || y >= height_) return -1;
    const int control = pick_overlay(x, y);
    if (control >= 0 || canvas_override()) return control;
#if MAX_PICK_QUERIES == 0
    return -1; // device builds: no 3D picking
#else
    const Renderer::PickQuery query{int16_t(x), int16_t(y)};
    scene_.setPickQueries(&query, 1);
    render();
    const auto hit = scene_.getPickResults()[0];
    scene_.setPickQueries(nullptr, 0);
    if (hit.hit)
        for (const auto& n : nodes_)
            if (n.object && n.object == hit.object) return int(n.d.id);
    return -1;
#endif
}

bool Runtime::node_state(uint32_t id, NodeDesc& out) const {
    const int index = index_of(id);
    if (index < 0 || nodes_[index].freed) return false;
    out = nodes_[index].d;
    return true;
}

} // namespace jetstudio
