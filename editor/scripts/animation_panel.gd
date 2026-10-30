extends VBoxContainer

signal preview_requested(player_id: int, animation: String, time: float)

const Document := preload("res://scripts/scene_document.gd")
const Style := preload("res://scripts/theme.gd")
const Icons := preload("res://scripts/icons.gd")

const RULER := 24.0
const ROW := 26.0
const NAMES := 240.0
const LOOPS := ["none", "linear", "pingpong"]

## Callable(merge_key, final, mutate(player)): an undoable change. Nodes are never kept, undo replaces them.
var apply: Callable
var scene_root: Callable
## Callable(node) -> Array of {key, label, property, type}.
var animatable: Callable

var player_id := 0
var animation := ""
var time := 0.0
var playing := false
var read_only := false
var _direction := 1.0
var _selected := Vector2i(-1, -1) # track, key
var _drag := {}
var _view_start := 0.0
var _pixels_per_second := 0.0 # 0 fits the whole animation
var _menu_target := {}
var _updating := false
var _key_shown := []
var _keep_key_editor := false

var _empty: Label
var _body: VBoxContainer
var _animations: OptionButton
var _edit_menu: MenuButton
var _time: SpinBox
var _length: SpinBox
var _loop: OptionButton
var _autoplay: Button
var _play: Button
var _play_backwards: Button
var _add_track: MenuButton
var _timeline: Control
var _scroll: ScrollContainer
var _key_box: HBoxContainer
var _node_menu: PopupMenu
var _property_menu: PopupMenu
var _context_menu: PopupMenu
var _rename: ConfirmationDialog
var _rename_text: LineEdit

func _ready() -> void:
	add_theme_constant_override("separation", 4)
	_empty = Label.new()
	_empty.text = "Select an AnimationPlayer to edit its animations (add one with the + of the Scene dock)."
	_empty.add_theme_color_override("font_color", Style.MUTED)
	add_child(_empty)
	_body = VBoxContainer.new()
	_body.size_flags_vertical = Control.SIZE_EXPAND_FILL
	add_child(_body)

	var bar := HBoxContainer.new()
	bar.add_theme_constant_override("separation", 4)
	_body.add_child(bar)
	_play_backwards = _button(bar, "PlayBackwards", "Play the animation backwards from here", func(): _start(-1.0))
	_button(bar, "Stop", "Stop", func(): _stop_preview())
	_play = _button(bar, "Play", "Play the animation from here", func(): _start(1.0))
	_button(bar, "PlayStart", "Play the animation from the start", func():
		_set_time(0.0)
		_start(1.0))
	bar.add_child(VSeparator.new())

	_edit_menu = MenuButton.new()
	_edit_menu.text = "Animation"
	_edit_menu.flat = false
	var popup := _edit_menu.get_popup()
	popup.add_icon_item(Icons.get_icon("Add"), "New", 0)
	popup.add_icon_item(Icons.get_icon("Rename"), "Rename...", 1)
	popup.add_icon_item(Icons.get_icon("Duplicate"), "Duplicate", 2)
	popup.add_icon_item(Icons.get_icon("Remove"), "Remove", 3)
	popup.id_pressed.connect(_on_edit_menu)
	bar.add_child(_edit_menu)
	_animations = OptionButton.new()
	_animations.custom_minimum_size.x = 160
	_animations.item_selected.connect(func(i):
		animation = _animations.get_item_text(i)
		_selected = Vector2i(-1, -1)
		_set_time(0.0)
		refresh())
	bar.add_child(_animations)
	_autoplay = _button(bar, "AutoPlay", "Autoplay on load: the scene starts playing this animation", _toggle_autoplay, true)
	bar.add_child(VSeparator.new())

	bar.add_child(_label("Time:"))
	_time = _spin(0.0, 10000.0, 0.01, func(value): _set_time(value))
	bar.add_child(_time)
	bar.add_child(_label("Length:"))
	_length = _spin(0.01, 10000.0, 0.01, func(value): _change_animation("length", true, func(a): a.length = value))
	bar.add_child(_length)
	_loop = OptionButton.new()
	_loop.add_icon_item(Icons.get_icon("Stop"), "No Loop")
	_loop.add_icon_item(Icons.get_icon("Loop"), "Loop")
	_loop.add_icon_item(Icons.get_icon("PingPongLoop"), "Ping-Pong")
	_loop.tooltip_text = "Animation looping"
	_loop.item_selected.connect(func(i): _change_animation("loop", true, func(a): a.loop = LOOPS[i]))
	bar.add_child(_loop)

	var spacer := Control.new()
	spacer.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	bar.add_child(spacer)
	_add_track = MenuButton.new()
	_add_track.text = "Add Track"
	_add_track.icon = Icons.get_icon("Add")
	_add_track.flat = false
	_add_track.get_popup().add_icon_item(Icons.get_icon("KeyValue"), "Property Track...", 0)
	_add_track.get_popup().add_icon_item(Icons.get_icon("KeyCall"), "Call Method Track...", 1)
	_add_track.get_popup().id_pressed.connect(func(id):
		_menu_target = {"method": id == 1}
		_show_node_menu())
	bar.add_child(_add_track)

	_scroll = ScrollContainer.new()
	_scroll.size_flags_vertical = Control.SIZE_EXPAND_FILL
	_scroll.horizontal_scroll_mode = ScrollContainer.SCROLL_MODE_DISABLED
	_body.add_child(_scroll)
	_timeline = Control.new()
	_timeline.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	_timeline.focus_mode = Control.FOCUS_CLICK
	_timeline.clip_contents = true
	_timeline.draw.connect(_draw_timeline)
	_timeline.gui_input.connect(_timeline_input)
	_timeline.resized.connect(func(): _timeline.queue_redraw())
	_scroll.add_child(_timeline)
	_key_box = HBoxContainer.new()
	_key_box.add_theme_constant_override("separation", 6)
	_body.add_child(_key_box)

	_node_menu = PopupMenu.new()
	_node_menu.id_pressed.connect(_on_node_chosen)
	add_child(_node_menu)
	_property_menu = PopupMenu.new()
	_property_menu.id_pressed.connect(_on_property_chosen)
	add_child(_property_menu)
	_context_menu = PopupMenu.new()
	_context_menu.id_pressed.connect(_on_context_menu)
	add_child(_context_menu)
	_rename = ConfirmationDialog.new()
	_rename.title = "Rename Animation"
	_rename_text = LineEdit.new()
	_rename.add_child(_rename_text)
	_rename.register_text_enter(_rename_text)
	_rename.confirmed.connect(_on_renamed)
	add_child(_rename)
	edit(0)

