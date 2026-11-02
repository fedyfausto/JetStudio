extends ConfirmationDialog

signal applied

const Style := preload("res://scripts/theme.gd")
const Hardware := preload("res://scripts/hardware.gd")
const Actions := preload("res://scripts/input_map.gd")
const Icons := preload("res://scripts/icons.gd")

# project.gd
var project
var _tabs: TabContainer
var _name: LineEdit
var _main: OptionButton
var _bootSplash: CheckBox
var _board: OptionButton
var _boardNotes: Label
var _display: OptionButton
var _displayNotes: Label
var _orientation: OptionButton
var _resolution: Label
var _spi: SpinBox
# display pin -> OptionButton
var _pins := {}
# custom display field -> control
var _custom := {}
var _customRows := []
# Input Map being edited: {name, keys, pin}
var _actions := []
# OptionButton per action
var _pinOptions := []
var _inputGrid: GridContainer
var _newAction: LineEdit
# "press a key" for an action's keys
var _capture: Window
var _captureIndex := -1
# {name, path} being edited
var _autoloads := []
var _autoloadGrid: GridContainer
var _autoloadScript: OptionButton
var _autoloadName: LineEdit
var _problems: Label
# hardware settings being edited
var _hw := {}
var _boardIds := []
var _displayIds := []
var _filling := false


func _ready():
	title = "Project Settings"
	cancel_button_text = "Cancel"
	var box := VBoxContainer.new()
	add_child(box)
	var tabs := TabContainer.new()
	tabs.custom_minimum_size = Vector2(600, 500)
	box.add_child(tabs)
	_tabs = tabs
	var general := _page(tabs, "General")
	_name = LineEdit.new()
	_row(general, "Name:", _name)
	_main = OptionButton.new()
	_row(general, "Main Scene:", _main)
	_bootSplash = CheckBox.new()
	_bootSplash.text = "Show \"Made with Jet Studio\" at startup"
	_bootSplash.tooltip_text = "A short splash before the main scene, in exported games and on the board (not in Play)."
	_row(general, "Boot Splash:", _bootSplash)

	var hardware := _page(tabs, "Hardware")
	_board = _option(hardware, "Board:", func(i):
		_hw.board = _boardIds[i]
		_hw.pins = Hardware.boards[_hw.board].get("display_pins", {}).duplicate()
		_refresh())
	_boardNotes = _note(hardware)
	_display = _option(hardware, "Display:", func(i):
		_hw.display = _displayIds[i]
		_hw.spi_hz = int(Hardware.displayOf(_hw).get("spi_hz", 40000000))
		_refresh())
	_displayNotes = _note(hardware)
	_orientation = _option(hardware, "Orientation:", func(i):
		_hw.orientation = Hardware.ORIENTATIONS[i]
		_refresh())
	for label in Hardware.ORIENTATION_NAMES: _orientation.add_item(label)

	_resolution = Label.new()
	_resolution.add_theme_color_override("font_color", Style.ACCENT)
	_row(hardware, "Resolution:", _resolution)
	_spi = SpinBox.new()
	_spi.min_value = 1
	_spi.max_value = 80
	_spi.suffix = "MHz"
	_spi.tooltip_text = "Display SPI clock: higher = more frames per second, if the display and wiring can handle it."
	_spi.value_changed.connect(func(v): if not _filling: _hw.spi_hz = int(v * 1000000); _check())
	_row(hardware, "SPI clock:", _spi)
	#custom display fields, only shown when "Custom" is selected.
	var controller := _option(hardware, "Controller:", func(i): _setCustom("controller", Hardware.CONTROLLERS[i]))

	for name in Hardware.CONTROLLERS: controller.add_item(name)

	_custom.controller = controller
	_customRows.append(_lastRow(hardware))

	for field in [["panel_w", "Panel width:"], ["panel_h", "Panel height:"], ["offset_x", "Offset X:"], ["offset_y", "Offset Y:"]]:
		var spin := SpinBox.new()
		spin.min_value = 0
		spin.max_value = 800
		spin.suffix = "px"
		var key: String = field[0]
		spin.value_changed.connect(func(_v): _customChanged())
		_row(hardware, field[1], spin)
		_custom[key] = spin
		_customRows.append(_lastRow(hardware))

	for field in [["invert", "Inverted colors"], ["rgb_order", "BGR order"]]:
		var check := CheckBox.new()
		check.text = field[1]
		check.toggled.connect(func(_on): _customChanged())
		_row(hardware, "", check)
		_custom[field[0]] = check
		_customRows.append(_lastRow(hardware))

	var pinsTitle := Label.new()
	pinsTitle.text = "DISPLAY PINS"
	pinsTitle.add_theme_font_size_override("font_size", 11)
	pinsTitle.add_theme_color_override("font_color", Style.MUTED)
	hardware.add_child(pinsTitle)
	hardware.add_child(Control.new())

	for key in Hardware.DISPLAY_PINS:
		var pinKey: String = key
		_pins[key] = _option(hardware, Hardware.PIN_NAMES[key] + ":", func(i): _pinChosen(pinKey, i))

	var defaults := Button.new()
	defaults.text = "Board default pins"
	defaults.pressed.connect(func():
		_hw.pins = Hardware.boards[_hw.board].get("display_pins", {}).duplicate()
		_refresh())
	_row(hardware, "", defaults)

	_inputTab(tabs)
	_autoloadTab(tabs)

	_problems = Label.new()
	_problems.add_theme_color_override("font_color", Style.ERROR)
	_problems.add_theme_font_size_override("font_size", 12)
	_problems.autowrap_mode = TextServer.AUTOWRAP_WORD_SMART
	_problems.custom_minimum_size.x = 600
	box.add_child(_problems)
	confirmed.connect(_apply)


