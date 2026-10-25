"""Board and display profiles (hardware/) and the firmware files generated from a project's hardware settings.

Pins are GPIO numbers on ESP32 and STM32 PinName values on STM32 (port * 16 + pin, so PA5 = 5, PB5 = 21).
editor/scripts/hardware.gd reads the same profiles and applies the same rules, keep them in sync.
"""
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


def firmware_dir(board):
    return FIRMWARE_STM32 if platform(board) == "stm32" else FIRMWARE


def pin_label(board, pin):
    for label, number in board["pins"]:
        if number == pin:
            return label
    return f"GPIO{pin}" if platform(board) == "esp32" else f"P{'ABCDEFGHIJK'[pin >> 4]}{pin & 15}"


def catalog(kind):
    return {path.stem: json.loads(path.read_text()) for path in sorted((HARDWARE / kind).glob("*.json"))}


def settings(config):
    """The project's hardware settings, with missing values taken from the profiles."""
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


def rotation(display, orientation):
    """LovyanGFX setRotation() value for an orientation."""
    w, h = display["panel"]
    base = {"portrait": 0, "landscape": 1, "portrait_flipped": 2, "landscape_flipped": 3}[orientation]
    return (base + 1) % 4 if w > h else base  # landscape-native panel


def validate(hw, board, display, button_pins=()):
    usable = {gpio for _, gpio in board["pins"]}
    used = {}
    for name in DISPLAY_PINS:
        pin = hw["pins"].get(name, -1)
        if pin < 0:
            if name not in OPTIONAL_PINS:
                raise RuntimeError(f"Display pin {name} is not set")
            continue
        if pin not in usable:
            raise RuntimeError(f"{pin_label(board, pin)} ({name}) is not usable on {board['name']}")
        if pin in used:
            raise RuntimeError(f"{pin_label(board, pin)} is used by both {used[pin]} and {name}")
        used[pin] = name
    spi = board.get("spi")
    for name in ("sck", "mosi"):
        if spi and hw["pins"][name] not in spi[name]:
            raise RuntimeError(f"{name.upper()} of {board['name']} must be one of "
                               + ", ".join(pin_label(board, p) for p in spi[name]) + f" ({spi['port']})")
    for action, pin in button_pins:
        if pin in used:
            raise RuntimeError(f"{pin_label(board, pin)} of {action} is also the display's {used[pin]}")
        if pin not in usable:
            raise RuntimeError(f"{pin_label(board, pin)} of {action} is not usable on {board['name']}")
    if display.get("controller") not in CONTROLLERS:
        raise RuntimeError(f"Unsupported display controller {display.get('controller')}")
    width, height = resolution(display, hw["orientation"])
    if width % 2 or height % 2 or min(width, height) < 64:
        raise RuntimeError(f"Display resolution {width}x{height} is not supported (even sides, at least 64)")
    if not 1000000 <= hw["spi_hz"] <= 80000000:
        raise RuntimeError("SPI clock must be between 1 and 80 MHz")


def write_if_changed(path, text):
    path.parent.mkdir(parents=True, exist_ok=True)
    if not path.exists() or path.read_text() != text:
        path.write_text(text)


