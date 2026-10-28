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
// Each frame runs the scripts and draws one field; the next frame draws the other field. With hardware SPI the
// DMA sends a field in the background while the next one is drawn into a second buffer.

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
uint16_t* fields[2] = {}; // fields[1] stays null when there is no RAM for a second field
uint32_t previous = 0, total = 0, scripts_time = 0, rendering = 0, waiting = 0;
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

volatile bool playing = false;

// Buttons wire the pin to GND and use the internal pull-up; the board's own button (B1) reads high when pressed.
// Bit i of the mask is action i.
void configure_buttons() {
    for (int i = 0; i < int(sizeof(input_pins) / sizeof(input_pins[0])); ++i)
        if (input_pins[i] >= 0) pinMode(pinNametoDigitalPin(PinName(input_pins[i])), input_active_high >> i & 1 ? INPUT : INPUT_PULLUP);
}

uint32_t read_buttons() {
    uint32_t mask = 0;
    for (int i = 0; i < int(sizeof(input_pins) / sizeof(input_pins[0])); ++i)
        if (input_pins[i] >= 0 && digitalReadFast(PinName(input_pins[i])) == (input_active_high >> i & 1 ? HIGH : LOW)) mask |= 1u << i;
    return mask;
}

void print_log() {
    for (const auto& line : runtime->take_log()) {
        if (line.error && !line.path.empty()) log_line('E', "%s:%d: %s", line.path.c_str(), line.line, line.text.c_str());
        else log_line(line.error ? 'E' : 'I', "%s", line.text.c_str());
    }
}

// Sends a field to the display, every other row, like the ESP32 scanout. A row is prepared while the DMA sends
// the previous one, and the DMA interrupt starts the next row, so loop() keeps drawing meanwhile.
namespace scanout {
std::vector<uint16_t> rows[2];
std::vector<jetstudio::OverlayItem> overlay;
const uint16_t* field = nullptr;
int width = 0, height = 0, field_width = 0, overlay_top = 0, overlay_bottom = 0, y = 0, current = 0;
volatile bool busy = false;
uint32_t started = 0;
volatile uint32_t send_time = 0;

#if !JET_SOFT_SPI
// The streams LovyanGFX uses for SPI1 and SPI2 TX.
DMA_Stream_TypeDef* stream() { return Board::spi() == SPI2 ? DMA1_Stream4 : DMA2_Stream3; }
IRQn_Type stream_irq() { return Board::spi() == SPI2 ? DMA1_Stream4_IRQn : DMA2_Stream3_IRQn; }
#endif

void prepare() {
    uint16_t* row = rows[current].data();
    Renderer::expandSwapRGB565(row, field + size_t(y >> 1) * field_width, size_t(field_width));
    if (y >= overlay_top && y < overlay_bottom) jetstudio::compose_overlay_row(overlay, row, y, width, true);
}

// Starts row y (already in panel byte order), then prepares the next one in the other buffer.
void send_row() {
    display.setAddrWindow(0, y, width, 1); // waits for the previous row
    display.panel_bus().writeBytes(reinterpret_cast<const uint8_t*>(rows[current].data()), uint32_t(width) * 2, true, true);
#if !JET_SOFT_SPI
    stream()->CR |= DMA_SxCR_TCIE;
#endif
    current ^= 1;
    y += 2;
    if (y < height) prepare();
}

void finish() {
    display.endWrite();
    send_time = send_time + (micros() - started);
    busy = false;
}

// DMA interrupt: a row is out.
void row_sent() {
    display.panel_bus().wait(); // also clears the transfer complete flag
    if (y < height) {
        send_row();
        return;
    }
#if !JET_SOFT_SPI
    stream()->CR &= ~DMA_SxCR_TCIE;
#endif
    finish();
}

void start(const uint16_t* pixels, int parity, const std::vector<jetstudio::OverlayItem>& items) {
    field = pixels;
    overlay = items;
    overlay_top = height;
    overlay_bottom = 0;
    for (const auto& it : overlay) {
        overlay_top = std::min(overlay_top, std::max(0, it.y));
        overlay_bottom = std::max(overlay_bottom, std::min(height, it.y + it.h));
    }
    y = parity;
    current = 0;
    busy = true;
    started = micros();
    prepare();
    display.startWrite();
#if JET_SOFT_SPI
    // Bit-banged: each row is out when writeBytes returns.
    while (y < height) send_row();
    finish();
#else
    send_row();
#endif
}

void wait() {
    while (busy) {}
}
} // namespace scanout
} // namespace

