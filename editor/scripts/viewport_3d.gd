extends VBoxContainer
## Navigation as in Godot: MMB orbit, Shift+MMB pan, Ctrl+MMB zoom, RMB freelook (WASD/QE), Alt+LMB acts as MMB, F focuses.

signal camera_changed
signal node_picked(id: int)
signal transform_edited(id: int, local: Transform3D, final: bool)
signal delete_requested
signal duplicate_requested
signal file_dropped(id: int, path: String)
signal model_dropped(path: String, position: Vector3)
signal control_moved(id: int, position: Vector2, final: bool)

const Document := preload("res://scripts/scene_document.gd")
const Style := preload("res://scripts/theme.gd")
const Icons := preload("res://scripts/icons.gd")
# Game resolution from the project hardware, see set_resolution().
var W := 320.0
var H := 240.0
const NEAR := 0.05
const ORBIT_SPEED := 0.0044 # radians per pixel (Godot: 0.25 degrees)
const GIZMO_PIXELS := 80.0

enum Tool { SELECT, MOVE, ROTATE, SCALE }
enum Nav { NONE, ORBIT, PAN, ZOOM, FREELOOK, GIZMO, MOVE_2D }

var bridge: Object
## Callable(node) -> [min, max] local bounds of an imported mesh part (empty if unknown).
var mesh_bounds: Callable
## Callable(node) -> Rect2 of a 2D Control in Jet screen pixels.
var control_rect: Callable
var root: Dictionary = {}
var selected := 0
var playing := false
var tool := Tool.MOVE
var local_space := false
var snap := false
var show_grid := true
var preview_camera := false

# Editor camera ("cursor" like Godot's): orbit pivot, distance and angles.
var pivot := Vector3(0, 0.5, 0)
var distance := 7.0
var yaw := deg_to_rad(35.0)
var pitch := deg_to_rad(-28.0)
var fov := 70.0
var fly_speed := 4.0

var view_origin := Vector3.ZERO
var view_basis := Basis.IDENTITY
var view_fov := 70.0

var area: Control
var image: TextureRect
var overlay: Control
var stats: Label
var _tool_buttons := []
var _local_button: Button
var _snap_button: Button
var _preview_button: Button
var _view_menu: MenuButton
var _badge: Label
var _nav := Nav.NONE
var _press := Vector2.ZERO
var _left_down := false
var _hover_axis := -1
var _drag := {}
var _gizmo_icons := [] # [{id, position}]

func _ready() -> void:
	add_theme_constant_override("separation", 2)
	var bar := HBoxContainer.new()
	bar.add_theme_constant_override("separation", 2)
	add_child(bar)
	var tools := [["ToolSelect", "Select Mode (Q)"], ["ToolMove", "Move Mode (W)"],
		["ToolRotate", "Rotate Mode (E)"], ["ToolScale", "Scale Mode (R)"]]
	var group := ButtonGroup.new()
	for i in tools.size():
		var button := _bar_button(tools[i][0], tools[i][1], true)
		button.button_group = group
		button.button_pressed = i == tool
		var index := i
		button.pressed.connect(func(): _set_tool(index))
		bar.add_child(button)
		_tool_buttons.append(button)
	bar.add_child(VSeparator.new())
	_local_button = _bar_button("Object", "Use Local Space (T)", true)
	_local_button.toggled.connect(func(on): local_space = on; overlay.queue_redraw())
	bar.add_child(_local_button)
	_snap_button = _bar_button("Snap", "Snap: 0.5 m · 15° · 10% (Y)", true)
	_snap_button.toggled.connect(func(on): snap = on)
	bar.add_child(_snap_button)
	bar.add_child(VSeparator.new())
	_view_menu = MenuButton.new()
	_view_menu.text = "View"
	_view_menu.tooltip_text = "Preset views and 3D view options"
	var popup := _view_menu.get_popup()
	popup.add_item("Top View (Kp 7)", 0)
	popup.add_item("Bottom View (Ctrl+Kp 7)", 1)
	popup.add_item("Front View (Kp 1)", 2)
	popup.add_item("Rear View (Ctrl+Kp 1)", 3)
	popup.add_item("Right View (Kp 3)", 4)
	popup.add_item("Left View (Ctrl+Kp 3)", 5)
	popup.add_separator()
	popup.add_item("Focus Selection (F)", 6)
	popup.add_item("Reset View", 7)
	popup.add_separator()
	popup.add_check_item("Show Grid", 8)
	popup.set_item_checked(popup.get_item_index(8), true)
	popup.id_pressed.connect(_on_view_menu)
	bar.add_child(_view_menu)
	_preview_button = _bar_button("Camera3D", "Preview: look through the selected Camera3D", true)
	_preview_button.text = "Preview"
	_preview_button.visible = false
	_preview_button.toggled.connect(func(on):
		preview_camera = on
		_update_view())
	bar.add_child(_preview_button)
	var spacer := Control.new()
	spacer.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	bar.add_child(spacer)
	stats = Label.new()
	stats.add_theme_color_override("font_color", Style.MUTED)
	stats.add_theme_font_size_override("font_size", 12)
	bar.add_child(stats)
	var frame := PanelContainer.new()
	frame.theme_type_variation = "ViewportFrame"
	frame.size_flags_vertical = Control.SIZE_EXPAND_FILL
	add_child(frame)
	area = Control.new()
	area.clip_contents = true
	area.focus_mode = Control.FOCUS_CLICK
	area.mouse_filter = Control.MOUSE_FILTER_STOP
	area.gui_input.connect(_input_event)
	area.set_drag_forwarding(Callable(), _can_drop_file, _drop_file)
	area.resized.connect(_layout)
	area.mouse_exited.connect(func():
		_hover_axis = -1
		overlay.queue_redraw())
	frame.add_child(area)
	var background := ColorRect.new()
	background.color = Style.VIEWPORT
	background.set_anchors_and_offsets_preset(Control.PRESET_FULL_RECT)
	background.mouse_filter = Control.MOUSE_FILTER_IGNORE
	area.add_child(background)
	image = TextureRect.new()
	image.expand_mode = TextureRect.EXPAND_IGNORE_SIZE
	image.stretch_mode = TextureRect.STRETCH_SCALE
	image.texture_filter = CanvasItem.TEXTURE_FILTER_NEAREST
	image.mouse_filter = Control.MOUSE_FILTER_IGNORE
	area.add_child(image)
	overlay = Control.new()
	overlay.clip_contents = true
	overlay.mouse_filter = Control.MOUSE_FILTER_IGNORE
	overlay.draw.connect(_draw_overlay)
	area.add_child(overlay)
	_badge = Label.new()
	_badge.position = Vector2(12, 9)
	_badge.add_theme_font_override("font", Style.bold_font)
	_badge.add_theme_font_size_override("font_size", 11)
	_badge.mouse_filter = Control.MOUSE_FILTER_IGNORE
	area.add_child(_badge)
	_update_view()

