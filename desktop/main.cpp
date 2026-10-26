#include "runtime.hpp"
#include <SDL.h>
#include <algorithm>
#include <array>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>

// Built twice: jet_player uses the editor's Jet config, jet_player_device uses firmware/config and looks like the device.

namespace {
void print_log(jetstudio::Runtime& runtime) {
    for (const auto& line : runtime.take_log()) (line.error ? std::cerr : std::cout) << line.text << '\n';
}
std::vector<std::vector<SDL_Scancode>> action_keys(const jetstudio::Description& d) {
    std::vector<std::vector<SDL_Scancode>> out;
    for (const auto& action : d.actions) {
        out.emplace_back();
        for (std::string name : action.keys) {
            // Godot -> SDL names where they differ.
            if (name == "Enter") name = "Return";
            else if (name == "Ctrl" || name == "Shift" || name == "Alt") name = "Left " + name;
            else if (name == "Meta") name = "Left GUI";
            const SDL_Scancode code = SDL_GetScancodeFromName(name.c_str());
            if (code != SDL_SCANCODE_UNKNOWN) out.back().push_back(code);
        }
    }
    return out;
}

uint32_t keyboard_actions(const std::vector<std::vector<SDL_Scancode>>& actions) {
    const Uint8* keys = SDL_GetKeyboardState(nullptr);
    uint32_t mask = 0;
    for (size_t i = 0; i < actions.size(); ++i)
        for (SDL_Scancode code : actions[i])
            if (keys[code]) mask |= 1u << i;
    return mask;
}

#if FIELD_BUFFERS

struct DeviceScreen {
    std::array<std::vector<uint16_t>, 2> fields;
    std::vector<uint16_t> screen;
    int parity = 0, width = 0, height = 0;
    explicit DeviceScreen(const jetstudio::Runtime& runtime) : width(runtime.width()), height(runtime.height()) {
        for (auto& f : fields) f.assign(size_t(runtime.buffer_width()) * runtime.buffer_rows(), 0);
        screen.assign(size_t(width) * height, 0);
    }
    void begin(jetstudio::Runtime& runtime) {
        parity = runtime.next_field();
        runtime.set_framebuffer(fields[size_t(parity)].data());
    }
    void scanout(const jetstudio::Runtime& runtime) {
        for (int y = parity; y < height; y += 2) {
            const uint16_t* source = fields[size_t(parity)].data() + size_t(y >> 1) * runtime.buffer_width();
            uint16_t* row = screen.data() + size_t(y) * width;
            for (int x = 0; x < width; ++x) row[x] = source[x >> 1];
            jetstudio::compose_overlay_row(runtime.overlay(), row, y, width, false);
        }
    }
    const uint16_t* pixels() const { return screen.data(); }
};
constexpr const char* Title = "Jet Studio · Nano ESP32 emulator (half-width fields, no z-buffer)";
#else
constexpr const char* Title = "Jet Studio · Desktop Player";
#endif

bool save_bmp(const std::string& path, const uint16_t* pixels, int width, int height) {
    SDL_Surface* surface = SDL_CreateRGBSurfaceWithFormatFrom(const_cast<uint16_t*>(pixels), width, height,
                                                              16, width * 2, SDL_PIXELFORMAT_RGB565);
    const bool ok = surface && SDL_SaveBMP(surface, path.c_str()) == 0;
    SDL_FreeSurface(surface);
    return ok;
}
} // namespace