func _toggle_autoplay() -> void:
	var name := animation
	var on := _autoplay.button_pressed
	_change("autoplay", true, func(player): player.autoplay = name if on else "")
	refresh()

func _button(parent: Control, icon: String, tooltip: String, action: Callable, toggle: bool = false) -> Button:
	var button := Button.new()
	button.theme_type_variation = "FlatButton"
	button.icon = Icons.get_icon(icon)
	button.tooltip_text = tooltip
	button.toggle_mode = toggle
	button.focus_mode = Control.FOCUS_NONE
	button.pressed.connect(action)
	parent.add_child(button)
	return button

func _label(text: String) -> Label:
	var label := Label.new()
	label.text = text
	label.add_theme_color_override("font_color", Style.MUTED)
	return label

func _spin(low: float, high: float, step: float, changed: Callable) -> SpinBox:
	var spin := SpinBox.new()
	spin.min_value = low
	spin.max_value = high
	spin.step = step
	spin.custom_minimum_size.x = 90
	spin.value_changed.connect(func(value):
		if not _updating: changed.call(value))
	return spin

func _player() -> Dictionary:
	return Document.find(scene_root.call(), player_id) if player_id > 0 and scene_root.is_valid() else {}

static func _list(player: Dictionary) -> Array:
	return player.animations if player.get("animations") is Array else []

func _current() -> Dictionary:
	for a in _list(_player()):
		if str(a.name) == animation: return a
	return {}

func _tracks() -> Array:
	var a := _current()
	return a.tracks if a.get("tracks") is Array else []

func _length_of(a: Dictionary) -> float:
	return maxf(float(a.get("length", 1.0)), 0.01)

func _change(merge: String, final: bool, mutate: Callable) -> void:
	if read_only or not apply.is_valid(): return
	apply.call(merge, final, mutate)

func _change_animation(merge: String, final: bool, mutate: Callable) -> void:
	var name := animation
	_change(merge, final, func(player):
		for a in _list(player):
			if str(a.name) == name: mutate.call(a))
	refresh()

func _change_track(index: int, merge: String, final: bool, mutate: Callable) -> void:
	_change_animation(merge, final, func(a):
		if a.get("tracks") is Array and index < a.tracks.size(): mutate.call(a.tracks[index]))

