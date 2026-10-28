extends HSplitContainer

signal saved(path: String)
signal script_info_changed(path: String, info: Dictionary)
signal new_script_requested
signal message(text: String, is_error: bool)

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
var docs := "" # Jet Studio's docs folder
var class_db := [] # from the bridge
var _scripts := {} # path -> {code, saved, info, error_line}
var _order := []
var _current := ""
var _list: ItemList
var _methods: ItemList
var _filter: LineEdit
var _holder: VBoxContainer
var _path_label: Label
var _status: Button
var _caret_label: Label
var _find_bar: HBoxContainer
var _find_edit: LineEdit
var _find_case: CheckBox
var _goto_dialog: ConfirmationDialog
var _goto_edit: SpinBox
var _check_timer: Timer
var _empty: Label
var _reload_dialog: ConfirmationDialog
var _reload_path := ""
var _runtime_error := {} # {path, line}

func _ready() -> void:
	split_offset = 210
	var left := VBoxContainer.new()
	left.custom_minimum_size.x = 160
	add_child(left)
	_filter = LineEdit.new()
	_filter.placeholder_text = "Filter Scripts"
	_filter.clear_button_enabled = true
	_filter.right_icon = Icons.get_icon("Search")
	_filter.text_changed.connect(func(_t): _refresh_list())
	left.add_child(_filter)
	_list = ItemList.new()
	_list.size_flags_vertical = Control.SIZE_EXPAND_FILL
	_list.size_flags_stretch_ratio = 1.4
	_list.item_selected.connect(func(i): show_script(str(_list.get_item_metadata(i))))
	_list.item_clicked.connect(func(i, _p, button):
		if button == MOUSE_BUTTON_MIDDLE: close_script(str(_list.get_item_metadata(i))))
	left.add_child(_list)
	var methods_title := Label.new()
	methods_title.text = "Methods"
	methods_title.theme_type_variation = "DockTitle"
	left.add_child(methods_title)
	_methods = ItemList.new()
	_methods.size_flags_vertical = Control.SIZE_EXPAND_FILL
	_methods.item_selected.connect(func(i): goto_line(int(_methods.get_item_metadata(i))))
	left.add_child(_methods)
	var right := VBoxContainer.new()
	right.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	add_child(right)
	var bar := HBoxContainer.new()
	right.add_child(bar)
	var file_menu := MenuButton.new()
	file_menu.text = "File"
	var popup := file_menu.get_popup()
	popup.add_icon_item(Icons.get_icon("ScriptCreate"), "New Script...", 0)
	popup.add_icon_item(Icons.get_icon("Save"), "Save  (Ctrl+S)", 1)
	popup.add_item("Save All  (Ctrl+Shift+Alt+S)", 2)
	popup.add_separator()
	popup.add_icon_item(Icons.get_icon("Close"), "Close  (Ctrl+W)", 3)
	popup.add_separator()
	popup.add_icon_item(Icons.get_icon("Help"), "JetScript Reference", 4)
	popup.id_pressed.connect(_on_file_menu)
	bar.add_child(file_menu)
	var search_menu := MenuButton.new()
	search_menu.text = "Search"
	var search := search_menu.get_popup()
	search.add_icon_item(Icons.get_icon("Search"), "Find...  (Ctrl+F)", 0)
	search.add_item("Find Next  (F3)", 1)
	search.add_item("Find Previous  (Shift+F3)", 2)
	search.add_separator()
	search.add_item("Go to Line...  (Ctrl+L)", 3)
	search.add_item("Toggle Comment  (Ctrl+K)", 4)
	search.id_pressed.connect(_on_search_menu)
	bar.add_child(search_menu)
	_path_label = Label.new()
	_path_label.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	_path_label.horizontal_alignment = HORIZONTAL_ALIGNMENT_RIGHT
	_path_label.add_theme_color_override("font_color", Style.MUTED)
	_path_label.clip_text = true
	bar.add_child(_path_label)
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
	_find_bar = HBoxContainer.new()
	_find_bar.visible = false
	right.add_child(_find_bar)
	_find_edit = LineEdit.new()
	_find_edit.placeholder_text = "Find"
	_find_edit.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	_find_edit.text_submitted.connect(func(_t): find_next(true))
	_find_edit.text_changed.connect(func(_t): _highlight_search())
	_find_bar.add_child(_find_edit)
	var previous := Button.new()
	previous.theme_type_variation = "FlatButton"
	previous.icon = Icons.get_icon("MoveUp")
	previous.pressed.connect(func(): find_next(false))
	_find_bar.add_child(previous)
	var next := Button.new()
	next.theme_type_variation = "FlatButton"
	next.icon = Icons.get_icon("MoveDown")
	next.pressed.connect(func(): find_next(true))
	_find_bar.add_child(next)
	_find_case = CheckBox.new()
	_find_case.text = "Match Case"
	_find_case.toggled.connect(func(_on): _highlight_search())
	_find_bar.add_child(_find_case)
	var close_find := Button.new()
	close_find.theme_type_variation = "FlatButton"
	close_find.icon = Icons.get_icon("Close")
	close_find.pressed.connect(_hide_find)
	_find_bar.add_child(close_find)
	var status_bar := HBoxContainer.new()
	right.add_child(status_bar)
	_status = Button.new()
	_status.theme_type_variation = "FlatButton"
	_status.alignment = HORIZONTAL_ALIGNMENT_LEFT
	_status.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	_status.clip_text = true
	_status.pressed.connect(_goto_status)
	status_bar.add_child(_status)
	_caret_label = Label.new()
	_caret_label.add_theme_color_override("font_color", Style.MUTED)
	_caret_label.add_theme_font_size_override("font_size", 12)
	status_bar.add_child(_caret_label)
	_check_timer = Timer.new()
	_check_timer.one_shot = true
	_check_timer.wait_time = 0.35
	_check_timer.timeout.connect(func(): check(_current))
	add_child(_check_timer)
	_goto_dialog = ConfirmationDialog.new()
	_goto_dialog.cancel_button_text = "Cancel"
	_goto_dialog.title = "Go to Line"
	_goto_edit = SpinBox.new()
	_goto_edit.min_value = 1
	_goto_edit.max_value = 100000
	_goto_dialog.add_child(_goto_edit)
	_goto_dialog.register_text_enter(_goto_edit.get_line_edit())
	_goto_dialog.confirmed.connect(func(): goto_line(int(_goto_edit.value)))
	add_child(_goto_dialog)
	_reload_dialog = ConfirmationDialog.new()
	_reload_dialog.cancel_button_text = "Cancel"
	_reload_dialog.title = "File Modified"
	_reload_dialog.ok_button_text = "Reload from Disk"
	_reload_dialog.cancel_button_text = "Keep My Version"
	_reload_dialog.confirmed.connect(func(): _reload(_reload_path))
	add_child(_reload_dialog)
	_update_status()

