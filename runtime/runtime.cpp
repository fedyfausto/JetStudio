#include "runtime.hpp"
#include "physics.hpp"
#include "script_host.hpp"
#include "Primitives.hpp"
#include "EnvironmentMapping.hpp"
#include "ParticleSystem.hpp"
#include "font_data.hpp"
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cctype>
#include <cmath>
#include <limits>
#include <unordered_set>
#include <set>

// Jet's depth fog distances (see JetConfig.hpp), set by Runtime::applyEnvironment()
int jetFogNear = std::numeric_limits<int>::max(), jetFogFar = std::numeric_limits<int>::max();

namespace jetstudio{
    namespace{
        // 1 m = 100 Jet units
        constexpr float JetUnits = 100;
        // Godot (right-handed, -Z forward) to Jet (left-handed, +Z forward).
        ::Vector3 toJet(Vec3 v){
            return {int32_t(std::lround(v.x * JetUnits)), int32_t(std::lround(v.y * JetUnits)), int32_t(std::lround(-v.z * JetUnits))};
        }
        bool bounded(Vec3 v, float bound){
            return v.finite() && std::abs(v.x) <= bound && std::abs(v.y) <= bound && std::abs(v.z) <= bound;
        }
        bool validScale(Vec3 s){
            for(int i = 0; i < 3; ++i) if(!std::isfinite(s[i]) || std::abs(s[i]) < 0.001f || std::abs(s[i]) > 1000) return false;

            return true;
        }
        Renderer::Color colorOf(uint32_t c){ return {uint8_t(c >> 16), uint8_t(c >> 8), uint8_t(c)}; }
        bool validName(const std::string& name){
            if(name.empty() || name.size() > 80) return false;

            for(char c : name) if(c == '/' || c == ':' || c == '"' || c == '%' || c == '@' || c == '.' || uint8_t(c) < 32) return false;

            return true;
        }
        int degreesInt(float radiansValue){
            int d = int(std::lround(degrees(radiansValue))) % 360;
            return d < 0 ? d + 360 : d;
        }
        bool same(Vec3 a, Vec3 b){ return std::abs(a.x - b.x) < 1e-4f && std::abs(a.y - b.y) < 1e-4f && std::abs(a.z - b.z) < 1e-4f; }

        // Profile points are {radius, y, normal radius, normal y}, revolved around Y.
        Renderer::Object* lathe(const std::vector<std::array<float, 4>>& profile, int segments, Renderer::Material* material){
            auto* mesh = new Renderer::Object();

            for(size_t p = 0; p < profile.size(); ++p)
                for(int s = 0; s <= segments; ++s){
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

            for(size_t p = 0; p + 1 < profile.size(); ++p)
                for(int s = 0; s < segments; ++s){
                    const uint16_t a = uint16_t(p * row + s), b = uint16_t(a + 1), c = uint16_t(a + row), d = uint16_t(c + 1);

                    if(profile[p][0] > 0) mesh->addTriangle(a, b, c, material);

                    if(profile[p + 1][0] > 0) mesh->addTriangle(b, d, c, material);
                }

            return mesh;
        }

        // Z is mirrored, so the winding is reversed too to keep faces pointing outwards in Jet's left-handed space.
        Renderer::Object* makeObject(const std::vector<MeshVertex>& vertices, const std::vector<uint16_t>& indices, Renderer::Material* material){
            auto* mesh = new Renderer::Object();
            mesh->vertices.reserve(vertices.size());

            for(const auto& v : vertices){
                Renderer::Object::Vertex out;
                out.position = {int32_t(std::lround(v.position.x * JetUnits)), int32_t(std::lround(v.position.y * JetUnits)),
                                int32_t(std::lround(-v.position.z * JetUnits))};
                const Vec3 n = v.normal.normalized();
                out.normal = {int32_t(std::lround(n.x * FIXED_POINT_SCALE)), int32_t(std::lround(n.y * FIXED_POINT_SCALE)),
                              int32_t(std::lround(-n.z * FIXED_POINT_SCALE))};
                out.uv = {int32_t(std::lround(v.u * FIXED_POINT_SCALE)), int32_t(std::lround(v.v * FIXED_POINT_SCALE))};
                mesh->vertices.push_back(out);
            }

            for(size_t i = 0; i + 2 < indices.size(); i += 3) mesh->addTriangle(indices[i], indices[i + 2], indices[i + 1], material);

            return mesh;
        }

        // Godot's QuadMesh: 1x1 m facing +Z, one-sided unless cull is Disabled.
        Renderer::Object* quad(Renderer::Material* material){
            const std::vector<MeshVertex> v = {
                {{-0.5f, 0.5f, 0}, {0, 0, 1}, 0, 0}, {{0.5f, 0.5f, 0}, {0, 0, 1}, 1, 0},
                {{0.5f, -0.5f, 0}, {0, 0, 1}, 1, 1}, {{-0.5f, -0.5f, 0}, {0, 0, 1}, 0, 1}};
            return makeObject(v, {0, 3, 2, 0, 2, 1}, material);
        }

        // Godot's TorusMesh: inner radius 0.5 m, outer 1 m.
        Renderer::Object* torus(int segments, Renderer::Material* material){
            const int rings = std::max(3, segments), sides = std::max(4, segments / 2);
            const float major = 0.75f, minor = 0.25f;
            std::vector<MeshVertex> v;
            std::vector<uint16_t> idx;

            for(int i = 0; i <= rings; ++i){
                const float a = 2 * Pi * float(i) / float(rings);
                const Vec3 radial{std::cos(a), 0, -std::sin(a)};

                for(int j = 0; j <= sides; ++j){
                    const float t = 2 * Pi * float(j) / float(sides);
                    const Vec3 normal = radial * std::cos(t) + Vec3{0, std::sin(t), 0};
                    v.push_back({radial * major + normal * minor, normal, float(i) / float(rings), float(j) / float(sides)});
                }
            }

            const int row = sides + 1;

            for(int i = 0; i < rings; ++i)
                for(int j = 0; j < sides; ++j){
                    const uint16_t a = uint16_t(i * row + j), b = uint16_t(a + 1), c = uint16_t(a + row), d = uint16_t(c + 1);
                    idx.insert(idx.end(), {a, c, b, b, c, d});
                }

            return makeObject(v, idx, material);
        }

        // Godot's CylinderMesh: radius 0.5 m, height 2 m. Jet's createCylinder mixes windings and breaks back-face culling.
        Renderer::Object* cylinder(int segments, Renderer::Material* material){
            const std::vector<std::array<float, 4>> profile = {
                {0, 100, 0, 1}, {50, 100, 0, 1}, {50, 100, 1, 0}, {50, -100, 1, 0}, {50, -100, 0, -1}, {0, -100, 0, -1}};
            return lathe(profile, segments, material);
        }

        // Godot's CapsuleMesh: radius 0.5 m, total height 2 m (Jet's own capsule has broken normals).
        Renderer::Object* capsule(int segments, Renderer::Material* material){
            const float radius = 50, half = 50;
            const int rings = std::max(2, segments / 4);
            std::vector<std::array<float, 4>> profile;

            for(int i = 0; i <= rings; ++i){
                const float a = Pi * 0.5f * float(i) / float(rings);
                profile.push_back({radius * std::sin(a), half + radius * std::cos(a), std::sin(a), std::cos(a)});
            }

            for(int i = 0; i <= rings; ++i){
                const float a = Pi * 0.5f + Pi * 0.5f * float(i) / float(rings);
                profile.push_back({radius * std::sin(a), -half + radius * std::cos(a), std::sin(a), std::cos(a)});
            }

            return lathe(profile, segments, material);
        }

        // Screen pixel from a float: clamped first, a float out of the int range would be undefined.
        int screenPixel(float v){
            constexpr float limit = float(1 << 28);
            return std::isnan(v) ? 0 : int(std::clamp(v, -limit, limit));
        }
    }

