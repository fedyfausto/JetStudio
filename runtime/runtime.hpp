#pragma once
#include "Scene.hpp"
#include "jetscript.hpp"
#include "math3d.hpp"
#include <array>
#include <cstdint>
#include <map>
#include <memory>
#include <new>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace jetstudio {
// Screen size comes from the project's display settings: any even size in this range.
constexpr int DefaultWidth = 320, DefaultHeight = 240;
constexpr int MinScreenSide = 64, MaxScreenWidth = 800, MaxScreenHeight = 480;
// Desktop draws every pixel; the device draws half-width interlaced fields like JetExamples (firmware/config/JetConfig.hpp).
constexpr int BufferColumnStep = HALF_WIDTH_BUFFERS ? 2 : 1, BufferRowStep = FIELD_BUFFERS ? 2 : 1;
constexpr size_t MaxNodes = 256, MaxMeshes = 128, MaxScripts = 64, MaxTextures = 64;
constexpr size_t MaxScriptBytes = 256 * 1024;
constexpr int MaxTextureSide = 1024;
constexpr size_t MaxMeshAssets = 128, MaxMeshVertices = 65535, MaxSceneVertices = 262144, MaxText = 512;
constexpr size_t MaxTextureBytes = 4 * 1024 * 1024; // all textures of a scene

// Named like Godot's. Label, TextureRect and ColorRect are 2D controls drawn over the 3D frame, in screen pixels.
enum class NodeType : uint8_t { Node, Node3D, MeshInstance3D, Camera3D, DirectionalLight3D, WorldEnvironment,
                                Label, TextureRect, ColorRect,
                                StaticBody3D, CharacterBody3D, RigidBody3D, Area3D, CollisionShape3D, CPUParticles3D, Sprite3D, Control };
// Godot's BoxShape3D, SphereShape3D, CapsuleShape3D.
enum class CollisionKind : uint8_t { Box, Sphere, Capsule };
// Primitives use Godot's default mesh sizes (BoxMesh 1 m, SphereMesh r 0.5...). Mesh is an imported model part.
enum class Shape : uint8_t { Box, Sphere, Plane, Pyramid, Cylinder, Capsule, Quad, Torus, Mesh };
enum class Shading : uint8_t { Gouraud, Flat, Unlit, Phong, Additive, Water };
// Sparks and Water are Jet's own looks. Puff is ours: soft discs that grow and fade (smoke, dust, steam).
enum class ParticleStyle : uint8_t { Sparks, Water, Puff };
// Subset of Godot's emission_shape.
enum class EmissionShape : uint8_t { Point, Sphere, Box };
// Size of the fixed pool in Jet's ParticleSystem, one per CPUParticles3D.
constexpr int MaxParticles = 200;
enum class CullMode : uint8_t { Back, Front, Disabled };
// Background is drawn first and never hides later geometry (rooms, floors, sky domes). OnTop is drawn last.
// Same result in the editor and on the device, which depth-sorts instead of using a z-buffer.
enum class DrawOrder : uint8_t { Default, Background, OnTop };
// Gradient follows the camera pitch like Godot's ProceduralSkyMaterial. Panorama is equirectangular.
enum class Background : uint8_t { Color, Gradient, Panorama };

// RGB565 pixels, or 8-bit palette indices when the image has at most 256 colours (lossless, half the memory).
struct TextureAsset {
    std::string path;
    uint16_t width = 0, height = 0;
    std::vector<uint16_t> pixels;  // RGB565, when palette is empty
    std::vector<uint8_t> indices;  // palette mode
    const uint8_t* packed_indices = nullptr; // palette mode, read in place from the package instead of indices
    std::vector<uint16_t> palette; // up to 256 entries
    bool has_alpha = false;
    uint16_t alpha_color = 0; // colour key of transparent pixels
    const uint8_t* index_data() const { return packed_indices ? packed_indices : indices.data(); }
    size_t bytes() const {
        return pixels.size() * 2 + (packed_indices ? size_t(width) * height : indices.size()) + palette.size() * 2;
    }
};
// Quantize forces the palette by reducing to 256 colours (median cut).
enum class PaletteMode : uint8_t { Auto, Quantize, Rgb565 };
// alpha < 128 counts as transparent.
bool make_texture(std::string path, int width, int height, const uint8_t* rgba, TextureAsset& out, std::string& error,
                  PaletteMode mode = PaletteMode::Auto);