## Shows an AnimationPlayer (0: none), keeping animation and time for the same player.
func edit(id: int) -> void:
	if id != player_id:
		player_id = id
		animation = ""
		time = 0.0
		_selected = Vector2i(-1, -1)
		_stop_preview()
	refresh()

func refresh() -> void:
	var player := _player()
	_empty.visible = player.is_empty()
	_body.visible = not player.is_empty()
	if player.is_empty():
		player_id = 0
		return

	var names := []
	for a in _list(player): names.append(str(a.name))
	if not animation in names: animation = names[0] if not names.is_empty() else ""
	_updating = true
	_animations.clear()
	for name in names:
		_animations.add_icon_item(Icons.get_icon("Animation"), name)
		if name == animation: _animations.select(_animations.item_count - 1)

	var a := _current()
	for control in [_animations, _loop, _autoplay, _play, _play_backwards, _add_track]:
		control.disabled = a.is_empty() or (read_only and control != _animations)
	_time.editable = not a.is_empty()
	_length.editable = not a.is_empty() and not read_only
	for item in [1, 2, 3]: _edit_menu.get_popup().set_item_disabled(item, a.is_empty())
	_edit_menu.disabled = read_only
	_length.value = _length_of(a) if not a.is_empty() else 1.0
	_time.max_value = _length.value
	_time.value = time
	_loop.select(maxi(0, LOOPS.find(str(a.get("loop", "none")))))
	_autoplay.button_pressed = not a.is_empty() and str(player.get("autoplay", "")) == animation
	_updating = false

	var tracks := _tracks()
	if _selected.x >= tracks.size() or (_selected.x >= 0 and _selected.y >= tracks[_selected.x].get("keys", []).size()):
		_selected = Vector2i(-1, -1)
	_timeline.custom_minimum_size.y = RULER + maxi(tracks.size(), 1) * ROW + 8
	_timeline.queue_redraw()
	_rebuild_key_editor()

func _set_time(value: float) -> void:
	var a := _current()
	time = clampf(value, 0.0, _length_of(a) if not a.is_empty() else 0.0)
	_updating = true
	_time.value = time
	_updating = false
	_timeline.queue_redraw()
	if not a.is_empty(): preview_requested.emit(player_id, animation, time)

func _unique_name(base: String) -> String:
	var names := _list(_player()).map(func(a): return str(a.name))
	var name := base
	var n := 2
	while name in names:
		name = "%s_%d" % [base, n]
		n += 1
	return name

func _on_edit_menu(id: int) -> void:
	match id:
		0:
			var name := _unique_name("new_animation")
			_change("", true, func(player):
				if not player.get("animations") is Array: player.animations = []
				player.animations.append({"name": name, "length": 1.0, "loop": "none", "tracks": []}))
			animation = name
			_set_time(0.0)
			refresh()
		1:
			_rename_text.text = animation
			_rename.popup_centered(Vector2i(320, 0))
			_rename_text.grab_focus()
			_rename_text.select_all()
		2:
			var copy: Dictionary = _current().duplicate(true)
			copy.name = _unique_name(animation + "_copy")
			_change("", true, func(player): player.animations.append(copy))
			animation = copy.name
			refresh()
		3:
			var name := animation
			_change("", true, func(player):
				player.animations = _list(player).filter(func(a): return str(a.name) != name)
				if str(player.get("autoplay", "")) == name: player.autoplay = "")
			animation = ""
			refresh()

func _on_renamed() -> void:
	var name := _rename_text.text.strip_edges()
	var old := animation
	if name.is_empty() or name == old or name in _list(_player()).map(func(a): return str(a.name)): return
	_change("", true, func(player):
		for a in _list(player):
			if str(a.name) == old: a.name = name
		if str(player.get("autoplay", "")) == old: player.autoplay = name)
	animation = name
	refresh()

static func _new_track(path: String, property: String, key: String, type: String) -> Dictionary:
	return {"path": path, "property": property, "key": key, "type": type, "keys": [],
		"interpolation": "linear" if type in ["float", "vector2", "vector3", "color"] else "nearest"}

