#include "runtime.hpp"
#include "script_host.hpp"
#include <cstring>

// .jetpack: little endian, strings and byte blocks are u32 length + data. encode_pack defines the layout.
// Scripts travel as bytecode so devices only need the VM. Everything is validated again after decoding.
namespace jetstudio {
namespace {
constexpr uint32_t Magic = 0x5054454a, Version = 16;

struct Writer {
    std::vector<uint8_t> out;
    void u8(uint8_t v) { out.push_back(v); }
    void u16(uint16_t v) { u8(uint8_t(v)); u8(uint8_t(v >> 8)); }
    void u32(uint32_t v) { for (int i = 0; i < 4; ++i) out.push_back(uint8_t(v >> (8 * i))); }
    void u64(uint64_t v) { u32(uint32_t(v)); u32(uint32_t(v >> 32)); }
    void f32(float f) { uint32_t v; std::memcpy(&v, &f, 4); u32(v); }
    void vec(Vec3 v) { f32(v.x); f32(v.y); f32(v.z); }
    void str(std::string_view s) { u32(uint32_t(s.size())); out.insert(out.end(), s.begin(), s.end()); }
    void bytes(const std::vector<uint8_t>& b) { u32(uint32_t(b.size())); out.insert(out.end(), b.begin(), b.end()); }
    bool value(const jetscript::Value& v) {
        using jetscript::Type;
        u8(uint8_t(v.type()));
        switch (v.type()) {
        case Type::Nil: return true;
        case Type::Bool: u8(v.as_bool() ? 1 : 0); return true;
        case Type::Int: u64(uint64_t(v.as_int())); return true;
        case Type::Float: f32(v.as_real()); return true;
        case Type::String: str(v.as_string()); return true;
        case Type::Vector3: { const auto x = v.as_vector3(); f32(x.x); f32(x.y); f32(x.z); return true; }
        case Type::Vector2: { const auto x = v.as_vector2(); f32(x.x); f32(x.y); return true; }
        case Type::Color: { const auto c = v.as_color(); f32(c.r); f32(c.g); f32(c.b); return true; }
        case Type::Array:
            u32(uint32_t(v.array_size()));
            for (size_t i = 0; i < v.array_size(); ++i) {
                const auto item = v.array_get(i);
                if (item.type() == Type::Array || item.type() == Type::Dictionary || item.type() == Type::Object || !value(item)) return false;
            }
            return true;
        default: return false;
        }
    }
};

struct Reader {
    const uint8_t* p;
    size_t remaining;
    bool ok = true;
    bool take(size_t n) {
        if (!ok || remaining < n) { ok = false; return false; }
        return true;
    }
    uint8_t u8() { if (!take(1)) return 0; const uint8_t v = *p; ++p; --remaining; return v; }
    uint16_t u16() { const uint16_t a = u8(); return uint16_t(a | uint16_t(u8()) << 8); }
    uint32_t u32() {
        if (!take(4)) return 0;
        const uint32_t v = uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24;
        p += 4; remaining -= 4; return v;
    }
    uint64_t u64() { const uint64_t low = u32(); return low | uint64_t(u32()) << 32; }
    float f32() { const uint32_t v = u32(); float f; std::memcpy(&f, &v, 4); return f; }
    Vec3 vec() { const float x = f32(), y = f32(); return {x, y, f32()}; }
    std::string str(size_t limit) {
        const uint32_t n = u32();
        if (n > limit || !take(n)) { ok = false; return {}; }
        std::string s(reinterpret_cast<const char*>(p), n);
        p += n; remaining -= n;
        return s;
    }
    std::vector<uint8_t> bytes(size_t limit) {
        const uint32_t n = u32();
        if (n > limit || !take(n)) { ok = false; return {}; }
        std::vector<uint8_t> b(p, p + n);
        p += n; remaining -= n;
        return b;
    }
    jetscript::Value value(bool nested = false) {
        using jetscript::Type;
        const auto type = Type(u8());
        switch (type) {
        case Type::Nil: return {};
        case Type::Bool: { const uint8_t b = u8(); if (b > 1) ok = false; return jetscript::Value(b == 1); }
        case Type::Int: return jetscript::Value(jetscript::Int(u64()));
        case Type::Float: return jetscript::Value(f32());
        case Type::String: return jetscript::Value(str(4096));
        case Type::Vector3: { const auto v = vec(); return jetscript::Value(jetscript::Vector3{v.x, v.y, v.z}); }
        case Type::Vector2: { const float x = f32(); return jetscript::Value(jetscript::Vector2{x, f32()}); }
        case Type::Color: { const auto v = vec(); return jetscript::Value(jetscript::Color{v.x, v.y, v.z}); }
        case Type::Array: {
            const uint32_t n = u32();
            if (nested || n > 64) { ok = false; return {}; }
            auto array = jetscript::Value::new_array();
            for (uint32_t i = 0; i < n && ok; ++i) array.array_push(value(true));
            return array;
        }
        default: ok = false; return {};
        }
    }
};
} // namespace

#ifndef JETSCRIPT_NO_COMPILER
std::vector<uint8_t> encode_pack(const Description& d, std::string& error) {
    if (!validate(d, error)) return {};
    Writer w;
    w.u32(Magic); w.u32(Version);
    w.str(d.name);
    w.u16(uint16_t(d.width)); w.u16(uint16_t(d.height));
    w.u8(uint8_t(d.actions.size())); // the Input Map, early so tools can read it
    for (const auto& action : d.actions) {
        w.str(action.name);
        w.u8(uint8_t(action.keys.size()));
        for (const auto& key : action.keys) w.str(key);
    }
    w.u32(uint32_t(d.scripts.size()));
    ScriptHost host;
    std::vector<std::string> autoload_names;
    for (const auto& a : d.autoloads) autoload_names.push_back(a.name);
    host.set_autoloads(std::move(autoload_names));
    for (const auto& s : d.scripts) {
        w.str(s.path);
        if (!s.bytecode.empty()) { w.bytes(s.bytecode); continue; }
        const auto compiled = jetscript::compile(s.source, s.path, host);
        if (!compiled.script) {
            const auto& e = compiled.errors.front();
            error = s.path + ":" + std::to_string(e.line) + ": " + e.message;
            return {};
        }
        w.bytes(jetscript::save_bytecode(*compiled.script));
    }
    w.u32(uint32_t(d.textures.size()));
    for (const auto& t : d.textures) {
        w.str(t.path); w.u16(t.width); w.u16(t.height); w.u8(t.has_alpha ? 1 : 0); w.u16(t.alpha_color);
        w.u16(uint16_t(t.palette.size()));
        for (uint16_t c : t.palette) w.u16(c);
        if (t.palette.empty()) for (uint16_t c : t.pixels) w.u16(c);
        else w.out.insert(w.out.end(), t.index_data(), t.index_data() + size_t(t.width) * t.height);
    }
    w.u32(uint32_t(d.meshes.size()));
    for (const auto& m : d.meshes) {
        w.str(m.path);
        w.u32(uint32_t(m.vertices.size()));
        for (const auto& v : m.vertices) { w.vec(v.position); w.vec(v.normal); w.f32(v.u); w.f32(v.v); }
        w.u32(uint32_t(m.indices.size()));
        for (uint16_t i : m.indices) w.u16(i);
    }
    auto write_nodes = [&](const std::vector<NodeDesc>& nodes, const std::vector<SceneConnection>& connections) {
        w.u32(uint32_t(nodes.size()));
        for (const auto& n : nodes) {
            w.u32(n.id); w.u32(uint32_t(n.parent)); w.u8(uint8_t(n.type)); w.str(n.name);
            w.vec(n.position); w.vec(n.rotation); w.vec(n.scale); w.u8(n.visible ? 1 : 0);
            w.u8(uint8_t(n.shape)); w.u32(n.color); w.u8(uint8_t(n.shading)); w.u16(n.segments);
            w.f32(n.more().fov); w.f32(n.more().near); w.f32(n.more().far); w.u8(n.more().current ? 1 : 0);
            w.u32(n.more().light_color); w.f32(n.more().energy); w.u32(n.more().background); w.u32(n.more().ambient);
            w.u32(uint32_t(n.texture)); w.f32(n.uv_scale[0]); w.f32(n.uv_scale[1]); w.f32(n.uv_offset[0]); w.f32(n.uv_offset[1]);
            w.u8(uint8_t((n.bilinear ? 1 : 0) | (n.repeat ? 2 : 0) | (n.perspective ? 4 : 0) | (n.billboard ? 8 : 0) | (n.reflection ? 16 : 0)));
            w.f32(n.alpha); w.f32(n.specular); w.u8(n.shininess); w.u8(uint8_t(n.cull)); w.u8(uint8_t(n.draw_order));
            w.u8(uint8_t(n.more().background_mode)); w.u32(n.more().sky_top); w.u32(n.more().sky_horizon); w.u32(n.more().ground_horizon); w.u32(n.more().ground_bottom);
            w.u32(uint32_t(n.more().sky_texture));
            w.f32(n.diffuse); w.u32(uint32_t(n.mesh)); w.str(n.more().text); w.vec(n.more().size); w.u8(n.more().text_scale);
            w.u8(uint8_t(n.more().collision)); w.vec(n.more().box_size); w.f32(n.more().radius); w.f32(n.more().height);
            w.u8(uint8_t((n.more().disabled ? 1 : 0) | (n.more().freeze ? 2 : 0) | (n.more().monitoring ? 4 : 0) | (n.more().one_way ? 8 : 0)));
            w.u32(n.more().collision_layer); w.u32(n.more().collision_mask);
            w.f32(n.more().mass); w.f32(n.more().gravity_scale); w.f32(n.more().linear_damp); w.f32(n.more().angular_damp); w.f32(n.more().friction); w.f32(n.more().bounce);
            w.vec(n.more().linear_velocity); w.vec(n.more().angular_velocity); w.f32(n.more().floor_max_angle);
            w.u8(uint8_t((n.more().emitting ? 1 : 0) | (n.more().one_shot ? 2 : 0) | (n.more().additive ? 4 : 0) | (n.more().no_depth_test ? 8 : 0))); w.u16(n.more().amount);
            w.f32(n.more().lifetime); w.f32(n.more().lifetime_randomness); w.f32(n.more().explosiveness); w.vec(n.more().direction); w.f32(n.more().spread);
            w.f32(n.more().velocity_min); w.f32(n.more().velocity_max); w.f32(n.more().damping_min); w.f32(n.more().damping_max); w.vec(n.more().gravity);
            w.u8(uint8_t(n.more().particle_style)); w.u8(uint8_t(n.more().emission_shape)); w.f32(n.more().emission_radius); w.vec(n.more().emission_box);
            w.u32(n.more().color_end); w.f32(n.more().puff_size); w.f32(n.more().puff_size_end);
            w.u16(n.hframes); w.u16(n.vframes); w.u16(n.frame); w.u8(uint8_t((n.flip_h ? 1 : 0) | (n.flip_v ? 2 : 0))); w.f32(n.pixel_size);
            w.u16(uint16_t(n.z_index)); w.u8(n.centered ? 1 : 0); w.f32(n.offset[0]); w.f32(n.offset[1]);
            w.str(n.more().animations); w.str(n.more().animation); w.f32(n.more().speed_scale); w.u8(n.more().autoplay ? 1 : 0);
            w.f32(n.more().zoom[0]); w.f32(n.more().zoom[1]);
            w.u32(uint32_t(n.script));
            w.u32(uint32_t(n.overrides.size()));
            for (const auto& o : n.overrides) {
                w.str(o.name);
                if (!w.value(o.value)) { error = "Value not exportable in " + n.name + "." + o.name; return false; }
            }
        }
        w.u32(uint32_t(connections.size()));
        for (const auto& c : connections) {
            w.u32(uint32_t(c.from)); w.u32(uint32_t(c.to)); w.str(c.signal); w.str(c.method); w.u32(c.flags);
        }
        return true;
    };
    w.str(d.path);
    if (!write_nodes(d.nodes, d.connections)) return {};
    w.u32(uint32_t(d.scenes.size()));
    for (const auto& scene : d.scenes) {
        w.str(scene.path);
        if (!write_nodes(scene.nodes, scene.connections)) return {};
    }
    w.u8(uint8_t(d.autoloads.size()));
    for (const auto& a : d.autoloads) {
        w.str(a.name);
        w.u32(uint32_t(a.script));
    }
    return w.out;
}
#endif

static bool read_nodes(Reader& r, std::vector<NodeDesc>& nodes, std::vector<SceneConnection>& connections, std::string& error) {
    const uint32_t count = r.u32();
    if (count > MaxNodes) { error = "Too many nodes in the package."; return false; }
    nodes.reserve(count); // no doubling: a whole scene's nodes at once
    for (uint32_t i = 0; i < count && r.ok; ++i) {
        NodeDesc n;
        NodeExtras e; // kept only by the node types that use it
        n.id = r.u32(); n.parent = int32_t(r.u32());
        const uint8_t type = r.u8();
        n.name = r.str(256);
        n.position = r.vec(); n.rotation = r.vec(); n.scale = r.vec();
        const uint8_t visible = r.u8();
        const uint8_t shape = r.u8();
        n.color = r.u32();
        const uint8_t shading = r.u8();
        n.segments = r.u16();
        e.fov = r.f32(); e.near = r.f32(); e.far = r.f32();
        const uint8_t current = r.u8();
        e.light_color = r.u32(); e.energy = r.f32(); e.background = r.u32(); e.ambient = r.u32();
        n.texture = int32_t(r.u32()); n.uv_scale[0] = r.f32(); n.uv_scale[1] = r.f32(); n.uv_offset[0] = r.f32(); n.uv_offset[1] = r.f32();
        const uint8_t flags = r.u8();
        n.bilinear = flags & 1; n.repeat = (flags & 2) != 0; n.perspective = (flags & 4) != 0; n.billboard = (flags & 8) != 0;
        n.alpha = r.f32(); n.specular = r.f32(); n.shininess = r.u8();
        const uint8_t cull = r.u8(), draw_order = r.u8(), background = r.u8();
        e.sky_top = r.u32(); e.sky_horizon = r.u32(); e.ground_horizon = r.u32(); e.ground_bottom = r.u32();
        e.sky_texture = int32_t(r.u32());
        n.diffuse = r.f32(); n.mesh = int32_t(r.u32()); e.text = r.str(MaxText); e.size = r.vec(); e.text_scale = r.u8();
        const uint8_t collision = r.u8();
        e.box_size = r.vec(); e.radius = r.f32(); e.height = r.f32();
        const uint8_t physics_flags = r.u8();
        e.disabled = physics_flags & 1; e.freeze = (physics_flags & 2) != 0; e.monitoring = (physics_flags & 4) != 0;
        e.one_way = (physics_flags & 8) != 0;
        e.collision_layer = r.u32(); e.collision_mask = r.u32();
        e.mass = r.f32(); e.gravity_scale = r.f32(); e.linear_damp = r.f32(); e.angular_damp = r.f32(); e.friction = r.f32(); e.bounce = r.f32();
        e.linear_velocity = r.vec(); e.angular_velocity = r.vec(); e.floor_max_angle = r.f32();
        if (collision > uint8_t(CollisionKind::Capsule) || physics_flags > 15) r.ok = false;
        e.collision = CollisionKind(collision);
        const uint8_t particle_flags = r.u8();
        e.emitting = particle_flags & 1; e.one_shot = (particle_flags & 2) != 0; e.additive = (particle_flags & 4) != 0;
        e.no_depth_test = (particle_flags & 8) != 0;
        e.amount = r.u16();
        e.lifetime = r.f32(); e.lifetime_randomness = r.f32(); e.explosiveness = r.f32(); e.direction = r.vec(); e.spread = r.f32();
        e.velocity_min = r.f32(); e.velocity_max = r.f32(); e.damping_min = r.f32(); e.damping_max = r.f32(); e.gravity = r.vec();
        const uint8_t style = r.u8(), emission = r.u8();
        e.emission_radius = r.f32(); e.emission_box = r.vec();
        e.color_end = r.u32(); e.puff_size = r.f32(); e.puff_size_end = r.f32();
        if (particle_flags > 15 || style > uint8_t(ParticleStyle::Puff) || emission > uint8_t(EmissionShape::Box)) r.ok = false;
        e.particle_style = ParticleStyle(style); e.emission_shape = EmissionShape(emission);
        n.reflection = (flags & 16) != 0;
        n.hframes = r.u16(); n.vframes = r.u16(); n.frame = r.u16();
        const uint8_t sprite_flags = r.u8();
        n.flip_h = sprite_flags & 1; n.flip_v = (sprite_flags & 2) != 0;
        n.pixel_size = r.f32();
        if (sprite_flags > 3) r.ok = false;
        n.z_index = int16_t(r.u16());
        const uint8_t centered = r.u8();
        n.centered = centered == 1;
        n.offset[0] = r.f32(); n.offset[1] = r.f32();
        e.animations = r.str(MaxText); e.animation = r.str(256); e.speed_scale = r.f32();
        const uint8_t autoplay = r.u8();
        e.autoplay = autoplay == 1;
        e.zoom[0] = r.f32(); e.zoom[1] = r.f32();
        if (centered > 1 || autoplay > 1) r.ok = false;
        n.script = int32_t(r.u32());
        const uint32_t overrides = r.u32();
        if (type > uint8_t(NodeType::Timer) || shape > uint8_t(Shape::Mesh) || shading > uint8_t(Shading::Water)
            || cull > uint8_t(CullMode::Disabled) || draw_order > uint8_t(DrawOrder::OnTop)
            || background > uint8_t(Background::Panorama) || flags > 31
            || visible > 1 || current > 1 || overrides > 256) { error = "Invalid node in the package."; return false; }
        n.type = NodeType(type); n.shape = Shape(shape); n.shading = Shading(shading);
        n.cull = CullMode(cull); n.draw_order = DrawOrder(draw_order); e.background_mode = Background(background);
        n.visible = visible == 1; e.current = current == 1;
        for (uint32_t j = 0; j < overrides && r.ok; ++j) {
            ScriptOverride o;
            o.name = r.str(80);
            o.value = r.value();
            n.overrides.push_back(std::move(o));
        }
        if (uses_extras(n.type)) n.extras() = std::move(e);
        nodes.push_back(std::move(n));
    }
    const uint32_t links = r.u32();
    if (links > MaxConnections) { error = "Too many signal connections in a scene."; return false; }
    connections.clear();
    connections.reserve(links);
    for (uint32_t i = 0; i < links && r.ok; ++i) {
        SceneConnection c;
        c.from = int32_t(r.u32()); c.to = int32_t(r.u32());
        c.signal = r.str(80); c.method = r.str(80);
        c.flags = r.u32();
        connections.push_back(std::move(c));
    }
    return true;
}

bool decode_pack(const uint8_t* data, size_t size, Description& out, std::string& error, bool textures_in_place) {
    error.clear();
    if (!data || size < 12) { error = "Truncated package."; return false; }
    Reader r{data, size};
    if (r.u32() != Magic || r.u32() != Version) { error = "Unsupported JETP format (expected version 16): export the project again."; return false; }
    Description d;
    d.name = r.str(256);
    d.width = r.u16();
    d.height = r.u16();
    const uint8_t actions = r.u8();
    if (actions > MaxActions) r.ok = false;
    d.actions.clear();
    for (uint8_t i = 0; i < actions && r.ok; ++i) {
        InputAction action;
        action.name = r.str(64);
        const uint8_t keys = r.u8();
        if (keys > MaxActionKeys) r.ok = false;
        for (uint8_t k = 0; k < keys && r.ok; ++k) action.keys.push_back(r.str(32));
        d.actions.push_back(std::move(action));
    }
    const uint32_t scripts = r.u32();
    if (scripts > MaxScripts) { error = "Too many scripts in the package."; return false; }
    for (uint32_t i = 0; i < scripts && r.ok; ++i) {
        ScriptFile s;
        s.path = r.str(512);
        s.bytecode = r.bytes(MaxScriptBytes);
        if (s.bytecode.empty()) r.ok = false;
        d.scripts.push_back(std::move(s));
    }
    const uint32_t textures = r.u32();
    if (textures > MaxTextures) { error = "Too many textures in the package."; return false; }
    size_t texture_bytes = 0;
    for (uint32_t i = 0; i < textures && r.ok; ++i) {
        TextureAsset t;
        t.path = r.str(512);
        t.width = r.u16(); t.height = r.u16();
        const uint8_t alpha = r.u8();
        t.alpha_color = r.u16();
        const uint16_t colors = r.u16();
        const size_t count = size_t(t.width) * t.height;
        texture_bytes += count * (colors ? 1 : 2);
        if (alpha > 1 || colors > 256 || !t.width || !t.height || t.width > MaxTextureSide || t.height > MaxTextureSide
            || texture_bytes > MaxTextureBytes) { error = "Invalid texture in the package."; return false; }
        t.has_alpha = alpha == 1;
        for (uint16_t k = 0; k < colors && r.ok; ++k) t.palette.push_back(r.u16());
        if (colors) {
            if (!r.take(count)) break;
            if (textures_in_place) t.packed_indices = r.p;
            else t.indices.assign(r.p, r.p + count);
            r.p += count; r.remaining -= count;
        } else {
            t.pixels.resize(count);
            for (size_t k = 0; k < count && r.ok; ++k) t.pixels[k] = r.u16();
        }
        d.textures.push_back(std::move(t));
    }
    const uint32_t meshes = r.u32();
    if (meshes > MaxMeshAssets) { error = "Too many models in the package."; return false; }
    size_t total_vertices = 0;
    for (uint32_t i = 0; i < meshes && r.ok; ++i) {
        MeshAsset m;
        m.path = r.str(512);
        const uint32_t vertices = r.u32();
        total_vertices += vertices;
        if (vertices > MaxMeshVertices || total_vertices > MaxSceneVertices) { error = "Invalid model in the package."; return false; }
        m.vertices.resize(vertices);
        for (auto& v : m.vertices) { v.position = r.vec(); v.normal = r.vec(); v.u = r.f32(); v.v = r.f32(); }
        const uint32_t indices = r.u32();
        if (indices > 6 * MaxMeshVertices) { error = "Invalid model in the package."; return false; }
        m.indices.resize(indices);
        for (auto& index : m.indices) index = r.u16();
        d.meshes.push_back(std::move(m));
    }
    d.path = r.str(512);
    d.nodes_offset = uint32_t(r.p - data);
    if (!read_nodes(r, d.nodes, d.connections, error)) return false;
    const uint32_t scenes = r.u32();
    if (scenes > MaxScenes) { error = "Too many scenes in the package."; return false; }
    for (uint32_t i = 0; i < scenes && r.ok; ++i) {
        SceneDesc scene;
        scene.path = r.str(512);
        scene.offset = uint32_t(r.p - data);
        if (!read_nodes(r, scene.nodes, scene.connections, error)) return false;
        d.scenes.push_back(std::move(scene));
    }
    const uint8_t autoloads = r.u8();
    if (autoloads > MaxAutoloads) { error = "Too many autoloads in the package."; return false; }
    for (uint8_t i = 0; i < autoloads && r.ok; ++i) {
        Autoload a;
        a.name = r.str(64);
        a.script = int32_t(r.u32());
        d.autoloads.push_back(std::move(a));
    }
    if (!r.ok || r.remaining) { error = "Truncated package or trailing data."; return false; }
    if (!validate(d, error)) return false;
    out = std::move(d);
    return true;
}


bool pack_resolution(const uint8_t* data, size_t size, int& width, int& height) {
    if (!data || size < 12) return false;
    Reader r{data, size};
    if (r.u32() != Magic || r.u32() != Version) return false;
    r.str(256);
    width = r.u16();
    height = r.u16();
    return r.ok;
}

bool decode_nodes(const uint8_t* data, size_t size, uint32_t offset, std::vector<NodeDesc>& nodes,
                  std::vector<SceneConnection>& connections, std::string& error) {
    error.clear();
    nodes.clear();
    if (!data || offset >= size) { error = "Truncated package."; return false; }
    Reader r{data + offset, size - offset};
    if (!read_nodes(r, nodes, connections, error)) return false;
    if (!r.ok) { error = "Truncated package."; return false; }
    return true;
}

} // namespace jetstudio
