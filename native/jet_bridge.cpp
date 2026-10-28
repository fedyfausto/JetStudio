#include "jet_bridge.hpp"
#include <godot_cpp/classes/image.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/variant/utility_functions.hpp>
#include <chrono>
#include <unordered_set>

using namespace godot;
namespace {
using jetstudio::NodeType;

std::string utf8(const String& s) { const CharString c = s.utf8(); return std::string(c.get_data(), size_t(c.length())); }
String gd(std::string_view s) { return String::utf8(s.data(), int64_t(s.size())); }

bool number(const Variant& v) { return v.get_type() == Variant::FLOAT || v.get_type() == Variant::INT; }
bool vec(const Dictionary& d, const char* key, jetstudio::Vec3& out) {
    if (!d.has(key)) return true; // keep the default
    const Variant value = d[key];
    if (value.get_type() == Variant::VECTOR3) { const godot::Vector3 v = value; out = {float(v.x), float(v.y), float(v.z)}; return true; }
    if (value.get_type() == Variant::VECTOR2) { const godot::Vector2 v = value; out = {float(v.x), float(v.y), 0}; return true; }
    if (value.get_type() != Variant::ARRAY) return false;
    const Array a = value;
    if (a.size() < 2 || a.size() > 3 || !number(a[0]) || !number(a[1]) || (a.size() == 3 && !number(a[2]))) return false;
    out = {float(double(a[0])), float(double(a[1])), a.size() == 3 ? float(double(a[2])) : 0.0f};
    return true;
}
bool real(const Dictionary& d, const char* key, float& out) {
    if (!d.has(key)) return true;
    if (!number(d[key])) return false;
    out = float(double(d[key]));
    return true;
}
bool integer(const Dictionary& d, const char* key, uint32_t& out, uint32_t max) {
    if (!d.has(key)) return true;
    if (!number(d[key])) return false;
    const double value = d[key];
    if (value < 0 || value > max || value != double(int64_t(value))) return false;
    out = uint32_t(value);
    return true;
}
bool flag(const Dictionary& d, const char* key, bool& out) {
    if (!d.has(key)) return true;
    if (d[key].get_type() != Variant::BOOL) return false;
    out = d[key];
    return true;
}
template <typename E> bool choice(const Dictionary& d, const char* key, E& out, std::initializer_list<const char*> names) {
    if (!d.has(key)) return true;
    const String value = d[key];
    int i = 0;
    for (const char* name : names) { if (value == name) { out = E(i); return true; } ++i; }
    return false;
}

jetscript::Value to_value(const Variant& v, int depth = 0) {
    switch (v.get_type()) {
    case Variant::BOOL: return jetscript::Value(bool(v));
    case Variant::INT: return jetscript::Value(jetscript::Int(int64_t(v)));
    case Variant::FLOAT: return jetscript::Value(double(v));
    case Variant::STRING: case Variant::STRING_NAME: return jetscript::Value(utf8(v));
    case Variant::VECTOR3: { const godot::Vector3 x = v; return jetscript::Value(jetscript::Vector3{float(x.x), float(x.y), float(x.z)}); }
    case Variant::COLOR: { const godot::Color c = v; return jetscript::Value(jetscript::Color{c.r, c.g, c.b}); }
    case Variant::ARRAY: {
        auto out = jetscript::Value::new_array();
        if (depth > 0) return out;
        const Array a = v;
        for (int i = 0; i < a.size(); ++i) out.array_push(to_value(a[i], depth + 1));
        return out;
    }
    default: return {};
    }
}
Variant to_variant(const jetscript::Value& v, int depth = 0) {
    using jetscript::Type;
    switch (v.type()) {
    case Type::Bool: return v.as_bool();
    case Type::Int: return int64_t(v.as_int());
    case Type::Float: return double(v.as_real());
    case Type::String: return gd(v.as_string());
    case Type::Vector3: { const auto x = v.as_vector3(); return godot::Vector3(x.x, x.y, x.z); }
    case Type::Color: { const auto c = v.as_color(); return godot::Color(c.r, c.g, c.b); }
    case Type::Array: {
        Array a;
        if (depth < 4) for (size_t i = 0; i < v.array_size(); ++i) a.push_back(to_variant(v.array_get(i), depth + 1));
        return a;
    }
    default: return Variant();
    }
}

bool import_options(const Dictionary& options, jetstudio::ObjImportOptions& out) {
    return real(options, "scale", out.scale) && flag(options, "mirror_x", out.mirror_x) && flag(options, "flip_faces", out.flip_faces)
        && flag(options, "flip_v", out.flip_v);
}

struct Flattener {
    jetstudio::Description d;
    const Dictionary& sources;
    String error;
    Dictionary images, models;
    std::unordered_map<std::string, JetBridge::CachedTexture>* textures = nullptr;
    std::unordered_map<std::string, JetBridge::CachedModel>* parsed = nullptr;
    // Texture index for a res:// path; -1 for none, -2 on error.
    int texture_index(const String& path) {
        if (path.is_empty()) return -1;
        const std::string p = utf8(path);
        for (size_t i = 0; i < d.textures.size(); ++i) if (d.textures[i].path == p) return int(i);
        Variant value = images.get(path, Variant());
        int mode = 0;
        if (value.get_type() == Variant::DICTIONARY) {
            const Dictionary entry = value;
            if (!choice(entry, "palette", mode, {"Auto", "Quantize", "RGB565"})) { error = "Invalid palette mode: " + path; return -2; }
            value = entry.get("image", Variant());
        }
        const Ref<Image> image = value;
        // A missing image renders untextured; the editor reports it and refuses exports.
        if (image.is_null() || image->is_empty()) return -1;
        auto& entry = (*textures)[p];
        if (entry.image != image->get_instance_id() || entry.mode != mode) {
            Ref<Image> rgba = image;
            if (image->get_format() != Image::FORMAT_RGBA8) {
                rgba = image->duplicate();
                rgba->convert(Image::FORMAT_RGBA8);
            }
            const PackedByteArray bytes = rgba->get_data();
            std::string message;
            if (!jetstudio::make_texture(p, rgba->get_width(), rgba->get_height(), bytes.ptr(), entry.asset, message,
                                         jetstudio::PaletteMode(mode))) {
                textures->erase(p);
                error = gd(message);
                return -2;
            }
            entry.image = image->get_instance_id();
            entry.mode = mode;
        }
        d.textures.push_back(entry.asset);
        return int(d.textures.size()) - 1;
    }
    // Mesh index for a part of an imported model; -1 when absent (draws nothing), -2 on error.
    int mesh_index(const String& path, const String& part) {
        if (path.is_empty()) return -1;
        const std::string key = utf8(path) + "#" + utf8(part);
        for (size_t i = 0; i < d.meshes.size(); ++i) if (d.meshes[i].path == key) return int(i);
        if (models.get(path, Variant()).get_type() != Variant::DICTIONARY) return -1;
        const Dictionary model = models[path];
        const String obj = model.get("obj", ""), mtl = model.get("mtl", "");
        const Dictionary options = model.get("options", Dictionary());
        const int64_t stamp[3] = {int64_t(obj.hash()) << 20 ^ obj.length(), int64_t(mtl.hash()), int64_t(options.hash())};
        auto& entry = (*parsed)[utf8(path)];
        if (!std::equal(stamp, stamp + 3, entry.stamp) || entry.parts.empty()) {
            jetstudio::ObjImportOptions o;
            std::vector<jetstudio::ObjMaterial> materials;
            std::string message;
            if (!import_options(options, o)) { parsed->erase(utf8(path)); error = "Invalid import options: " + path; return -2; }
            if (!jetstudio::import_obj(utf8(obj), utf8(mtl), o, entry.parts, materials, message)) {
                parsed->erase(utf8(path));
                error = path + String(": ") + gd(message);
                return -2;
            }
            std::copy(stamp, stamp + 3, entry.stamp);
        }
        for (const auto& p : entry.parts) {
            if (gd(p.name) != part) continue;
            if (d.meshes.size() >= jetstudio::MaxMeshAssets) { error = "At most 128 models per scene."; return -2; }
            d.meshes.push_back(p.mesh);
            d.meshes.back().path = key;
            return int(d.meshes.size()) - 1;
        }
        return -1;
    }
    int script_index(const String& path) {
        const std::string p = utf8(path);
        for (size_t i = 0; i < d.scripts.size(); ++i) if (d.scripts[i].path == p) return int(i);
        if (!sources.has(path)) { error = "Script not found: " + path; return -1; }
        d.scripts.push_back({p, utf8(sources[path])});
        return int(d.scripts.size()) - 1;
    }
    // Iterative on purpose, so a deep scene tree can't blow the native stack.
    bool run(const Dictionary& root) {
        std::vector<std::pair<Dictionary, int32_t>> stack{{root, -1}};
        while (!stack.empty()) {
            const Dictionary node = stack.back().first;
            const int32_t parent = stack.back().second;
            stack.pop_back();
            if (d.nodes.size() >= jetstudio::MaxNodes) { error = "At most 256 nodes per scene."; return false; }
            jetstudio::NodeDesc n;
            n.parent = parent;
            if (!number(node.get("id", Variant())) || node.get("name", Variant()).get_type() != Variant::STRING) {
                error = "Node without id or name."; return false;
            }
            n.id = uint32_t(int64_t(node["id"]));
            n.name = utf8(node["name"]);
            if (!choice(node, "type", n.type, {"Node", "Node3D", "MeshInstance3D", "Camera3D", "DirectionalLight3D", "WorldEnvironment",
                                               "Label", "TextureRect", "ColorRect", "StaticBody3D", "CharacterBody3D",
                                               "RigidBody3D", "Area3D", "CollisionShape3D", "CPUParticles3D", "Sprite3D", "Control"})) {
                error = "Unsupported node type: " + String(node["type"]); return false;
            }
            jetstudio::Vec3 uv_scale{n.uv_scale[0], n.uv_scale[1], 1}, uv_offset{};
            uint32_t shininess = n.shininess;
            int filter = n.bilinear ? 1 : 0;
            bool ok = vec(node, "position", n.position) && vec(node, "rotation", n.rotation) && vec(node, "scale", n.scale)
                && flag(node, "visible", n.visible)
                && choice(node, "mesh", n.shape, {"Box", "Sphere", "Plane", "Pyramid", "Cylinder", "Capsule", "Quad", "Torus", "File"})
                && integer(node, "color", n.color, 0xffffff)
                && choice(node, "shading", n.shading, {"Gouraud", "Flat", "Unlit", "Phong", "Additive", "Water"})
                && vec(node, "uv_scale", uv_scale) && vec(node, "uv_offset", uv_offset)
                && choice(node, "texture_filter", filter, {"Nearest", "Linear"})
                && flag(node, "texture_repeat", n.repeat) && flag(node, "perspective_correct", n.perspective)
                && real(node, "alpha", n.alpha) && real(node, "specular", n.specular) && integer(node, "shininess", shininess, 255)
                && choice(node, "cull_mode", n.cull, {"Back", "Front", "Disabled"}) && flag(node, "billboard", n.billboard)
                && choice(node, "draw_order", n.draw_order, {"Default", "Background", "On Top"})
                && real(node, "diffuse", n.diffuse) && flag(node, "reflection", n.reflection)
                && vec(node, "size", n.extras().size)
                && choice(node, "background_mode", n.extras().background_mode, {"Color", "Gradient", "Panorama"})
                && integer(node, "sky_top_color", n.extras().sky_top, 0xffffff) && integer(node, "sky_horizon_color", n.extras().sky_horizon, 0xffffff)
                && integer(node, "ground_horizon_color", n.extras().ground_horizon, 0xffffff)
                && integer(node, "ground_bottom_color", n.extras().ground_bottom, 0xffffff)
                && real(node, "fov", n.extras().fov) && real(node, "near", n.extras().near) && real(node, "far", n.extras().far) && flag(node, "current", n.extras().current)
                && integer(node, "light_color", n.extras().light_color, 0xffffff) && real(node, "light_energy", n.extras().energy)
                && integer(node, "background_color", n.extras().background, 0xffffff) && integer(node, "ambient_color", n.extras().ambient, 0xffffff);
            // Physics bodies and shapes.
            uint32_t layer = n.more().collision_layer, mask = n.more().collision_mask;
            ok = ok && choice(node, "shape", n.extras().collision, {"Box", "Sphere", "Capsule"}) && real(node, "radius", n.extras().radius)
                && real(node, "height", n.extras().height) && flag(node, "disabled", n.extras().disabled)
                && integer(node, "collision_layer", layer, 0xffffffffu) && integer(node, "collision_mask", mask, 0xffffffffu)
                && real(node, "mass", n.extras().mass) && real(node, "gravity_scale", n.extras().gravity_scale) && real(node, "linear_damp", n.extras().linear_damp)
                && real(node, "angular_damp", n.extras().angular_damp) && real(node, "friction", n.extras().friction) && real(node, "bounce", n.extras().bounce)
                && vec(node, "linear_velocity", n.extras().linear_velocity) && vec(node, "angular_velocity", n.extras().angular_velocity)
                && flag(node, "freeze", n.extras().freeze) && real(node, "floor_max_angle", n.extras().floor_max_angle) && flag(node, "monitoring", n.extras().monitoring);
            n.extras().collision_layer = layer;
            n.extras().collision_mask = mask;
            // CPUParticles3D, same property names as Godot.
            uint32_t amount = n.more().amount;
            ok = ok && flag(node, "emitting", n.extras().emitting) && flag(node, "one_shot", n.extras().one_shot) && flag(node, "additive", n.extras().additive)
                && flag(node, "no_depth_test", n.extras().no_depth_test)
                && integer(node, "amount", amount, jetstudio::MaxParticles) && real(node, "lifetime", n.extras().lifetime)
                && real(node, "lifetime_randomness", n.extras().lifetime_randomness) && real(node, "explosiveness", n.extras().explosiveness)
                && vec(node, "direction", n.extras().direction) && real(node, "spread", n.extras().spread) && vec(node, "gravity", n.extras().gravity)
                && real(node, "initial_velocity_min", n.extras().velocity_min) && real(node, "initial_velocity_max", n.extras().velocity_max)
                && real(node, "damping_min", n.extras().damping_min) && real(node, "damping_max", n.extras().damping_max)
                && choice(node, "particle_style", n.extras().particle_style, {"Sparks", "Water", "Puff"})
                && integer(node, "color_end", n.extras().color_end, 0xffffff) && real(node, "puff_size", n.extras().puff_size)
                && real(node, "puff_size_end", n.extras().puff_size_end)
                && choice(node, "emission_shape", n.extras().emission_shape, {"Point", "Sphere", "Box"})
                && real(node, "emission_sphere_radius", n.extras().emission_radius) && vec(node, "emission_box_extents", n.extras().emission_box);
            n.extras().amount = uint16_t(amount);
            // "size" is in pixels on Controls; on a CollisionShape3D it is the box size.
            if (n.type == NodeType::CollisionShape3D) {
                n.extras().box_size = n.extras().size;
                n.extras().size = jetstudio::NodeDesc{}.extras().size;
            }
            // Sprite3D, Godot's names; modulate is the colour that tints the texture.
            uint32_t hframes = n.hframes, vframes = n.vframes, frame = n.frame;
            ok = ok && integer(node, "hframes", hframes, 256) && integer(node, "vframes", vframes, 256) && integer(node, "frame", frame, 65535)
                && flag(node, "flip_h", n.flip_h) && flag(node, "flip_v", n.flip_v) && real(node, "pixel_size", n.pixel_size);
            n.hframes = uint16_t(std::max<uint32_t>(1, hframes));
            n.vframes = uint16_t(std::max<uint32_t>(1, vframes));
            n.frame = uint16_t(frame);
            if (n.type == NodeType::Sprite3D) {
                n.color = 0xffffff; // Godot's default modulate: the texture as it is
                ok = ok && integer(node, "modulate", n.color, 0xffffff);
            }
            uint32_t segments = n.segments, text_scale = n.more().text_scale;
            ok = ok && integer(node, "segments", segments, 64) && integer(node, "text_scale", text_scale, 8);
            n.segments = uint16_t(segments);
            n.extras().text_scale = uint8_t(text_scale);
            if (node.has("text")) {
                if (node["text"].get_type() != Variant::STRING) ok = false;
                else n.extras().text = utf8(node["text"]);
            }
            if (!ok) { error = "Invalid property in node " + String(node["name"]) + "."; return false; }
            n.uv_scale[0] = uv_scale.x; n.uv_scale[1] = uv_scale.y; n.uv_offset[0] = uv_offset.x; n.uv_offset[1] = uv_offset.y;
            n.shininess = uint8_t(shininess);
            n.bilinear = filter == 1;
            if (n.type == NodeType::MeshInstance3D) n.texture = texture_index(node.get("albedo_texture", ""));
            if (n.type == NodeType::TextureRect || n.type == NodeType::Sprite3D) n.texture = texture_index(node.get("texture", ""));
            if (n.type == NodeType::WorldEnvironment) n.extras().sky_texture = texture_index(node.get("sky_texture", ""));
            if (n.type == NodeType::MeshInstance3D && n.shape == jetstudio::Shape::Mesh)
                n.mesh = mesh_index(node.get("mesh_file", ""), node.get("mesh_part", ""));
            if (n.texture == -2 || n.more().sky_texture == -2 || n.mesh == -2) return false;
            const String script = node.get("script", "");
            if (!script.is_empty()) {
                n.script = script_index(script);
                if (n.script < 0) return false;
                const Dictionary overrides = node.get("script_properties", Dictionary());
                const Array keys = overrides.keys();
                for (int i = 0; i < keys.size(); ++i)
                    n.overrides.push_back({utf8(keys[i]), to_value(overrides[keys[i]])});
            }
            const int32_t index = int32_t(d.nodes.size());
            d.nodes.push_back(std::move(n));
            const Array children = node.get("children", Array());
            for (int i = children.size() - 1; i >= 0; --i) {
                if (children[i].get_type() != Variant::DICTIONARY) { error = "Invalid child."; return false; }
                stack.push_back({children[i], index});
            }
        }
        return true;
    }
};

Array diagnostics(const std::vector<jetscript::Diagnostic>& list) {
    Array out;
    for (const auto& e : list) {
        Dictionary item;
        item["line"] = e.line; item["column"] = e.column; item["message"] = gd(e.message);
        out.push_back(item);
    }
    return out;
}
} // namespace

