extends VBoxContainer

signal open_requested(path: String)
signal file_selected(path: String)
signal new_script_requested(folder: String)
signal new_scene_requested(folder: String)
signal file_moved(old_path: String, new_path: String)
signal file_removed(path: String)
signal main_scene_requested(path: String)
signal message(text: String, is_error: bool)

const Style := preload("res://scripts/theme.gd")
const Icons := preload("res://scripts/icons.gd")

enum { MENU_OPEN, MENU_NEW_SCRIPT, MENU_NEW_SCENE, MENU_NEW_FOLDER, MENU_RENAME, MENU_DUPLICATE, MENU_DELETE,
	MENU_COPY_PATH, MENU_SHOW, MENU_MAIN }

var project # project.gd instance
var tree: Tree
var _filter: LineEdit
var _menu: PopupMenu
var _name_dialog: ConfirmationDialog
var _name_edit: LineEdit
var _name_action := Callable()
var _delete_dialog: ConfirmationDialog
var _pending_delete := ""
var _expanded := {"res://": true}
var _selected := ""
var _refreshing := false

func _ready() -> void:
	var bar := HBoxContainer.new()
	add_child(bar)
	_filter = LineEdit.new()
	_filter.placeholder_text = "Filter Files"
	_filter.clear_button_enabled = true
	_filter.right_icon = Icons.get_icon("Search")
	_filter.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	_filter.text_changed.connect(func(_t): refresh())
	bar.add_child(_filter)
	var reload := Button.new()
	reload.theme_type_variation = "FlatButton"
	reload.icon = Icons.get_icon("Reload")
	reload.tooltip_text = "Rescan Filesystem"
	reload.pressed.connect(refresh)
	bar.add_child(reload)
	tree = Tree.new()
	tree.size_flags_vertical = Control.SIZE_EXPAND_FILL
	tree.allow_rmb_select = true
	tree.hide_root = false
	tree.item_activated.connect(_on_activated)
	tree.item_selected.connect(func():
		var item := tree.get_selected()
		if item:
			_selected = str(item.get_metadata(0))
			# Rebuilding the tree re-selects the item; that's not a user click.
			if not _refreshing: file_selected.emit(_selected))
	tree.item_collapsed.connect(func(item): _expanded[str(item.get_metadata(0))] = not item.collapsed)
	tree.item_mouse_selected.connect(func(pos, button):
		if button == MOUSE_BUTTON_RIGHT: _show_menu(tree.get_screen_position() + pos))
	tree.empty_clicked.connect(func(pos, button):
		if button == MOUSE_BUTTON_RIGHT:
			_selected = "res://"
			_show_menu(tree.get_screen_position() + pos))
	tree.set_drag_forwarding(_get_drag, Callable(), Callable())
	add_child(tree)
	_menu = PopupMenu.new()
	_menu.id_pressed.connect(_on_menu)
	add_child(_menu)
	_name_dialog = ConfirmationDialog.new()
	_name_dialog.cancel_button_text = "Cancel"
	_name_edit = LineEdit.new()
	_name_edit.custom_minimum_size.x = 320
	_name_dialog.add_child(_name_edit)
	_name_dialog.register_text_enter(_name_edit)
	_name_dialog.confirmed.connect(func(): if _name_action.is_valid(): _name_action.call(_name_edit.text.strip_edges()))
	add_child(_name_dialog)
	_delete_dialog = ConfirmationDialog.new()
	_delete_dialog.cancel_button_text = "Cancel"
	_delete_dialog.title = "Delete"
	_delete_dialog.ok_button_text = "Move to Trash"
	_delete_dialog.confirmed.connect(_delete_confirmed)
	add_child(_delete_dialog)

func _notification(what: int) -> void:
	if what == NOTIFICATION_APPLICATION_FOCUS_IN and project != null: refresh()

