extends AcceptDialog

signal open_requested(folder: String)
signal create_requested(folder: String, title: String)
signal forget_requested(folder: String)

const Style := preload("res://scripts/theme.gd")
const Icons := preload("res://scripts/icons.gd")
const Project := preload("res://scripts/project.gd")

var exampleFolder := ""
var _list: ItemList
var _openButton: Button
var _folderDialog: FileDialog
var _newDialog: ConfirmationDialog
var _newName: LineEdit
var _newParent: LineEdit
var _newPreview: Label
var _parentDialog: FileDialog


func _ready():
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
	_list.item_selected.connect(func(_i): _openButton.disabled = false)
	left.add_child(_list)
	var buttons := VBoxContainer.new()
	buttons.custom_minimum_size.x = 200
	row.add_child(buttons)
	_openButton = _button(buttons, "Load", "Open", func():
		var selected := _list.get_selected_items()
		if not selected.is_empty(): _open(str(_list.get_item_metadata(selected[0]))))
	_button(buttons, "New", "New Project...", _showNew)
	_button(buttons, "Folder", "Open Folder...", func(): _folderDialog.popup_centered_ratio(0.6))
	_button(buttons, "PackedScene", "Open Example Project", func(): _open(exampleFolder))
	buttons.add_child(HSeparator.new())
	_button(buttons, "Remove", "Remove from List", func():
		var selected := _list.get_selected_items()
		if not selected.is_empty(): forget_requested.emit(str(_list.get_item_metadata(selected[0]))))
	_folderDialog = FileDialog.new()
	_folderDialog.file_mode = FileDialog.FILE_MODE_OPEN_DIR
	_folderDialog.access = FileDialog.ACCESS_FILESYSTEM
	_folderDialog.title = "Open a Jet Studio Project Folder"
	_folderDialog.dir_selected.connect(_open)
	add_child(_folderDialog)
	_newDialog = ConfirmationDialog.new()
	_newDialog.cancel_button_text = "Cancel"
	_newDialog.title = "New Project"
	_newDialog.ok_button_text = "Create & Edit"
	var form := GridContainer.new()
	form.columns = 3
	form.add_theme_constant_override("h_separation", 8)
	form.add_theme_constant_override("v_separation", 8)
	_newDialog.add_child(form)
	var nameLabel := Label.new()
	nameLabel.text = "Project Name:"
	form.add_child(nameLabel)
	_newName = LineEdit.new()
	_newName.custom_minimum_size.x = 340
	_newName.text_changed.connect(func(_t): _updatePreview())
	form.add_child(_newName)
	form.add_child(Control.new())
	var parentLabel := Label.new()
	parentLabel.text = "Folder:"
	form.add_child(parentLabel)
	_newParent = LineEdit.new()
	_newParent.text_changed.connect(func(_t): _updatePreview())
	form.add_child(_newParent)
	var browse := Button.new()
	browse.icon = Icons.get_icon("Folder")
	browse.pressed.connect(func(): _parentDialog.popup_centered_ratio(0.6))
	form.add_child(browse)
	form.add_child(Control.new())
	_newPreview = Label.new()
	_newPreview.add_theme_color_override("font_color", Style.MUTED)
	form.add_child(_newPreview)
	_newDialog.register_text_enter(_newName)
	_newDialog.confirmed.connect(func(): create_requested.emit(_newFolder(), _newName.text.strip_edges()))
	add_child(_newDialog)
	_parentDialog = FileDialog.new()
	_parentDialog.file_mode = FileDialog.FILE_MODE_OPEN_DIR
	_parentDialog.access = FileDialog.ACCESS_FILESYSTEM
	_parentDialog.dir_selected.connect(func(dir): _newParent.text = dir; _updatePreview())
	add_child(_parentDialog)


func _button(parent: VBoxContainer, icon: String, text: String, action: Callable) -> Button:
	var button := Button.new()
	button.icon = Icons.get_icon(icon)
	button.text = text
	button.alignment = HORIZONTAL_ALIGNMENT_LEFT
	button.pressed.connect(action)
	parent.add_child(button)
	return button


func open(recent: Array):
	_list.clear()

	for folder in recent:
		var configName: String = str(folder).get_file()
		var data = JSON.parse_string(FileAccess.get_file_as_string(str(folder).path_join(Project.FILE)))

		if data is Dictionary: configName = str(data.get("name", configName))

		var index := _list.add_item("%s\n%s" % [configName, folder], Icons.get_icon("Tools"))
		_list.set_item_metadata(index, folder)

		if not Project.isProject(folder): _list.set_item_custom_fg_color(index, Style.DISABLED)

	_openButton.disabled = true
	popup_centered(Vector2i(780, 480))


func _open(folder: String):
	if not Project.isProject(folder):
		var alert := AcceptDialog.new()
		alert.dialog_text = "No project.jetproject in:\n" + folder
		add_child(alert)
		alert.popup_centered()
		return

	hide()
	open_requested.emit(folder)


func _showNew():
	_newName.text = "New Game"

	if _newParent.text.is_empty():
		var documents := OS.get_system_dir(OS.SYSTEM_DIR_DOCUMENTS)
		_newParent.text = documents.path_join("JetStudio") if not documents.is_empty() else OS.get_environment("HOME")

	_updatePreview()
	_newDialog.popup_centered(Vector2i(560, 170))
	_newName.grab_focus()
	_newName.select_all()


func _newFolder() -> String:
	var slug := _newName.text.strip_edges().to_lower().replace(" ", "_").validate_filename()
	return _newParent.text.strip_edges().path_join(slug)


func _updatePreview():
	var ok := not _newName.text.strip_edges().is_empty() and not _newParent.text.strip_edges().is_empty()
	_newPreview.text = "Will create: " + _newFolder() if ok else "Enter a name and a folder."
	_newDialog.get_ok_button().disabled = not ok