void JetBridge::_bind_methods() {
    ClassDB::bind_method(D_METHOD("load_scene", "document", "scripts", "assets"), &JetBridge::load_scene);
    ClassDB::bind_method(D_METHOD("import_model", "obj", "mtl", "options"), &JetBridge::import_model);
    ClassDB::bind_method(D_METHOD("texture_info", "image", "palette"), &JetBridge::texture_info);
    ClassDB::bind_method(D_METHOD("set_editor_camera", "position", "rotation", "fov"), &JetBridge::set_editor_camera);
    ClassDB::bind_method(D_METHOD("use_editor_camera", "enabled"), &JetBridge::use_editor_camera);
    ClassDB::bind_method(D_METHOD("update", "delta"), &JetBridge::update);
    ClassDB::bind_method(D_METHOD("render_frame"), &JetBridge::render_frame);
    ClassDB::bind_method(D_METHOD("play_start"), &JetBridge::play_start);
    ClassDB::bind_method(D_METHOD("play_stop"), &JetBridge::play_stop);
    ClassDB::bind_method(D_METHOD("is_playing"), &JetBridge::is_playing);
    ClassDB::bind_method(D_METHOD("set_input", "mask"), &JetBridge::set_input);
    ClassDB::bind_method(D_METHOD("take_log"), &JetBridge::take_log);
    ClassDB::bind_method(D_METHOD("pick", "x", "y"), &JetBridge::pick);
    ClassDB::bind_method(D_METHOD("compile_script", "source", "path"), &JetBridge::compile_script);
    ClassDB::bind_method(D_METHOD("class_db"), &JetBridge::class_db);
    ClassDB::bind_method(D_METHOD("node_state", "id"), &JetBridge::node_state);
    ClassDB::bind_method(D_METHOD("statistics"), &JetBridge::statistics);
    ClassDB::bind_method(D_METHOD("export_pack"), &JetBridge::export_pack);
    ClassDB::bind_method(D_METHOD("set_resolution", "width", "height"), &JetBridge::set_resolution);
    ClassDB::bind_method(D_METHOD("set_input_map", "actions"), &JetBridge::set_input_map);
    ClassDB::bind_method(D_METHOD("set_scenes", "path", "others"), &JetBridge::set_scenes);
    ClassDB::bind_method(D_METHOD("set_autoloads", "autoloads"), &JetBridge::set_autoloads);
    ClassDB::bind_method(D_METHOD("frame_bytes"), &JetBridge::frame_bytes);
}