func _bar_button(icon: String, tooltip: String, toggle: bool) -> Button:
	var button := Button.new()
	button.theme_type_variation = "FlatButton"
	button.icon = Icons.get_icon(icon)
	button.tooltip_text = tooltip
	button.toggle_mode = toggle
	button.focus_mode = Control.FOCUS_NONE
	return button

func _set_tool(index: int) -> void:
	tool = index
	_tool_buttons[index].button_pressed = true
	overlay.queue_redraw()

func set_resolution(size: Vector2i) -> void:
	if Vector2(size) == Vector2(W, H): return
	W = size.x
	H = size.y
	_layout()
	if not root.is_empty(): set_scene(root, selected, playing)

func set_frame(texture: Texture2D) -> void:
	image.texture = texture

func set_scene(new_root: Dictionary, new_selected: int, is_playing: bool) -> void:
	root = new_root
	selected = new_selected
	playing = is_playing
	var node := Document.find(root, selected) if selected > 0 else {}
	var is_camera := not node.is_empty() and str(node.type) == "Camera3D"
	_preview_button.visible = is_camera
	if not is_camera and preview_camera:
		preview_camera = false
		_preview_button.set_pressed_no_signal(false)
		_update_view()
	elif preview_camera:
		_update_view()
	if playing:
		_badge.text = "▶  GAME RUNNING · editor view"
		_badge.add_theme_color_override("font_color", Style.WARNING)
	elif preview_camera:
		_badge.text = "Camera preview: " + str(node.get("name", ""))
		_badge.add_theme_color_override("font_color", Style.ACCENT)
	else:
		_badge.text = "PERSPECTIVE  ·  JET %d×%d" % [int(W), int(H)]
		_badge.add_theme_color_override("font_color", Color(Style.MUTED, 0.9))
	overlay.queue_redraw()

func _layout() -> void:
	var rect := _image_rect()
	image.position = rect.position
	image.size = rect.size
	overlay.position = rect.position
	overlay.size = rect.size
	overlay.queue_redraw()

func _image_rect() -> Rect2:
	var k := minf(area.size.x / W, area.size.y / H)
	var fitted := Vector2(W, H) * k
	return Rect2((area.size - fitted) * 0.5, fitted)

# ------------------------------------------------------------------ camera

func editor_basis() -> Basis:
	return Basis.from_euler(Vector3(pitch, yaw, 0))

func editor_eye() -> Vector3:
	return pivot + editor_basis() * Vector3(0, 0, distance)

func _update_view() -> void:
	var node := Document.find(root, selected) if preview_camera and selected > 0 else {}
	if not node.is_empty() and str(node.type) == "Camera3D":
		var t := Document.global_transform(root, selected)
		view_origin = t.origin
		view_basis = t.basis.orthonormalized()
		view_fov = float(node.get("fov", 75.0))
	else:
		view_origin = editor_eye()
		view_basis = editor_basis()
		view_fov = fov
	camera_changed.emit()
	if overlay: overlay.queue_redraw()

## Camera rotation for the runtime (YXZ Euler, radians).
func view_rotation() -> Vector3:
	return view_basis.get_euler()

func save_state() -> Dictionary:
	return {"pivot": [pivot.x, pivot.y, pivot.z], "distance": distance, "yaw": yaw, "pitch": pitch}

func load_state(state: Dictionary) -> void:
	if state.is_empty(): return
	var p: Array = state.get("pivot", [0, 0.5, 0])
	pivot = Vector3(p[0], p[1], p[2])
	distance = float(state.get("distance", 7.0))
	yaw = float(state.get("yaw", yaw))
	pitch = float(state.get("pitch", pitch))
	_update_view()

func reset_view() -> void:
	pivot = Vector3(0, 0.5, 0)
	distance = 7.0
	yaw = deg_to_rad(35.0)
	pitch = deg_to_rad(-28.0)
	_update_view()