const char* node_type_name(NodeType);
bool is_node3d(NodeType);
bool is_control(NodeType);
// The node types whose fields include NodeExtras (all but Node, Node3D, MeshInstance3D and Sprite3D).
bool uses_extras(NodeType);

// Godot space (metres, Y up, -Z forward), UV origin top-left.
struct MeshVertex { Vec3 position, normal; float u = 0, v = 0; };
struct MeshAsset {
    std::string path; // res://model.obj#part
    std::vector<MeshVertex> vertices;
    std::vector<uint16_t> indices; // triangles, counter-clockwise when seen from outside
};

struct ScriptOverride { std::string name; jetscript::Value value; };

// Composited one row at a time, on the device during scanout, so HUD text stays sharp over half-width fields.
struct OverlayItem {
    NodeType type = NodeType::ColorRect;
    int x = 0, y = 0, w = 0, h = 0; // screen rectangle (Label: the text block)
    uint16_t color = 0;             // RGB565 fill / text colour
    uint32_t tint = 0xffffff;       // TextureRect modulate (0xRRGGBB)
    uint8_t alpha = 255, scale = 1;
    const TextureAsset* texture = nullptr;
    std::vector<std::vector<uint8_t>> lines; // Label glyph indices per line
};
// swapped: the row holds big-endian pixels, as sent to the panel.
void compose_overlay_row(const std::vector<OverlayItem>& items, uint16_t* row, int y, int width, bool swapped);

// Rarely used node data (Lazy blocks) is allocated through these: malloc/free, but the ESP32 firmware puts it
// in PSRAM, keeping the internal RAM for the buffers that drawing touches every frame.
void* cold_alloc(size_t bytes);
void cold_free(void* p);

// A T allocated on the first edit and copied with its owner; until then get() returns the defaults.
template <class T> class Lazy {
public:
    Lazy() = default;
    Lazy(const Lazy& o) : p_(o.p_ ? make(o.p_.get()) : nullptr) {}
    Lazy(Lazy&&) noexcept = default;
    Lazy& operator=(const Lazy& o) {
        if (this != &o) p_.reset(o.p_ ? make(o.p_.get()) : nullptr);
        return *this;
    }
    Lazy& operator=(Lazy&&) noexcept = default;
    // A class member, not a local static: no initialization guard (a memory barrier on the ESP32) on every read.
    const T& get() const { return p_ ? *p_ : defaults_; }
    T& edit() {
        if (!p_) p_.reset(make(nullptr));
        return *p_;
    }
    bool allocated() const { return p_ != nullptr; }

private:
    struct Free {
        void operator()(T* p) const { p->~T(); cold_free(p); }
    };
    static T* make(const T* from) {
        void* memory = cold_alloc(sizeof(T));
        return from ? new (memory) T(*from) : new (memory) T();
    }
    static const T defaults_;
    std::unique_ptr<T, Free> p_;
};
template <class T> const T Lazy<T>::defaults_{};

