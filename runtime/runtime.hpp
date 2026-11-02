#pragma once
#include "Math.hpp" // Jet's Vec3f
#include "Scene.hpp"
#include "jetscript.hpp"
#include "math3d.hpp"
#include <algorithm>
#include <array>
#include <cstdint>
#include <map>
#include <memory>
#include <new>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace jetstudio{
    // Screen size comes from the project's display settings: any even size in this range.
    constexpr int DefaultWidth = 320, DefaultHeight = 240;
    constexpr int MinScreenSide = 64, MaxScreenWidth = 800, MaxScreenHeight = 480;
    // Desktop draws every pixel; the device draws half-width interlaced fields like JetExamples (firmware/config/JetConfig.hpp).
    constexpr int BufferColumnStep = HALF_WIDTH_BUFFERS ? 2 : 1, BufferRowStep = FIELD_BUFFERS ? 2 : 1;
    constexpr size_t MaxNodes = 256, MaxMeshes = 128, MaxScripts = 64, MaxTextures = 64;
    constexpr size_t MaxScriptBytes = 256 * 1024;
    constexpr int MaxTextureSide = 1024;
    constexpr size_t MaxMeshAssets = 128, MaxMeshVertices = 65535, MaxSceneVertices = 262144, MaxText = 512;
    // all textures of a scene
    constexpr size_t MaxTextureBytes = 4 * 1024 * 1024;

    // Named like Godot's. Label, TextureRect and ColorRect are 2D controls drawn over the 3D frame, in screen pixels.
    enum class NodeType : uint8_t{ Node, Node3D, MeshInstance3D, Camera3D, DirectionalLight3D, WorldEnvironment,
                                    Label, TextureRect, ColorRect,
                                    StaticBody3D, CharacterBody3D, RigidBody3D, Area3D, CollisionShape3D, CPUParticles3D, Sprite3D, Control,
                                    Node2D, Sprite2D, AnimatedSprite2D, Camera2D,
                                    StaticBody2D, CharacterBody2D, RigidBody2D, Area2D, CollisionShape2D, Timer, AnimationPlayer };
    // Godot's BoxShape3D, SphereShape3D, CapsuleShape3D; in 2D RectangleShape2D, CircleShape2D, CapsuleShape2D.
    enum class CollisionKind : uint8_t{ Box, Sphere, Capsule };
    // Primitives use Godot's default mesh sizes (BoxMesh 1 m, SphereMesh r 0.5...). Mesh is an imported model part.
    enum class Shape : uint8_t{ Box, Sphere, Plane, Pyramid, Cylinder, Capsule, Quad, Torus, Mesh };
    enum class Shading : uint8_t{ Gouraud, Flat, Unlit, Phong, Additive, Water };
    // Sparks and Water are Jet's own looks. Puff is ours: soft discs that grow and fade (smoke, dust, steam).
    enum class ParticleStyle : uint8_t{ Sparks, Water, Puff };
    enum class EmissionShape : uint8_t{ Point, Sphere, Box };
    // Most particles of one CPUParticles3D (its amount), the size of Jet's ParticleSystem pool.
    constexpr int MaxParticles = 200;
    enum class CullMode : uint8_t{ Back, Front, Disabled };
    // Background is drawn first and never hides later geometry (rooms, floors, sky domes). OnTop is drawn last.
    // Same result in the editor and on the device, which depth-sorts instead of using a z-buffer.
    enum class DrawOrder : uint8_t{ Default, Background, OnTop };
    // Gradient follows the camera pitch like Godot's ProceduralSkyMaterial. Panorama is equirectangular.
    enum class Background : uint8_t{ Color, Gradient, Panorama };

    // RGB565 pixels, or 8-bit palette indices when the image has at most 256 colours (lossless, half the memory).
    struct TextureAsset{
        std::string path;
        uint16_t width = 0, height = 0;
        // RGB565, when palette is empty
        std::vector<uint16_t> pixels;
        std::vector<uint8_t> indices;
        // palette mode, read in place from the package instead of indices
        const uint8_t* packedIndices = nullptr;
        std::vector<uint16_t> palette;
        bool has_alpha = false;
        // colour key of transparent pixels
        uint16_t alphaColor = 0;
        const uint8_t* indexData() const{ return packedIndices ? packedIndices : indices.data(); }
        size_t bytes() const{
            return pixels.size() * 2 + (packedIndices ? size_t(width) * height : indices.size()) + palette.size() * 2;
        }
    };
    // Quantize forces the palette by reducing to 256 colours (median cut).
    enum class PaletteMode : uint8_t{ Auto, Quantize, Rgb565 };
    // alpha < 128 counts as transparent.
    bool makeTexture(std::string path, int width, int height, const uint8_t* rgba, TextureAsset& out, std::string& error,
                      PaletteMode mode = PaletteMode::Auto);

    const char* nodeTypeName(NodeType);
    bool isNode3d(NodeType);
    bool is_control(NodeType);
    // Node2D and the 2D nodes that extend it: drawn over the 3D scene, under the Controls, in canvas pixels.
    bool isNode2d(NodeType);
    // One animation of an AnimatedSprite2D (NodeExtras::animations): frames first..last of its sprite sheet.
    struct SpriteAnimation{ std::string name; int first = 0, last = 0; float fps = 5; bool loop = true; };
    // False when the text is malformed or a frame is past frames.
    bool parseAnimations(std::string_view text, int frames, std::vector<SpriteAnimation>& out);
    // The node types whose fields include NodeExtras (all but Node, Node3D, MeshInstance3D and Sprite3D).
    bool usesExtras(NodeType);

    // Godot space (metres, Y up, -Z forward), UV origin top-left.
    struct MeshVertex{ Vec3 position, normal; float u = 0, v = 0; };
    struct MeshAsset{
        // res://model.obj#part
        std::string path;
        std::vector<MeshVertex> vertices;
        // triangles, counter-clockwise when seen from outside
        std::vector<uint16_t> indices;
    };

    struct ScriptOverride{ std::string name; jetscript::Value value; };

    // Composited one row at a time, on the device during scanout, so HUD text stays sharp over half-width fields.
    struct OverlayItem{
        // Sprite2D for every 2D sprite
        NodeType type = NodeType::ColorRect;
        uint32_t id = 0;
        // screen rectangle (Label: the text block; a rotated sprite: its bounds)
        int x = 0, y = 0, w = 0, h = 0;
        // RGB565 fill / text colour
        uint16_t color = 0;
        // TextureRect modulate (0xRRGGBB)
        uint32_t tint = 0xffffff;
        uint8_t alpha = 255, scale = 1;
        const TextureAsset* texture = nullptr;
        // Label glyph indices per line
        std::vector<std::vector<uint8_t>> lines;
        // Label: glyphs on its longest line
        int columns = 0;
        // Label in the editor's zoomed 2D view: glyphs sampled at any size, not whole pixels
        bool stretched = false;
        // Sprite2D: the frame inside the texture, and for a rotated or skewed sprite the screen-to-texel map
        // (16.16 fixed point, at pixel centres): u = ux * x + uy * y + u0, v = vx * x + vy * y + v0.
        int frameX = 0, frameY = 0, frameW = 0, frameH = 0;
        bool flip_h = false, flip_v = false, affine = false;
        int32_t ux = 0, uy = 0, vx = 0, vy = 0;
        int64_t u0 = 0, v0 = 0;
    };
    // swapped: the row holds big-endian pixels, as sent to the panel.
    void composeOverlayRow(const std::vector<OverlayItem>& items, uint16_t* row, int y, int width, bool swapped);

    // Rarely used node data (Lazy blocks) is allocated through these: malloc/free, but the ESP32 firmware puts it
    // in PSRAM, keeping the internal RAM for the buffers that drawing touches every frame.
    // coldAlloc() never returns null: out of memory stops there with a message, not later at a null write.
    void* coldAlloc(size_t bytes);
    void coldFree(void* p);

    // A T allocated on the first edit and copied with its owner; until then get() returns the defaults.
    template <class T> class Lazy{
        public:
            Lazy() = default;
            Lazy(const Lazy& o) : p_(o.p_ ? make(o.p_.get()) : nullptr){}
            Lazy(Lazy&&) noexcept = default;
            Lazy& operator=(const Lazy& o){
                if(this != &o) p_.reset(o.p_ ? make(o.p_.get()) : nullptr);

                return *this;
            }
            Lazy& operator=(Lazy&&) noexcept = default;
            // A class member, not a local static: no initialization guard (a memory barrier on the ESP32) on every read.
            const T& get() const{ return p_ ? *p_ : defaults_; }
            T& edit(){
                if(!p_) p_.reset(make(nullptr));

                return *p_;
            }
            bool allocated() const{ return p_ != nullptr; }

        private:
            struct Free{
                void operator()(T* p) const{ p->~T(); coldFree(p); }
            };
            static T* make(const T* from){
                void* memory = coldAlloc(sizeof(T));
                return from ? new (memory) T(*from) : new (memory) T();
            }
            static const T defaults_;
            std::unique_ptr<T, Free> p_;
    };
    template <class T> const T Lazy<T>::defaults_{};

    // A std allocator over coldAlloc, for containers that drawing does not touch every frame.
    template <class T> struct ColdAllocator{
        using value_type = T;
        ColdAllocator() = default;
        template <class U> ColdAllocator(const ColdAllocator<U>&){}
        T* allocate(size_t count){ return static_cast<T*>(coldAlloc(count * sizeof(T))); }
        void deallocate(T* p, size_t){ coldFree(p); }
        template <class U> bool operator==(const ColdAllocator<U>&) const{ return true; }
        template <class U> bool operator!=(const ColdAllocator<U>&) const{ return false; }
    };

    // A vector that grows N elements at a time: growing never moves the elements, and it never needs one big free
    // piece of memory (on the boards 128 nodes of ~500 bytes would be 64 KB). Blocks come from coldAlloc.
    template <class T, size_t N> class BlockList{
        public:
            BlockList() = default;
            BlockList(const BlockList&) = delete;
            BlockList& operator=(const BlockList&) = delete;
            ~BlockList(){ clear(); }

            size_t size() const{ return size_; }
            bool empty() const{ return size_ == 0; }
            size_t capacity() const{ return blocks_.size() * N; }
            T& operator[](size_t i){ return blocks_[i / N][i % N]; }
            const T& operator[](size_t i) const{ return blocks_[i / N][i % N]; }
            T& back(){ return (*this)[size_ - 1]; }

            void reserve(size_t count){
                while(capacity() < count) blocks_.push_back(static_cast<T*>(coldAlloc(sizeof(T) * N)));
            }
            void push_back(T&& value){
                reserve(size_ + 1);
                new (&(*this)[size_]) T(std::move(value));
                ++size_;
            }
            // Shrinking frees the blocks left empty, but one: spawning and freeing around a block's end does not churn.
            void resize(size_t count){
                reserve(count);

                for(; size_ < count; ++size_) new (&(*this)[size_]) T();

                while(size_ > count) (*this)[--size_].~T();

                while(blocks_.size() > (size_ + N - 1) / N + 1){
                    coldFree(blocks_.back());
                    blocks_.pop_back();
                }
            }
            void clear(){
                resize(0);

                for(T* block : blocks_) coldFree(block);

                std::vector<T*>().swap(blocks_);
            }

            // Walks a block with a pointer, as fast as a vector's iterator.
            template <class L, class V> struct Iterator{
                L* list;
                size_t i;
                V* p;
                V& operator*() const{ return *p; }
                V* operator->() const{ return p; }
                Iterator& operator++(){
                    if(++i % N) ++p;
                    else p = i < list->size() ? &(*list)[i] : nullptr;
                    return *this;
                }
                bool operator!=(const Iterator& o) const{ return i != o.i; }
                bool operator==(const Iterator& o) const{ return i == o.i; }
            };
            Iterator<BlockList, T> begin(){ return {this, 0, size_ ? blocks_[0] : nullptr}; }
            Iterator<BlockList, T> end(){ return {this, size_, nullptr}; }
            Iterator<const BlockList, const T> begin() const{ return {this, 0, size_ ? blocks_[0] : nullptr}; }
            Iterator<const BlockList, const T> end() const{ return {this, size_, nullptr}; }

        private:
            std::vector<T*> blocks_;
            size_t size_ = 0;
    };

    struct AnimationKey{
        float time = 0;
        // Godot's ease() curve to the next key: 1 linear, above 1 in, below 1 out, negative in-out
        float transition = 1;
        jetscript::Value value;
        // method tracks: method(args) on the track's node
        std::string method;
        std::vector<jetscript::Value> args;
    };
    struct AnimationTrack{
        // the node, from the player's parent: "." (the parent), "Ship", "Ship/Gun"
        std::string path;
        // "position", "position:x", or a script variable; empty for a method track
        std::string property;
        // Godot's INTERPOLATION_NEAREST (0) or LINEAR (1)
        uint8_t interpolation = 1;
        // sorted by time
        std::vector<AnimationKey> keys;
        // Looked up the first time the track plays.
        mutable int propertyId = -2;
        mutable int8_t component = -1;
    };
    struct Animation{
        std::string name;
        float length = 1;
        // Godot's LOOP_NONE, LOOP_LINEAR, LOOP_PINGPONG
        uint8_t loop = 0;
        std::vector<AnimationTrack> tracks;
    };
    constexpr size_t MaxAnimations = 64, MaxTracks = 128, MaxKeys = 4096;

    // Fields of the less common node types (2D, camera, light, environment, physics, particles).
    struct NodeExtras{
        // Label / TextureRect / ColorRect (color and alpha are shared with materials)
        std::string text;
        // TextureRect: 0 = texture size
        Vec3 size{100, 30, 0};
        // integer pixel scale of Label text and TextureRect images
        uint8_t text_scale = 1;
        // Camera3D (vertical FOV in degrees, as in Godot)
        float fov = 75, near = 0.05f, far = 100;
        bool current = false;
        // DirectionalLight3D
        uint32_t light_color = 0xffffff;
        float energy = 1;
        // WorldEnvironment
        uint32_t background = 0x161c28, ambient = 0x4e586e;
        Background background_mode = Background::Color;
        uint32_t skyTop = 0x0a1a3a, skyHorizon = 0x3d6aa8, groundHorizon = 0x2a3442, groundBottom = 0x10141c;
        // panorama, index in Description::textures
        int32_t sky_texture = -1;
        // Godot's depth fog distances (m): Jet fades the geometry into the background between them, it has no fog color
        bool fog_enabled = false;
        float fog_begin = 10, fog_end = 100;
        // Physics (arcade, see physics.hpp); 2D bodies and shapes use the same fields, in pixels
        CollisionKind collision = CollisionKind::Box;
        Vec3 boxSize{1, 1, 1};
        // sphere/capsule radius; capsule total height (Godot)
        float radius = 0.5f, height = 2.0f;
        bool disabled = false;
        // CollisionShape2D: one_way_collision, bodies only land on it from above
        bool oneWay = false;
        // Godot's layer (what it is) and mask (what it detects)
        uint32_t collision_layer = 1, collision_mask = 1;
        // RigidBody3D (also friction/bounce of StaticBody3D)
        float mass = 1, gravity_scale = 1, linear_damp = 0, angular_damp = 0, friction = 1, bounce = 0;
        // initial (m/s, rad/s; 2D: px/s and angular_velocity.z)
        Vec3 linear_velocity{}, angular_velocity{};
        bool freeze = false;
        // CharacterBody3D, degrees
        float floor_max_angle = 45;
        // Area3D
        bool monitoring = true;
        // CPUParticles3D (Godot's names and defaults; emission in world space)
        bool emitting = true, one_shot = false, additive = true;
        // drawn over everything (no occlusion: cheaper on the device)
        bool no_depth_test = false;
        // particles per cycle, 1..MaxParticles
        uint16_t amount = 8;
        float lifetime = 1, lifetime_randomness = 0, explosiveness = 0;
        // local; spread is the cone half angle in degrees
        Vec3 direction{1, 0, 0};
        // initial_velocity_min/max, m/s
        float spread = 45, velocityMin = 0, velocityMax = 0;
        // speed lost per second, m/s² (smoke drifts and stops)
        float damping_min = 0, damping_max = 0;
        // m/s²
        Vec3 gravity{0, -9.8f, 0};
        ParticleStyle particle_style = ParticleStyle::Sparks;
        EmissionShape emission_shape = EmissionShape::Point;
        // emission_sphere_radius
        float emissionRadius = 1;
        // emission_box_extents (half size)
        Vec3 emissionBox{1, 1, 1};
        // Puff: colour goes from color to color_end, opacity from alpha to 0, diameter from puff_size to puff_size_end (m)
        uint32_t color_end = 0x404040;
        float puff_size = 0.2f, puff_size_end = 0.6f;
        // AnimatedSprite2D: frame ranges of the sprite sheet, as text: "idle 0-3 8, run 4-9 12, die 10-14 10 once"
        // (name, first-last frame, frames per second, "once" when it does not loop). The rest is the playing state.
        std::string animations, animation;
        float speed_scale = 1;
        bool autoplay = true, playing = false;
        float animTime = 0;
        // the animation playing, read from animations by play()
        uint16_t animFirst = 0, animLast = 0;
        float animFps = 5;
        bool animLoop = true;
        // Camera2D (current says whether it is enabled)
        float zoom[2] = {1, 1};
        // Timer: wait_time is lifetime, autostart is autoplay, and it runs while playing (time left in Live::cycle).
        bool paused = false;
        // AnimationPlayer: `animation` is the autoplay name, animTime the position in playerCurrent.
        std::vector<Animation> playerAnimations;
        int16_t playerCurrent = -1;
        // just started: keys at its first instant fire too
        bool playerFresh = false;
        float playerSpeed = 1;
    };

    // ints stay ints; vectors and colours go per component
    jetscript::Value interpolate(const jetscript::Value& a, const jetscript::Value& b, float weight);
    float componentOf(const jetscript::Value& v, int component);
    jetscript::Value withComponent(const jetscript::Value& v, int component, float x);
    // Godot's ease()
    float easeCurve(float x, float curve);

    // Godot conventions: metres, Y up, -Z forward, rotation in degrees applied in YXZ order.
    struct NodeDesc{
            uint32_t id = 1;
            // index in Description::nodes; parents precede children
            int32_t parent = -1;
            NodeType type = NodeType::Node3D;
            // next to type: read by every transform pass
            Vec3 position{}, rotation{}, scale{1, 1, 1};
            bool visible = true;
            std::string name = "Node3D";
            // MeshInstance3D, material is a StandardMaterial3D subset
            Shape shape = Shape::Box;
            // albedo; multiplies the texture
            uint32_t color = 0xe6a45c;
            Shading shading = Shading::Gouraud;
            uint16_t segments = 12;
            // index in Description::textures
            int32_t texture = -1;
            float uv_scale[2] = {1, 1}, uv_offset[2] = {0, 0};
            bool bilinear = false, repeat = true, perspective = true;
            // 0..1
            float alpha = 1;
            // Phong highlight / water ripple strength, 0..1
            float specular = 0.5f;
            uint8_t shininess = 32;
            CullMode cull = CullMode::Back;
            DrawOrder draw_order = DrawOrder::Default;
            bool billboard = false;
            // GeometryInstance3D visibility_range_begin, _begin_margin, _end, _end_margin in cm (= Jet units), 0 = no limit;
            // fade is Godot's VISIBILITY_RANGE_FADE_SELF
            uint16_t visibility_begin = 0, visibility_begin_margin = 0, visibility_end = 0, visibility_end_margin = 0;
            bool visibility_fade = false;
            // Sprite3D, as in Godot: frame `frame` of an hframes × vframes sheet, `pixel_size` metres per texel.
            uint16_t hframes = 1, vframes = 1, frame = 0;
            bool flip_h = false, flip_v = false;
            float pixel_size = 0.01f;
            // 2D nodes (Godot's Node2D/Sprite2D): the rotation is rotation.z in degrees, clockwise as y grows downwards.
            int16_t z_index = 0;
            bool centered = true;
            // Sprite2D: from the node's origin; Camera2D: added to its position
            float offset[2] = {0, 0};
            // diffuse reflectance 0..1
            float diffuse = 1;
            // albedo texture used as environment panorama (chrome, glass)
            bool reflection = false;
            // Shape::Mesh: index in Description::meshes
            int32_t mesh = -1;
            // index in Description::scripts
            int32_t script = -1;
            std::vector<ScriptOverride> overrides;

            // Every live node keeps its NodeDesc and most are meshes or sprites, so the other types' fields live
            // in a block of their own: more() reads them (the defaults when missing), extras() allocates it to write.
            const NodeExtras& more() const{ return extras_.get(); }
            NodeExtras& extras(){ return extras_.edit(); }
            bool hasExtras() const{ return extras_.allocated(); }

        private:
            Lazy<NodeExtras> extras_;
        // A signal connected in the editor (Godot's [connection] of a scene), its nodes by index in the scene.
    };
    struct SceneConnection{
        int32_t from = 0, to = 0;
        std::string signal, method;
        // CONNECT_ONE_SHOT (4)
        uint32_t flags = 0;
    };
    // Source in the editor, bytecode in packages.
    struct ScriptFile{ std::string path, source; std::vector<uint8_t> bytecode; };
    // Bit i of the input mask is actions[i]. Keys are Godot key names, used by the PC players.
    // Board buttons are wired per action in the firmware (generated_inputs.hpp).
    struct InputAction{
        std::string name;
        std::vector<std::string> keys;
    };
    constexpr size_t MaxActions = 32, MaxActionKeys = 8;
    // Godot's built-in ui_* actions with their keys (arrows/WASD, Space/Enter, Escape/Backspace).
    std::vector<InputAction> defaultActions();
    // Other scenes share the Description's scripts, textures and meshes.
    struct SceneDesc{
        // res://...jetscene
        std::string path;
        std::vector<NodeDesc> nodes;
        std::vector<SceneConnection> connections;
        // where its nodes start in the package (decode_pack)
        uint32_t offset = 0;
    };
    constexpr size_t MaxScenes = 64;
    // per scene
    constexpr size_t MaxConnections = 512;

    // Godot autoloads, scripts only: they survive scene changes and any script reaches them by name (Game.score).
    struct Autoload{
        std::string name;
        int32_t script = -1;
    };
    constexpr size_t MaxAutoloads = 16;
    // node ids of the autoloads; scene ids stay below
    constexpr uint32_t AutoloadIdBase = 0x7fff0000u;
    // node ids made by instantiate(), below the autoloads'
    constexpr uint32_t InstanceIdBase = 0x40000000u;
    // Tweens and their Tweeners, never reused
    constexpr uint32_t TweenIdBase = 0x60000000u, TweenerIdBase = 0x68000000u;
    struct Description{
        std::string name = "Scene";
        // the starting scene, held in `nodes`
        std::string path;
        int width = DefaultWidth, height = DefaultHeight;
        std::vector<InputAction> actions = defaultActions();
        // depth-first order, nodes[0] is the root
        std::vector<NodeDesc> nodes;
        std::vector<SceneConnection> connections;
        // where `nodes` start in the package (decode_pack)
        uint32_t nodesOffset = 0;
        std::vector<SceneDesc> scenes;
        std::vector<Autoload> autoloads;
        std::vector<ScriptFile> scripts;
        std::vector<TextureAsset> textures;
        std::vector<MeshAsset> meshes;
    };
    bool validate(const Description&, std::string& error);
    uint16_t rgb565(uint32_t rgb);

    // Bits of the default actions (the first six of defaultActions()).
    enum Action : uint32_t{ UiLeft = 1, UiRight = 2, UiUp = 4, UiDown = 8, UiAccept = 16, UiCancel = 32 };

    struct LogLine{
        std::string text;
        bool error = false;
        // script location for errors, when known
        std::string path;
        int line = 0;
    };
    struct ScriptDiagnostic{ std::string path; int line = 0, column = 0; std::string message; };

    class ScriptHost;

    // No Godot dependency here: this library also builds for ESP-IDF.
    namespace physics{ struct World; struct Contact; }
}
namespace Renderer{ class ParticleSystem; }
namespace jetstudio{