func _page(tabs: TabContainer, titleText: String) -> GridContainer:
	var scroll := ScrollContainer.new()
	scroll.name = titleText
	scroll.horizontal_scroll_mode = ScrollContainer.SCROLL_MODE_DISABLED
	tabs.add_child(scroll)
	var grid := GridContainer.new()
	grid.columns = 2
	grid.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	grid.add_theme_constant_override("h_separation", 12)
	grid.add_theme_constant_override("v_separation", 8)
	scroll.add_child(grid)
	return grid


func _row(grid: GridContainer, text: String, control: Control):
	var label := Label.new()
	label.text = text
	grid.add_child(label)
	control.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	grid.add_child(control)


# Label and control of the last row added to `grid`.
func _lastRow(grid: GridContainer) -> Array:
	return [grid.get_child(grid.get_child_count() - 2), grid.get_child(grid.get_child_count() - 1)]


func _option(grid: GridContainer, text: String, onSelect: Callable) -> OptionButton:
	var option := OptionButton.new()
	option.fit_to_longest_item = false
	option.item_selected.connect(func(i): if not _filling: onSelect.call(i))
	_row(grid, text, option)
	return option


func _note(grid: GridContainer) -> Label:
	grid.add_child(Control.new())
	var note := Label.new()
	note.add_theme_color_override("font_color", Style.MUTED)
	note.add_theme_font_size_override("font_size", 12)
	note.autowrap_mode = TextServer.AUTOWRAP_WORD_SMART
	note.custom_minimum_size.x = 380
	grid.add_child(note)
	return note


# `tab`: 0 general, 1 hardware, 2 Input Map.
func open(target, tab: int = 0):
	project = target
	_tabs.current_tab = tab
	_hw = Hardware.settings(project.config)
	_name.text = project.projectName()
	_bootSplash.button_pressed = bool(project.config.get("boot_splash", true))
	_main.clear()
	var scenes := []
	var stack: Array = [project.scan()]

	while not stack.is_empty():
		var entry: Dictionary = stack.pop_back()

		for child in entry.get("children", []):
			if child.dir: stack.append(child)
			elif str(child.path).get_extension() == "jetscene": scenes.append(child.path)

	scenes.sort()

	for path in scenes:
		_main.add_item(path)

		if path == project.main_scene(): _main.select(_main.item_count - 1)

	_boardIds = Hardware.boards.keys()
	_displayIds = Hardware.displays.keys()
	_actions = Actions.of(project.config).duplicate(true)
	_buildInputRows()
	_autoloads = project.config.get("autoload", []).duplicate(true)
	_fillAutoloadScripts()
	_buildAutoloadRows()
	_refresh()
	_check()
	popup_centered()


