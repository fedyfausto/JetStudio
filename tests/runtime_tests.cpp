#include "obj_import.hpp"
#include "runtime.hpp"
#include "script_host.hpp"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <fstream>
#include <iostream>
#include <limits>

namespace {
int failures = 0;
void check(bool condition, const char* message) {
    if (!condition) { std::cerr << "FAIL: " << message << '\n'; ++failures; }
}

using namespace jetstudio;
constexpr int Width = DefaultWidth, Height = DefaultHeight;

NodeDesc node(uint32_t id, int32_t parent, NodeType type, const char* name) {
    NodeDesc n;
    n.id = id; n.parent = parent; n.type = type; n.name = name;
    return n;
}
// Root, light, camera and an orange cube at the origin.
Description scene() {
    Description d;
    d.nodes.push_back(node(1, -1, NodeType::Node3D, "World"));
    auto sun = node(2, 0, NodeType::DirectionalLight3D, "Sun");
    sun.rotation = {-50, -35, 0};
    d.nodes.push_back(sun);
    auto camera = node(3, 0, NodeType::Camera3D, "Camera3D");
    camera.position = {0, 2, 5}; camera.rotation = {-20, 0, 0}; camera.current = true;
    d.nodes.push_back(camera);
    auto cube = node(4, 0, NodeType::MeshInstance3D, "Cube");
    cube.position = {0, 0.5f, 0}; cube.color = 0xf0a45b;
    d.nodes.push_back(cube);
    return d;
}
std::vector<uint16_t> frame(Runtime& runtime) { runtime.render(); return runtime.pixels(); }
size_t lit(const std::vector<uint16_t>& pixels, uint32_t background = 0x161c28) {
    return size_t(std::count_if(pixels.begin(), pixels.end(), [&](uint16_t c) { return c != rgb565(background); }));
}
bool has_log(Runtime& runtime, const std::string& text, bool error) {
    for (const auto& line : runtime.take_log()) if (line.error == error && line.text.find(text) != std::string::npos) return true;
    return false;
}
std::vector<uint16_t> render_scene(const Description& d) {
    Runtime runtime;
    std::string error;
    if (!runtime.load(d, error)) { std::cerr << error << '\n'; return {}; }
    return frame(runtime);
}
std::vector<uint16_t> play_scene(const Description& d, float seconds = 0, uint32_t input = 0) {
    Runtime runtime;
    std::string error;
    std::vector<ScriptDiagnostic> diagnostics;
    if (!runtime.load(d, error) || !runtime.start(diagnostics)) {
        std::cerr << error;
        for (const auto& e : diagnostics) std::cerr << e.path << ':' << e.line << ' ' << e.message << '\n';
        return {};
    }
    runtime.set_input(input);
    for (float t = 0; t < seconds; t += 0.05f) runtime.update(0.05f);
    runtime.update(0);
    return frame(runtime);
}
} // namespace

// Touches most of the language: loops, match, containers, formatting, enums, typed vars, calls, $ paths, builtins.
const char* const coverage_script = R"(extends Node3D
const LIMIT := 4
enum Mode { IDLE, RUN = 5, JUMP }
@export var speed := 2.5
@export var tint := Color(1.0, 0.5, 0.25)
var counter: int = 0
var names := ["a", "b"]
var table := {"x": 1, 2: "two"}
@onready var cube = $Cube

func add(a: int, b := 3) -> int:
	return a + b

func describe(value) -> String:
	match value:
		0, 1:
			return "small"
		Mode.RUN:
			return "run"
		_:
			return "other %d" % value

func _ready():
	var total := 0
	for i in range(LIMIT):
		total += i
	for i in range(10, 0, -3):
		total += i
	for n in names:
		total += len(n)
	for k in table:
		total += 1
	for c in "xyz":
		total += 1
	var j := 0
	while j < 5:
		j += 1
		if j == 2:
			continue
		if j == 4:
			break
		total += j
	var v := Vector3(1, 2, 3) * 2.0
	v.x += 0.5
	var text := "%s %d %.2f" % ["t", total, v.length()]
	var mixed = [1, 2.5, "s", v, tint]
	mixed.append(add(1) + add(1, 1))
	table["y"] = describe(5) + describe(9)
	cube.position.y = 0.25 * speed
	print(text, " ", mixed.size(), " ", table["y"], " ", Mode.JUMP, " ", str(total is int), " ", sqrt(16.0))
)";

std::vector<std::string> run_log(Runtime& runtime) {
    std::vector<std::string> lines;
    for (const auto& line : runtime.take_log()) lines.push_back((line.error ? "E " : "") + line.text);
    return lines;
}

uint32_t crc32(const uint8_t* data, size_t size) {
    uint32_t crc = 0xFFFFFFFFu;
    for (size_t i = 0; i < size; ++i) {
        crc ^= data[i];
        for (int k = 0; k < 8; ++k) crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
    }
    return ~crc;
}

void bytecode_tests() {
    ScriptHost host;
    const char* const sources[] = {
        coverage_script,
        "extends Node3D\n@export var speed := 90.0\nfunc _process(delta):\n\trotation_degrees.y += speed * delta\n",
        "extends Node\nvar data := {}\nfunc _ready():\n\tfor i in 3:\n\t\tdata[i] = [i, str(i)]\n\tprint(data)\n",
    };
    for (const char* source : sources) {
        auto compiled = jetscript::compile(source, "res://t.jet", host);
        check(compiled.errors.empty(), "coverage script compiles");
        if (!compiled.script) { for (auto& e : compiled.errors) std::cerr << e.line << ": " << e.message << '\n'; continue; }
        const auto bytes = jetscript::save_bytecode(*compiled.script);
        auto loaded = jetscript::load_bytecode(bytes.data(), bytes.size(), host);
        check(loaded.script != nullptr, "bytecode loads");
        if (!loaded.script) { for (auto& e : loaded.errors) std::cerr << e.message << '\n'; continue; }
        check(jetscript::save_bytecode(*loaded.script) == bytes, "bytecode round trip is stable");
        check(loaded.script->exports().size() == compiled.script->exports().size()
              && loaded.script->base_class() == compiled.script->base_class(), "bytecode keeps metadata");
        // Every truncation and every single-byte corruption is rejected without crashing.
        for (size_t n = 0; n < bytes.size(); n += 7)
            check(jetscript::load_bytecode(bytes.data(), n, host).script == nullptr, "reject truncated bytecode");
        for (size_t i = 0; i < bytes.size(); i += 3) {
            auto bad = bytes;
            bad[i] ^= 0x5a;
            check(jetscript::load_bytecode(bad.data(), bad.size(), host).script == nullptr, "reject corrupted bytecode");
        }
    }
    // Corruption with a valid checksum must be caught by the verifier (or be harmless).
    auto compiled = jetscript::compile(coverage_script, "res://t.jet", host);
    if (compiled.script) {
        const auto bytes = jetscript::save_bytecode(*compiled.script);
        int rejected = 0;
        for (size_t i = 20; i < bytes.size(); ++i) {
            auto bad = bytes;
            bad[i] = uint8_t(bad[i] + 0x40);
            const uint32_t crc = crc32(bad.data() + 20, bad.size() - 20);
            for (int k = 0; k < 4; ++k) bad[16 + k] = uint8_t(crc >> (8 * k));
            rejected += jetscript::load_bytecode(bad.data(), bad.size(), host).script == nullptr;
        }
        check(rejected > int(bytes.size()) / 4, "verifier rejects most re-checksummed corruptions");
    }
}

// RGBA checkerboard: `colors` distinct values, optional transparent pixels.
std::vector<uint8_t> checker(int w, int h, int colors, bool holes) {
    std::vector<uint8_t> rgba(size_t(w) * h * 4);
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            uint8_t* p = &rgba[(size_t(y) * w + x) * 4];
            const int c = (x / 4 + y / 4) % 2;
            if (colors == 2) { p[0] = c ? 240 : 20; p[1] = c ? 200 : 40; p[2] = c ? 40 : 200; }
            else { p[0] = uint8_t(x * 2); p[1] = uint8_t(y * 2); p[2] = uint8_t((x + y) & 255); } // every pixel different
            p[3] = holes && (x + y) % 9 == 0 ? 0 : 255;
        }
    return rgba;
}

