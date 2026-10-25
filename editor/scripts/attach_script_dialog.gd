extends ConfirmationDialog

signal script_chosen(path: String, create: bool, base: String, template: bool)

const Style := preload("res://scripts/theme.gd")
const Icons := preload("res://scripts/icons.gd")

var project
var _base: LineEdit
var _template: OptionButton
var _path: LineEdit
var _status: Label

func _ready() -> void:
	cancel_button_text = "Cancel"
	title = "Attach Node Script"
	ok_button_text = "Create"
	min_size = Vector2i(520, 260)
	var box := VBoxContainer.new()
	box.add_theme_constant_override("separation", 10)
	add_child(box)
	var grid := GridContainer.new()
	grid.columns = 2
	grid.add_theme_constant_override("h_separation", 10)
	grid.add_theme_constant_override("v_separation", 8)
	box.add_child(grid)
	_field(grid, "Language:")
	var language := OptionButton.new()
	language.add_icon_item(Icons.get_icon("Script"), "JetScript")
	language.disabled = true
	language.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	grid.add_child(language)
	_field(grid, "Inherits:")
	_base = LineEdit.new()
	_base.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	_base.text_changed.connect(func(_t): _validate())
	grid.add_child(_base)
	_field(grid, "Template:")
	_template = OptionButton.new()
	_template.add_item("Node: Default (_ready and _process)")
	_template.add_item("Empty (extends only)")
	grid.add_child(_template)
	_field(grid, "Path:")
	_path = LineEdit.new()
	_path.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	_path.text_changed.connect(func(_t): _validate())
	grid.add_child(_path)
	register_text_enter(_path)
	_status = Label.new()
	_status.clip_text = true
	box.add_child(_status)
	confirmed.connect(func():
		var path := _path.text.strip_edges()
		script_chosen.emit(path, not project.exists(path), _base.text.strip_edges(), _template.selected == 0))

func _field(grid: GridContainer, text: String) -> void:
	var label := Label.new()
	label.text = text
	label.theme_type_variation = "PropertyLabel"
	grid.add_child(label)

func open(base: String, suggested_path: String) -> void:
	_base.text = base
	_path.text = suggested_path
	_template.select(0)
	_validate()
	popup_centered(Vector2i(540, 260))
	_path.grab_focus()
	var file := suggested_path.get_file()
	var start := suggested_path.length() - file.length()
	_path.select(start, start + file.get_basename().length())

func _validate() -> void:
	var path := _path.text.strip_edges()
	var ok := true
	var text := ""
	if not path.begins_with("res://") or path.get_extension() != "jet" or not path.get_file().get_basename().is_valid_filename() or path.get_file().begins_with("."):
		ok = false
		text = "Invalid path: use res://folder/name.jet"
	elif not _base.text.strip_edges().is_valid_identifier():
		ok = false
		text = "Invalid base class."
	elif project.exists(path):
		text = "File exists: the existing script will be attached."
	else:
		text = "A new script file will be created."
	_status.text = text
	_status.add_theme_color_override("font_color", Style.SUCCESS if ok else Style.ERROR)
	ok_button_text = "Load" if ok and project.exists(path) else "Create"
	get_ok_button().disabled = not ok