JetBridge::JetBridge() { rgb_.resize(jetstudio::DefaultWidth * jetstudio::DefaultHeight * 3); }

String JetBridge::load_scene(const Dictionary& document, const Dictionary& scripts, const Dictionary& assets) {
    if (int(document.get("version", 0)) != 2 || document.get("root", Variant()).get_type() != Variant::DICTIONARY)
        return "Invalid scene document (expected jetscene version 2).";
    Flattener flat{{}, scripts, {}};
    flat.images = assets.get("textures", Dictionary());
    flat.models = assets.get("models", Dictionary());
    flat.textures = &texture_cache_;
    flat.parsed = &model_cache_;
    if (!flat.run(document["root"])) return flat.error;
    flat.d.name = flat.d.nodes.front().name;
    flat.d.path = utf8(scene_path_);
    std::vector<jetstudio::NodeDesc> main = std::move(flat.d.nodes);
    const Array paths = other_scenes_.keys();
    for (int i = 0; i < paths.size(); ++i) {
        const Dictionary other = other_scenes_[paths[i]];
        if (int(other.get("version", 0)) != 2 || other.get("root", Variant()).get_type() != Variant::DICTIONARY)
            return "Invalid scene document: " + String(paths[i]);
        flat.d.nodes.clear();
        if (!flat.run(other["root"])) return flat.error + " (" + String(paths[i]) + ")";
        flat.d.scenes.push_back({utf8(paths[i]), std::move(flat.d.nodes)});
    }
    flat.d.nodes = std::move(main);
    for (int i = 0; i < autoloads_.size(); ++i) {
        const Dictionary entry = autoloads_[i];
        const int script = flat.script_index(entry.get("path", ""));
        if (script < 0) return flat.error;
        flat.d.autoloads.push_back({utf8(entry.get("name", "")), script});
    }
    flat.d.width = width_;
    flat.d.height = height_;
    flat.d.actions = actions_;
    std::string error;
    if (!runtime_.load(flat.d, error)) return gd(error);
    return "";
}

