extends VBoxContainer

signal script_link(path: String, line: int)
signal connection_changed(connected: bool)
signal preferences_changed(port: String, baud: int)
## Firmware frame stats summary, "" when unknown.
signal stats_changed(text: String)

const Style := preload("res://scripts/theme.gd")
const Icons := preload("res://scripts/icons.gd")
const BAUDS := [9600, 57600, 115200, 230400, 460800, 921600]
const MAX_LINES := 4000

var serial: Object # JetSerial (GDExtension)
## Set by the editor (addr2line on the Debug ELF): Callable(addresses) -> Array of [function, location].
var decode: Callable
var _port_select: OptionButton
var _baud_select: OptionButton
var _connect_button: Button
var _reset_button: Button
var _auto_check: CheckBox
var _time_check: CheckBox
var _stats_text := ""
var _output: RichTextLabel
var _ports := [] # [{path, name}]
var _preferred_port := ""
var _wanted := false # the user asked for a connection (kept across resets and flashes)
var _suspended := false
var _retry := 0.0
var _force_until := 0 # ms: keep retrying after a flash even without auto-reconnect
var _pending := PackedByteArray()
var _pending_age := 0.0
var _last_stats_ms := -1
var _ansi := RegEx.create_from_string("\u001b\\[([0-9;]*)m")
var _esp_log := RegEx.create_from_string("^([EWIDV]) \\((\\d+)\\) ([^:]+): (.*)$")
var _frame_stats := RegEx.create_from_string("^Frame ([\\d.]+) ms \\(scripts ([\\d.]+) ms\\); (\\d+) raster tris; free heap (\\d+), PSRAM (\\d+)")
var _script_ref := RegEx.create_from_string("(res://[^\\s:]+\\.jet):(\\d+)")
var _backtrace_pc := RegEx.create_from_string("0x([0-9a-fA-F]{8}):0x[0-9a-fA-F]{8}")

func _ready() -> void:
	add_theme_constant_override("separation", 4)
	var bar := HBoxContainer.new()
	bar.add_theme_constant_override("separation", 6)
	add_child(bar)
	_port_select = OptionButton.new()
	_port_select.custom_minimum_size.x = 180
	_port_select.fit_to_longest_item = false
	_port_select.clip_text = true
	_port_select.focus_mode = Control.FOCUS_NONE
	_port_select.tooltip_text = "The board's USB port (Nano ESP32: \"Espressif USB JTAG serial debug unit\"; Nucleo: \"STMicroelectronics STLINK\")"
	_port_select.item_selected.connect(func(_i): _remember())
	bar.add_child(_port_select)
	var refresh := Button.new()
	refresh.theme_type_variation = "FlatButton"
	refresh.icon = Icons.get_icon("Reload")
	refresh.tooltip_text = "Refresh the port list"
	refresh.focus_mode = Control.FOCUS_NONE
	refresh.pressed.connect(refresh_ports)
	bar.add_child(refresh)
	_baud_select = OptionButton.new()
	for baud in BAUDS: _baud_select.add_item("%d baud" % baud)
	_baud_select.select(BAUDS.find(115200))
	_baud_select.focus_mode = Control.FOCUS_NONE
	_baud_select.tooltip_text = "Baud rate: 115200 for the firmware (the ESP32-S3's native USB ignores it)"
	_baud_select.item_selected.connect(func(_i): _remember())
	bar.add_child(_baud_select)
	_connect_button = Button.new()
	_connect_button.focus_mode = Control.FOCUS_NONE
	_connect_button.pressed.connect(func(): disconnect_port() if _wanted else connect_port())
	bar.add_child(_connect_button)
	_reset_button = Button.new()
	_reset_button.text = "Reset"
	_reset_button.icon = Icons.get_icon("Reload")
	_reset_button.tooltip_text = "Reset the ESP32 over USB (as after flashing) to see the log from boot (STM32 boards are reset with their RESET button)"
	_reset_button.focus_mode = Control.FOCUS_NONE
	_reset_button.pressed.connect(reset_board)
	bar.add_child(_reset_button)
	_auto_check = CheckBox.new()
	_auto_check.text = "Reconnect"
	_auto_check.button_pressed = true
	_auto_check.tooltip_text = "Reconnect automatically after a reset or when the cable is plugged back in"
	_auto_check.focus_mode = Control.FOCUS_NONE
	bar.add_child(_auto_check)
	_time_check = CheckBox.new()
	_time_check.text = "Time"
	_time_check.tooltip_text = "Prefix each line with the PC's time"
	_time_check.focus_mode = Control.FOCUS_NONE
	bar.add_child(_time_check)
	_output = RichTextLabel.new()
	_output.scroll_following = true
	_output.selection_enabled = true
	_output.context_menu_enabled = true
	_output.size_flags_vertical = Control.SIZE_EXPAND_FILL
	_output.add_theme_font_override("normal_font", Style.code_font)
	_output.add_theme_font_size_override("normal_font_size", 13)
	_output.meta_clicked.connect(func(meta):
		var text := str(meta)
		var colon := text.rfind(":")
		if colon > 6: script_link.emit(text.substr(0, colon), int(text.substr(colon + 1))))
	add_child(_output)
	_update_ui()
	_system("Serial monitor: connect the board via USB (STM32 Nucleo: the ST-LINK USB) and press Connect. The log comes from the Debug firmware.")

