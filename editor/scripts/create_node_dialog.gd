extends ConfirmationDialog

signal type_chosen(type: String)

const Document := preload("res://scripts/scene_document.gd")
const Style := preload("res://scripts/theme.gd")
const Icons := preload("res://scripts/icons.gd")

var _search: LineEdit
var _tree: Tree
var _description: RichTextLabel

func _ready() -> void:
	cancel_button_text = "Cancel"
	title = "Create New Node"
	ok_button_text = "Create"
	min_size = Vector2i(460, 520)
	var box := VBoxContainer.new()
	add_child(box)
	var label := Label.new()
	label.text = "Search:"
	box.add_child(label)
	_search = LineEdit.new()
	_search.placeholder_text = "Class name"
	_search.clear_button_enabled = true
	_search.right_icon = Icons.get_icon("Search")
	_search.text_changed.connect(func(_t): _fill())
	_search.gui_input.connect(_search_input)
	box.add_child(_search)
	register_text_enter(_search)
	var matches := Label.new()
	matches.text = "Matches:"
	box.add_child(matches)
	_tree = Tree.new()
	_tree.size_flags_vertical = Control.SIZE_EXPAND_FILL
	_tree.hide_root = true
	_tree.item_selected.connect(_on_selected)
	_tree.item_activated.connect(func(): _confirm())
	box.add_child(_tree)
	var title_label := Label.new()
	title_label.text = "Description:"
	box.add_child(title_label)
	# Fixed height, so a long description doesn't grow the dialog.
	_description = RichTextLabel.new()
	_description.custom_minimum_size = Vector2(0, 58)
	_description.scroll_active = false
	_description.add_theme_font_override("normal_font", Style.main_font)
	_description.add_theme_font_size_override("normal_font_size", 13)
	_description.add_theme_color_override("default_color", Style.MUTED)
	box.add_child(_description)
	confirmed.connect(_confirm)

## Also picks a new scene's root and a node's new type (Change Type, which starts on the current one).
func open(dialog_title: String = "Create New Node", ok_text: String = "Create", current_type: String = "") -> void:
	title = dialog_title
	ok_button_text = ok_text
	_search.text = ""
	_fill()
	if not current_type.is_empty():
		var item := _tree.get_root()
		while item != null and str(item.get_metadata(0)) != current_type: item = item.get_next_in_tree()
		if item != null:
			item.select(0)
			_tree.scroll_to_item(item)
	popup_centered(Vector2i(460, 560))
	_search.grab_focus()

func _children_of(type: String) -> Array:
	var out := []
	for t in Document.BASE:
		if Document.BASE[t] == type: out.append(t)
	return out

func _fill() -> void:
	_tree.clear()
	var root := _tree.create_item()
	var text := _search.text.strip_edges().to_lower()
	var first: TreeItem = null
	if text.is_empty():
		var stack := [["Node", root]]
		while not stack.is_empty():
			var item: Array = stack.pop_front()
			var node := _add(item[1], item[0])
			if first == null: first = node
			for child in _children_of(item[0]): stack.append([child, node])
	else:
		var best: TreeItem = null
		for type in Document.TYPES:
			if text in type.to_lower():
				var node := _add(root, type)
				if first == null: first = node
				if best == null and type.to_lower().begins_with(text): best = node
		if best: first = best
	if first:
		first.select(0)
		_tree.scroll_to_item(first)
	get_ok_button().disabled = first == null

func _add(parent: TreeItem, type: String) -> TreeItem:
	var item := _tree.create_item(parent)
	item.set_text(0, type)
	item.set_icon(0, Icons.for_node(type))
	item.set_metadata(0, type)
	item.set_tooltip_text(0, Document.DESCRIPTIONS.get(type, ""))
	if type in Document.ABSTRACT:
		# Abstract classes only group the tree, so they're greyed out like in Godot.
		item.set_selectable(0, false)
		item.set_custom_color(0, Style.MUTED)
	return item

func _on_selected() -> void:
	var item := _tree.get_selected()
	_description.text = Document.DESCRIPTIONS.get(str(item.get_metadata(0)), "") if item else ""
	get_ok_button().disabled = item == null

func _search_input(event: InputEvent) -> void:
	# Up/Down move through the list while you type in the search box, like Godot.
	if event is InputEventKey and event.pressed and event.keycode in [KEY_UP, KEY_DOWN]:
		var item := _tree.get_selected()
		var next: TreeItem = null
		if item: next = item.get_next_visible() if event.keycode == KEY_DOWN else item.get_prev_visible()
		if next:
			next.select(0)
			_tree.scroll_to_item(next)
		_search.accept_event()

func _confirm() -> void:
	var item := _tree.get_selected()
	if item == null: return
	hide()
	type_chosen.emit(str(item.get_metadata(0)))
