// Runtime with the device Jet config (firmware/config); fields are put back together the way the firmware sends them.
#include "runtime.hpp"
#include <algorithm>
#include <array>
#include <iostream>

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

Description scene() {
    Description d;
    d.nodes.push_back(node(1, -1, NodeType::Node3D, "World"));
    auto camera = node(2, 0, NodeType::Camera3D, "Camera3D");
    camera.position = {0, 1, 6}; camera.extras().current = true;
    d.nodes.push_back(camera);
    auto cube = node(3, 0, NodeType::MeshInstance3D, "Cube");
    cube.position = {0, 0.6f, 2.5f}; cube.color = 0xff0000; cube.shading = Shading::Unlit;
    d.nodes.push_back(cube);
    return d;
}

// Consecutive frames (both parities) assembled into a full screen image.
struct Device {
    std::array<std::vector<uint16_t>, 2> fields;
    std::vector<uint16_t> screen;
    std::vector<int> parities;
    int width = 0;
    void frame(Runtime& runtime) {
        width = runtime.width();
        for (auto& f : fields) f.resize(size_t(runtime.buffer_width()) * runtime.buffer_rows());
        screen.resize(size_t(width) * runtime.height());
        const int parity = runtime.next_field();
        parities.push_back(parity);
        runtime.set_framebuffer(fields[size_t(parity)].data());
        runtime.render();
        for (int y = parity; y < runtime.height(); y += 2) {
            const uint16_t* source = fields[size_t(parity)].data() + size_t(y >> 1) * runtime.buffer_width();
            // Panel byte order, then the 2D nodes, as the scanout does.
            uint16_t* row = screen.data() + size_t(y) * width;
            for (int x = 0; x < width; ++x) row[x] = uint16_t(source[x >> 1] << 8 | source[x >> 1] >> 8);
            compose_overlay_row(runtime.overlay(), row, y, width, true);
            for (int x = 0; x < width; ++x) row[x] = uint16_t(row[x] << 8 | row[x] >> 8);
        }
    }
    uint16_t at(int x, int y) const { return screen[size_t(y) * width + size_t(x)]; }
};

uint16_t centre(const Description& d) {
    Runtime runtime;
    std::string error;
    if (!runtime.load(d, error)) { std::cerr << error << '\n'; return 0; }
    std::vector<ScriptDiagnostic> diagnostics;
    runtime.start(diagnostics); // play: the scene camera
    Device device;
    device.frame(runtime);
    device.frame(runtime);
    return device.at(Width / 2, Height / 2 + 6);
}
} // namespace