void JetBridge::set_autoloads(const Array& autoloads) {
    autoloads_ = autoloads;
    std::vector<std::string> names;
    for (int i = 0; i < autoloads.size(); ++i) names.push_back(utf8(Dictionary(autoloads[i]).get("name", "")));
    compile_host_.set_autoloads(std::move(names));
}

void JetBridge::set_scenes(const String& path, const Dictionary& others) {
    scene_path_ = path;
    other_scenes_ = others;
}

void JetBridge::set_input_map(const Array& actions) {
    actions_.clear();
    for (int i = 0; i < actions.size(); ++i) {
        if (actions[i].get_type() != Variant::DICTIONARY) continue;
        const Dictionary entry = actions[i];
        jetstudio::InputAction action;
        action.name = utf8(entry.get("name", ""));
        const Array keys = entry.get("keys", Array());
        for (int k = 0; k < keys.size(); ++k) action.keys.push_back(utf8(keys[k]));
        actions_.push_back(std::move(action));
    }
}

Dictionary JetBridge::import_model(const String& obj, const String& mtl, const Dictionary& options) const {
    Dictionary out;
    jetstudio::ObjImportOptions o;
    std::vector<jetstudio::ObjPart> parts;
    std::vector<jetstudio::ObjMaterial> materials;
    std::string error;
    if (!import_options(options, o)) error = "Invalid import options.";
    else jetstudio::import_obj(utf8(obj), utf8(mtl), o, parts, materials, error);
    out["error"] = gd(error);
    Array part_list, material_list;
    for (const auto& p : parts) {
        Dictionary item;
        item["name"] = gd(p.name); item["object"] = gd(p.object); item["material"] = gd(p.material);
        item["vertices"] = int64_t(p.mesh.vertices.size()); item["triangles"] = int64_t(p.mesh.indices.size() / 3);
        jetstudio::Vec3 lo{1e30f, 1e30f, 1e30f}, hi{-1e30f, -1e30f, -1e30f};
        for (const auto& v : p.mesh.vertices) {
            lo = {std::min(lo.x, v.position.x), std::min(lo.y, v.position.y), std::min(lo.z, v.position.z)};
            hi = {std::max(hi.x, v.position.x), std::max(hi.y, v.position.y), std::max(hi.z, v.position.z)};
        }
        item["aabb"] = AABB(godot::Vector3(lo.x, lo.y, lo.z), godot::Vector3(hi.x - lo.x, hi.y - lo.y, hi.z - lo.z));
        part_list.push_back(item);
    }
    for (const auto& m : materials) {
        Dictionary item;
        item["name"] = gd(m.name); item["color"] = int64_t(m.color); item["texture"] = gd(m.texture);
        item["alpha"] = m.alpha; item["shininess"] = m.shininess;
        material_list.push_back(item);
    }
    out["parts"] = part_list;
    out["materials"] = material_list;
    return out;
}