func focus_selection() -> void:
	if selected <= 0: return
	var node := Document.find(root, selected)
	if node.is_empty() or not Document.is_3d(str(node.type)): return
	var t := Document.global_transform(root, selected)
	var radius := 1.0
	if str(node.type) == "MeshInstance3D":
		var bounds := _bounds_of(node)
		radius = maxf((t.basis * (bounds[1] - bounds[0])).length() * 0.5, 0.3)
		t.origin = t * ((bounds[0] + bounds[1]) * 0.5)
	pivot = t.origin
	distance = clampf(radius * 3.0, 1.0, 200.0)
	_update_view()

func _on_view_menu(id: int) -> void:
	match id:
		0: _set_angles(0, -PI * 0.5 + 0.0001)
		1: _set_angles(0, PI * 0.5 - 0.0001)
		2: _set_angles(0, 0)
		3: _set_angles(PI, 0)
		4: _set_angles(PI * 0.5, 0)
		5: _set_angles(-PI * 0.5, 0)
		6: focus_selection()
		7: reset_view()
		8:
			show_grid = not show_grid
			var popup := _view_menu.get_popup()
			popup.set_item_checked(popup.get_item_index(8), show_grid)
			overlay.queue_redraw()

func _set_angles(new_yaw: float, new_pitch: float) -> void:
	yaw = new_yaw
	pitch = new_pitch
	_update_view()

func _process(delta: float) -> void:
	if _nav != Nav.FREELOOK: return
	var direction := Vector3.ZERO
	if Input.is_physical_key_pressed(KEY_W): direction.z -= 1
	if Input.is_physical_key_pressed(KEY_S): direction.z += 1
	if Input.is_physical_key_pressed(KEY_A): direction.x -= 1
	if Input.is_physical_key_pressed(KEY_D): direction.x += 1
	if Input.is_physical_key_pressed(KEY_E): direction.y += 1
	if Input.is_physical_key_pressed(KEY_Q): direction.y -= 1
	if direction == Vector3.ZERO: return
	var speed := fly_speed * (3.0 if Input.is_key_pressed(KEY_SHIFT) else 1.0) * (0.25 if Input.is_key_pressed(KEY_ALT) else 1.0)
	var basis := editor_basis()
	var motion := (basis * direction.normalized()) * speed * delta
	if direction.y != 0 and direction.x == 0 and direction.z == 0: motion = Vector3(0, direction.y, 0) * speed * delta
	pivot += motion
	_update_view()

# ------------------------------------------------------------------ projection

func _pixels_per_unit() -> float:
	return _image_rect().size.x / W

func _focal() -> float:
	return (H * 0.5) / tan(deg_to_rad(view_fov) * 0.5)

func _to_camera(p: Vector3) -> Vector3:
	var rel := p - view_origin
	return Vector3(view_basis.x.dot(rel), view_basis.y.dot(rel), view_basis.z.dot(rel))

## Screen position (area coordinates) of a camera-space point in front of the camera.
func _camera_to_screen(c: Vector3) -> Vector2:
	var depth := -c.z
	var f := _focal()
	var virtual := Vector2(W * 0.5 + c.x * f / depth, H * 0.5 - c.y * f / depth)
	var rect := _image_rect()
	return rect.position + virtual * (rect.size.x / W)

func project(p: Vector3) -> Variant:
	var c := _to_camera(p)
	if -c.z < NEAR: return null
	return _camera_to_screen(c)

func ray(screen: Vector2) -> Array:
	var rect := _image_rect()
	var virtual := (screen - rect.position) / (rect.size.x / W)
	var f := _focal()
	var direction := Vector3((virtual.x - W * 0.5) / f, -(virtual.y - H * 0.5) / f, -1.0)
	return [view_origin, (view_basis * direction).normalized()]

## World units covered by one screen pixel at point p.
func _world_per_pixel(p: Vector3) -> float:
	var depth := maxf(-_to_camera(p).z, NEAR)
	return depth / (_focal() * _pixels_per_unit())

func _segment(a: Vector3, b: Vector3, color: Color, width: float = 1.0) -> void:
	var ca := _to_camera(a)
	var cb := _to_camera(b)
	var da := -ca.z
	var db := -cb.z
	if da < NEAR and db < NEAR: return
	if da < NEAR: ca = ca.lerp(cb, (NEAR - da) / (db - da))
	elif db < NEAR: cb = cb.lerp(ca, (NEAR - db) / (da - db))
	overlay.draw_line(_camera_to_screen(ca), _camera_to_screen(cb), color, width, true)

# ------------------------------------------------------------------ overlay

