extends VBoxContainer

signal script_link(path: String, line: int)
signal connection_changed(connected: bool)
signal preferences_changed(port: String, baud: int)
# Firmware frame stats summary, "" when unknown.
signal stats_changed(text: String)

const Style := preload("res://scripts/theme.gd")
const Icons := preload("res://scripts/icons.gd")
const BAUDS := [9600, 57600, 115200, 230400, 460800, 921600]
const MAX_LINES := 4000

# JetSerial (GDExtension)
var serial: Object
# Set by the editor (addr2line on the Debug ELF): Callable(addresses) -> Array of [function, location].
var decode: Callable
var _portSelect: OptionButton
var _baudSelect: OptionButton
var _connectButton: Button
var _resetButton: Button
var _autoCheck: CheckBox
var _timeCheck: CheckBox
var _statsText := ""
var _output: RichTextLabel
# [{path, name}]
var _ports := []
var _preferredPort := ""
# the user asked for a connection (kept across resets and flashes)
var _wanted := false
var _suspended := false
var _retry := 0.0
# ms: keep retrying after a flash even without auto-reconnect
var _forceUntil := 0
var _pending := PackedByteArray()
var _pendingAge := 0.0
var _lastStatsMs := -1
var _ansi := RegEx.create_from_string("\u001b\\[([0-9;]*)m")
var _espLog := RegEx.create_from_string("^([EWIDV]) \\((\\d+)\\) ([^:]+): (.*)$")
var _frameStats := RegEx.create_from_string("^Frame ([\\d.]+) ms \\(scripts ([\\d.]+) ms\\); (\\d+) raster tris; free heap (\\d+), PSRAM (\\d+)")
var _scriptRef := RegEx.create_from_string("(res://[^\\s:]+\\.jet):(\\d+)")
var _backtracePc := RegEx.create_from_string("0x([0-9a-fA-F]{8}):0x[0-9a-fA-F]{8}")


func _ready():
	add_theme_constant_override("separation", 4)
	var bar := HBoxContainer.new()
	bar.add_theme_constant_override("separation", 6)
	add_child(bar)
	_portSelect = OptionButton.new()
	_portSelect.custom_minimum_size.x = 180
	_portSelect.fit_to_longest_item = false
	_portSelect.clip_text = true
	_portSelect.focus_mode = Control.FOCUS_NONE
	_portSelect.tooltip_text = "The board's USB port (Nano ESP32: \"Espressif USB JTAG serial debug unit\"; Nucleo: \"STMicroelectronics STLINK\")"
	_portSelect.item_selected.connect(func(_i): _remember())
	bar.add_child(_portSelect)
	var refresh := Button.new()
	refresh.theme_type_variation = "FlatButton"
	refresh.icon = Icons.get_icon("Reload")
	refresh.tooltip_text = "Refresh the port list"
	refresh.focus_mode = Control.FOCUS_NONE
	refresh.pressed.connect(refreshPorts)
	bar.add_child(refresh)
	_baudSelect = OptionButton.new()

	for baud in BAUDS: _baudSelect.add_item("%d baud" % baud)

	_baudSelect.select(BAUDS.find(115200))
	_baudSelect.focus_mode = Control.FOCUS_NONE
	_baudSelect.tooltip_text = "Baud rate: 115200 for the firmware (the ESP32-S3's native USB ignores it)"
	_baudSelect.item_selected.connect(func(_i): _remember())
	bar.add_child(_baudSelect)
	_connectButton = Button.new()
	_connectButton.focus_mode = Control.FOCUS_NONE
	_connectButton.pressed.connect(func(): disconnectPort() if _wanted else connectPort())
	bar.add_child(_connectButton)
	_resetButton = Button.new()
	_resetButton.text = "Reset"
	_resetButton.icon = Icons.get_icon("Reload")
	_resetButton.tooltip_text = "Reset the ESP32 over USB (as after flashing) to see the log from boot (STM32 boards are reset with their RESET button)"
	_resetButton.focus_mode = Control.FOCUS_NONE
	_resetButton.pressed.connect(reset_board)
	bar.add_child(_resetButton)
	_autoCheck = CheckBox.new()
	_autoCheck.text = "Reconnect"
	_autoCheck.button_pressed = true
	_autoCheck.tooltip_text = "Reconnect automatically after a reset or when the cable is plugged back in"
	_autoCheck.focus_mode = Control.FOCUS_NONE
	bar.add_child(_autoCheck)
	_timeCheck = CheckBox.new()
	_timeCheck.text = "Time"
	_timeCheck.tooltip_text = "Prefix each line with the PC's time"
	_timeCheck.focus_mode = Control.FOCUS_NONE
	bar.add_child(_timeCheck)
	_output = RichTextLabel.new()
	_output.scroll_following = true
	_output.selection_enabled = true
	_output.context_menu_enabled = true
	_output.size_flags_vertical = Control.SIZE_EXPAND_FILL
	_output.add_theme_font_override("normal_font", Style.codeFont)
	_output.add_theme_font_size_override("normal_font_size", 13)
	_output.meta_clicked.connect(func(meta):
		var text := str(meta)
		var colon := text.rfind(":")
		if colon > 6: script_link.emit(text.substr(0, colon), int(text.substr(colon + 1))))
	add_child(_output)
	_updateUi()
	_system("Serial monitor: connect the board via USB (STM32 Nucleo: the ST-LINK USB) and press Connect. The log comes from the Debug firmware.")