// Fields of the less common node types (2D, camera, light, environment, physics, particles).
struct NodeExtras {
    // Label / TextureRect / ColorRect (color and alpha are shared with materials)
    std::string text;
    Vec3 size{100, 30, 0};  // TextureRect: 0 = texture size
    uint8_t text_scale = 1; // integer pixel scale of Label text and TextureRect images
    // Camera3D (vertical FOV in degrees, as in Godot)
    float fov = 75, near = 0.05f, far = 100;
    bool current = false;
    // DirectionalLight3D
    uint32_t light_color = 0xffffff;
    float energy = 1;
    // WorldEnvironment
    uint32_t background = 0x161c28, ambient = 0x4e586e;
    Background background_mode = Background::Color;
    uint32_t sky_top = 0x0a1a3a, sky_horizon = 0x3d6aa8, ground_horizon = 0x2a3442, ground_bottom = 0x10141c;
    int32_t sky_texture = -1; // panorama, index in Description::textures
    // Physics (arcade, see physics.hpp)
    CollisionKind collision = CollisionKind::Box;
    Vec3 box_size{1, 1, 1};
    float radius = 0.5f, height = 2.0f; // sphere/capsule radius; capsule total height (Godot)
    bool disabled = false;
    // Godot's layer (what it is) and mask (what it detects)
    uint32_t collision_layer = 1, collision_mask = 1;
    // RigidBody3D (also friction/bounce of StaticBody3D)
    float mass = 1, gravity_scale = 1, linear_damp = 0, angular_damp = 0, friction = 1, bounce = 0;
    Vec3 linear_velocity{}, angular_velocity{}; // initial (m/s, rad/s)
    bool freeze = false;
    float floor_max_angle = 45; // CharacterBody3D, degrees
    bool monitoring = true;     // Area3D
    // CPUParticles3D (Godot's names and defaults; emission in world space)
    bool emitting = true, one_shot = false, additive = true;
    bool no_depth_test = false;    // drawn over everything (no occlusion: cheaper on the device)
    uint16_t amount = 8;           // particles per cycle, 1..MaxParticles
    float lifetime = 1, lifetime_randomness = 0, explosiveness = 0;
    Vec3 direction{1, 0, 0};       // local; spread is the cone half angle in degrees
    float spread = 45, velocity_min = 0, velocity_max = 0; // initial_velocity_min/max, m/s
    float damping_min = 0, damping_max = 0; // speed lost per second, m/s² (smoke drifts and stops)
    Vec3 gravity{0, -9.8f, 0};     // m/s²
    ParticleStyle particle_style = ParticleStyle::Sparks;
    EmissionShape emission_shape = EmissionShape::Point;
    float emission_radius = 1;     // emission_sphere_radius
    Vec3 emission_box{1, 1, 1};    // emission_box_extents (half size)
    // Puff: colour goes from color to color_end, opacity from alpha to 0, diameter from puff_size to puff_size_end (m)
    uint32_t color_end = 0x404040;
    float puff_size = 0.2f, puff_size_end = 0.6f;
};

// Godot conventions: metres, Y up, -Z forward, rotation in degrees applied in YXZ order.
struct NodeDesc {
    uint32_t id = 1;
    int32_t parent = -1; // index in Description::nodes; parents precede children
    NodeType type = NodeType::Node3D;
    Vec3 position{}, rotation{}, scale{1, 1, 1}; // next to type: read by every transform pass
    bool visible = true;
    std::string name = "Node3D";
    // MeshInstance3D, material is a StandardMaterial3D subset
    Shape shape = Shape::Box;
    uint32_t color = 0xe6a45c; // albedo; multiplies the texture
    Shading shading = Shading::Gouraud;
    uint16_t segments = 12;
    int32_t texture = -1; // index in Description::textures
    float uv_scale[2] = {1, 1}, uv_offset[2] = {0, 0};
    bool bilinear = false, repeat = true, perspective = true;
    float alpha = 1;      // 0..1
    float specular = 0.5f; // Phong highlight / water ripple strength, 0..1
    uint8_t shininess = 32;
    CullMode cull = CullMode::Back;
    DrawOrder draw_order = DrawOrder::Default;
    bool billboard = false;
    // Sprite3D, as in Godot: frame `frame` of an hframes × vframes sheet, `pixel_size` metres per texel.
    uint16_t hframes = 1, vframes = 1, frame = 0;
    bool flip_h = false, flip_v = false;
    float pixel_size = 0.01f;
    float diffuse = 1;      // diffuse reflectance 0..1
    bool reflection = false; // albedo texture used as environment panorama (chrome, glass)
    int32_t mesh = -1;      // Shape::Mesh: index in Description::meshes
    int32_t script = -1; // index in Description::scripts
    std::vector<ScriptOverride> overrides;