func _draw_overlay() -> void:
	var rect := _image_rect()
	_gizmo_icons.clear()
	if root.is_empty(): return
	# The overlay is clipped to the Jet image; draw in area coordinates.
	overlay.draw_set_transform(-overlay.position)
	if show_grid and not preview_camera: _draw_grid()
	var nodes := []
	Document.walk(root, func(n, _p, _d): nodes.append(n))
	for node in nodes:
		var type := str(node.type)
		if type == "CollisionShape3D":
			_draw_collision(node, Document.global_transform(root, int(node.id)), int(node.id) == selected)
			continue
		if type == "CPUParticles3D" and not playing:
			var t := Document.global_transform(root, int(node.id))
			_draw_particles(node, t, int(node.id) == selected)
			var point = project(t.origin)
			if point != null and rect.has_point(point):
				var icon := Icons.get_icon("CPUParticles3D")
				if icon:
					overlay.draw_texture_rect(icon, Rect2(point - Vector2(10, 10), Vector2(20, 20)), false,
						Color(1, 1, 1, 0.95 if int(node.id) == selected else 0.7))
				_gizmo_icons.append({"id": int(node.id), "position": point})
			continue
		if type == "Camera3D" or type == "DirectionalLight3D":
			var t := Document.global_transform(root, int(node.id))
			if preview_camera and int(node.id) == selected: continue
			var is_selected := int(node.id) == selected
			if type == "Camera3D": _draw_camera(t, float(node.get("fov", 75.0)), is_selected)
			else: _draw_light(t, is_selected)
			var point = project(t.origin)
			if point != null and rect.has_point(point):
				var icon := Icons.get_icon("GizmoCamera3D" if type == "Camera3D" else "GizmoDirectionalLight")
				if icon:
					overlay.draw_texture_rect(icon, Rect2(point - Vector2(12, 12), Vector2(24, 24)), false,
						Color(1, 1, 1, 0.95 if is_selected else 0.7))
				_gizmo_icons.append({"id": int(node.id), "position": point})
	var node := Document.find(root, selected) if selected > 0 else {}
	if not node.is_empty() and Document.is_control(str(node.type)) and control_rect.is_valid():
		_draw_control(control_rect.call(node), rect)
		return
	if node.is_empty() or not Document.is_3d(str(node.type)) or preview_camera: return
	var global := Document.global_transform(root, selected)
	if str(node.type) == "MeshInstance3D":
		var bounds := _bounds_of(node)
		_draw_box(global, bounds[0], bounds[1], Color("ffb35c"))
	if playing: return
	match tool:
		Tool.MOVE: _draw_move(global)
		Tool.ROTATE: _draw_rotate(global)
		Tool.SCALE: _draw_scale(global)
		Tool.SELECT: _draw_axes_cross(global)

func _bounds_of(node: Dictionary) -> Array:
	var mesh := str(node.get("mesh", "Box"))
	if mesh == "File":
		var bounds: Array = mesh_bounds.call(node) if mesh_bounds.is_valid() else []
		return bounds if not bounds.is_empty() else [Vector3(-0.1, -0.1, -0.1), Vector3(0.1, 0.1, 0.1)]
	return Document.MESH_BOUNDS.get(mesh, Document.MESH_BOUNDS.Box)

## Selection frame of a 2D node (Jet pixels mapped onto the displayed image).
func _draw_control(box: Rect2, image_rect: Rect2) -> void:
	var k := image_rect.size.x / W
	var frame := Rect2(image_rect.position + box.position * k, box.size * k)
	var color := Color("ffb35c")
	overlay.draw_rect(frame, color, false, 1.5)
	for corner in [frame.position, Vector2(frame.end.x, frame.position.y), frame.end, Vector2(frame.position.x, frame.end.y)]:
		overlay.draw_rect(Rect2(corner - Vector2(3, 3), Vector2(6, 6)), color)

## Topmost visible 2D node under a Jet pixel (later nodes draw on top), or 0.
func _control_at(virtual: Vector2) -> int:
	if not control_rect.is_valid(): return 0
	var found := [0]
	var hidden := {}
	Document.walk(root, func(n, p, _d):
		if not bool(n.get("visible", true)) or hidden.has(int(p.get("id", 0))):
			hidden[int(n.id)] = true
			return
		if Document.is_control(str(n.type)) and (control_rect.call(n) as Rect2).has_point(virtual): found[0] = int(n.id))
	return found[0]

func _virtual(at: Vector2) -> Vector2:
	var rect := _image_rect()
	return (at - rect.position) / (rect.size.x / W)

func _begin_move_2d(id: int, at: Vector2) -> void:
	var node := Document.find(root, id)
	var p: Array = node.get("position", [0, 0])
	_drag = {"id": id, "origin": Vector2(float(p[0]), float(p[1])), "press": _virtual(at)}
	_nav = Nav.MOVE_2D

func _update_move_2d(at: Vector2, final: bool) -> void:
	if _drag.is_empty(): return
	var target: Vector2 = (_drag.origin + _virtual(at) - _drag.press).round()
	if snap: target = target.snapped(Vector2(8, 8))
	if target != _drag.origin: _drag.moved = true
	# A plain click selects without leaving an empty undo step.
	if _drag.get("moved", false): control_moved.emit(int(_drag.id), target, final)

func _draw_grid() -> void:
	var step := 1.0 if distance < 40 else 10.0
	var extent := int(clampf(distance * 2.5, 10.0, 200.0) / step)
	var center := Vector3(snappedf(pivot.x, step), 0, snappedf(pivot.z, step))
	for i in range(-extent, extent + 1):
		var x := center.x + i * step
		var z := center.z + i * step
		var fade := 1.0 - absf(float(i)) / extent
		var minor := Color(1, 1, 1, 0.06 * fade + 0.02)
		var major := Color(1, 1, 1, 0.11 * fade + 0.03)
		var cx := minor if int(round(x / step)) % 5 != 0 else major
		var cz := minor if int(round(z / step)) % 5 != 0 else major
		if absf(x) < 0.0001: cx = Color(Style.AXIS[2], 0.55)
		if absf(z) < 0.0001: cz = Color(Style.AXIS[0], 0.55)
		_segment(Vector3(x, 0, center.z - extent * step), Vector3(x, 0, center.z + extent * step), cx)
		_segment(Vector3(center.x - extent * step, 0, z), Vector3(center.x + extent * step, 0, z), cz)

func _draw_box(t: Transform3D, low: Vector3, high: Vector3, color: Color) -> void:
	var corners := []
	for i in 8:
		corners.append(t * Vector3(high.x if i & 1 else low.x, high.y if i & 2 else low.y, high.z if i & 4 else low.z))
	for edge in [[0, 1], [2, 3], [4, 5], [6, 7], [0, 2], [1, 3], [4, 6], [5, 7], [0, 4], [1, 5], [2, 6], [3, 7]]:
		_segment(corners[edge[0]], corners[edge[1]], color, 1.5)

