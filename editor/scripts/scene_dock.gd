extends VBoxContainer

signal node_selected(id: int)
signal add_requested(parent_id: int)
signal rename_requested(id: int, new_name: String)
signal move_requested(id: int, parent_id: int, index: int)
signal delete_requested(id: int)
signal duplicate_requested(id: int)
signal script_requested(id: int, action: String)
signal visibility_toggled(id: int)
signal copy_path_requested(id: int)
signal file_dropped(id: int, path: String)
signal reorder_requested(id: int, delta: int)
signal signals_requested(id: int)
signal change_type_requested(id: int)

const Document := preload("res://scripts/scene_document.gd")
const Style := preload("res://scripts/theme.gd")
const Icons := preload("res://scripts/icons.gd")

enum { BUTTON_SCRIPT = 1, BUTTON_VISIBLE = 2, BUTTON_WARNING = 3, BUTTON_SIGNALS = 4 }
enum { MENU_ADD, MENU_RENAME, MENU_DUPLICATE, MENU_UP, MENU_DOWN, MENU_ATTACH, MENU_DETACH, MENU_PATH, MENU_DELETE, MENU_CHANGE_TYPE }

var tree: Tree
var read_only := false
var connections := [] # the scene's signal connections, for the icon of the nodes that emit them
var _root: Dictionary = {}
var _selected := 0
var _collapsed := {}
var _rebuilding := false
var _filter: LineEdit
var _add_button: Button
var _script_button: Button
var _menu: PopupMenu
var _warning_dialog: AcceptDialog

func _ready() -> void:
	var bar := HBoxContainer.new()
	add_child(bar)
	_add_button = _tool_button("Add", "Add Child Node (Ctrl+A)", func(): add_requested.emit(_selected if _selected > 0 else _root_id()))
	bar.add_child(_add_button)
	_filter = LineEdit.new()
	_filter.placeholder_text = "Filter Nodes"
	_filter.clear_button_enabled = true
	_filter.right_icon = Icons.get_icon("Search")
	_filter.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	_filter.text_changed.connect(func(_t): _apply_filter())
	bar.add_child(_filter)
	_script_button = _tool_button("ScriptCreate", "Attach a new or existing script to the selected node.", func():
		if _selected > 0:
			var node := Document.find(_root, _selected)
			script_requested.emit(_selected, "detach" if not str(node.get("script", "")).is_empty() else "attach"))
	bar.add_child(_script_button)
	tree = Tree.new()
	tree.size_flags_vertical = Control.SIZE_EXPAND_FILL
	tree.allow_rmb_select = true
	tree.select_mode = Tree.SELECT_SINGLE
	tree.item_selected.connect(_on_selected)
	tree.item_edited.connect(_on_edited)
	tree.item_activated.connect(_begin_rename)
	tree.button_clicked.connect(_on_button)
	tree.item_collapsed.connect(func(item):
		if not _rebuilding: _collapsed[int(item.get_metadata(0))] = item.collapsed)
	tree.item_mouse_selected.connect(func(pos, button):
		if button == MOUSE_BUTTON_RIGHT: _show_menu(tree.get_screen_position() + pos))
	tree.empty_clicked.connect(func(pos, button):
		if button == MOUSE_BUTTON_RIGHT and not _root.is_empty():
			_select_id(_root_id())
			_show_menu(tree.get_screen_position() + pos))
	tree.set_drag_forwarding(_get_drag, _can_drop, _drop)
	tree.gui_input.connect(_tree_input)
	add_child(tree)
	_menu = PopupMenu.new()
	_menu.id_pressed.connect(_on_menu)
	add_child(_menu)
	_warning_dialog = AcceptDialog.new()
	_warning_dialog.title = "Node configuration warnings"
	add_child(_warning_dialog)

func _tool_button(icon: String, tooltip: String, action: Callable) -> Button:
	var button := Button.new()
	button.theme_type_variation = "FlatButton"
	button.icon = Icons.get_icon(icon)
	button.tooltip_text = tooltip
	button.pressed.connect(action)
	return button

func _root_id() -> int:
	return int(_root.id) if not _root.is_empty() else 0