    const char* nodeTypeName(NodeType type){
        switch(type){
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
    bool is_control(NodeType type){
        return type == NodeType::Control || type == NodeType::Label || type == NodeType::TextureRect || type == NodeType::ColorRect;
    }
    // Weak: a firmware may define its own (see coldAlloc in runtime.hpp). MinGW can not link weak symbols from a
    // static library, and Windows never builds a firmware.
#ifdef _WIN32
#define JET_DEFAULT
#else
#define JET_DEFAULT __attribute__((weak))
#endif
    JET_DEFAULT void* coldAlloc(size_t bytes){
        void* p = std::malloc(bytes ? bytes : 1);

        if(!p){
            std::fprintf(stderr, "Out of memory (%zu bytes)\n", bytes);
            std::abort();
        }

        return p;
    }
    JET_DEFAULT void coldFree(void* p){ std::free(p); }

    bool usesExtras(NodeType type){
        return type != NodeType::Node && type != NodeType::Node3D && type != NodeType::MeshInstance3D && type != NodeType::Sprite3D
            && type != NodeType::Node2D && type != NodeType::Sprite2D;
    }
    bool isNode2d(NodeType type){
        return type == NodeType::Node2D || type == NodeType::Sprite2D || type == NodeType::AnimatedSprite2D || type == NodeType::Camera2D
            || physics::isFlat(type);
    }
    bool isNode3d(NodeType type){
        return type != NodeType::Node && type != NodeType::WorldEnvironment && type != NodeType::Timer
            && type != NodeType::AnimationPlayer && !is_control(type) && !isNode2d(type);
    }
    uint16_t rgb565(uint32_t c){ return uint16_t(((c >> 19) & 31) << 11 | ((c >> 10) & 63) << 5 | ((c >> 3) & 31)); }

    bool validate(const Description& d, std::string& error){
        error.clear();
        auto fail = [&](std::string message){ error = std::move(message); return false; };

        if(d.width < MinScreenSide || d.height < MinScreenSide || d.width > MaxScreenWidth || d.height > MaxScreenHeight
            || d.width % 2 || d.height % 2)
            return fail("Unsupported resolution: " + std::to_string(d.width) + "x" + std::to_string(d.height) + " (even sides, from "
                        + std::to_string(MinScreenSide) + " to " + std::to_string(MaxScreenWidth) + "x" + std::to_string(MaxScreenHeight) + ").");

        if(d.actions.size() > MaxActions) return fail("At most 32 actions in the Input Map.");

        for(size_t i = 0; i < d.actions.size(); ++i){
            const auto& action = d.actions[i];
            bool ok = !action.name.empty() && action.name.size() <= 64 && action.keys.size() <= MaxActionKeys;

            for(char c : action.name) ok = ok && (std::isalnum(uint8_t(c)) || c == '_');

            for(const auto& key : action.keys) ok = ok && !key.empty() && key.size() <= 32;

            for(size_t j = 0; j < i; ++j) ok = ok && d.actions[j].name != action.name;

            if(!ok) return fail("Invalid action in the Input Map: \"" + action.name + "\" (letters, digits and _; unique names).");
        }

        if(d.scripts.size() > MaxScripts) return fail("At most 64 scripts per scene.");

        size_t scriptBytes = 0;

        for(const auto& script : d.scripts){
            if(script.path.rfind("res://", 0) != 0 || script.path.size() > 512) return fail("Invalid script path: " + script.path);

            scriptBytes += script.source.size() + script.bytecode.size();
        }

        if(scriptBytes > MaxScriptBytes) return fail("Scripts exceed 256 KiB in total.");

        if(d.textures.size() > MaxTextures) return fail("At most 64 textures per scene.");

        size_t textureBytes = 0;

        for(const auto& t : d.textures){
            const size_t count = size_t(t.width) * t.height;
            const bool indexed = !t.palette.empty();

            if(!t.width || !t.height || t.width > MaxTextureSide || t.height > MaxTextureSide
                || (indexed ? ((!t.packedIndices && t.indices.size() != count) || t.palette.size() > 256 || !t.pixels.empty())
                            : (t.pixels.size() != count || !t.indices.empty() || t.packedIndices)))
                return fail("Invalid texture: " + t.path);

            if(indexed)
                for(size_t i = 0; i < count; ++i) if(t.indexData()[i] >= t.palette.size()) return fail("Invalid texture: " + t.path);

            textureBytes += t.bytes();
        }

        if(textureBytes > MaxTextureBytes) return fail("Textures exceed 4 MiB in total.");

        auto textureOk = [&](int32_t i){ return i >= -1 && (i < 0 || size_t(i) < d.textures.size()); };

        if(d.meshes.size() > MaxMeshAssets) return fail("At most 128 models per scene.");

        size_t totalVertices = 0;

        for(const auto& m : d.meshes){
            totalVertices += m.vertices.size();

            if(m.vertices.empty() || m.vertices.size() > MaxMeshVertices || m.indices.empty() || m.indices.size() % 3)
                return fail("Invalid model: " + m.path);

            for(uint16_t i : m.indices) if(i >= m.vertices.size()) return fail("Invalid model: " + m.path);

            for(const auto& v : m.vertices)
                if(!bounded(v.position, 1000) || !v.normal.finite() || !std::isfinite(v.u) || !std::isfinite(v.v) || std::abs(v.u) > 1000
                    || std::abs(v.v) > 1000) return fail("Model with invalid coordinates: " + m.path);
        }

        if(totalVertices > MaxSceneVertices) return fail("Models exceed 262144 vertices in total.");

        auto checkConnections = [&](const std::vector<NodeDesc>& nodes, const std::vector<SceneConnection>& connections) -> bool {
            if(connections.size() > MaxConnections) return fail("At most 512 signal connections per scene.");

            auto identifier = [](const std::string& s){
                if(s.empty() || s.size() > 80 || std::isdigit(uint8_t(s[0]))) return false;

                for(char c : s) if(!std::isalnum(uint8_t(c)) && c != '_') return false;

                return true;
            };

            for(const auto& c : connections)
                if(c.from < 0 || c.to < 0 || size_t(c.from) >= nodes.size() || size_t(c.to) >= nodes.size() || !identifier(c.signal)
                    || !identifier(c.method) || (c.flags & ~6u))
                    return fail("Invalid signal connection: " + c.signal + " -> " + c.method + ".");
            return true;
        };

        auto checkNodes = [&](const std::vector<NodeDesc>& nodes) -> bool {
            if(nodes.empty()) return fail("The scene must have a root node.");

            if(nodes.size() > MaxNodes) return fail("At most 256 nodes per scene.");

            std::unordered_set<uint32_t> ids;
            std::unordered_set<std::string> siblings;
            size_t meshes = 0;

            for(size_t i = 0; i < nodes.size(); ++i){
                const auto& n = nodes[i];

                if(i == 0 ? n.parent != -1 : (n.parent < 0 || size_t(n.parent) >= i)) return fail("Invalid node hierarchy.");

                if(!n.id || n.id >= InstanceIdBase || !ids.insert(n.id).second) return fail("Duplicate or null node identifier.");

                if(!validName(n.name)) return fail("Invalid node name: \"" + n.name + "\" (1..80 characters, without . : @ / \" %).");

                if(!siblings.insert(std::to_string(n.parent) + "/" + n.name).second) return fail("Two sibling nodes have the same name: " + n.name);

                if(uint8_t(n.type) > uint8_t(NodeType::AnimationPlayer)) return fail("Unsupported node type.");

                if(n.type == NodeType::AnimationPlayer){
                    const auto& animations = n.more().playerAnimations;

                    if(animations.size() > MaxAnimations) return fail("At most 64 animations in " + n.name + ".");

                    for(const auto& a : animations){
                        bool ok = !a.name.empty() && a.name.size() <= 80 && std::isfinite(a.length) && a.length > 0 && a.length <= 10000
                            && a.loop <= 2 && a.tracks.size() <= MaxTracks;

                        for(const auto& t : a.tracks){
                            ok = ok && t.path.size() <= 256 && t.property.size() <= 80 && t.interpolation <= 1 && t.keys.size() <= MaxKeys;

                            for(size_t k = 0; ok && k < t.keys.size(); ++k)
                                ok = std::isfinite(t.keys[k].time) && t.keys[k].time >= 0 && t.keys[k].time <= a.length
                                    && std::isfinite(t.keys[k].transition) && (k == 0 || t.keys[k].time >= t.keys[k - 1].time)
                                    && t.keys[k].method.size() <= 80 && t.keys[k].args.size() <= 8;
                        }

                        if(!ok) return fail("Invalid animation \"" + a.name + "\" in " + n.name + ".");
                    }
                }

                if(n.type == NodeType::Timer && (!std::isfinite(n.more().lifetime) || n.more().lifetime < 0.001f || n.more().lifetime > 100000))
                    return fail("Invalid wait time in " + n.name + " (0.001 to 100000 seconds).");

                if(n.type == NodeType::CPUParticles3D){
                    const float values[] = {n.more().lifetime, n.more().lifetime_randomness, n.more().explosiveness, n.more().spread, n.more().velocityMin, n.more().velocityMax,
                                            n.more().damping_min, n.more().damping_max, n.more().emissionRadius};

                    for(float v : values) if(!std::isfinite(v)) return fail("Invalid particle parameter in " + n.name + ".");

                    if(n.more().amount < 1 || n.more().amount > MaxParticles || n.more().lifetime < 0.01f || n.more().lifetime > 600 || n.more().lifetime_randomness < 0
                        || n.more().lifetime_randomness > 1 || n.more().explosiveness < 0 || n.more().explosiveness > 1 || n.more().spread < 0 || n.more().spread > 180
                        || n.more().velocityMin < 0 || n.more().velocityMax < 0 || n.more().velocityMin > 1000 || n.more().velocityMax > 1000 || !bounded(n.more().direction, 1000)
                        || n.more().damping_min < 0 || n.more().damping_max < 0 || n.more().damping_min > 1000 || n.more().damping_max > 1000
                        || !bounded(n.more().gravity, 1000) || n.more().emissionRadius < 0 || n.more().emissionRadius > 1000 || !bounded(n.more().emissionBox, 1000)
                        || n.more().emissionBox.x < 0 || n.more().emissionBox.y < 0 || n.more().emissionBox.z < 0
                        || uint8_t(n.more().particle_style) > uint8_t(ParticleStyle::Puff) || uint8_t(n.more().emission_shape) > uint8_t(EmissionShape::Box)
                        || n.color > 0xffffff || n.more().color_end > 0xffffff || !std::isfinite(n.alpha) || n.alpha < 0 || n.alpha > 1
                        || !std::isfinite(n.more().puff_size) || !std::isfinite(n.more().puff_size_end) || n.more().puff_size < 0 || n.more().puff_size > 100
                        || n.more().puff_size_end < 0 || n.more().puff_size_end > 100)
                        return fail("Particle parameter out of range in " + n.name + " (amount 1-200, lifetime 0.01-600 s, spread 0-180).");
                }

                //sizes in metres, or in pixels for 2D shapes (whose box depth is not used).
                const float most = physics::isFlat(n.type) ? 100000 : 1000;

                if(physics::isCollisionShape(n.type)
                    && (uint8_t(n.more().collision) > uint8_t(CollisionKind::Capsule) || !bounded(n.more().boxSize, most) || n.more().boxSize.x <= 0 || n.more().boxSize.y <= 0
                        || (n.type == NodeType::CollisionShape3D && n.more().boxSize.z <= 0) || !std::isfinite(n.more().radius) || n.more().radius <= 0
                        || n.more().radius > most || !std::isfinite(n.more().height) || n.more().height <= 0 || n.more().height > most))
                    return fail("Invalid collision shape in " + n.name + ".");

                if(physics::isCollisionObject(n.type)){
                    const float values[] = {n.more().mass, n.more().gravity_scale, n.more().linear_damp, n.more().angular_damp, n.more().friction, n.more().bounce, n.more().floor_max_angle};

                    for(float v : values) if(!std::isfinite(v) || std::abs(v) > 100000) return fail("Invalid physics parameter in " + n.name + ".");

                    if(n.more().mass <= 0 || n.more().bounce < 0 || n.more().bounce > 1 || n.more().friction < 0 || n.more().linear_damp < 0 || n.more().angular_damp < 0
                        || n.more().floor_max_angle < 0 || n.more().floor_max_angle > 89 || !bounded(n.more().linear_velocity, most) || !bounded(n.more().angular_velocity, 1000))
                        return fail("Physics parameter out of range in " + n.name + ".");
                }

                if(isNode2d(n.type)){
                    //negative scales flip, as in Godot.
                    if(!bounded(n.position, 100000) || !bounded(n.rotation, 100000) || !bounded(n.scale, 1000) || !std::isfinite(n.offset[0])
                        || !std::isfinite(n.offset[1]) || std::abs(n.offset[0]) > 100000 || std::abs(n.offset[1]) > 100000
                        || n.z_index < -4096 || n.z_index > 4096 || n.color > 0xffffff || !std::isfinite(n.alpha) || n.alpha < 0 || n.alpha > 1)
                        return fail("Invalid 2D node: " + n.name + ".");
                } else if(!bounded(n.position, is_control(n.type) ? 10000 : 1000) || !bounded(n.rotation, 100000) || !validScale(n.scale)){
                    return fail("Transform out of range in " + n.name + ".");
                }

                if(n.type == NodeType::Sprite2D || n.type == NodeType::AnimatedSprite2D){
                    std::vector<SpriteAnimation> animations;

                    if(!textureOk(n.texture) || n.hframes < 1 || n.vframes < 1 || n.hframes > 256 || n.vframes > 256
                        || n.frame >= n.hframes * n.vframes || n.more().animations.size() > MaxText
                        || !parseAnimations(n.more().animations, n.hframes * n.vframes, animations) || !std::isfinite(n.more().speed_scale)
                        || std::abs(n.more().speed_scale) > 100)
                        return fail("Invalid sprite in " + n.name + ".");
                }

                if(n.type == NodeType::Camera2D){
                    const float* zoom = n.more().zoom;

                    if(!std::isfinite(zoom[0]) || !std::isfinite(zoom[1]) || zoom[0] <= 0.01f || zoom[1] <= 0.01f || zoom[0] > 100 || zoom[1] > 100)
                        return fail("Invalid Camera2D zoom in " + n.name + ".");
                }

                if(is_control(n.type) && (n.more().text.size() > MaxText || !n.more().size.finite() || n.more().size.x < 0 || n.more().size.y < 0 || n.more().size.x > 4096
                    || n.more().size.y > 4096 || n.more().text_scale < 1 || n.more().text_scale > 8 || n.color > 0xffffff || !std::isfinite(n.alpha) || n.alpha < 0
                    || n.alpha > 1 || !textureOk(n.texture))) return fail("Invalid 2D node: " + n.name + ".");

                if(n.type == NodeType::MeshInstance3D){
                    if(++meshes > MaxMeshes) return fail("At most 128 meshes per scene.");

                    if(uint8_t(n.shape) > uint8_t(Shape::Mesh) || uint8_t(n.shading) > uint8_t(Shading::Water)
                        || n.color > 0xffffff || n.segments < 3 || n.segments > 48) return fail("Invalid mesh in " + n.name + ".");
                    //mesh -1: no model assigned yet, the node draws nothing
                    if(n.shape == Shape::Mesh && (n.mesh < -1 || (n.mesh >= 0 && size_t(n.mesh) >= d.meshes.size())))
                        return fail("Missing model for " + n.name + ".");

                    if(!std::isfinite(n.diffuse) || n.diffuse < 0 || n.diffuse > 1) return fail("Invalid material in " + n.name + ".");

                    const bool uvOk = std::isfinite(n.uv_scale[0]) && std::isfinite(n.uv_scale[1]) && std::isfinite(n.uv_offset[0])
                        && std::isfinite(n.uv_offset[1]) && std::abs(n.uv_scale[0]) <= 1000 && std::abs(n.uv_scale[1]) <= 1000
                        && std::abs(n.uv_offset[0]) <= 1000 && std::abs(n.uv_offset[1]) <= 1000;

                    if(!textureOk(n.texture) || uint8_t(n.cull) > uint8_t(CullMode::Disabled) || uint8_t(n.draw_order) > uint8_t(DrawOrder::OnTop)
                        || !uvOk || !std::isfinite(n.alpha)
                        || n.alpha < 0 || n.alpha > 1 || !std::isfinite(n.specular) || n.specular < 0 || n.specular > 1)
                        return fail("Invalid material in " + n.name + ".");
                }

                if(n.type == NodeType::Sprite3D){
                    if(++meshes > MaxMeshes) return fail("At most 128 meshes per scene.");

                    if(!textureOk(n.texture) || n.hframes < 1 || n.vframes < 1 || n.hframes > 256 || n.vframes > 256
                        || n.frame >= n.hframes * n.vframes || !std::isfinite(n.pixel_size) || n.pixel_size <= 0 || n.pixel_size > 100
                        || n.color > 0xffffff || !std::isfinite(n.alpha) || n.alpha < 0 || n.alpha > 1
                        || uint8_t(n.draw_order) > uint8_t(DrawOrder::OnTop)) return fail("Invalid sprite in " + n.name + ".");
                }

                if(n.type == NodeType::Camera3D && (!std::isfinite(n.more().fov) || n.more().fov < 1 || n.more().fov > 179 || !std::isfinite(n.more().near)
                    || !std::isfinite(n.more().far) || n.more().near < 0.001f || n.more().far <= n.more().near || n.more().far > 10000)) return fail("Invalid camera in " + n.name + ".");

                if(n.type == NodeType::DirectionalLight3D && (n.more().light_color > 0xffffff || !std::isfinite(n.more().energy) || n.more().energy < 0 || n.more().energy > 16))
                    return fail("Invalid light in " + n.name + ".");

                if(n.type == NodeType::WorldEnvironment && (n.more().background > 0xffffff || n.more().ambient > 0xffffff
                    || uint8_t(n.more().background_mode) > uint8_t(Background::Panorama) || n.more().skyTop > 0xffffff || n.more().skyHorizon > 0xffffff
                    || n.more().groundHorizon > 0xffffff || n.more().groundBottom > 0xffffff || !textureOk(n.more().sky_texture)
                    || !std::isfinite(n.more().fog_begin) || !std::isfinite(n.more().fog_end) || n.more().fog_begin < 0
                    || n.more().fog_end <= n.more().fog_begin))
                    return fail("Invalid environment in " + n.name + ".");

                if(n.script < -1 || (n.script >= 0 && size_t(n.script) >= d.scripts.size())) return fail("Missing script for " + n.name + ".");

                for(const auto& o : n.overrides) if(o.name.empty() || o.name.size() > 80) return fail("Invalid script property in " + n.name + ".");
            }

            return true;
        };

        if(!checkNodes(d.nodes) || !checkConnections(d.nodes, d.connections)) return false;

        if(d.autoloads.size() > MaxAutoloads) return fail("At most 16 autoloads.");

        for(size_t i = 0; i < d.autoloads.size(); ++i){
            const auto& a = d.autoloads[i];
            bool ok = !a.name.empty() && a.name.size() <= 64 && !std::isdigit(uint8_t(a.name[0])) && a.name != "Input"
                && a.script >= 0 && size_t(a.script) < d.scripts.size();

            for(char c : a.name) ok = ok && (std::isalnum(uint8_t(c)) || c == '_');

            for(size_t j = 0; j < i; ++j) ok = ok && d.autoloads[j].name != a.name;

            if(!ok) return fail("Invalid autoload: \"" + a.name + "\" (a unique name of letters, digits and _, and a script).");
        }

        if(d.scenes.size() > MaxScenes) return fail("At most 64 scenes per project.");

        for(size_t i = 0; i < d.scenes.size(); ++i){
            const auto& scene = d.scenes[i];
            bool ok = scene.path.rfind("res://", 0) == 0 && scene.path.size() <= 512 && scene.path != d.path;

            for(size_t j = 0; j < i; ++j) ok = ok && d.scenes[j].path != scene.path;

            if(!ok) return fail("Invalid or duplicate scene path: " + scene.path);

            if(!checkNodes(scene.nodes) || !checkConnections(scene.nodes, scene.connections)){
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
        host_(std::make_unique<ScriptHost>(this)), vm_(std::make_unique<jetscript::VM>(*host_)){
    
        scene_.getRenderer()->interlacedMode = FIELD_BUFFERS != 0;
        scene_.setCamera(&camera_);
        scene_.setDirectionalLight(&sun_);
        scene_.setAmbientLight(&ambient_);
        Description empty;
        empty.nodes.push_back({});
        std::string error;
        load(empty, error);
    }
    Runtime::~Runtime(){
        vm_.reset();
        scene_.getObjects().clear();
    }

    bool Runtime::load(Description d, std::string& error){
        if(!validate(d, error)) return false;

        pack_ = nullptr;
        packSize_ = 0;
        playing_ = false;
        editorView_ = true;
        vm_->clear();
        vm_->takeErrors();
        description_ = std::move(d);
        sceneNodes_ = &description_.nodes;
        currentScene_ = description_.path;
        pendingScene_.clear();
        resize(description_.width, description_.height);
        return build(error);
    }

    int Runtime::indexOf(uint32_t id) const{
        const auto found = index_.find(id);
        return found == index_.end() ? -1 : found->second;
    }

    namespace{
        // The frame of a Sprite3D sheet as a UV window; a flip runs the window backwards.
        void spriteUv(NodeDesc& d){
            const float w = 1.0f / float(d.hframes), h = 1.0f / float(d.vframes);
            const int col = d.frame % d.hframes, row = d.frame / d.hframes;
            d.uv_scale[0] = d.flip_h ? -w : w;
            d.uv_scale[1] = d.flip_v ? -h : h;
            d.uv_offset[0] = float(d.flip_h ? col + 1 : col) * w;
            d.uv_offset[1] = float(d.flip_v ? row + 1 : row) * h;
        }
    }

    // Jet's primitives are symmetric under the Z mirror, so they are used as is and keep Jet's winding and UV layout.
    Renderer::Object* Runtime::makeMesh(const NodeDesc& d, Renderer::Material* material) const{
        switch(d.shape){
            case Shape::Box: return Primitives::createCube(100, 100, 100, material);
            case Shape::Sphere: return Primitives::createSphere(50, d.segments, material);
            case Shape::Plane: return Primitives::createPlane(200, 200, material);
            case Shape::Pyramid:{
                Renderer::Object* mesh = Primitives::createPyramid(100, 100, material);

                //pivot at the centre
                for(auto& vertex : mesh->vertices) vertex.position.y -= 50;

                //Jet's pyramid is wound inwards, flip it so it culls like the others.
                for(auto& t : mesh->triangles) std::swap(t.v2, t.v3);

                return mesh;
            }
            case Shape::Cylinder: return cylinder(d.segments, material);
            case Shape::Capsule: return capsule(d.segments, material);
            case Shape::Quad: return quad(material);
            case Shape::Torus: return torus(d.segments, material);
            case Shape::Mesh: break;
        }

        const auto& asset = description_.meshes[size_t(d.mesh)];
        return makeObject(asset.vertices, asset.indices, material);
    }

    size_t Runtime::nodeBytes() const{
        size_t bytes = nodes_.capacity() * sizeof(Live);

        for(const auto& n : nodes_){
            if(n.d.hasExtras()) bytes += sizeof(NodeExtras);

            if(n.motion_.allocated()) bytes += sizeof(Motion);
        }

        return bytes;
    }

    bool Runtime::load_pack(const uint8_t* data, size_t size, std::string& error, bool textures_in_place){
        Description d;

        if(!decode_pack(data, size, d, error, textures_in_place) || !load(std::move(d), error)) return false;

        pack_ = data;
        packSize_ = size;
        //every scene was validated while decoding; the built one now lives in nodes_.
        std::vector<NodeDesc>().swap(description_.nodes);
        std::vector<SceneConnection>().swap(description_.connections);

        for(auto& scene : description_.scenes){
            std::vector<NodeDesc>().swap(scene.nodes);
            std::vector<SceneConnection>().swap(scene.connections);
        }

        return true;
    }

    // The nodes of a scene, from the description or read again from the package.
    const std::vector<NodeDesc>* Runtime::nodesOf(std::string_view path, std::vector<SceneConnection>* connections){
        const std::vector<NodeDesc>* nodes = nullptr;
        const std::vector<SceneConnection>* links = nullptr;
        uint32_t offset = 0;

        if(path == description_.path){ nodes = &description_.nodes; links = &description_.connections; offset = description_.nodesOffset; }

        for(const auto& scene : description_.scenes)
            if(scene.path == path){ nodes = &scene.nodes; links = &scene.connections; offset = scene.offset; }

        if(!nodes || !pack_ || !nodes->empty()){
            if(connections && links) *connections = *links;

            return nodes;
        }

        std::string error;
        std::vector<SceneConnection> read;

        if(!decodeNodes(pack_, packSize_, offset, packNodes_, read, error)) return nullptr;

        if(connections) *connections = std::move(read);

        return &packNodes_;
    }

    bool Runtime::build(std::string& error){
        error.clear();
        //the scene only borrows the objects, so drop its pointers first.
        scene_.getObjects().clear();
        objects_.clear();
        queuesReserved_ = false;
        sharedMeshes_.clear();
        materials_.clear();
        textures_.clear();
        nodes_.clear();
        index_.clear();
        meshes_ = 0;
        pendingFree_ = reorderNeeded_ = false;
        nextInstanceId_ = InstanceIdBase;

        if(!sceneNodes_) sceneNodes_ = &description_.nodes;

        const auto& sceneNodes = *sceneNodes_;

        nodes_.clear();
        nodes_.reserve(sceneNodes.size() + description_.autoloads.size());
        nodes_.resize(sceneNodes.size());

        for(size_t i = 0; i < nodes_.size(); ++i){
            Live& n = nodes_[i];
            n.d = sceneNodes[i];
            n.parent = n.d.parent;

            if(n.parent >= 0) nodes_[n.parent].children.push_back(int32_t(i));

            index_[n.d.id] = int32_t(i);
            createVisual(int32_t(i));
        }

        sceneIds_.clear();

        if(!sceneConnections_.empty())
            for(size_t i = 0; i < sceneNodes.size(); ++i) sceneIds_.push_back(nodes_[i].d.id);

        //autoloads come after the scene, outside its tree, and keep their ids across scene changes.
        for(size_t i = 0; i < description_.autoloads.size(); ++i){
            Live a;
            a.d.type = NodeType::Node;
            a.d.id = AutoloadIdBase + uint32_t(i);
            a.d.name = description_.autoloads[i].name;
            a.d.script = description_.autoloads[i].script;
            index_[a.d.id] = int32_t(nodes_.size());
            nodes_.push_back(std::move(a));
        }

        if(sceneNodes_ == &packNodes_){
            std::vector<NodeDesc>().swap(packNodes_);
            sceneNodes_ = nullptr;
        }

        collectPhysics();
        //created by start()
        particles_.clear();
        particleTriangles_ = 0;
        updateTransforms();
        applyEnvironment();
        applyCamera();
        return true;
    }

    void Runtime::createVisual(int32_t index){
        Live& n = nodes_[size_t(index)];

        if(n.d.type == NodeType::Sprite3D){
            //a quad, unlit and double-sided like Godot's Sprite3D, the frame picked through the UVs.
            n.d.shape = Shape::Quad;
            n.d.shading = Shading::Unlit;
            n.d.cull = CullMode::Disabled;
            n.d.repeat = false;
            n.d.perspective = true;
            n.d.reflection = false;
            spriteUv(n.d);
        } else if(n.d.type != NodeType::MeshInstance3D || (n.d.shape == Shape::Mesh && n.d.mesh < 0)){
            return;
        }

        auto material = std::make_unique<Renderer::Material>(rgb565(n.d.color));
        //reflections rewrite their UVs every frame, so they always keep geometry of their own.
        //the others get their shared copy at the first bake, once the world scale is known.
        Renderer::Object* mesh = n.d.reflection ? makeMesh(n.d, material.get()) : new Renderer::Object();
        mesh->calculateBoundingBox();
        //forces the first bake
        n.bakedScale = {0, 0, 0};
        n.object = mesh;
        n.material = material.get();

        if(n.d.texture >= 0){
            const auto& asset = description_.textures[size_t(n.d.texture)];
            const bool indexed = !asset.palette.empty();
            auto* data = indexed ? reinterpret_cast<uint16_t*>(const_cast<uint8_t*>(asset.indexData()))
                                 : const_cast<uint16_t*>(asset.pixels.data());
            auto texture = std::make_unique<Renderer::Texture>(asset.width, asset.height, data, asset.has_alpha, asset.alphaColor,
                false, Renderer::WRAP, indexed ? const_cast<uint16_t*>(asset.palette.data()) : nullptr);
            n.texture = texture.get();
            material->diffuseMap = n.texture;
            textures_.push_back(std::move(texture));
        }

        n.applied = Applied::of(n.d);
        applyMaterial(n);
        scene_.addObject(mesh);
        objects_.emplace_back(mesh);
        materials_.push_back(std::move(material));
        ++meshes_;
    }

    // Frees what a freed node holds outside nodes_: its script, Jet object, material, texture and particles.
    void Runtime::releaseNode(Live& n){
        if(vm_->hasInstance(n.d.id)) vm_->detach(n.d.id);

        vm_->forget(n.d.id);

        if(n.object){
            auto& drawn = scene_.getObjects();
            drawn.erase(std::remove(drawn.begin(), drawn.end(), n.object), drawn.end());
            objects_.erase(std::find_if(objects_.begin(), objects_.end(), [&](const auto& o){ return o.get() == n.object; }));
            materials_.erase(std::find_if(materials_.begin(), materials_.end(), [&](const auto& m){ return m.get() == n.material; }));

            if(n.texture)
                textures_.erase(std::find_if(textures_.begin(), textures_.end(), [&](const auto& t){ return t.get() == n.texture; }));
            n.object = nullptr;
            n.material = nullptr;
            n.texture = nullptr;
            --meshes_;
        }

        if(n.particles >= 0){
            particles_[size_t(n.particles)] = {};
            n.particles = -1;
        }
    }

    // Each shape belongs to the nearest body or area above it. Called again whenever the tree changes.
    void Runtime::collectPhysics(){
        shapeOwners_.clear();
        collisionObjects_.clear();

        for(size_t i = 0; i < nodes_.size(); ++i){
            const Live& n = nodes_[i];

            if(!physics::isCollisionShape(n.d.type) || n.freed || !n.inTree) continue;

            //a 2D shape belongs to a 2D body or area, a 3D one to a 3D one.
            const bool flat = physics::isFlat(n.d.type);
            int body = n.parent;

            while(body >= 0 && !(physics::isCollisionObject(nodes_[size_t(body)].d.type) && physics::isFlat(nodes_[size_t(body)].d.type) == flat))
                body = nodes_[size_t(body)].parent;

            if(body < 0) continue;

            shapeOwners_.emplace_back(int32_t(i), body);

            if(std::find(collisionObjects_.begin(), collisionObjects_.end(), body) == collisionObjects_.end())
                collisionObjects_.push_back(body);
        }

        hasPhysics_ = !shapeOwners_.empty();
        physics_->built = physics_->valid = false;
    }

    namespace{
        // Scale and UV transform baked into the vertices, so Jet's bounds match the drawn shape. Normals take the inverse scale.
        void bakeVertices(std::vector<Renderer::Object::Vertex>& vertices, Vec3 s, const float* us, const float* uo){
            if(same(s, {1, 1, 1}) && us[0] == 1 && us[1] == 1 && uo[0] == 0 && uo[1] == 0) return;

            for(auto& vertex : vertices){
                vertex.uv.x = int32_t(std::lround(vertex.uv.x * us[0] + uo[0] * FIXED_POINT_SCALE));
                vertex.uv.y = int32_t(std::lround(vertex.uv.y * us[1] + uo[1] * FIXED_POINT_SCALE));
                vertex.position.x = int32_t(std::lround(vertex.position.x * s.x));
                vertex.position.y = int32_t(std::lround(vertex.position.y * s.y));
                vertex.position.z = int32_t(std::lround(vertex.position.z * s.z));
                auto& normal = vertex.normal;
                const double nx = normal.x / s.x, ny = normal.y / s.y, nz = normal.z / s.z;
                const double length = std::sqrt(nx * nx + ny * ny + nz * nz);

                if(length > 0) normal = {int32_t(nx / length * FIXED_POINT_SCALE), int32_t(ny / length * FIXED_POINT_SCALE), int32_t(nz / length * FIXED_POINT_SCALE)};
            }
        }

        float radiusOf(const std::vector<Renderer::Object::Vertex>& vertices){
            float r2 = 0;

            for(const auto& v : vertices)
                r2 = std::max(r2, float(v.position.x) * v.position.x + float(v.position.y) * v.position.y + float(v.position.z) * v.position.z);
            return std::sqrt(r2);
        }
    }

    // Nodes with the same mesh, world scale and UVs draw one shared copy, like MeshInstance3Ds that share a Godot Mesh.
    // Each keeps its own Jet object for the transform and the material, with the copy as its only instance.
    void Runtime::share(Live& n, Vec3 s){
        const auto q = [](float v){ return int32_t(std::clamp(std::round(v * 10000), -2e9f, 2e9f)); };
        const NodeDesc& d = n.d;
        const bool segmented = d.shape == Shape::Sphere || d.shape == Shape::Cylinder || d.shape == Shape::Capsule || d.shape == Shape::Torus;
        //front-face culling is a reversed winding (see applyMaterial), so it is part of the copy.
        const bool flip = d.cull == CullMode::Front;
        const std::array<int32_t, 11> key{int32_t(d.shape), segmented ? int32_t(d.segments) : 0, d.shape == Shape::Mesh ? d.mesh : -1,
            q(s.x), q(s.y), q(s.z), q(d.uv_scale[0]), q(d.uv_scale[1]), q(d.uv_offset[0]), q(d.uv_offset[1]), flip};
        const auto found = sharedMeshes_.find(key);
        Renderer::Object::SharedMesh mesh = found != sharedMeshes_.end() ? found->second.lock() : nullptr;

        if(!mesh){
            //objects spawned at many different scales would leave an expired entry each
            for(auto it = sharedMeshes_.begin(); it != sharedMeshes_.end();) it = it->second.expired() ? sharedMeshes_.erase(it) : std::next(it);

            std::unique_ptr<Renderer::Object> geometry(makeMesh(d, &sharedMaterial_));
            bakeVertices(geometry->vertices, s, d.uv_scale, d.uv_offset);

            if(flip) for(auto& t : geometry->triangles) std::swap(t.v2, t.v3);

            mesh = Renderer::Object::freezeMesh(std::move(*geometry));
            sharedMeshes_[key] = mesh;
        }

        n.object->addInstance(mesh, {}, n.material);
        n.object->calculateBoundingBox();
        n.flipped = flip;
        n.bakedScale = s;
        n.jetRadius = radiusOf(mesh->vertices);
    }

    void Runtime::bake(Live& n, Vec3 s){
        auto& object = *n.object;
        const bool first = same(n.bakedScale, {0, 0, 0});

        if(first && object.vertices.empty()){
            share(n, s);
            return;
        }

        //a script changed the scale or the UVs: the node leaves its shared copy for geometry of its own.
        //that geometry stays unscaled in baseVertices for the next changes; most meshes never get here.
        if(!object.instances.empty()){
            object.instances.clear();
            std::unique_ptr<Renderer::Object> fresh(makeMesh(n.d, n.material));
            object.triangles = std::move(fresh->triangles);

            if(n.d.cull == CullMode::Front) for(auto& t : object.triangles) std::swap(t.v2, t.v3);

            n.flipped = n.d.cull == CullMode::Front;
            n.baseVertices = std::move(fresh->vertices);
        } else if(n.baseVertices.empty() && !first){
            std::unique_ptr<Renderer::Object> fresh(makeMesh(n.d, n.material));
            n.baseVertices = std::move(fresh->vertices);
        }

        if(!n.baseVertices.empty()) object.vertices = n.baseVertices;

        bakeVertices(object.vertices, s, n.d.uv_scale, n.d.uv_offset);
        object.calculateBoundingBox();
        n.bakedScale = s;
        n.jetRadius = radiusOf(object.vertices);
    }

    namespace{
        uint16_t tint565(uint16_t c, uint32_t albedo){
            const uint32_t r = (c >> 11) & 31, g = (c >> 5) & 63, b = c & 31;
            return uint16_t(((r * ((albedo >> 16) & 255) + 127) / 255) << 11 | ((g * ((albedo >> 8) & 255) + 127) / 255) << 5
                            | ((b * (albedo & 255) + 127) / 255));
        }
        bool sameMaterial(const NodeDesc& a, const Runtime::Applied& b){
            return a.color == b.color && a.shading == b.shading && a.alpha == b.alpha && a.specular == b.specular && a.shininess == b.shininess
                && a.diffuse == b.diffuse
                && a.bilinear == b.bilinear && a.repeat == b.repeat && a.perspective == b.perspective && a.cull == b.cull && a.billboard == b.billboard
                && a.visibility_begin == b.visibility[0] && a.visibility_begin_margin == b.visibility[1] && a.visibility_end == b.visibility[2]
                && a.visibility_end_margin == b.visibility[3] && a.visibility_fade == b.visibility_fade;
        }
        bool sameUv(const NodeDesc& a, const Runtime::Applied& b){
            return a.uv_scale[0] == b.uv_scale[0] && a.uv_scale[1] == b.uv_scale[1] && a.uv_offset[0] == b.uv_offset[0] && a.uv_offset[1] == b.uv_offset[1];
        }
    }

    void Runtime::applyMaterial(Live& n){
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

        if(n.texture){
            //albedo multiplies the texture, as in Godot. Palette textures are tinted through the palette, which is cheap.
            //Jet only filters RGB565 data bilinearly, so bilinear filtering expands a palette texture to pixels.
            //the colour key is left alone, and a tinted colour that lands on it is nudged by one bit.
            const auto& asset = description_.textures[size_t(d.texture)];
            const bool indexed = !asset.palette.empty();
            const bool expand = indexed && d.bilinear && BILINEAR_FILTER;
            auto tint = [&](uint16_t c){
                if(d.color == 0xffffff || (asset.has_alpha && c == asset.alphaColor)) return c;

                c = tint565(c, d.color);
                return asset.has_alpha && c == asset.alphaColor ? uint16_t(c ^ 1) : c;
            };

            if(expand){
                std::vector<uint16_t> palette(asset.palette.size());

                for(size_t i = 0; i < palette.size(); ++i) palette[i] = tint(asset.palette[i]);

                n.tinted.resize(size_t(asset.width) * asset.height);

                for(size_t i = 0; i < n.tinted.size(); ++i) n.tinted[i] = palette[asset.indexData()[i]];
            } else if(d.color != 0xffffff){
                const std::vector<uint16_t>& source = indexed ? asset.palette : asset.pixels;
                n.tinted.resize(source.size());

                for(size_t i = 0; i < source.size(); ++i) n.tinted[i] = tint(source[i]);
            } else {
                n.tinted.clear();
            }

            if(indexed && !expand){
                n.texture->data = reinterpret_cast<uint16_t*>(const_cast<uint8_t*>(asset.indexData()));
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
        //Jet's CULL_FRONTFACES draws nothing, so front culling is a reversed winding plus back-face culling.
        //a node drawing a shared copy has no triangles yet: share() picks a copy with the right winding.
        const bool flip = d.cull == CullMode::Front;

        if(flip != n.flipped && !o.triangles.empty()){
            for(auto& t : o.triangles) std::swap(t.v2, t.v3);

            n.flipped = flip;
        }

        o.cullingMode = d.cull == CullMode::Disabled ? Renderer::CullingMode::NO_CULLING : Renderer::CullingMode::CULL_BACKFACES;
        o.isBillboard = d.billboard;
        //only Background and OnTop get their own draw bands; translucent and additive stay depth-sorted with the rest.
        o.noWriteZBuffer = d.draw_order == DrawOrder::Background;
        o.ignoreZBuffer = d.draw_order == DrawOrder::OnTop;
        //without a z-buffer (device) triangles in one depth bucket keep scene order, e.g. a far wheel over the bodywork.
        o.preciseDepthSort = d.draw_order == DrawOrder::Default;
        //Godot's visibility range, in Jet units like the cm stored: with fade it dissolves from end to end + margin and
        //appears from begin - margin to begin; without it is cut at end and begin. Jet skips the object outside the range.
        const int32_t end = d.visibility_end, begin = d.visibility_begin;
        o.fadeNear = end;
        o.fadeFar = end > 0 && d.visibility_fade ? end + d.visibility_end_margin : end;
        o.appearFar = begin;
        o.appearNear = begin > 0 && d.visibility_fade ? std::max<int32_t>(0, begin - int32_t(d.visibility_begin_margin)) : begin;
        n.applied = Applied::of(d);
    }

    void Runtime::syncObject(Live& n){
        //a script may have changed frame or flips
        if(n.d.type == NodeType::Sprite3D) spriteUv(n.d);

        if(n.synced && n.syncedVisible == n.worldVisible && n.syncedWorld == n.world && sameMaterial(n.d, n.applied)
            && sameUv(n.d, n.applied)) return;
        n.synced = true;
        n.syncedWorld = n.world;
        n.syncedVisible = n.worldVisible;
        Vec3 scale;
        const Basis rotation = n.world.basis.orthonormalized(&scale);
        bool shown = n.worldVisible;

        if(n.d.type == NodeType::Sprite3D){
            //the quad takes the size of one frame, pixel_size metres per texel; no texture, nothing drawn.
            shown = shown && n.d.texture >= 0;

            if(n.d.texture >= 0){
                const auto& t = description_.textures[size_t(n.d.texture)];
                scale.x *= float(t.width) / float(n.d.hframes) * n.d.pixel_size;
                scale.y *= float(t.height) / float(n.d.vframes) * n.d.pixel_size;
            }
        }

        if(!same(scale, n.bakedScale) || (n.object->vertices.empty() && n.object->instances.empty()) || !sameUv(n.d, n.applied)){
            n.applied.uv_scale[0] = n.d.uv_scale[0]; n.applied.uv_scale[1] = n.d.uv_scale[1];
            n.applied.uv_offset[0] = n.d.uv_offset[0]; n.applied.uv_offset[1] = n.d.uv_offset[1];
            bake(n, scale);
        }

        if(!sameMaterial(n.d, n.applied)) applyMaterial(n);

        const Vec3 e = rotation.mirroredZ().eulerZyx();
        n.object->setRotation(degreesInt(e.x), degreesInt(e.y), degreesInt(e.z));
        n.object->setPosition(toJet(n.world.origin));
        n.object->enabled = shown;
    }

    // Physics ticks pass sync = false: world transforms only, the Jet objects are synced later.
    void Runtime::updateTransforms(bool sync){
        for(auto& n : nodes_){
            const Live* parent = n.parent >= 0 && isNode3d(nodes_[n.parent].d.type) ? &nodes_[n.parent] : nullptr;

            if(isNode3d(n.d.type)){
                if(!n.localValid || n.d.position != n.localPosition || n.d.rotation != n.localRotation || n.d.scale != n.localScale){
                    n.local = localTransform(n.d.position, n.d.rotation, n.d.scale);
                    n.localPosition = n.d.position;
                    n.localRotation = n.d.rotation;
                    n.localScale = n.d.scale;
                    n.localValid = true;
                }

                n.world = parent ? parent->world * n.local : n.local;
                n.worldVisible = n.d.visible && !n.freed && n.inTree && (!parent || parent->worldVisible);
            } else if(isNode2d(n.d.type)){
                //a 2D node inherits from a 2D parent; under anything else it starts a canvas of its own, as in Godot.
                const Live* parent2d = n.parent >= 0 && isNode2d(nodes_[n.parent].d.type) ? &nodes_[n.parent] : nullptr;

                if(!n.localValid || n.d.position != n.localPosition || n.d.rotation != n.localRotation || n.d.scale != n.localScale){
                    const Vec3 position{n.d.position.x, n.d.position.y, 0}, rotation{0, 0, n.d.rotation.z}, scale{n.d.scale.x, n.d.scale.y, 1};
                    n.local = localTransform(position, rotation, scale);
                    n.localPosition = n.d.position;
                    n.localRotation = n.d.rotation;
                    n.localScale = n.d.scale;
                    n.localValid = true;
                }

                n.world = parent2d ? parent2d->world * n.local : n.local;
                n.worldVisible = n.d.visible && !n.freed && n.inTree && (!parent2d || parent2d->worldVisible);
                n.canvasZ = int16_t(std::clamp(n.d.z_index + (parent2d ? parent2d->canvasZ : 0), -4096, 4096));
            } else if(is_control(n.d.type)){
                //as in Godot, a Control sits relative to its parent Control, and any hidden ancestor hides it.
                const bool nested = n.parent >= 0 && is_control(nodes_[n.parent].d.type);
                n.world = {};
                n.world.origin = nested ? nodes_[n.parent].world.origin + n.d.position : n.d.position;
                n.worldVisible = n.d.visible && !n.freed && n.inTree && (n.parent < 0 || nodes_[n.parent].worldVisible);
            } else {
                n.world = {};
                n.worldVisible = !n.freed && n.inTree;
            }

            if(sync && n.object) syncObject(n);
        }

        transformsStale_ = false;
    }

    void Runtime::applyEnvironment(){
        const Live* environment = nullptr;
        const Live* light = nullptr;

        for(const auto& n : nodes_){
            if(n.freed || !n.inTree) continue;

            if(!environment && n.d.type == NodeType::WorldEnvironment) environment = &n;

            if(!light && n.d.type == NodeType::DirectionalLight3D && n.worldVisible) light = &n;
        }

        scene_.setBackcolor(rgb565(environment ? environment->d.more().background : 0x161c28));
        ambient_.color = colorOf(environment ? environment->d.more().ambient : 0x4e586e);
        //depth fog (JetConfig.hpp): triangles fade into the background from fog_begin to fog_end
        const bool fog = environment && environment->d.more().fog_enabled;
        const float fogBegin = fog ? std::clamp(environment->d.more().fog_begin * JetUnits, 0.0f, 1e9f) : 0;
        jetFogNear = fog ? int(fogBegin) : std::numeric_limits<int>::max();
        jetFogFar = fog ? std::max(jetFogNear + 1, int(std::clamp(environment->d.more().fog_end * JetUnits, 0.0f, 1e9f)))
                        : std::numeric_limits<int>::max();

        if(!light){ scene_.setDirectionalLight(nullptr); return; }
        //Godot lights shine along -Z; Jet wants the direction towards the light.
        const Vec3 toward = light->world.basis.orthonormalized().column(2).normalized();
        sun_.worldLightDir = {int32_t(std::lround(toward.x * FIXED_POINT_SCALE)), int32_t(std::lround(toward.y * FIXED_POINT_SCALE)),
                              int32_t(std::lround(-toward.z * FIXED_POINT_SCALE))};
        sun_.lightDir = sun_.worldLightDir;
        sun_.color = colorOf(light->d.more().light_color);
        sun_.intensity = uint16_t(std::clamp(light->d.more().energy * 220.0f, 0.0f, 255.0f));
        scene_.setDirectionalLight(&sun_);
    }

    void Runtime::applyCamera(){
        const Live* camera = nullptr;

        if(playing_ && !editorView_){
            for(const auto& n : nodes_) if(!n.freed && n.inTree && n.d.type == NodeType::Camera3D && n.d.more().current){ camera = &n; break; }

            if(!camera) for(const auto& n : nodes_) if(!n.freed && n.inTree && n.d.type == NodeType::Camera3D){ camera = &n; break; }
        }

        Basis basis;
        Vec3 position;
        float fov = editorFov_, near = 0.05f, far = 500;

        if(camera){
            basis = camera->world.basis.orthonormalized();
            position = camera->world.origin;
            fov = camera->d.more().fov; near = camera->d.more().near; far = camera->d.more().far;
        } else {
            basis = Basis::from_euler(editorRotation_);
            position = editorPosition_;
        }

        viewBasis_ = basis;
        viewFov_ = fov;
        //Jet's camera basis is Ry * Rx * Rz in its own (mirrored) space.
        const Vec3 e = basis.mirroredZ().euler();
        camera_.setPosition(toJet(position));
        camera_.rotation.x = e.x;
        camera_.rotation.y = e.y;
        camera_.rotation.z = e.z;
        //Godot's FOV is vertical; Jet's projection factor is derived from the width.
        const float horizontal = 2 * std::atan(float(width_) / height_ * std::tan(radians(fov) * 0.5f));
        camera_.setFOV(degrees(horizontal), width_);
        camera_.nearPlane = std::clamp(int32_t(near * JetUnits), int32_t(5), int32_t(1000));
        camera_.farPlane = std::clamp(int32_t(far * JetUnits), camera_.nearPlane + 1, int32_t(60000));
    }

    void Runtime::set_editor_camera(Vec3 position, Vec3 rotation, float fov){
        if(!position.finite() || !rotation.finite() || !std::isfinite(fov)) return;

        editorPosition_ = position;
        editorRotation_ = rotation;
        editorFov_ = std::clamp(fov, 1.0f, 179.0f);
    }

    void Runtime::log(std::string text, bool error, std::string path, int line){
        if(log_.size() >= 512) log_.erase(log_.begin(), log_.begin() + 256);

        log_.push_back({std::move(text), error, std::move(path), line});
    }

    std::vector<LogLine> Runtime::take_log(){
        std::vector<LogLine> out;
        out.swap(log_);
        return out;
    }

    void Runtime::flushScriptErrors(){
        for(auto& e : vm_->takeErrors()){
            std::string where = e.path + ":" + std::to_string(e.line);

            if(!e.function.empty()) where += " in " + e.function + "()";

            log("Script error: " + e.message + "  (" + where + ")", true, e.path, e.line);

            for(size_t i = 1; i < e.stack.size(); ++i)
                log("    at " + e.stack[i].function + "() " + e.stack[i].path + ":" + std::to_string(e.stack[i].line), true,
                    e.stack[i].path, e.stack[i].line);
        }
    }

    namespace{
        // Scene overrides come from JSON, which has no Vector3 or Color.
        bool coerce(const jetscript::Value& in, jetscript::Type type, jetscript::Value& out){
            using jetscript::Type;
            const Type from = in.type();

            if(type == Type::Nil || from == type){ out = in; return true; }

            if(type == Type::Float && from == Type::Int){ out = jetscript::Value(in.asReal()); return true; }

            //asInt() saturates: a plain cast of a huge float is undefined
            if(type == Type::Int && from == Type::Float && std::isfinite(in.asReal())){ out = jetscript::Value(in.asInt()); return true; }

            if(type == Type::Bool && from == Type::Int){ out = jetscript::Value(in.asInt() != 0); return true; }

            if((type == Type::Vector3 || type == Type::Color) && from == Type::Array && in.arraySize() == 3){
                float v[3];

                for(int i = 0; i < 3; ++i){
                    const auto item = in.arrayGet(size_t(i));

                    if(!item.isNumber()) return false;

                    v[i] = item.asReal();
                }

                out = type == Type::Vector3 ? jetscript::Value(jetscript::Vector3{v[0], v[1], v[2]}) : jetscript::Value(jetscript::Color{v[0], v[1], v[2]});
                return true;
            }

            if(type == Type::Vector2 && from == Type::Array && in.arraySize() == 2){
                const auto x = in.arrayGet(0), y = in.arrayGet(1);

                if(!x.isNumber() || !y.isNumber()) return false;

                out = jetscript::Value(jetscript::Vector2{x.asReal(), y.asReal()});
                return true;
            }

            if(type == Type::Color && from == Type::Int){
                const auto c = uint32_t(in.asInt());
                out = jetscript::Value(jetscript::Color{((c >> 16) & 255) / 255.0f, ((c >> 8) & 255) / 255.0f, (c & 255) / 255.0f});
                return true;
            }

            return false;
        }
    }

    bool Runtime::start(std::vector<ScriptDiagnostic>& errors){
        errors.clear();
        std::string error;
        playing_ = false;
        vm_->clear();
        vm_->takeErrors();
        killTweens(false);
        sceneNodes_ = nodesOf(description_.path, &sceneConnections_);
        currentScene_ = description_.path;
        pendingScene_.clear();
        build(error);
        //compile every scene's scripts now, so errors show up at Play.
        std::vector<std::shared_ptr<const jetscript::Script>> compiled(description_.scripts.size());
        //a package keeps only the scripts its scenes and autoloads use.
        std::vector<bool> used(description_.scripts.size(), pack_ != nullptr);

        for(const auto& a : description_.autoloads) used[size_t(a.script)] = true;

        for(const auto& n : description_.nodes) if(n.script >= 0) used[n.script] = true;

        for(const auto& scene : description_.scenes)
            for(const auto& n : scene.nodes) if(n.script >= 0) used[n.script] = true;

        for(size_t i = 0; i < compiled.size(); ++i){
            if(!used[i]) continue;

            const auto& file = description_.scripts[i];
            jetscript::CompileResult result;

            if(!file.bytecode.empty()){
                result = jetscript::loadBytecode(file.bytecode.data(), file.bytecode.size(), *host_);
            } else {
#ifndef JETSCRIPT_NO_COMPILER
                result = jetscript::compile(file.source, file.path, *host_);
#else
                result.errors.push_back({0, 0, "Script without bytecode: this runtime does not include the compiler."});
#endif
            }

            for(const auto& d : result.errors) errors.push_back({file.path, d.line, d.column, d.message});

            compiled[i] = result.script;
        }

        if(!errors.empty()) return false;

        compiled_ = std::move(compiled);
        playing_ = true;
        editorView_ = false;
        input_ = previousInput_ = physicsPreviousInput_ = 0;
        startAutoloads();
        enterScene();
        return true;
    }

    // Like Godot, the autoloads are ready before the first scene.
    // Scripts may add nodes, which can move nodes_: loops that call them go by id, never keeping a Live&.
    void Runtime::startAutoloads(){
        std::vector<uint32_t> ids;

        for(const auto& n : nodes_)
            if(n.d.id >= AutoloadIdBase && n.d.script >= 0) ids.push_back(n.d.id);

        for(uint32_t id : ids){
            const int i = indexOf(id);

            if(i >= 0 && vm_->attach(id, compiled_[size_t(nodes_[size_t(i)].d.script)])) vm_->ready(id);
        }

        flushScriptErrors();
    }

    void Runtime::enterScene(){
        physicsTime_ = 0;
        startParticles();

        for(auto& n : nodes_)
            if(physics::isRigid(n.d.type)){
                n.editMotion().velocity = n.d.more().linear_velocity;
                n.editMotion().angular = n.d.more().angular_velocity;
            }

        std::vector<uint32_t> ids;

        for(const auto& n : nodes_)
            if(n.d.script >= 0) ids.push_back(n.d.id);

        //autoloads are already running
        for(uint32_t id : ids) attachScript(id);

        //as in Godot, the scene's connections are made before _ready().
        connectScene(sceneConnections_, sceneIds_);
        std::vector<SceneConnection>().swap(sceneConnections_);
        std::vector<uint32_t>().swap(sceneIds_);
        flushScriptErrors();
        //_ready(): children before parents, siblings in tree order, as in Godot.
        if(!nodes_.empty()) enterTree(0);

        freePending();
        updateTransforms();

        for(int32_t i : collisionObjects_) nodes_[size_t(i)].editMotion().lastOrigin = nodes_[size_t(i)].world.origin;

        applyEnvironment();
        applyCamera();
    }

    // Creates the script instance (its member initializers run) and applies the exported values set in the scene.
    void Runtime::attachScript(uint32_t id){
        int i = indexOf(id);

        if(i < 0 || nodes_[size_t(i)].d.script < 0 || vm_->hasInstance(id)) return;

        const auto& script = compiled_[size_t(nodes_[size_t(i)].d.script)];

        if(!vm_->attach(id, script)) return;

        //the initializers may have added nodes: look the node up again.
        i = indexOf(id);

        if(i < 0) return;

        const NodeDesc& d = nodes_[size_t(i)].d;

        for(const auto& o : d.overrides){
            const auto& exports = script->exports();
            const auto found = std::find_if(exports.begin(), exports.end(), [&](const auto& e){ return e.name == o.name; });

            //the variable was removed from the script
            if(found == exports.end()) continue;

            jetscript::Value value;

            if(!coerce(o.value, found->type, value) || !vm_->setMember(id, o.name, value))
                log("Invalid value for " + d.name + "." + o.name, true, script->path(), found->line);
        }
    }

    // The nodes of a scene, copied from the description or read again from the package.
    bool Runtime::sceneDescs(std::string_view path, std::vector<NodeDesc>& out, std::vector<SceneConnection>& connections) const{
        const std::vector<NodeDesc>* nodes = nullptr;
        const std::vector<SceneConnection>* links = nullptr;
        uint32_t offset = 0;

        if(path == description_.path){ nodes = &description_.nodes; links = &description_.connections; offset = description_.nodesOffset; }

        for(const auto& scene : description_.scenes)
            if(scene.path == path){ nodes = &scene.nodes; links = &scene.connections; offset = scene.offset; }

        if(!nodes) return false;

        if(!nodes->empty() || !pack_){
            out = *nodes;
            connections = *links;
            return !out.empty();
        }

        std::string error;
        return decodeNodes(pack_, packSize_, offset, out, connections, error);
    }

    void Runtime::connectScene(const std::vector<SceneConnection>& connections, const std::vector<uint32_t>& ids){
        auto name = [&](uint32_t id){
            const int i = indexOf(id);
            return i >= 0 ? nodes_[size_t(i)].d.name : std::string("?");
        };

        for(const auto& c : connections){
            if(size_t(c.from) >= ids.size() || size_t(c.to) >= ids.size()) continue;

            const uint32_t from = ids[size_t(c.from)], to = ids[size_t(c.to)];
            std::string error;

            if(!vm_->connect(from, c.signal, to, c.method, c.flags, error))
                log("Signal connection " + name(from) + "." + c.signal + " -> " + name(to) + "." + c.method + "(): " + error, true);
        }
    }

    // PackedScene.instantiate(): the scene's nodes outside the tree, with their scripts attached but not ready yet.
    uint32_t Runtime::instantiate(std::string_view path, std::string& error){
        std::vector<NodeDesc> descs;
        std::vector<SceneConnection> connections;

        if(!sceneDescs(path, descs, connections)){
            error = "Failed to instantiate \"" + std::string(path) + "\": no such scene in the package.";
            return 0;
        }

        const size_t first = nodes_.size();
        std::vector<uint32_t> ids;

        for(auto& desc : descs){
            Live n;
            n.d = std::move(desc);
            n.d.id = nextInstanceId_++;
            n.parent = n.d.parent >= 0 ? int32_t(first) + n.d.parent : -1;
            n.inTree = false;
            const int32_t index = int32_t(nodes_.size());

            if(n.parent >= 0) nodes_[size_t(n.parent)].children.push_back(index);

            index_[n.d.id] = index;
            ids.push_back(n.d.id);
            nodes_.push_back(std::move(n));
            createVisual(index);
        }

        //scripts once the whole subtree exists, since their initializers may look for children.
        for(uint32_t id : ids) attachScript(id);

        connectScene(connections, ids);
        flushScriptErrors();
        return ids.front();
    }

    // Node.add_child(): the child (with its subtree) joins the parent; if the parent is in the tree, it enters it too.
    bool Runtime::add_child(int32_t parent, int32_t child, std::string& error){
        Live& c = nodes_[size_t(child)];
        Live& p = nodes_[size_t(parent)];

        if(c.parent >= 0){
            error = "Can not add child \"" + c.d.name + "\" to \"" + p.d.name + "\", already has a parent \"" +
                    nodes_[size_t(c.parent)].d.name + "\".";
            return false;
        }

        if(c.inTree || c.d.id >= AutoloadIdBase || child == 0){
            error = "Can not add child \"" + c.d.name + "\": it is a scene root or an autoload.";
            return false;
        }

        for(int32_t i = parent; i >= 0; i = nodes_[size_t(i)].parent)
            if(i == child){
                error = "Can not add child \"" + c.d.name + "\" to \"" + p.d.name + "\": it is one of its ancestors.";
                return false;
            }

        //siblings need distinct names (paths go by name): Bullet, Bullet2, Bullet3...
        const std::string base = c.d.name;

        for(int n = 2;; ++n){
            const bool taken = std::any_of(p.children.begin(), p.children.end(),
                                           [&](int32_t s){ return nodes_[size_t(s)].d.name == c.d.name; });

            if(!taken) break;

            c.d.name = base + std::to_string(n);
        }

        c.parent = parent;
        p.children.push_back(child);

        if(child < parent) reorderNeeded_ = true;

        if(p.inTree) enterTree(child);

        return true;
    }

    // Node.remove_child(): the child stays alive outside the tree, as in Godot; add_child() can put it back.
    bool Runtime::remove_child(int32_t parent, int32_t child, std::string& error){
        Live& p = nodes_[size_t(parent)];
        const auto found = std::find(p.children.begin(), p.children.end(), child);

        if(found == p.children.end()){
            error = "Cannot remove child \"" + nodes_[size_t(child)].d.name + "\" as it is not a child of \"" + p.d.name + "\".";
            return false;
        }

        p.children.erase(found);
        nodes_[size_t(child)].parent = -1;

        std::vector<int32_t> stack{child};

        while(!stack.empty()){
            Live& n = nodes_[size_t(stack.back())];
            stack.pop_back();
            n.inTree = false;
            stack.insert(stack.end(), n.children.begin(), n.children.end());
        }

        collectPhysics();
        return true;
    }

    // A subtree joins the scene: particles, the initial velocity of rigid bodies, physics, then _ready() with
    // children before parents, as in Godot.
    void Runtime::enterTree(int32_t root){
        std::vector<uint32_t> ready;
        std::vector<std::pair<int32_t, size_t>> stack{{root, 0}};

        while(!stack.empty()){
            auto& [index, child] = stack.back();
            Live& n = nodes_[size_t(index)];

            if(child == 0){
                if(!n.inTree && physics::isRigid(n.d.type)){
                    n.editMotion().velocity = n.d.more().linear_velocity;
                    n.editMotion().angular = n.d.more().angular_velocity;
                }

                n.inTree = true;

                if(playing_ && n.d.type == NodeType::CPUParticles3D && n.particles < 0) startParticle(n);

                if(playing_ && n.d.type == NodeType::AnimatedSprite2D && n.d.more().autoplay && !n.d.more().playing) playAnimation(n, {});

                if(playing_ && n.d.type == NodeType::Timer && n.d.more().autoplay && !n.d.more().playing) startTimer(n);

                if(playing_ && n.d.type == NodeType::AnimationPlayer && !n.d.more().animation.empty() && !n.d.more().playing)
                    playPlayer(n, n.d.more().animation, 1, false);
            }

            if(child < n.children.size()){
                const int32_t next = n.children[child++];
                stack.push_back({next, 0});
                continue;
            }

            if(!n.freed && vm_->hasInstance(n.d.id)) ready.push_back(n.d.id);

            stack.pop_back();
        }

        collectPhysics();

        //_ready() may add nodes, which can move nodes_: go by id.
        for(uint32_t id : ready){
            const int i = indexOf(id);

            if(i >= 0 && !nodes_[size_t(i)].freed) vm_->ready(id);
        }

        flushScriptErrors();
    }

    // "idle 0-3 8, run 4-9 12, die 10-14 10 once": name, first-last frame (or one frame), frames per second, "once".
    bool parseAnimations(std::string_view text, int frames, std::vector<SpriteAnimation>& out){
        out.clear();
        size_t start = 0;

        while(start <= text.size()){
            size_t end = text.find_first_of(",\n", start);

            if(end == std::string_view::npos) end = text.size();

            std::vector<std::string_view> words;

            for(size_t i = start; i < end;){
                while(i < end && text[i] == ' ') ++i;

                size_t j = i;

                while(j < end && text[j] != ' ') ++j;

                if(j > i) words.push_back(text.substr(i, j - i));

                i = j;
            }

            start = end + 1;

            if(words.empty()) continue;

            if(words.size() < 2 || words.size() > 4 || out.size() >= 64) return false;

            SpriteAnimation a;
            a.name = std::string(words[0]);
            const std::string range(words[1]);
            char* rest = nullptr;
            a.first = int(std::strtol(range.c_str(), &rest, 10));
            a.last = *rest == '-' ? int(std::strtol(rest + 1, &rest, 10)) : a.first;

            if(*rest || a.first < 0 || a.last < a.first || a.last >= frames) return false;

            if(words.size() >= 3){
                const std::string fps(words[2]);
                a.fps = std::strtof(fps.c_str(), &rest);

                if(*rest || !std::isfinite(a.fps) || a.fps <= 0 || a.fps > 240) return false;
            }

            if(words.size() == 4){
                if(words[3] != "once") return false;

                a.loop = false;
            }

            out.push_back(std::move(a));
        }

        return true;
    }

    // AnimatedSprite2D.play(): an empty name plays the current animation, or the first one.
    bool Runtime::playAnimation(Live& n, std::string_view name){
        auto& e = n.d.extras();
        std::vector<SpriteAnimation> animations;
        parseAnimations(e.animations, n.d.hframes * n.d.vframes, animations);

        if(name.empty()) name = !e.animation.empty() ? std::string_view(e.animation) : std::string_view();

        const auto found = std::find_if(animations.begin(), animations.end(), [&](const auto& a){ return name.empty() || a.name == name; });

        if(found == animations.end()) return false;

        if(e.animation != found->name || !e.playing){
            e.animation = found->name;
            e.animTime = 0;
            n.d.frame = uint16_t(found->first);
        }

        e.animFirst = uint16_t(found->first);
        e.animLast = uint16_t(found->last);
        e.animFps = found->fps;
        e.animLoop = found->loop;
        e.playing = true;
        return true;
    }

    // A "once" animation stops on its last frame.
    void Runtime::updateAnimations(float dt){
        for(auto& n : nodes_){
            if((n.d.type != NodeType::AnimatedSprite2D && n.d.type != NodeType::Timer && n.d.type != NodeType::AnimationPlayer)
                || n.freed || !n.inTree || !n.d.more().playing)
                continue;

            if(n.d.type == NodeType::AnimationPlayer){
                advancePlayer(n, dt);
                continue;
            }

            auto& e = n.d.extras();

            //Godot's Timer: at most one timeout per frame, a repeating one keeps the time it overran by.
            if(n.d.type == NodeType::Timer){
                if(e.paused) continue;

                n.cycle -= dt;

                if(n.cycle >= 0) continue;

                if(e.one_shot){
                    e.playing = false;
                    n.cycle = 0;
                } else {
                    n.cycle += e.lifetime;
                }

                deferSignal(n.d.id, "timeout");
                continue;
            }

            const int count = int(e.animLast) - int(e.animFirst) + 1;

            if(count <= 0) continue;

            e.animTime += dt * e.speed_scale;

            //a looping animation keeps its time within one cycle, so it does not grow for ever
            if(e.animLoop && e.animFps > 0){
                const float cycle = float(count) / e.animFps;
                e.animTime = std::fmod(e.animTime, cycle);

                if(e.animTime < 0) e.animTime += cycle;
            }

            const float position = std::floor(e.animTime * e.animFps);
            int step = std::isfinite(position) ? int(std::clamp(position, -1.0f, float(count))) : 0;

            if(e.animLoop){
                step %= count;

                if(step < 0) step += count;
            } else if(step >= count || step < 0){
                step = step < 0 ? 0 : count - 1;
                e.playing = false;
                deferSignal(n.d.id, "animation_finished");
            }

            n.d.frame = uint16_t(e.animFirst + step);
        }

        runDeferred();
    }

    void Runtime::startTimer(Live& n, float wait_time){
        auto& e = n.d.extras();

        if(wait_time > 0) e.lifetime = wait_time;

        e.playing = true;
        n.cycle = e.lifetime;
    }

    // a signal nobody listens to is not even queued
    void Runtime::deferSignal(uint32_t source, const char* signal, std::vector<jetscript::Value> args){
        if(vm_->has_connections(source)) deferred_.push_back({source, signal, {}, std::move(args)});
    }

    void Runtime::runDeferred(){
        if(deferred_.empty()) return;

        std::vector<Deferred> pending;
        pending.swap(deferred_);

        for(const auto& d : pending){
            const jetscript::Value* args = d.args.empty() ? nullptr : d.args.data();

            if(d.signal) vm_->emit_signal(d.source, d.signal, args, int(d.args.size()));
            else vm_->callCallable(d.callable, args, int(d.args.size()));
        }

        flushScriptErrors();
    }

    bool Runtime::changeScene(std::string_view path){
        bool found = path == description_.path;

        for(const auto& scene : description_.scenes) found = found || scene.path == path;

        if(!found || !playing_) return false;

        pendingScene_ = std::string(path);
        return true;
    }

    void Runtime::switchScene(){
        const std::string path = std::move(pendingScene_);
        pendingScene_.clear();
        std::vector<SceneConnection> connections;
        const std::vector<NodeDesc>* target = nodesOf(path, &connections);

        if(!target) return;

        for(const auto& n : nodes_){
            if(n.d.id >= AutoloadIdBase) continue;

            if(vm_->hasInstance(n.d.id)) vm_->detach(n.d.id);

            //the next scene's nodes may get the same ids
            vm_->forget(n.d.id);
        }

        killTweens(true);
        flushScriptErrors();
        sceneNodes_ = target;
        sceneConnections_ = std::move(connections);
        currentScene_ = path;
        std::string error;
        build(error);
        enterScene();
    }

    void Runtime::stop(){
        if(!playing_) return;

        playing_ = false;
        vm_->clear();
        killTweens(false);
        flushScriptErrors();
        sceneNodes_ = nodesOf(description_.path);
        currentScene_ = description_.path;
        pendingScene_.clear();
        compiled_.clear();
        std::string error;
        build(error);
        editorView_ = true;
    }

    // queue_free() is deferred to the end of the frame, as in Godot. The freed nodes, their subtrees included,
    // release what they hold and leave nodes_, so spawning and freeing does not grow it forever.
    void Runtime::freePending(){
        if(!pendingFree_ && !reorderNeeded_) return;

        if(pendingFree_){
            pendingFree_ = false;
            std::vector<int32_t> stack;

            for(size_t i = 0; i < nodes_.size(); ++i)
                if(nodes_[i].freeRequested) stack.push_back(int32_t(i));

            while(!stack.empty()){
                Live& n = nodes_[size_t(stack.back())];
                stack.pop_back();

                if(n.freed) continue;

                n.freed = true;
                stack.insert(stack.end(), n.children.begin(), n.children.end());
            }

            for(auto& n : nodes_)
                if(n.freed) releaseNode(n);
        }

        compactNodes();
    }

    // Drops the freed nodes and keeps parents before children (updateTransforms() needs it), then fixes every index.
    void Runtime::compactNodes(){
        std::vector<int32_t> order;
        order.reserve(nodes_.size());

        if(reorderNeeded_){
            //a node went under one that comes later: walk the trees again, roots in their current order.
            std::vector<int32_t> stack;

            for(size_t i = nodes_.size(); i-- > 0;)
                if(nodes_[i].parent < 0) stack.push_back(int32_t(i));

            while(!stack.empty()){
                const int32_t i = stack.back();
                stack.pop_back();

                if(nodes_[size_t(i)].freed) continue;

                order.push_back(i);
                const auto& children = nodes_[size_t(i)].children;
                stack.insert(stack.end(), children.rbegin(), children.rend());
            }
        } else {
            for(size_t i = 0; i < nodes_.size(); ++i)
                if(!nodes_[i].freed) order.push_back(int32_t(i));

            if(order.size() == nodes_.size()) return;
        }

        std::vector<int32_t> movedTo(nodes_.size(), -1);

        for(size_t k = 0; k < order.size(); ++k) movedTo[size_t(order[k])] = int32_t(k);

        if(reorderNeeded_){
            //in place, one cycle of the permutation at a time: a second array would need as much RAM again.
            //source[k] is the node that goes to slot k; the freed ones go last.
            std::vector<int32_t> source(order);

            for(size_t i = 0; i < nodes_.size(); ++i)
                if(movedTo[i] < 0) source.push_back(int32_t(i));

            std::vector<bool> placed(source.size(), false);

            for(size_t k = 0; k < source.size(); ++k){
                if(placed[k]) continue;

                placed[k] = true;

                if(size_t(source[k]) == k) continue;

                Live held = std::move(nodes_[k]);
                size_t j = k;

                while(size_t(source[j]) != k){
                    nodes_[j] = std::move(nodes_[size_t(source[j])]);
                    j = size_t(source[j]);
                    placed[j] = true;
                }

                nodes_[j] = std::move(held);
            }

            nodes_.resize(order.size());
        } else {
            //same order minus the freed: in place, no second array.
            for(size_t k = 0; k < order.size(); ++k)
                if(size_t(order[k]) != k) nodes_[k] = std::move(nodes_[size_t(order[k])]);

            nodes_.resize(order.size());
        }

        reorderNeeded_ = false;

        index_.clear();

        for(size_t k = 0; k < nodes_.size(); ++k){
            Live& n = nodes_[k];
            index_[n.d.id] = int32_t(k);
            n.parent = n.parent >= 0 ? movedTo[size_t(n.parent)] : -1;
            n.children.clear();

            //found again at the next physics tick
            if(n.motion_.allocated()) n.editMotion().floorBody = -1;
        }

        for(size_t k = 0; k < nodes_.size(); ++k)
            if(nodes_[k].parent >= 0) nodes_[size_t(nodes_[k].parent)].children.push_back(int32_t(k));

        collectPhysics();
    }

    void Runtime::update(float delta){
        const float dt = std::isfinite(delta) ? std::clamp(delta, 0.0f, 0.1f) : 0.0f;

        if(playing_){
            scene_.waterTime += dt;
            //fixed 60 Hz physics ticks (at most 4 per frame), before _process as in Godot.
            physicsTime_ += dt;
            int ticks = 0;

            while(physicsTime_ >= physics::Step && ticks < 4){
                physicsTick(physics::Step);
                physicsTime_ -= physics::Step;
                ++ticks;
            }

            if(ticks == 4) physicsTime_ = 0;

            freePending();

            for(size_t i = 0; i < nodes_.size(); ++i){
                const Live& n = nodes_[i];

                if(n.freed || !n.inTree || n.d.script < 0) continue;

                const uint32_t id = n.d.id;

                if(vm_->hasInstance(id)) vm_->process(id, dt);
            }

            flushScriptErrors();
            updateTweens(dt);
            freePending();
            updateAnimations(dt);
            previousInput_ = input_;

            if(!pendingScene_.empty()) switchScene();
        }

        updateTransforms();

        //emitters where the scripts left them
        if(playing_) updateParticles(dt);

        applyEnvironment();
        applyCamera();
    }

    namespace{
        uint16_t lerp565(uint32_t a, uint32_t b, float t){
            auto channel = [&](int shift){
                const float x = float((a >> shift) & 255), y = float((b >> shift) & 255);
                return uint32_t(std::lround(x + (y - x) * t));
            };

            return rgb565(channel(16) << 16 | channel(8) << 8 | channel(0));
        }
    }

    // Jet clears each row with gradient_ (water reflections use it too). A panorama is drawn here and Jet's clear is skipped.
    void Runtime::prepareBackground(){
        const Live* env = nullptr;

        for(const auto& n : nodes_) if(!n.freed && n.inTree && n.d.type == NodeType::WorldEnvironment){ env = &n; break; }
        gradient_.resize(size_t(height_));
        const Vec3 forward = -viewBasis_.column(2);
        const float focal = (height_ * 0.5f) / std::tan(radians(viewFov_) * 0.5f);
        const float pitch = std::asin(std::clamp(forward.y, -1.0f, 1.0f));
        const Background mode = env ? env->d.more().background_mode : Background::Color;

        if(mode == Background::Color){
            std::fill(gradient_.begin(), gradient_.end(), rgb565(env ? env->d.more().background : 0x161c28));
        } else {
            //horizon row from the camera pitch; colours follow the angle from it.
            const float horizon = height_ * 0.5f + focal * std::tan(pitch);
            const auto& e = env->d;

            for(int y = 0; y < height_; ++y){
                const float angle = std::atan((horizon - float(y)) / focal) / (Pi * 0.5f);

                if(angle >= 0){
                    const float t = 1 - (1 - std::min(angle, 1.0f)) * (1 - std::min(angle, 1.0f));
                    gradient_[size_t(y)] = lerp565(e.more().skyHorizon, e.more().skyTop, t);
                } else {
                    const float t = std::min(-angle * 4, 1.0f);
                    gradient_[size_t(y)] = lerp565(e.more().groundHorizon, e.more().groundBottom, t);
                }
            }
        }

        scene_.backgroundGradientColors = gradient_.data();
        const bool panorama = mode == Background::Panorama && env->d.more().sky_texture >= 0;
        scene_.setClearBuffer(!panorama);

        if(!panorama) return;

        //separable: yaw depends only on the column, pitch only on the row.
        const auto& sky = description_.textures[size_t(env->d.more().sky_texture)];
        const float yaw = std::atan2(forward.x, -forward.z);
        skyColumns_.resize(size_t(width_));
        skyRows_.resize(size_t(height_));

        for(int x = 0; x < width_; ++x){
            float u = (yaw + std::atan((x - width_ * 0.5f) / focal)) / (2 * Pi) + 0.5f;
            u -= std::floor(u);
            skyColumns_[size_t(x)] = std::min(int32_t(u * sky.width), int32_t(sky.width) - 1);
        }

        for(int y = 0; y < height_; ++y){
            const float elevation = pitch + std::atan((height_ * 0.5f - y) / focal);
            const float v = std::clamp(0.5f - elevation / Pi, 0.0f, 0.9999f);
            skyRows_[size_t(y)] = int32_t(v * sky.height) * sky.width;
        }

        const bool indexed = !sky.palette.empty();
        const uint8_t* indices = sky.indexData();
        //only the rows and columns this build stores (see bufferWidth()).
        uint16_t* out = target_;

        for(int y = FIELD_BUFFERS ? nextField() : 0; y < height_; y += BufferRowStep){
            const int32_t row = skyRows_[size_t(y)];

            if(indexed)
                for(int x = 0; x < width_; x += BufferColumnStep) *out++ = sky.palette[indices[size_t(row + skyColumns_[size_t(x)])]];
            else
                for(int x = 0; x < width_; x += BufferColumnStep) *out++ = sky.pixels[size_t(row + skyColumns_[size_t(x)])];
        }

        std::fill(depth_.begin(), depth_.end(), uint16_t(0xFFFF));
    }

    // Reflection UVs depend on the view and the world normals, so they are recomputed every frame.
    void Runtime::updateReflections(){
        for(auto& n : nodes_){
            if(!n.object || !n.texture || !n.d.reflection || !n.worldVisible) continue;

            const auto& r = n.object->rotation;
            const Basis rotation = Basis::rotationZ(radians(float(r.z))) * Basis::rotationY(radians(float(r.y))) * Basis::rotationX(radians(float(r.x)));
            const Vec3 origin{float(n.object->position.x), float(n.object->position.y), float(n.object->position.z)};
            int32_t anchor = 0;
            bool first = true;

            for(auto& v : n.object->vertices){
                const Vec3 local{float(v.position.x), float(v.position.y), float(v.position.z)};
                const Vec3 normal = rotation.xform({float(v.normal.x), float(v.normal.y), float(v.normal.z)});
                const Vec3 world = rotation.xform(local) + origin;
                const ::Vector3 p{int32_t(std::lround(world.x)), int32_t(std::lround(world.y)), int32_t(std::lround(world.z))};
                const ::Vector3 nn{int32_t(std::lround(normal.x)), int32_t(std::lround(normal.y)), int32_t(std::lround(normal.z))};
                v.uv = Renderer::environmentReflectionUV(p, nn, camera_.position);
                //keep each surface on one side of the panorama seam.
                if(first){ anchor = v.uv.x; first = false; }
                else v.uv.x = Renderer::unwrapEnvironmentU(v.uv.x, anchor);
            }
        }
    }

    namespace{
        uint16_t blend565(uint16_t dst, uint16_t src, uint32_t alpha){
            if(alpha >= 255) return src;

            const uint32_t inv = 255 - alpha;
            const uint32_t r = (((src >> 11) & 31) * alpha + ((dst >> 11) & 31) * inv) / 255;
            const uint32_t g = (((src >> 5) & 63) * alpha + ((dst >> 5) & 63) * inv) / 255;
            const uint32_t b = ((src & 31) * alpha + (dst & 31) * inv) / 255;
            return uint16_t(r << 11 | g << 5 | b);
        }
        // The font covers ASCII and Latin-1; anything else becomes '?'.
        int glyphIndex(uint32_t code){
            if(code >= 32 && code <= 126) return int(code - 32);

            if(code >= 160 && code <= 255) return int(code - 160 + 95);

            return '?' - 32;
        }
    }

    void Runtime::set_editor_canvas(bool enabled, float zoom, Vec3 origin){
        editorCanvas_ = enabled;
        editorCanvasZoom_ = std::isfinite(zoom) ? std::clamp(zoom, 0.05f, 64.0f) : 1;
        editorCanvasOrigin_ = origin.finite() ? origin : Vec3{};
    }

    // The current Camera2D's view: screen = canvas * zoom + shift.
    void Runtime::canvasView(float& zoomX, float& zoomY, float& shiftX, float& shiftY) const{
        zoomX = zoomY = 1;
        shiftX = shiftY = 0;

        if(canvasOverride()){
            zoomX = zoomY = editorCanvasZoom_;
            shiftX = -editorCanvasOrigin_.x * zoomX;
            shiftY = -editorCanvasOrigin_.y * zoomY;
            return;
        }

        for(const auto& n : nodes_)
            if(n.d.type == NodeType::Camera2D && !n.freed && n.inTree && n.d.more().current){
                zoomX = n.d.more().zoom[0];
                zoomY = n.d.more().zoom[1];
                shiftX = width_ * 0.5f - (n.world.origin.x + n.d.offset[0]) * zoomX;
                shiftY = height_ * 0.5f - (n.world.origin.y + n.d.offset[1]) * zoomY;
                return;
            }
    }

    std::vector<float> Runtime::shape_outline(uint32_t id) const{
        std::vector<float> out;
        const int index = indexOf(id);

        if(index < 0 || nodes_[size_t(index)].d.type != NodeType::CollisionShape2D || nodes_[size_t(index)].freed) return out;

        const Live& n = nodes_[size_t(index)];
        float zoomX, zoomY, shiftX, shiftY;
        canvasView(zoomX, zoomY, shiftX, shiftY);
        auto add = [&](float x, float y){
            const Vec3 p = n.world.xform({x, y, 0});
            out.push_back(p.x * zoomX + shiftX);
            out.push_back(p.y * zoomY + shiftY);
        };

        //a closed polygon: the rectangle's corners, or the lower and upper half of the circle (capsule: apart).
        const NodeExtras& e = n.d.more();

        if(e.collision == CollisionKind::Box){
            const float w = e.boxSize.x * 0.5f, h = e.boxSize.y * 0.5f;
            add(-w, -h); add(w, -h); add(w, h); add(-w, h);
            return out;
        }

        constexpr int Arc = 12;
        const float r = e.radius, half = e.collision == CollisionKind::Capsule ? std::max(0.0f, e.height * 0.5f - r) : 0;

        for(int i = 0; i <= Arc; ++i) add(std::cos(Pi * i / Arc) * r, std::sin(Pi * i / Arc) * r + half);

        for(int i = 0; i <= Arc; ++i) add(-std::cos(Pi * i / Arc) * r, -std::sin(Pi * i / Arc) * r - half);

        return out;
    }

    // The 2D nodes as seen by the current Camera2D (its position at the screen centre) or, without one, with the canvas
    // origin at the top-left corner, as in Godot. Sorted by z_index, then tree order; the Controls come after them.
    void Runtime::prepareCanvas(){
        float zoomX, zoomY, shiftX, shiftY;
        canvasView(zoomX, zoomY, shiftX, shiftY);
        auto screen = [&](const Live& n, float x, float y){
            const Vec3 p = n.world.xform({x, y, 0});
            return Vec3{p.x * zoomX + shiftX, p.y * zoomY + shiftY, 0};
        };

        std::vector<std::pair<int, int32_t>> order;

        for(size_t i = 0; i < nodes_.size(); ++i){
            const Live& n = nodes_[i];

            if((n.d.type == NodeType::Sprite2D || n.d.type == NodeType::AnimatedSprite2D) && n.worldVisible && n.d.texture >= 0)
                order.emplace_back(n.canvasZ, int32_t(i));
        }

        std::stable_sort(order.begin(), order.end(), [](const auto& a, const auto& b){ return a.first < b.first; });

        for(const auto& [z, index] : order){
            const Live& n = nodes_[size_t(index)];
            const NodeDesc& d = n.d;
            //opacity comes down from the 2D parents, like Godot's modulate.
            float alpha = std::clamp(d.alpha, 0.0f, 1.0f);

            for(int p = n.parent; p >= 0 && isNode2d(nodes_[p].d.type); p = nodes_[p].parent) alpha *= std::clamp(nodes_[p].d.alpha, 0.0f, 1.0f);

            const auto& t = description_.textures[size_t(d.texture)];
            const int fw = t.width / d.hframes, fh = t.height / d.vframes;

            if(alpha <= 0 || fw <= 0 || fh <= 0) continue;

            OverlayItem item;
            item.type = NodeType::Sprite2D;
            item.id = d.id;
            item.texture = &t;
            item.tint = d.color;
            item.alpha = uint8_t(std::lround(alpha * 255));
            const int frame = std::min(int(d.frame), d.hframes * d.vframes - 1);
            item.frameX = (frame % d.hframes) * fw;
            item.frameY = (frame / d.hframes) * fh;
            item.frameW = fw;
            item.frameH = fh;
            item.flip_h = d.flip_h;
            item.flip_v = d.flip_v;

            //the frame's corners on the screen: a = (0, 0), b = (fw, 0), c = (0, fh).
            const float left = (d.centered ? -fw * 0.5f : 0) + d.offset[0], top = (d.centered ? -fh * 0.5f : 0) + d.offset[1];
            const Vec3 a = screen(n, left, top), b = screen(n, left + fw, top), c = screen(n, left, top + fh);
            const Vec3 across = b - a, down = c - a;

            if(!a.finite() || !b.finite() || !c.finite()) continue;

            if(std::abs(across.y) < 1e-3f && std::abs(down.x) < 1e-3f){
                //not rotated: a rectangle, flipped by negative scales.
                if(across.x < 0) item.flip_h = !item.flip_h;

                if(down.y < 0) item.flip_v = !item.flip_v;

                item.x = screenPixel(std::round(std::min(a.x, b.x)));
                item.y = screenPixel(std::round(std::min(a.y, c.y)));
                item.w = screenPixel(std::round(std::max(a.x, b.x))) - item.x;
                item.h = screenPixel(std::round(std::max(a.y, c.y))) - item.y;
            } else {
                //rotated or skewed: each screen pixel finds its texel through the inverse map.
                const float det = across.x * down.y - down.x * across.y;

                if(std::abs(det) < 1e-6f) continue;

                const float m[4] = {down.y * fw / det, -down.x * fw / det, -across.y * fh / det, across.x * fh / det};

                //texels per pixel in 16.16: a sprite squeezed this thin would overflow it
                if(std::max({std::abs(m[0]), std::abs(m[1]), std::abs(m[2]), std::abs(m[3])}) >= 32767) continue;

                item.ux = int32_t(std::lround(m[0] * 65536));
                item.uy = int32_t(std::lround(m[1] * 65536));
                item.vx = int32_t(std::lround(m[2] * 65536));
                item.vy = int32_t(std::lround(m[3] * 65536));
                //at pixel centres: u = m0 * (x + 0.5 - a.x) + m1 * (y + 0.5 - a.y).
                item.u0 = int64_t(std::llround((m[0] * (0.5f - a.x) + m[1] * (0.5f - a.y)) * 65536));
                item.v0 = int64_t(std::llround((m[2] * (0.5f - a.x) + m[3] * (0.5f - a.y)) * 65536));
                item.affine = true;
                const Vec3 e = b + down;
                const float x0 = std::min({a.x, b.x, c.x, e.x}), x1 = std::max({a.x, b.x, c.x, e.x});
                const float y0 = std::min({a.y, b.y, c.y, e.y}), y1 = std::max({a.y, b.y, c.y, e.y});
                item.x = screenPixel(std::floor(x0));
                item.y = screenPixel(std::floor(y0));
                item.w = screenPixel(std::ceil(x1)) - item.x;
                item.h = screenPixel(std::ceil(y1)) - item.y;
            }

            if(item.w <= 0 || item.h <= 0 || item.x >= width_ || item.y >= height_ || item.x + item.w <= 0 || item.y + item.h <= 0) continue;

            overlay_.push_back(std::move(item));
        }
    }

    void Runtime::prepareOverlay(){
        overlay_.clear();
        prepareCanvas();

        for(const auto& n : nodes_){
            if(!is_control(n.d.type) || n.d.type == NodeType::Control || !n.worldVisible) continue;

            //opacity is inherited from the parent Controls, like Godot's modulate.
            float alpha = std::clamp(n.d.alpha, 0.0f, 1.0f);

            for(int p = n.parent; p >= 0 && is_control(nodes_[p].d.type); p = nodes_[p].parent) alpha *= std::clamp(nodes_[p].d.alpha, 0.0f, 1.0f);

            if(alpha <= 0) continue;

            const auto& d = n.d;
            OverlayItem item;
            item.type = d.type;
            item.id = d.id;
            //the editor's 2D view moves and zooms them with the canvas (text and images by whole pixels).
            const float zoom = canvasOverride() ? editorCanvasZoom_ : 1;
            const Vec3 at = canvasOverride() ? (n.world.origin - editorCanvasOrigin_) * zoom : n.world.origin;
            item.x = screenPixel(std::round(at.x));
            item.y = screenPixel(std::round(at.y));
            item.color = rgb565(d.color);
            item.tint = d.color;
            item.alpha = uint8_t(std::lround(alpha * 255));
            item.scale = uint8_t(std::max<int>(1, d.more().text_scale));
            const float scale = item.scale * zoom;

            if(d.type == NodeType::ColorRect){
                item.w = screenPixel(std::round(d.more().size.x * zoom));
                item.h = screenPixel(std::round(d.more().size.y * zoom));
            } else if(d.type == NodeType::TextureRect){
                if(d.texture < 0) continue;

                item.texture = &description_.textures[size_t(d.texture)];
                item.w = screenPixel(std::round(d.more().size.x > 0 ? d.more().size.x * zoom : item.texture->width * scale));
                item.h = screenPixel(std::round(d.more().size.y > 0 ? d.more().size.y * zoom : item.texture->height * scale));
            } else {
                //UTF-8 to glyphs, one list per line.
                item.lines.emplace_back();
                const std::string& text = d.more().text;
                size_t longest = 0;

                for(size_t i = 0; i < text.size();){
                    uint32_t code = uint8_t(text[i++]);

                    if(code >= 0xC0 && code < 0xE0 && i < text.size()) code = ((code & 0x1F) << 6) | (uint8_t(text[i++]) & 0x3F);
                    else if(code >= 0xE0){ while(i < text.size() && (uint8_t(text[i]) & 0xC0) == 0x80) ++i; code = '?'; }

                    if(code == '\n'){ item.lines.emplace_back(); continue; }
                    item.lines.back().push_back(uint8_t(glyphIndex(code)));
                    longest = std::max(longest, item.lines.back().size());
                }

                item.columns = int(longest);
                item.w = screenPixel(std::round(float(longest * font::Width) * scale));
                item.h = screenPixel(std::round(float(item.lines.size() * font::Height) * scale));
                item.stretched = zoom != 1;
            }

            if(item.w <= 0 || item.h <= 0 || item.x >= width_ || item.y >= height_ || item.x + item.w <= 0 || item.y + item.h <= 0) continue;

            overlay_.push_back(std::move(item));
        }
    }

    void composeOverlayRow(const std::vector<OverlayItem>& items, uint16_t* row, int y, int width, bool swapped){
        auto put = [&](int x, uint16_t c, uint32_t alpha){
            uint16_t dst = row[x];

            if(swapped) dst = uint16_t(dst << 8 | dst >> 8);

            dst = blend565(dst, c, alpha);
            row[x] = swapped ? uint16_t(dst << 8 | dst >> 8) : dst;
        };

        for(const auto& it : items){
            if(y < it.y || y >= it.y + it.h) continue;

            const int x0 = std::max(0, it.x), x1 = std::min(width, it.x + it.w);

            if(it.type == NodeType::ColorRect){
                for(int x = x0; x < x1; ++x) put(x, it.color, it.alpha);
            } else if(it.type == NodeType::Sprite2D){
                const auto& t = *it.texture;
                const uint8_t* indices = t.palette.empty() ? nullptr : t.indexData();
                auto draw = [&](int x, int u, int v){
                    if(it.flip_h) u = it.frameW - 1 - u;

                    if(it.flip_v) v = it.frameH - 1 - v;

                    const size_t i = size_t(it.frameY + v) * t.width + size_t(it.frameX + u);
                    uint16_t c = indices ? t.palette[indices[i]] : t.pixels[i];

                    if(t.has_alpha && c == t.alphaColor) return;

                    if(it.tint != 0xffffff) c = tint565(c, it.tint);

                    put(x, c, it.alpha);
                };

                if(!it.affine){
                    //64-bit: a sprite scaled up a lot is millions of pixels wide
                    const int v = int(int64_t(y - it.y) * it.frameH / it.h);

                    for(int x = x0; x < x1; ++x) draw(x, int(int64_t(x - it.x) * it.frameW / it.w), v);
                } else {
                    int64_t u = int64_t(it.ux) * x0 + int64_t(it.uy) * y + it.u0;
                    int64_t v = int64_t(it.vx) * x0 + int64_t(it.vy) * y + it.v0;
                    const int64_t uw = int64_t(it.frameW) << 16, vh = int64_t(it.frameH) << 16;

                    for(int x = x0; x < x1; ++x, u += it.ux, v += it.vx)
                        if(u >= 0 && u < uw && v >= 0 && v < vh) draw(x, int(u >> 16), int(v >> 16));
                }
            } else if(it.type == NodeType::TextureRect){
                //nearest sampling, modulate tints.
                const auto& t = *it.texture;
                const bool indexed = !t.palette.empty();
                const size_t ty = size_t(int64_t(y - it.y) * t.height / it.h) * t.width;

                for(int x = x0; x < x1; ++x){
                    const size_t i = ty + size_t(int64_t(x - it.x) * t.width / it.w);
                    uint16_t c = indexed ? t.palette[t.indexData()[i]] : t.pixels[i];

                    if(t.has_alpha && c == t.alphaColor) continue;

                    if(it.tint != 0xffffff) c = tint565(c, it.tint);

                    put(x, c, it.alpha);
                }
            } else if(it.stretched){
                //the editor's zoomed 2D view: each pixel takes its glyph pixel, so text keeps its size at any zoom.
                const int columns = it.columns * font::Width, rows = int(it.lines.size()) * font::Height;
                const int gyAll = int(int64_t(y - it.y) * rows / it.h);
                const auto& glyphs = it.lines[size_t(gyAll / font::Height)];
                const int gy = gyAll % font::Height;

                for(int x = x0; x < x1; ++x){
                    const int gxAll = int(int64_t(x - it.x) * columns / it.w);
                    const size_t g = size_t(gxAll / font::Width);

                    if(g < glyphs.size() && (font::Glyphs[glyphs[g]][gy] & (0x40 >> (gxAll % font::Width)))) put(x, it.color, it.alpha);
                }
            } else {
                const int cell = font::Height * it.scale;
                const size_t line = size_t((y - it.y) / cell);

                if(line >= it.lines.size()) continue;

                const int gy = ((y - it.y) % cell) / it.scale;
                int cx = it.x;

                for(uint8_t glyph : it.lines[line]){
                    if(cx >= width) break;

                    const uint8_t bits = font::Glyphs[glyph][gy];

                    for(int gx = 0; bits && gx < font::Width; ++gx){
                        if(!(bits & (0x40 >> gx))) continue;

                        for(int sx = 0; sx < it.scale; ++sx){
                            const int x = cx + gx * it.scale + sx;

                            if(x >= 0 && x < width) put(x, it.color, it.alpha);
                        }
                    }

                    cx += font::Width * it.scale;
                }
            }
        }
    }

    // With half-width fields the device composites the overlay while sending each row instead.
    void Runtime::drawOverlay(){
        prepareOverlay();
#if !HALF_WIDTH_BUFFERS && !FIELD_BUFFERS
        int top = height_, bottom = 0;

        for(const auto& it : overlay_){
            top = std::min(top, std::max(0, it.y));
            bottom = std::max(bottom, std::min(height_, it.y + it.h));
        }

        for(int y = top; y < bottom; ++y) composeOverlayRow(overlay_, target_ + size_t(y) * width_, y, width_, false);
#endif
    }

    int Runtime::pickOverlay(int x, int y) const{
        //the 2D view moved and zoomed them: as drawn, topmost first.
        if(canvasOverride()){
            for(auto it = overlay_.rbegin(); it != overlay_.rend(); ++it)
                if(x >= it->x && y >= it->y && x < it->x + it->w && y < it->y + it->h) return int(it->id);

            return -1;
        }

        for(size_t i = nodes_.size(); i-- > 0;){
            const Live* it = &nodes_[i];
            const auto& d = it->d;

            if(!is_control(d.type) || d.type == NodeType::Control || !it->worldVisible) continue;

            int w = int(d.more().size.x), h = int(d.more().size.y);

            if(d.type == NodeType::Label){
                int lines = 1, longest = 0, current = 0;

                for(char ch : d.more().text){
                    if(ch == '\n'){ ++lines; current = 0; continue; }

                    if((uint8_t(ch) & 0xC0) != 0x80) longest = std::max(longest, ++current);
                }

                w = longest * font::Width * d.more().text_scale;
                h = lines * font::Height * d.more().text_scale;
            } else if(d.type == NodeType::TextureRect && d.texture >= 0){
                const auto& t = description_.textures[size_t(d.texture)];

                if(w <= 0) w = t.width * d.more().text_scale;

                if(h <= 0) h = t.height * d.more().text_scale;
            }

            const Vec3& p = it->world.origin;

            if(x >= p.x && y >= p.y && x < p.x + w && y < p.y + h) return int(d.id);
        }

        //then the 2D sprites as last drawn, topmost first (rotated ones by their bounds).
        for(auto it = overlay_.rbegin(); it != overlay_.rend(); ++it)
            if(it->type == NodeType::Sprite2D && x >= it->x && y >= it->y && x < it->x + it->w && y < it->y + it->h) return int(it->id);

        return -1;
    }

    size_t MemoryReport::largestScene() const{
        size_t most = 0;

        for(const auto& s : scenes) most = std::max(most, s.total());

        return most;
    }

    namespace{
        // 32-bit sizes (arm-none-eabi, firmware config) plus newlib's 8 bytes per allocation.
        constexpr size_t Block = 8;
        constexpr size_t LiveBytes = 504, ExtrasBytes = 364 + Block, MotionBytes = 96 + Block, DescBytes = 184;
        constexpr size_t ObjectBytes = 188 + Block, MaterialBytes = 32 + Block, TextureBytes = 44 + Block, InstanceBytes = 68 + Block;
        constexpr size_t VertexBytes = 36, TriangleBytes = 16, QueueBytesPerTriangle = 129;
        constexpr size_t ParticleBytes = 40, EmitterBytes = 16 + Block, DrawerBytes = MaxParticles * 36 + 12 + Block;
        constexpr size_t OverlayBytes = 104, AssetBytes = 72;
        // The runtime, Jet's scene and rasteriser state and the VM before any content (fitted to a Nucleo-F746ZG's free heap).
        constexpr size_t EngineBytes = 26 * 1024;
    }

    MemoryReport Runtime::estimateMemory(bool textures_in_place) const{
        MemoryReport report;
        report.engine = EngineBytes;
        //the device draws half-width interlaced fields, whatever this build draws.
        report.field = size_t(width_ / 2) * size_t(height_ / 2) * 2;
        report.frames = report.field + 2 * size_t(width_) * 2;

        for(const auto& t : description_.textures){
            const size_t texels = size_t(t.width) * t.height;
            report.textures += AssetBytes + t.palette.size() * 2 + (t.palette.empty() ? texels * 2 : textures_in_place ? 0 : texels);
        }

        //compiled scripts: about twice their bytecode; instances count with their nodes.
        std::vector<size_t> instanceBytes(description_.scripts.size(), 64);
#ifndef JETSCRIPT_NO_COMPILER

        for(size_t i = 0; i < description_.scripts.size(); ++i){
            const auto& file = description_.scripts[i];
            const auto result = file.bytecode.empty() ? jetscript::compile(file.source, file.path, *host_)
                                                      : jetscript::loadBytecode(file.bytecode.data(), file.bytecode.size(), *host_);

            if(!result.script) continue;

            report.scripts += jetscript::saveBytecode(*result.script).size() * 2 + 256;
            instanceBytes[i] += result.script->members().size() * 16;
        }
#endif

        auto scene = [&](const std::string& path, const std::vector<NodeDesc>& nodes){
            MemoryEstimate e;
            e.scene = path;
            e.loading = nodes.size() * DescBytes;
            size_t triangles = 0;
            //Jet's pool that draws the streaks, once
            bool drawer = false;
            std::set<std::array<int32_t, 5>> shared;

            for(const auto& n : nodes){
                e.nodes += LiveBytes;

                if(usesExtras(n.type)) e.nodes += ExtrasBytes + n.more().text.size() + n.more().animations.size();

                if(physics::isCollisionObject(n.type)) e.nodes += MotionBytes;

                if(n.script >= 0 && size_t(n.script) < instanceBytes.size()) e.nodes += instanceBytes[size_t(n.script)];

                if(is_control(n.type) || n.type == NodeType::Sprite2D || n.type == NodeType::AnimatedSprite2D) e.nodes += OverlayBytes;

                if(n.type == NodeType::CPUParticles3D){
                    e.particles += EmitterBytes + size_t(n.more().amount) * ParticleBytes;
                    drawer = drawer || n.more().particle_style != ParticleStyle::Puff;
                }

                const bool mesh = n.type == NodeType::Sprite3D || (n.type == NodeType::MeshInstance3D && !(n.shape == Shape::Mesh && n.mesh < 0));

                if(!mesh) continue;

                //one Jet object per node; identical meshes (shape, detail, model and size) share their geometry.
                NodeDesc d = n;

                if(d.type == NodeType::Sprite3D) d.shape = Shape::Quad;

                std::unique_ptr<Renderer::Object> geometry(makeMesh(d, nullptr));
                const size_t tris = geometry ? geometry->triangles.size() : 0;
                triangles += tris;
                e.meshes += ObjectBytes + MaterialBytes + InstanceBytes + (n.texture >= 0 ? TextureBytes : 0);
                //reflective meshes keep geometry of their own.
                const std::array<int32_t, 5> key{int32_t(d.shape), d.segments, d.mesh, int32_t(std::lround(n.scale.x * 100)),
                                                  int32_t(std::lround(n.scale.y * 100 + n.scale.z * 10000))};

                if(geometry && (d.reflection || shared.insert(key).second))
                    e.meshes += ObjectBytes + geometry->vertices.size() * VertexBytes + tris * TriangleBytes + 2 * Block;
            }

            e.triangles = (triangles / 2 + 32) * QueueBytesPerTriangle;

            if(drawer) e.particles += DrawerBytes;

            for(const auto& a : description_.autoloads) e.nodes += LiveBytes + (a.script >= 0 ? instanceBytes[size_t(a.script)] : 0);

            //in blocks of 8
            const size_t count = nodes.size() + description_.autoloads.size();
            e.nodes += ((count + 7) / 8 * 8 - count) * LiveBytes;
            report.scenes.push_back(std::move(e));
        };

        scene(description_.path, description_.nodes);

        for(const auto& s : description_.scenes) scene(s.path, s.nodes);

        return report;
    }

    bool Runtime::drawn_rect(uint32_t id, int& x, int& y, int& w, int& h) const{
        for(const auto& it : overlay_)
            if(it.id == id){
                x = it.x;
                y = it.y;
                w = it.w;
                h = it.h;
                return true;
            }

        return false;
    }

    // Jet's per-frame queues would grow while drawing, each time holding the old and the doubled buffer for a moment:
    // reserve them once per scene instead. Back faces and what is out of view never reach them, so half of the scene's
    // triangles is usually plenty (hidden nodes count, they have geometry too); a busier frame still grows them.
    void Runtime::reserveTriangleQueues(){
        size_t triangles = 0;

        for(const auto& o : objects_){
            triangles += o->triangles.size();
#if JET_MESH_INSTANCING

            for(const auto& instance : o->instances) triangles += instance.mesh->triangles.size();
#endif
        }

        scene_.reserveTriangles(triangles / 2 + 32);
        queuesReserved_ = true;
    }

    void Runtime::render(){
        if(canvasOverride()){
            //the editor's 2D view: the canvas on the scene's background colour, without the 3D world.
            uint32_t background = 0x161c28;

            for(const auto& n : nodes_)
                if(n.d.type == NodeType::WorldEnvironment && !n.freed && n.inTree){ background = n.d.more().background; break; }
            std::fill(target_, target_ + size_t(bufferWidth()) * bufferRows(), rgb565(background));
            particleTriangles_ = 0;
            drawOverlay();
            return;
        }

        applyCamera();
        prepareBackground();
        updateReflections();

        if(!queuesReserved_) reserveTriangleQueues();

        scene_.render(executor_);
        //after the raster bands joined, as JetExamples' RenderEffects
        renderParticles();
        drawOverlay();
    }

    void Runtime::set_input(uint32_t pressed){ input_ = pressed; }

    int Runtime::actionIndex(std::string_view name) const{
        for(size_t i = 0; i < description_.actions.size(); ++i)
            if(description_.actions[i].name == name) return int(i);

        return -1;
    }

    std::vector<InputAction> defaultActions(){
        return {{"ui_left", {"Left", "A"}}, {"ui_right", {"Right", "D"}}, {"ui_up", {"Up", "W"}}, {"ui_down", {"Down", "S"}},
                {"ui_accept", {"Space", "Enter"}}, {"ui_cancel", {"Escape", "Backspace"}}};
    }

    void Runtime::resize(int width, int height){
        if(width == width_ && height == height_) return;

        width_ = width;
        height_ = height;
        color_.assign(size_t(bufferWidth()) * bufferRows(), 0);
        depth_.assign(Z_BUFFERING ? size_t(ZBUFFER_STRIDE(width)) * height : 0, 0);
        target_ = color_.data();
        scene_.resize(color_.data(), Z_BUFFERING ? depth_.data() : nullptr, width, height);
    }

    void Runtime::set_framebuffer(uint16_t* buffer){
        target_ = buffer ? buffer : color_.data();
        scene_.setFramebuffer(target_);
    }

    int Runtime::pick(int x, int y){
        if(x < 0 || y < 0 || x >= width_ || y >= height_) return -1;

        const int control = pickOverlay(x, y);

        if(control >= 0 || canvasOverride()) return control;
#if MAX_PICK_QUERIES == 0

        //device builds: no 3D picking
        return -1;
#else
        const Renderer::PickQuery query{int16_t(x), int16_t(y)};
        scene_.setPickQueries(&query, 1);
        render();
        const auto hit = scene_.getPickResults()[0];
        scene_.setPickQueries(nullptr, 0);

        if(hit.hit)
            for(const auto& n : nodes_)
                if(n.object && n.object == hit.object) return int(n.d.id);

        return -1;
#endif
    }

    bool Runtime::node_state(uint32_t id, NodeDesc& out) const{
        const int index = indexOf(id);

        if(index < 0 || nodes_[index].freed) return false;

        out = nodes_[index].d;
        return true;
    }

}