## Collision shapes in Godot's debug colour (cyan; dim when disabled).
func _draw_collision(node: Dictionary, t: Transform3D, is_selected: bool) -> void:
	var color := Color(0.35, 0.85, 1.0, 0.95 if is_selected else 0.6)
	if bool(node.get("disabled", false)): color = Color(0.6, 0.6, 0.6, 0.35)
	match str(node.get("shape", "Box")):
		"Box":
			var s: Array = node.get("size", [1, 1, 1])
			var half := Vector3(float(s[0]), float(s[1]), float(s[2])) * 0.5
			_draw_box(t, -half, half, color)
		"Sphere":
			var r := float(node.get("radius", 0.5))
			for axes in [[Vector3.RIGHT, Vector3.UP], [Vector3.RIGHT, Vector3.BACK], [Vector3.UP, Vector3.BACK]]:
				_draw_circle(t, Vector3.ZERO, axes[0] * r, axes[1] * r, color)
		"Capsule":
			var r := float(node.get("radius", 0.5))
			var half := maxf(float(node.get("height", 2.0)) * 0.5 - r, 0.0)
			for y in [-half, half]: _draw_circle(t, Vector3(0, y, 0), Vector3.RIGHT * r, Vector3.BACK * r, color)
			for side in [Vector3.RIGHT, Vector3.BACK, Vector3.LEFT, Vector3.FORWARD]:
				_segment(t * (side * r + Vector3(0, -half, 0)), t * (side * r + Vector3(0, half, 0)), color, 1.0)
			for side in [Vector3.RIGHT, Vector3.BACK]:
				_draw_arc(t, Vector3(0, half, 0), side * r, Vector3.UP * r, color)
				_draw_arc(t, Vector3(0, -half, 0), side * r, Vector3.DOWN * r, color)

## CPUParticles3D: emission shape and the spread cone around Direction (1 m long).
func _draw_particles(node: Dictionary, t: Transform3D, is_selected: bool) -> void:
	var color := Color(1.0, 0.78, 0.3, 0.95 if is_selected else 0.55)
	match str(node.get("emission_shape", "Point")):
		"Sphere":
			var r := float(node.get("emission_sphere_radius", 1.0))
			for axes in [[Vector3.RIGHT, Vector3.UP], [Vector3.RIGHT, Vector3.BACK], [Vector3.UP, Vector3.BACK]]:
				_draw_circle(t, Vector3.ZERO, axes[0] * r, axes[1] * r, color)
		"Box":
			var e: Array = node.get("emission_box_extents", [1, 1, 1])
			var half := Vector3(float(e[0]), float(e[1]), float(e[2]))
			_draw_box(t, -half, half, color)
	if not is_selected: return
	var d: Array = node.get("direction", [1, 0, 0])
	var axis := Vector3(float(d[0]), float(d[1]), float(d[2]))
	if axis.length_squared() < 1e-8: return
	axis = axis.normalized()
	var u := axis.cross(Vector3.UP if absf(axis.y) < 0.9 else Vector3.RIGHT).normalized()
	var v := axis.cross(u)
	var spread := deg_to_rad(clampf(float(node.get("spread", 45.0)), 0.0, 180.0))
	var frame := Transform3D(t.basis.orthonormalized(), t.origin)
	var tip := axis * cos(spread)
	var ring := sin(spread)
	_segment(t.origin, frame * axis, color, 1.5)
	for side in [u, -u, v, -v]: _segment(t.origin, frame * (tip + side * ring), color, 1.0)
	_draw_circle(frame, tip, u * ring, v * ring, color)

func _draw_circle(t: Transform3D, center: Vector3, u: Vector3, v: Vector3, color: Color) -> void:
	var previous := t * (center + u)
	for i in range(1, 33):
		var a := TAU * i / 32.0
		var point := t * (center + u * cos(a) + v * sin(a))
		_segment(previous, point, color, 1.0)
		previous = point

## Half circle from -u through v to +u (capsule caps).
func _draw_arc(t: Transform3D, center: Vector3, u: Vector3, v: Vector3, color: Color) -> void:
	var previous := t * (center - u)
	for i in range(1, 17):
		var a := PI * i / 16.0
		var point := t * (center - u * cos(a) + v * sin(a))
		_segment(previous, point, color, 1.0)
		previous = point

func _draw_camera(t: Transform3D, camera_fov: float, is_selected: bool) -> void:
	var color := Color("d28cff") if is_selected else Color(0.82, 0.55, 1.0, 0.45)
	var b := t.basis.orthonormalized()
	var depth := 0.8
	var half_h := tan(deg_to_rad(camera_fov) * 0.5) * depth
	var half_w := half_h * W / H
	var o := t.origin
	var corners := []
	for c in [Vector2(-1, -1), Vector2(1, -1), Vector2(1, 1), Vector2(-1, 1)]:
		corners.append(o + b * Vector3(c.x * half_w, c.y * half_h, -depth))
	for i in 4:
		_segment(o, corners[i], color)
		_segment(corners[i], corners[(i + 1) % 4], color)
	_segment(o + b * Vector3(-half_w * 0.4, half_h * 1.15, -depth), o + b * Vector3(half_w * 0.4, half_h * 1.15, -depth), color)
	_segment(o + b * Vector3(half_w * 0.4, half_h * 1.15, -depth), o + b * Vector3(0, half_h * 1.5, -depth), color)
	_segment(o + b * Vector3(0, half_h * 1.5, -depth), o + b * Vector3(-half_w * 0.4, half_h * 1.15, -depth), color)

