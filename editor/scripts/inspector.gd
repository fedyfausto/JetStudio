extends VBoxContainer
## Only emits edits; the editor applies them and merges the undo steps.

signal property_edited(key: String, value: Variant, final: bool)
signal script_property_edited(name: String, value: Variant, final: bool) # null value = revert
signal script_action(action: String)
signal texture_requested(key: String)
signal model_requested(key: String)
signal import_applied(path: String, settings: Dictionary)

const Document := preload("res://scripts/scene_document.gd")
const Style := preload("res://scripts/theme.gd")
const Icons := preload("res://scripts/icons.gd")
const SpinSlider := preload("res://scripts/spin_slider.gd")

static var folded := {}
static var _thumbnails := {} # absolute path -> {mtime, texture, size}

## res:// -> absolute path, set by the editor for the thumbnails.
var resolve_path: Callable
## Names of the parts of a model: Callable(res_path) -> Array of {name, triangles}.
var model_parts: Callable
## Size of a texture after its import settings: Callable(res_path) -> Vector2i.
var imported_size: Callable

var node: Dictionary = {}
var script_info: Dictionary = {}
var read_only := false
var _header_icon: TextureRect
var _header_name: Label
var _header_class: Label
var _filter: LineEdit
var _content: VBoxContainer
var _rows := [] # [{label, row, section}]
var _updaters := {} # key -> Callable(value)
var _revert := {} # key -> Button

func _ready() -> void:
	add_theme_constant_override("separation", 4)
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
	_header_class = Label.new()
	_header_class.add_theme_color_override("font_color", Style.MUTED)
	_header_class.add_theme_font_size_override("font_size", 12)
	header.add_child(_header_class)
	_filter = LineEdit.new()
	_filter.placeholder_text = "Filter Properties"
	_filter.clear_button_enabled = true
	_filter.right_icon = Icons.get_icon("Search")
	_filter.text_changed.connect(func(_t): _apply_filter())
	add_child(_filter)
	var scroll := ScrollContainer.new()
	scroll.horizontal_scroll_mode = ScrollContainer.SCROLL_MODE_DISABLED
	scroll.size_flags_vertical = Control.SIZE_EXPAND_FILL
	add_child(scroll)
	_content = VBoxContainer.new()
	_content.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	_content.add_theme_constant_override("separation", 2)
	scroll.add_child(_content)
	edit({}, {}, false)

var _import_path := ""

func edit(target: Dictionary, info: Dictionary, locked: bool) -> void:
	_import_path = ""
	node = target
	script_info = info
	read_only = locked
	_rebuild()

## Import tab mode: the import options of a PNG or OBJ file, applied with "Reimport".
func edit_import(path: String, settings: Dictionary, summary: String) -> void:
	_import_path = path
	node = {}
	read_only = false
	_clear()
	var kind := Document.import_kind(path)
	_header_icon.texture = Icons.for_file(path)
	_header_name.text = path.get_file()
	_header_class.text = "Import · " + ("Texture2D" if kind == "texture" else "Mesh")
	var section := _category("Import Settings", Icons.get_icon("ImportCheck"))
	var pending := settings.duplicate()
	var apply := Button.new()
	apply.text = "Reimport"
	apply.icon = Icons.get_icon("Reload")
	apply.disabled = true
	for prop in Document.IMPORT_OPTIONS.get(kind, []):
		var key := str(prop.key)
		var editor := _make_editor(prop, pending.get(key, prop.default), func(value, _final):
			pending[key] = value
			apply.disabled = false)
		_row(section, Document.label_for(prop), str(prop.get("tooltip", "")), editor.control, key)
	var info := Label.new()
	info.text = summary
	info.autowrap_mode = TextServer.AUTOWRAP_WORD_SMART
	info.add_theme_color_override("font_color", Style.MUTED)
	info.add_theme_font_size_override("font_size", 12)
	section.add_child(info)
	apply.pressed.connect(func():
		apply.disabled = true
		import_applied.emit(path, pending.duplicate()))
	section.add_child(apply)
	_apply_filter()

func editing_import() -> String:
	return _import_path

## Updates displayed values in place (gizmo drags, live values during play).
func refresh_values(target: Dictionary) -> void:
	if not _import_path.is_empty(): return
	node = target
	for key in _updaters:
		if key.begins_with("script:"):
			_updaters[key].call(_script_value(key.trim_prefix("script:")))
		elif node.has(key):
			_updaters[key].call(node[key])
	_update_reverts()