# Refills the hardware controls from _hw after a board, display or orientation change.
func _refresh():
	_filling = true
	_board.clear()

	for id in _boardIds:
		var board: Dictionary = Hardware.boards[id]
		_board.add_item(str(board.name) + ("" if board.get("verified", false) else "  (unverified)"))

	_board.select(_boardIds.find(_hw.board))
	var boardProfile: Dictionary = Hardware.boards.get(_hw.board, {})

	if Hardware.platform(_hw) == "stm32":
		_boardNotes.text = "%s · %s · RAM %s · flash %s. %s" % [str(boardProfile.get("chip", "")).to_upper(), boardProfile.get("cpu", ""),
			boardProfile.get("ram", "?"), boardProfile.get("flash", "?"), boardProfile.get("notes", "")]
	else:
		_boardNotes.text = "%s · flash %s · PSRAM %s. %s" % [str(boardProfile.get("chip", "")).to_upper().replace("ESP32S3", "ESP32-S3"),
			boardProfile.get("flash", "?"), boardProfile.get("psram", "?"), boardProfile.get("notes", "")]

	_display.clear()

	for id in _displayIds:
		var display: Dictionary = Hardware.displays[id]
		_display.add_item(str(display.name) + ("" if display.get("verified", false) or id == "custom" else "  (unverified)"))

	_display.select(_displayIds.find(_hw.display))
	var display := Hardware.displayOf(_hw)
	_displayNotes.text = "%s · recommended SPI %d MHz. %s" % [display.controller, int(display.get("spi_hz", 0)) / 1000000, display.get("notes", "")]
	_orientation.select(Hardware.ORIENTATIONS.find(_hw.orientation))
	_spi.value = int(_hw.spi_hz) / 1000000.0
	var custom: bool = _hw.display == "custom"

	for row in _customRows:
		for control in row: control.visible = custom

	_custom.controller.select(Hardware.CONTROLLERS.find(str(display.controller)))
	_custom.panel_w.value = int(display.panel[0])
	_custom.panel_h.value = int(display.panel[1])
	_custom.offset_x.value = int(display.get("offset", [0, 0])[0])
	_custom.offset_y.value = int(display.get("offset", [0, 0])[1])
	_custom.invert.button_pressed = bool(display.get("invert", false))
	_custom.rgb_order.button_pressed = bool(display.get("rgb_order", false))

	for key in Hardware.DISPLAY_PINS:
		var option: OptionButton = _pins[key]
		option.clear()
		option.add_item("—  not connected" if key in Hardware.OPTIONAL_PINS else "—  choose")
		#STM32: where the SPI peripheral drives SCK/MOSI
		var fast := Hardware.spiPins(_hw, key)

		for entry in Hardware.boardPins(_hw):
			var label := Hardware.pin_label(_hw, int(entry[1]))

			if int(entry[1]) in fast: label += "  · " + str(Hardware.boards[_hw.board].spi.port)

			option.add_item(label)

		_selectGpio(option, int(_hw.pins.get(key, -1)))

	_refreshButtons()
	_filling = false
	_check()


# Actions can use any board pin the display is not using.
func _refreshButtons():
	var display_pins := {}

	for key in Hardware.DISPLAY_PINS: display_pins[int(_hw.pins.get(key, -1))] = true

	var wasFilling := _filling
	_filling = true

	for i in _pinOptions.size():
		var option: OptionButton = _pinOptions[i]
		option.clear()
		option.add_item("—  none")
		option.set_item_metadata(0, -1)

		for entry in Hardware.boardPins(_hw):
			if display_pins.has(int(entry[1])): continue

			option.add_item(Hardware.pin_label(_hw, int(entry[1])))
			option.set_item_metadata(option.item_count - 1, int(entry[1]))

		_selectGpio(option, int(_actions[i].pin))

	_filling = wasFilling


func _selectGpio(option: OptionButton, gpio: int):
	if option.item_count > 0 and option.get_item_metadata(0) == null:
		#index 0 is "none", then the board pins in order.
		var pins := Hardware.boardPins(_hw)
		var index := 0

		for i in pins.size():
			if int(pins[i][1]) == gpio: index = i + 1

		option.select(index)
		return

	for i in option.item_count:
		if int(option.get_item_metadata(i)) == gpio:
			option.select(i)
			return

	option.select(0)


func _gpioOf(option: OptionButton) -> int:
	if option.selected < 0 or option.item_count == 0: return -1

	var meta = option.get_item_metadata(option.selected)
	return int(meta) if meta != null else -1


func _pinChosen(key: String, index: int):
	var pins := Hardware.boardPins(_hw)
	_hw.pins[key] = int(pins[index - 1][1]) if index > 0 else -1
	_refreshButtons()
	_check()


func _setCustom(key: String, value: Variant):
	var custom: Dictionary = _hw.get("custom_display", {})
	custom[key] = value
	_hw.custom_display = custom
	_check()


