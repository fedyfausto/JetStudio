// The runtime first: Arduino.h defines radians()/degrees() as macros.
#include "runtime.hpp"
#include <Arduino.h>
#undef radians
#undef degrees
#include "generated_board.hpp"
#include "PixelOps.hpp"
#include "generated_scene.hpp"
#include "generated_inputs.hpp"
#include <cstdarg>
#include <cstring>
#include <malloc.h>
#include <sys/time.h>
#include <unistd.h>

// newlib's clock for std::chrono (JetScript's randomize() seed): time since boot.
extern "C" int _gettimeofday(struct timeval* tv, void*) {
    const uint64_t us = uint64_t(millis()) * 1000 + micros() % 1000;
    tv->tv_sec = time_t(us / 1000000);
    tv->tv_usec = suseconds_t(us % 1000000);
    return 0;
}

// Same package, runtime and device Jet profile as the ESP32 firmware, but on a single core.
// Each frame runs the scripts, draws one field and sends its rows; the next frame draws the other field.

namespace {
// Release profile (-DJET_RELEASE): no serial port, no statistics.
#if defined(JET_RELEASE)
constexpr bool SerialLog = false;
#else
constexpr bool SerialLog = true;
#endif
static_assert(FIELD_BUFFERS && HALF_WIDTH_BUFFERS && !Z_BUFFERING, "the firmware expects firmware/config/JetConfig.hpp");

LGFX display;
jetstudio::Runtime* runtime = nullptr;
std::vector<uint16_t> row; // one display row, sent while the next is prepared
uint32_t previous = 0, total = 0, scripts_time = 0, rendering = 0, sending = 0;
unsigned count = 0;

// ESP-IDF log format ("I (ms) JetStudio: text"), so the editor's Serial monitor colours, links and parses it.
void log_line(char level, const char* format, ...) {
    if (!SerialLog) return;
    char text[320];
    va_list args;
    va_start(args, format);
    vsnprintf(text, sizeof(text), format, args);
    va_end(args);
    Serial.printf("%c (%lu) JetStudio: %s\r\n", level, millis(), text);
}

// Heap left: between the heap end and the stack, plus the freed blocks.
unsigned free_heap() {
    char top;
    return unsigned(&top - static_cast<char*>(sbrk(0))) + unsigned(mallinfo().fordblks);
}

[[noreturn]] void halt(const char* reason) {
    log_line('E', "%s", reason);
    for (;;) delay(1000);
}

// Buttons wire the pin to GND and use the internal pull-up; bit i of the mask is action i.
void configure_buttons() {
    for (int pin : input_pins)
        if (pin >= 0) pinMode(pinNametoDigitalPin(PinName(pin)), INPUT_PULLUP);
}

uint32_t read_buttons() {
    uint32_t mask = 0;
    for (int i = 0; i < int(sizeof(input_pins) / sizeof(input_pins[0])); ++i)
        if (input_pins[i] >= 0 && digitalReadFast(PinName(input_pins[i])) == LOW) mask |= 1u << i;
    return mask;
}

void print_log() {
    for (const auto& line : runtime->take_log()) {
        if (line.error && !line.path.empty()) log_line('E', "%s:%d: %s", line.path.c_str(), line.line, line.text.c_str());
        else log_line(line.error ? 'E' : 'I', "%s", line.text.c_str());
    }
}

// LovyanGFX sends long writes with a DMA that hangs on the F7 (no SPI1 interrupt handler, DTCM, data cache).
// Rows go through the SPI data register like its command bytes: the SPI clock is the limit either way.
void write_row(const uint16_t* pixels, int count) {
#if JET_SOFT_SPI
    display.panel_bus().writeBytes(reinterpret_cast<const uint8_t*>(pixels), uint32_t(count) * 2, true, false);
#else
    SPI_TypeDef* spi = Board::spi();
    auto* dr = reinterpret_cast<volatile uint8_t*>(&spi->DR);
    while (!(spi->SR & SPI_SR_TXE) || (spi->SR & SPI_SR_BSY)) {} // the address command is out
    digitalWriteFast(PinName(Board::dc), HIGH);
    const auto* bytes = reinterpret_cast<const uint8_t*>(pixels);
    for (int i = 0, n = count * 2; i < n; ++i) {
        while (!(spi->SR & SPI_SR_TXE)) {}
        *dr = bytes[i];
    }
    while (!(spi->SR & SPI_SR_TXE) || (spi->SR & SPI_SR_BSY)) {}
#endif
}

void clear_screen() {
    std::vector<uint16_t> black(size_t(Board::screenWidth), 0);
    display.startWrite();
    display.setAddrWindow(0, 0, Board::screenWidth, Board::screenHeight);
    for (int y = 0; y < Board::screenHeight; ++y) write_row(black.data(), Board::screenWidth);
    display.endWrite();
}

// Sends the rows of the field just drawn (every other display row), like the ESP32 scanout.
void send_field(int parity) {
    const int width = runtime->width(), height = runtime->height(), field_width = runtime->buffer_width();
    const uint16_t* field = runtime->pixels().data();
    const auto& overlay = runtime->overlay();
    int overlay_top = height, overlay_bottom = 0;
    for (const auto& it : overlay) {
        overlay_top = std::min(overlay_top, std::max(0, it.y));
        overlay_bottom = std::max(overlay_bottom, std::min(height, it.y + it.h));
    }
    display.startWrite();
    for (int y = parity; y < height; y += 2) {
        Renderer::expandSwapRGB565(row.data(), field + size_t(y >> 1) * field_width, size_t(field_width));
        if (y >= overlay_top && y < overlay_bottom) jetstudio::compose_overlay_row(overlay, row.data(), y, width, true);
        display.setAddrWindow(0, y, width, 1);
        write_row(row.data(), width); // already in panel byte order
    }
    display.endWrite();
}
} // namespace