func _clear() -> void:
	_rows.clear()
	_updaters.clear()
	_revert.clear()
	for child in _content.get_children():
		_content.remove_child(child)
		child.queue_free()

func _rebuild() -> void:
	_clear()
	if node.is_empty():
		_header_icon.texture = null
		_header_name.text = "No node selected"
		_header_class.text = ""
		var hint := Label.new()
		hint.text = "Select a node in the scene tree\nor in the 3D view to edit its properties."
		hint.add_theme_color_override("font_color", Style.MUTED)
		hint.autowrap_mode = TextServer.AUTOWRAP_WORD_SMART
		_content.add_child(hint)
		return
	var type := str(node.type)
	_header_icon.texture = Icons.for_node(type)
	_header_name.text = str(node.name)
	_header_class.text = type + ("  ·  live values" if read_only else "")
	var script_path := str(node.get("script", ""))
	if not script_path.is_empty() and not script_info.get("exports", []).is_empty():
		var section := _category(script_path.get_file(), Icons.get_icon("Script"))
		for export in script_info.exports:
			_script_row(section, export)
	elif not script_path.is_empty() and not script_info.get("ok", true):
		var section := _category(script_path.get_file(), Icons.get_icon("Script"))
		var warn := Label.new()
		warn.text = "The script has errors: fix them to see the exported variables."
		warn.autowrap_mode = TextServer.AUTOWRAP_WORD_SMART
		warn.add_theme_color_override("font_color", Style.WARNING)
		warn.add_theme_font_size_override("font_size", 12)
		section.add_child(warn)
	for cls in Document.class_chain(type):
		var props: Array = Document.PROPERTIES.get(cls, [])
		if props.is_empty() and cls != "Node": continue
		var section := _category(cls, Icons.for_node(cls))
		var groups := {}
		var order := []
		for prop in props:
			var group := str(prop.get("group", ""))
			if not groups.has(group):
				groups[group] = []
				order.append(group)
			groups[group].append(prop)
		for group in order:
			var target := section
			if not group.is_empty(): target = _group(section, cls + "/" + group, group)
			for prop in groups[group]:
				if not Document.property_visible(prop, node): continue
				_property_row(target, prop)
		if cls == "Node": _script_property(section)
	_update_reverts()
	_apply_filter()

func _category(title: String, icon: Texture2D) -> VBoxContainer:
	var panel := PanelContainer.new()
	panel.add_theme_stylebox_override("panel", Style.box(Style.LIGHT, Style.RADIUS, Color.TRANSPARENT, Vector4(8, 4, 8, 4)))
	_content.add_child(panel)
	var row := HBoxContainer.new()
	row.alignment = BoxContainer.ALIGNMENT_CENTER
	panel.add_child(row)
	var picture := TextureRect.new()
	picture.texture = icon
	picture.custom_minimum_size = Vector2(16, 16)
	picture.expand_mode = TextureRect.EXPAND_IGNORE_SIZE
	picture.stretch_mode = TextureRect.STRETCH_KEEP_ASPECT_CENTERED
	row.add_child(picture)
	var label := Label.new()
	label.text = title
	label.theme_type_variation = "HeaderLabel"
	row.add_child(label)
	var body := VBoxContainer.new()
	body.add_theme_constant_override("separation", 2)
	_content.add_child(body)
	return body

func _group(parent: VBoxContainer, key: String, title: String) -> VBoxContainer:
	var button := Button.new()
	button.theme_type_variation = "FlatButton"
	button.text = title.to_upper()
	button.alignment = HORIZONTAL_ALIGNMENT_LEFT
	button.add_theme_stylebox_override("normal", Style.empty(Vector4(2, 4, 4, 2)))
	button.add_theme_stylebox_override("hover", Style.box(Style.LIGHT, Style.RADIUS, Color.TRANSPARENT, Vector4(2, 4, 4, 2)))
	button.add_theme_stylebox_override("pressed", Style.empty(Vector4(2, 4, 4, 2)))
	button.add_theme_color_override("font_color", Style.MUTED)
	button.add_theme_color_override("font_hover_color", Style.TEXT)
	button.add_theme_font_override("font", Style.bold_font)
	button.add_theme_font_size_override("font_size", 11)
	parent.add_child(button)
	var body := VBoxContainer.new()
	body.add_theme_constant_override("separation", 2)
	parent.add_child(body)
	var update := func():
		var open: bool = not folded.get(key, false)
		body.visible = open
		button.icon = Icons.get_icon("GuiTreeArrowDown" if open else "GuiTreeArrowRight")
	button.pressed.connect(func():
		folded[key] = not folded.get(key, false)
		update.call())
	update.call()
	return body