func _customChanged():
	if _filling: return

	var custom: Dictionary = _hw.get("custom_display", {})
	custom.panel = [int(_custom.panel_w.value), int(_custom.panel_h.value)]
	custom.memory = custom.panel.duplicate()
	custom.offset = [int(_custom.offset_x.value), int(_custom.offset_y.value)]
	custom.invert = _custom.invert.button_pressed
	custom.rgb_order = _custom.rgb_order.button_pressed
	_hw.custom_display = custom
	_check()


func _buttonPins() -> Dictionary:
	return Actions.pins(_actions)


# Shows the resolution and the problems; OK stays disabled while there are any.
func _check():
	var size := Hardware.resolution(_hw)
	_resolution.text = "%d × %d px (the game uses the display's resolution)" % [size.x, size.y]

	if Hardware.softwareSpi(_hw):
		_resolution.text += "\nSCK/MOSI are not on %s: the display runs on software SPI." % Hardware.boards[_hw.board].spi.port

	var problems := Hardware.problems(_hw, _buttonPins())
	var buttons := _buttonPins().values()

	for gpio in buttons:
		if buttons.count(gpio) > 1:
			problems.append("Two buttons use the same pin.")
			break

	var names := {}

	for action in _actions:
		if not Actions.validName(action.name): problems.append("Invalid action name: \"%s\" (letters, digits and _)." % action.name)
		elif names.has(action.name): problems.append("Two actions are named \"%s\"." % action.name)
		names[action.name] = true

	var globals := {}

	for entry in _autoloads:
		var global := str(entry.name)

		if not Actions.validName(global) or global[0] in "0123456789" or global == "Input":
			problems.append("Invalid autoload name: \"%s\"." % global)
		elif globals.has(global): problems.append("Two autoloads are named \"%s\"." % global)
		elif not project.exists(str(entry.path)): problems.append("Autoload script not found: " + str(entry.path))
		globals[global] = true

	_problems.text = "\n".join(problems)
	get_ok_button().disabled = not problems.is_empty()


func _apply():
	if not _name.text.strip_edges().is_empty(): project.config.name = _name.text.strip_edges()

	if _main.selected >= 0: project.config.main_scene = _main.get_item_text(_main.selected)

	project.config.boot_splash = _bootSplash.button_pressed
	var hardware := {"board": _hw.board, "display": _hw.display, "orientation": _hw.orientation, "spi_hz": int(_hw.spi_hz),
		"pins": _hw.pins.duplicate()}

	if _hw.display == "custom": hardware.custom_display = _hw.get("custom_display", {})

	project.config.hardware = hardware
	var input_map := []

	for action in _actions: input_map.append({"name": action.name, "keys": action.keys.duplicate(), "pin": int(action.pin)})

	project.config.input_map = input_map
	#old format, now part of input_map
	project.config.erase("input_pins")
	project.config.autoload = _autoloads.duplicate(true)
	var error: String = project.save()

	if not error.is_empty(): push_error(error)

	applied.emit()

func _inputTab(tabs: TabContainer):
	var scroll := ScrollContainer.new()
	scroll.name = "Input Map"
	scroll.horizontal_scroll_mode = ScrollContainer.SCROLL_MODE_DISABLED
	tabs.add_child(scroll)
	var box := VBoxContainer.new()
	box.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	box.add_theme_constant_override("separation", 8)
	scroll.add_child(box)
	var hint := Label.new()
	hint.text = "Actions are read by scripts: Input.is_action_pressed(\"jump\"). Keys work on PC (Game view, player,\nemulator); the pin is the button on the board, wired between the pin and GND (internal pull-up)."
	hint.add_theme_color_override("font_color", Style.MUTED)
	hint.add_theme_font_size_override("font_size", 12)
	box.add_child(hint)
	var addRow := HBoxContainer.new()
	_newAction = LineEdit.new()
	_newAction.placeholder_text = "New action, e.g. jump"
	_newAction.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	_newAction.text_submitted.connect(func(_text): _addAction())
	addRow.add_child(_newAction)
	var add := Button.new()
	add.text = "Add"
	add.icon = Icons.get_icon("Add")
	add.pressed.connect(_addAction)
	addRow.add_child(add)
	box.add_child(addRow)
	_inputGrid = GridContainer.new()
	_inputGrid.columns = 4
	_inputGrid.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	_inputGrid.add_theme_constant_override("h_separation", 10)
	_inputGrid.add_theme_constant_override("v_separation", 6)
	box.add_child(_inputGrid)
	_capture = _captureWindow()