## The Inspector's key icon: a key at the current time, on a new track if the property has none.
func insert_key(path: String, property: String, key: String, type: String, value: Variant) -> void:
	if _current().is_empty(): return
	var at := time
	_change_animation("", true, func(a):
		if not a.get("tracks") is Array: a.tracks = []
		var track := {}
		for t in a.tracks:
			if str(t.get("path", "")) == path and str(t.get("property", "")) == property: track = t
		if track.is_empty():
			track = _new_track(path, property, key, type)
			a.tracks.append(track)
		_put_key(track, {"time": at, "value": value, "transition": 1.0}))
	refresh()

static func _put_key(track: Dictionary, key: Dictionary) -> void:
	var keys: Array = track.keys
	for i in keys.size():
		if absf(float(keys[i].time) - float(key.time)) < 0.0005:
			keys[i] = key
			return
	keys.append(key)
	keys.sort_custom(func(x, y): return float(x.time) < float(y.time))

func _show_node_menu() -> void:
	_node_menu.clear()
	var entries := []
	Document.walk(scene_root.call(), func(node, _parent, depth): entries.append([node, depth]))
	for i in entries.size():
		var node: Dictionary = entries[i][0]
		_node_menu.add_icon_item(Icons.for_node(str(node.type)), "    ".repeat(int(entries[i][1])) + str(node.name), i)
		_node_menu.set_item_metadata(i, int(node.id))
	_node_menu.reset_size()
	_node_menu.popup(Rect2i(Vector2i(_add_track.get_screen_position() + Vector2(0, _add_track.size.y)), Vector2i.ZERO))

func _on_node_chosen(index: int) -> void:
	var id := int(_node_menu.get_item_metadata(index))
	var root: Dictionary = scene_root.call()
	var parent := Document.parent_of(root, player_id)
	_menu_target.path = Document.relative_path(root, int(parent.id), id) if not parent.is_empty() else "."

	if _menu_target.get("method", false):
		var path: String = _menu_target.path
		_change_animation("", true, func(a):
			if not a.get("tracks") is Array: a.tracks = []
			a.tracks.append({"path": path, "method": true, "keys": []}))
		refresh()
		return

	_property_menu.clear()
	var choices: Array = animatable.call(Document.find(root, id)) if animatable.is_valid() else []
	_menu_target.choices = choices
	for i in choices.size(): _property_menu.add_item(str(choices[i].label), i)
	if choices.is_empty(): _property_menu.add_item("(nothing to animate on this node)", -1)
	_property_menu.reset_size()
	_property_menu.popup(Rect2i(Vector2i(_node_menu.position), Vector2i.ZERO))

func _on_property_chosen(index: int) -> void:
	if index < 0: return
	var choice: Dictionary = _menu_target.choices[index]
	var path: String = _menu_target.path
	_change_animation("", true, func(a):
		if not a.get("tracks") is Array: a.tracks = []
		for t in a.tracks:
			if str(t.get("path", "")) == path and str(t.get("property", "")) == str(choice.property): return
		a.tracks.append(_new_track(path, str(choice.property), str(choice.key), str(choice.type))))
	refresh()

## Colours are 0xRRGGBB ints and vectors arrays, as the scene stores them.
static func value_at(track: Dictionary, at: float) -> Variant:
	var keys: Array = track.get("keys", [])
	if keys.is_empty(): return null
	var next := 0
	while next < keys.size() and float(keys[next].time) <= at: next += 1
	if next == 0: return keys[0].value
	if next == keys.size() or str(track.get("interpolation", "linear")) == "nearest": return keys[next - 1].value

	var a = keys[next - 1].value
	var b = keys[next].value
	var w := (at - float(keys[next - 1].time)) / maxf(float(keys[next].time) - float(keys[next - 1].time), 0.0001)
	if str(track.get("type", "")) == "color":
		var ca := Color.hex((int(a) << 8) | 0xff)
		var cb := Color.hex((int(b) << 8) | 0xff)
		return ca.lerp(cb, w).to_rgba32() >> 8
	if a is Array and b is Array and a.size() == b.size():
		var out := []
		for i in a.size(): out.append(lerpf(float(a[i]), float(b[i]), w))
		return out
	if (a is float or a is int) and (b is float or b is int):
		var x := lerpf(float(a), float(b), w)
		return roundi(x) if str(track.get("type", "")) == "int" else x
	return a

func _insert_at(track_index: int, at: float) -> void:
	var tracks := _tracks()
	if track_index < 0 or track_index >= tracks.size(): return
	var track: Dictionary = tracks[track_index]
	if track.get("method", false):
		_change_track(track_index, "", true, func(t): _put_key(t, {"time": at, "method": "", "args": []}))
	else:
		var value = value_at(track, at)
		if value == null: value = _current_value(track)
		_change_track(track_index, "", true, func(t): _put_key(t, {"time": at, "value": value, "transition": 1.0}))
	refresh()

