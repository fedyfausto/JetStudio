extends HSplitContainer

signal saved(path: String)
signal script_info_changed(path: String, info: Dictionary)
signal new_script_requested
signal message(text: String, isError: bool)

const Style := preload("res://scripts/theme.gd")
const Icons := preload("res://scripts/icons.gd")
const Highlighter := preload("res://scripts/script_highlighter.gd")

const KEYWORDS := ["if", "elif", "else", "for", "while", "break", "continue", "pass", "return", "match", "and", "or", "not",
	"in", "is", "as", "var", "const", "func", "extends", "enum", "signal", "class", "class_name", "static", "await",
	"yield", "preload", "breakpoint", "super", "void", "self"]
const CONTROL_FLOW := ["if", "elif", "else", "for", "while", "break", "continue", "pass", "return", "match", "await", "yield"]
const BASE_TYPES := ["bool", "int", "float", "String", "Vector2", "Vector3", "Color", "Array", "Dictionary", "Callable", "Signal",
	"null", "true", "false", "PI", "TAU", "INF", "NAN", "OK", "CONNECT_ONE_SHOT"]
const ENGINE_TYPES := ["Node", "Node3D", "MeshInstance3D", "Camera3D", "DirectionalLight3D", "WorldEnvironment", "Input"]
const BUILTINS := ["print", "printerr", "str", "int", "float", "bool", "len", "range", "abs", "sign", "floor", "ceil", "round",
	"sqrt", "pow", "exp", "log", "sin", "cos", "tan", "asin", "acos", "atan", "atan2", "deg_to_rad", "rad_to_deg", "min", "max",
	"clamp", "clampf", "clampi", "lerp", "lerpf", "inverse_lerp", "move_toward", "wrapf", "wrapi", "fmod", "fposmod", "posmod",
	"snapped", "is_equal_approx", "is_zero_approx", "randomize", "seed", "randf", "randi", "randf_range", "randi_range",
	"is_instance_valid", "typeof"]
const MEMBERS := {
	"Vector3": ["x", "y", "z", "length", "length_squared", "normalized", "dot", "cross", "distance_to", "distance_squared_to",
		"lerp", "move_toward", "abs", "rotated", "angle_to", "is_zero_approx", "is_equal_approx",
		"ZERO", "ONE", "UP", "DOWN", "LEFT", "RIGHT", "FORWARD", "BACK"],
	"Color": ["r", "g", "b", "a", "lerp", "to_html", "BLACK", "WHITE", "RED", "GREEN", "BLUE", "YELLOW", "CYAN", "MAGENTA", "ORANGE", "GRAY"],
	"String": ["length", "to_upper", "to_lower", "begins_with", "ends_with", "contains", "find", "substr", "split", "strip_edges",
		"replace", "to_int", "to_float", "is_empty"],
	"Array": ["size", "is_empty", "append", "push_back", "push_front", "pop_back", "pop_front", "insert", "remove_at", "erase",
		"has", "find", "clear", "front", "back", "duplicate", "reverse", "sort", "pick_random"],
	"Dictionary": ["size", "is_empty", "has", "get", "keys", "values", "erase", "clear", "duplicate"],
	"Signal": ["emit", "connect", "disconnect", "is_connected", "get_name", "get_object"],
	"Callable": ["call", "callv", "get_method", "get_object", "is_valid"],
}
const TEMPLATE := "extends %s\n\n\n# Called when the node enters the scene tree for the first time.\nfunc _ready() -> void:\n\tpass # Replace with function body.\n\n\n# Called every frame. 'delta' is the elapsed time since the previous frame.\nfunc _process(delta: float) -> void:\n\tpass\n"

var bridge: Object
var project
# Jet Studio's docs folder
var docs := ""
# from the bridge
var class_db := []
# path -> {code, saved, info, error_line}
var _scripts := {}
var _order := []
var _current := ""
var _list: ItemList
var _methods: ItemList
var _filter: LineEdit
var _holder: VBoxContainer
var _pathLabel: Label
var _status: Button
var _caretLabel: Label
var _findBar: HBoxContainer
var _findEdit: LineEdit
var _findCase: CheckBox
var _gotoDialog: ConfirmationDialog
var _gotoEdit: SpinBox
var _checkTimer: Timer
var _empty: Label
var _reloadDialog: ConfirmationDialog
var _reloadPath := ""
# {path, line}
var _runtimeError := {}