func _highlighter() -> SyntaxHighlighter:
	return Highlighter.new(KEYWORDS, CONTROL_FLOW, BASE_TYPES, ENGINE_TYPES + class_db.map(func(c): return str(c.name)))

func _make_code() -> CodeEdit:
	var code := CodeEdit.new()
	code.size_flags_vertical = Control.SIZE_EXPAND_FILL
	code.syntax_highlighter = _highlighter()
	code.add_theme_font_override("font", Style.code_font)
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
	# Godot's slot icon next to the methods that the scene's signals call.
	code.add_gutter()
	var slots := code.get_gutter_count() - 1
	code.set_meta("slot_gutter", slots)
	code.set_gutter_type(slots, TextEdit.GUTTER_TYPE_ICON)
	code.set_gutter_width(slots, 18)
	code.text_changed.connect(_on_text_changed.bind(code))
	code.caret_changed.connect(_update_caret)
	code.code_completion_requested.connect(_on_completion.bind(code))
	code.gui_input.connect(_code_input)
	return code

# ------------------------------------------------------------------ open/save

func has_unsaved() -> bool:
	for path in _order:
		if is_modified(path): return true
	return false

func is_modified(path: String) -> bool:
	return _scripts.has(path) and _scripts[path].code.text != _scripts[path].saved

func open_script(path: String, line: int = 0) -> bool:
	if not _scripts.has(path):
		var file: String = project.to_abs(path)
		if not FileAccess.file_exists(file):
			message.emit("Script not found: " + path, true)
			return false
		var text := FileAccess.get_file_as_string(file)
		var code := _make_code()
		code.text = text
		code.clear_undo_history()
		code.visible = false
		_holder.add_child(code)
		_scripts[path] = {"code": code, "saved": text, "info": {}, "mtime": FileAccess.get_modified_time(file)}
		_order.append(path)
		check(path)
	show_script(path)
	if line > 0: goto_line(line)
	return true

