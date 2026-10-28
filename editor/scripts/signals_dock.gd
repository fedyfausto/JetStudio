extends VBoxContainer
## Godot's Node dock, Signals tab (here a tab of its own): the selected node's signals (its script's, then its classes') and their connections.
## Only emits requests; the editor changes the scene.

signal connect_requested(signal_name: String, arguments: Array)
signal disconnect_requested(index: int) # in the scene's connections
signal method_requested(target_id: int, method: String)

const Document := preload("res://scripts/scene_document.gd")
const Style := preload("res://scripts/theme.gd")
const Icons := preload("res://scripts/icons.gd")

var class_db := [] # from the bridge
## Whether a connection's method is there: Callable(target node, method) -> bool.
var method_exists: Callable

var _doc: Dictionary = {}
var _node: Dictionary = {}
var _read_only := false
var _header_icon: TextureRect
var _header_name: Label
var _tree: Tree
var _button: Button

func _ready() -> void:
	add_theme_constant_override("separation", 6)
	var header := HBoxContainer.new()
	add_child(header)
	_header_icon = TextureRect.new()
	_header_icon.custom_minimum_size = Vector2(18, 18)
	_header_icon.expand_mode = TextureRect.EXPAND_IGNORE_SIZE
	_header_icon.stretch_mode = TextureRect.STRETCH_KEEP_ASPECT_CENTERED
	header.add_child(_header_icon)
	_header_name = Label.new()
	_header_name.theme_type_variation = "HeaderLabel"
	_header_name.add_theme_font_size_override("font_size", 17)
	_header_name.add_theme_color_override("font_color", Style.ORANGE)
	_header_name.clip_text = true
	_header_name.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	header.add_child(_header_name)

	_tree = Tree.new()
	_tree.hide_root = true
	_tree.size_flags_vertical = Control.SIZE_EXPAND_FILL
	_tree.item_selected.connect(_update_button)
	_tree.item_activated.connect(_on_activated)
	add_child(_tree)
	_button = Button.new()
	_button.icon = Icons.get_icon("Signals")
	_button.pressed.connect(_on_button)
	add_child(_button)
	edit({}, {}, {}, false)

func edit(doc: Dictionary, node: Dictionary, script_info: Dictionary, locked: bool) -> void:
	_doc = doc
	_node = node
	_read_only = locked
	_tree.clear()
	_header_icon.texture = Icons.for_node(str(node.get("type", ""))) if not node.is_empty() else null
	_header_name.text = str(node.get("name", ""))
	if node.is_empty():
		_update_button()
		return
	var root := _tree.create_item()

	var script_path := str(node.get("script", ""))
	if not script_info.get("signals", []).is_empty():
		_section(root, script_path.get_file(), Icons.get_icon("Script"), script_info.signals)
	for cls in _class_chain(str(node.type)):
		if not cls.get("signals", []).is_empty(): _section(root, str(cls.name), Icons.for_node(str(cls.name)), cls.signals)
	if root.get_child_count() == 0:
		var none := _tree.create_item(root)
		none.set_text(0, "This node has no signals yet: declare one in its script (signal hit).")
		none.set_custom_color(0, Style.MUTED)
		none.set_selectable(0, false)
	_update_button()

func _section(root: TreeItem, title: String, icon: Texture2D, signals: Array) -> void:
	var section := _tree.create_item(root)
	section.set_text(0, title)
	section.set_icon(0, icon)
	section.set_selectable(0, false)
	section.set_custom_color(0, Style.MUTED)
	var list := Document.connections(_doc) if not _doc.is_empty() else []
	for sig in signals:
		var arguments: Array = sig.get("arguments", [])
		var item := _tree.create_item(section)
		item.set_text(0, "%s(%s)" % [sig.name, ", ".join(arguments)])
		item.set_icon(0, Icons.get_icon("Signal"))
		item.set_metadata(0, {"signal": str(sig.name), "arguments": arguments})
		item.set_tooltip_text(0, "Signal %s\nDouble-click or Connect... to call a method when it is emitted." % sig.name)
		for i in list.size():
			var c: Dictionary = list[i]
			if int(c.from) != int(_node.id) or str(c["signal"]) != str(sig.name): continue
			var target := Document.find(_doc.root, int(c.to))
			var link := _tree.create_item(item)
			var one_shot := int(c.get("flags", 0)) & 4 != 0
			link.set_text(0, "%s :: %s()%s" % [target.get("name", "?"), c.method, "  (one shot)" if one_shot else ""])
			link.set_icon(0, Icons.get_icon("Slot"))
			link.set_metadata(0, {"index": i, "target": int(c.to), "method": str(c.method)})
			if method_exists.is_valid() and not method_exists.call(target, str(c.method)):
				link.set_custom_color(0, Style.ERROR)
				link.set_tooltip_text(0, "%s has no method %s(): add it to its script, or Play reports the connection as an error." % [target.get("name", "?"), c.method])
			else:
				link.set_tooltip_text(0, "Calls %s() on %s. Double-click: go to the method." % [c.method, target.get("name", "?")])

func _class_chain(type: String) -> Array:
	var chain := []
	var current := type
	while not current.is_empty():
		var found := {}
		for c in class_db:
			if c.name == current: found = c
		if found.is_empty(): break
		chain.append(found)
		current = str(found.base)
	return chain

func _selected_meta() -> Dictionary:
	var item := _tree.get_selected()
	return item.get_metadata(0) if item != null and item.get_metadata(0) is Dictionary else {}

func _update_button() -> void:
	var meta := _selected_meta()
	var is_connection := meta.has("index")
	_button.text = "Disconnect" if is_connection else "Connect..."
	_button.icon = Icons.get_icon("Remove" if is_connection else "Signals")
	_button.disabled = _read_only or meta.is_empty()

func _on_button() -> void:
	var meta := _selected_meta()
	if _read_only or meta.is_empty(): return
	if meta.has("index"): disconnect_requested.emit(int(meta.index))
	else: connect_requested.emit(str(meta["signal"]), meta.arguments)

func _on_activated() -> void:
	var meta := _selected_meta()
	if meta.has("index"): method_requested.emit(int(meta.target), str(meta.method))
	elif not meta.is_empty() and not _read_only: connect_requested.emit(str(meta["signal"]), meta.arguments)