// Result of converting an image as the runtime would: {indexed, colors, bytes}; empty on error.
Dictionary JetBridge::texture_info(const Ref<Image>& image, const String& palette) const {
    Dictionary out;
    int mode = 0;
    Dictionary options;
    options["palette"] = palette;
    if (image.is_null() || image->is_empty() || !choice(options, "palette", mode, {"Auto", "Quantize", "RGB565"})) return out;
    Ref<Image> rgba = image;
    if (image->get_format() != Image::FORMAT_RGBA8) {
        rgba = image->duplicate();
        rgba->convert(Image::FORMAT_RGBA8);
    }
    const PackedByteArray bytes = rgba->get_data();
    jetstudio::TextureAsset asset;
    std::string error;
    if (!jetstudio::make_texture("", rgba->get_width(), rgba->get_height(), bytes.ptr(), asset, error, jetstudio::PaletteMode(mode)))
        return out;
    std::unordered_set<uint16_t> colors(asset.pixels.begin(), asset.pixels.end());
    out["indexed"] = !asset.palette.empty();
    out["colors"] = int64_t(asset.palette.empty() ? colors.size() : asset.palette.size());
    out["bytes"] = int64_t(asset.bytes());
    return out;
}

void JetBridge::set_editor_camera(const godot::Vector3& p, const godot::Vector3& r, double fov) {
    runtime_.set_editor_camera({float(p.x), float(p.y), float(p.z)}, {float(r.x), float(r.y), float(r.z)}, float(fov));
}
void JetBridge::use_editor_camera(bool enabled) { runtime_.use_editor_camera(enabled); }