func _buildInputRows():
	for child in _inputGrid.get_children():
		_inputGrid.remove_child(child)
		child.queue_free()

	_pinOptions.clear()

	for header in ["Action", "Keys (PC)", "Pin (board)", ""]:
		var label := Label.new()
		label.text = header
		label.add_theme_color_override("font_color", Style.MUTED)
		label.add_theme_font_size_override("font_size", 11)
		_inputGrid.add_child(label)

	for i in _actions.size():
		var index := i
		var action: Dictionary = _actions[i]
		var builtin := i < Actions.DEFAULTS.size()

		if builtin:
			var label := Label.new()
			label.text = action.name
			label.tooltip_text = "Built-in Godot action"
			_inputGrid.add_child(label)
		else:
			var edit := LineEdit.new()
			edit.text = action.name
			edit.custom_minimum_size.x = 150
			edit.text_changed.connect(func(text): _actions[index].name = text.strip_edges(); _check())
			_inputGrid.add_child(edit)

		var keys := Button.new()
		keys.text = ", ".join(action.keys) if not action.keys.is_empty() else "—  no keys"
		keys.tooltip_text = "Click to add a key (by pressing it) or clear the keys"
		keys.size_flags_horizontal = Control.SIZE_EXPAND_FILL
		keys.clip_text = true
		keys.pressed.connect(func(): _captureKeys(index))
		_inputGrid.add_child(keys)
		var pin := OptionButton.new()
		pin.fit_to_longest_item = false
		pin.custom_minimum_size.x = 150
		pin.item_selected.connect(func(_item):
			if _filling: return
			_actions[index].pin = _gpioOf(pin)
			_check())
		_inputGrid.add_child(pin)
		_pinOptions.append(pin)
		var remove := Button.new()
		remove.icon = Icons.get_icon("Remove")
		remove.flat = true
		remove.disabled = builtin
		remove.tooltip_text = "Built-in actions cannot be removed" if builtin else "Remove action"
		remove.pressed.connect(func():
			_actions.remove_at(index)
			_buildInputRows()
			_check())
		_inputGrid.add_child(remove)

	_refreshButtons()


func _addAction():
	var name := _newAction.text.strip_edges()

	if name.is_empty(): return

	var problem := ""

	if not Actions.validName(name): problem = "Invalid action name: use letters, digits and _ (e.g. jump, shoot_2)."
	elif _actions.size() >= Actions.MAX_ACTIONS: problem = "Maximum %d actions." % Actions.MAX_ACTIONS

	for action in _actions:
		if action.name == name: problem = "Action \"%s\" already exists." % name

	if not problem.is_empty():
		_problems.text = problem
		return

	_actions.append({"name": name, "keys": [], "pin": -1})
	_newAction.clear()
	_buildInputRows()
	_check()


# A plain Window rather than a dialog, so Enter and Escape can be captured as keys too.
func _captureWindow() -> Window:
	var window := Window.new()
	window.title = "Action Keys"
	window.visible = false
	window.exclusive = true
	window.transient = true
	window.unresizable = true
	window.close_requested.connect(window.hide)
	var panel := PanelContainer.new()
	panel.set_anchors_and_offsets_preset(Control.PRESET_FULL_RECT)
	window.add_child(panel)
	var box := VBoxContainer.new()
	box.add_theme_constant_override("separation", 12)
	box.alignment = BoxContainer.ALIGNMENT_CENTER
	panel.add_child(box)
	var label := Label.new()
	label.name = "Message"
	label.horizontal_alignment = HORIZONTAL_ALIGNMENT_CENTER
	box.add_child(label)
	var row := HBoxContainer.new()
	row.alignment = BoxContainer.ALIGNMENT_CENTER
	box.add_child(row)
	var clear := Button.new()
	clear.text = "Clear keys"
	clear.focus_mode = Control.FOCUS_NONE
	clear.pressed.connect(func():
		_actions[_captureIndex].keys = []
		window.hide()
		_buildInputRows())
	row.add_child(clear)
	var close := Button.new()
	close.text = "Close"
	close.focus_mode = Control.FOCUS_NONE
	close.pressed.connect(window.hide)
	row.add_child(close)
	window.window_input.connect(func(event):
		if not (event is InputEventKey and event.pressed and not event.echo): return
		var key := OS.get_keycode_string(event.keycode)
		if event.keycode == KEY_NONE or key.is_empty(): return
		var keys: Array = _actions[_captureIndex].keys
		if not key in keys and keys.size() < Actions.MAX_KEYS: keys.append(key)
		window.set_input_as_handled()
		window.hide()
		_buildInputRows())
	add_child(window)
	return window