## Appends Godot's stub for a connected signal's method and returns its line; the script stays unsaved, as in Godot.
func add_function(path: String, function_name: String, arguments: Array) -> int:
	if not open_script(path): return 0
	var code: CodeEdit = _scripts[path].code
	var text := code.text
	var gap := "\n\n" if text.ends_with("\n") else "\n\n\n"
	if text.is_empty(): gap = ""
	var last := code.get_line_count() - 1
	code.insert_text(gap + "func %s(%s) -> void:\n\tpass # Replace with function body.\n" % [function_name, ", ".join(arguments)],
		last, code.get_line(last).length())
	_refresh_list_marks()
	check(path)
	for f in _scripts[path].info.get("functions", []):
		if str(f.name) == function_name: return int(f.line)
	return code.get_line_count()

func show_script(path: String) -> void:
	if not _scripts.has(path): return
	_current = path
	for p in _scripts: _scripts[p].code.visible = p == path
	_empty.visible = false
	_refresh_list()
	_refresh_methods()
	_update_status()
	_update_caret()
	_path_label.text = path
	_scripts[path].code.grab_focus()

func current_path() -> String:
	return _current

func source_of(path: String) -> Variant:
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
	entry.mtime = FileAccess.get_modified_time(project.to_abs(path))
	_refresh_list()
	saved.emit(path)
	return true

func save_all() -> bool:
	var ok := true
	for path in _order:
		if is_modified(path): ok = save(path) and ok
	return ok

func close_script(path: String) -> void:
	if not _scripts.has(path): return
	if is_modified(path): save(path) # no "save changes?" prompt like Godot's, just save so nothing is lost
	_scripts[path].code.queue_free()
	_scripts.erase(path)
	_order.erase(path)
	if _current == path:
		_current = ""
		if not _order.is_empty(): show_script(_order[-1])
		else:
			_empty.visible = true
			_path_label.text = ""
			_refresh_list()
			_refresh_methods()
			_update_status()

func close_all() -> void:
	for path in _order.duplicate(): close_script(path)

## Called when the FileSystem dock renames or removes a path.
func file_moved(old_path: String, new_path: String) -> void:
	for path in _order.duplicate():
		if path == old_path or path.begins_with(old_path + "/"):
			var target: String = new_path + path.trim_prefix(old_path)
			_scripts[target] = _scripts[path]
			_scripts.erase(path)
			_order[_order.find(path)] = target
			if _current == path: _current = target
	_refresh_list()
	if not _current.is_empty(): _path_label.text = _current

func file_removed(path: String) -> void:
	for open in _order.duplicate():
		if open == path or open.begins_with(path + "/"):
			_drop(open)

func _drop(path: String) -> void:
	_scripts[path].code.queue_free()
	_scripts.erase(path)
	_order.erase(path)
	if _current == path:
		_current = ""
		if not _order.is_empty(): show_script(_order[-1])
		else:
			_empty.visible = true
	_refresh_list()

func new_script_text(base: String) -> String:
	return TEMPLATE % base

func _refresh_list() -> void:
	_list.clear()
	var text := _filter.text.strip_edges().to_lower()
	for path in _order:
		if not text.is_empty() and not text in path.to_lower(): continue
		var title: String = path.get_file() + ("(*)" if is_modified(path) else "")
		var index := _list.add_item(title, Icons.get_icon("Script"))
		_list.set_item_metadata(index, path)
		_list.set_item_tooltip(index, path)
		var info: Dictionary = _scripts[path].info
		if not info.get("ok", true): _list.set_item_custom_fg_color(index, Style.ERROR)
		if path == _current: _list.select(index)

func _refresh_methods() -> void:
	_methods.clear()
	if _current.is_empty(): return
	for f in _scripts[_current].info.get("functions", []):
		var index := _methods.add_item(str(f.name), Icons.get_icon("MemberMethod"))
		_methods.set_item_metadata(index, int(f.line))

func _on_file_menu(id: int) -> void:
	match id:
		0: new_script_requested.emit()
		1: save()
		2: save_all()
		3: close_script(_current)
		4: OS.shell_open(docs.path_join("jetscript.md"))

