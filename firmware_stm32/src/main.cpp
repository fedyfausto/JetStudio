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
extern "C" int _gettimeofday(struct timeval* tv, void*){
    const uint64_t us = uint64_t(millis()) * 1000 + micros() % 1000;
    tv->tv_sec = time_t(us / 1000000);
    tv->tv_usec = suseconds_t(us % 1000000);
    return 0;
}

// Same package, runtime and device Jet profile as the ESP32 firmware, but on a single core.
// Each frame runs the scripts and draws one field; the next frame draws the other field. With hardware SPI the
// DMA sends a field in the background while the next one is drawn into a second buffer.

namespace{
    // Release profile (-DJET_RELEASE): no serial port, no statistics.
#if defined(JET_RELEASE)
    constexpr bool SerialLog = false;
#else
    constexpr bool SerialLog = true;
#endif
    static_assert(FIELD_BUFFERS && HALF_WIDTH_BUFFERS && !Z_BUFFERING, "the firmware expects firmware/config/JetConfig.hpp");

    LGFX display;
    jetstudio::Runtime* runtime = nullptr;
    // fields[1] stays null when there is no RAM for a second field
    uint16_t* fields[2] = {};
    uint32_t previous = 0, total = 0, scriptsTime = 0, rendering = 0, waiting = 0;
    unsigned count = 0;

    // ESP-IDF log format ("I (ms) JetStudio: text"), so the editor's Serial monitor colours, links and parses it.
    void logLine(char level, const char* format, ...){
        if(!SerialLog) return;

        char text[320];
        va_list args;
        va_start(args, format);
        vsnprintf(text, sizeof(text), format, args);
        va_end(args);
        Serial.printf("%c (%lu) JetStudio: %s\r\n", level, millis(), text);
    }

    // Heap left: between the heap end and the stack, plus the freed blocks.
    unsigned freeHeap(){
        char top;
        return unsigned(&top - static_cast<char*>(sbrk(0))) + unsigned(mallinfo().fordblks);
    }

    [[noreturn]] void halt(const char* reason){
        logLine('E', "%s", reason);

        for(;;) delay(1000);
    }

    volatile bool playing = false;

    // Buttons wire the pin to GND and use the internal pull-up; the board's own button (B1) reads high when pressed.
    // Bit i of the mask is action i.
    void configureButtons(){
        for(int i = 0; i < int(sizeof(input_pins) / sizeof(input_pins[0])); ++i)
            if(input_pins[i] >= 0) pinMode(pinNametoDigitalPin(PinName(input_pins[i])), input_active_high >> i & 1 ? INPUT : INPUT_PULLUP);
    }

    uint32_t readButtons(){
        uint32_t mask = 0;

        for(int i = 0; i < int(sizeof(input_pins) / sizeof(input_pins[0])); ++i)
            if(input_pins[i] >= 0 && digitalReadFast(PinName(input_pins[i])) == (input_active_high >> i & 1 ? HIGH : LOW)) mask |= 1u << i;

        return mask;
    }

    void printLog(){
        for(const auto& line : runtime->take_log()){
            if(line.error && !line.path.empty()) logLine('E', "%s:%d: %s", line.path.c_str(), line.line, line.text.c_str());
            else logLine(line.error ? 'E' : 'I', "%s", line.text.c_str());
        }
    }

    // Sends a field to the display, every other row, like the ESP32 scanout. A row is prepared while the DMA sends
    // the previous one, and the DMA interrupt starts the next row, so loop() keeps drawing meanwhile.
    namespace scanout{
        std::vector<uint16_t> rows[2];
        std::vector<jetstudio::OverlayItem> overlay;
        const uint16_t* field = nullptr;
        int width = 0, height = 0, fieldWidth = 0, overlayTop = 0, overlayBottom = 0, y = 0, current = 0;
        volatile bool busy = false;
        uint32_t started = 0;
        volatile uint32_t sendTime = 0;
        // LovyanGFX could not start the DMA and sent a row itself: no interrupt comes, rows go out one after another
        bool polled = false;

#if !JET_SOFT_SPI
        // The streams LovyanGFX uses for SPI1 and SPI2 TX.
        DMA_Stream_TypeDef* stream(){ return Board::spi() == SPI2 ? DMA1_Stream4 : DMA2_Stream3; }
        IRQn_Type streamIrq(){ return Board::spi() == SPI2 ? DMA1_Stream4_IRQn : DMA2_Stream3_IRQn; }
        bool transferComplete(){ return Board::spi() == SPI2 ? (DMA1->HISR & DMA_HISR_TCIF4) != 0 : (DMA2->LISR & DMA_LISR_TCIF3) != 0; }
#endif