    // RAM on the 32-bit boards, in bytes: an estimate from sizes measured on a Cortex-M7 (the ESP32-S3 is close).
    // What the package needs whatever the scene, then each scene; only one scene is in memory at a time.
    struct MemoryEstimate{
        std::string scene;
        // live nodes, their extra fields, body state, script instances, what they draw
        size_t nodes = 0;
        // Jet objects and materials, and the geometry shared by identical meshes
        size_t meshes = 0;
        // particle pools
        size_t particles = 0;
        // Jet's per-frame triangle queues
        size_t triangles = 0;
        // the scene's description while it is being built
        size_t loading = 0;
        // The description is freed before the first frame reserves the queues.
        size_t total() const{ return nodes + meshes + particles + std::max(triangles, loading); }
    };
    struct MemoryReport{
        // runtime, Jet and VM state that does not depend on the content
        size_t engine = 0;
        // one field buffer and the two scanout rows
        size_t frames = 0;
        // the second field buffer (drawing while the previous field is sent)
        size_t field = 0;
        // every texture of the package: only the palette when it is read in place
        size_t textures = 0;
        size_t scripts = 0;
        std::vector<MemoryEstimate> scenes;
        size_t largestScene() const;
    };

    class Runtime{
        public:
            Runtime();
            ~Runtime();
            // Stops play. On error the previous scene is kept.
            // std::move it in to skip the copy
            bool load(Description, std::string& error);
            // An exported package that outlives the runtime (flash on the device): only the scene being played stays
            // in RAM, the others are read again from the package when entered.
            // textures_in_place: palette textures are read from the package, not copied (boards without PSRAM).
            bool load_pack(const uint8_t* data, size_t size, std::string& error, bool textures_in_place = false);
            // Compiles scripts and runs _ready(). On compile errors nothing runs.
            bool start(std::vector<ScriptDiagnostic>& errors);
            // back to the authored state
            void stop();
            bool playing() const{ return playing_; }
            // Scripts only run while playing.
            void update(float delta);
            void render();
            // Lets the device split rasterizeBand across cores. The executor must rasterize every row and join its workers.
            void setRasterExecutor(Renderer::Scene::RasterExecutor executor){ executor_ = executor; }
            // render() fills bufferWidth() x bufferRows() pixels.
            int width() const{ return width_; }
            int height() const{ return height_; }
            int bufferWidth() const{ return width_ / BufferColumnStep; }
            int bufferRows() const{ return height_ / BufferRowStep; }
            // Devices render straight into their DMA buffers. nullptr goes back to the internal one.
            void set_framebuffer(uint16_t* buffer);
            // Row parity (0 even, 1 odd) the next render() fills with FIELD_BUFFERS, else 0.
            int nextField() const{ return FIELD_BUFFERS ? (scene_.frameCounter % 2 == 0 ? 1 : 0) : 0; }
            // 2D nodes of the last render(), for composeOverlayRow().
            const std::vector<OverlayItem>& overlay() const{ return overlay_; }
            // Editor viewpoint (Godot coordinates, rotation in radians, vertical FOV).
            void set_editor_camera(Vec3 position, Vec3 rotation, float fov);
            // Use the editor viewpoint even while playing or when the scene has a camera.
            void use_editor_camera(bool enabled){ editorView_ = enabled; }
            // Editor 2D view, with the editor viewpoint: only the canvas, seen from `origin` (the canvas point at the top-left
            // corner) at `zoom`. Controls sit on the screen rectangle at the canvas origin, as Godot shows a CanvasLayer.
            void set_editor_canvas(bool enabled, float zoom = 1, Vec3 origin = {});
            // node id or -1
            int pick(int x, int y);
            // bit i = Description::actions[i]
            void set_input(uint32_t pressed);
            // Deferred to the end of the frame, like Godot. False if the package lacks the scene.
            bool changeScene(std::string_view path);
            const std::string& current_scene() const{ return currentScene_; }
            // -1 when the Input Map has no such action
            int actionIndex(std::string_view name) const;
            std::vector<LogLine> take_log();