func _ready():
	split_offset = 210
	var left := VBoxContainer.new()
	left.custom_minimum_size.x = 160
	add_child(left)
	_filter = LineEdit.new()
	_filter.placeholder_text = "Filter Scripts"
	_filter.clear_button_enabled = true
	_filter.right_icon = Icons.get_icon("Search")
	_filter.text_changed.connect(func(_t): _refreshList())
	left.add_child(_filter)
	_list = ItemList.new()
	_list.size_flags_vertical = Control.SIZE_EXPAND_FILL
	_list.size_flags_stretch_ratio = 1.4
	_list.item_selected.connect(func(i): showScript(str(_list.get_item_metadata(i))))
	_list.item_clicked.connect(func(i, _p, button):
		if button == MOUSE_BUTTON_MIDDLE: closeScript(str(_list.get_item_metadata(i))))
	left.add_child(_list)
	var methodsTitle := Label.new()
	methodsTitle.text = "Methods"
	methodsTitle.theme_type_variation = "DockTitle"
	left.add_child(methodsTitle)
	_methods = ItemList.new()
	_methods.size_flags_vertical = Control.SIZE_EXPAND_FILL
	_methods.item_selected.connect(func(i): goto_line(int(_methods.get_item_metadata(i))))
	left.add_child(_methods)
	var right := VBoxContainer.new()
	right.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	add_child(right)
	var bar := HBoxContainer.new()
	right.add_child(bar)
	var fileMenu := MenuButton.new()
	fileMenu.text = "File"
	var popup := fileMenu.get_popup()
	popup.add_icon_item(Icons.get_icon("ScriptCreate"), "New Script...", 0)
	popup.add_icon_item(Icons.get_icon("Save"), "Save  (Ctrl+S)", 1)
	popup.add_item("Save All  (Ctrl+Shift+Alt+S)", 2)
	popup.add_separator()
	popup.add_icon_item(Icons.get_icon("Close"), "Close  (Ctrl+W)", 3)
	popup.add_separator()
	popup.add_icon_item(Icons.get_icon("Help"), "JetScript Reference", 4)
	popup.id_pressed.connect(_onFileMenu)
	bar.add_child(fileMenu)
	var searchMenu := MenuButton.new()
	searchMenu.text = "Search"
	var search := searchMenu.get_popup()
	search.add_icon_item(Icons.get_icon("Search"), "Find...  (Ctrl+F)", 0)
	search.add_item("Find Next  (F3)", 1)
	search.add_item("Find Previous  (Shift+F3)", 2)
	search.add_separator()
	search.add_item("Go to Line...  (Ctrl+L)", 3)
	search.add_item("Toggle Comment  (Ctrl+K)", 4)
	search.id_pressed.connect(_onSearchMenu)
	bar.add_child(searchMenu)
	_pathLabel = Label.new()
	_pathLabel.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	_pathLabel.horizontal_alignment = HORIZONTAL_ALIGNMENT_RIGHT
	_pathLabel.add_theme_color_override("font_color", Style.MUTED)
	_pathLabel.clip_text = true
	bar.add_child(_pathLabel)
	_holder = VBoxContainer.new()
	_holder.size_flags_vertical = Control.SIZE_EXPAND_FILL
	right.add_child(_holder)
	_empty = Label.new()
	_empty.text = "No script open.\n\nOpen a .jet file from the FileSystem dock (double-click),\nclick a node's script icon, or create a new script\nfrom the File menu or with the 'Attach Script' button in the Scene dock."
	_empty.horizontal_alignment = HORIZONTAL_ALIGNMENT_CENTER
	_empty.vertical_alignment = VERTICAL_ALIGNMENT_CENTER
	_empty.size_flags_vertical = Control.SIZE_EXPAND_FILL
	_empty.add_theme_color_override("font_color", Style.MUTED)
	_holder.add_child(_empty)
	_findBar = HBoxContainer.new()
	_findBar.visible = false
	right.add_child(_findBar)
	_findEdit = LineEdit.new()
	_findEdit.placeholder_text = "Find"
	_findEdit.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	_findEdit.text_submitted.connect(func(_t): findNext(true))
	_findEdit.text_changed.connect(func(_t): _highlightSearch())
	_findBar.add_child(_findEdit)
	var previous := Button.new()
	previous.theme_type_variation = "FlatButton"
	previous.icon = Icons.get_icon("MoveUp")
	previous.pressed.connect(func(): findNext(false))
	_findBar.add_child(previous)
	var next := Button.new()
	next.theme_type_variation = "FlatButton"
	next.icon = Icons.get_icon("MoveDown")
	next.pressed.connect(func(): findNext(true))
	_findBar.add_child(next)
	_findCase = CheckBox.new()
	_findCase.text = "Match Case"
	_findCase.toggled.connect(func(_on): _highlightSearch())
	_findBar.add_child(_findCase)
	var closeFind := Button.new()
	closeFind.theme_type_variation = "FlatButton"
	closeFind.icon = Icons.get_icon("Close")
	closeFind.pressed.connect(_hideFind)
	_findBar.add_child(closeFind)
	var statusBar := HBoxContainer.new()
	right.add_child(statusBar)
	_status = Button.new()
	_status.theme_type_variation = "FlatButton"
	_status.alignment = HORIZONTAL_ALIGNMENT_LEFT
	_status.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	_status.clip_text = true
	_status.pressed.connect(_gotoStatus)
	statusBar.add_child(_status)
	_caretLabel = Label.new()
	_caretLabel.add_theme_color_override("font_color", Style.MUTED)
	_caretLabel.add_theme_font_size_override("font_size", 12)
	statusBar.add_child(_caretLabel)
	_checkTimer = Timer.new()
	_checkTimer.one_shot = true
	_checkTimer.wait_time = 0.35
	_checkTimer.timeout.connect(func(): check(_current))
	add_child(_checkTimer)
	_gotoDialog = ConfirmationDialog.new()
	_gotoDialog.cancel_button_text = "Cancel"
	_gotoDialog.title = "Go to Line"
	_gotoEdit = SpinBox.new()
	_gotoEdit.min_value = 1
	_gotoEdit.max_value = 100000
	_gotoDialog.add_child(_gotoEdit)
	_gotoDialog.register_text_enter(_gotoEdit.get_line_edit())
	_gotoDialog.confirmed.connect(func(): goto_line(int(_gotoEdit.value)))
	add_child(_gotoDialog)
	_reloadDialog = ConfirmationDialog.new()
	_reloadDialog.cancel_button_text = "Cancel"
	_reloadDialog.title = "File Modified"
	_reloadDialog.ok_button_text = "Reload from Disk"
	_reloadDialog.cancel_button_text = "Keep My Version"
	_reloadDialog.confirmed.connect(func(): _reload(_reloadPath))
	add_child(_reloadDialog)
	_updateStatus()