func _current_value(track: Dictionary) -> Variant:
	var root: Dictionary = scene_root.call()
	var parent := Document.parent_of(root, player_id)
	var node := _node_at(root, parent, str(track.get("path", ".")))
	var key := str(track.get("key", track.get("property", "")))
	return node.get(key, 0.0) if not node.is_empty() else 0.0

static func _node_at(root: Dictionary, from: Dictionary, path: String) -> Dictionary:
	var current := from
	for part in path.split("/", false):
		if current.is_empty(): return {}
		if part == "..": current = Document.parent_of(root, int(current.id))
		elif part != ".":
			var found := {}
			for child in Document.children(current):
				if str(child.name) == part: found = child
			current = found
	return current

func _delete_selected() -> void:
	if _selected.x < 0: return
	var key_index := _selected.y
	_change_track(_selected.x, "", true, func(t): t.keys.remove_at(key_index))
	_selected = Vector2i(-1, -1)

func _seconds_per_pixel_fit() -> float:
	return _length_of(_current()) / maxf(_timeline.size.x - NAMES - 16.0, 50.0)

func _pps() -> float:
	return _pixels_per_second if _pixels_per_second > 0 else 1.0 / _seconds_per_pixel_fit()

func _x_of(t: float) -> float:
	return NAMES + 8.0 + (t - _view_start) * _pps()

func _time_of(x: float) -> float:
	return _view_start + (x - NAMES - 8.0) / _pps()

func _row_of(y: float) -> int:
	return int(floor((y - RULER) / ROW)) if y >= RULER else -1

func _draw_timeline() -> void:
	var c := _timeline
	var width := c.size.x
	var a := _current()
	c.draw_rect(Rect2(Vector2.ZERO, c.size), Style.VIEWPORT)
	if a.is_empty():
		c.draw_string(get_theme_default_font(), Vector2(12, 40), "No animation: Animation > New creates one.", HORIZONTAL_ALIGNMENT_LEFT, -1, 13, Style.MUTED)
		return

	var font := get_theme_default_font()
	var end_x := _x_of(_length_of(a))
	if end_x < width: c.draw_rect(Rect2(end_x, 0, width - end_x, c.size.y), Color(0, 0, 0, 0.25))

	# ruler labels at least 60 px apart
	c.draw_rect(Rect2(0, 0, width, RULER), Style.LIGHT)
	var step := 0.01
	for s in [0.01, 0.02, 0.05, 0.1, 0.2, 0.25, 0.5, 1.0, 2.0, 5.0, 10.0, 30.0, 60.0]:
		step = s
		if s * _pps() >= 60.0: break
	var t := ceilf(maxf(_view_start, 0.0) / step) * step
	while _x_of(t) < width:
		var x := _x_of(t)
		c.draw_line(Vector2(x, RULER - 8), Vector2(x, RULER), Style.MUTED)
		c.draw_line(Vector2(x, RULER), Vector2(x, c.size.y), Color(1, 1, 1, 0.04))
		c.draw_string(font, Vector2(x + 3, 14), String.num(t, 2).rstrip("0").rstrip(".") if t != 0 else "0", HORIZONTAL_ALIGNMENT_LEFT, -1, 11, Style.MUTED)
		t += step

	var tracks := _tracks()
	var root: Dictionary = scene_root.call()
	var parent := Document.parent_of(root, player_id)
	for i in tracks.size():
		var track: Dictionary = tracks[i]
		var y := RULER + i * ROW
		if i % 2 == 1: c.draw_rect(Rect2(0, y, width, ROW), Color(1, 1, 1, 0.02))
		if i == _selected.x: c.draw_rect(Rect2(0, y, NAMES, ROW), Color(Style.ACCENT, 0.12))

		var node := _node_at(root, parent, str(track.get("path", ".")))
		var icon: Texture2D = Icons.for_node(str(node.type)) if not node.is_empty() else Icons.get_icon("NodeWarning")
		if icon: c.draw_texture_rect(icon, Rect2(6, y + 5, 16, 16), false)
		var method: bool = track.get("method", false)
		var label := "%s: %s" % [str(node.get("name", track.get("path", "?"))), "methods" if method else str(track.get("property", ""))]
		c.draw_string(font, Vector2(28, y + 17), label, HORIZONTAL_ALIGNMENT_LEFT, NAMES - 80, 13,
			Style.ERROR if node.is_empty() else Color("cdcfd2"))
		if not method:
			var interp := Icons.get_icon("InterpLinear" if str(track.get("interpolation", "linear")) == "linear" else "InterpRaw")
			if interp: c.draw_texture_rect(interp, Rect2(NAMES - 46, y + 5, 16, 16), false)
		var remove := Icons.get_icon("Remove")
		if remove: c.draw_texture_rect(remove, Rect2(NAMES - 24, y + 5, 16, 16), false, Color(1, 1, 1, 0.7))

		var keys: Array = track.get("keys", [])
		for k in keys.size():
			var kx := _x_of(float(keys[k].time))
			if kx < NAMES or kx > width: continue
			var color := Color("ffb35c") if method else Color("cbe2ff")
			if Vector2i(i, k) == _selected: color = Style.ACCENT
			var center := Vector2(kx, y + ROW * 0.5)
			var diamond := PackedVector2Array([center + Vector2(0, -6), center + Vector2(6, 0), center + Vector2(0, 6), center + Vector2(-6, 0)])
			c.draw_colored_polygon(diamond, color)
		c.draw_line(Vector2(0, y + ROW), Vector2(width, y + ROW), Color(1, 1, 1, 0.05))
	c.draw_line(Vector2(NAMES, 0), Vector2(NAMES, c.size.y), Color(1, 1, 1, 0.12))

	var px := _x_of(time)
	if px >= NAMES:
		c.draw_line(Vector2(px, 0), Vector2(px, c.size.y), Style.ACCENT, 2.0)
		c.draw_colored_polygon(PackedVector2Array([Vector2(px - 6, 0), Vector2(px + 6, 0), Vector2(px, 8)]), Style.ACCENT)

