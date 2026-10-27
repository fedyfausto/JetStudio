extends RefCounted
## Same checks as tools/hardware.py, which generates the firmware files from these settings.
## ESP32-S3 pins are GPIO numbers. STM32 pins are PinName values (port * 16 + pin).

const DEFAULT_BOARD := "nano_esp32"
const DEFAULT_DISPLAY := "ili9341_240x320"
const ORIENTATIONS := ["landscape", "portrait", "landscape_flipped", "portrait_flipped"]
const ORIENTATION_NAMES := ["Landscape", "Portrait", "Landscape (flipped)", "Portrait (flipped)"]
const DISPLAY_PINS := ["sck", "mosi", "cs", "dc", "reset", "backlight"]
const PIN_NAMES := {"sck": "SCK (clock)", "mosi": "MOSI / SDA", "cs": "CS", "dc": "DC / RS", "reset": "RESET", "backlight": "Backlight"}
const OPTIONAL_PINS := ["reset", "backlight"]
const CONTROLLERS := ["ILI9341", "ST7789", "ST7796", "GC9A01", "ST7735S"]
const CUSTOM_DISPLAY := {"name": "Custom", "controller": "ILI9341", "panel": [240, 320], "memory": [240, 320],
	"offset": [0, 0], "invert": false, "rgb_order": false, "spi_hz": 40000000, "spi_mode": 0,
	"notes": "Choose controller, panel size (in portrait), offset and colors."}

static var boards := {}   # id -> profile
static var displays := {} # id -> profile (plus "custom")

static func load_catalog(root: String) -> void:
	boards = _read(root.path_join("hardware/boards"))
	displays = _read(root.path_join("hardware/displays"))
	displays["custom"] = CUSTOM_DISPLAY

static func _read(folder: String) -> Dictionary:
	var out := {}
	var files := Array(DirAccess.get_files_at(folder))
	files.sort()
	for file in files:
		if file.get_extension() != "json": continue
		var data = JSON.parse_string(FileAccess.get_file_as_string(folder.path_join(file)))
		if data is Dictionary: out[file.get_basename()] = data
	return out

## The project's hardware settings, with profile defaults where something is missing.
static func settings(config: Dictionary) -> Dictionary:
	var hw: Dictionary = config.get("hardware", {}).duplicate(true)
	if not boards.has(str(hw.get("board", ""))): hw.board = DEFAULT_BOARD if boards.has(DEFAULT_BOARD) else (boards.keys()[0] if not boards.is_empty() else "")
	if not displays.has(str(hw.get("display", ""))): hw.display = DEFAULT_DISPLAY
	if not str(hw.get("orientation", "")) in ORIENTATIONS: hw.orientation = "landscape"
	var display := display_of(hw)
	if int(hw.get("spi_hz", 0)) <= 0: hw.spi_hz = int(display.get("spi_hz", 40000000))
	var pins: Dictionary = boards.get(hw.board, {}).get("display_pins", {}).duplicate()
	for key in hw.get("pins", {}):
		if key in DISPLAY_PINS: pins[key] = int(hw.pins[key])
	hw.pins = pins
	return hw

static func display_of(hw: Dictionary) -> Dictionary:
	if str(hw.get("display", "")) == "custom":
		var custom := CUSTOM_DISPLAY.duplicate(true)
		custom.merge(hw.get("custom_display", {}), true)
		return custom
	return displays.get(str(hw.get("display", "")), displays.get(DEFAULT_DISPLAY, CUSTOM_DISPLAY))

static func resolution(hw: Dictionary) -> Vector2i:
	var panel: Array = display_of(hw).get("panel", [240, 320])
	var a := int(panel[0])
	var b := int(panel[1])
	if str(hw.get("orientation", "landscape")).begins_with("landscape"): return Vector2i(maxi(a, b), mini(a, b))
	return Vector2i(mini(a, b), maxi(a, b))

static func platform(hw: Dictionary) -> String:
	return str(boards.get(str(hw.get("board", "")), {}).get("platform", "esp32"))

## Board pins as [label, pin] pairs.
static func board_pins(hw: Dictionary) -> Array:
	return boards.get(str(hw.get("board", "")), {}).get("pins", [])

## Pins where the board's SPI peripheral drives SCK or MOSI. Empty on ESP32, where any pin works.
static func spi_pins(hw: Dictionary, key: String) -> Array[int]:
	var out: Array[int] = []
	for pin in boards.get(str(hw.get("board", "")), {}).get("spi", {}).get(key, []): out.append(int(pin))
	return out

static func pin_label(hw: Dictionary, gpio: int) -> String:
	var stm32 := platform(hw) == "stm32"
	for entry in board_pins(hw):
		if int(entry[1]) != gpio: continue
		if stm32 or str(entry[0]).begins_with("GPIO"): return str(entry[0])
		return "%s · GPIO%d" % [entry[0], gpio]
	return "P%s%d" % ["ABCDEFGHIJK"[(gpio >> 4) & 15], gpio & 15] if stm32 else "GPIO%d" % gpio

## STM32 SCK/MOSI off the SPI peripheral: the firmware sends bit by bit (same check as tools/hardware.py).
static func software_spi(hw: Dictionary) -> bool:
	for key in ["sck", "mosi"]:
		var allowed := spi_pins(hw, key)
		if not allowed.is_empty() and not int(hw.pins.get(key, -1)) in allowed: return true
	return false

## Errors that block a firmware build.
static func problems(hw: Dictionary, buttons: Dictionary = {}) -> Array:
	var out := []
	var usable := {}
	for entry in board_pins(hw): usable[int(entry[1])] = true
	var used := {}
	for key in DISPLAY_PINS:
		var pin := int(hw.pins.get(key, -1))
		if pin < 0:
			if not key in OPTIONAL_PINS: out.append("Display pin %s not set." % PIN_NAMES[key])
			continue
		if not usable.has(pin): out.append("%s (%s) is not usable on this board." % [pin_label(hw, pin), PIN_NAMES[key]])
		elif used.has(pin): out.append("%s is used by both %s and %s." % [pin_label(hw, pin), PIN_NAMES[used[pin]], PIN_NAMES[key]])
		used[pin] = key
	for action in buttons:
		var pin := int(buttons[action])
		if used.has(pin): out.append("The %s button pin is also the display's %s pin." % [action, PIN_NAMES[used[pin]]])
	var size := resolution(hw)
	if size.x % 2 or size.y % 2 or mini(size.x, size.y) < 64 or size.x > 800 or size.y > 480:
		out.append("Resolution %d×%d not supported (even sides, 64 up to 800×480)." % [size.x, size.y])
	return out