        void prepare(){
            uint16_t* row = rows[current].data();
            Renderer::expandSwapRGB565(row, field + size_t(y >> 1) * fieldWidth, size_t(fieldWidth));

            if(y >= overlayTop && y < overlayBottom) jetstudio::composeOverlayRow(overlay, row, y, width, true);
        }

        // Starts row y (already in panel byte order), then prepares the next one in the other buffer.
        void sendRow(){
            //waits for the previous row
            display.setAddrWindow(0, y, width, 1);
            display.panel_bus().writeBytes(reinterpret_cast<const uint8_t*>(rows[current].data()), uint32_t(width) * 2, true, true);
#if !JET_SOFT_SPI
            //neither running nor done: the row was written without the DMA
            if(!polled && !(stream()->CR & DMA_SxCR_EN) && !transferComplete()) polled = true;

            if(!polled) stream()->CR |= DMA_SxCR_TCIE;
#endif
            current ^= 1;
            y += 2;

            if(y < height) prepare();
        }

        void finish(){
            display.endWrite();
            sendTime = sendTime + (micros() - started);
            busy = false;
        }

        // DMA interrupt: a row is out.
        void rowSent(){
            //also clears the transfer complete flag
            display.panel_bus().wait();

            if(y < height){
                sendRow();

                if(!polled) return;

                //no further interrupt: the rest of the field goes out now
                while(y < height) sendRow();
            }
#if !JET_SOFT_SPI

            stream()->CR &= ~DMA_SxCR_TCIE;
#endif
            finish();
        }

        void start(int parity, const std::vector<jetstudio::OverlayItem>& items){
            //copied before the field is chosen: when the copy runs out of RAM, the second field buffer is freed
            overlay = items;
            field = fields[1] && parity ? fields[1] : fields[0];
            overlayTop = height;
            overlayBottom = 0;

            for(const auto& it : overlay){
                overlayTop = std::min(overlayTop, std::max(0, it.y));
                overlayBottom = std::max(overlayBottom, std::min(height, it.y + it.h));
            }

            y = parity;
            current = 0;
            busy = true;
            started = micros();
            prepare();
            display.startWrite();
#if JET_SOFT_SPI
            //bit-banged: each row is out when writeBytes returns.
            while(y < height) sendRow();

            finish();
#else
            //the first row starts from here: its interrupt must wait until the next row is prepared
            NVIC_DisableIRQ(streamIrq());
            sendRow();
            NVIC_EnableIRQ(streamIrq());

            if(polled){
                while(y < height) sendRow();

                finish();
            }
#endif
        }

        void wait(){
            while(busy){}
        }
    }
}

// Out of RAM (-Wl,--wrap=malloc, see platformio.ini): first the second field buffer goes, so drawing waits for
// the DMA again; if that is not enough, say so instead of crashing on a null pointer later.
extern "C" void* __real_malloc(size_t size);
extern "C" void* __wrap_malloc(size_t size){
    if(void* p = __real_malloc(size)) return p;

    if(size == 0) return nullptr;

    if(fields[1]){
        scanout::wait();

        if(runtime){
            runtime->set_framebuffer(nullptr);
            runtime->set_reflect_buffer(nullptr);
        }

        free(fields[1]);
        fields[1] = nullptr;

        if(SerialLog){
            Serial.print("W (");
            Serial.print(millis());
            Serial.print(") JetStudio: Low RAM: one field buffer from now on, the DMA no longer sends while drawing\r\n");
        }

        if(void* p = __real_malloc(size)) return p;
    }

    if(SerialLog){
        Serial.print("E (");
        Serial.print(millis());
        Serial.print(playing ? ") JetStudio: Out of memory while playing (" : ") JetStudio: Out of memory loading the scene (");
        Serial.print(unsigned(size));
        Serial.print(" bytes, ");
        Serial.print(freeHeap());
        Serial.print(" free in pieces): it does not fit in this board's RAM\r\n");
    }

    for(;;) delay(1000);
}

#if !JET_SOFT_SPI
extern "C" void DMA2_Stream3_IRQHandler(){ scanout::rowSent(); }
extern "C" void DMA1_Stream4_IRQHandler(){ scanout::rowSent(); }
#endif