int main(int argc, char** argv) {
    const char* usage = "Usage: jet_player scene.jetpack [--frames=N] [--fixed-step=SECONDS] [--screenshot=file.bmp]\n";
    if (argc < 2) { std::cerr << usage; return 2; }
    int frame_limit = 0;
    float fixed_step = 0;
    std::string screenshot;
    for (int i = 2; i < argc; ++i) {
        const std::string option = argv[i];
        if (option.rfind("--frames=", 0) == 0) frame_limit = std::atoi(option.c_str() + 9);
        else if (option.rfind("--fixed-step=", 0) == 0) fixed_step = float(std::atof(option.c_str() + 13));
        else if (option.rfind("--screenshot=", 0) == 0) screenshot = option.substr(13);
        else { std::cerr << usage; return 2; }
    }
    if (frame_limit < 0 || fixed_step < 0 || fixed_step > 1 || (!screenshot.empty() && frame_limit < 1)) { std::cerr << usage; return 2; }
    std::ifstream file(argv[1], std::ios::binary);
    if (!file) { std::cerr << "Cannot open the scene.\n"; return 2; }
    const std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(file)), {});
    jetstudio::Description description;
    std::string error;
    if (!jetstudio::decode_pack(bytes.data(), bytes.size(), description, error)) { std::cerr << error << '\n'; return 2; }
    jetstudio::Runtime runtime;
    if (!runtime.load(description, error)) { std::cerr << error << '\n'; return 2; }
    std::vector<jetstudio::ScriptDiagnostic> diagnostics;
    if (!runtime.start(diagnostics)) {
        for (const auto& d : diagnostics) std::cerr << d.path << ':' << d.line << ':' << d.column << ": " << d.message << '\n';
        return 3;
    }
    print_log(runtime);
    if (SDL_Init(SDL_INIT_VIDEO) != 0) { std::cerr << SDL_GetError() << '\n'; return 1; }
    const auto keys = action_keys(runtime.description());
    const int width = runtime.width(), height = runtime.height();
    const int zoom = std::max(1, 960 / width); // about 960 pixels wide, integer scale
    auto* window = SDL_CreateWindow(Title, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, width * zoom, height * zoom, SDL_WINDOW_RESIZABLE);
    auto* renderer = window ? SDL_CreateRenderer(window, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC) : nullptr;
    if (window && !renderer) renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_SOFTWARE);
    auto* texture = renderer ? SDL_CreateTexture(renderer, SDL_PIXELFORMAT_RGB565, SDL_TEXTUREACCESS_STREAMING, width, height) : nullptr;
    if (!texture) { std::cerr << SDL_GetError() << '\n'; SDL_Quit(); return 1; }
    SDL_RenderSetLogicalSize(renderer, width, height);
#if FIELD_BUFFERS
    DeviceScreen device(runtime);
#endif
    bool running = true, paused = false;
    auto previous = SDL_GetPerformanceCounter();
    int frames = 0;
    while (running) {
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            if (e.type == SDL_QUIT || (e.type == SDL_KEYDOWN && e.key.keysym.sym == SDLK_ESCAPE)) running = false;
            if (e.type == SDL_KEYDOWN && e.key.keysym.sym == SDLK_p) paused = !paused;
        }
        const auto now = SDL_GetPerformanceCounter();
        const float dt = fixed_step > 0 ? fixed_step : float(double(now - previous) / SDL_GetPerformanceFrequency());
        previous = now;
        runtime.set_input(keyboard_actions(keys));
#if FIELD_BUFFERS
        device.begin(runtime);
#endif
        runtime.update(paused ? 0 : dt);
        runtime.render();
        print_log(runtime);
#if FIELD_BUFFERS
        device.scanout(runtime);
        const uint16_t* pixels = device.pixels();
#else
        const uint16_t* pixels = runtime.pixels().data();
#endif
        SDL_UpdateTexture(texture, nullptr, pixels, width * 2);
        SDL_SetRenderDrawColor(renderer, 10, 12, 17, 255);
        SDL_RenderClear(renderer);
        SDL_RenderCopy(renderer, texture, nullptr, nullptr);
        SDL_RenderPresent(renderer);
        if (frame_limit && ++frames >= frame_limit) {
            running = false;
            if (!screenshot.empty() && !save_bmp(screenshot, pixels, width, height)) std::cerr << "Screenshot not saved: " << SDL_GetError() << '\n';
        }
    }
    SDL_DestroyTexture(texture);
    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    SDL_Quit();
}