    // Every live node keeps its NodeDesc and most are meshes or sprites, so the other types' fields live
    // in a block of their own: more() reads them (the defaults when missing), extras() allocates it to write.
    const NodeExtras& more() const { return extras_.get(); }
    NodeExtras& extras() { return extras_.edit(); }
    bool has_extras() const { return extras_.allocated(); }

private:
    Lazy<NodeExtras> extras_;
};// Source in the editor, bytecode in packages.
struct ScriptFile { std::string path, source; std::vector<uint8_t> bytecode; };
// Bit i of the input mask is actions[i]. Keys are Godot key names, used by the PC players.
// Board buttons are wired per action in the firmware (generated_inputs.hpp).
struct InputAction {
    std::string name;
    std::vector<std::string> keys;
};
constexpr size_t MaxActions = 32, MaxActionKeys = 8;
// Godot's built-in ui_* actions with their keys (arrows/WASD, Space/Enter, Escape/Backspace).
std::vector<InputAction> default_actions();
// Other scenes share the Description's scripts, textures and meshes.
struct SceneDesc {
    std::string path; // res://...jetscene
    std::vector<NodeDesc> nodes;
    uint32_t offset = 0; // where its nodes start in the package (decode_pack)
};
constexpr size_t MaxScenes = 64;

// Godot autoloads, scripts only: they survive scene changes and any script reaches them by name (Game.score).
struct Autoload {
    std::string name;
    int32_t script = -1;
};
constexpr size_t MaxAutoloads = 16;
constexpr uint32_t AutoloadIdBase = 0x7fff0000u; // node ids of the autoloads; scene ids stay below
struct Description {
    std::string name = "Scene";
    std::string path;            // the starting scene, held in `nodes`
    int width = DefaultWidth, height = DefaultHeight; // screen resolution
    std::vector<InputAction> actions = default_actions();
    std::vector<NodeDesc> nodes; // depth-first order, nodes[0] is the root
    uint32_t nodes_offset = 0;   // where `nodes` start in the package (decode_pack)
    std::vector<SceneDesc> scenes; // the project's other scenes
    std::vector<Autoload> autoloads;
    std::vector<ScriptFile> scripts;
    std::vector<TextureAsset> textures;
    std::vector<MeshAsset> meshes;
};
bool validate(const Description&, std::string& error);
uint16_t rgb565(uint32_t rgb);

// Bits of the default actions (the first six of default_actions()).
enum Action : uint32_t { UiLeft = 1, UiRight = 2, UiUp = 4, UiDown = 8, UiAccept = 16, UiCancel = 32 };

struct LogLine {
    std::string text;
    bool error = false;
    std::string path; // script location for errors, when known
    int line = 0;
};
struct ScriptDiagnostic { std::string path; int line = 0, column = 0; std::string message; };

class ScriptHost;