            const std::vector<uint16_t>& pixels() const{ return color_; }
            const Description& description() const{ return description_; }
            int triangles() const{ return scene_.lastFrameRasterizedTriangles + int(particleTriangles_); }
            // all CPUParticles3D, while playing
            int liveParticles() const;
            size_t framebuffer_bytes() const{ return (color_.size() + depth_.size()) * sizeof(uint16_t); }
            size_t meshCount() const{ return meshes_; }
            size_t node_count() const{ return nodes_.size(); }
            // Screen rectangle of a drawn 2D node or Control, from the last render (editor selection).
            bool drawn_rect(uint32_t id, int& x, int& y, int& w, int& h) const;
            // Editor: a CollisionShape2D's outline on the screen, as x, y pairs of a closed polygon.
            std::vector<float> shape_outline(uint32_t id) const;
            // The loaded description (all its scenes) on a board; textures_in_place as the STM32 firmware does.
            MemoryReport estimateMemory(bool textures_in_place) const;
            // RAM of the live nodes: their array, the extra fields and the body state of the nodes that have them.
            size_t nodeBytes() const;
            // Live state of a node during play (for the editor); false if missing/freed.
            bool node_state(uint32_t id, NodeDesc& out) const;
            // Editor preview: the values at `time`, without method keys.
            bool preview_animation(uint32_t player, std::string_view name, float time);