void material_tests() {
    std::string error;
    // Conversion: palette when <= 256 colours, RGB565 otherwise, colour key for alpha.
    TextureAsset small, large, holes;
    auto a = checker(16, 16, 2, false), b = checker(128, 128, 300, false), c = checker(16, 16, 2, true);
    check(make_texture("res://a.png", 16, 16, a.data(), small, error) && small.palette.size() == 2 && small.indices.size() == 256,
          "two-colour image becomes a 2-entry palette texture");
    check(make_texture("res://b.png", 128, 128, b.data(), large, error) && large.palette.empty() && large.pixels.size() == 128 * 128,
          "image with more than 256 colours stays RGB565");
    check(make_texture("res://c.png", 16, 16, c.data(), holes, error) && holes.has_alpha
          && std::find(holes.palette.begin(), holes.palette.end(), holes.alpha_color) != holes.palette.end(), "transparent pixels use a colour key");
    TextureAsset bad;
    check(!make_texture("res://x.png", 2048, 4, b.data(), bad, error), "reject oversized textures");

    // Back-face culling must not drop visible faces of closed primitives.
    // Covered pixels must match; colours may differ where back faces tied in depth.
    for (int shape : {0, 1, 3, 4, 5}) {
        Description d = scene();
        d.nodes[3].shape = Shape(shape);
        d.nodes[3].rotation = {20, 35, 0};
        Description open = d, front = d;
        open.nodes[3].cull = CullMode::Disabled;
        front.nodes[3].cull = CullMode::Front;
        const long all = long(lit(render_scene(open))), inside = long(lit(render_scene(front)));
        check(render_scene(d) == render_scene(open), "back-face culling keeps closed primitives intact");
        check(std::abs(inside - all) <= all / 50 + 2 && render_scene(front) != render_scene(open), "front-face culling shows the inside faces");
    }

    // Textures, filtering, perspective and albedo tint change the image.
    Description plain = scene();
    plain.nodes[3].position = {0, 0.4f, 3.6f};
    plain.nodes[3].rotation = {0, 55, 0};
    plain.nodes[3].scale = {1.5f, 1.5f, 1.5f};
    plain.nodes[3].color = 0xffffff;
    Description textured = plain;
    textured.textures.push_back(small);
    textured.nodes[3].texture = 0;
    const auto untextured_frame = render_scene(plain), textured_frame = render_scene(textured);
    check(textured_frame != untextured_frame, "textured box differs from a plain box");
    Description linear = textured;
    linear.nodes[3].bilinear = true;
    Description affine = textured;
    affine.nodes[3].perspective = false;
    Description tiled = textured;
    tiled.nodes[3].uv_scale[0] = tiled.nodes[3].uv_scale[1] = 3;
    Description tinted = textured;
    tinted.nodes[3].color = 0xff0000;
    check(render_scene(linear) != textured_frame, "bilinear filtering changes the magnified texture");
    check(render_scene(affine) != textured_frame, "affine mapping differs from perspective-correct mapping");
    check(render_scene(tiled) != textured_frame, "UV scale repeats the texture");
    const auto red = render_scene(tinted);
    size_t blue = 0;
    for (uint16_t px : red) blue += px & 31;
    size_t blue_plain = 0;
    for (uint16_t px : textured_frame) blue_plain += px & 31;
    check(blue < blue_plain, "albedo colour multiplies the texture");
    Description glow = plain;
    glow.nodes[3].shading = Shading::Additive;
    glow.nodes[3].alpha = 0.6f;
    check(render_scene(glow) != untextured_frame, "additive translucent material renders differently");

    // Sky: gradient follows the camera pitch, panorama fills the background.
    Description sky = scene();
    sky.nodes.push_back(node(5, 0, NodeType::WorldEnvironment, "Env"));
    sky.nodes[4].background_mode = Background::Gradient;
    sky.nodes[4].sky_top = 0x0000ff; sky.nodes[4].sky_horizon = 0xffffff; sky.nodes[4].ground_horizon = 0x404040; sky.nodes[4].ground_bottom = 0x000000;
    Runtime r;
    check(r.load(sky, error), "scene with gradient sky loads");
    r.set_editor_camera({0, 1, 6}, {0, 0, 0}, 70);
    const auto level = frame(r);
    check(level[0] != level[size_t(Height - 1) * Width], "gradient: sky at the top differs from ground at the bottom");
    r.set_editor_camera({0, 1, 6}, {0.6f, 0, 0}, 70);
    check(frame(r) != level, "gradient horizon moves with the camera pitch");
    Description pano = sky;
    pano.nodes[4].background_mode = Background::Panorama;
    pano.textures.push_back(large);
    pano.nodes[4].sky_texture = 0;
    check(r.load(pano, error), "scene with panorama sky loads");
    r.set_editor_camera({0, 1, 6}, {0, 0, 0}, 70);
    const auto first = frame(r);
    check(std::find(large.pixels.begin(), large.pixels.end(), first[0]) != large.pixels.end(), "panorama pixels come from the texture");
    check(lit(first, 0) > 0 && r.pick(Width / 2, Height / 2 + 20) == 4, "objects render over the panorama (depth cleared)");
    r.set_editor_camera({0, 1, 6}, {0, 0.8f, 0}, 70);
    check(frame(r) != first, "panorama turns with the camera");

    // Script access to material properties.
    Description scripted = textured;
    scripted.scripts.push_back({"res://uv.jet", "extends MeshInstance3D\nfunc _process(delta):\n\tuv_offset.x += 0.25\n\talpha = 0.5\n", {}});
    scripted.nodes[3].script = 0;
    check(play_scene(scripted, 0.1f) != play_scene(textured, 0.1f), "scripts can scroll UVs and change alpha");

    // Package v4 keeps textures and material fields.
    Description packed = textured;
    packed.textures.push_back(large);
    packed.textures.push_back(holes);
    packed.nodes[3].shading = Shading::Phong;
    packed.nodes[3].uv_offset[0] = 0.5f;
    packed.nodes[3].cull = CullMode::Front;
    packed.nodes.push_back(node(9, 0, NodeType::WorldEnvironment, "Env"));
    packed.nodes.back().background_mode = Background::Panorama;
    packed.nodes.back().sky_texture = 1;
    const auto bytes = encode_pack(packed, error);
    Description back;
    check(decode_pack(bytes.data(), bytes.size(), back, error), "pack with textures round trips");
    check(back.textures.size() == 3 && back.textures[0].palette == small.palette && back.textures[0].indices == small.indices
          && back.textures[1].pixels == large.pixels && back.textures[2].has_alpha && back.textures[2].alpha_color == holes.alpha_color,
          "pack keeps palette, RGB565 and colour-key textures");
    check(back.nodes[3].shading == Shading::Phong && back.nodes[3].uv_offset[0] == 0.5f && back.nodes[3].cull == CullMode::Front
          && back.nodes.back().background_mode == Background::Panorama && back.nodes.back().sky_texture == 1, "pack keeps material and sky fields");
    check(render_scene(packed) == render_scene(back), "decoded pack renders identically");
}

// Unit cube OBJ: two objects and materials, quads, shared per-face normals, negative indices, plus an MTL.
const char* const cube_obj = R"(# test cube
mtllib cube.mtl
v -0.5 -0.5 0.5
v 0.5 -0.5 0.5
v 0.5 0.5 0.5
v -0.5 0.5 0.5
v -0.5 -0.5 -0.5
v 0.5 -0.5 -0.5
v 0.5 0.5 -0.5
v -0.5 0.5 -0.5
vt 0 0
vt 1 0
vt 1 1
vt 0 1
vn 0 0 1
vn 0 0 -1
vn 1 0 0
vn -1 0 0
vn 0 1 0
vn 0 -1 0
o Sides
usemtl Paint
f 1/1/1 2/2/1 3/3/1 4/4/1
f 6/1/2 5/2/2 8/3/2 7/4/2
f 2/1/3 6/2/3 7/3/3 3/4/3
f 5/1/4 1/2/4 4/3/4 8/4/4
o Caps
usemtl Glass
f 4/1/5 3/2/5 7/3/5 8/4/5
f -7/1/6 -8/2/6 -4/3/6 -3/4/6
)";
const char* const cube_mtl = R"(newmtl Paint
Kd 1.0 0.5 0.0
Ns 32
newmtl Glass
Kd 0 0 1
d 0.5
map_Kd -s 1 1 1 glass.png
)";