func _highlighter() -> SyntaxHighlighter:
	return Highlighter.new(KEYWORDS, CONTROL_FLOW, BASE_TYPES, ENGINE_TYPES + class_db.map(func(c): return str(c.name)))


func _makeCode() -> CodeEdit:
	var code := CodeEdit.new()
	code.size_flags_vertical = Control.SIZE_EXPAND_FILL
	code.syntax_highlighter = _highlighter()
	code.add_theme_font_override("font", Style.codeFont)
	code.add_theme_font_size_override("font_size", 14)
	code.add_theme_color_override("background_color", Style.VIEWPORT)
	code.add_theme_color_override("font_color", Color("cdcfd2"))
	code.add_theme_color_override("current_line_color", Color(1, 1, 1, 0.04))
	code.add_theme_color_override("selection_color", Color("3d5a80", 0.75))
	code.add_theme_color_override("line_number_color", Color("cdcfd2", 0.45))
	code.add_theme_color_override("caret_color", Color.WHITE)
	code.add_theme_color_override("word_highlighted_color", Color(1, 1, 1, 0.08))
	code.add_theme_color_override("search_result_color", Color("e7c547", 0.25))
	code.add_theme_color_override("brace_mismatch_color", Color("ff3333"))
	code.add_theme_color_override("code_folding_color", Color("cdcfd2", 0.6))
	code.add_theme_stylebox_override("normal", Style.box(Style.VIEWPORT, Style.RADIUS, Style.BORDER, Vector4(6, 6, 6, 6)))
	code.add_theme_stylebox_override("focus", Style.empty())
	code.gutters_draw_line_numbers = true
	code.gutters_draw_fold_gutter = true
	code.line_folding = true
	code.highlight_current_line = true
	code.highlight_all_occurrences = true
	code.draw_tabs = true
	code.minimap_draw = true
	code.minimap_width = 70
	code.caret_blink = true
	code.scroll_past_end_of_file = true
	code.indent_use_spaces = false
	code.indent_size = 4
	code.indent_automatic = true
	code.auto_brace_completion_enabled = true
	code.auto_brace_completion_highlight_matching = true
	code.code_completion_enabled = true
	code.code_completion_prefixes = [".", "$", "(", "\""]
	code.add_comment_delimiter("#", "", true)
	code.add_string_delimiter("\"", "\"", false)
	code.add_string_delimiter("'", "'", false)
	#Godot's slot icon next to the methods that the scene's signals call.
	code.add_gutter()
	var slots := code.get_gutter_count() - 1
	code.set_meta("slot_gutter", slots)
	code.set_gutter_type(slots, TextEdit.GUTTER_TYPE_ICON)
	code.set_gutter_width(slots, 18)
	code.text_changed.connect(_onTextChanged.bind(code))
	code.caret_changed.connect(_updateCaret)
	code.code_completion_requested.connect(_onCompletion.bind(code))
	code.gui_input.connect(_codeInput)
	return code