// No Godot dependency here: this library also builds for ESP-IDF.
namespace physics { struct World; struct Contact; }
} // namespace jetstudio
namespace Renderer { class ParticleSystem; }
namespace jetstudio {

class Runtime {
public:
    Runtime();
    ~Runtime();
    // Stops play. On error the previous scene is kept.
    bool load(Description, std::string& error); // std::move it in to skip the copy
    // An exported package that outlives the runtime (flash on the device): only the scene being played stays
    // in RAM, the others are read again from the package when entered.
    // textures_in_place: palette textures are read from the package, not copied (boards without PSRAM).
    bool load_pack(const uint8_t* data, size_t size, std::string& error, bool textures_in_place = false);
    // Compiles scripts and runs _ready(). On compile errors nothing runs.
    bool start(std::vector<ScriptDiagnostic>& errors);
    void stop(); // back to the authored state
    bool playing() const { return playing_; }
    // Scripts only run while playing.
    void update(float delta);
    void render();
    // Lets the device split rasterizeBand across cores. The executor must rasterize every row and join its workers.
    void set_raster_executor(Renderer::Scene::RasterExecutor executor) { executor_ = executor; }
    // render() fills buffer_width() x buffer_rows() pixels.
    int width() const { return width_; }
    int height() const { return height_; }
    int buffer_width() const { return width_ / BufferColumnStep; }
    int buffer_rows() const { return height_ / BufferRowStep; }
    // Devices render straight into their DMA buffers. nullptr goes back to the internal one.
    void set_framebuffer(uint16_t* buffer);
    // Row parity (0 even, 1 odd) the next render() fills with FIELD_BUFFERS, else 0.
    int next_field() const { return FIELD_BUFFERS ? (scene_.frameCounter % 2 == 0 ? 1 : 0) : 0; }
    // 2D nodes of the last render(), for compose_overlay_row().
    const std::vector<OverlayItem>& overlay() const { return overlay_; }
    // Editor viewpoint (Godot coordinates, rotation in radians, vertical FOV).
    void set_editor_camera(Vec3 position, Vec3 rotation, float fov);
    // Use the editor viewpoint even while playing or when the scene has a camera.
    void use_editor_camera(bool enabled) { editor_view_ = enabled; }
    int pick(int x, int y); // node id or -1
    void set_input(uint32_t pressed); // bit i = Description::actions[i]
    // Deferred to the end of the frame, like Godot. False if the package lacks the scene.
    bool change_scene(std::string_view path);
    const std::string& current_scene() const { return current_scene_; }
    int action_index(std::string_view name) const; // -1 when the Input Map has no such action
    std::vector<LogLine> take_log();

    const std::vector<uint16_t>& pixels() const { return color_; }
    const Description& description() const { return description_; }
    int triangles() const { return scene_.lastFrameRasterizedTriangles + int(particle_triangles_); }
    int live_particles() const; // all CPUParticles3D, while playing
    size_t framebuffer_bytes() const { return (color_.size() + depth_.size()) * sizeof(uint16_t); }
    size_t mesh_count() const { return meshes_; }
    size_t node_count() const { return nodes_.size(); }
    // RAM of the live nodes: their array, the extra fields and the body state of the nodes that have them.
    size_t node_bytes() const;
    // Live state of a node during play (for the editor); false if missing/freed.
    bool node_state(uint32_t id, NodeDesc& out) const;

    // Material and UV values last pushed to a node's Jet object.
    struct Applied {
        uint32_t color = 0;
        Shading shading = Shading::Gouraud;
        float alpha = 1, specular = 0, diffuse = 1;
        uint8_t shininess = 0;
        bool bilinear = false, repeat = true, perspective = true, billboard = false;
        CullMode cull = CullMode::Back;
        float uv_scale[2] = {1, 1}, uv_offset[2] = {0, 0};
        static Applied of(const NodeDesc& d) {
            return {d.color, d.shading, d.alpha, d.specular, d.diffuse, d.shininess, d.bilinear, d.repeat, d.perspective,
                    d.billboard, d.cull, {d.uv_scale[0], d.uv_scale[1]}, {d.uv_offset[0], d.uv_offset[1]}};
        }
    };