int main() {
    Runtime runtime;
    std::string error;
    check(runtime.load(scene(), error), "scene loads with the device configuration");
    check(runtime.buffer_width() == Width / 2 && runtime.buffer_rows() == Height / 2, "device fields are 160x120");
    std::vector<ScriptDiagnostic> diagnostics;
    check(runtime.start(diagnostics), "play starts");
    Device device;
    device.frame(runtime);
    device.frame(runtime);
    device.frame(runtime);
    check(device.parities.size() == 3 && device.parities[0] != device.parities[1] && device.parities[1] != device.parities[2],
          "consecutive frames fill alternate rows");
    size_t red = 0;
    for (uint16_t c : device.screen) red += c == rgb565(0xff0000);
    check(red > 2000, "the cube is drawn into the fields");
    check(device.at(Width / 2, Height / 2) == rgb565(0xff0000) && device.at(Width / 2, Height / 2 + 1) == rgb565(0xff0000),
          "both parities carry the image");
    check(device.at(2, 2) != rgb565(0xff0000), "the clear colour fills the rest");

    // 2D nodes are composited at full resolution while rows are sent.
    Description hud = scene();
    auto panel = node(9, 0, NodeType::ColorRect, "Panel");
    panel.position = {11, 21, 0}; panel.extras().size = {3, 3, 0}; panel.color = 0x00ff00;
    hud.nodes.push_back(panel);
    Runtime overlay;
    check(overlay.load(hud, error) && overlay.start(diagnostics), "HUD scene starts");
    Device screen;
    screen.frame(overlay);
    screen.frame(overlay);
    check(screen.at(11, 21) == rgb565(0x00ff00) && screen.at(13, 23) == rgb565(0x00ff00) && screen.at(10, 21) != rgb565(0x00ff00)
          && screen.at(14, 21) != rgb565(0x00ff00), "HUD pixels are not halved (odd column and row exact)");
    check(overlay.pick(12, 22) == 9 && overlay.pick(160, 120) == -1, "2D picking works; no 3D picking on the device");

    // Without z-buffer: depth sorting and the draw bands.
    Description pair = scene();
    auto far = node(5, 0, NodeType::MeshInstance3D, "Far");
    far.shading = Shading::Unlit; far.color = 0x0000ff; far.position = {0, 0.6f, -1}; far.scale = {2, 2, 2};
    pair.nodes.push_back(far);
    check(centre(pair) == rgb565(0xff0000), "nearer object drawn over the farther one (depth sort)");
    Description background = pair;
    background.nodes[2].draw_order = DrawOrder::Background;
    check(centre(background) == rgb565(0x0000ff), "Background geometry never hides later geometry");
    Description on_top = pair;
    on_top.nodes[3].draw_order = DrawOrder::OnTop;
    check(centre(on_top) == rgb565(0x0000ff), "On Top geometry covers nearer geometry");

    // Panorama sky into the half-width field layout.
    Description sky = scene();
    auto env = node(6, 0, NodeType::WorldEnvironment, "Env");
    env.extras().background_mode = Background::Panorama;
    env.extras().sky_texture = 0;
    sky.nodes.push_back(env);
    std::vector<uint8_t> rgba(64 * 32 * 4);
    for (size_t i = 0; i < 64 * 32; ++i) { rgba[i * 4] = 40; rgba[i * 4 + 1] = uint8_t(i % 64 * 4); rgba[i * 4 + 2] = 200; rgba[i * 4 + 3] = 255; }
    TextureAsset panorama;
    check(make_texture("res://sky.png", 64, 32, rgba.data(), panorama, error), "panorama texture");
    sky.textures.push_back(panorama);
    Runtime skybox;
    check(skybox.load(sky, error) && skybox.start(diagnostics), "panorama scene starts");
    Device view;
    view.frame(skybox);
    view.frame(skybox);
    check(std::find(panorama.palette.begin(), panorama.palette.end(), view.at(3, 3)) != panorama.palette.end()
          && std::find(panorama.palette.begin(), panorama.palette.end(), view.at(3, 4)) != panorama.palette.end(),
          "panorama fills both fields from the texture");

    // Other displays: 480x320 (ST7796) and 240x240 (square ST7789 / GC9A01).
    for (auto [w, h] : {std::pair{480, 320}, std::pair{240, 240}}) {
        Description d = scene();
        d.width = w; d.height = h;
        Runtime other;
        check(other.load(d, error) && other.start(diagnostics), "scene loads at another resolution");
        check(other.width() == w && other.buffer_width() == w / 2 && other.buffer_rows() == h / 2, "fields follow the resolution");
        Device screen_other;
        screen_other.frame(other);
        screen_other.frame(other);
        check(screen_other.at(w / 2, h / 2) == rgb565(0xff0000), "the cube is centred at any resolution");
    }
    // Particles: puffs land in the half-width field rows, and meshes hide them unless No Depth Test is on.
    {
        auto two_fields = [&](const Description& scene_desc, int updates) {
            Runtime r;
            check(r.load(scene_desc, error) && r.start(diagnostics), "particle scene starts");
            for (int i = 0; i < updates; ++i) r.update(1.0f / 60);
            Device device;
            device.frame(r);
            device.frame(r);
            return device;
        };
        auto green = [](uint16_t c) { return ((c >> 5) & 63) > 30 && (c >> 11) < 8; };
        auto red = [](uint16_t c) { return (c >> 11) > 24 && ((c >> 5) & 63) < 16; };

        Description d = scene();
        auto puff = node(9, 0, NodeType::CPUParticles3D, "Smoke");
        puff.position = {0, 1, 3};
        puff.extras().particle_style = ParticleStyle::Puff;
        puff.extras().amount = 1; puff.extras().explosiveness = 1; puff.extras().lifetime = 10;
        puff.extras().gravity = {}; puff.color = puff.extras().color_end = 0x00ff00; puff.alpha = 1; puff.extras().puff_size = puff.extras().puff_size_end = 1;
        d.nodes.push_back(puff);
        auto edge = puff; // huge, half off screen
        edge.id = 10; edge.name = "Edge"; edge.position = {2.2f, 2.4f, 5}; edge.extras().puff_size = edge.extras().puff_size_end = 40;
        d.nodes.push_back(edge);

        Device screen = two_fields(d, 90); // past the fade in
        check(green(screen.at(Width / 2, Height / 2)) && green(screen.at(Width / 2, Height / 2 + 1)), "puffs fill both fields at the screen centre");

        // Behind the red cube (z 2.5).
        Description behind = d;
        behind.nodes.pop_back();
        behind.nodes[3].position = {0, 0.6f, 1.5f};
        behind.nodes[3].extras().puff_size = behind.nodes[3].extras().puff_size_end = 5;
        Device view = two_fields(behind, 90);
        check(red(view.at(Width / 2, Height / 2 + 6)) && green(view.at(Width / 2 - 40, Height / 2 + 6)),
              "meshes hide the puffs behind them (occlusion cells)");

        Description over = behind;
        over.nodes[3].extras().no_depth_test = true;
        check(green(two_fields(over, 90).at(Width / 2, Height / 2 + 6)), "No Depth Test puffs are drawn over the meshes");

        auto quad_centre = [&](float yaw) {
            Description q = behind;
            q.nodes[2].shape = Shape::Quad;
            q.nodes[2].rotation = {0, yaw, 0};
            return two_fields(q, 90).at(Width / 2, Height / 2 + 6);
        };
        check(red(quad_centre(0)) && green(quad_centre(180)), "only front faces hide particles, as Jet culls");

        auto sparks_on_cube = [&](bool over_meshes) {
            Description sparks = behind;
            auto& e = sparks.nodes[3];
            e.extras().particle_style = ParticleStyle::Sparks; e.extras().additive = false; e.extras().no_depth_test = over_meshes;
            e.extras().amount = 150; e.extras().spread = 180; e.extras().velocity_min = e.extras().velocity_max = 0.2f;
            e.extras().emission_shape = EmissionShape::Sphere; e.extras().emission_radius = 0.2f;
            const Device frames = two_fields(sparks, 6);
            int bright = 0;
            for (int y = Height / 2 - 8; y <= Height / 2 + 14; ++y)
                for (int x = Width / 2 - 8; x <= Width / 2 + 8; ++x) bright += ((frames.at(x, y) >> 5) & 63) > 40;
            return bright;
        };
        check(sparks_on_cube(false) == 0 && sparks_on_cube(true) > 5, "sparks behind a mesh are left out on the device, unless No Depth Test");
    }

    Description wrong = scene();
    wrong.width = 321;
    check(!runtime.load(wrong, error) && error.find("321") != std::string::npos, "odd or out-of-range resolutions are rejected");

    if (failures) { std::cerr << failures << " device test(s) failed\n"; return 1; }
    std::cout << "PASS: device profile (half-width fields, no z-buffer): layout, parity, HUD rows, draw bands, panorama, resolutions, puffs\n";
}