func hasUnsaved() -> bool:
	for path in _order:
		if isModified(path): return true

	return false


func isModified(path: String) -> bool:
	return _scripts.has(path) and _scripts[path].code.text != _scripts[path].saved


func openScript(path: String, line: int = 0) -> bool:
	if not _scripts.has(path):
		var file: String = project.toAbs(path)

		if not FileAccess.file_exists(file):
			message.emit("Script not found: " + path, true)
			return false

		var text := FileAccess.get_file_as_string(file)
		var code := _makeCode()
		code.text = text
		code.clear_undo_history()
		code.visible = false
		_holder.add_child(code)
		_scripts[path] = {"code": code, "saved": text, "info": {}, "mtime": FileAccess.get_modified_time(file)}
		_order.append(path)
		check(path)

	showScript(path)

	if line > 0: goto_line(line)

	return true


# Appends Godot's stub for a connected signal's method and returns its line; the script stays unsaved, as in Godot.
func addFunction(path: String, functionName: String, arguments: Array) -> int:
	if not openScript(path): return 0

	var code: CodeEdit = _scripts[path].code
	var text := code.text
	var gap := "\n\n" if text.ends_with("\n") else "\n\n\n"

	if text.is_empty(): gap = ""

	var last := code.get_line_count() - 1
	code.insert_text(gap + "func %s(%s) -> void:\n\tpass # Replace with function body.\n" % [functionName, ", ".join(arguments)],
		last, code.get_line(last).length())
	_refreshListMarks()
	check(path)

	for f in _scripts[path].info.get("functions", []):
		if str(f.name) == functionName: return int(f.line)

	return code.get_line_count()


func showScript(path: String):
	if not _scripts.has(path): return

	_current = path

	for p in _scripts: _scripts[p].code.visible = p == path

	_empty.visible = false
	_refreshList()
	_refreshMethods()
	_updateStatus()
	_updateCaret()
	_pathLabel.text = path
	_scripts[path].code.grab_focus()


func current_path() -> String:
	return _current


func sourceOf(path: String) -> Variant:
	return _scripts[path].code.text if _scripts.has(path) else null


func save(path: String = "") -> bool:
	if path.is_empty(): path = _current

	if not _scripts.has(path): return false

	var entry: Dictionary = _scripts[path]
	var error: String = project.write(path, entry.code.text)

	if not error.is_empty():
		message.emit(error, true)
		return false

	entry.saved = entry.code.text
	entry.mtime = FileAccess.get_modified_time(project.toAbs(path))
	_refreshList()
	saved.emit(path)
	return true


func saveAll() -> bool:
	var ok := true

	for path in _order:
		if isModified(path): ok = save(path) and ok

	return ok


func closeScript(path: String):
	if not _scripts.has(path): return

	#no "save changes?" prompt like Godot's, just save so nothing is lost
	if isModified(path): save(path)

	_scripts[path].code.queue_free()
	_scripts.erase(path)
	_order.erase(path)

	if _current == path:
		_current = ""

		if not _order.is_empty(): showScript(_order[-1])
		else:
			_empty.visible = true
			_pathLabel.text = ""
			_refreshList()
			_refreshMethods()
			_updateStatus()


func closeAll():
	for path in _order.duplicate(): closeScript(path)