func _row(parent: VBoxContainer, text: String, tooltip: String, editor: Control, key: String, vertical: bool = false) -> void:
	var row := HBoxContainer.new()
	row.add_theme_constant_override("separation", 4)
	parent.add_child(row)
	var label_box := HBoxContainer.new()
	label_box.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	label_box.size_flags_stretch_ratio = 0.8
	label_box.size_flags_vertical = Control.SIZE_SHRINK_BEGIN if vertical else Control.SIZE_FILL
	row.add_child(label_box)
	var label := Label.new()
	label.theme_type_variation = "PropertyLabel"
	label.text = text
	label.tooltip_text = tooltip if not tooltip.is_empty() else text
	label.mouse_filter = Control.MOUSE_FILTER_PASS
	label.clip_text = true
	label.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	label.custom_minimum_size.y = 26
	label.vertical_alignment = VERTICAL_ALIGNMENT_CENTER
	label_box.add_child(label)
	var revert := Button.new()
	revert.theme_type_variation = "FlatButton"
	revert.icon = Icons.get_icon("Reload")
	revert.tooltip_text = "Revert to default value"
	revert.visible = false
	revert.size_flags_vertical = Control.SIZE_SHRINK_BEGIN
	label_box.add_child(revert)
	_revert[key] = revert
	editor.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	row.add_child(editor)
	_rows.append({"text": text.to_lower() + " " + key.to_lower(), "row": row})

func _property_row(parent: VBoxContainer, prop: Dictionary) -> void:
	var key := str(prop.key)
	var editor := _make_editor(prop, node.get(key, prop.default), func(value, final): property_edited.emit(key, value, final))
	_row(parent, Document.label_for(prop), str(prop.get("tooltip", "")), editor.control, key, prop.type in ["vector3", "vector2", "text"])
	_updaters[key] = editor.update
	_revert[key].pressed.connect(func():
		var value = prop.default
		property_edited.emit(key, value.duplicate() if value is Array else value, true))

func _script_value(var_name: String) -> Variant:
	var overrides: Dictionary = node.get("script_properties", {})
	if overrides.has(var_name): return _plain(overrides[var_name])
	for export in script_info.get("exports", []):
		if export.name == var_name: return _plain(export.get("default"))
	return null

## Scene documents store vectors as arrays and colours as 0xRRGGBB integers.
static func _plain(value: Variant) -> Variant:
	if value is Vector3: return [value.x, value.y, value.z]
	if value is Color: return value.to_rgba32() >> 8
	return value

func _script_row(parent: VBoxContainer, export: Dictionary) -> void:
	var var_name := str(export.name)
	var prop := {"key": var_name, "type": _export_type(str(export.type)), "default": export.get("default")}
	if export.has("range"):
		prop.min = float(export.range[0])
		prop.max = float(export.range[1])
		if float(export.range[2]) > 0: prop.step = float(export.range[2])
	if prop.type == "":
		return
	var editor := _make_editor(prop, _script_value(var_name), func(value, final): script_property_edited.emit(var_name, value, final))
	_row(parent, var_name.capitalize(), "%s: %s (line %d)" % [var_name, export.type, int(export.get("line", 0))], editor.control, "script:" + var_name, prop.type == "vector3")
	_updaters["script:" + var_name] = editor.update
	_revert["script:" + var_name].pressed.connect(func(): script_property_edited.emit(var_name, null, true))

func _export_type(type: String) -> String:
	match type:
		"bool": return "bool"
		"int": return "int"
		"float": return "float"
		"String": return "string"
		"Vector3": return "vector3"
		"Color": return "color"
	return ""

