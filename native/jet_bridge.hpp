#pragma once
#include "obj_import.hpp"
#include "runtime.hpp"
#include "script_host.hpp"
#include <unordered_map>
#include <godot_cpp/classes/image.hpp>
#include <godot_cpp/classes/image_texture.hpp>
#include <godot_cpp/classes/ref_counted.hpp>
#include <godot_cpp/variant/array.hpp>
#include <godot_cpp/variant/dictionary.hpp>
#include <godot_cpp/variant/packed_byte_array.hpp>
#include <godot_cpp/variant/packed_vector2_array.hpp>
#include <godot_cpp/variant/rect2i.hpp>

// Runtime wrapper for the Godot UI. Scenes come as the editor's JSON tree (jetscene v2), scripts as source text.
// assets.textures: {path: Image | {"image": Image, "palette": "Auto"|"Quantize"|"RGB565"}}
// assets.models: {path: {"obj": String, "mtl": String, "options": Dictionary}}
class JetBridge : public godot::RefCounted {
    GDCLASS(JetBridge, godot::RefCounted)
public:
    JetBridge();
    godot::String load_scene(const godot::Dictionary& document, const godot::Dictionary& scripts, const godot::Dictionary& assets);
    godot::Dictionary import_model(const godot::String& obj, const godot::String& mtl, const godot::Dictionary& options) const;
    godot::Dictionary texture_info(const godot::Ref<godot::Image>& image, const godot::String& palette) const;
    void set_editor_camera(const godot::Vector3& position, const godot::Vector3& rotation, double fov);
    void use_editor_camera(bool enabled);
    void set_editor_canvas(bool enabled, double zoom, const godot::Vector2& origin);
    void update(double delta);
    godot::Ref<godot::ImageTexture> render_frame();
    godot::Array play_start();
    void play_stop();
    bool is_playing() const;
    void set_input(int mask);
    godot::Array take_log();
    int pick(int x, int y);
    godot::Rect2i drawn_rect(int id) const;
    godot::PackedVector2Array shape_outline(int id) const;
    godot::Dictionary memory_estimate(bool textures_in_place) const;
    godot::Dictionary compile_script(const godot::String& source, const godot::String& path) const;
    godot::Array class_db() const;
    godot::Dictionary node_state(int id) const;
    godot::Dictionary statistics() const;
    godot::PackedByteArray export_pack() const;
    // The project's display size, used from the next load_scene().
    void set_resolution(int width, int height) { width_ = width; height_ = height; }
    // Input Map as [{name, keys}] in bit order, used from the next load_scene().
    void set_input_map(const godot::Array& actions);
    // The scene's res:// path and the other project scenes {path: document} for change_scene_to_file().
    // load_scene() flattens those as well.
    void set_scenes(const godot::String& path, const godot::Dictionary& others);
    // Autoload scripts as [{name, path}].
    void set_autoloads(const godot::Array& autoloads);
    godot::PackedByteArray frame_bytes() const;
private:
    jetstudio::Runtime runtime_;
public:
    // Converted textures by path, valid while the editor passes the same Image instance.
    struct CachedTexture { uint64_t image = 0; int mode = -1; jetstudio::TextureAsset asset; };
    // Imported models by path, valid while the source text and options are unchanged.
    struct CachedModel { int64_t stamp[3] = {0, 0, 0}; std::vector<jetstudio::ObjPart> parts; };
private:
    std::unordered_map<std::string, CachedTexture> texture_cache_;
    std::unordered_map<std::string, CachedModel> model_cache_;
    jetstudio::ScriptHost compile_host_;
    godot::PackedByteArray rgb_;
    godot::Ref<godot::ImageTexture> texture_;
    double render_ms_ = 0, script_ms_ = 0;
    int width_ = jetstudio::DefaultWidth, height_ = jetstudio::DefaultHeight;
    std::vector<jetstudio::InputAction> actions_ = jetstudio::default_actions();
    godot::String scene_path_;
    godot::Dictionary other_scenes_;
    godot::Array autoloads_;
protected:
    static void _bind_methods();
};