void model_tests() {
    std::string error;
    // Quantization: a many-colour image becomes a 256-entry palette close to the source.
    auto many = checker(128, 128, 300, false), holes = checker(64, 64, 300, true);
    TextureAsset quantized, direct, keyed;
    check(make_texture("res://q.png", 128, 128, many.data(), quantized, error, PaletteMode::Quantize) && quantized.palette.size() <= 256
          && quantized.palette.size() > 200 && quantized.indices.size() == 128 * 128, "Quantize reduces to a 256-colour palette");
    long worst = 0;
    for (size_t i = 0; i < quantized.indices.size(); ++i) {
        const uint16_t c = quantized.palette[quantized.indices[i]];
        const long r = long((c >> 11) << 3), g = long(((c >> 5) & 63) << 2), b = long((c & 31) << 3);
        worst = std::max({worst, std::abs(r - many[i * 4]), std::abs(g - many[i * 4 + 1]), std::abs(b - many[i * 4 + 2])});
    }
    check(worst < 48, "quantized colours stay close to the source");
    auto two = checker(16, 16, 2, false);
    check(make_texture("res://d.png", 16, 16, two.data(), direct, error, PaletteMode::Rgb565) && direct.palette.empty()
          && direct.pixels.size() == 256, "RGB565 mode never builds a palette");
    check(make_texture("res://k.png", 64, 64, holes.data(), keyed, error, PaletteMode::Quantize) && keyed.has_alpha
          && keyed.palette.size() <= 256, "Quantize keeps the colour key");
    bool key_only_on_holes = true;
    for (size_t i = 0; i < keyed.indices.size(); ++i)
        if ((keyed.palette[keyed.indices[i]] == keyed.alpha_color) != (holes[i * 4 + 3] < 128)) key_only_on_holes = false;
    check(key_only_on_holes, "quantized opaque pixels never take the key colour");

    // OBJ import: parts per object and material, triangulation, MTL fields, UV flip.
    std::vector<ObjPart> parts;
    std::vector<ObjMaterial> materials;
    check(import_obj(cube_obj, cube_mtl, {}, parts, materials, error), "OBJ cube imports");
    check(parts.size() == 2 && parts[0].name == "Sides" && parts[0].material == "Paint" && parts[1].name == "Caps",
          "one part per object/material");
    check(parts.size() == 2 && parts[0].mesh.indices.size() == 24 && parts[1].mesh.indices.size() == 12, "quads become two triangles");
    check(parts.size() == 2 && parts[0].mesh.vertices.size() == 16 && parts[1].mesh.vertices.size() == 8, "vertices shared per face");
    check(materials.size() == 2 && materials[0].color == 0xff8000 && materials[0].shininess == 32 && materials[1].alpha == 0.5f
          && materials[1].texture == "glass.png", "MTL colour, shininess, alpha and map_Kd options");
    check(!parts.empty() && parts[0].mesh.vertices[0].v == 1.0f && parts[0].mesh.vertices[2].v == 0.0f, "OBJ V is flipped to rows");
    std::vector<ObjPart> mirrored;
    ObjImportOptions options;
    options.mirror_x = true;
    options.scale = 2;
    check(import_obj(cube_obj, cube_mtl, options, mirrored, materials, error) && mirrored[0].mesh.vertices[0].position.x == 1.0f
          && mirrored[0].mesh.vertices[0].normal.z == 1.0f, "mirror X and scale");
    check(!import_obj("v 0 0 0\nf 1 2 3\n", "", {}, parts, materials, error) && error.find("line 2") != std::string::npos,
          "bad face index reports the line");
    check(!import_obj("v 0 0 0\n", "", {}, parts, materials, error), "OBJ without faces is rejected");
    check(import_obj(cube_obj, cube_mtl, {}, parts, materials, error), "re-import");

    // Imported meshes render like the primitive they model and cull correctly.
    Description box = scene();
    box.nodes[3].shading = Shading::Unlit;
    box.nodes[3].rotation = {20, 35, 0};
    Description model = box;
    model.meshes = {parts[0].mesh, parts[1].mesh};
    model.nodes[3].shape = Shape::Mesh;
    model.nodes[3].mesh = 0;
    auto caps = model.nodes[3];
    caps.id = 5; caps.name = "Caps"; caps.mesh = 1;
    model.nodes.push_back(caps);
    const auto primitive = render_scene(box), imported = render_scene(model);
    check(!imported.empty() && std::labs(long(lit(imported)) - long(lit(primitive))) <= long(lit(primitive)) / 50,
          "imported cube covers the same pixels as the Box primitive");
    Description open = model;
    open.nodes[3].cull = open.nodes[4].cull = CullMode::Disabled;
    check(render_scene(open) == imported, "imported winding faces outwards (culling keeps it intact)");
    options = {};
    options.flip_faces = true;
    std::vector<ObjPart> inside;
    check(import_obj(cube_obj, cube_mtl, options, inside, materials, error), "flip faces imports");
    // Lit shading shows the inverted normals (unlit, both silhouettes are equal).
    Description lit_model = model, flipped = model;
    lit_model.nodes[3].shading = lit_model.nodes[4].shading = Shading::Gouraud;
    flipped.nodes[3].shading = flipped.nodes[4].shading = Shading::Gouraud;
    flipped.meshes = {inside[0].mesh, inside[1].mesh};
    check(render_scene(flipped) != render_scene(lit_model), "flip faces turns the model inside out");
    Description empty = model;
    empty.nodes[3].mesh = empty.nodes[4].mesh = -1;
    Runtime runtime;
    check(runtime.load(empty, error), "MeshInstance3D without a model loads");
    frame(runtime);
    check(runtime.triangles() == 0, "a mesh without a model draws nothing");
    empty.nodes[3].mesh = 7;
    check(!runtime.load(empty, error), "reject a mesh index out of range");

    // Quad and Torus primitives.
    Description torus = scene();
    torus.nodes[3].shape = Shape::Torus;
    torus.nodes[3].rotation = {50, 20, 0};
    Description torus_open = torus;
    torus_open.nodes[3].cull = CullMode::Disabled;
    check(lit(render_scene(torus)) > 500 && render_scene(torus) == render_scene(torus_open), "torus renders closed and outward");
    Description quad = scene();
    quad.nodes[3].shape = Shape::Quad;
    quad.nodes[3].shading = Shading::Unlit;
    const size_t quad_front = lit(render_scene(quad));
    quad.nodes[3].rotation = {0, 180, 0};
    const size_t quad_back = lit(render_scene(quad));
    check(quad_front > 100 && quad_back == 0, "quad faces +Z and its back is culled");
    quad.nodes[3].cull = CullMode::Disabled;
    check(lit(render_scene(quad)) == quad_front, "quad without culling is two-sided");

    // Diffuse coefficient darkens lit materials; environment reflection moves with the camera.
    Description shiny = scene();
    shiny.nodes[3].shading = Shading::Phong;
    shiny.nodes[3].color = 0xffffff;
    Description dull = shiny;
    dull.nodes[3].diffuse = 0.3f;
    auto brightness = [](const std::vector<uint16_t>& px) {
        size_t sum = 0;
        for (uint16_t c : px) sum += (c >> 11) + ((c >> 5) & 63) / 2 + (c & 31);
        return sum;
    };
    check(brightness(render_scene(dull)) < brightness(render_scene(shiny)), "lower diffuse is darker");
    Description mirror = scene();
    TextureAsset environment;
    check(make_texture("res://env.png", 128, 128, many.data(), environment, error), "environment texture");
    mirror.textures.push_back(environment);
    mirror.nodes[3].texture = 0;
    mirror.nodes[3].color = 0xffffff;
    mirror.nodes[3].shading = Shading::Unlit;
    Description chrome = mirror;
    chrome.nodes[3].reflection = true;
    check(render_scene(chrome) != render_scene(mirror), "reflection replaces the authored UVs");
    Runtime reflective;
    check(reflective.load(chrome, error), "reflective scene loads");
    reflective.set_editor_camera({0.3f, 1.6f, 4}, {-15, 4, 0}, 70);
    const auto view_a = frame(reflective);
    reflective.set_editor_camera({-0.3f, 1.6f, 4}, {-15, -4, 0}, 70);
    const auto view_b = frame(reflective);
    Runtime fixed;
    check(fixed.load(mirror, error), "plain textured scene loads");
    fixed.set_editor_camera({0.3f, 1.6f, 4}, {-15, 4, 0}, 70);
    const auto plain_a = frame(fixed);
    check(view_a != plain_a && view_b != view_a, "reflection follows the camera");

    // 2D nodes over the 3D scene: fill, alpha, nesting, text, texture, picking.
    Description hud = scene();
    auto panel = node(10, 0, NodeType::ColorRect, "Panel");
    panel.position = {10, 10, 0}; panel.size = {40, 30, 0}; panel.color = 0xff0000; panel.alpha = 1;
    hud.nodes.push_back(panel);
    auto inner = node(11, 4, NodeType::ColorRect, "Inner");
    inner.position = {5, 5, 0}; inner.size = {4, 4, 0}; inner.color = 0x00ff00; inner.alpha = 1;
    hud.nodes.push_back(inner);
    auto title = node(12, 0, NodeType::Label, "Title");
    title.position = {100, 200, 0}; title.text = "HI"; title.text_scale = 2; title.color = 0xffffff; title.alpha = 1;
    hud.nodes.push_back(title);
    auto icon = node(13, 0, NodeType::TextureRect, "Icon");
    icon.position = {250, 20, 0}; icon.texture = 0; icon.text_scale = 2; icon.color = 0xffffff; icon.alpha = 1; icon.size = {0, 0, 0};
    hud.nodes.push_back(icon);
    TextureAsset two_colour;
    check(make_texture("res://icon.png", 16, 16, two.data(), two_colour, error), "icon texture");
    hud.textures.push_back(two_colour);
    Runtime overlay;
    check(overlay.load(hud, error), "scene with 2D nodes loads");
    if (!error.empty()) std::cerr << error << '\n';
    auto px = frame(overlay);
    auto at = [&](int x, int y) { return px[size_t(y) * Width + size_t(x)]; };
    check(at(12, 12) == rgb565(0xff0000) && at(16, 16) == rgb565(0x00ff00), "ColorRect fills; child Control is relative to its parent");
    size_t white = 0;
    for (int y = 200; y < 230; ++y) for (int x = 100; x < 128; ++x) white += at(x, y) == rgb565(0xffffff);
    check(white > 40, "Label draws scaled bitmap text");
    check(at(250, 20) == two_colour.palette[two_colour.indices[0]] && at(281, 51) == two_colour.palette[two_colour.indices[255]],
          "TextureRect draws the texture at integer scale");
    check(overlay.pick(12, 12) == 10 && overlay.pick(16, 16) == 11 && overlay.pick(110, 205) == 12 && overlay.pick(260, 30) == 13,
          "2D nodes are picked before 3D objects");
    hud.nodes[4].alpha = 0.5f;
    hud.nodes[5].visible = false;
    check(overlay.load(hud, error), "translucent panel loads");
    px = frame(overlay);
    check(at(12, 12) != rgb565(0xff0000) && at(16, 16) != rgb565(0x00ff00), "alpha blends; hidden parent hides children");
    hud.nodes[5].visible = true;
    hud.scripts.push_back({"res://hud.jet", "extends Label\nfunc _process(delta):\n\ttext = \"SCORE 99\"\n\tposition.x = 20.0\n", {}});
    hud.nodes[6].script = 0;
    check(play_scene(hud, 0.1f) != render_scene(hud), "scripts change Label text and Control position");

    // Draw order bands: Background never hides later geometry, On Top covers it.
    auto pair = [](DrawOrder near_order, DrawOrder far_order) {
        Description d = scene();
        d.nodes[3].shading = Shading::Unlit;
        d.nodes[3].color = 0xff0000;
        d.nodes[3].position = {0, 0.6f, 2.5f}; // near the camera
        d.nodes[3].draw_order = near_order;
        auto far = node(5, 0, NodeType::MeshInstance3D, "Far");
        far.shading = Shading::Unlit;
        far.color = 0x0000ff;
        far.position = {0, 0.6f, -1};
        far.scale = {2, 2, 2};
        far.draw_order = far_order;
        d.nodes.push_back(far);
        Runtime r;
        std::string e;
        r.load(d, e);
        r.set_editor_camera({0, 1, 6}, {-0.08f, 0, 0}, 70);
        r.render();
        return r.pixels()[size_t(Height / 2 + 6) * Width + Width / 2];
    };
    check(pair(DrawOrder::Default, DrawOrder::Default) == rgb565(0xff0000), "nearer object hides the farther one");
    check(pair(DrawOrder::Background, DrawOrder::Default) == rgb565(0x0000ff), "Background geometry never hides later geometry");
    check(pair(DrawOrder::Default, DrawOrder::OnTop) == rgb565(0x0000ff), "On Top geometry covers nearer geometry");
    // Translucent surfaces in front blend with what is behind them.
    {
        Description d = scene();
        d.nodes[3].shading = Shading::Unlit;
        d.nodes[3].color = 0xff0000;
        auto glass = node(5, 0, NodeType::MeshInstance3D, "Glass");
        glass.shape = Shape::Quad;
        glass.shading = Shading::Unlit;
        glass.color = 0x0000ff;
        glass.alpha = 0.5f;
        glass.position = {0, 0.5f, 1.5f};
        glass.scale = {3, 3, 1};
        d.nodes.push_back(glass);
        Runtime r;
        std::string e;
        r.load(d, e);
        r.set_editor_camera({0, 0.5f, 6}, {0, 0, 0}, 70);
        r.render();
        const uint16_t c = r.pixels()[size_t(Height / 2) * Width + Width / 2];
        // Jet's blend is not linear in alpha: both colours just have to be present.
        check((c >> 11) >= 4 && (c & 31) >= 4, "a translucent quad in front shows the object behind it");
    }

    // The screen resolution travels with the scene and the package.
    {
        Description wide = scene();
        wide.width = 480;
        wide.height = 320;
        Runtime r;
        check(r.load(wide, error) && r.width() == 480 && r.pixels().size() == size_t(480 * 320), "framebuffer follows the scene resolution");
        r.render();
        check(lit(r.pixels()) > 1000, "a 480x320 scene renders");
        const auto bytes = encode_pack(wide, error);
        Description back;
        check(decode_pack(bytes.data(), bytes.size(), back, error) && back.width == 480 && back.height == 320, "pack keeps the resolution");
        check(r.load(scene(), error) && r.pixels().size() == size_t(Width * Height), "loading a 320x240 scene resizes back");
    }

    // Package v7: resolution, meshes, primitives, 2D nodes, draw order and the new material fields.
    Description packed = hud;
    packed.meshes = model.meshes;
    auto car = model.nodes[3];
    car.id = 20; car.parent = 0; car.name = "Car"; car.diffuse = 0.4f; car.reflection = true; car.texture = 0;
    car.draw_order = DrawOrder::Background;
    packed.nodes.push_back(car);
    auto ring = torus.nodes[3];
    ring.id = 21; ring.parent = 0; ring.name = "Ring"; ring.position = {1.5f, 0.5f, 0};
    packed.nodes.push_back(ring);
    const auto bytes = encode_pack(packed, error);
    if (bytes.empty()) std::cerr << "pack: " << error << '\n';
    Description back;
    check(decode_pack(bytes.data(), bytes.size(), back, error), "pack v13 round trips");
    check(back.meshes.size() == 2 && back.meshes[0].vertices.size() == model.meshes[0].vertices.size()
          && back.meshes[0].indices == model.meshes[0].indices, "pack keeps imported meshes");
    const auto& c = back.nodes[back.nodes.size() - 2];
    check(c.shape == Shape::Mesh && c.mesh == 0 && c.diffuse == 0.4f && c.reflection && c.draw_order == DrawOrder::Background
          && back.nodes.back().shape == Shape::Torus,
          "pack keeps mesh references and material fields");
    check(back.nodes[6].type == NodeType::Label && back.nodes[6].text == "HI" && back.nodes[6].text_scale == 2
          && back.nodes[4].size.x == 40, "pack keeps 2D nodes");
    check(render_scene(packed) == render_scene(back), "decoded v13 pack renders identically");
}