func _draw_light(t: Transform3D, is_selected: bool) -> void:
	var color := Color("ffd76a") if is_selected else Color(1.0, 0.84, 0.42, 0.5)
	var direction := -t.basis.orthonormalized().z
	var length := 1.2
	var tip := t.origin + direction * length
	_segment(t.origin, tip, color, 2.0 if is_selected else 1.0)
	var side := direction.cross(Vector3.UP if absf(direction.y) < 0.9 else Vector3.RIGHT).normalized() * 0.12
	_segment(tip, tip - direction * 0.25 + side, color)
	_segment(tip, tip - direction * 0.25 - side, color)

func _gizmo_axes(t: Transform3D, local: bool) -> Array:
	if local:
		var b := t.basis.orthonormalized()
		return [b.x, b.y, b.z]
	return [Vector3.RIGHT, Vector3.UP, Vector3.BACK]

func _axis_color(axis: int) -> Color:
	var color: Color = Style.AXIS[axis]
	if axis == _hover_axis or (not _drag.is_empty() and _drag.axis == axis): return color.lightened(0.45)
	return color

func _draw_move(t: Transform3D) -> void:
	var length := GIZMO_PIXELS * _world_per_pixel(t.origin)
	var axes := _gizmo_axes(t, local_space)
	for axis in 3:
		var tip: Vector3 = t.origin + axes[axis] * length
		_segment(t.origin, tip, _axis_color(axis), 3.0)
		var a = project(t.origin + axes[axis] * length * 0.82)
		var b = project(tip)
		if a != null and b != null:
			var direction: Vector2 = (b - a).normalized()
			var normal := Vector2(-direction.y, direction.x) * 6.0
			overlay.draw_colored_polygon(PackedVector2Array([b + direction * 6.0, a + normal, a - normal]), _axis_color(axis))
	var center = project(t.origin)
	if center != null: overlay.draw_circle(center, 3.5, Color(1, 1, 1, 0.9))

func _draw_scale(t: Transform3D) -> void:
	var length := GIZMO_PIXELS * _world_per_pixel(t.origin)
	var axes := _gizmo_axes(t, true)
	for axis in 3:
		var tip: Vector3 = t.origin + axes[axis] * length
		_segment(t.origin, tip, _axis_color(axis), 3.0)
		var b = project(tip)
		if b != null: overlay.draw_rect(Rect2(b - Vector2(5, 5), Vector2(10, 10)), _axis_color(axis))
	var center = project(t.origin)
	if center != null: overlay.draw_circle(center, 3.5, Color(1, 1, 1, 0.9))

func _ring_points(t: Transform3D, axis: int) -> Array:
	var axes := _gizmo_axes(t, local_space)
	var radius := GIZMO_PIXELS * _world_per_pixel(t.origin)
	var u: Vector3 = axes[(axis + 1) % 3]
	var v: Vector3 = axes[(axis + 2) % 3]
	var points := []
	for i in 49:
		var angle := TAU * i / 48.0
		points.append(t.origin + (u * cos(angle) + v * sin(angle)) * radius)
	return points

func _draw_rotate(t: Transform3D) -> void:
	for axis in 3:
		var points := _ring_points(t, axis)
		for i in 48:
			_segment(points[i], points[i + 1], _axis_color(axis), 2.5)
	var center = project(t.origin)
	if center != null: overlay.draw_circle(center, 3.0, Color(1, 1, 1, 0.8))

func _draw_axes_cross(t: Transform3D) -> void:
	var length := 30.0 * _world_per_pixel(t.origin)
	var axes := _gizmo_axes(t, true)
	for axis in 3:
		_segment(t.origin, t.origin + axes[axis] * length, Color(Style.AXIS[axis], 0.8), 1.5)

# ------------------------------------------------------------------ input

func _input_event(event: InputEvent) -> void:
	if event is InputEventMouseButton: _mouse_button(event)
	elif event is InputEventMouseMotion: _mouse_motion(event)
	elif event is InputEventPanGesture:
		if event.shift_pressed: _pan(-event.delta * 12.0)
		else: _orbit(-event.delta * 12.0)
		area.accept_event()
	elif event is InputEventMagnifyGesture:
		_zoom(1.0 / event.factor)
		area.accept_event()
	elif event is InputEventKey and event.pressed and not event.echo:
		_key(event)

