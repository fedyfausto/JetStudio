# Board and display profiles (hardware/) and the firmware files generated from a project's hardware settings.
# Pins are GPIO numbers on ESP32 and STM32 PinName values on STM32 (port * 16 + pin, so PA5 = 5, PB5 = 21).
# editor/scripts/hardware.gd reads the same profiles and applies the same rules, keep them in sync.
import json
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
HARDWARE = ROOT / "hardware"
FIRMWARE = ROOT / "firmware"
FIRMWARE_STM32 = ROOT / "firmware_stm32"
DEFAULT_BOARD, DEFAULT_DISPLAY = "nano_esp32", "ili9341_240x320"
ORIENTATIONS = ["landscape", "portrait", "landscape_flipped", "portrait_flipped"]
DISPLAY_PINS = ["sck", "mosi", "cs", "dc", "reset", "backlight"]
OPTIONAL_PINS = {"reset", "backlight"}
CONTROLLERS = {"ILI9341": "Panel_ILI9341", "ST7789": "Panel_ST7789", "ST7796": "Panel_ST7796",
               "GC9A01": "Panel_GC9A01", "ST7735S": "Panel_ST7735S"}
CUSTOM_DISPLAY = {"name": "Custom", "controller": "ILI9341", "panel": [240, 320], "memory": [240, 320],
                  "offset": [0, 0], "invert": False, "rgb_order": False, "spi_hz": 40000000, "spi_mode": 0}


def platform(board):
    return board.get("platform", "esp32")


def firmwareDir(board):
    return FIRMWARE_STM32 if platform(board) == "stm32" else FIRMWARE


def pinLabel(board, pin):
    for label, number in board["pins"]:
        if number == pin:
            return label

    if platform(board) == "esp32":
        return f"GPIO{pin}"

    return f"P{'ABCDEFGHIJK'[pin >> 4]}{pin & 15}" if 0 <= pin < 176 else f"pin {pin}"


def catalog(kind):
    return {path.stem: json.loads(path.read_text(encoding="utf-8")) for path in sorted((HARDWARE / kind).glob("*.json"))}


# The project's hardware settings, with missing values taken from the profiles.
def settings(config):
    boards, displays = catalog("boards"), catalog("displays")
    hw = dict(config.get("hardware", {}))

    if hw.get("board") not in boards:
        hw["board"] = DEFAULT_BOARD

    if hw.get("display") != "custom" and hw.get("display") not in displays:
        hw["display"] = DEFAULT_DISPLAY

    display = dict(CUSTOM_DISPLAY, **hw.get("custom_display", {})) if hw["display"] == "custom" else displays[hw["display"]]

    if hw.get("orientation") not in ORIENTATIONS:
        hw["orientation"] = "landscape"

    hw["spi_hz"] = int(hw.get("spi_hz") or display["spi_hz"])
    pins = dict(boards[hw["board"]]["display_pins"])
    pins.update({k: int(v) for k, v in hw.get("pins", {}).items() if k in DISPLAY_PINS})
    hw["pins"] = pins
    return hw, boards[hw["board"]], display


def resolution(display, orientation):
    w, h = display["panel"]
    landscape = orientation.startswith("landscape")
    return (max(w, h), min(w, h)) if landscape else (min(w, h), max(w, h))


# LovyanGFX setRotation() value for an orientation.
def rotation(display, orientation):
    w, h = display["panel"]
    base = {"portrait": 0, "landscape": 1, "portrait_flipped": 2, "landscape_flipped": 3}[orientation]
    #landscape-native panel
    return (base + 1) % 4 if w > h else base


def validate(hw, board, display, buttonPins=()):
    usable = {gpio for _, gpio in board["pins"]}
    used = {}

    for name in DISPLAY_PINS:
        pin = hw["pins"].get(name, -1)

        if pin < 0:
            if name not in OPTIONAL_PINS:
                raise RuntimeError(f"Display pin {name} is not set")

            continue

        if pin not in usable:
            raise RuntimeError(f"{pinLabel(board, pin)} ({name}) is not usable on {board['name']}")

        if pin in used:
            raise RuntimeError(f"{pinLabel(board, pin)} is used by both {used[pin]} and {name}")

        used[pin] = name

    for action, pin in buttonPins:
        if pin in used:
            raise RuntimeError(f"{pinLabel(board, pin)} of {action} is also the display's {used[pin]}")

        if pin not in usable:
            raise RuntimeError(f"{pinLabel(board, pin)} of {action} is not usable on {board['name']}")

    if display.get("controller") not in CONTROLLERS:
        raise RuntimeError(f"Unsupported display controller {display.get('controller')}")

    width, height = resolution(display, hw["orientation"])

    if width % 2 or height % 2 or min(width, height) < 64:
        raise RuntimeError(f"Display resolution {width}x{height} is not supported (even sides, at least 64)")

    if not 1000000 <= hw["spi_hz"] <= 80000000:
        raise RuntimeError("SPI clock must be between 1 and 80 MHz")