func rebuild(root: Dictionary, selected: int, locked: bool) -> void:
	_root = root
	_selected = selected
	read_only = locked
	_rebuilding = true
	tree.clear()
	_add_button.disabled = locked or root.is_empty()
	_script_button.disabled = locked or selected <= 0
	if root.is_empty():
		_rebuilding = false
		return
	var items := {}
	Document.walk(root, func(node, parent, _depth):
		var item := tree.create_item(items.get(int(parent.id), null) if not parent.is_empty() else null)
		items[int(node.id)] = item
		var id := int(node.id)
		item.set_text(0, str(node.name))
		item.set_icon(0, Icons.for_node(str(node.type)))
		item.set_metadata(0, id)
		item.set_tooltip_text(0, "%s (%s)\nType: %s" % [node.name, Document.relative_path(root, int(root.id), id), node.type])
		var warnings := Document.configuration_warnings(root, node)
		if not warnings.is_empty():
			item.add_button(0, Icons.get_icon("NodeWarning"), BUTTON_WARNING, false, "\n\n".join(warnings))
		var outgoing := connections.filter(func(c): return int(c.from) == id)
		if not outgoing.is_empty():
			var lines := outgoing.map(func(c):
				var target := Document.find(root, int(c.to))
				return "%s → %s :: %s()" % [c["signal"], target.get("name", "?"), c.method])
			item.add_button(0, Icons.get_icon("Signals"), BUTTON_SIGNALS, false, "Node has connections:\n" + "\n".join(lines))
		var script_path := str(node.get("script", ""))
		if not script_path.is_empty():
			item.add_button(0, Icons.get_icon("Script"), BUTTON_SCRIPT, false, "Open Script: " + script_path)
		if not Document.property_def(str(node.type), "visible").is_empty():
			var shown := bool(node.get("visible", true))
			item.add_button(0, Icons.get_icon("GuiVisibilityVisible" if shown else "GuiVisibilityHidden"), BUTTON_VISIBLE, locked,
				"Visible" if shown else "Hidden")
			if not shown: item.set_custom_color(0, Style.MUTED)
		item.collapsed = _collapsed.get(id, false)
		if id == selected:
			item.select(0)
			tree.scroll_to_item(item))
	_rebuilding = false
	_apply_filter()
	_update_script_button()

func _update_script_button() -> void:
	var node := Document.find(_root, _selected) if _selected > 0 else {}
	var has_script := not node.is_empty() and not str(node.get("script", "")).is_empty()
	_script_button.icon = Icons.get_icon("ScriptRemove" if has_script else "ScriptCreate")
	_script_button.tooltip_text = "Detach the script from the selected node." if has_script else "Attach a new or existing script to the selected node."
	_script_button.disabled = read_only or node.is_empty()

func select(id: int) -> void:
	_selected = id
	_rebuilding = true
	var item := _find_item(tree.get_root(), id)
	if item:
		var parent := item.get_parent()
		while parent:
			parent.collapsed = false
			parent = parent.get_parent()
		item.select(0)
		tree.scroll_to_item(item)
	else:
		tree.deselect_all()
	_rebuilding = false
	_update_script_button()

func _select_id(id: int) -> void:
	select(id)
	node_selected.emit(id)

func _find_item(item: TreeItem, id: int) -> TreeItem:
	if item == null: return null
	if int(item.get_metadata(0)) == id: return item
	for child in item.get_children():
		var found := _find_item(child, id)
		if found: return found
	return null

func _on_selected() -> void:
	if _rebuilding: return
	var item := tree.get_selected()
	if item == null: return
	_selected = int(item.get_metadata(0))
	_update_script_button()
	node_selected.emit(_selected)

func _begin_rename() -> void:
	if read_only: return
	var item := tree.get_selected()
	if item == null: return
	item.set_editable(0, true)
	tree.edit_selected(true)

func _on_edited() -> void:
	var item := tree.get_edited()
	if item == null: return
	item.set_editable(0, false)
	var id := int(item.get_metadata(0))
	var text := item.get_text(0).strip_edges()
	rename_requested.emit(id, text)

func _on_button(item: TreeItem, _column: int, id: int, _mouse: int) -> void:
	var node_id := int(item.get_metadata(0))
	if id == BUTTON_SCRIPT: script_requested.emit(node_id, "open")
	elif id == BUTTON_SIGNALS: signals_requested.emit(node_id)
	elif id == BUTTON_VISIBLE and not read_only: visibility_toggled.emit(node_id)
	elif id == BUTTON_WARNING:
		var node := Document.find(_root, node_id)
		if node.is_empty(): return
		_warning_dialog.dialog_text = "\n\n".join(Document.configuration_warnings(_root, node))
		_warning_dialog.popup_centered()

func _show_menu(at: Vector2) -> void:
	if _selected <= 0: return
	var node := Document.find(_root, _selected)
	if node.is_empty(): return
	var is_root := _selected == _root_id()
	var has_script := not str(node.get("script", "")).is_empty()
	_menu.clear()
	_menu.add_icon_item(Icons.get_icon("Add"), "Add Child Node...  (Ctrl+A)", MENU_ADD)
	_menu.add_separator()
	_menu.add_icon_item(Icons.get_icon("Rename"), "Rename  (F2)", MENU_RENAME)
	_menu.add_icon_item(Icons.get_icon("Duplicate"), "Duplicate  (Ctrl+D)", MENU_DUPLICATE)
	_menu.add_icon_item(Icons.get_icon("MoveUp"), "Move Up  (Ctrl+↑)", MENU_UP)
	_menu.add_icon_item(Icons.get_icon("MoveDown"), "Move Down  (Ctrl+↓)", MENU_DOWN)
	_menu.add_separator()
	_menu.add_icon_item(Icons.get_icon("Reload"), "Change Type...", MENU_CHANGE_TYPE)
	_menu.add_icon_item(Icons.get_icon("ScriptCreate"), "Attach Script...", MENU_ATTACH)
	if has_script: _menu.add_icon_item(Icons.get_icon("ScriptRemove"), "Detach Script", MENU_DETACH)
	_menu.add_icon_item(Icons.get_icon("CopyNodePath"), "Copy Node Path  (Ctrl+Shift+C)", MENU_PATH)
	_menu.add_separator()
	_menu.add_icon_item(Icons.get_icon("Remove"), "Delete Node  (Del)", MENU_DELETE)
	for id in [MENU_RENAME, MENU_DUPLICATE, MENU_UP, MENU_DOWN, MENU_DELETE]:
		_menu.set_item_disabled(_menu.get_item_index(id), read_only or is_root)
	for id in [MENU_ADD, MENU_ATTACH, MENU_DETACH, MENU_CHANGE_TYPE]:
		var index := _menu.get_item_index(id)
		if index >= 0: _menu.set_item_disabled(index, read_only)
	_menu.set_item_disabled(_menu.get_item_index(MENU_RENAME), read_only)
	_menu.position = Vector2i(at)
	_menu.reset_size()
	_menu.popup()