            // Material and UV values last pushed to a node's Jet object.
            struct Applied{
                uint32_t color = 0;
                Shading shading = Shading::Gouraud;
                float alpha = 1, specular = 0, diffuse = 1;
                uint8_t shininess = 0;
                bool bilinear = false, repeat = true, perspective = true, billboard = false;
                CullMode cull = CullMode::Back;
                float uv_scale[2] = {1, 1}, uv_offset[2] = {0, 0};
                uint16_t visibility[4] = {0, 0, 0, 0};
                bool visibility_fade = false;
                static Applied of(const NodeDesc& d){
                    return {d.color, d.shading, d.alpha, d.specular, d.diffuse, d.shininess, d.bilinear, d.repeat, d.perspective,
                            d.billboard, d.cull, {d.uv_scale[0], d.uv_scale[1]}, {d.uv_offset[0], d.uv_offset[1]},
                            {d.visibility_begin, d.visibility_begin_margin, d.visibility_end, d.visibility_end_margin}, d.visibility_fade};
                }
            };

            // Node state while playing, shared with ScriptHost.
            struct Motion{
                // m/s, rad/s, accumulated force for the next step
                Vec3 velocity{}, angular{}, force{};
                bool onFloor = false, onWall = false, onCeiling = false, sleeping = false;
                Vec3 floorNormal{0, 1, 0};
                // CharacterBody3D: what it stands on (moving platforms)
                int floorBody = -1;
                // world position at the end of the previous physics tick
                Vec3 lastOrigin{};
                // how far it moved during that tick
                Vec3 lastMotion{};
                float stillTime = 0;
                // Area3D/Area2D: ids of the bodies inside
                std::vector<uint32_t> overlaps;
                // and of the other areas
                std::vector<uint32_t> areas;
            };
            struct Live{
                // Hot in every transform pass: kept together since each scattered field costs a PSRAM cache miss on the ESP32.
                int32_t parent = -1;
                bool freed = false, freeRequested = false, worldVisible = true, localValid = false;
                // false for an instantiated scene until add_child() puts it in the tree
                bool inTree = true;
                // 2D: z_index plus the parents'
                int16_t canvasZ = 0;
                Transform world;
                // Cached from d.position/rotation/scale, recomputed when they change.
                Transform local;
                Vec3 localPosition, localRotation, localScale;
                NodeDesc d;
                std::vector<int32_t> children;
                Vec3 bakedScale{1, 1, 1};
                // farthest baked vertex from the origin (Jet units)
                float jetRadius = 0;
                Renderer::Object* object = nullptr;
                Renderer::Material* material = nullptr;
                Renderer::Texture* texture = nullptr;
                // albedo-tinted palette, or RGB565 pixels (tinted and/or expanded)
                std::vector<uint16_t> tinted;
                // winding reversed to emulate front-face culling
                bool flipped = false;
                std::vector<Renderer::Object::Vertex> baseVertices;
                Applied applied;
                // pose last pushed to the Jet object
                Transform syncedWorld;
                bool synced = false, syncedVisible = false;
                // Physics (physics.cpp): only bodies allocate it.
                const Motion& motion() const{ return motion_.get(); }
                Motion& editMotion(){ return motion_.edit(); }
                Lazy<Motion> motion_;
                // CPUParticles3D (particles.cpp)
                // index in Runtime::particles_
                int32_t particles = -1;
                // seconds into the current emission cycle
                float cycle = 0;
                // particles emitted in this cycle
                int emitted = 0;
            };
            // 8 nodes (~4 KB on the boards) per block.
            using Nodes = BlockList<Live, 8>;