# Called when the FileSystem dock renames or removes a path.
func fileMoved(oldPath: String, newPath: String):
	for path in _order.duplicate():
		if path == oldPath or path.begins_with(oldPath + "/"):
			var target: String = newPath + path.trim_prefix(oldPath)
			_scripts[target] = _scripts[path]
			_scripts.erase(path)
			_order[_order.find(path)] = target

			if _current == path: _current = target

	_refreshList()

	if not _current.is_empty(): _pathLabel.text = _current


func file_removed(path: String):
	for open in _order.duplicate():
		if open == path or open.begins_with(path + "/"):
			_drop(open)


func _drop(path: String):
	_scripts[path].code.queue_free()
	_scripts.erase(path)
	_order.erase(path)

	if _current == path:
		_current = ""

		if not _order.is_empty(): showScript(_order[-1])
		else:
			_empty.visible = true

	_refreshList()


func newScriptText(base: String) -> String:
	return TEMPLATE % base


func _refreshList():
	_list.clear()
	var text := _filter.text.strip_edges().to_lower()

	for path in _order:
		if not text.is_empty() and not text in path.to_lower(): continue

		var title: String = path.get_file() + ("(*)" if isModified(path) else "")
		var index := _list.add_item(title, Icons.get_icon("Script"))
		_list.set_item_metadata(index, path)
		_list.set_item_tooltip(index, path)
		var info: Dictionary = _scripts[path].info

		if not info.get("ok", true): _list.set_item_custom_fg_color(index, Style.ERROR)

		if path == _current: _list.select(index)


func _refreshMethods():
	_methods.clear()

	if _current.is_empty(): return

	for f in _scripts[_current].info.get("functions", []):
		var index := _methods.add_item(str(f.name), Icons.get_icon("MemberMethod"))
		_methods.set_item_metadata(index, int(f.line))


func _onFileMenu(id: int):
	match id:
		0: new_script_requested.emit()
		1: save()
		2: saveAll()
		3: closeScript(_current)
		4: OS.shell_open(docs.path_join("jetscript.md"))


func _onSearchMenu(id: int):
	match id:
		0: showFind()
		1: findNext(true)
		2: findNext(false)
		3:
			if _current.is_empty(): return

			_gotoEdit.value = _scripts[_current].code.get_caret_line() + 1
			_gotoDialog.popup_centered(Vector2i(260, 90))
			_gotoEdit.get_line_edit().grab_focus()
			_gotoEdit.get_line_edit().select_all()

		4: toggleComment()

func _onTextChanged(code: CodeEdit):
	_checkTimer.start()
	_refreshListMarks()
	#auto-popup completion while typing an identifier, as Godot does.
	var line := code.get_line(code.get_caret_line())
	var column := code.get_caret_column()

	if column >= 2:
		var word := ""
		var i := column - 1

		while i >= 0 and (line[i] == "_" or line[i].is_valid_identifier() or line[i].is_valid_int()):
			word = line[i] + word
			i -= 1

		if word.length() >= 2 and not word[0].is_valid_int() and not _inCommentOrString(line, column):
			code.request_code_completion()

	if not _runtimeError.is_empty() and _runtimeError.path == _current:
		code.set_line_background_color(int(_runtimeError.line) - 1, Color(0, 0, 0, 0))
		_runtimeError = {}


func _inCommentOrString(line: String, column: int) -> bool:
	var quote := ""

	for i in mini(column, line.length()):
		var c := line[i]

		if quote.is_empty():
			if c == "#": return true

			if c == "\"" or c == "'": quote = c
		elif c == quote: quote = ""

	return not quote.is_empty()


func _refreshListMarks():
	for i in _list.item_count:
		var path := str(_list.get_item_metadata(i))
		_list.set_item_text(i, path.get_file() + ("(*)" if isModified(path) else ""))


func _updateCaret():
	if _current.is_empty():
		_caretLabel.text = ""
		return

	var code: CodeEdit = _scripts[_current].code
	_caretLabel.text = "Line %d, Col %d" % [code.get_caret_line() + 1, code.get_caret_column() + 1]


func _codeInput(event: InputEvent):
	if not event is InputEventKey or not event.pressed: return

	if event.keycode == KEY_F3:
		findNext(not event.shift_pressed)
		accept_event()
	elif event.keycode == KEY_ESCAPE and _findBar.visible:
		_hideFind()
		accept_event()