NodeDesc shape(uint32_t id, int32_t parent, CollisionKind kind, Vec3 size = {1, 1, 1}, float radius = 0.5f, float height = 2) {
    NodeDesc n = node(id, parent, NodeType::CollisionShape3D, "Shape");
    n.collision = kind; n.box_size = size; n.radius = radius; n.height = height;
    return n;
}
// Static 20x1x20 floor with its top at y = 0.
Description floor_scene() {
    Description d;
    d.nodes.push_back(node(1, -1, NodeType::Node3D, "World"));
    auto floor = node(2, 0, NodeType::StaticBody3D, "Floor");
    floor.position = {0, -0.5f, 0};
    d.nodes.push_back(floor);
    d.nodes.push_back(shape(3, 1, CollisionKind::Box, {20, 1, 20}));
    return d;
}
// Adds a body with one shape child; returns the body's node index.
int add_body(Description& d, uint32_t id, NodeType type, const char* name, Vec3 position, NodeDesc collision) {
    auto body = node(id, 0, type, name);
    body.position = position;
    d.nodes.push_back(body);
    const int index = int(d.nodes.size()) - 1;
    collision.id = id + 1000;
    collision.parent = index;
    d.nodes.push_back(collision);
    return index;
}
Vec3 position_of(Runtime& runtime, uint32_t id) {
    NodeDesc out;
    runtime.node_state(id, out);
    return out.position;
}
void simulate(Runtime& runtime, float seconds) {
    for (float t = 0; t < seconds; t += 1.0f / 60) runtime.update(1.0f / 60);
}
bool start_scene(Runtime& runtime, const Description& d) {
    std::string error;
    std::vector<ScriptDiagnostic> diagnostics;
    const bool ok = runtime.load(d, error) && runtime.start(diagnostics);
    if (!ok) {
        std::cerr << error;
        for (const auto& e : diagnostics) std::cerr << e.path << ':' << e.line << ' ' << e.message << '\n';
    }
    return ok;
}