func set_preferences(port: String, baud: int) -> void:
	_preferred_port = port
	if BAUDS.has(baud): _baud_select.select(BAUDS.find(baud))
	refresh_ports()

func is_connected_to_board() -> bool:
	return serial != null and serial.is_open()

func baud() -> int:
	return BAUDS[maxi(_baud_select.selected, 0)]

func refresh_ports() -> void:
	if serial == null: return
	var current := selected_port()
	if current.is_empty(): current = _preferred_port
	_ports = serial.list_ports()
	_port_select.clear()
	var best := -1
	for i in _ports.size():
		var port: Dictionary = _ports[i]
		_port_select.add_item(str(port.name))
		_port_select.set_item_tooltip(i, str(port.path))
		if str(port.path) == current: best = i
		elif best < 0 and RegEx.create_from_string("(?i)espressif|arduino|esp32|jtag|stlink|stmicro").search(str(port.name)): best = i
	if _ports.is_empty(): _port_select.add_item("No ports found")
	_port_select.select(maxi(best, 0))
	_update_ui()

func selected_port() -> String:
	var i := _port_select.selected
	return str(_ports[i].path) if i >= 0 and i < _ports.size() else ""

func connect_port(quiet: bool = false) -> bool:
	if serial == null: return false
	var path := selected_port()
	if path.is_empty():
		refresh_ports()
		path = selected_port()
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
	_update_ui()
	connection_changed.emit(true)
	return true

func disconnect_port() -> void:
	_wanted = false
	_force_until = 0
	if serial != null and serial.is_open():
		serial.close()
		_system("Disconnected.")
	_update_ui()
	connection_changed.emit(false)

func reset_board() -> void:
	if not is_connected_to_board(): return
	_system("Resetting the board…")
	if not serial.reset_board(): _system("This port does not support resetting the board.", Style.WARNING)
	_force_until = Time.get_ticks_msec() + 8000

## Frees the port for esptool. Returns whether it was connected.
func suspend() -> bool:
	var was_open := is_connected_to_board()
	_suspended = true
	if was_open:
		serial.close()
		_system("Port released for flashing…")
	_update_ui()
	return was_open

## After a flash: reconnect (the board re-enumerates for a few seconds).
func resume(open: bool) -> void:
	_suspended = false
	if open:
		_wanted = true
		_force_until = Time.get_ticks_msec() + 12000
		_retry = 0.5
	_update_ui()

func clear() -> void:
	_output.clear()

func _process(delta: float) -> void:
	if serial == null: return
	if serial.is_open():
		var data: PackedByteArray = serial.read()
		if not data.is_empty(): feed(data)
		elif not _pending.is_empty():
			# A line without newline (prompt, partial write): show it after a short pause.
			_pending_age += delta
			if _pending_age > 0.3: _flush_pending()
		if not serial.is_open(): _lost()
	elif _wanted and not _suspended and (_auto_check.button_pressed or Time.get_ticks_msec() < _force_until):
		_retry -= delta
		if _retry <= 0.0:
			_retry = 1.0
			refresh_ports()
			connect_port(true)

