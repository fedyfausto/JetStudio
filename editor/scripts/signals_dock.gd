extends VBoxContainer
# Godot's Node dock, Signals tab (here a tab of its own): the selected node's signals (its script's, then its classes') and their connections.
# Only emits requests; the editor changes the scene.

signal connect_requested(signalName: String, arguments: Array)
# in the scene's connections
signal disconnect_requested(index: int)
signal method_requested(targetId: int, method: String)

const Document := preload("res://scripts/scene_document.gd")
const Style := preload("res://scripts/theme.gd")
const Icons := preload("res://scripts/icons.gd")

# from the bridge
var class_db := []
# Whether a connection's method is there: Callable(target node, method) -> bool.
var methodExists: Callable

var _doc: Dictionary = {}
var _node: Dictionary = {}
var _readOnly := false
var _headerIcon: TextureRect
var _headerName: Label
var _tree: Tree
var _button: Button


func _ready():
	add_theme_constant_override("separation", 6)
	var header := HBoxContainer.new()
	add_child(header)
	_headerIcon = TextureRect.new()
	_headerIcon.custom_minimum_size = Vector2(18, 18)
	_headerIcon.expand_mode = TextureRect.EXPAND_IGNORE_SIZE
	_headerIcon.stretch_mode = TextureRect.STRETCH_KEEP_ASPECT_CENTERED
	header.add_child(_headerIcon)
	_headerName = Label.new()
	_headerName.theme_type_variation = "HeaderLabel"
	_headerName.add_theme_font_size_override("font_size", 17)
	_headerName.add_theme_color_override("font_color", Style.ORANGE)
	_headerName.clip_text = true
	_headerName.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	header.add_child(_headerName)

	_tree = Tree.new()
	_tree.hide_root = true
	_tree.size_flags_vertical = Control.SIZE_EXPAND_FILL
	_tree.item_selected.connect(_updateButton)
	_tree.item_activated.connect(_onActivated)
	add_child(_tree)
	_button = Button.new()
	_button.icon = Icons.get_icon("Signals")
	_button.pressed.connect(_onButton)
	add_child(_button)
	edit({}, {}, {}, false)


func edit(doc: Dictionary, node: Dictionary, scriptInfo: Dictionary, locked: bool):
	_doc = doc
	_node = node
	_readOnly = locked
	_tree.clear()
	_headerIcon.texture = Icons.forNode(str(node.get("type", ""))) if not node.is_empty() else null
	_headerName.text = str(node.get("name", ""))

	if node.is_empty():
		_updateButton()
		return

	var root := _tree.create_item()

	var scriptPath := str(node.get("script", ""))

	if not scriptInfo.get("signals", []).is_empty():
		_section(root, scriptPath.get_file(), Icons.get_icon("Script"), scriptInfo.signals)

	for cls in _classChain(str(node.type)):
		if not cls.get("signals", []).is_empty(): _section(root, str(cls.name), Icons.forNode(str(cls.name)), cls.signals)

	if root.get_child_count() == 0:
		var none := _tree.create_item(root)
		none.set_text(0, "This node has no signals yet: declare one in its script (signal hit).")
		none.set_custom_color(0, Style.MUTED)
		none.set_selectable(0, false)

	_updateButton()


func _section(root: TreeItem, title: String, icon: Texture2D, signals: Array):
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

			if methodExists.is_valid() and not methodExists.call(target, str(c.method)):
				link.set_custom_color(0, Style.ERROR)
				link.set_tooltip_text(0, "%s has no method %s(): add it to its script, or Play reports the connection as an error." % [target.get("name", "?"), c.method])
			else:
				link.set_tooltip_text(0, "Calls %s() on %s. Double-click: go to the method." % [c.method, target.get("name", "?")])


func _classChain(type: String) -> Array:
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


func _selectedMeta() -> Dictionary:
	var item := _tree.get_selected()
	return item.get_metadata(0) if item != null and item.get_metadata(0) is Dictionary else {}


func _updateButton():
	var meta := _selectedMeta()
	var isConnection := meta.has("index")
	_button.text = "Disconnect" if isConnection else "Connect..."
	_button.icon = Icons.get_icon("Remove" if isConnection else "Signals")
	_button.disabled = _readOnly or meta.is_empty()


func _onButton():
	var meta := _selectedMeta()

	if _readOnly or meta.is_empty(): return

	if meta.has("index"): disconnect_requested.emit(int(meta.index))
	else: connect_requested.emit(str(meta["signal"]), meta.arguments)


func _onActivated():
	var meta := _selectedMeta()

	if meta.has("index"): method_requested.emit(int(meta.target), str(meta.method))
	elif not meta.is_empty() and not _readOnly: connect_requested.emit(str(meta["signal"]), meta.arguments)