func _on_search_menu(id: int) -> void:
	match id:
		0: show_find()
		1: find_next(true)
		2: find_next(false)
		3:
			if _current.is_empty(): return
			_goto_edit.value = _scripts[_current].code.get_caret_line() + 1
			_goto_dialog.popup_centered(Vector2i(260, 90))
			_goto_edit.get_line_edit().grab_focus()
			_goto_edit.get_line_edit().select_all()
		4: toggle_comment()

# ------------------------------------------------------------------ editing

func _on_text_changed(code: CodeEdit) -> void:
	_check_timer.start()
	_refresh_list_marks()
	# Auto-popup completion while typing an identifier, as Godot does.
	var line := code.get_line(code.get_caret_line())
	var column := code.get_caret_column()
	if column >= 2:
		var word := ""
		var i := column - 1
		while i >= 0 and (line[i] == "_" or line[i].is_valid_identifier() or line[i].is_valid_int()):
			word = line[i] + word
			i -= 1
		if word.length() >= 2 and not word[0].is_valid_int() and not _in_comment_or_string(line, column):
			code.request_code_completion()
	if not _runtime_error.is_empty() and _runtime_error.path == _current:
		code.set_line_background_color(int(_runtime_error.line) - 1, Color(0, 0, 0, 0))
		_runtime_error = {}

func _in_comment_or_string(line: String, column: int) -> bool:
	var quote := ""
	for i in mini(column, line.length()):
		var c := line[i]
		if quote.is_empty():
			if c == "#": return true
			if c == "\"" or c == "'": quote = c
		elif c == quote: quote = ""
	return not quote.is_empty()

func _refresh_list_marks() -> void:
	for i in _list.item_count:
		var path := str(_list.get_item_metadata(i))
		_list.set_item_text(i, path.get_file() + ("(*)" if is_modified(path) else ""))

func _update_caret() -> void:
	if _current.is_empty():
		_caret_label.text = ""
		return
	var code: CodeEdit = _scripts[_current].code
	_caret_label.text = "Line %d, Col %d" % [code.get_caret_line() + 1, code.get_caret_column() + 1]

func _code_input(event: InputEvent) -> void:
	if not event is InputEventKey or not event.pressed: return
	if event.keycode == KEY_F3:
		find_next(not event.shift_pressed)
		accept_event()
	elif event.keycode == KEY_ESCAPE and _find_bar.visible:
		_hide_find()
		accept_event()

func goto_line(line: int) -> void:
	if _current.is_empty(): return
	var code: CodeEdit = _scripts[_current].code
	var target := clampi(line - 1, 0, code.get_line_count() - 1)
	code.set_caret_line(target)
	code.set_caret_column(code.get_line(target).length() - code.get_line(target).strip_edges(true, false).length())
	code.center_viewport_to_caret()
	code.grab_focus()

func toggle_comment() -> void:
	if _current.is_empty(): return
	var code: CodeEdit = _scripts[_current].code
	var from := code.get_selection_from_line() if code.has_selection() else code.get_caret_line()
	var to := code.get_selection_to_line() if code.has_selection() else code.get_caret_line()
	var all_commented := true
	for i in range(from, to + 1):
		if not code.get_line(i).strip_edges().is_empty() and not code.get_line(i).strip_edges().begins_with("#"): all_commented = false
	code.begin_complex_operation()
	for i in range(from, to + 1):
		var text := code.get_line(i)
		if all_commented:
			var at := text.find("#")
			if at >= 0: code.set_line(i, text.substr(0, at) + text.substr(at + 1))
		elif not text.strip_edges().is_empty():
			code.set_line(i, "#" + text)
	code.end_complex_operation()

func show_find() -> void:
	if _current.is_empty(): return
	_find_bar.visible = true
	var code: CodeEdit = _scripts[_current].code
	if code.has_selection() and code.get_selection_from_line() == code.get_selection_to_line():
		_find_edit.text = code.get_selected_text()
	_find_edit.grab_focus()
	_find_edit.select_all()
	_highlight_search()

func _hide_find() -> void:
	_find_bar.visible = false
	if not _current.is_empty():
		_scripts[_current].code.set_search_text("")
		_scripts[_current].code.queue_redraw()
		_scripts[_current].code.grab_focus()