func refresh() -> void:
	tree.clear()
	if project == null or project.root.is_empty(): return
	var text := _filter.text.strip_edges().to_lower()
	var listing: Dictionary = project.scan()
	var root := tree.create_item()
	root.set_text(0, "res://")
	root.set_icon(0, Icons.get_icon("Folder"))
	root.set_icon_modulate(0, Style.ACCENT)
	root.set_metadata(0, "res://")
	_refreshing = true
	_add_children(root, listing, text)
	if _selected == "res://": root.select(0)
	_refreshing = false

func _add_children(parent: TreeItem, entry: Dictionary, filter: String) -> bool:
	var any := false
	for child in entry.get("children", []):
		var path: String = child.path
		var item := tree.create_item(parent)
		item.set_text(0, path.get_file())
		item.set_metadata(0, path)
		item.set_tooltip_text(0, path)
		if child.dir:
			item.set_icon(0, Icons.get_icon("Folder"))
			item.set_icon_modulate(0, Color("8db8ff"))
			var has := _add_children(item, child, filter)
			item.collapsed = not _expanded.get(path, false) and filter.is_empty()
			if not filter.is_empty() and not has and not filter in path.get_file().to_lower():
				item.free()
				continue
			any = true
		else:
			# .import files stay hidden: their settings are edited in the Inspector, like in Godot.
			if path.get_extension() == "import" or (not filter.is_empty() and not filter in path.get_file().to_lower()):
				item.free()
				continue
			item.set_icon(0, Icons.for_file(path))
			if project != null and path == project.main_scene():
				item.set_custom_color(0, Style.ACCENT)
				item.set_tooltip_text(0, path + "\nMain Scene")
			any = true
		if path == _selected: item.select(0)
	return any

func _on_activated() -> void:
	var item := tree.get_selected()
	if item == null: return
	var path := str(item.get_metadata(0))
	if DirAccess.dir_exists_absolute(project.to_abs(path)):
		item.collapsed = not item.collapsed
	else:
		open_requested.emit(path)

func _folder_of(path: String) -> String:
	if path.is_empty(): return "res://"
	if DirAccess.dir_exists_absolute(project.to_abs(path)): return path
	return path.get_base_dir()

func _show_menu(at: Vector2) -> void:
	if project == null: return
	var is_dir := DirAccess.dir_exists_absolute(project.to_abs(_selected))
	var is_root := _selected == "res://"
	_menu.clear()
	if not is_dir:
		_menu.add_icon_item(Icons.get_icon("Load"), "Open", MENU_OPEN)
		if _selected.get_extension() == "jetscene": _menu.add_icon_item(Icons.get_icon("PlayScene"), "Set as Main Scene", MENU_MAIN)
		_menu.add_separator()
	_menu.add_icon_item(Icons.get_icon("ScriptCreate"), "New Script...", MENU_NEW_SCRIPT)
	_menu.add_icon_item(Icons.get_icon("PackedScene"), "New Scene...", MENU_NEW_SCENE)
	_menu.add_icon_item(Icons.get_icon("FolderCreate"), "New Folder...", MENU_NEW_FOLDER)
	if not is_root:
		_menu.add_separator()
		_menu.add_icon_item(Icons.get_icon("Rename"), "Rename...", MENU_RENAME)
		if not is_dir: _menu.add_icon_item(Icons.get_icon("Duplicate"), "Duplicate...", MENU_DUPLICATE)
		_menu.add_icon_item(Icons.get_icon("Remove"), "Delete", MENU_DELETE)
	_menu.add_separator()
	_menu.add_icon_item(Icons.get_icon("CopyNodePath"), "Copy Path", MENU_COPY_PATH)
	_menu.add_icon_item(Icons.get_icon("ShowInFileSystem"), "Open in File Manager", MENU_SHOW)
	_menu.position = Vector2i(at)
	_menu.reset_size()
	_menu.popup()

func _ask_name(title: String, initial: String, action: Callable) -> void:
	_name_dialog.title = title
	_name_edit.text = initial
	_name_action = action
	_name_dialog.popup_centered()
	_name_edit.grab_focus()
	var dot := initial.rfind(".")
	_name_edit.select(0, dot if dot > 0 else initial.length())