void JetBridge::update(double delta) {
    const auto start = std::chrono::steady_clock::now();
    runtime_.update(float(delta));
    script_ms_ = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
}

Ref<ImageTexture> JetBridge::render_frame() {
    const auto start = std::chrono::steady_clock::now();
    runtime_.render();
    render_ms_ = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
    const auto& pixels = runtime_.pixels();
    if (rgb_.size() != int64_t(pixels.size() * 3)) rgb_.resize(int64_t(pixels.size() * 3));
    auto* dst = rgb_.ptrw();
    for (size_t i = 0; i < pixels.size(); ++i) {
        const auto c = pixels[i];
        const unsigned r = (c >> 11) & 31, g = (c >> 5) & 63, b = c & 31;
        dst[3 * i] = uint8_t((r << 3) | (r >> 2)); dst[3 * i + 1] = uint8_t((g << 2) | (g >> 4)); dst[3 * i + 2] = uint8_t((b << 3) | (b >> 2));
    }
    const auto image = Image::create_from_data(runtime_.width(), runtime_.height(), false, Image::FORMAT_RGB8, rgb_);
    // ImageTexture::update() can't resize, so a new resolution needs a new texture.
    if (texture_.is_null() || texture_->get_width() != runtime_.width() || texture_->get_height() != runtime_.height())
        texture_ = ImageTexture::create_from_image(image);
    else texture_->update(image);
    return texture_;
}