func _key_at(row: int, x: float) -> int:
	var tracks := _tracks()
	if row < 0 or row >= tracks.size(): return -1
	var keys: Array = tracks[row].get("keys", [])
	for k in range(keys.size() - 1, -1, -1):
		if absf(_x_of(float(keys[k].time)) - x) <= 7.0: return k
	return -1

func _timeline_input(event: InputEvent) -> void:
	if _current().is_empty(): return
	var tracks := _tracks()

	if event is InputEventMouseButton:
		var row := _row_of(event.position.y)
		if event.pressed and event.button_index in [MOUSE_BUTTON_WHEEL_UP, MOUSE_BUTTON_WHEEL_DOWN] and (event.ctrl_pressed or event.shift_pressed):
			var up: bool = event.button_index == MOUSE_BUTTON_WHEEL_UP
			if event.ctrl_pressed:
				var anchor := _time_of(event.position.x)
				_pixels_per_second = clampf(_pps() * (1.2 if up else 1 / 1.2), 5.0, 5000.0)
				_view_start = maxf(0.0, anchor - (event.position.x - NAMES - 8.0) / _pps())
			else:
				_view_start = maxf(0.0, _view_start + (-60.0 if up else 60.0) / _pps())
			_timeline.queue_redraw()
			_timeline.accept_event()
			return

		if event.button_index == MOUSE_BUTTON_LEFT and event.pressed:
			_timeline.grab_focus()
			if event.position.y < RULER and event.position.x >= NAMES:
				_drag = {"scrub": true}
				_set_time(_time_of(event.position.x))
			elif row >= 0 and row < tracks.size() and event.position.x < NAMES:
				if read_only: return
				if event.position.x >= NAMES - 26:
					var index := row
					_change_animation("", true, func(a): a.tracks.remove_at(index))
					_selected = Vector2i(-1, -1)
				elif event.position.x >= NAMES - 48 and not tracks[row].get("method", false):
					_change_track(row, "", true, func(t): t.interpolation = "nearest" if str(t.get("interpolation", "linear")) == "linear" else "linear")
				else:
					_selected = Vector2i(row, -1)
				refresh()
			elif row >= 0 and row < tracks.size():
				var k := _key_at(row, event.position.x)
				if event.double_click and k < 0 and not read_only:
					_insert_at(row, snappedf(_time_of(event.position.x), 0.01))
				elif k >= 0:
					_selected = Vector2i(row, k)
					_drag = {"track": row, "key": k, "from": float(tracks[row].keys[k].time), "x": event.position.x}
					refresh()
				else:
					_selected = Vector2i(row, -1)
					refresh()
			_timeline.accept_event()

		elif event.button_index == MOUSE_BUTTON_LEFT and not event.pressed:
			if _drag.has("track") and _drag.get("moved", false):
				_move_key(event.position.x, true)
			_drag = {}

		elif event.button_index == MOUSE_BUTTON_RIGHT and event.pressed and row >= 0 and row < tracks.size() and event.position.x >= NAMES and not read_only:
			var k := _key_at(row, event.position.x)
			_menu_target = {"track": row, "key": k, "time": snappedf(_time_of(event.position.x), 0.01)}
			_context_menu.clear()
			_context_menu.add_icon_item(Icons.get_icon("Key"), "Insert Key", 0)
			if k >= 0: _context_menu.add_icon_item(Icons.get_icon("Remove"), "Delete Key", 1)
			_context_menu.reset_size()
			_context_menu.popup(Rect2i(Vector2i(_timeline.get_screen_position() + event.position), Vector2i.ZERO))
			_timeline.accept_event()

	elif event is InputEventMouseMotion:
		if _drag.get("scrub", false):
			_set_time(_time_of(event.position.x))
		elif _drag.has("track") and not read_only:
			if absf(event.position.x - float(_drag.x)) > 3.0: _drag.moved = true
			if _drag.get("moved", false): _move_key(event.position.x, false)

	elif event is InputEventKey and event.pressed and not event.echo and event.keycode == KEY_DELETE and _selected.y >= 0 and not read_only:
		_delete_selected()
		_timeline.accept_event()