func _mouse_button(event: InputEventMouseButton) -> void:
	area.grab_focus()
	var emulate: bool = event.button_index == MOUSE_BUTTON_LEFT and event.alt_pressed
	if event.button_index == MOUSE_BUTTON_MIDDLE or emulate:
		if event.pressed and not preview_camera:
			_nav = Nav.PAN if event.shift_pressed else (Nav.ZOOM if event.ctrl_pressed else Nav.ORBIT)
		elif not event.pressed and _nav in [Nav.ORBIT, Nav.PAN, Nav.ZOOM]:
			_nav = Nav.NONE
	elif event.button_index == MOUSE_BUTTON_RIGHT:
		if event.pressed and not preview_camera:
			_nav = Nav.FREELOOK
			Input.mouse_mode = Input.MOUSE_MODE_CAPTURED
		elif not event.pressed and _nav == Nav.FREELOOK:
			_nav = Nav.NONE
			Input.mouse_mode = Input.MOUSE_MODE_VISIBLE
			if DisplayServer.get_name() != "Wayland": area.warp_mouse(event.position)
	elif event.button_index == MOUSE_BUTTON_LEFT:
		if event.pressed:
			_left_down = true
			_press = event.position
			if not playing and _hover_axis >= 0 and tool != Tool.SELECT: _begin_drag(event.position)
			elif not playing and not preview_camera and _image_rect().has_point(event.position):
				# 2D nodes are moved by dragging them, as in Godot's 2D editor.
				var control := _control_at(_virtual(event.position))
				if control > 0:
					if control != selected: node_picked.emit(control)
					_begin_move_2d(control, event.position)
		else:
			_left_down = false
			if _nav == Nav.GIZMO:
				_end_drag()
			elif _nav == Nav.MOVE_2D:
				_nav = Nav.NONE
				_update_move_2d(event.position, true)
				_drag = {}
			elif _press.distance_to(event.position) < 4.0:
				_click(event.position)
	elif event.pressed and event.button_index in [MOUSE_BUTTON_WHEEL_UP, MOUSE_BUTTON_WHEEL_DOWN]:
		var up: bool = event.button_index == MOUSE_BUTTON_WHEEL_UP
		if _nav == Nav.FREELOOK:
			fly_speed = clampf(fly_speed * (1.15 if up else 1 / 1.15), 0.2, 200.0)
		elif not preview_camera:
			_zoom(1 / 1.08 if up else 1.08)
	area.accept_event()

func _mouse_motion(event: InputEventMouseMotion) -> void:
	match _nav:
		Nav.ORBIT: _orbit(event.relative)
		Nav.PAN: _pan(event.relative)
		Nav.ZOOM: _zoom(exp(event.relative.y * 0.01))
		Nav.FREELOOK:
			var eye := editor_eye()
			yaw -= event.relative.x * ORBIT_SPEED
			pitch = clampf(pitch - event.relative.y * ORBIT_SPEED, -PI * 0.5 + 0.001, PI * 0.5 - 0.001)
			pivot = eye - editor_basis() * Vector3(0, 0, distance)
			_update_view()
		Nav.GIZMO: _update_drag(event.position)
		Nav.MOVE_2D: _update_move_2d(event.position, false)
		_:
			var axis := _pick_axis(event.position) if not playing and not _left_down else -1
			if axis != _hover_axis:
				_hover_axis = axis
				overlay.queue_redraw()
	area.accept_event()

func _orbit(relative: Vector2) -> void:
	yaw -= relative.x * ORBIT_SPEED
	pitch = clampf(pitch - relative.y * ORBIT_SPEED, -PI * 0.5 + 0.001, PI * 0.5 - 0.001)
	_update_view()

func _pan(relative: Vector2) -> void:
	var per_pixel := distance / (_focal() * _pixels_per_unit())
	var basis := editor_basis()
	pivot += basis.x * (-relative.x * per_pixel) + basis.y * (relative.y * per_pixel)
	_update_view()

func _zoom(factor: float) -> void:
	distance = clampf(distance * factor, 0.05, 2000.0)
	_update_view()

func _key(event: InputEventKey) -> void:
	if event.ctrl_pressed and event.keycode == KEY_D:
		duplicate_requested.emit()
	elif event.keycode == KEY_DELETE:
		delete_requested.emit()
	elif event.ctrl_pressed or event.alt_pressed or _nav == Nav.FREELOOK:
		return
	else:
		match event.keycode:
			KEY_Q: _set_tool(Tool.SELECT)
			KEY_W: _set_tool(Tool.MOVE)
			KEY_E: _set_tool(Tool.ROTATE)
			KEY_R: _set_tool(Tool.SCALE)
			KEY_T: _local_button.button_pressed = not _local_button.button_pressed
			KEY_Y: _snap_button.button_pressed = not _snap_button.button_pressed
			KEY_F: focus_selection()
			KEY_KP_7: _on_view_menu(1 if event.ctrl_pressed else 0)
			KEY_KP_1: _on_view_menu(3 if event.ctrl_pressed else 2)
			KEY_KP_3: _on_view_menu(5 if event.ctrl_pressed else 4)
			_: return
	area.accept_event()

func _click(at: Vector2) -> void:
	for icon in _gizmo_icons:
		if at.distance_to(icon.position) < 14.0:
			node_picked.emit(int(icon.id))
			return
	if bridge == null: return
	var rect := _image_rect()
	if not rect.has_point(at):
		node_picked.emit(-1)
		return
	var virtual := (at - rect.position) / (rect.size.x / W)
	var hit: int = bridge.pick(int(virtual.x), int(virtual.y))
	node_picked.emit(hit)

## A PNG dropped on a mesh textures it, like in Godot. OBJ models land where the cursor meets the floor (y = 0).
func _can_drop_file(_at: Vector2, data: Variant) -> bool:
	if playing or not data is Dictionary or data.get("type") != "files": return false
	for file in data.get("files", []):
		if str(file).get_extension().to_lower() in ["png", "obj"]: return true
	return false

func _drop_file(at: Vector2, data: Variant) -> void:
	var rect := _image_rect()
	if bridge == null or not rect.has_point(at): return
	for file in data.files:
		if Document.import_kind(str(file)) == "model":
			var hit = _plane_hit(at, Vector3.ZERO, Vector3.UP)
			if hit == null or (hit - view_origin).length() > 200.0: hit = pivot
			model_dropped.emit(str(file), Vector3(snappedf(hit.x, 0.01), snappedf(hit.y, 0.01), snappedf(hit.z, 0.01)))
			return
	var virtual := (at - rect.position) / (rect.size.x / W)
	var hit: int = bridge.pick(int(virtual.x), int(virtual.y))
	if hit <= 0: return
	for file in data.files:
		if str(file).get_extension().to_lower() == "png":
			file_dropped.emit(hit, str(file))
			return