func goto_line(line: int):
	if _current.is_empty(): return

	var code: CodeEdit = _scripts[_current].code
	var target := clampi(line - 1, 0, code.get_line_count() - 1)
	code.set_caret_line(target)
	code.set_caret_column(code.get_line(target).length() - code.get_line(target).strip_edges(true, false).length())
	code.center_viewport_to_caret()
	code.grab_focus()


func toggleComment():
	if _current.is_empty(): return

	var code: CodeEdit = _scripts[_current].code
	var from := code.get_selection_from_line() if code.has_selection() else code.get_caret_line()
	var to := code.get_selection_to_line() if code.has_selection() else code.get_caret_line()
	var allCommented := true

	for i in range(from, to + 1):
		if not code.get_line(i).strip_edges().is_empty() and not code.get_line(i).strip_edges().begins_with("#"): allCommented = false

	code.begin_complex_operation()

	for i in range(from, to + 1):
		var text := code.get_line(i)

		if allCommented:
			var at := text.find("#")

			if at >= 0: code.set_line(i, text.substr(0, at) + text.substr(at + 1))
		elif not text.strip_edges().is_empty():
			code.set_line(i, "#" + text)

	code.end_complex_operation()


func showFind():
	if _current.is_empty(): return

	_findBar.visible = true
	var code: CodeEdit = _scripts[_current].code

	if code.has_selection() and code.get_selection_from_line() == code.get_selection_to_line():
		_findEdit.text = code.get_selected_text()

	_findEdit.grab_focus()
	_findEdit.select_all()
	_highlightSearch()


func _hideFind():
	_findBar.visible = false

	if not _current.is_empty():
		_scripts[_current].code.set_search_text("")
		_scripts[_current].code.queue_redraw()
		_scripts[_current].code.grab_focus()


func _highlightSearch():
	if _current.is_empty(): return

	var code: CodeEdit = _scripts[_current].code
	code.set_search_text(_findEdit.text)
	code.set_search_flags(TextEdit.SEARCH_MATCH_CASE if _findCase.button_pressed else 0)
	code.queue_redraw()


func findNext(forward: bool):
	if _current.is_empty() or _findEdit.text.is_empty(): return

	var code: CodeEdit = _scripts[_current].code
	var flags := TextEdit.SEARCH_MATCH_CASE if _findCase.button_pressed else 0

	if not forward: flags |= TextEdit.SEARCH_BACKWARDS

	var line := code.get_caret_line()
	var column := code.get_caret_column()

	if not forward and code.has_selection(): column = code.get_selection_from_column()

	var found := code.search(_findEdit.text, flags, line, column)

	if found.x < 0: return

	code.select(found.y, found.x, found.y, found.x + _findEdit.text.length())
	code.set_caret_line(found.y)
	code.set_caret_column(found.x + (_findEdit.text.length() if forward else 0))
	code.center_viewport_to_caret()

func check(path: String):
	if not _scripts.has(path) or bridge == null: return

	var entry: Dictionary = _scripts[path]
	var code: CodeEdit = entry.code
	var info: Dictionary = bridge.compile_script(code.text, path)

	for line in code.get_line_count():
		if line != int(_runtimeError.get("line", 0)) - 1 or _runtimeError.get("path") != path:
			code.set_line_background_color(line, Color(0, 0, 0, 0))

	for warning in info.get("warnings", []):
		var line := int(warning.line) - 1

		if line >= 0 and line < code.get_line_count(): code.set_line_background_color(line, Color(Style.WARNING, 0.12))

	for error in info.get("errors", []):
		var line := int(error.line) - 1

		if line >= 0 and line < code.get_line_count(): code.set_line_background_color(line, Color(Style.ERROR, 0.22))

	entry.info = info
	_markConnected(path)

	if path == _current:
		_updateStatus()
		_refreshMethods()

	_refreshList()
	script_info_changed.emit(path, info)

# script path -> {method: "Node.signal"}, from the scene being edited
var _connected := {}


# The methods of each script that the scene's connections call, marked in the gutter.
func setConnectedMethods(methods: Dictionary):
	_connected = methods

	for path in _scripts: _markConnected(path)


