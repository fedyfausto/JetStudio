extends AcceptDialog

signal open_requested(folder: String)
signal create_requested(folder: String, title: String)
signal forget_requested(folder: String)

const Style := preload("res://scripts/theme.gd")
const Icons := preload("res://scripts/icons.gd")
const Project := preload("res://scripts/project.gd")

var example_folder := ""
var _list: ItemList
var _open_button: Button
var _folder_dialog: FileDialog
var _new_dialog: ConfirmationDialog
var _new_name: LineEdit
var _new_parent: LineEdit
var _new_preview: Label
var _parent_dialog: FileDialog

func _ready() -> void:
	title = "Jet Studio · Project Manager"
	ok_button_text = "Close"
	min_size = Vector2i(760, 460)
	var row := HBoxContainer.new()
	row.add_theme_constant_override("separation", 10)
	add_child(row)
	var left := VBoxContainer.new()
	left.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	row.add_child(left)
	var heading := Label.new()
	heading.text = "Recent Projects"
	heading.theme_type_variation = "DockTitle"
	left.add_child(heading)
	_list = ItemList.new()
	_list.size_flags_vertical = Control.SIZE_EXPAND_FILL
	_list.item_activated.connect(func(i): _open(str(_list.get_item_metadata(i))))
	_list.item_selected.connect(func(_i): _open_button.disabled = false)
	left.add_child(_list)
	var buttons := VBoxContainer.new()
	buttons.custom_minimum_size.x = 200
	row.add_child(buttons)
	_open_button = _button(buttons, "Load", "Open", func():
		var selected := _list.get_selected_items()
		if not selected.is_empty(): _open(str(_list.get_item_metadata(selected[0]))))
	_button(buttons, "New", "New Project...", _show_new)
	_button(buttons, "Folder", "Open Folder...", func(): _folder_dialog.popup_centered_ratio(0.6))
	_button(buttons, "PackedScene", "Open Example Project", func(): _open(example_folder))
	buttons.add_child(HSeparator.new())
	_button(buttons, "Remove", "Remove from List", func():
		var selected := _list.get_selected_items()
		if not selected.is_empty(): forget_requested.emit(str(_list.get_item_metadata(selected[0]))))
	_folder_dialog = FileDialog.new()
	_folder_dialog.file_mode = FileDialog.FILE_MODE_OPEN_DIR
	_folder_dialog.access = FileDialog.ACCESS_FILESYSTEM
	_folder_dialog.title = "Open a Jet Studio Project Folder"
	_folder_dialog.dir_selected.connect(_open)
	add_child(_folder_dialog)
	_new_dialog = ConfirmationDialog.new()
	_new_dialog.cancel_button_text = "Cancel"
	_new_dialog.title = "New Project"
	_new_dialog.ok_button_text = "Create & Edit"
	var form := GridContainer.new()
	form.columns = 3
	form.add_theme_constant_override("h_separation", 8)
	form.add_theme_constant_override("v_separation", 8)
	_new_dialog.add_child(form)
	var name_label := Label.new()
	name_label.text = "Project Name:"
	form.add_child(name_label)
	_new_name = LineEdit.new()
	_new_name.custom_minimum_size.x = 340
	_new_name.text_changed.connect(func(_t): _update_preview())
	form.add_child(_new_name)
	form.add_child(Control.new())
	var parent_label := Label.new()
	parent_label.text = "Folder:"
	form.add_child(parent_label)
	_new_parent = LineEdit.new()
	_new_parent.text_changed.connect(func(_t): _update_preview())
	form.add_child(_new_parent)
	var browse := Button.new()
	browse.icon = Icons.get_icon("Folder")
	browse.pressed.connect(func(): _parent_dialog.popup_centered_ratio(0.6))
	form.add_child(browse)
	form.add_child(Control.new())
	_new_preview = Label.new()
	_new_preview.add_theme_color_override("font_color", Style.MUTED)
	form.add_child(_new_preview)
	_new_dialog.register_text_enter(_new_name)
	_new_dialog.confirmed.connect(func(): create_requested.emit(_new_folder(), _new_name.text.strip_edges()))
	add_child(_new_dialog)
	_parent_dialog = FileDialog.new()
	_parent_dialog.file_mode = FileDialog.FILE_MODE_OPEN_DIR
	_parent_dialog.access = FileDialog.ACCESS_FILESYSTEM
	_parent_dialog.dir_selected.connect(func(dir): _new_parent.text = dir; _update_preview())
	add_child(_parent_dialog)

func _button(parent: VBoxContainer, icon: String, text: String, action: Callable) -> Button:
	var button := Button.new()
	button.icon = Icons.get_icon(icon)
	button.text = text
	button.alignment = HORIZONTAL_ALIGNMENT_LEFT
	button.pressed.connect(action)
	parent.add_child(button)
	return button

func open(recent: Array) -> void:
	_list.clear()
	for folder in recent:
		var config_name: String = str(folder).get_file()
		var data = JSON.parse_string(FileAccess.get_file_as_string(str(folder).path_join(Project.FILE)))
		if data is Dictionary: config_name = str(data.get("name", config_name))
		var index := _list.add_item("%s\n%s" % [config_name, folder], Icons.get_icon("Tools"))
		_list.set_item_metadata(index, folder)
		if not Project.is_project(folder): _list.set_item_custom_fg_color(index, Style.DISABLED)
	_open_button.disabled = true
	popup_centered(Vector2i(780, 480))

func _open(folder: String) -> void:
	if not Project.is_project(folder):
		var alert := AcceptDialog.new()
		alert.dialog_text = "No project.jetproject in:\n" + folder
		add_child(alert)
		alert.popup_centered()
		return
	hide()
	open_requested.emit(folder)

func _show_new() -> void:
	_new_name.text = "New Game"
	if _new_parent.text.is_empty():
		var documents := OS.get_system_dir(OS.SYSTEM_DIR_DOCUMENTS)
		_new_parent.text = documents.path_join("JetStudio") if not documents.is_empty() else OS.get_environment("HOME")
	_update_preview()
	_new_dialog.popup_centered(Vector2i(560, 170))
	_new_name.grab_focus()
	_new_name.select_all()

func _new_folder() -> String:
	var slug := _new_name.text.strip_edges().to_lower().replace(" ", "_").validate_filename()
	return _new_parent.text.strip_edges().path_join(slug)

func _update_preview() -> void:
	var ok := not _new_name.text.strip_edges().is_empty() and not _new_parent.text.strip_edges().is_empty()
	_new_preview.text = "Will create: " + _new_folder() if ok else "Enter a name and a folder."
	_new_dialog.get_ok_button().disabled = not ok