func _highlight_search() -> void:
	if _current.is_empty(): return
	var code: CodeEdit = _scripts[_current].code
	code.set_search_text(_find_edit.text)
	code.set_search_flags(TextEdit.SEARCH_MATCH_CASE if _find_case.button_pressed else 0)
	code.queue_redraw()

func find_next(forward: bool) -> void:
	if _current.is_empty() or _find_edit.text.is_empty(): return
	var code: CodeEdit = _scripts[_current].code
	var flags := TextEdit.SEARCH_MATCH_CASE if _find_case.button_pressed else 0
	if not forward: flags |= TextEdit.SEARCH_BACKWARDS
	var line := code.get_caret_line()
	var column := code.get_caret_column()
	if not forward and code.has_selection(): column = code.get_selection_from_column()
	var found := code.search(_find_edit.text, flags, line, column)
	if found.x < 0: return
	code.select(found.y, found.x, found.y, found.x + _find_edit.text.length())
	code.set_caret_line(found.y)
	code.set_caret_column(found.x + (_find_edit.text.length() if forward else 0))
	code.center_viewport_to_caret()

# ------------------------------------------------------------------ diagnostics

func check(path: String) -> void:
	if not _scripts.has(path) or bridge == null: return
	var entry: Dictionary = _scripts[path]
	var code: CodeEdit = entry.code
	var info: Dictionary = bridge.compile_script(code.text, path)
	for line in code.get_line_count():
		if line != int(_runtime_error.get("line", 0)) - 1 or _runtime_error.get("path") != path:
			code.set_line_background_color(line, Color(0, 0, 0, 0))
	for warning in info.get("warnings", []):
		var line := int(warning.line) - 1
		if line >= 0 and line < code.get_line_count(): code.set_line_background_color(line, Color(Style.WARNING, 0.12))
	for error in info.get("errors", []):
		var line := int(error.line) - 1
		if line >= 0 and line < code.get_line_count(): code.set_line_background_color(line, Color(Style.ERROR, 0.22))
	entry.info = info
	_mark_connected(path)
	if path == _current:
		_update_status()
		_refresh_methods()
	_refresh_list()
	script_info_changed.emit(path, info)

var _connected := {} # script path -> {method: "Node.signal"}, from the scene being edited

## The methods of each script that the scene's connections call, marked in the gutter.
func set_connected_methods(methods: Dictionary) -> void:
	_connected = methods
	for path in _scripts: _mark_connected(path)

func _mark_connected(path: String) -> void:
	var code: CodeEdit = _scripts[path].code
	var marks: Dictionary = _connected.get(path, {})
	var lines := {}
	for f in _scripts[path].info.get("functions", []):
		if marks.has(str(f.name)): lines[int(f.line) - 1] = marks[str(f.name)]
	var slots: int = code.get_meta("slot_gutter")
	for line in code.get_line_count():
		code.set_line_gutter_icon(line, slots, Icons.get_icon("Slot") if lines.has(line) else null)

func info_of(path: String) -> Dictionary:
	return _scripts[path].info if _scripts.has(path) else {}

func _update_status() -> void:
	_status.remove_theme_color_override("font_color")
	if _current.is_empty():
		_status.text = ""
		_status.icon = null
		return
	var info: Dictionary = _scripts[_current].info
	if not _runtime_error.is_empty() and _runtime_error.path == _current:
		_status.text = "Runtime error (line %d): %s" % [_runtime_error.line, _runtime_error.message]
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

func _goto_status() -> void:
	if _current.is_empty(): return
	if not _runtime_error.is_empty() and _runtime_error.path == _current:
		goto_line(int(_runtime_error.line))
		return
	var info: Dictionary = _scripts[_current].info
	for key in ["errors", "warnings"]:
		if not info.get(key, []).is_empty():
			goto_line(int(info[key][0].line))
			return

## Highlights a runtime error line (like Godot's debugger break).
func show_runtime_error(path: String, line: int, text: String) -> void:
	if not open_script(path, line): return
	_runtime_error = {"path": path, "line": line, "message": text}
	var code: CodeEdit = _scripts[path].code
	if line - 1 < code.get_line_count(): code.set_line_background_color(line - 1, Color(Style.ERROR, 0.35))
	_update_status()