Array JetBridge::play_start() {
    std::vector<jetstudio::ScriptDiagnostic> errors;
    Array out;
    if (runtime_.start(errors)) return out;
    for (const auto& e : errors) {
        Dictionary item;
        item["path"] = gd(e.path); item["line"] = e.line; item["column"] = e.column; item["message"] = gd(e.message);
        out.push_back(item);
    }
    return out;
}
void JetBridge::play_stop() { runtime_.stop(); }
bool JetBridge::is_playing() const { return runtime_.playing(); }
void JetBridge::set_input(int mask) { runtime_.set_input(uint32_t(mask)); }

Array JetBridge::take_log() {
    Array out;
    for (const auto& line : runtime_.take_log()) {
        Dictionary item;
        item["text"] = gd(line.text); item["error"] = line.error; item["path"] = gd(line.path); item["line"] = line.line;
        out.push_back(item);
    }
    return out;
}

int JetBridge::pick(int x, int y) { return runtime_.pick(x, y); }

Dictionary JetBridge::compile_script(const String& source, const String& path) const {
    const auto result = jetscript::compile(utf8(source), utf8(path), compile_host_);
    Dictionary out;
    out["ok"] = result.errors.empty();
    out["errors"] = diagnostics(result.errors);
    out["warnings"] = diagnostics(result.warnings);
    if (!result.script) return out;
    const auto& script = *result.script;
    out["base"] = gd(script.base_class());
    Array exports, members, functions;
    for (const auto& e : script.exports()) {
        Dictionary item;
        item["name"] = gd(e.name); item["type"] = jetscript::type_name(e.type); item["default"] = to_variant(e.default_value);
        item["line"] = e.line;
        if (e.has_range) { Array range; range.push_back(e.min); range.push_back(e.max); range.push_back(e.step); item["range"] = range; }
        exports.push_back(item);
    }
    for (const auto& m : script.members()) {
        Dictionary item;
        item["name"] = gd(m.name); item["type"] = jetscript::type_name(m.type); item["line"] = m.line;
        item["exported"] = m.exported; item["onready"] = m.onready; item["constant"] = m.constant;
        members.push_back(item);
    }
    for (const auto& f : script.functions()) {
        Dictionary item;
        Array args;
        for (const auto& a : f.arguments) args.push_back(gd(a));
        item["name"] = gd(f.name); item["line"] = f.line; item["arguments"] = args;
        functions.push_back(item);
    }
    out["exports"] = exports; out["members"] = members; out["functions"] = functions;
    return out;
}