func _on_menu(id: int) -> void:
	match id:
		MENU_ADD: add_requested.emit(_selected)
		MENU_RENAME: _begin_rename()
		MENU_DUPLICATE: duplicate_requested.emit(_selected)
		MENU_UP: reorder_requested.emit(_selected, -1)
		MENU_DOWN: reorder_requested.emit(_selected, 1)
		MENU_ATTACH: script_requested.emit(_selected, "attach")
		MENU_DETACH: script_requested.emit(_selected, "detach")
		MENU_PATH: copy_path_requested.emit(_selected)
		MENU_DELETE: delete_requested.emit(_selected)
		MENU_CHANGE_TYPE: change_type_requested.emit(_selected)

func _tree_input(event: InputEvent) -> void:
	if not event is InputEventKey or not event.pressed or event.echo or read_only: return
	var handled := true
	if event.keycode == KEY_DELETE and _selected > 0: delete_requested.emit(_selected)
	elif event.keycode == KEY_F2: _begin_rename()
	elif event.ctrl_pressed and event.keycode == KEY_D and _selected > 0: duplicate_requested.emit(_selected)
	elif event.ctrl_pressed and event.keycode == KEY_A: add_requested.emit(_selected if _selected > 0 else _root_id())
	elif event.ctrl_pressed and event.keycode == KEY_UP: reorder_requested.emit(_selected, -1)
	elif event.ctrl_pressed and event.keycode == KEY_DOWN: reorder_requested.emit(_selected, 1)
	elif event.ctrl_pressed and event.shift_pressed and event.keycode == KEY_C: copy_path_requested.emit(_selected)
	else: handled = false
	if handled: tree.accept_event()

func _get_drag(at: Vector2) -> Variant:
	if read_only: return null
	var item := tree.get_item_at_position(at)
	if item == null: return null
	var id := int(item.get_metadata(0))
	if id == _root_id(): return null
	var preview := Label.new()
	preview.text = "  " + item.get_text(0)
	preview.add_theme_color_override("font_color", Style.ACCENT)
	tree.set_drag_preview(preview)
	return {"type": "nodes", "ids": [id]}

func _can_drop(at: Vector2, data: Variant) -> bool:
	if read_only or not data is Dictionary: return false
	var item := tree.get_item_at_position(at)
	if item == null: return false
	if data.get("type") == "files":
		tree.drop_mode_flags = Tree.DROP_MODE_ON_ITEM
		for path in data.get("files", []):
			if str(path).get_extension().to_lower() in ["jet", "png", "obj"]: return true
		return false
	if data.get("type") != "nodes": return false
	tree.drop_mode_flags = Tree.DROP_MODE_ON_ITEM | Tree.DROP_MODE_INBETWEEN
	var target := int(item.get_metadata(0))
	var section := tree.get_drop_section_at_position(at)
	for id in data.ids:
		if id == target or Document.is_ancestor(_root, int(id), target): return false
	if target == _root_id() and section != 0: return false
	return true

func _drop(at: Vector2, data: Variant) -> void:
	var item := tree.get_item_at_position(at)
	if item == null: return
	var target := int(item.get_metadata(0))
	if data.get("type") == "files":
		for path in data.files:
			if str(path).get_extension().to_lower() in ["jet", "png", "obj"]:
				file_dropped.emit(target, str(path))
				return
		return
	var section := tree.get_drop_section_at_position(at)
	for id in data.ids:
		if section == 0:
			move_requested.emit(int(id), target, -1)
		else:
			var parent := Document.parent_of(_root, target)
			if parent.is_empty(): return
			var index := Document.children(parent).find(Document.find(_root, target))
			move_requested.emit(int(id), int(parent.id), index + (1 if section > 0 else 0))

func _apply_filter() -> void:
	var text := _filter.text.strip_edges().to_lower()
	if tree.get_root() == null: return
	_filter_item(tree.get_root(), text)

func _filter_item(item: TreeItem, text: String) -> bool:
	var any := text.is_empty() or text in item.get_text(0).to_lower()
	for child in item.get_children():
		if _filter_item(child, text): any = true
	item.visible = any
	return any