func _script_property(parent: VBoxContainer) -> void:
	var path := str(node.get("script", ""))
	var box := HBoxContainer.new()
	var button := Button.new()
	button.text = path.get_file() if not path.is_empty() else "<empty>"
	button.icon = Icons.get_icon("Script") if not path.is_empty() else null
	button.tooltip_text = path if not path.is_empty() else "No script: create one or load an existing one."
	button.alignment = HORIZONTAL_ALIGNMENT_LEFT
	button.clip_text = true
	button.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	button.disabled = read_only and path.is_empty()
	button.pressed.connect(func(): script_action.emit("open" if not path.is_empty() else "attach"))
	box.add_child(button)
	var menu := MenuButton.new()
	menu.icon = Icons.get_icon("GuiTreeArrowDown")
	menu.flat = true
	menu.disabled = read_only
	var popup := menu.get_popup()
	popup.add_icon_item(Icons.get_icon("ScriptCreate"), "New Script...", 0)
	popup.add_icon_item(Icons.get_icon("Load"), "Load Existing Script...", 1)
	if not path.is_empty():
		popup.add_icon_item(Icons.get_icon("Edit"), "Edit", 2)
		popup.add_separator()
		popup.add_icon_item(Icons.get_icon("ScriptRemove"), "Detach Script", 3)
	popup.id_pressed.connect(func(id): script_action.emit(["attach", "load", "open", "detach"][id]))
	box.add_child(menu)
	_row(parent, "Script", "JetScript script attached to the node (like Godot's 'script' property).", box, "__script")

## Returns {"control": Control, "update": Callable(value)}.
func _make_editor(prop: Dictionary, value: Variant, emit: Callable) -> Dictionary:
	match str(prop.type):
		"vector3", "vector2":
			var column := VBoxContainer.new()
			column.add_theme_constant_override("separation", 2)
			var fields := []
			var count := 3 if prop.type == "vector3" else 2
			for axis in count:
				var field := SpinSlider.new()
				field.setup(float(value[axis]) if value is Array else 0.0, float(prop.get("min", -INF)), float(prop.get("max", INF)), float(prop.get("step", 0.01)))
				field.show_slider = false
				field.prefix = ["x", "y", "z"][axis]
				field.prefix_color = Style.AXIS[axis]
				field.suffix = str(prop.get("suffix", ""))
				field.read_only = read_only
				var capture := axis
				var collect := func(final: bool):
					var out := []
					for f in fields: out.append(f.value)
					emit.call(out, final)
				field.value_changed.connect(func(_v): collect.call(false))
				field.committed.connect(func(_v): collect.call(true))
				fields.append(field)
				column.add_child(field)
			var update_vector := func(v):
				if v is Array:
					for axis in mini(count, v.size()): fields[axis].set_value_no_signal(float(v[axis]))
			return {"control": column, "update": update_vector}
		"float", "int":
			var field := SpinSlider.new()
			var is_int: bool = prop.type == "int"
			field.setup(float(value) if value != null else 0.0, float(prop.get("min", -INF)), float(prop.get("max", INF)),
				float(prop.get("step", 1.0 if is_int else 0.01)), is_int)
			field.suffix = str(prop.get("suffix", ""))
			field.read_only = read_only
			field.value_changed.connect(func(v): emit.call(int(v) if is_int else v, false))
			field.committed.connect(func(v): emit.call(int(v) if is_int else v, true))
			var update_number := func(v):
				if v != null: field.set_value_no_signal(float(v))
			return {"control": field, "update": update_number}
		"bool":
			var check := CheckBox.new()
			check.text = "On"
			check.button_pressed = bool(value)
			check.disabled = read_only
			check.toggled.connect(func(on): emit.call(on, true))
			return {"control": check, "update": func(v): check.set_pressed_no_signal(bool(v))}
		"enum":
			var option := OptionButton.new()
			var options: Array = prop.options
			for i in options.size():
				var icon_name: String = Document.MESH_ICONS.get(options[i], "")
				if not icon_name.is_empty(): option.add_icon_item(Icons.get_icon(icon_name), options[i], i)
				else: option.add_item(options[i], i)
			option.select(maxi(options.find(str(value)), 0))
			option.disabled = read_only
			option.item_selected.connect(func(i): emit.call(options[i], true))
			return {"control": option, "update": func(v): option.select(maxi(options.find(str(v)), 0))}
		"color":
			var picker := ColorPickerButton.new()
			picker.edit_alpha = false
			picker.custom_minimum_size = Vector2(60, 26)
			picker.color = _to_color(value)
			picker.disabled = read_only
			picker.color_changed.connect(func(c): emit.call(c.to_rgba32() >> 8, false))
			picker.popup_closed.connect(func(): emit.call(picker.color.to_rgba32() >> 8, true))
			return {"control": picker, "update": func(v): picker.color = _to_color(v)}
		"texture":
			return _texture_editor(prop, str(value) if value != null else "", emit)
		"model":
			return _model_editor(prop, str(value) if value != null else "", emit)
		"part":
			return _part_editor(str(value) if value != null else "", emit)
		"text":
			var box := TextEdit.new()
			box.text = str(value) if value != null else ""
			box.editable = not read_only
			box.custom_minimum_size.y = 58
			box.scroll_fit_content_height = true
			box.wrap_mode = TextEdit.LINE_WRAPPING_NONE
			box.text_changed.connect(func(): emit.call(box.text, false))
			box.focus_exited.connect(func(): emit.call(box.text, true))
			var update_box := func(v):
				if not box.has_focus() and box.text != str(v): box.text = str(v)
			return {"control": box, "update": update_box}
		"string":
			var line := LineEdit.new()
			line.text = str(value) if value != null else ""
			line.editable = not read_only
			line.text_submitted.connect(func(t): emit.call(t, true))
			line.focus_exited.connect(func(): emit.call(line.text, true))
			var update_text := func(v):
				if not line.has_focus(): line.text = str(v)
			return {"control": line, "update": update_text}
	var unknown := Label.new()
	unknown.text = str(value)
	return {"control": unknown, "update": func(v): unknown.text = str(v)}

