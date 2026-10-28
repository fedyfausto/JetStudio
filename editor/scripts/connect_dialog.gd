extends ConfirmationDialog
## Godot's "Connect a Signal to a Method": the receiver node, its method (added to its script when missing), One Shot.

signal connection_chosen(target_id: int, method: String, flags: int)

const Document := preload("res://scripts/scene_document.gd")
const Style := preload("res://scripts/theme.gd")
const Icons := preload("res://scripts/icons.gd")

var class_db := [] # from the bridge
## The functions of a script: Callable(res path) -> Array of names.
var script_functions: Callable

var _doc: Dictionary = {}
var _source: Dictionary = {}
var _signal := ""
var _from: Label
var _tree: Tree
var _method: LineEdit
var _one_shot: CheckBox
var _status: Label
var _typed := false # the method name was edited by hand: keep it when the receiver changes

func _ready() -> void:
	title = "Connect a Signal to a Method"
	ok_button_text = "Connect"
	cancel_button_text = "Cancel"
	min_size = Vector2i(560, 520)
	var box := VBoxContainer.new()
	box.add_theme_constant_override("separation", 8)
	add_child(box)
	_from = Label.new()
	_from.add_theme_color_override("font_color", Style.ACCENT)
	box.add_child(_from)
	var receiver := Label.new()
	receiver.text = "Connect to Node:"
	receiver.theme_type_variation = "PropertyLabel"
	box.add_child(receiver)
	_tree = Tree.new()
	_tree.custom_minimum_size = Vector2(0, 280)
	_tree.size_flags_vertical = Control.SIZE_EXPAND_FILL
	_tree.item_selected.connect(_on_target)
	box.add_child(_tree)
	var row := HBoxContainer.new()
	box.add_child(row)
	var method_label := Label.new()
	method_label.text = "Receiver Method:"
	method_label.theme_type_variation = "PropertyLabel"
	row.add_child(method_label)
	_method = LineEdit.new()
	_method.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	_method.text_changed.connect(func(_t):
		_typed = true
		_validate())
	row.add_child(_method)
	register_text_enter(_method)
	_one_shot = CheckBox.new()
	_one_shot.text = "One Shot"
	_one_shot.tooltip_text = "Disconnects after the first emission."
	box.add_child(_one_shot)
	_status = Label.new()
	_status.autowrap_mode = TextServer.AUTOWRAP_WORD_SMART
	_status.custom_minimum_size = Vector2(500, 40) # a wrapped label needs a width, or the dialog grows with it
	box.add_child(_status)
	confirmed.connect(func():
		var target := _target()
		if not target.is_empty(): connection_chosen.emit(int(target.id), _method.text.strip_edges(), 4 if _one_shot.button_pressed else 0))

## Starts on the nearest node with a script, from the source up to the root, as the usual receiver.
func open(doc: Dictionary, source: Dictionary, signal_name: String) -> void:
	_doc = doc
	_source = source
	_signal = signal_name
	_typed = false
	_one_shot.button_pressed = false
	_from.text = "From Signal:  %s.%s" % [source.name, signal_name]
	var start := source
	while not start.is_empty() and str(start.get("script", "")).is_empty():
		start = Document.parent_of(doc.root, int(start.id))
	if start.is_empty(): start = doc.root
	_tree.clear()
	var items := {}
	Document.walk(doc.root, func(node, parent, _depth):
		var item := _tree.create_item(items.get(int(parent.id), null) if not parent.is_empty() else null)
		items[int(node.id)] = item
		var path := str(node.get("script", ""))
		item.set_text(0, str(node.name))
		item.set_icon(0, Icons.for_node(str(node.type)))
		item.set_metadata(0, int(node.id))
		if path.is_empty():
			item.set_custom_color(0, Style.MUTED)
			item.set_tooltip_text(0, "%s has no script: only its engine methods can be connected." % node.name)
		else:
			item.set_tooltip_text(0, "Script: " + path))
	items[int(start.id)].select(0)
	_tree.scroll_to_item(items[int(start.id)])
	_on_target()
	popup_centered(Vector2i(560, 560))

func _target() -> Dictionary:
	var item := _tree.get_selected()
	return Document.find(_doc.root, int(item.get_metadata(0))) if item != null else {}

func _on_target() -> void:
	var target := _target()
	if target.is_empty(): return
	if not _typed: _method.text = Document.callback_name(_source, target, _signal)
	_validate()

func _engine_methods(type: String) -> Array:
	var names := []
	var current := type
	while not current.is_empty():
		var found := {}
		for c in class_db:
			if c.name == current: found = c
		if found.is_empty(): break
		for m in found.methods: names.append(str(m.name))
		current = str(found.base)
	return names

func _validate() -> void:
	var target := _target()
	var method := _method.text.strip_edges()
	var problem := ""
	var note := ""
	if target.is_empty():
		problem = "Choose the node that receives the signal."
	elif not method.is_valid_identifier():
		problem = "The method name must be a valid identifier."
	elif Document.find_connection(_doc, int(_source.id), _signal, int(target.id), method) >= 0:
		problem = "%s.%s is already connected to %s()." % [_source.name, _signal, method]
	else:
		var path := str(target.get("script", ""))
		var functions: Array = script_functions.call(path) if not path.is_empty() and script_functions.is_valid() else []
		if method in functions:
			note = "Connects to %s() in %s." % [method, path]
		elif not path.is_empty():
			note = "%s() will be added to %s." % [method, path]
		elif method in _engine_methods(str(target.type)):
			note = "Connects to the engine method %s() of %s." % [method, target.name]
		else:
			var some := _engine_methods(str(target.type)).filter(func(m): return m in ["hide", "show", "queue_free", "start", "stop", "play"])
			problem = "%s has no script: attach one first, or use one of its methods%s." % [
				target.name, (" (" + ", ".join(some) + "...)") if not some.is_empty() else ""]
	_status.text = problem if not problem.is_empty() else note
	_status.add_theme_color_override("font_color", Style.ERROR if not problem.is_empty() else Style.MUTED)
	get_ok_button().disabled = not problem.is_empty()