    // Node state while playing, shared with ScriptHost.
    struct Motion {
        Vec3 velocity{}, angular{}, force{}; // m/s, rad/s, accumulated force for the next step
        bool on_floor = false, on_wall = false, on_ceiling = false, sleeping = false;
        Vec3 floor_normal{0, 1, 0};
        int floor_body = -1;            // CharacterBody3D: what it stands on (moving platforms)
        Vec3 last_origin{};             // world position at the end of the previous physics tick
        Vec3 last_motion{};             // how far it moved during that tick
        float still_time = 0;
        std::vector<uint32_t> overlaps; // Area3D: ids of the bodies inside
    };
    struct Live {
        // Hot in every transform pass: kept together since each scattered field costs a PSRAM cache miss on the ESP32.
        int32_t parent = -1;
        bool freed = false, free_requested = false, world_visible = true, local_valid = false;
        Transform world;
        // Cached from d.position/rotation/scale, recomputed when they change.
        Transform local;
        Vec3 local_position, local_rotation, local_scale;
        NodeDesc d;
        std::vector<int32_t> children;
        Vec3 baked_scale{1, 1, 1};
        float jet_radius = 0;         // farthest baked vertex from the origin (Jet units)
        Renderer::Object* object = nullptr;
        Renderer::Material* material = nullptr;
        Renderer::Texture* texture = nullptr;
        std::vector<uint16_t> tinted; // albedo-tinted palette, or RGB565 pixels (tinted and/or expanded)
        bool flipped = false;         // winding reversed to emulate front-face culling
        std::vector<Renderer::Object::Vertex> base_vertices;
        Applied applied;              // material/UV state last pushed to Jet
        Transform synced_world;       // pose last pushed to the Jet object
        bool synced = false, synced_visible = false;
        // Physics (physics.cpp): only bodies allocate it.
        const Motion& motion() const { return motion_.get(); }
        Motion& edit_motion() { return motion_.edit(); }
        Lazy<Motion> motion_;
        // CPUParticles3D (particles.cpp)
        int32_t particles = -1; // index in Runtime::particles_
        float cycle = 0;        // seconds into the current emission cycle
        int emitted = 0;        // particles emitted in this cycle
    };

private:
    friend class ScriptHost;
    bool build(std::string& error);
    Renderer::Object* make_mesh(const NodeDesc&, Renderer::Material*) const;
    void enter_scene();
    void switch_scene();
    const std::vector<NodeDesc>* nodes_of(std::string_view path);
    void start_autoloads();
    void update_transforms(bool sync = true);
    // Physics (physics.cpp)
    void physics_tick(float dt);
    void physics_begin();
    void physics_step(float dt);
    void update_areas();
    void move_body(int index, const Transform& world, bool rotated = true);
    void wake(Live& n);
    bool deepest_contact(int index, physics::Contact& best, int& other) const;
    bool move_and_slide(int index);
    // CPUParticles3D (particles.cpp)
    void start_particles();
    void update_particles(float dt);
    void render_particles();
    struct ParticleView;
    unsigned draw_puffs(const Live& n, Renderer::ParticleSystem& system, const ParticleView& view);
    unsigned draw_particles(const Live& n, const ParticleView& view);
    void build_occlusion(const ParticleView& view, int x0, int y0, int x1, int y1, float far_z);
    bool occluded(const ParticleView& view, float x, float y, float z) const;
    void restart_particles(Live& n);
    void emit_particle(Live& n, Renderer::ParticleSystem& system, float age);
    float random01();
    void sync_object(Live&);
    void reserve_triangle_queues();
    void bake(Live&, Vec3 scale);
    void share(Live&, Vec3 scale);
    void apply_camera();
    void apply_environment();
    void apply_material(Live&);
    void prepare_background();
    void update_reflections();
    void prepare_overlay();
    void draw_overlay();
    int pick_overlay(int x, int y) const;
    void log(std::string text, bool error, std::string path = {}, int line = 0);
    void flush_script_errors();
    void free_pending();
    int index_of(uint32_t id) const;