func clear_runtime_error() -> void:
	if _runtime_error.is_empty(): return
	var path: String = _runtime_error.path
	_runtime_error = {}
	if _scripts.has(path): check(path)
	_update_status()

## Reloads files changed outside the editor, e.g. in VS Code.
func check_external_changes() -> void:
	for path in _order:
		var file: String = project.to_abs(path)
		if not FileAccess.file_exists(file): continue
		var entry: Dictionary = _scripts[path]
		var mtime := FileAccess.get_modified_time(file)
		if mtime == entry.mtime: continue
		entry.mtime = mtime
		var text := FileAccess.get_file_as_string(file)
		if text == entry.saved: continue
		if not is_modified(path): _reload(path)
		elif not _reload_dialog.visible:
			_reload_path = path
			_reload_dialog.dialog_text = "%s was modified outside Jet Studio.\nReload it from disk?" % path
			_reload_dialog.popup_centered()

func _reload(path: String) -> void:
	if not _scripts.has(path): return
	var text := FileAccess.get_file_as_string(project.to_abs(path))
	var code: CodeEdit = _scripts[path].code
	var line := code.get_caret_line()
	code.text = text
	code.set_caret_line(mini(line, code.get_line_count() - 1))
	_scripts[path].saved = text
	check(path)

# ------------------------------------------------------------------ completion

func _on_completion(code: CodeEdit) -> void:
	var line := code.get_line(code.get_caret_line()).substr(0, code.get_caret_column())
	var word_start := line.length()
	while word_start > 0 and (line[word_start - 1] == "_" or line[word_start - 1].is_valid_identifier() or line[word_start - 1].is_valid_int()):
		word_start -= 1
	var before := line.substr(0, word_start)
	var info: Dictionary = _scripts[_current].info if not _current.is_empty() else {}
	var added := {}
	var add := func(kind: int, text: String, insert: String, color: Color, icon: String):
		if added.has(text): return
		added[text] = true
		code.add_code_completion_option(kind, text, insert, color, Icons.get_icon(icon))
	if before.ends_with("$"):
		for node_name in _scene_node_names: add.call(CodeEdit.KIND_NODE_PATH, node_name, node_name, Color("63c259"), "Node")
		code.update_code_completion_options(true)
		return
	if before.ends_with("."):
		var owner := before.trim_suffix(".")
		var owner_word := ""
		var i := owner.length() - 1
		while i >= 0 and (owner[i] == "_" or owner[i].is_valid_identifier() or owner[i].is_valid_int()):
			owner_word = owner[i] + owner_word
			i -= 1
		if MEMBERS.has(owner_word):
			for m in MEMBERS[owner_word]: add.call(CodeEdit.KIND_MEMBER, m, m, Color("bce0ff"), "MemberProperty")
		elif owner_word == "Input":
			for m in _class_methods("Input"): add.call(CodeEdit.KIND_FUNCTION, m + "()", m + "(", Color("57b3ff"), "MemberMethod")
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
	for c in _class_chain(base):
		for p in c.properties: add.call(CodeEdit.KIND_MEMBER, p.name, p.name, Color("bce0ff"), "MemberProperty")
		for m in c.methods: add.call(CodeEdit.KIND_FUNCTION, m.name + "()", m.name + "(", Color("57b3ff"), "MemberMethod")
		for sig in c.get("signals", []): add.call(CodeEdit.KIND_SIGNAL, sig.name, sig.name, Color("ffd760"), "Signal")
	for word in BUILTINS: add.call(CodeEdit.KIND_FUNCTION, word + "()", word + "(", Color("57b3ff"), "MemberMethod")
	for word in BASE_TYPES + ENGINE_TYPES: add.call(CodeEdit.KIND_CLASS, word, word, Color("42ffc2"), "Object")
	for word in KEYWORDS: add.call(CodeEdit.KIND_PLAIN_TEXT, word, word, Color("ff7085"), "")
	for word in ["_ready", "_process"]: add.call(CodeEdit.KIND_FUNCTION, word, word, Color("57b3ff"), "MemberMethod")
	code.update_code_completion_options(false)

var _scene_node_names := []
func set_scene_node_names(names: Array) -> void:
	_scene_node_names = names

func _class_methods(cls: String) -> Array:
	for c in class_db:
		if c.name == cls: return c.methods.map(func(m): return m.name)
	return []

func _class_chain(cls: String) -> Array:
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