func setPreferences(port: String, baud: int):
	_preferredPort = port

	if BAUDS.has(baud): _baudSelect.select(BAUDS.find(baud))

	refreshPorts()


func isConnectedToBoard() -> bool:
	return serial != null and serial.is_open()


func baud() -> int:
	return BAUDS[maxi(_baudSelect.selected, 0)]


func refreshPorts():
	if serial == null: return

	var current := selectedPort()

	if current.is_empty(): current = _preferredPort

	_ports = serial.list_ports()
	_portSelect.clear()
	var best := -1

	for i in _ports.size():
		var port: Dictionary = _ports[i]
		_portSelect.add_item(str(port.name))
		_portSelect.set_item_tooltip(i, str(port.path))

		if str(port.path) == current: best = i
		elif best < 0 and RegEx.create_from_string("(?i)espressif|arduino|esp32|jtag|stlink|stmicro").search(str(port.name)): best = i

	if _ports.is_empty(): _portSelect.add_item("No ports found")

	_portSelect.select(maxi(best, 0))
	_updateUi()


func selectedPort() -> String:
	var i := _portSelect.selected
	return str(_ports[i].path) if i >= 0 and i < _ports.size() else ""


func connectPort(quiet: bool = false) -> bool:
	if serial == null: return false

	var path := selectedPort()

	if path.is_empty():
		refreshPorts()
		path = selectedPort()

	if path.is_empty():
		if not quiet: _system("No serial port: plug in the board and press Refresh.", Style.WARNING)

		return false

	var error: String = serial.open(path, baud())

	if not error.is_empty():
		if not quiet: _system("Could not open the port: " + error, Style.ERROR)

		return false

	_wanted = true
	_pending.clear()
	_system("Connected to %s · %d baud" % [path, baud()], Style.SUCCESS)
	_remember()
	_updateUi()
	connection_changed.emit(true)
	return true


func disconnectPort():
	_wanted = false
	_forceUntil = 0

	if serial != null and serial.is_open():
		serial.close()
		_system("Disconnected.")

	_updateUi()
	connection_changed.emit(false)


func reset_board():
	if not isConnectedToBoard(): return

	_system("Resetting the board…")

	if not serial.reset_board(): _system("This port does not support resetting the board.", Style.WARNING)

	_forceUntil = Time.get_ticks_msec() + 8000


# Frees the port for esptool. Returns whether it was connected.
func suspend() -> bool:
	var wasOpen := isConnectedToBoard()
	_suspended = true

	if wasOpen:
		serial.close()
		_system("Port released for flashing…")

	_updateUi()
	return wasOpen


# After a flash: reconnect (the board re-enumerates for a few seconds).
func resume(open: bool):
	_suspended = false

	if open:
		_wanted = true
		_forceUntil = Time.get_ticks_msec() + 12000
		_retry = 0.5

	_updateUi()


func clear():
	_output.clear()


func _process(delta: float):
	if serial == null: return

	if serial.is_open():
		var data: PackedByteArray = serial.read()

		if not data.is_empty(): feed(data)
		elif not _pending.is_empty():
			#a line without newline (prompt, partial write): show it after a short pause.
			_pendingAge += delta

			if _pendingAge > 0.3: _flushPending()

		if not serial.is_open(): _lost()
	elif _wanted and not _suspended and (_autoCheck.button_pressed or Time.get_ticks_msec() < _forceUntil):
		_retry -= delta

		if _retry <= 0.0:
			_retry = 1.0
			refreshPorts()
			connectPort(true)


func _lost():
	_flushPending()
	var automatic := _autoCheck.button_pressed or Time.get_ticks_msec() < _forceUntil
	_system("Board disconnected (reset or cable)%s" % ("; reconnecting automatically…" if automatic else "."), Style.WARNING)
	_retry = 0.5
	_updateUi()
	connection_changed.emit(false)