// Out of RAM (-Wl,--wrap=malloc, see platformio.ini): first the second field buffer goes, so drawing waits for
// the DMA again; if that isn't enough, say so instead of crashing on a null pointer later.
extern "C" void* __real_malloc(size_t size);
extern "C" void* __wrap_malloc(size_t size) {
    if (void* p = __real_malloc(size)) return p;
    if (size == 0) return nullptr;
    if (fields[1]) {
        scanout::wait();
        if (runtime) runtime->set_framebuffer(nullptr);
        free(fields[1]);
        fields[1] = nullptr;
        if (SerialLog) {
            Serial.print("W (");
            Serial.print(millis());
            Serial.print(") JetStudio: Low RAM: one field buffer from now on, the DMA no longer sends while drawing\r\n");
        }
        if (void* p = __real_malloc(size)) return p;
    }
    if (SerialLog) {
        Serial.print("E (");
        Serial.print(millis());
        Serial.print(playing ? ") JetStudio: Out of memory while playing (" : ") JetStudio: Out of memory loading the scene (");
        Serial.print(unsigned(size));
        Serial.print(" bytes, ");
        Serial.print(free_heap());
        Serial.print(" free in pieces): it doesn't fit in this board's RAM\r\n");
    }
    for (;;) delay(1000);
}

#if !JET_SOFT_SPI
extern "C" void DMA2_Stream3_IRQHandler() { scanout::row_sent(); }
extern "C" void DMA1_Stream4_IRQHandler() { scanout::row_sent(); }
#endif

void setup() {
    if (SerialLog) Serial.begin(115200);
    log_line('I', "Starting on %s", Board::name);
    Board::begin_spi();
    if (!display.init()) halt("Display initialization failed");
    display.setRotation(Board::rotation);
    display.fillScreen(TFT_BLACK);
#if !JET_SOFT_SPI
    HAL_NVIC_SetPriority(scanout::stream_irq(), 5, 0);
    HAL_NVIC_EnableIRQ(scanout::stream_irq());
#endif
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
    // The package stays in flash: only the scene being played is decoded into RAM, and palette textures are
    // read from the flash too (no PSRAM here).
    runtime = new jetstudio::Runtime();
#if !JET_SOFT_SPI
    // Before the scene, so a big scene can't take its place. Optional: without it there is one field buffer.
    fields[1] = static_cast<uint16_t*>(__real_malloc(size_t(width / jetstudio::BufferColumnStep) * (height / jetstudio::BufferRowStep) * 2));
#endif
    if (!runtime->load_pack(scene_data, sizeof(scene_data), error, true)) halt(("Invalid scene: " + error).c_str());
    std::vector<jetstudio::ScriptDiagnostic> diagnostics;
    if (!runtime->start(diagnostics)) {
        for (const auto& d : diagnostics) log_line('E', "%s:%d:%d %s", d.path.c_str(), d.line, d.column, d.message.c_str());
        halt("Scripts not started");
    }
    for (auto& row : scanout::rows) row.assign(size_t(runtime->width()), 0);
    scanout::width = runtime->width();
    scanout::height = runtime->height();
    scanout::field_width = runtime->buffer_width();
    fields[0] = const_cast<uint16_t*>(runtime->pixels().data());
    print_log();
    configure_buttons();
    log_line('I', "%s on %s (%s): %dx%d, fields of %dx%d, %s", Board::name, Board::port, F_CPU >= 200000000 ? "Cortex-M7" : "Cortex-M4",
        runtime->width(), runtime->height(), runtime->buffer_width(), runtime->buffer_rows(),
        fields[1] ? "DMA sends a field while the next is drawn" : JET_SOFT_SPI ? "software SPI" : "one field buffer (low RAM)");
    log_line('I', "Scene loaded: %u nodes (%u bytes), %u meshes, %u scripts; free heap %u", unsigned(runtime->node_count()),
        unsigned(runtime->node_bytes()), unsigned(runtime->mesh_count()), unsigned(runtime->description().scripts.size()), free_heap());
    previous = micros();
    playing = true;
}

void loop() {
    const uint32_t start = micros();
    const int parity = runtime->next_field();
    runtime->set_input(read_buttons());
    runtime->update(float(start - previous) / 1000000.0f);
    previous = start;
    const uint32_t updated = micros();
    // The second buffer may go while the scene grows (see __wrap_malloc): check it again before each use.
    runtime->set_framebuffer(fields[1] && parity ? fields[1] : nullptr);
    // One buffer: the display must have the whole field before it is drawn again.
    if (!fields[1]) scanout::wait();
    runtime->render();
    const uint32_t rendered = micros();
    scanout::wait();
    const uint32_t sent = micros();
    scanout::start(fields[1] && parity ? fields[1] : fields[0], parity, runtime->overlay());
    print_log();
    scripts_time += updated - start;
    rendering += rendered - updated;
    waiting += sent - rendered;
    total += micros() - start;
    if (SerialLog && ++count == 60) {
        // The editor's Serial monitor parses the first part, keep the format.
        log_line('I', "Frame %.2f ms (scripts %.2f ms); %d raster tris; free heap %u, PSRAM 0; render %.2f ms, field send %.2f ms (waited %.2f)",
            total / 60000.0, scripts_time / 60000.0, runtime->triangles(), free_heap(), rendering / 60000.0,
            scanout::send_time / 60000.0, waiting / 60000.0);
        count = 0; total = 0; scripts_time = 0; rendering = 0; waiting = 0; scanout::send_time = 0;
    }
}