func _thumbnail(path: String) -> Dictionary:
	if path.is_empty() or not resolve_path.is_valid(): return {}
	var file: String = resolve_path.call(path)
	if not FileAccess.file_exists(file): return {}
	var mtime := FileAccess.get_modified_time(file)
	var cached: Dictionary = _thumbnails.get(file, {})
	if cached.get("mtime", -1) == mtime: return cached
	var image := Image.load_from_file(file)
	if image == null or image.is_empty(): return {}
	var size := image.get_size()
	var thumb := image.duplicate() as Image
	var scale := 48.0 / maxf(size.x, size.y)
	if scale < 1.0: thumb.resize(maxi(1, int(size.x * scale)), maxi(1, int(size.y * scale)), Image.INTERPOLATE_BILINEAR)
	cached = {"mtime": mtime, "texture": ImageTexture.create_from_image(thumb), "size": size}
	_thumbnails[file] = cached
	return cached

func _texture_editor(prop: Dictionary, path: String, emit: Callable) -> Dictionary:
	var key := str(prop.key)
	var column := VBoxContainer.new()
	column.add_theme_constant_override("separation", 2)
	var row := HBoxContainer.new()
	column.add_child(row)
	var preview := TextureRect.new()
	preview.custom_minimum_size = Vector2(30, 30)
	preview.expand_mode = TextureRect.EXPAND_IGNORE_SIZE
	preview.stretch_mode = TextureRect.STRETCH_KEEP_ASPECT_CENTERED
	preview.texture_filter = CanvasItem.TEXTURE_FILTER_NEAREST
	row.add_child(preview)
	var button := Button.new()
	button.alignment = HORIZONTAL_ALIGNMENT_LEFT
	button.clip_text = true
	button.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	button.disabled = read_only
	button.pressed.connect(func(): texture_requested.emit(key))
	row.add_child(button)
	var clear := Button.new()
	clear.theme_type_variation = "FlatButton"
	clear.icon = Icons.get_icon("Clear")
	clear.tooltip_text = "Remove texture"
	clear.disabled = read_only
	clear.pressed.connect(func(): emit.call("", true))
	row.add_child(clear)
	var info := Label.new()
	info.add_theme_font_size_override("font_size", 11)
	info.autowrap_mode = TextServer.AUTOWRAP_WORD_SMART
	column.add_child(info)
	# Accept PNG files dragged from the FileSystem dock.
	var can_drop := func(_at, data) -> bool:
		if read_only or not data is Dictionary or data.get("type") != "files": return false
		for file in data.get("files", []):
			if str(file).get_extension().to_lower() in Document.IMAGE_EXTENSIONS: return true
		return false
	var drop := func(_at, data):
		for file in data.files:
			if str(file).get_extension().to_lower() in Document.IMAGE_EXTENSIONS:
				emit.call(str(file), true)
				return
	button.set_drag_forwarding(Callable(), can_drop, drop)
	var update := func(v):
		var current := str(v) if v != null else ""
		button.text = current.get_file() if not current.is_empty() else "<empty>  (drop a PNG here)"
		button.tooltip_text = current if not current.is_empty() else "Choose a PNG from the project or drag it from the FileSystem dock."
		clear.visible = not current.is_empty()
		var thumb := _thumbnail(current)
		preview.texture = thumb.get("texture")
		preview.visible = not current.is_empty()
		info.remove_theme_color_override("font_color")
		if current.is_empty():
			info.visible = false
		elif thumb.is_empty():
			info.visible = true
			info.text = "File not found or unreadable (PNG only)."
			info.add_theme_color_override("font_color", Style.ERROR)
		else:
			var size: Vector2i = thumb.size
			var imported: Vector2i = imported_size.call(current) if imported_size.is_valid() else size
			var pot := (imported.x & (imported.x - 1)) == 0 and (imported.y & (imported.y - 1)) == 0
			info.visible = true
			info.text = "%d × %d px" % [imported.x, imported.y] + (" (original %d × %d)" % [size.x, size.y] if imported != size else "") \
				+ ("" if pot else "  ·  non-power-of-2 sides: slower on ESP32")
			info.add_theme_color_override("font_color", Style.MUTED if pot else Style.WARNING)
	update.call(path)
	return {"control": column, "update": update}