# ------------------------------------------------------------------ gizmo dragging

func _distance_to_segment(p: Vector2, a: Vector2, b: Vector2) -> float:
	var ab := b - a
	var t := clampf((p - a).dot(ab) / maxf(ab.length_squared(), 0.0001), 0, 1)
	return p.distance_to(a + ab * t)

func _pick_axis(at: Vector2) -> int:
	if selected <= 0 or tool == Tool.SELECT or preview_camera: return -1
	var node := Document.find(root, selected)
	if node.is_empty() or not Document.is_3d(str(node.type)): return -1
	var t := Document.global_transform(root, selected)
	var best := -1
	var best_distance := 9.0
	if tool == Tool.ROTATE:
		for axis in 3:
			var points := _ring_points(t, axis)
			for i in 48:
				var a = project(points[i])
				var b = project(points[i + 1])
				if a == null or b == null: continue
				var d := _distance_to_segment(at, a, b)
				if d < best_distance:
					best_distance = d
					best = axis
		return best
	var length := GIZMO_PIXELS * _world_per_pixel(t.origin)
	var axes := _gizmo_axes(t, local_space or tool == Tool.SCALE)
	var origin = project(t.origin)
	if origin == null: return -1
	for axis in 3:
		var tip = project(t.origin + axes[axis] * length)
		if tip == null: continue
		var d := _distance_to_segment(at, origin, tip)
		if d < best_distance:
			best_distance = d
			best = axis
	return best

## Parameter along the line (origin, axis) closest to the mouse ray.
func _axis_param(at: Vector2, origin: Vector3, axis: Vector3) -> Variant:
	var r := ray(at)
	var o: Vector3 = r[0]
	var d: Vector3 = r[1]
	var w0 := origin - o
	var b := axis.dot(d)
	var denominator := 1.0 - b * b
	if denominator < 0.0001: return null
	return (b * d.dot(w0) - axis.dot(w0)) / denominator

func _plane_hit(at: Vector2, origin: Vector3, normal: Vector3) -> Variant:
	var r := ray(at)
	var denominator: float = normal.dot(r[1])
	if absf(denominator) < 0.0001: return null
	var t: float = normal.dot(origin - r[0]) / denominator
	if t < 0: return null
	return r[0] + r[1] * t

func _begin_drag(at: Vector2) -> void:
	var node := Document.find(root, selected)
	var global := Document.global_transform(root, selected)
	var parent := Document.parent_of(root, selected)
	var parent_global := Document.global_transform(root, int(parent.id)) if not parent.is_empty() and Document.is_3d(str(parent.type)) else Transform3D.IDENTITY
	var axes := _gizmo_axes(global, local_space or tool == Tool.SCALE)
	var axis: Vector3 = axes[_hover_axis].normalized()
	_drag = {"axis": _hover_axis, "dir": axis, "global": global, "parent": parent_global, "local": Document.local_transform(node),
		"scale": Vector3(node.scale[0], node.scale[1], node.scale[2])}
	if tool == Tool.ROTATE:
		var hit = _plane_hit(at, global.origin, axis)
		if hit == null:
			_drag = {}
			return
		_drag.start = hit - global.origin
	else:
		var param = _axis_param(at, global.origin, axis)
		if param == null:
			_drag = {}
			return
		_drag.start = param
	_nav = Nav.GIZMO

func _update_drag(at: Vector2) -> void:
	if _drag.is_empty(): return
	var global: Transform3D = _drag.global
	var axis: Vector3 = _drag.dir
	var result := global
	if tool == Tool.MOVE:
		var param = _axis_param(at, global.origin, axis)
		if param == null: return
		var offset: float = param - _drag.start
		if snap: offset = snappedf(offset, 0.5)
		result.origin = global.origin + axis * offset
	elif tool == Tool.ROTATE:
		var hit = _plane_hit(at, global.origin, axis)
		if hit == null: return
		var v0: Vector3 = _drag.start
		var v1: Vector3 = hit - global.origin
		var angle := atan2(axis.dot(v0.cross(v1)), v0.dot(v1))
		if snap: angle = snappedf(angle, deg_to_rad(15.0))
		result.basis = Basis(axis, angle) * global.basis
	elif tool == Tool.SCALE:
		var param = _axis_param(at, global.origin, axis)
		if param == null or absf(_drag.start) < 0.0001: return
		var ratio: float = param / _drag.start
		if snap: ratio = snappedf(ratio, 0.1)
		var scale: Vector3 = _drag.scale
		var component: float = scale[_drag.axis] * ratio
		if absf(component) < 0.001: component = 0.001 * signf(component if component != 0 else 1.0)
		scale[_drag.axis] = component
		var local: Transform3D = _drag.local
		var rotation := local.basis.orthonormalized()
		transform_edited.emit(selected, Transform3D(rotation.scaled_local(scale), local.origin), false)
		return
	var local: Transform3D = _drag.parent.affine_inverse() * result
	transform_edited.emit(selected, local, false)

func _end_drag() -> void:
	_nav = Nav.NONE
	if _drag.is_empty(): return
	_drag = {}
	var node := Document.find(root, selected)
	if not node.is_empty(): transform_edited.emit(selected, Document.local_transform(node), true)
	overlay.queue_redraw()