void setup(){
    if(SerialLog) Serial.begin(115200);

    logLine('I', "Starting on %s", Board::name);
    Board::begin_spi();

    if(!display.init()) halt("Display initialization failed");

    display.setRotation(Board::rotation);
    display.fillScreen(TFT_BLACK);
#if !JET_SOFT_SPI
    HAL_NVIC_SetPriority(scanout::streamIrq(), 5, 0);
    HAL_NVIC_EnableIRQ(scanout::streamIrq());
#endif

    if(Board::backlight >= 0){
        pinMode(pinNametoDigitalPin(PinName(Board::backlight)), OUTPUT);
        digitalWrite(pinNametoDigitalPin(PinName(Board::backlight)), HIGH);
    }

    std::string error;
    int width = 0, height = 0;

    if(!jetstudio::packResolution(scene_data, sizeof(scene_data), width, height)) halt("Invalid scene: export the project again");

    if(width != Board::screenWidth || height != Board::screenHeight){
        logLine('E', "The scene is %dx%d, the display (%s) %dx%d: export it with the project's hardware settings",
            width, height, Board::name, Board::screenWidth, Board::screenHeight);
        halt("Wrong resolution");
    }

    //the package stays in flash: only the scene being played is decoded into RAM, and palette textures are
    //read from the flash too (no PSRAM here).
    runtime = new jetstudio::Runtime();
#if !JET_SOFT_SPI
    //before the scene, so a big scene can not take its place. Optional: without it there is one field buffer.
    fields[1] = static_cast<uint16_t*>(__real_malloc(size_t(width / jetstudio::BufferColumnStep) * (height / jetstudio::BufferRowStep) * 2));
#endif

    if(!runtime->load_pack(scene_data, sizeof(scene_data), error, true)) halt(("Invalid scene: " + error).c_str());

    std::vector<jetstudio::ScriptDiagnostic> diagnostics;

    if(!runtime->start(diagnostics)){
        for(const auto& d : diagnostics) logLine('E', "%s:%d:%d %s", d.path.c_str(), d.line, d.column, d.message.c_str());

        halt("Scripts not started");
    }

    for(auto& row : scanout::rows) row.assign(size_t(runtime->width()), 0);

    scanout::width = runtime->width();
    scanout::height = runtime->height();
    scanout::fieldWidth = runtime->bufferWidth();
    fields[0] = const_cast<uint16_t*>(runtime->pixels().data());
    printLog();
    configureButtons();
    logLine('I', "%s on %s (%s): %dx%d, fields of %dx%d, %s", Board::name, Board::port, F_CPU >= 200000000 ? "Cortex-M7" : "Cortex-M4",
        runtime->width(), runtime->height(), runtime->bufferWidth(), runtime->bufferRows(),
        fields[1] ? "DMA sends a field while the next is drawn" : JET_SOFT_SPI ? "software SPI" : "one field buffer (low RAM)");
    logLine('I', "Scene loaded: %u nodes (%u bytes), %u meshes, %u scripts; free heap %u", unsigned(runtime->node_count()),
        unsigned(runtime->nodeBytes()), unsigned(runtime->meshCount()), unsigned(runtime->description().scripts.size()), freeHeap());
    previous = micros();
    playing = true;
}

void loop(){
    const uint32_t start = micros();
    const int parity = runtime->nextField();
    runtime->set_input(readButtons());
    runtime->update(float(start - previous) / 1000000.0f);
    previous = start;
    const uint32_t updated = micros();
    //the second buffer may go while the scene grows (see __wrap_malloc): check it again before each use.
    runtime->set_framebuffer(fields[1] && parity ? fields[1] : nullptr);
    //two buffers: Water reflects the field being sent, already complete
    runtime->set_reflect_buffer(fields[1] ? fields[1 - parity] : nullptr);
    //one buffer: the display must have the whole field before it is drawn again.
    if(!fields[1]) scanout::wait();

    runtime->render();
    const uint32_t rendered = micros();
    scanout::wait();
    const uint32_t sent = micros();
    scanout::start(parity, runtime->overlay());
    printLog();
    scriptsTime += updated - start;
    rendering += rendered - updated;
    waiting += sent - rendered;
    total += micros() - start;

    if(SerialLog && ++count == 60){
        //the DMA interrupt adds to it
        noInterrupts();
        const uint32_t sendTime = scanout::sendTime;
        scanout::sendTime = 0;
        interrupts();
        //the editor's Serial monitor parses the first part, keep the format.
        logLine('I', "Frame %.2f ms (scripts %.2f ms); %d raster tris; free heap %u, PSRAM 0; render %.2f ms, field send %.2f ms (waited %.2f)",
            total / 60000.0, scriptsTime / 60000.0, runtime->triangles(), freeHeap(), rendering / 60000.0,
            sendTime / 60000.0, waiting / 60000.0);
        count = 0; total = 0; scriptsTime = 0; rendering = 0; waiting = 0;
    }
}