void physics_tests() {
    std::string error;
    {   // A ball falls, lands on the floor and goes to sleep.
        Description d = floor_scene();
        add_body(d, 10, NodeType::RigidBody3D, "Ball", {0, 3, 0}, shape(0, 0, CollisionKind::Sphere));
        Runtime r;
        check(start_scene(r, d), "physics scene starts");
        simulate(r, 0.3f);
        check(position_of(r, 10).y < 2.7f, "a rigid body falls");
        simulate(r, 3);
        const float y = position_of(r, 10).y;
        check(std::abs(y - 0.5f) < 0.03f, "the ball rests on the floor");
        NodeDesc state;
        check(r.node_state(10, state), "rigid body state");
    }
    {   // Bounce.
        Description d = floor_scene();
        const int ball = add_body(d, 10, NodeType::RigidBody3D, "Ball", {0, 3, 0}, shape(0, 0, CollisionKind::Sphere));
        d.nodes[size_t(ball)].bounce = 0.8f;
        Runtime r;
        start_scene(r, d);
        float lowest = 10, highest_after = 0;
        bool landed = false;
        for (int i = 0; i < 180; ++i) {
            r.update(1.0f / 60);
            const float y = position_of(r, 10).y;
            if (y < 0.6f) landed = true;
            if (!landed) lowest = std::min(lowest, y);
            else highest_after = std::max(highest_after, y);
        }
        check(landed && highest_after > 1.5f, "a bouncy ball bounces back up");
    }
    {   // A box lands on a box.
        Description d = floor_scene();
        add_body(d, 10, NodeType::StaticBody3D, "Crate", {0, 0.5f, 0}, shape(0, 0, CollisionKind::Box));
        add_body(d, 20, NodeType::RigidBody3D, "Top", {0.2f, 3, 0}, shape(0, 0, CollisionKind::Box));
        Runtime r;
        start_scene(r, d);
        simulate(r, 3);
        check(std::abs(position_of(r, 20).y - 1.5f) < 0.03f, "a rigid box rests on a box");
    }
    const char* walker = R"(extends CharacterBody3D
@export var walk := Vector3.ZERO
var floor_time := 0.0

func _physics_process(delta):
	velocity.y -= 9.8 * delta
	velocity.x = walk.x
	velocity.z = walk.z
	move_and_slide()
	if is_on_floor():
		floor_time += delta
	if floor_time > 0.5 and floor_time < 0.52:
		print("floor ", is_on_floor())
)";
    {   // Character: falls, stands on the floor, stops at a wall.
        Description d = floor_scene();
        const int c = add_body(d, 10, NodeType::CharacterBody3D, "Player", {0, 2, 0}, shape(0, 0, CollisionKind::Capsule));
        d.scripts.push_back({"res://walker.jet", walker, {}});
        d.nodes[size_t(c)].script = 0;
        d.nodes[size_t(c)].overrides.push_back({"walk", jetscript::Value(jetscript::Vector3{4, 0, 0})});
        add_body(d, 30, NodeType::StaticBody3D, "Wall", {3, 1, 0}, shape(0, 0, CollisionKind::Box, {1, 2, 4}));
        Runtime r;
        check(start_scene(r, d), "character scene starts");
        simulate(r, 2);
        const Vec3 p = position_of(r, 10);
        check(has_log(r, "floor true", false), "the character lands on the floor (is_on_floor)");
        check(std::abs(p.y - 1.0f) < 0.05f, "the capsule stands on the floor");
        check(p.x > 1.8f && p.x < 2.05f, "the character stops at the wall");
    }
    auto slope = [&](float degrees_) {
        Description d = floor_scene();
        auto ramp = node(30, 0, NodeType::StaticBody3D, "Ramp");
        ramp.position = {0, 0, 0};
        ramp.rotation = {0, 0, degrees_};
        d.nodes.push_back(ramp);
        d.nodes.push_back(shape(31, int32_t(d.nodes.size()) - 1, CollisionKind::Box, {10, 1, 4}));
        const int c = add_body(d, 10, NodeType::CharacterBody3D, "Player", {0, 2.5f, 0}, shape(0, 0, CollisionKind::Capsule));
        d.scripts.push_back({"res://walker.jet", walker, {}});
        d.nodes[size_t(c)].script = 0;
        Runtime r;
        start_scene(r, d);
        simulate(r, 1);
        const float start_x = position_of(r, 10).x;
        simulate(r, 2);
        return std::abs(position_of(r, 10).x - start_x);
    };
    check(slope(20) < 0.05f, "standing on a gentle slope does not slide");
    check(slope(60) > 0.5f, "a steep slope is a wall: the character slides down");
    {   // A script disables the floor's shape: the ball resting on it falls.
        Description d = floor_scene();
        d.scripts.push_back({"res://floor.jet", "extends StaticBody3D\nvar t := 0.0\nfunc _physics_process(delta):\n"
                             "\tt += delta\n\tif t > 1.0:\n\t\tget_node(\"Shape\").disabled = true\n", {}});
        d.nodes[1].script = 0;
        add_body(d, 10, NodeType::RigidBody3D, "Ball", {0, 1, 0}, shape(0, 0, CollisionKind::Sphere));
        Runtime r;
        start_scene(r, d);
        simulate(r, 0.9f);
        const float resting = position_of(r, 10).y;
        simulate(r, 1);
        check(std::abs(resting - 0.5f) < 0.03f && position_of(r, 10).y < -1, "disabling a shape at run time removes it");
    }
    {   // A character standing on a moving platform rides it.
        Description d = floor_scene();
        const int platform = add_body(d, 30, NodeType::StaticBody3D, "Platform", {0, 1, 0}, shape(0, 0, CollisionKind::Box, {3, 0.2f, 3}));
        d.scripts.push_back({"res://walker.jet", walker, {}});
        d.scripts.push_back({"res://mover.jet", "extends StaticBody3D\nfunc _physics_process(delta):\n\tposition.x += 1.0 * delta\n", {}});
        d.nodes[size_t(platform)].script = 1;
        const int c = add_body(d, 10, NodeType::CharacterBody3D, "Player", {0, 2.2f, 0}, shape(0, 0, CollisionKind::Capsule));
        d.nodes[size_t(c)].script = 0;
        Runtime r;
        start_scene(r, d);
        simulate(r, 0.5f);
        const float offset = position_of(r, 10).x - position_of(r, 30).x;
        simulate(r, 1.5f);
        check(std::abs(position_of(r, 10).x - position_of(r, 30).x - offset) < 0.05f && position_of(r, 30).x > 1.5f,
              "a character rides a moving platform");
    }
    {   // Area: a falling ball enters and leaves it.
        Description d = floor_scene();
        add_body(d, 10, NodeType::RigidBody3D, "Ball", {0, 5, 0}, shape(0, 0, CollisionKind::Sphere));
        const int area = add_body(d, 20, NodeType::Area3D, "Zone", {0, 3, 0}, shape(0, 0, CollisionKind::Box, {2, 0.5f, 2}));
        d.scripts.push_back({"res://zone.jet", "extends Area3D\nfunc _on_body_entered(body):\n\tprint(\"in \", body.name, \" \", get_overlapping_bodies().size())\nfunc _on_body_exited(body):\n\tprint(\"out \", body.name)\n", {}});
        d.nodes[size_t(area)].script = 0;
        Runtime r;
        check(start_scene(r, d), "area scene starts");
        simulate(r, 3);
        const auto log = run_log(r);
        const bool in = std::find(log.begin(), log.end(), "in Ball 1") != log.end();
        const bool out = std::find(log.begin(), log.end(), "out Ball") != log.end();
        check(in && out, "Area3D calls _on_body_entered and _on_body_exited");
    }
    {   // The character pushes a crate.
        Description d = floor_scene();
        const int c = add_body(d, 10, NodeType::CharacterBody3D, "Player", {0, 1, 0}, shape(0, 0, CollisionKind::Capsule));
        d.scripts.push_back({"res://walker.jet", walker, {}});
        d.nodes[size_t(c)].script = 0;
        d.nodes[size_t(c)].overrides.push_back({"walk", jetscript::Value(jetscript::Vector3{3, 0, 0})});
        add_body(d, 30, NodeType::RigidBody3D, "Crate", {1.5f, 0.5f, 0}, shape(0, 0, CollisionKind::Box));
        Runtime r;
        start_scene(r, d);
        simulate(r, 2);
        check(position_of(r, 30).x > 3 && position_of(r, 10).x > 2, "the character pushes a rigid crate");
    }
    {   // Layers and masks.
        Description d = floor_scene();
        const int ball = add_body(d, 10, NodeType::RigidBody3D, "Ghost", {0, 2, 0}, shape(0, 0, CollisionKind::Sphere));
        d.nodes[size_t(ball)].collision_layer = 2;
        d.nodes[size_t(ball)].collision_mask = 2;
        Runtime r;
        start_scene(r, d);
        simulate(r, 1.5f);
        check(position_of(r, 10).y < -1, "bodies on other layers do not collide");
    }
    {   // Package v13 keeps the physics fields; 30 balls cost a few milliseconds per second.
        Description d = floor_scene();
        for (int i = 0; i < 30; ++i) {
            const int ball = add_body(d, uint32_t(100 + i * 2), NodeType::RigidBody3D, "Ball", {float(i % 6) - 2.5f, 1.0f + float(i / 6) * 1.2f, 0},
                                      shape(0, 0, CollisionKind::Sphere, {1, 1, 1}, 0.4f));
            d.nodes[size_t(ball)].name = "Ball" + std::to_string(i);
        }
        d.nodes[3].bounce = 0.3f;
        d.nodes[3].linear_velocity = {1, 2, 3};
        const auto bytes = encode_pack(d, error);
        Description back;
        check(decode_pack(bytes.data(), bytes.size(), back, error) && back.nodes[3].bounce == 0.3f && back.nodes[4].radius == 0.4f
              && back.nodes[4].collision == CollisionKind::Sphere && back.nodes[3].linear_velocity.z == 3, "pack keeps the physics fields");
        Runtime r;
        start_scene(r, d);
        const auto t0 = std::chrono::steady_clock::now();
        simulate(r, 1);
        const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        std::cout << "timing: 30 rigid spheres, 60 physics steps " << ms << " ms\n";
        bool above = true;
        for (int i = 0; i < 30; ++i) above = above && position_of(r, uint32_t(100 + i * 2)).y > 0.3f;
        check(above, "a pile of balls stays above the floor");
    }
}