func _captureKeys(index: int):
	_captureIndex = index
	var action: Dictionary = _actions[index]
	(_capture.find_child("Message", true, false) as Label).text = "Press the key to add to \"%s\"\n(current: %s)" % [action.name,
		", ".join(action.keys) if not action.keys.is_empty() else "none"]
	_capture.popup_centered(Vector2i(420, 150))

func _autoloadTab(tabs: TabContainer):
	var scroll := ScrollContainer.new()
	scroll.name = "Autoload"
	scroll.horizontal_scroll_mode = ScrollContainer.SCROLL_MODE_DISABLED
	tabs.add_child(scroll)
	var box := VBoxContainer.new()
	box.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	box.add_theme_constant_override("separation", 8)
	scroll.add_child(box)

	var hint := Label.new()
	hint.text = "Scripts that stay alive across scene changes, like Godot's autoloads.\nAny script reaches them by name (Game.score) or at /root/Game."
	hint.add_theme_color_override("font_color", Style.MUTED)
	hint.add_theme_font_size_override("font_size", 12)
	box.add_child(hint)

	var addRow := HBoxContainer.new()
	_autoloadScript = OptionButton.new()
	_autoloadScript.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	_autoloadScript.item_selected.connect(func(i):
		_autoloadName.text = str(_autoloadScript.get_item_text(i)).get_file().get_basename().to_pascal_case())
	addRow.add_child(_autoloadScript)
	_autoloadName = LineEdit.new()
	_autoloadName.placeholder_text = "Node name"
	_autoloadName.custom_minimum_size.x = 150
	addRow.add_child(_autoloadName)
	var add := Button.new()
	add.text = "Add"
	add.icon = Icons.get_icon("Add")
	add.pressed.connect(_addAutoload)
	addRow.add_child(add)
	box.add_child(addRow)

	_autoloadGrid = GridContainer.new()
	_autoloadGrid.columns = 3
	_autoloadGrid.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	_autoloadGrid.add_theme_constant_override("h_separation", 10)
	_autoloadGrid.add_theme_constant_override("v_separation", 6)
	box.add_child(_autoloadGrid)


func _fillAutoloadScripts():
	_autoloadScript.clear()
	var scripts := []
	var stack: Array = [project.scan()]

	while not stack.is_empty():
		var entry: Dictionary = stack.pop_back()

		for child in entry.get("children", []):
			if child.dir: stack.append(child)
			elif str(child.path).get_extension() == "jet": scripts.append(child.path)

	scripts.sort()

	for path in scripts: _autoloadScript.add_item(path)

	if not scripts.is_empty(): _autoloadName.text = str(scripts[0]).get_file().get_basename().to_pascal_case()


func _buildAutoloadRows():
	for child in _autoloadGrid.get_children():
		_autoloadGrid.remove_child(child)
		child.queue_free()

	for header in ["Name", "Script", ""]:
		var label := Label.new()
		label.text = header
		label.add_theme_color_override("font_color", Style.MUTED)
		label.add_theme_font_size_override("font_size", 11)
		_autoloadGrid.add_child(label)

	for i in _autoloads.size():
		var index := i
		var nameEdit := LineEdit.new()
		nameEdit.text = str(_autoloads[i].name)
		nameEdit.custom_minimum_size.x = 150
		nameEdit.text_changed.connect(func(text): _autoloads[index].name = text.strip_edges(); _check())
		_autoloadGrid.add_child(nameEdit)
		var path := Label.new()
		path.text = str(_autoloads[i].path)
		path.size_flags_horizontal = Control.SIZE_EXPAND_FILL
		_autoloadGrid.add_child(path)
		var remove := Button.new()
		remove.icon = Icons.get_icon("Remove")
		remove.flat = true
		remove.tooltip_text = "Remove the autoload"
		remove.pressed.connect(func():
			_autoloads.remove_at(index)
			_buildAutoloadRows()
			_check())
		_autoloadGrid.add_child(remove)


func _addAutoload():
	if _autoloadScript.selected < 0: return

	var autoloadName := _autoloadName.text.strip_edges()

	if autoloadName.is_empty(): return

	_autoloads.append({"name": autoloadName, "path": _autoloadScript.get_item_text(_autoloadScript.selected)})
	_buildAutoloadRows()
	_check()