        private:
            friend class ScriptHost;
            bool build(std::string& error);
            void createVisual(int32_t index);
            void releaseNode(Live& n);
            void collectPhysics();
            void compactNodes();
            bool sceneDescs(std::string_view path, std::vector<NodeDesc>& out, std::vector<SceneConnection>& connections) const;
            // The editor's connections of a scene, between the nodes with these ids (by index in the scene).
            void connectScene(const std::vector<SceneConnection>& connections, const std::vector<uint32_t>& ids);
            void attachScript(uint32_t id);
            // Godot's PackedScene.instantiate() and Node.add_child()/remove_child(), for the script host.
            uint32_t instantiate(std::string_view path, std::string& error);
            bool add_child(int32_t parent, int32_t child, std::string& error);
            bool remove_child(int32_t parent, int32_t child, std::string& error);
            void enterTree(int32_t root);
            Renderer::Object* makeMesh(const NodeDesc&, Renderer::Material*) const;
            void enterScene();
            void switchScene();
            const std::vector<NodeDesc>* nodesOf(std::string_view path, std::vector<SceneConnection>* connections = nullptr);
            void startAutoloads();
            void updateTransforms(bool sync = true);
            // Physics (physics.cpp)
            void physicsTick(float dt);
            void physicsBegin();
            void physicsStep(float dt);
            void updateAreas();
            void moveBody(int index, const Transform& world, bool rotated = true);
            void wake(Live& n);
            // motion: how far the body just moved (one-way shapes only stop what comes down onto them).
            bool deepestContact(int index, Vec3 motion, physics::Contact& best, int& other) const;
            bool move_and_slide(int index);
            // CPUParticles3D (particles.cpp)
            void startParticles();
            void startParticle(Live& n);
            void updateParticles(float dt);
            void updateAnimations(float dt);
            void startTimer(Live& n, float wait_time = -1);
            // Tween steps run in sequence, the tweeners of a step together.
            struct Tweener{
                enum class Kind : uint8_t{ Property, Interval, Callback, Method };
                Kind kind = Kind::Property;
                uint32_t id = 0;
                int step = 0;
                // Property: the object and its property; "position:x" tweens one component
                uint32_t target = 0;
                int property = -1;
                int8_t component = -1;
                jetscript::Value from, to, start, end, callable;
                bool hasFrom = false, relative = false, started = false, finished = false;
                float duration = 0, delay = 0;
                // Godot's Tween.TRANS_LINEAR, Tween.EASE_IN_OUT
                uint8_t trans = 0, ease = 2;
            };
            struct Tween{
                // bound: the node it dies with (0: the SceneTree's)
                uint32_t id = 0, bound = 0;
                std::vector<Tweener> tweeners;
                // loops 0: forever
                int steps = 0, step = 0, loops = 1, loop = 0;
                bool parallel = false, joinNext = false, newStep = false, running = true, started = false, done = false;
                float speed = 1, stepTime = 0, elapsed = 0;
                uint8_t trans = 0, ease = 2;
            };
            uint32_t create_tween(uint32_t bound);
            Tween* tweenOf(uint32_t id);
            Tweener* tweenerOf(uint32_t id);
            Tweener& addTweener(Tween& t, Tweener::Kind kind);
            void updateTweens(float dt);
            bool advanceTween(Tween& t, float time);
            void startTweener(Tweener& tw);
            void applyTweener(Tweener& tw, float progress);
            void killTweens(bool keepAutoloads);
            bool playPlayer(Live& n, std::string_view name, float speed, bool from_end);
            void stopPlayer(Live& n, bool keepState);
            void seekPlayer(Live& n, float time, bool update);
            void advancePlayer(Live& n, float dt);
            void applyPlayer(const Live& n, const Animation& a, float time);
            void fireMethodKeys(const Live& n, const Animation& a, float from, float to);
            int animationTarget(const Live& player, const std::string& path) const;
            // Queued while walking nodes_ and run after it: scripts may add and free nodes.
            struct Deferred{ uint32_t source = 0; const char* signal = nullptr; jetscript::Value callable; std::vector<jetscript::Value> args; };
            void deferSignal(uint32_t source, const char* signal, std::vector<jetscript::Value> args = {});
            void runDeferred();
            bool playAnimation(Live& n, std::string_view name);
            void renderParticles();
            struct ParticleView;
            unsigned drawPuffs(const Live& n, const ParticleView& view);
            unsigned drawParticles(const Live& n, const ParticleView& view);
            void buildOcclusion(const ParticleView& view, int x0, int y0, int x1, int y1, float farZ);
            bool occluded(const ParticleView& view, float x, float y, float z) const;
            void restartParticles(Live& n);
            struct ParticlePool;
            void emitParticle(Live& n, ParticlePool& pool, float age);
            float random01();
            void syncObject(Live&);
            void reserveTriangleQueues();
            void bake(Live&, Vec3 scale);
            void share(Live&, Vec3 scale);
            void applyCamera();
            void applyEnvironment();
            void applyMaterial(Live&);
            void prepareBackground();
            void updateReflections();
            void prepareOverlay();
            void prepareCanvas();
            void canvasView(float& zoomX, float& zoomY, float& shiftX, float& shiftY) const;
            void drawOverlay();
            int pickOverlay(int x, int y) const;
            void log(std::string text, bool error, std::string path = {}, int line = 0);
            void flushScriptErrors();
            void freePending();
            int indexOf(uint32_t id) const;