// CPUParticles3D: Godot's emission cycle on the pool of Jet's ParticleSystem.
void particle_tests() {
    std::string error;
    auto emitter = [](Description& d) -> NodeDesc& {
        auto p = node(20, 0, NodeType::CPUParticles3D, "Particles");
        p.position = {0, 1, 0};
        p.direction = {0, 1, 0};
        p.spread = 20;
        p.velocity_min = p.velocity_max = 2;
        d.nodes.push_back(p);
        return d.nodes.back();
    };
    auto step = [](Runtime& r, float seconds) {
        for (float t = 0; t < seconds; t += 1.0f / 60) { r.update(1.0f / 60); r.render(); }
    };
    {   // Continuous emission: `amount` particles over each lifetime, drawn over the scene.
        Description d = scene();
        auto& p = emitter(d);
        p.amount = 20;
        p.lifetime = 1;
        Runtime r;
        check(start_scene(r, d), "particle scene starts");
        step(r, 0.5f);
        const int half = r.live_particles();
        step(r, 1.5f);
        const int steady = r.live_particles();
        check(half >= 9 && half <= 11 && steady >= 19 && steady <= 20, "particles: amount per lifetime");
        const auto with = frame(r);
        const int drawn = r.triangles();
        Description off = d;
        off.nodes.back().emitting = false;
        Runtime quiet;
        start_scene(quiet, off);
        step(quiet, 2);
        check(quiet.live_particles() == 0 && frame(quiet) != with && drawn > quiet.triangles(), "particles are drawn (one streak each)");
    }
    {   // One shot, explosiveness 1: the whole amount at once, then the emitter stops.
        Description d = scene();
        auto& p = emitter(d);
        p.amount = 30;
        p.lifetime = 0.5f;
        p.explosiveness = 1;
        p.one_shot = true;
        Runtime r;
        start_scene(r, d);
        step(r, 1.0f / 60);
        const int burst = r.live_particles();
        step(r, 0.6f);
        NodeDesc state;
        r.node_state(20, state);
        check(burst == 30 && r.live_particles() == 0 && !state.emitting, "one-shot explosive burst ends and stops emitting");
    }
    {   // Pool limit and lifetime randomness; emission shapes stay inside their volume.
        Description d = scene();
        auto& p = emitter(d);
        p.amount = 200;
        p.lifetime = 2;
        p.explosiveness = 1;
        p.lifetime_randomness = 1;
        p.emission_shape = EmissionShape::Box;
        p.emission_box = {3, 0.1f, 3};
        Runtime r;
        start_scene(r, d);
        step(r, 1.0f / 60);
        const int all = r.live_particles();
        step(r, 1);
        // Lifetimes shorter than the first frame expire at once.
        check(all >= 195 && all <= 200 && r.live_particles() > 20 && r.live_particles() < 180, "200-particle pool, randomized lifetimes");
    }
    {   // Puff style (smoke): soft discs over the scene, coloured by the node; damping slows them.
        Description d = scene();
        auto& p = emitter(d);
        p.particle_style = ParticleStyle::Puff;
        p.amount = 30;
        p.lifetime = 2;
        p.color = 0xff0000;
        p.color_end = 0xff0000;
        p.alpha = 1;
        p.puff_size = p.puff_size_end = 0.5f;
        p.velocity_min = p.velocity_max = 3;
        p.gravity = {};
        Runtime r;
        start_scene(r, d);
        step(r, 1);
        const auto smoke = frame(r);
        const size_t red = size_t(std::count_if(smoke.begin(), smoke.end(), [](uint16_t c) { return (c >> 11) > 24 && ((c >> 5) & 63) < 20; }));
        Description slow = d;
        slow.nodes.back().damping_min = slow.nodes.back().damping_max = 3; // stops within a second: puffs stay near the emitter
        Runtime damped;
        start_scene(damped, slow);
        step(damped, 1);
        check(red > 200 && r.triangles() > 0 && frame(damped) != smoke, "puff particles draw coloured discs; damping changes the motion");
    }
    {   // A mesh in front hides the puffs behind it (z-buffer), not those beside it.
        Description d = scene();
        d.nodes[3].shading = Shading::Unlit;
        d.nodes[3].color = 0x0000ff;
        d.nodes[3].position = {0, 1, 0};
        auto& p = emitter(d);
        p.position = {0, 0.6f, -2}; // behind the cube, on the line from the camera (0, 2, 5)
        p.particle_style = ParticleStyle::Puff;
        p.amount = 1; p.explosiveness = 1; p.lifetime = 10;
        p.gravity = {}; p.color = p.color_end = 0x00ff00; p.alpha = 1; p.puff_size = p.puff_size_end = 4;
        p.velocity_min = p.velocity_max = 0; // stays where it is emitted
        auto green = [](uint16_t c) { return ((c >> 5) & 63) > 30 && (c >> 11) < 8; };
        const auto plain = render_scene(d);
        int x0 = Width, y0 = Height, x1 = -1, y1 = -1;
        for (int y = 0; y < Height; ++y)
            for (int x = 0; x < Width; ++x)
                if (plain[size_t(y) * Width + x] == rgb565(0x0000ff)) { x0 = std::min(x0, x); x1 = std::max(x1, x); y0 = std::min(y0, y); y1 = std::max(y1, y); }
        const size_t centre = size_t((y0 + y1) / 2) * Width + size_t((x0 + x1) / 2), beside = size_t((y0 + y1) / 2) * Width + size_t(std::max(0, x0 - 8));
        Runtime r;
        start_scene(r, d);
        step(r, 1.5f);
        const auto pixels = frame(r);
        check(x1 >= 0 && pixels[centre] == rgb565(0x0000ff) && green(pixels[beside]), "a mesh in front hides the puffs behind it");
        Description over = d;
        over.nodes.back().no_depth_test = true;
        Runtime r_over;
        start_scene(r_over, over);
        step(r_over, 1.5f);
        check(green(frame(r_over)[centre]), "No Depth Test puffs are drawn over the meshes");
        // Jet's sparks: hidden by the z-buffer, over the cube with No Depth Test.
        auto sparks_on_cube = [&](bool over_meshes) {
            Description s = d;
            auto& e = s.nodes.back();
            e.particle_style = ParticleStyle::Sparks; e.additive = false; e.no_depth_test = over_meshes;
            e.amount = 150; e.spread = 180; e.velocity_min = e.velocity_max = 0.3f; e.emission_shape = EmissionShape::Sphere;
            e.emission_radius = 0.3f;
            Runtime rs;
            start_scene(rs, s);
            step(rs, 0.1f);
            const auto px = frame(rs);
            int white = 0;
            for (int y = y0; y <= y1; ++y)
                for (int x = x0; x <= x1; ++x) white += (px[size_t(y) * Width + x] >> 11) > 24;
            return white;
        };
        check(sparks_on_cube(false) == 0 && sparks_on_cube(true) > 10, "Jet's sparks: depth tested, or over the meshes with No Depth Test");
    }
    {   // Script API: properties and restart(), as in Godot.
        Description d = scene();
        auto& p = emitter(d);
        p.emitting = false;
        p.script = 0;
        d.scripts.push_back({"res://burst.jet", "extends CPUParticles3D\nvar done := false\nfunc _ready():\n"
            "\tamount = 5\n\tone_shot = true\n\texplosiveness = 1.0\n\tlifetime = 0.2\n\tgravity = Vector3(0, -2, 0)\n\trestart()\n"
            "func _process(delta):\n\tif not emitting and not done:\n\t\tdone = true\n\t\tprint(\"finished \", amount, \" \", gravity.y)\n", {}});
        Runtime r;
        check(start_scene(r, d), "particle script starts");
        step(r, 0.5f);
        check(has_log(r, "finished 5 -2", false), "restart() runs one cycle; properties read back");
    }
    {   // Package v13 keeps the particle fields; out-of-range values are rejected.
        Description d = scene();
        auto& p = emitter(d);
        p.amount = 64; p.lifetime = 1.5f; p.one_shot = true; p.additive = false; p.spread = 90; p.gravity = {0, -3, 1};
        p.damping_min = 0.5f; p.damping_max = 2;
        p.color_end = 0x123456; p.puff_size_end = 1.25f; p.no_depth_test = true;
        p.particle_style = ParticleStyle::Water; p.emission_shape = EmissionShape::Sphere; p.emission_radius = 0.25f;
        const auto bytes = encode_pack(d, error);
        Description back;
        check(decode_pack(bytes.data(), bytes.size(), back, error) && back.nodes.back().amount == 64 && back.nodes.back().one_shot
              && !back.nodes.back().additive && back.nodes.back().particle_style == ParticleStyle::Water
              && back.nodes.back().emission_shape == EmissionShape::Sphere && back.nodes.back().emission_radius == 0.25f
              && back.nodes.back().gravity.z == 1 && back.nodes.back().lifetime == 1.5f && back.nodes.back().damping_max == 2
              && back.nodes.back().color_end == 0x123456 && back.nodes.back().puff_size_end == 1.25f && back.nodes.back().no_depth_test,
              "pack keeps the particle fields");
        Description bad = d;
        bad.nodes.back().amount = 0;
        Runtime r;
        check(!r.load(bad, error), "particles: amount 0 is rejected");
    }
}