func _lost() -> void:
	_flush_pending()
	var automatic := _auto_check.button_pressed or Time.get_ticks_msec() < _force_until
	_system("Board disconnected (reset or cable)%s" % ("; reconnecting automatically…" if automatic else "."), Style.WARNING)
	_retry = 0.5
	_update_ui()
	connection_changed.emit(false)

## Raw bytes from the board (also used by the tests).
func feed(data: PackedByteArray) -> void:
	_pending.append_array(data)
	_pending_age = 0.0
	var start := 0
	for i in _pending.size():
		if _pending[i] == 10:
			_add_line(_pending.slice(start, i).get_string_from_utf8().trim_suffix("\r"))
			start = i + 1
	_pending = _pending.slice(start)
	if _pending.size() > 4096: _flush_pending()

func _flush_pending() -> void:
	if _pending.is_empty(): return
	_add_line(_pending.get_string_from_utf8().trim_suffix("\r"))
	_pending.clear()
	_pending_age = 0.0

func _add_line(raw: String) -> void:
	var color := Style.TEXT
	var found := _ansi.search(raw)
	if found:
		var codes := found.get_string(1).split(";")
		if "31" in codes: color = Style.ERROR
		elif "33" in codes: color = Style.WARNING
		elif "32" in codes: color = Style.TEXT
	var text := _ansi.sub(raw, "", true)
	var entry := _esp_log.search(text)
	if entry:
		match entry.get_string(1):
			"E": color = Style.ERROR
			"W": color = Style.WARNING
			"D", "V": color = Style.MUTED
		var stats := _frame_stats.search(entry.get_string(4))
		if stats:
			_show_stats(int(entry.get_string(2)), stats)
			color = Style.MUTED
	_write(text, color)
	if text.strip_edges().begins_with("Backtrace:"): _decode_backtrace(text)

## Turns "Backtrace: 0xPC:0xSP ..." lines into source locations, like PlatformIO's esp32_exception_decoder.
func _decode_backtrace(text: String) -> void:
	var addresses := PackedStringArray()
	for found in _backtrace_pc.search_all(text): addresses.append("0x" + found.get_string(1))
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

func _show_stats(time_ms: int, stats: RegExMatch) -> void:
	var fps := ""
	if _last_stats_ms >= 0 and time_ms > _last_stats_ms: fps = "%.1f FPS · " % (60000.0 / (time_ms - _last_stats_ms))
	_last_stats_ms = time_ms
	_set_stats("%s%.1f ms (script %.1f) · %s tri · heap %s · PSRAM %s" % [fps, float(stats.get_string(1)), float(stats.get_string(2)),
		stats.get_string(3), String.humanize_size(int(stats.get_string(4))), String.humanize_size(int(stats.get_string(5)))])

func stats_text() -> String:
	return _stats_text

func _set_stats(text: String) -> void:
	if text == _stats_text: return
	_stats_text = text
	stats_changed.emit(text)

func _write(text: String, color: Color) -> void:
	if _output.get_paragraph_count() > MAX_LINES: _output.remove_paragraph(0)
	_output.push_color(color)
	if _time_check.button_pressed:
		var now := Time.get_time_dict_from_system()
		_output.add_text("[%02d:%02d:%02d] " % [now.hour, now.minute, now.second])
	# Script errors from the firmware ("res://x.jet:12: ...") open the script.
	var link := _script_ref.search(text)
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

func _system(text: String, color: Color = Style.MUTED) -> void:
	_write("— " + text, color)

func _remember() -> void:
	var port := selected_port()
	if not port.is_empty(): preferences_changed.emit(port, baud())

func _update_ui() -> void:
	if _connect_button == null: return
	var open := is_connected_to_board()
	var waiting := _wanted and not open
	_connect_button.text = "Disconnect" if open or waiting else "Connect"
	_connect_button.icon = Icons.get_icon("Stop" if open or waiting else "Play")
	_connect_button.tooltip_text = "Waiting for the board…" if waiting else ""
	_reset_button.disabled = not open
	_port_select.disabled = open
	_baud_select.disabled = open
	if not open and not waiting:
		_last_stats_ms = -1
		_set_stats("")