def writeIfChanged(path, text):
    path.parent.mkdir(parents=True, exist_ok=True)

    if not path.exists() or path.read_text(encoding="utf-8") != text:
        path.write_text(text, encoding="utf-8")


# STM32 SCK/MOSI that the board's SPI peripheral can not drive: the firmware sends bit by bit instead.
def softwareSpi(hw, board):
    spi = board.get("spi")
    return bool(spi) and (hw["pins"]["sck"] not in spi["sck"] or hw["pins"]["mosi"] not in spi["mosi"])


def boardHeader(hw, board, display):
    p = hw["pins"]
    width, height = resolution(display, hw["orientation"])
    panelW, panelH = display["panel"]
    memoryW, memoryH = display.get("memory", display["panel"])
    offset_x, offset_y = display.get("offset", [0, 0])
    stm32 = platform(board) == "stm32"
    source = f"hardware/boards/{hw['board']}.json, " + (
        "custom display" if hw["display"] == "custom" else f"hardware/displays/{hw['display']}.json")
    busClass = "lgfx::Bus_SPI"

    if stm32 and softwareSpi(hw, board):
        includes = '#define JET_SOFT_SPI 1\n#include "soft_spi.hpp"\n'
        pinsNote = "    // Pins are STM32 PinName values (port * 16 + pin). SCK/MOSI are not on the SPI peripheral: software SPI.\n"
        boardExtra = """    constexpr const char* port = "software SPI";
    inline void begin_spi(){}
"""
        bus, busClass = "", "SoftSpiBus"
        #main.cpp switches the backlight on
        lightMember, light = "", ""
    elif stm32:
        #LovyanGFX's STM32 bus writes the SPI registers itself but takes transactions from Arduino's SPI object.
        #begin_spi() starts that object on the same pins and peripheral.
        includes = "#define JET_SOFT_SPI 0\n#include <SPI.h>\n"
        pinsNote = "    // Pins are STM32 PinName values (port * 16 + pin).\n"
        boardExtra = f"""    constexpr const char* port = "{board['spi']['port']}";
    inline SPI_TypeDef* spi(){{ return {board['spi']['port']}; }}

    inline void begin_spi(){{
        SPI.setSCLK(PinName(clock));
        SPI.setMOSI(PinName(mosi));
        SPI.begin();
    }}
"""
        bus = f"""            b.spi_port = {board['spi']['port']};
            b.spi_mode = Board::spiMode;
            b.freq_write = Board::writeHz;
            b.freq_read = 8000000;"""
        #main.cpp switches the backlight on
        lightMember, light = "", ""
    else:
        includes, pinsNote, boardExtra = "", "", ""
        #SPI2 on the ESP32-S3, where the queued DMA scanout drives it; a board profile may name another host
        note = "            //required by the queued DMA scanout\n" if board.get("chip", "esp32s3") == "esp32s3" else ""
        bus = f"""{note}            b.spi_host = {board.get('spi_host', 'SPI2_HOST')};
            b.spi_mode = Board::spiMode;
            b.freq_write = Board::writeHz;
            b.freq_read = 16000000;
            b.spi_3wire = false;
            b.use_lock = true;
            b.dma_channel = SPI_DMA_CH_AUTO;"""
        lightMember = "        lgfx::Light_PWM light;\n"
        light = """
            if(Board::backlight >= 0){
                auto l = light.config();
                l.pin_bl = Board::backlight;
                l.invert = false;
                l.freq = 44100;
                l.pwm_channel = 7;
                light.config(l);
                panel.setLight(&light);
            }
"""

    busLines = bus + "\n" if bus else ""
    return f"""#pragma once
// Generated by tools/hardware.py from {source} and the project's hardware settings.
// Do not edit: change them in Project Settings.
// Panel/bus setup adapted from board_3d / JetExamples (MIT, see hardware/LICENSE.JetExamples).
#define LGFX_USE_V1
#include <LovyanGFX.hpp>
{includes}
namespace Board{{
    constexpr const char* name = {json.dumps(board['name'] + ' + ' + display['name'], ensure_ascii=False)};
    constexpr int screenWidth = {width}, screenHeight = {height}, rotation = {rotation(display, hw['orientation'])};
{pinsNote}    constexpr int clock = {p['sck']}, mosi = {p['mosi']}, dc = {p['dc']}, cs = {p['cs']}, reset = {p.get('reset', -1)}, backlight = {p.get('backlight', -1)};
    constexpr int spiMode = {display.get('spi_mode', 0)};
    constexpr int writeHz = {hw['spi_hz']};
{boardExtra}}}

class LGFX : public lgfx::LGFX_Device{{
    public:
        {busClass}& panel_bus(){{ return bus; }}

        LGFX(){{
            auto b = bus.config();
{busLines}            b.pin_sclk = Board::clock;
            b.pin_mosi = Board::mosi;
            b.pin_miso = -1;
            b.pin_dc = Board::dc;
            bus.config(b);
            panel.setBus(&bus);

            auto c = panel.config();
            c.pin_cs = Board::cs;
            c.pin_rst = Board::reset;
            c.pin_busy = -1;
            c.panel_width = {panelW};
            c.panel_height = {panelH};
            c.memory_width = {memoryW};
            c.memory_height = {memoryH};
            c.offset_x = {offset_x};
            c.offset_y = {offset_y};
            c.offset_rotation = 0;
            c.dummy_read_pixel = 8;
            c.dummy_read_bits = 1;
            c.readable = false;
            c.invert = {'true' if display.get('invert') else 'false'};
            c.rgb_order = {'true' if display.get('rgb_order') else 'false'};
            c.dlen_16bit = false;
            c.bus_shared = false;
            panel.config(c);
{light}
            setPanel(&panel);
        }}

    private:
        lgfx::{CONTROLLERS[display['controller']]} panel;
{lightMember}        {busClass} bus;
}};
"""