func _move_key(x: float, final: bool) -> void:
	var length := _length_of(_current())
	var to := clampf(snappedf(float(_drag.from) + (x - float(_drag.x)) / _pps(), 0.01), 0.0, length)
	var track_index: int = _drag.track
	var drag := _drag
	_change_track(track_index, "drag_key", final, func(t):
		var key: Dictionary = t.keys[drag.key]
		key.time = to
		t.keys.sort_custom(func(p, q): return float(p.time) < float(q.time))
		drag.key = t.keys.find(key))
	_selected = Vector2i(track_index, int(_drag.key))
	refresh()

func _on_context_menu(id: int) -> void:
	match id:
		0: _insert_at(int(_menu_target.track), float(_menu_target.time))
		1:
			_selected = Vector2i(int(_menu_target.track), int(_menu_target.key))
			_delete_selected()

# Edits made from the key editor keep it, so its fields don't lose focus; undo rebuilds it.
func _set_key_field(track: int, key: int, field: String, value: Variant, merge: String = "", final: bool = true) -> void:
	var tracks := _tracks()
	if track >= tracks.size() or key >= tracks[track].keys.size(): return
	var old = tracks[track].keys[key].get(field)
	if merge.is_empty() and typeof(old) == typeof(value) and old == value: return
	_keep_key_editor = true
	_change_track(track, merge, final, func(t): t.keys[key][field] = value)
	_keep_key_editor = false