    void resize(int width, int height);
    int width_ = DefaultWidth, height_ = DefaultHeight;
    std::vector<uint16_t> color_, depth_;
    uint16_t* target_ = nullptr; // framebuffer being drawn (color_ or a device buffer)
    std::vector<OverlayItem> overlay_;
    Renderer::Camera camera_;
    Renderer::DirectionalLight sun_;
    Renderer::AmbientLight ambient_;
    std::vector<std::unique_ptr<Renderer::Material>> materials_;
    std::vector<std::unique_ptr<Renderer::Object>> objects_;
    bool queues_reserved_ = false; // Jet's per-frame triangle queues, sized once per scene
    // Baked meshes shared by nodes with the same mesh, scale and UVs; a copy lives while some node draws it.
    std::map<std::array<int32_t, 11>, std::weak_ptr<const Renderer::Object>> shared_meshes_;
    Renderer::Material shared_material_; // what shared copies point at: every node draws them with its own material
    std::vector<std::unique_ptr<Renderer::Texture>> textures_;
    std::vector<uint16_t> gradient_;
    std::vector<int32_t> sky_columns_, sky_rows_;
    Basis view_basis_;
    float view_fov_ = 70;
    Renderer::Scene scene_;
    Renderer::Scene::RasterExecutor executor_ = nullptr;
    std::unique_ptr<physics::World> physics_;
    float physics_time_ = 0, physics_delta_ = 1.0f / 60.0f;
    bool has_physics_ = false; // the scene has collision shapes in bodies or areas
    std::vector<std::pair<int32_t, int32_t>> shape_owners_; // CollisionShape3D node -> its body or area
    std::vector<int32_t> collision_objects_; // bodies and areas with shapes, once each
    bool transforms_stale_ = false; // scripts ran after the last update_transforms()
    const std::vector<NodeDesc>* scene_nodes_ = nullptr; // the scene that is built
    const uint8_t* pack_ = nullptr; // set by load_pack()
    size_t pack_size_ = 0;
    std::vector<NodeDesc> pack_nodes_; // a scene read from the package, until build() copies it
    std::string current_scene_, pending_scene_;
    std::vector<std::shared_ptr<const jetscript::Script>> compiled_;
    std::vector<std::unique_ptr<Renderer::ParticleSystem>> particles_; // one per CPUParticles3D while playing
    std::vector<std::array<float, MaxParticles>> particle_damping_;    // per particle, beside each pool
    uint32_t random_ = 0x9e3779b9;  // particle emission (xorshift)
    unsigned particle_triangles_ = 0; // drawn in the last frame
    // The device has no z-buffer: opaque mesh depth in 4x4 pixel cells hides the particles behind them.
    std::vector<uint16_t> occlusion_;
    std::vector<float> occlusion_points_; // scratch: projected vertices
    Description description_;
    std::vector<Live> nodes_;
    std::unordered_map<uint32_t, int32_t> index_;
    size_t meshes_ = 0;
    bool playing_ = false, editor_view_ = true;
    Vec3 editor_position_{4, 3, 6}, editor_rotation_{-0.4f, 0.6f, 0};
    float editor_fov_ = 70;
    // "Just pressed" is true for one frame in _process and for one tick in _physics_process, as in Godot.
    uint32_t input_ = 0, previous_input_ = 0, physics_previous_input_ = 0;
    bool in_physics_ = false;
    bool pending_free_ = false;
    std::vector<LogLine> log_;
    std::unique_ptr<ScriptHost> host_;
    std::unique_ptr<jetscript::VM> vm_;
};

#ifndef JETSCRIPT_NO_COMPILER
// Packages carry bytecode only, no script source.
std::vector<uint8_t> encode_pack(const Description&, std::string& error);
#endif
bool decode_pack(const uint8_t*, size_t, Description&, std::string& error, bool textures_in_place = false);
bool pack_resolution(const uint8_t*, size_t, int& width, int& height); // header only
// One scene's nodes, read again from the package at SceneDesc::offset or Description::nodes_offset.
bool decode_nodes(const uint8_t*, size_t, uint32_t offset, std::vector<NodeDesc>&, std::string& error);
} // namespace jetstudio