func _on_menu(id: int) -> void:
	var folder := _folder_of(_selected)
	match id:
		MENU_OPEN: open_requested.emit(_selected)
		MENU_MAIN: main_scene_requested.emit(_selected)
		MENU_NEW_SCRIPT: new_script_requested.emit(folder)
		MENU_NEW_SCENE: new_scene_requested.emit(folder)
		MENU_NEW_FOLDER:
			_ask_name("Create Folder", "new_folder", func(text):
				if not _valid_file_name(text): return
				var path := folder.path_join(text)
				if DirAccess.make_dir_recursive_absolute(project.to_abs(path)) != OK: message.emit("Could not create " + path, true)
				_expanded[folder] = true
				refresh())
		MENU_RENAME:
			var old := _selected
			_ask_name("Rename", old.get_file(), func(text):
				if not _valid_file_name(text): return
				var target := old.get_base_dir().path_join(text)
				if target == old: return
				if project.exists(target):
					message.emit("Already exists: " + target, true)
					return
				if DirAccess.rename_absolute(project.to_abs(old), project.to_abs(target)) != OK:
					message.emit("Rename failed: " + old, true)
					return
				if FileAccess.file_exists(project.to_abs(old) + ".import"):
					DirAccess.rename_absolute(project.to_abs(old) + ".import", project.to_abs(target) + ".import")
				_selected = target
				file_moved.emit(old, target)
				refresh())
		MENU_DUPLICATE:
			var old := _selected
			var base := old.get_file().get_basename()
			_ask_name("Duplicate", base + "_copy." + old.get_extension(), func(text):
				if not _valid_file_name(text): return
				var target := old.get_base_dir().path_join(text)
				if project.exists(target):
					message.emit("Already exists: " + target, true)
					return
				if DirAccess.copy_absolute(project.to_abs(old), project.to_abs(target)) != OK: message.emit("Copy failed.", true)
				elif FileAccess.file_exists(project.to_abs(old) + ".import"):
					DirAccess.copy_absolute(project.to_abs(old) + ".import", project.to_abs(target) + ".import")
				refresh())
		MENU_DELETE:
			_pending_delete = _selected
			_delete_dialog.dialog_text = "Move \"%s\" to the trash?\nReferences to this file in scenes will stop working." % _selected
			_delete_dialog.popup_centered()
		MENU_COPY_PATH: DisplayServer.clipboard_set(_selected)
		MENU_SHOW: OS.shell_show_in_file_manager(project.to_abs(_selected))

func _valid_file_name(text: String) -> bool:
	if text.is_empty() or text.begins_with(".") or not text.is_valid_filename():
		message.emit("Invalid name: " + text, true)
		return false
	return true

func _delete_confirmed() -> void:
	var path := _pending_delete
	if path.is_empty() or path == "res://": return
	var error := OS.move_to_trash(project.to_abs(path))
	if error != OK:
		message.emit("Delete failed: " + path, true)
		return
	if FileAccess.file_exists(project.to_abs(path) + ".import"): OS.move_to_trash(project.to_abs(path) + ".import")
	file_removed.emit(path)
	_selected = path.get_base_dir()
	refresh()

func _get_drag(at: Vector2) -> Variant:
	var item := tree.get_item_at_position(at)
	if item == null: return null
	var path := str(item.get_metadata(0))
	if path == "res://": return null
	var preview := HBoxContainer.new()
	var icon := TextureRect.new()
	icon.texture = Icons.for_file(path)
	icon.custom_minimum_size = Vector2(16, 16)
	icon.expand_mode = TextureRect.EXPAND_IGNORE_SIZE
	preview.add_child(icon)
	var label := Label.new()
	label.text = path.get_file()
	preview.add_child(label)
	tree.set_drag_preview(preview)
	return {"type": "files", "files": [path]}