func _markConnected(path: String):
	var code: CodeEdit = _scripts[path].code
	var marks: Dictionary = _connected.get(path, {})
	var lines := {}

	for f in _scripts[path].info.get("functions", []):
		if marks.has(str(f.name)): lines[int(f.line) - 1] = marks[str(f.name)]

	var slots: int = code.get_meta("slot_gutter")

	for line in code.get_line_count():
		code.set_line_gutter_icon(line, slots, Icons.get_icon("Slot") if lines.has(line) else null)


func infoOf(path: String) -> Dictionary:
	return _scripts[path].info if _scripts.has(path) else {}


func _updateStatus():
	_status.remove_theme_color_override("font_color")

	if _current.is_empty():
		_status.text = ""
		_status.icon = null
		return

	var info: Dictionary = _scripts[_current].info

	if not _runtimeError.is_empty() and _runtimeError.path == _current:
		_status.text = "Runtime error (line %d): %s" % [_runtimeError.line, _runtimeError.message]
		_status.icon = Icons.get_icon("StatusError")
		_status.add_theme_color_override("font_color", Style.ERROR)
	elif not info.get("errors", []).is_empty():
		var e: Dictionary = info.errors[0]
		var extra: int = info.errors.size() - 1
		_status.text = "Line %d, col %d: %s%s" % [e.line, e.column, e.message, ("  (+%d more)" % extra) if extra > 0 else ""]
		_status.icon = Icons.get_icon("StatusError")
		_status.add_theme_color_override("font_color", Style.ERROR)
	elif not info.get("warnings", []).is_empty():
		var w: Dictionary = info.warnings[0]
		_status.text = "Line %d: %s" % [w.line, w.message]
		_status.icon = Icons.get_icon("StatusWarning")
		_status.add_theme_color_override("font_color", Style.WARNING)
	else:
		_status.text = "No errors"
		_status.icon = Icons.get_icon("StatusSuccess")
		_status.add_theme_color_override("font_color", Style.SUCCESS)


func _gotoStatus():
	if _current.is_empty(): return

	if not _runtimeError.is_empty() and _runtimeError.path == _current:
		goto_line(int(_runtimeError.line))
		return

	var info: Dictionary = _scripts[_current].info

	for key in ["errors", "warnings"]:
		if not info.get(key, []).is_empty():
			goto_line(int(info[key][0].line))
			return


# Highlights a runtime error line (like Godot's debugger break).
func showRuntimeError(path: String, line: int, text: String):
	if not openScript(path, line): return

	_runtimeError = {"path": path, "line": line, "message": text}
	var code: CodeEdit = _scripts[path].code

	if line - 1 < code.get_line_count(): code.set_line_background_color(line - 1, Color(Style.ERROR, 0.35))

	_updateStatus()


func clearRuntimeError():
	if _runtimeError.is_empty(): return

	var path: String = _runtimeError.path
	_runtimeError = {}

	if _scripts.has(path): check(path)

	_updateStatus()


# Reloads files changed outside the editor, e.g. in VS Code.
func checkExternalChanges():
	for path in _order:
		var file: String = project.toAbs(path)

		if not FileAccess.file_exists(file): continue

		var entry: Dictionary = _scripts[path]
		var mtime := FileAccess.get_modified_time(file)

		if mtime == entry.mtime: continue

		entry.mtime = mtime
		var text := FileAccess.get_file_as_string(file)

		if text == entry.saved: continue

		if not isModified(path): _reload(path)
		elif not _reloadDialog.visible:
			_reloadPath = path
			_reloadDialog.dialog_text = "%s was modified outside Jet Studio.\nReload it from disk?" % path
			_reloadDialog.popup_centered()


func _reload(path: String):
	if not _scripts.has(path): return

	var text := FileAccess.get_file_as_string(project.toAbs(path))
	var code: CodeEdit = _scripts[path].code
	var line := code.get_caret_line()
	code.text = text
	code.set_caret_line(mini(line, code.get_line_count() - 1))
	_scripts[path].saved = text
	check(path)