void setup() {
    if (SerialLog) Serial.begin(115200);
    log_line('I', "Starting on %s", Board::name);
    Board::begin_spi();
    // Reset by hand and init without LovyanGFX's clear(), which would start its DMA (see write_row).
    if (Board::reset >= 0) {
        const uint32_t reset = pinNametoDigitalPin(PinName(Board::reset));
        pinMode(reset, OUTPUT);
        digitalWrite(reset, LOW);
        delay(10);
        digitalWrite(reset, HIGH);
        delay(120);
    }
    if (!display.init_without_reset(false)) halt("Display initialization failed");
    display.setRotation(Board::rotation);
    clear_screen();
    if (Board::backlight >= 0) {
        pinMode(pinNametoDigitalPin(PinName(Board::backlight)), OUTPUT);
        digitalWrite(pinNametoDigitalPin(PinName(Board::backlight)), HIGH);
    }
    std::string error;
    int width = 0, height = 0;
    if (!jetstudio::pack_resolution(scene_data, sizeof(scene_data), width, height)) halt("Invalid scene: export the project again");
    if (width != Board::screenWidth || height != Board::screenHeight) {
        log_line('E', "The scene is %dx%d, the display (%s) %dx%d: export it with the project's hardware settings",
            width, height, Board::name, Board::screenWidth, Board::screenHeight);
        halt("Wrong resolution");
    }
    // The package stays in flash: only the scene being played is decoded into RAM.
    runtime = new jetstudio::Runtime();
    if (!runtime->load_pack(scene_data, sizeof(scene_data), error)) halt(("Invalid scene: " + error).c_str());
    std::vector<jetstudio::ScriptDiagnostic> diagnostics;
    if (!runtime->start(diagnostics)) {
        for (const auto& d : diagnostics) log_line('E', "%s:%d:%d %s", d.path.c_str(), d.line, d.column, d.message.c_str());
        halt("Scripts not started");
    }
    row.assign(size_t(runtime->width()), 0);
    print_log();
    configure_buttons();
    log_line('I', "%s on %s (%s): %dx%d, fields of %dx%d", Board::name, Board::port, F_CPU >= 200000000 ? "Cortex-M7" : "Cortex-M4",
        runtime->width(), runtime->height(), runtime->buffer_width(), runtime->buffer_rows());
    log_line('I', "Scene loaded: %u nodes, %u meshes, %u scripts; free heap %u", unsigned(runtime->node_count()),
        unsigned(runtime->mesh_count()), unsigned(runtime->description().scripts.size()), free_heap());
    previous = micros();
}

void loop() {
    const uint32_t start = micros();
    const int parity = runtime->next_field();
    runtime->set_input(read_buttons());
    runtime->update(float(start - previous) / 1000000.0f);
    previous = start;
    const uint32_t updated = micros();
    runtime->render();
    const uint32_t rendered = micros();
    send_field(parity);
    print_log();
    const uint32_t sent = micros();
    scripts_time += updated - start;
    rendering += rendered - updated;
    sending += sent - rendered;
    total += sent - start;
    if (SerialLog && ++count == 60) {
        // The editor's Serial monitor parses the first part, keep the format.
        log_line('I', "Frame %.2f ms (scripts %.2f ms); %d raster tris; free heap %u, PSRAM 0; render %.2f ms, field send %.2f ms",
            total / 60000.0, scripts_time / 60000.0, runtime->triangles(), free_heap(), rendering / 60000.0, sending / 60000.0);
        count = 0; total = 0; scripts_time = 0; rendering = 0; sending = 0;
    }
}