def board_header(hw, board, display):
    p = hw["pins"]
    width, height = resolution(display, hw["orientation"])
    panel_w, panel_h = display["panel"]
    memory_w, memory_h = display.get("memory", display["panel"])
    offset_x, offset_y = display.get("offset", [0, 0])
    stm32 = platform(board) == "stm32"
    source = f"hardware/boards/{hw['board']}.json, " + (
        "custom display" if hw["display"] == "custom" else f"hardware/displays/{hw['display']}.json")
    if stm32:
        # LovyanGFX's STM32 bus writes the SPI registers itself but takes transactions from Arduino's SPI object.
        # begin_spi() starts that object on the same pins and peripheral.
        includes = "#include <SPI.h>\n"
        pins_note = "// Pins are STM32 PinName values (port * 16 + pin).\n"
        board_extra = f"""constexpr const char* port = "{board['spi']['port']}";
inline void begin_spi() {{
    SPI.setSCLK(PinName(clock));
    SPI.setMOSI(PinName(mosi));
    SPI.begin();
}}
"""
        bus = f"""        b.spi_port = {board['spi']['port']};
        b.spi_mode = Board::spiMode;
        b.freq_write = Board::writeHz;
        b.freq_read = 8000000;"""
        light_member, light = "", ""  # main.cpp switches the backlight on
    else:
        includes, pins_note, board_extra = "", "", ""
        bus = """        b.spi_host = SPI2_HOST; // required by the queued DMA scanout
        b.spi_mode = Board::spiMode;
        b.freq_write = Board::writeHz;
        b.freq_read = 16000000;
        b.spi_3wire = false;
        b.use_lock = true;
        b.dma_channel = SPI_DMA_CH_AUTO;"""
        light_member = "    lgfx::Light_PWM light;\n"
        light = """        if (Board::backlight >= 0) {
            auto l = light.config();
            l.pin_bl = Board::backlight;
            l.invert = false;
            l.freq = 44100;
            l.pwm_channel = 7;
            light.config(l);
            panel.setLight(&light);
        }
"""
    return f"""#pragma once
// Generated by tools/hardware.py from {source} and the project's hardware settings.
// Do not edit: change them in Project Settings.
// Panel/bus setup adapted from board_3d / JetExamples (MIT, see hardware/LICENSE.JetExamples).
#define LGFX_USE_V1
#include <LovyanGFX.hpp>
{includes}
namespace Board {{
constexpr const char* name = {json.dumps(board['name'] + ' + ' + display['name'], ensure_ascii=False)};
constexpr int screenWidth = {width}, screenHeight = {height}, rotation = {rotation(display, hw['orientation'])};
{pins_note}constexpr int clock = {p['sck']}, mosi = {p['mosi']}, dc = {p['dc']}, cs = {p['cs']}, reset = {p.get('reset', -1)}, backlight = {p.get('backlight', -1)};
constexpr int spiMode = {display.get('spi_mode', 0)};
constexpr int writeHz = {hw['spi_hz']};
{board_extra}}}

class LGFX : public lgfx::LGFX_Device {{
    lgfx::{CONTROLLERS[display['controller']]} panel;
{light_member}    lgfx::Bus_SPI bus;
public:
    LGFX() {{
        auto b = bus.config();
{bus}
        b.pin_sclk = Board::clock;
        b.pin_mosi = Board::mosi;
        b.pin_miso = -1;
        b.pin_dc = Board::dc;
        bus.config(b);
        panel.setBus(&bus);
        auto c = panel.config();
        c.pin_cs = Board::cs;
        c.pin_rst = Board::reset;
        c.pin_busy = -1;
        c.panel_width = {panel_w};
        c.panel_height = {panel_h};
        c.memory_width = {memory_w};
        c.memory_height = {memory_h};
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
{light}        setPanel(&panel);
    }}
}};
"""


def boards_ini(target):
    lines = ["; Generated by tools/hardware.py from hardware/boards/*.json. Do not edit.",
             "; One environment per board, debug and release.", ""]
    for key, board in catalog("boards").items():
        if firmware_dir(board) != target:
            continue
        if target == FIRMWARE:
            lines += [f"[env:{key}]", "extends = jet", f"board = {board['platformio_board']}",
                      f"board_build.cmake_extra_args = -DJET_BOARD={key}", "",
                      f"[env:{key}_release]", f"extends = env:{key}",
                      f"board_build.cmake_extra_args = -DJET_BOARD={key} -DJET_RELEASE=ON", ""]
        else:
            # USB CDC for the log on boards without an ST-LINK virtual COM port.
            flags = "${jet.build_flags}" + (" -DPIO_FRAMEWORK_ARDUINO_ENABLE_CDC -DUSBCON" if board.get("usb_serial") else "")
            lines += [f"[env:{key}]", "extends = jet", f"board = {board['platformio_board']}",
                      f"upload_protocol = {board.get('upload', 'stlink')}", f"build_flags = {flags}", "",
                      f"[env:{key}_release]", f"extends = env:{key}", f"build_flags = {flags} -DJET_RELEASE", ""]
    return "\n".join(lines)


def generate(config, button_pins=()):
    """Writes the firmware files; returns (environment, (width, height), board label, firmware dir)."""
    hw, board, display = settings(config)
    validate(hw, board, display, button_pins)
    target = firmware_dir(board)
    write_if_changed(target / "src/generated_board.hpp", board_header(hw, board, display))
    for folder in (FIRMWARE, FIRMWARE_STM32):
        write_if_changed(folder / "boards.ini", boards_ini(folder))
    for key, data in catalog("boards").items():
        if platform(data) == "esp32":
            write_if_changed(FIRMWARE / "boards" / f"{key}.defaults",
                             f"# Generated from hardware/boards/{key}.json (flash and PSRAM).\n" + "\n".join(data["sdkconfig"]) + "\n")
    return hw["board"], resolution(display, hw["orientation"]), f"{board['name']} + {display['name']}", target