# Raw bytes from the board (also used by the tests).
func feed(data: PackedByteArray):
	_pending.append_array(data)
	_pendingAge = 0.0
	var start := 0

	for i in _pending.size():
		if _pending[i] == 10:
			_addLine(_pending.slice(start, i).get_string_from_utf8().trim_suffix("\r"))
			start = i + 1

	_pending = _pending.slice(start)

	if _pending.size() > 4096: _flushPending()


func _flushPending():
	if _pending.is_empty(): return

	_addLine(_pending.get_string_from_utf8().trim_suffix("\r"))
	_pending.clear()
	_pendingAge = 0.0


func _addLine(raw: String):
	var color := Style.TEXT
	var found := _ansi.search(raw)

	if found:
		var codes := found.get_string(1).split(";")

		if "31" in codes: color = Style.ERROR
		elif "33" in codes: color = Style.WARNING
		elif "32" in codes: color = Style.TEXT

	var text := _ansi.sub(raw, "", true)
	var entry := _espLog.search(text)

	if entry:
		match entry.get_string(1):
			"E": color = Style.ERROR
			"W": color = Style.WARNING
			"D", "V": color = Style.MUTED

		var stats := _frameStats.search(entry.get_string(4))

		if stats:
			_showStats(int(entry.get_string(2)), stats)
			color = Style.MUTED

	_write(text, color)

	if text.strip_edges().begins_with("Backtrace:"): _decodeBacktrace(text)


# Turns "Backtrace: 0xPC:0xSP ..." lines into source locations, like PlatformIO's esp32_exception_decoder.
func _decodeBacktrace(text: String):
	var addresses := PackedStringArray()

	for found in _backtracePc.search_all(text): addresses.append("0x" + found.get_string(1))

	if addresses.is_empty(): return

	var frames: Array = decode.call(addresses) if decode.is_valid() else []

	if frames.is_empty():
		_system("Backtrace not decoded: it needs the board's Debug firmware build (Build or Flash with · Debug).", Style.WARNING)
		return

	for frame in frames:
		_output.push_color(Style.WARNING if str(frame[1]).begins_with("firmware/") or str(frame[1]).begins_with("runtime/") else Style.MUTED)
		_output.add_text("    ↳ %s  " % frame[0])
		_output.push_color(Style.LABEL)
		_output.add_text(str(frame[1]) + "\n")
		_output.pop()
		_output.pop()


func _showStats(timeMs: int, stats: RegExMatch):
	var fps := ""

	if _lastStatsMs >= 0 and timeMs > _lastStatsMs: fps = "%.1f FPS · " % (60000.0 / (timeMs - _lastStatsMs))

	_lastStatsMs = timeMs
	_setStats("%s%.1f ms (script %.1f) · %s tri · heap %s · PSRAM %s" % [fps, float(stats.get_string(1)), float(stats.get_string(2)),
		stats.get_string(3), String.humanize_size(int(stats.get_string(4))), String.humanize_size(int(stats.get_string(5)))])


func statsText() -> String:
	return _statsText


func _setStats(text: String):
	if text == _statsText: return

	_statsText = text
	stats_changed.emit(text)


func _write(text: String, color: Color):
	if _output.get_paragraph_count() > MAX_LINES: _output.remove_paragraph(0)

	_output.push_color(color)

	if _timeCheck.button_pressed:
		var now := Time.get_time_dict_from_system()
		_output.add_text("[%02d:%02d:%02d] " % [now.hour, now.minute, now.second])
	#script errors from the firmware ("res://x.jet:12: ...") open the script.
	var link := _scriptRef.search(text)

	if link:
		_output.add_text(text.substr(0, link.get_start()))
		_output.push_meta(link.get_string())
		_output.push_underline()
		_output.add_text(link.get_string())
		_output.pop()
		_output.pop()
		_output.add_text(text.substr(link.get_end()))
	else:
		_output.add_text(text)

	_output.add_text("\n")
	_output.pop()


func _system(text: String, color: Color = Style.MUTED):
	_write("— " + text, color)


func _remember():
	var port := selectedPort()

	if not port.is_empty(): preferences_changed.emit(port, baud())


func _updateUi():
	if _connectButton == null: return

	var open := isConnectedToBoard()
	var waiting := _wanted and not open
	_connectButton.text = "Disconnect" if open or waiting else "Connect"
	_connectButton.icon = Icons.get_icon("Stop" if open or waiting else "Play")
	_connectButton.tooltip_text = "Waiting for the board…" if waiting else ""
	_resetButton.disabled = not open
	_portSelect.disabled = open
	_baudSelect.disabled = open

	if not open and not waiting:
		_lastStatsMs = -1
		_setStats("")