            void resize(int width, int height);
            int width_ = DefaultWidth, height_ = DefaultHeight;
            std::vector<uint16_t> color_, depth_;
            // framebuffer being drawn (color_ or a device buffer)
            uint16_t* target_ = nullptr;
            std::vector<OverlayItem> overlay_;
            Renderer::Camera camera_;
            Renderer::DirectionalLight sun_;
            Renderer::AmbientLight ambient_;
            std::vector<std::unique_ptr<Renderer::Material>> materials_;
            std::vector<std::unique_ptr<Renderer::Object>> objects_;
            // Jet's per-frame triangle queues, sized once per scene
            bool queuesReserved_ = false;
            // Baked meshes shared by nodes with the same mesh, scale and UVs; a copy lives while some node draws it.
            std::map<std::array<int32_t, 11>, std::weak_ptr<const Renderer::Object>> sharedMeshes_;
            // what shared copies point at: every node draws them with its own material
            Renderer::Material sharedMaterial_;
            std::vector<std::unique_ptr<Renderer::Texture>> textures_;
            std::vector<uint16_t> gradient_;
            std::vector<int32_t> skyColumns_, skyRows_;
            Basis viewBasis_;
            float viewFov_ = 70;
            Renderer::Scene scene_;
            Renderer::Scene::RasterExecutor executor_ = nullptr;
            std::unique_ptr<physics::World> physics_;
            float physicsTime_ = 0, physicsDelta_ = 1.0f / 60.0f;
            // the scene has collision shapes in bodies or areas
            bool hasPhysics_ = false;
            // CollisionShape3D node -> its body or area
            std::vector<std::pair<int32_t, int32_t>> shapeOwners_;
            // bodies and areas with shapes, once each
            std::vector<int32_t> collisionObjects_;
            // scripts ran after the last updateTransforms()
            bool transformsStale_ = false;
            // the scene that is built
            const std::vector<NodeDesc>* sceneNodes_ = nullptr;
            // set by load_pack()
            const uint8_t* pack_ = nullptr;
            size_t packSize_ = 0;
            // a scene read from the package, until build() copies it
            std::vector<NodeDesc> packNodes_;
            // of the scene being built, until enterScene() makes them
            std::vector<SceneConnection> sceneConnections_;
            // its nodes' ids, by index in the scene, while they wait
            std::vector<uint32_t> sceneIds_;
            std::string currentScene_, pendingScene_;
            std::vector<std::shared_ptr<const jetscript::Script>> compiled_;
            // A CPUParticles3D's particles while playing, in Jet's units: about as many as its amount.
            // Cold memory like Jet's own pools (PSRAM on the ESP32): Jet's transform scratch wants the internal RAM.
            struct Particle{ Renderer::Vec3f pos, vel; float life = 0, maxLife = 0, damping = 0; bool active = false; };
            struct ParticlePool{ std::vector<Particle, ColdAllocator<Particle>> particles; bool used = false; };
            // one per CPUParticles3D while playing; freed slots are reused
            std::vector<ParticlePool> particles_;
            // Jet's fixed pool of 200, refilled with each emitter's particles to draw them.
            struct FreeDrawer{ void operator()(Renderer::ParticleSystem* p) const; };
            std::unique_ptr<Renderer::ParticleSystem, FreeDrawer> particleDrawer_;
            // particle emission (xorshift)
            uint32_t random_ = 0x9e3779b9;
            // drawn in the last frame
            unsigned particleTriangles_ = 0;
            // The device has no z-buffer: opaque mesh depth in 4x4 pixel cells hides the particles behind them.
            std::vector<uint16_t> occlusion_;
            // scratch: projected vertices
            std::vector<float> occlusionPoints_;
            Description description_;
            Nodes nodes_;
            std::unordered_map<uint32_t, int32_t> index_;
            size_t meshes_ = 0;
            bool playing_ = false, editorView_ = true;
            bool editorCanvas_ = false;
            float editorCanvasZoom_ = 1;
            Vec3 editorCanvasOrigin_;
            bool canvasOverride() const{ return editorCanvas_ && editorView_; }
            Vec3 editorPosition_{4, 3, 6}, editorRotation_{-0.4f, 0.6f, 0};
            float editorFov_ = 70;
            // "Just pressed" is true for one frame in _process and for one tick in _physics_process, as in Godot.
            uint32_t input_ = 0, previousInput_ = 0, physicsPreviousInput_ = 0;
            bool inPhysics_ = false;
            bool pendingFree_ = false;
            // a node was added under one that comes later in nodes_
            bool reorderNeeded_ = false;
            uint32_t nextInstanceId_ = InstanceIdBase;
            std::vector<LogLine> log_;
            std::vector<Deferred> deferred_;
            std::vector<Tween> tweens_;
            uint32_t nextTweenId_ = TweenIdBase, nextTweenerId_ = TweenerIdBase;
            std::unique_ptr<ScriptHost> host_;
            std::unique_ptr<jetscript::VM> vm_;
    };

#ifndef JETSCRIPT_NO_COMPILER
    // Packages carry bytecode only, no script source.
    std::vector<uint8_t> encode_pack(const Description&, std::string& error);
#endif
    bool decode_pack(const uint8_t*, size_t, Description&, std::string& error, bool textures_in_place = false);
    // header only
    bool packResolution(const uint8_t*, size_t, int& width, int& height);
    // One scene's nodes and connections, read again from the package at SceneDesc::offset or Description::nodesOffset.
    bool decodeNodes(const uint8_t*, size_t, uint32_t offset, std::vector<NodeDesc>&, std::vector<SceneConnection>&,
                      std::string& error);
}