func _onCompletion(code: CodeEdit):
	var line := code.get_line(code.get_caret_line()).substr(0, code.get_caret_column())
	var wordStart := line.length()

	while wordStart > 0 and (line[wordStart - 1] == "_" or line[wordStart - 1].is_valid_identifier() or line[wordStart - 1].is_valid_int()):
		wordStart -= 1

	var before := line.substr(0, wordStart)
	var info: Dictionary = _scripts[_current].info if not _current.is_empty() else {}
	var added := {}
	var add := func(kind: int, text: String, insert: String, color: Color, icon: String):
		if added.has(text): return

		added[text] = true
		code.add_code_completion_option(kind, text, insert, color, Icons.get_icon(icon))

	if before.ends_with("$"):
		for nodeName in _sceneNodeNames: add.call(CodeEdit.KIND_NODE_PATH, nodeName, nodeName, Color("63c259"), "Node")

		code.update_code_completion_options(true)
		return

	if before.ends_with("."):
		var owner := before.trim_suffix(".")
		var ownerWord := ""
		var i := owner.length() - 1

		while i >= 0 and (owner[i] == "_" or owner[i].is_valid_identifier() or owner[i].is_valid_int()):
			ownerWord = owner[i] + ownerWord
			i -= 1

		if MEMBERS.has(ownerWord):
			for m in MEMBERS[ownerWord]: add.call(CodeEdit.KIND_MEMBER, m, m, Color("bce0ff"), "MemberProperty")
		elif ownerWord == "Input":
			for m in _classMethods("Input"): add.call(CodeEdit.KIND_FUNCTION, m + "()", m + "(", Color("57b3ff"), "MemberMethod")
		else:
			for c in class_db:
				for p in c.properties: add.call(CodeEdit.KIND_MEMBER, p.name, p.name, Color("bce0ff"), "MemberProperty")

				for m in c.methods: add.call(CodeEdit.KIND_FUNCTION, m.name + "()", m.name + "(", Color("57b3ff"), "MemberMethod")

				for sig in c.get("signals", []): add.call(CodeEdit.KIND_SIGNAL, sig.name, sig.name, Color("ffd760"), "Signal")

			for sig in info.get("signals", []): add.call(CodeEdit.KIND_SIGNAL, sig.name, sig.name, Color("ffd760"), "Signal")

			for type in ["Vector3", "String", "Array", "Dictionary", "Color", "Signal", "Callable"]:
				for m in MEMBERS[type]: add.call(CodeEdit.KIND_MEMBER, m, m, Color("bce0ff"), "MemberProperty")

		code.update_code_completion_options(true)
		return

	for m in info.get("members", []):
		add.call(CodeEdit.KIND_CONSTANT if m.constant else CodeEdit.KIND_MEMBER, m.name, m.name, Color("bce0ff"), "MemberConstant" if m.constant else "MemberProperty")

	for f in info.get("functions", []):
		add.call(CodeEdit.KIND_FUNCTION, f.name + "()", f.name + "(", Color("57b3ff"), "MemberMethod")

	for sig in info.get("signals", []): add.call(CodeEdit.KIND_SIGNAL, sig.name, sig.name, Color("ffd760"), "Signal")

	var base := str(info.get("base", "Node"))

	for c in _classChain(base):
		for p in c.properties: add.call(CodeEdit.KIND_MEMBER, p.name, p.name, Color("bce0ff"), "MemberProperty")

		for m in c.methods: add.call(CodeEdit.KIND_FUNCTION, m.name + "()", m.name + "(", Color("57b3ff"), "MemberMethod")

		for sig in c.get("signals", []): add.call(CodeEdit.KIND_SIGNAL, sig.name, sig.name, Color("ffd760"), "Signal")

	for word in BUILTINS: add.call(CodeEdit.KIND_FUNCTION, word + "()", word + "(", Color("57b3ff"), "MemberMethod")

	for word in BASE_TYPES + ENGINE_TYPES: add.call(CodeEdit.KIND_CLASS, word, word, Color("42ffc2"), "Object")

	for word in KEYWORDS: add.call(CodeEdit.KIND_PLAIN_TEXT, word, word, Color("ff7085"), "")

	for word in ["_ready", "_process"]: add.call(CodeEdit.KIND_FUNCTION, word, word, Color("57b3ff"), "MemberMethod")

	code.update_code_completion_options(false)

var _sceneNodeNames := []


func setSceneNodeNames(names: Array):
	_sceneNodeNames = names


func _classMethods(cls: String) -> Array:
	for c in class_db:
		if c.name == cls: return c.methods.map(func(m): return m.name)

	return []


func _classChain(cls: String) -> Array:
	var chain := []
	var current := cls

	while not current.is_empty():
		var found := {}

		for c in class_db:
			if c.name == current: found = c

		if found.is_empty(): break

		chain.append(found)
		current = str(found.base)

	return chain