func _model_editor(prop: Dictionary, path: String, emit: Callable) -> Dictionary:
	var key := str(prop.key)
	var row := HBoxContainer.new()
	var button := Button.new()
	button.icon = Icons.get_icon("ArrayMesh")
	button.alignment = HORIZONTAL_ALIGNMENT_LEFT
	button.clip_text = true
	button.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	button.disabled = read_only
	button.pressed.connect(func(): model_requested.emit(key))
	row.add_child(button)
	var clear := Button.new()
	clear.theme_type_variation = "FlatButton"
	clear.icon = Icons.get_icon("Clear")
	clear.tooltip_text = "Remove model"
	clear.disabled = read_only
	clear.pressed.connect(func(): emit.call("", true))
	row.add_child(clear)
	var can_drop := func(_at, data) -> bool:
		if read_only or not data is Dictionary or data.get("type") != "files": return false
		for file in data.get("files", []):
			if Document.import_kind(str(file)) == "model": return true
		return false
	var drop := func(_at, data):
		for file in data.files:
			if Document.import_kind(str(file)) == "model":
				emit.call(str(file), true)
				return
	button.set_drag_forwarding(Callable(), can_drop, drop)
	var update := func(v):
		var current := str(v) if v != null else ""
		button.text = current.get_file() if not current.is_empty() else "<empty>  (drop an OBJ here)"
		button.tooltip_text = current if not current.is_empty() else "Choose an OBJ model from the project."
		clear.visible = not current.is_empty()
	update.call(path)
	return {"control": row, "update": update}

## Picks one of the parts found in the node's OBJ model.
func _part_editor(value: String, emit: Callable) -> Dictionary:
	var option := OptionButton.new()
	option.disabled = read_only
	option.clip_text = true
	option.fit_to_longest_item = false
	var names := []
	var fill := func(current: String):
		option.clear()
		names.clear()
		var parts: Array = model_parts.call(str(node.get("mesh_file", ""))) if model_parts.is_valid() else []
		for part in parts:
			names.append(str(part.name))
			option.add_item("%s  (%d tri)" % [part.name, int(part.get("triangles", 0))])
		if not current.is_empty() and not current in names:
			names.append(current)
			option.add_item(current + "  (not found)")
		option.select(names.find(current))
	fill.call(value)
	option.item_selected.connect(func(i): emit.call(names[i], true))
	return {"control": option, "update": func(v): fill.call(str(v))}

func _to_color(value: Variant) -> Color:
	if value is Color: return value
	if value is Array and value.size() >= 3: return Color(float(value[0]), float(value[1]), float(value[2]))
	if value is float or value is int: return Color.hex((int(value) << 8) | 0xff)
	return Color.BLACK

func _same(a: Variant, b: Variant) -> bool:
	if a is Array and b is Array:
		if a.size() != b.size(): return false
		for i in a.size():
			if not is_equal_approx(float(a[i]), float(b[i])): return false
		return true
	if (a is float or a is int) and (b is float or b is int): return is_equal_approx(float(a), float(b))
	return a == b

func _update_reverts() -> void:
	for key in _revert:
		var button: Button = _revert[key]
		if key == "__script":
			button.visible = false
		elif key.begins_with("script:"):
			button.visible = not read_only and node.get("script_properties", {}).has(key.trim_prefix("script:"))
		else:
			var default = Document.default_value(str(node.type), key)
			button.visible = not read_only and default != null and not _same(node.get(key), default)

func _apply_filter() -> void:
	var text := _filter.text.strip_edges().to_lower()
	for item in _rows:
		item.row.visible = text.is_empty() or text in item.text