def boardsIni(target):
    lines = ["; Generated by tools/hardware.py from hardware/boards/*.json. Do not edit.",
             "; One environment per board, debug and release.", ""]

    for key, board in catalog("boards").items():
        if firmwareDir(board) != target:
            continue

        if target == FIRMWARE:
            #PlatformIO writes its board's flash size into the image header, whatever sdkconfig says
            sizes = [line.split("=")[0].removeprefix("CONFIG_ESPTOOLPY_FLASHSIZE_") for line in board.get("sdkconfig", [])
                     if line.startswith("CONFIG_ESPTOOLPY_FLASHSIZE_")]
            flash = [f"board_upload.flash_size = {sizes[0]}"] if sizes else []
            lines += [f"[env:{key}]", "extends = jet", f"board = {board['platformio_board']}", *flash,
                      f"board_build.cmake_extra_args = -DJET_BOARD={key}", "",
                      f"[env:{key}_release]", f"extends = env:{key}",
                      f"board_build.cmake_extra_args = -DJET_BOARD={key} -DJET_RELEASE=ON", ""]
        else:
            #USB CDC for the log on boards without an ST-LINK virtual COM port.
            flags = "${jet.build_flags}" + (" -DPIO_FRAMEWORK_ARDUINO_ENABLE_CDC -DUSBCON" if board.get("usb_serial") else "")
            lines += [f"[env:{key}]", "extends = jet", f"board = {board['platformio_board']}",
                      f"upload_protocol = {board.get('upload', 'stlink')}", f"build_flags = {flags}", "",
                      f"[env:{key}_release]", f"extends = env:{key}", f"build_flags = {flags} -DJET_RELEASE", ""]

    return "\n".join(lines)


# Writes the firmware files; returns (environment, (width, height), board label, firmware dir).
def generate(config, buttonPins=()):
    hw, board, display = settings(config)
    validate(hw, board, display, buttonPins)
    target = firmwareDir(board)
    writeIfChanged(target / "src/generated_board.hpp", boardHeader(hw, board, display))

    for folder in (FIRMWARE, FIRMWARE_STM32):
        writeIfChanged(folder / "boards.ini", boardsIni(folder))

    for key, data in catalog("boards").items():
        if platform(data) == "esp32":
            writeIfChanged(FIRMWARE / "boards" / f"{key}.defaults",
                             f"# Generated from hardware/boards/{key}.json (flash and PSRAM).\n" + "\n".join(data["sdkconfig"]) + "\n")

    return hw["board"], resolution(display, hw["orientation"]), f"{board['name']} + {display['name']}", target