void scene_tests() {
    std::string error;
    Description d;
    d.path = "res://menu.jetscene";
    d.nodes.push_back(node(1, -1, NodeType::Node3D, "Menu"));
    d.nodes[0].script = 0;
    d.scripts.push_back({"res://menu.jet", "extends Node3D\nvar frames := 0\nfunc _process(delta):\n\tframes += 1\n"
        "\tif frames == 3:\n\t\tprint(\"missing \", get_tree().change_scene_to_file(\"res://nope.jetscene\"))\n"
        "\t\tprint(\"go \", get_tree().change_scene_to_file(\"res://level.jetscene\"))\n", {}});
    SceneDesc level;
    level.path = "res://level.jetscene";
    level.nodes.push_back(node(1, -1, NodeType::Node3D, "Level"));
    auto cube = node(2, 0, NodeType::MeshInstance3D, "Cube");
    cube.script = 1;
    level.nodes.push_back(cube);
    d.scripts.push_back({"res://level.jet", "extends MeshInstance3D\nvar frames := 0\nfunc _ready():\n"
        "\tprint(\"level ready \", get_tree().current_scene.name)\nfunc _process(delta):\n\tframes += 1\n"
        "\tif frames == 2:\n\t\tget_tree().reload_current_scene()\n", {}});
    d.scenes.push_back(level);
    Runtime r;
    check(start_scene(r, d), "multi-scene project starts");
    for (int i = 0; i < 3; ++i) r.update(1.0f / 60);
    const auto log = r.take_log();
    auto logged = [&](const std::string& text) {
        return std::any_of(log.begin(), log.end(), [&](const LogLine& l) { return l.text.find(text) != std::string::npos; });
    };
    check(logged("missing 19") && logged("go 0") && logged("level ready Level") && r.current_scene() == "res://level.jetscene",
          "change_scene_to_file() switches at the end of the frame; unknown scenes return ERR_CANT_OPEN");
    NodeDesc state;
    check(r.node_state(2, state) && state.name == "Cube", "the new scene's nodes are live");
    for (int i = 0; i < 3; ++i) r.update(1.0f / 60);
    check(has_log(r, "level ready Level", false), "reload_current_scene() starts the scene again");
    r.stop();
    check(r.current_scene() == "res://menu.jetscene" && r.node_state(1, state) && state.name == "Menu", "Stop goes back to the edited scene");
    const auto bytes = encode_pack(d, error);
    Description back;
    check(decode_pack(bytes.data(), bytes.size(), back, error) && back.path == d.path && back.scenes.size() == 1
          && back.scenes[0].path == "res://level.jetscene" && back.scenes[0].nodes.size() == 2, "the package keeps every scene");
    Description clash = d;
    clash.scenes.push_back(level);
    check(!r.load(clash, error), "duplicate scene paths are rejected");

    // Autoloads keep their state across scene changes and are reachable by name.
    Description game = d;
    game.scripts.push_back({"res://game.jet", "extends Node\nvar score := 0\nfunc _ready():\n\tprint(\"game ready\")\n", {}});
    game.autoloads.push_back({"Game", int32_t(game.scripts.size()) - 1});
    game.scripts[0].source = "extends Node3D\nfunc _ready():\n\tprint(\"menu ready\")\n\tGame.score = 7\n"
        "\tget_tree().change_scene_to_file(\"res://level.jetscene\")\n";
    game.scripts[1].source = "extends MeshInstance3D\nfunc _ready():\n"
        "\tprint(\"score \", Game.score, \" \", get_node(\"/root/Game\").score)\n";
    Runtime with_game;
    check(start_scene(with_game, game), "a project with an autoload starts");
    with_game.update(1.0f / 60);
    const auto lines = with_game.take_log();
    auto index_of_line = [&](const std::string& text) {
        for (size_t i = 0; i < lines.size(); ++i) if (lines[i].text.find(text) != std::string::npos) return int(i);
        return -1;
    };
    check(index_of_line("game ready") >= 0 && index_of_line("game ready") < index_of_line("menu ready"), "autoloads are ready before the scene");
    check(index_of_line("score 7 7") >= 0, "an autoload keeps its state across a scene change (Game and /root/Game)");

    const auto game_bytes = encode_pack(game, error);
    Description game_back;
    check(decode_pack(game_bytes.data(), game_bytes.size(), game_back, error) && game_back.autoloads.size() == 1
          && game_back.autoloads[0].name == "Game", "the package keeps the autoloads (scripts compiled with their names)");
    Description bad_autoload = game;
    bad_autoload.autoloads.push_back({"Input", 0});
    check(!r.load(bad_autoload, error), "an autoload can't be called Input");
}