Array JetBridge::class_db() const {
    Array out;
    for (const auto& c : compile_host_.classes()) {
        Dictionary item;
        Array properties, methods;
        for (const auto& p : c.properties) {
            Dictionary prop;
            prop["name"] = gd(p.name); prop["type"] = jetscript::type_name(p.type); prop["read_only"] = p.read_only;
            properties.push_back(prop);
        }
        for (const auto& m : c.methods) {
            Dictionary method;
            method["name"] = gd(m.name); method["min_args"] = m.min_args; method["max_args"] = m.max_args;
            methods.push_back(method);
        }
        item["name"] = gd(c.name); item["base"] = gd(c.base); item["properties"] = properties; item["methods"] = methods;
        out.push_back(item);
    }
    return out;
}

Dictionary JetBridge::node_state(int id) const {
    Dictionary out;
    jetstudio::NodeDesc n;
    if (id <= 0 || !runtime_.node_state(uint32_t(id), n)) return out;
    out["position"] = godot::Vector3(n.position.x, n.position.y, n.position.z);
    out["rotation"] = godot::Vector3(n.rotation.x, n.rotation.y, n.rotation.z);
    out["scale"] = godot::Vector3(n.scale.x, n.scale.y, n.scale.z);
    out["visible"] = n.visible;
    return out;
}

Dictionary JetBridge::statistics() const {
    Dictionary d;
    d["render_ms"] = render_ms_;
    d["script_ms"] = script_ms_;
    d["triangles"] = runtime_.triangles();
    d["particles"] = runtime_.live_particles();
    d["nodes"] = int(runtime_.description().nodes.size());
    d["meshes"] = int(runtime_.mesh_count());
    d["framebuffer_bytes"] = int64_t(runtime_.framebuffer_bytes());
    d["width"] = runtime_.width();
    d["height"] = runtime_.height();
    return d;
}

PackedByteArray JetBridge::export_pack() const {
    std::string error;
    const auto pack = jetstudio::encode_pack(runtime_.description(), error);
    if (pack.empty()) godot::UtilityFunctions::push_error(gd("Export failed: " + error));
    PackedByteArray result;
    result.resize(int64_t(pack.size()));
    if (!pack.empty()) std::copy(pack.begin(), pack.end(), result.ptrw());
    return result;
}

PackedByteArray JetBridge::frame_bytes() const { return rgb_; }