func _rebuild_key_editor() -> void:
	var tracks := _tracks()
	var has_key := _selected.x >= 0 and _selected.y >= 0 and _selected.x < tracks.size()
	var shown := [player_id, animation, _selected, tracks[_selected.x].keys[_selected.y] if has_key else null]
	if _keep_key_editor or shown == _key_shown:
		_key_shown = shown.duplicate(true)
		return
	_key_shown = shown.duplicate(true)

	for child in _key_box.get_children(): child.queue_free()
	if not has_key:
		_key_box.add_child(_label("Double-click a track to add a key, or use the key icons in the Inspector. Ctrl+wheel zooms."))
		return

	var track: Dictionary = tracks[_selected.x]
	var key: Dictionary = track.keys[_selected.y]
	var ti := _selected.x
	var ki := _selected.y
	_key_box.add_child(_label("Key time:"))
	var key_time := _spin(0.0, _length_of(_current()), 0.01, func(value):
		_change_track(ti, "", true, func(t):
			var moving: Dictionary = t.keys[ki]
			moving.time = value
			t.keys.sort_custom(func(p, q): return float(p.time) < float(q.time))
			_selected = Vector2i(ti, t.keys.find(moving)))
		refresh())
	key_time.value = float(key.time)
	_key_box.add_child(key_time)

	if track.get("method", false):
		_key_box.add_child(_label("Method:"))
		var method := LineEdit.new()
		method.text = str(key.get("method", ""))
		method.placeholder_text = "method name"
		method.custom_minimum_size.x = 160
		method.text_submitted.connect(func(text): _set_key_field(ti, ki, "method", text.strip_edges()))
		method.focus_exited.connect(func(): _set_key_field(ti, ki, "method", method.text.strip_edges()))
		_key_box.add_child(method)

		_key_box.add_child(_label("Arguments:"))
		var args := LineEdit.new()
		args.text = JSON.stringify(key.get("args", [])).trim_prefix("[").trim_suffix("]")
		args.placeholder_text = "1, \"text\", true"
		args.custom_minimum_size.x = 160
		args.tooltip_text = "Values separated by commas: numbers, \"strings\", true/false."
		var set_args := func(text: String):
			var parsed = JSON.parse_string("[" + text + "]")
			if parsed is Array: _set_key_field(ti, ki, "args", parsed)
		args.text_submitted.connect(set_args)
		args.focus_exited.connect(func(): set_args.call(args.text))
		_key_box.add_child(args)
		return

	_key_box.add_child(_label("Value:"))
	_key_box.add_child(_value_editor(str(track.get("type", "float")), key.get("value"), func(value, final):
		_set_key_field(ti, ki, "value", value, "key_value", final)))
	_key_box.add_child(_label("Easing:"))
	var easing := _spin(-10.0, 10.0, 0.05, func(value): _set_key_field(ti, ki, "transition", value, "key_easing"))
	easing.value = float(key.get("transition", 1.0))
	easing.tooltip_text = "Curve to the next key, as Godot's: 1 linear, above 1 eases in, below 1 eases out, negative eases in and out."
	_key_box.add_child(easing)

func _value_editor(type: String, value: Variant, changed: Callable) -> Control:
	match type:
		"vector2", "vector3":
			var box := HBoxContainer.new()
			var count := 2 if type == "vector2" else 3
			var values: Array = (value as Array).duplicate() if value is Array else [0.0, 0.0, 0.0].slice(0, count)
			for i in count:
				var index := i
				var spin := _spin(-1e6, 1e6, 0.01, func(v):
					values[index] = v
					changed.call(values.duplicate(), true))
				spin.allow_greater = true
				spin.allow_lesser = true
				spin.value = float(values[i]) if i < values.size() else 0.0
				spin.prefix = ["x", "y", "z"][i]
				box.add_child(spin)
			return box
		"color":
			var picker := ColorPickerButton.new()
			picker.custom_minimum_size = Vector2(70, 24)
			picker.edit_alpha = false
			picker.color = Color.hex((int(value if value != null else 0xffffff) << 8) | 0xff)
			picker.color_changed.connect(func(c): changed.call(c.to_rgba32() >> 8, false))
			picker.popup_closed.connect(func(): changed.call(picker.color.to_rgba32() >> 8, true))
			return picker
		"bool":
			var check := CheckBox.new()
			check.text = "On"
			check.button_pressed = bool(value)
			check.toggled.connect(func(on): changed.call(on, true))
			return check
		"float", "int":
			var spin := _spin(-1e6, 1e6, 1.0 if type == "int" else 0.01, func(v): changed.call(int(v) if type == "int" else v, true))
			spin.allow_greater = true
			spin.allow_lesser = true
			spin.value = float(value) if value != null else 0.0
			return spin
		_:
			var line := LineEdit.new()
			line.text = str(value) if value != null else ""
			line.custom_minimum_size.x = 160
			line.text_submitted.connect(func(text): changed.call(text, true))
			return line

func _start(direction: float) -> void:
	if _current().is_empty(): return
	_direction = direction
	playing = true
	var length := _length_of(_current())
	if direction > 0 and time >= length: time = 0.0
	if direction < 0 and time <= 0.0: time = length

func _stop_preview() -> void:
	playing = false

func _process(delta: float) -> void:
	if not playing or not is_visible_in_tree(): return
	var a := _current()
	if a.is_empty():
		playing = false
		return

	var length := _length_of(a)
	var next := time + delta * _direction
	match str(a.get("loop", "none")):
		"linear": next = fposmod(next, length)
		"pingpong":
			if next > length or next < 0.0:
				_direction = -_direction
				next = clampf(next, 0.0, length)
		_:
			if next >= length or next <= 0.0:
				playing = false
				next = clampf(next, 0.0, length)
	_set_time(next)