int main(int argc, char** argv) {
    std::string error;
    bytecode_tests();
    material_tests();
    model_tests();
    physics_tests();
    particle_tests();
    scene_tests();
    Runtime runtime;
    const Description base = scene();
    check(runtime.load(base, error), "valid scene loads");
    const auto initial = frame(runtime);
    check(runtime.triangles() > 0, "Jet rasterizes geometry");
    check(lit(initial) > 500, "cube appears in the editor view");

    // Hierarchy: a child of a moved/rotated Node3D renders like the composed transform.
    {
        Description nested = base, flat = base;
        auto pivot = node(5, 0, NodeType::Node3D, "Pivot");
        pivot.position = {1, 0, 0}; pivot.rotation = {0, 90, 0};
        nested.nodes.push_back(pivot);
        nested.nodes[3].parent = 4; // the cube is now a child of Pivot (index 4)
        std::swap(nested.nodes[3], nested.nodes[4]); // keep parents before children
        nested.nodes[3].parent = 0;
        nested.nodes[4].parent = 3;
        nested.nodes[4].position = {1, 0.5f, 0};
        flat.nodes[3].position = {1, 0.5f, -1};
        flat.nodes[3].rotation = {0, 90, 0};
        check(render_scene(nested) == render_scene(flat), "child transform composes with parent (Godot conventions)");
        Description hidden = nested;
        hidden.nodes[3].visible = false;
        check(lit(render_scene(hidden)) < lit(render_scene(nested)), "hiding a parent hides its children");
        Description scaled = nested;
        scaled.nodes[3].scale = {2, 2, 2};
        check(render_scene(scaled) != render_scene(nested), "parent scale affects children");
    }

    // Validation.
    {
        Description bad = base;
        bad.nodes.push_back(node(4, 0, NodeType::Node3D, "Other"));
        check(!runtime.load(bad, error), "reject duplicate ids");
        bad = base;
        bad.nodes.push_back(node(9, 0, NodeType::Node3D, "Cube"));
        check(!runtime.load(bad, error), "reject duplicate sibling names");
        bad = base;
        bad.nodes[3].scale.x = std::numeric_limits<float>::quiet_NaN();
        check(!runtime.load(bad, error), "reject NaN transforms");
        bad = base;
        bad.nodes[3].parent = 3;
        check(!runtime.load(bad, error), "reject parents after children");
        bad = base;
        bad.nodes[3].name = "a/b";
        check(!runtime.load(bad, error), "reject invalid names");
        check(runtime.description().nodes.size() == base.nodes.size(), "failed load keeps the previous scene");
    }

    // Picking returns the node id.
    {
        check(runtime.load(base, error), "reload");
        int id = -1;
        const auto pixels = frame(runtime);
        for (int y = 0; y < Height && id < 0; y += 3)
            for (int x = 0; x < Width && id < 0; x += 3)
                if (pixels[y * Width + x] != rgb565(0x161c28)) id = runtime.pick(x, y);
        check(id == 4, "picking maps Jet geometry to the node id");
        check(runtime.pick(-1, 0) == -1, "reject pick outside the image");
    }

    // Scripts: behaviour, exported overrides, stop restores the authored scene.
    {
        Description d = base;
        d.scripts.push_back({"res://spin.jet", "extends Node3D\n@export var speed := 90.0\n\nfunc _ready():\n\tprint(\"ready \", name)\n\nfunc _process(delta):\n\trotation_degrees.y += speed * delta\n"});
        d.nodes[3].script = 0;
        check(runtime.load(d, error), "scene with script loads");
        std::vector<ScriptDiagnostic> diagnostics;
        check(runtime.start(diagnostics), "scripts compile and start");
        check(has_log(runtime, "ready Cube", false), "print() from _ready reaches the log");
        runtime.use_editor_camera(true);
        const auto before = frame(runtime);
        for (int i = 0; i < 10; ++i) runtime.update(0.05f);
        check(frame(runtime) != before, "_process rotates the cube");
        runtime.stop();
        check(frame(runtime) == initial, "stop restores the authored frame");
        d.nodes[3].overrides.push_back({"speed", jetscript::Value(0)});
        check(runtime.load(d, error) && runtime.start(diagnostics), "override loads");
        runtime.use_editor_camera(true);
        const auto still = frame(runtime);
        for (int i = 0; i < 10; ++i) runtime.update(0.05f);
        check(frame(runtime) == still, "scene override of an exported variable (int to float)");
    }

    // Scene camera is used while playing.
    {
        Description d = base;
        check(play_scene(d) != render_scene(d), "play uses the Camera3D, edit uses the editor camera");
    }

    // get_node, $ paths and value-type write-back.
    {
        Description d = base, moved = base;
        d.scripts.push_back({"res://root.jet", "extends Node3D\nfunc _ready():\n\t$Cube.position.x = 1.5\n\tget_node(\"Cube\").rotation_degrees.y = 30\n"});
        d.nodes[0].script = 0;
        moved.nodes[3].position.x = 1.5f;
        moved.nodes[3].rotation.y = 30;
        check(play_scene(d) == play_scene(moved), "$Cube.position.x = ... writes the property back");
    }

    // Input actions.
    {
        Description d = base;
        d.scripts.push_back({"res://move.jet", "extends Node3D\nfunc _process(delta):\n\tposition.x += Input.get_axis(\"ui_left\", \"ui_right\") * 2.0 * delta\n"});
        d.nodes[3].script = 0;
        check(play_scene(d, 0.5f, UiRight) != play_scene(d, 0.5f, 0), "Input.get_axis follows the pressed actions");
        // The project's own actions (Input Map): bit i = actions[i].
        Description custom = base;
        custom.actions.push_back({"jump", {"J"}});
        custom.scripts.push_back({"res://jump.jet", "extends Node3D\nfunc _process(delta):\n\tif Input.is_action_pressed(\"jump\"):\n\t\tposition.y += 2.0 * delta\n"});
        custom.nodes[3].script = 0;
        check(play_scene(custom, 0.5f, 1u << 6) != play_scene(custom, 0.5f, 0), "custom Input Map actions drive scripts");
        Description unknown = custom;
        unknown.actions.pop_back();
        Runtime r;
        std::vector<ScriptDiagnostic> diagnostics;
        r.load(unknown, error);
        r.start(diagnostics);
        r.update(0.1f);
        check(has_log(r, "The InputMap action \"jump\" doesn't exist.", true), "unknown actions report Godot's error");
        const auto bytes = encode_pack(custom, error);
        Description back;
        check(decode_pack(bytes.data(), bytes.size(), back, error) && back.actions.size() == 7 && back.actions[6].name == "jump"
              && back.actions[6].keys == std::vector<std::string>{"J"} && back.actions[4].keys[1] == "Enter", "the package keeps the Input Map");
        Description bad = custom;
        bad.actions.push_back({"jump", {}});
        Description spaced = custom;
        spaced.actions.back().name = "jump high";
        check(!r.load(bad, error) && !r.load(spaced, error), "duplicate or invalid action names are rejected");
    }

    // A press counts once per physics tick sequence and once per frame, whatever the frame rate.
    {
        Description d = base;
        d.scripts.push_back({"res://press.jet", "extends Node3D\nfunc _physics_process(delta):\n"
                             "\tif Input.is_action_just_pressed(\"ui_accept\"):\n\t\tposition.x += 1.0\n"
                             "func _process(delta):\n\tif Input.is_action_just_pressed(\"ui_accept\"):\n\t\tposition.y += 1.0\n"});
        d.nodes[3].script = 0;
        Runtime r;
        std::vector<ScriptDiagnostic> diagnostics;
        NodeDesc cube;
        r.load(d, error);
        r.start(diagnostics);
        r.set_input(UiAccept);
        r.update(0.034f); // two ticks in one frame
        r.node_state(d.nodes[3].id, cube);
        const float y = d.nodes[3].position.y;
        check(cube.position.x == 1 && cube.position.y == y + 1, "a press seen by two ticks of the same frame counts once");
        r.set_input(0);
        r.update(0.02f);
        r.set_input(UiAccept);
        r.update(0.005f); // no tick in this frame
        r.update(0.01f);
        r.node_state(d.nodes[3].id, cube);
        check(cube.position.x == 2 && cube.position.y == y + 2, "a press in a frame without ticks reaches the next tick");
    }

    // queue_free removes the node (and its children) at the end of the frame.
    {
        Description d = base, without = base;
        d.scripts.push_back({"res://free.jet", "extends Node3D\nfunc _ready():\n\t$Cube.queue_free()\n"});
        d.nodes[0].script = 0;
        without.nodes[3].visible = false;
        check(play_scene(d) == play_scene(without), "queue_free hides the node");
    }

    // Errors: compile diagnostics and runtime errors with location.
    {
        Description d = base;
        d.scripts.push_back({"res://bad.jet", "extends Node3D\nfunc _process(delta):\n\tposition.x += undefined_name\n"});
        d.nodes[3].script = 0;
        std::vector<ScriptDiagnostic> diagnostics;
        check(runtime.load(d, error) && !runtime.start(diagnostics), "compile errors block play");
        check(!diagnostics.empty() && diagnostics[0].line == 3 && diagnostics[0].path == "res://bad.jet", "compile error reports file and line");
        d.scripts[0].source = "extends Node3D\nvar target = null\nfunc _process(delta):\n\ttarget.position.x = 1\n";
        check(runtime.load(d, error) && runtime.start(diagnostics), "valid script starts");
        runtime.update(0.016f);
        bool located = false;
        for (const auto& line : runtime.take_log()) located = located || (line.error && line.path == "res://bad.jet" && line.line == 4);
        check(located, "runtime error reports file and line");
        runtime.update(0.016f); // the game keeps running after an error
        d.scripts[0].source = "extends Camera3D\n";
        check(runtime.load(d, error) && runtime.start(diagnostics), "start with mismatched base still runs");
        check(has_log(runtime, "Camera3D", true), "mismatched script base class is reported");
    }

    // Package round trip (version 3: scripts as bytecode) with overrides.
    {
        Description d = base;
        d.scripts.push_back({"res://spin.jet", "extends Node3D\n@export var speed := 1.0\n", {}});
        d.nodes[3].script = 0;
        d.nodes[3].overrides.push_back({"speed", jetscript::Value(2.5)});
        d.nodes[3].overrides.push_back({"tint", jetscript::Value(jetscript::Color{1, 0.5f, 0})});
        auto pack = encode_pack(d, error);
        Description decoded;
        check(!pack.empty() && decode_pack(pack.data(), pack.size(), decoded, error), "pack round trip");
        check(decoded.nodes.size() == d.nodes.size() && decoded.nodes[3].name == "Cube" && decoded.scripts[0].source.empty()
              && !decoded.scripts[0].bytecode.empty(), "pack keeps names and ships bytecode, not source");
        check(decoded.nodes[3].overrides.size() == 2 && decoded.nodes[3].overrides[0].value.as_real() == 2.5f
              && decoded.nodes[3].overrides[1].value.type() == jetscript::Type::Color, "pack keeps typed overrides");
        check(!decode_pack(pack.data(), pack.size() - 1, decoded, error), "reject truncated pack");
        pack.push_back(0);
        check(!decode_pack(pack.data(), pack.size(), decoded, error), "reject trailing bytes");
        std::vector<uint8_t> v2 = {'J', 'E', 'T', 'P', 2, 0, 0, 0, 0, 0, 0, 0};
        check(!decode_pack(v2.data(), v2.size(), decoded, error), "reject older packs");
        Description broken = base;
        broken.scripts.push_back({"res://bad.jet", "extends Node3D\nfunc _process(delta):\n\tposition += 1\n", {}});
        broken.nodes[3].script = 0;
        check(encode_pack(broken, error).empty() && error.find("res://bad.jet:3") != std::string::npos, "export reports compile errors");
    }

    // Scripts run from a package exactly as from source (coverage script and exported overrides).
    {
        Description d = base;
        d.scripts.push_back({"res://coverage.jet", coverage_script, {}});
        d.nodes[0].script = 0;
        d.nodes[0].overrides.push_back({"speed", jetscript::Value(4)});
        Runtime from_source, from_pack;
        std::vector<ScriptDiagnostic> diagnostics;
        check(from_source.load(d, error) && from_source.start(diagnostics), "coverage script runs from source");
        for (const auto& e : diagnostics) std::cerr << e.line << ": " << e.message << '\n';
        const auto pack = encode_pack(d, error);
        Description decoded;
        check(decode_pack(pack.data(), pack.size(), decoded, error) && from_pack.load(decoded, error) && from_pack.start(diagnostics),
              "coverage script runs from bytecode");
        const auto expected = run_log(from_source), actual = run_log(from_pack);
        check(!expected.empty() && expected[0] == "t 39 7.63 6 runother 9 6 true 4.0", "coverage script computes the expected values");
        check(!expected.empty() && expected == actual, "bytecode output matches source output");
        if (expected != actual || expected.empty())
            for (const auto& line : expected) std::cerr << "source: " << line << '\n';
        from_source.update(0.1f); from_pack.update(0.1f);
        check(frame(from_source) == frame(from_pack), "bytecode renders the same frame");
        // Corrupted bytecode inside a package: start() reports it instead of running.
        decoded.scripts[0].bytecode[decoded.scripts[0].bytecode.size() / 2] ^= 0xff;
        check(from_pack.load(decoded, error) && !from_pack.start(diagnostics) && !diagnostics.empty(), "corrupted bytecode is reported");
    }

    // Package for the VM-only desktop player test (ctest "player").
    if (argc > 1) {
        Description d = base;
        d.scripts.push_back({"res://sample.jet", "extends Node3D\nfunc _ready():\n\tprint(\"sample ready\")\nfunc _process(delta):\n\trotate_y(delta)\n", {}});
        d.nodes[3].script = 0;
        const auto pack = encode_pack(d, error);
        std::ofstream(argv[1], std::ios::binary).write(reinterpret_cast<const char*>(pack.data()), std::streamsize(pack.size()));
        check(!pack.empty(), "sample package written");
    }

    // Deleting meshes removes geometry.
    {
        Description d = base;
        d.nodes.pop_back();
        check(runtime.load(d, error), "scene without meshes loads");
        frame(runtime);
        check(runtime.triangles() == 0, "no geometry without meshes");
    }

    if (failures) { std::cerr << failures << " runtime test(s) failed\n"; return 1; }
    std::cout << "PASS: hierarchy, Godot transforms, validation, picking, scripts, input, queue_free, errors, bytecode, textures, materials, sky, OBJ import, quantization, Quad/Torus, reflections, 2D nodes, draw order, resolution, physics, particles, input map, scenes, autoloads, pack v13\n";
}
