extends Control
## Like Godot's EditorSpinSlider: drag to change (Shift = precise, Ctrl = snap) or click to type a value.

signal value_changed(value: float)
signal committed(value: float)

const Style := preload("res://scripts/theme.gd")

var value := 0.0
var min_value := -INF
var max_value := INF
var step := 0.01
var integer := false
var prefix := ""
var prefix_color := Style.MUTED
var suffix := ""
var read_only := false
var show_slider := false

var _press_position := Vector2.ZERO
var _pressed := false
var _dragging := false
var _drag_start_value := 0.0
var _drag_accum := 0.0
var _hover := false
var _editor: LineEdit

func _init() -> void:
	custom_minimum_size = Vector2(60, 26)
	focus_mode = Control.FOCUS_ALL
	mouse_default_cursor_shape = Control.CURSOR_HSIZE
	size_flags_horizontal = Control.SIZE_EXPAND_FILL
	mouse_entered.connect(func(): _hover = true; queue_redraw())
	mouse_exited.connect(func(): _hover = false; queue_redraw())

func setup(initial: float, low: float = -INF, high: float = INF, increment: float = 0.01, is_int: bool = false) -> void:
	min_value = low
	max_value = high
	step = maxf(increment, 0.0001)
	integer = is_int
	if is_int: step = maxf(1.0, round(step))
	show_slider = is_finite(low) and is_finite(high)
	value = _clamp(initial)
	tooltip_text = ""
	queue_redraw()

func set_value_no_signal(new_value: float) -> void:
	value = _clamp(new_value)
	queue_redraw()

func _clamp(v: float) -> float:
	if integer: v = round(v)
	return clampf(v, min_value, max_value)

func _text() -> String:
	if integer: return str(int(value))
	var decimals := clampi(int(ceil(-log(step) / log(10.0))), 0, 6)
	var text := String.num(value, decimals)
	if "." in text: text = text.rstrip("0").trim_suffix(".")
	if text == "-0": text = "0"
	return text

func _set(new_value: float, final: bool) -> void:
	var clamped := _clamp(new_value)
	if not is_equal_approx(clamped, value):
		value = clamped
		queue_redraw()
		value_changed.emit(value)
	if final: committed.emit(value)

func _draw() -> void:
	var rect := Rect2(Vector2.ZERO, size)
	var style := get_theme_stylebox("read_only" if read_only else ("focus" if has_focus() and not _dragging else "normal"), "LineEdit")
	draw_style_box(get_theme_stylebox("normal", "LineEdit"), rect)
	if has_focus() and not read_only: draw_style_box(style, rect)
	var font := get_theme_font("font", "LineEdit")
	var font_size := get_theme_font_size("font_size", "LineEdit") - 1
	var baseline := (size.y + font.get_ascent(font_size) - font.get_descent(font_size)) * 0.5
	var x := 6.0
	if not prefix.is_empty():
		draw_string(font, Vector2(x, baseline), prefix, HORIZONTAL_ALIGNMENT_LEFT, -1, font_size, prefix_color)
		x += font.get_string_size(prefix, HORIZONTAL_ALIGNMENT_LEFT, -1, font_size).x + 6
	var color := Style.MUTED if read_only else Style.TEXT
	var text := _text() + (" " + suffix if not suffix.is_empty() else "")
	draw_string(font, Vector2(x, baseline), text, HORIZONTAL_ALIGNMENT_LEFT, size.x - x - 16, font_size, color)
	if show_slider and max_value > min_value:
		var ratio := clampf((value - min_value) / (max_value - min_value), 0, 1)
		draw_rect(Rect2(3, size.y - 4, size.x - 6, 2), Color(1, 1, 1, 0.08))
		draw_rect(Rect2(3, size.y - 4, (size.x - 6) * ratio, 2), Style.ACCENT_DARK.lightened(0.2))
	if _hover and not read_only and not _dragging:
		var cx := size.x - 9
		var cy := size.y * 0.5
		draw_colored_polygon(PackedVector2Array([Vector2(cx - 4, cy - 2), Vector2(cx + 4, cy - 2), Vector2(cx, cy - 7)]), Style.MUTED)
		draw_colored_polygon(PackedVector2Array([Vector2(cx - 4, cy + 2), Vector2(cx + 4, cy + 2), Vector2(cx, cy + 7)]), Style.MUTED)

func _gui_input(event: InputEvent) -> void:
	if read_only: return
	if event is InputEventMouseButton and event.button_index == MOUSE_BUTTON_LEFT:
		if event.pressed:
			if event.position.x > size.x - 16:
				_set(value + (step if event.position.y < size.y * 0.5 else -step), true)
				accept_event()
				return
			_pressed = true
			_dragging = false
			_press_position = event.position
			_drag_start_value = value
			_drag_accum = 0.0
		elif _pressed:
			_pressed = false
			if _dragging:
				_dragging = false
				Input.mouse_mode = Input.MOUSE_MODE_VISIBLE
				if DisplayServer.get_name() != "Wayland": warp_mouse(_press_position)
				committed.emit(value)
			else:
				_begin_text_edit()
			queue_redraw()
		accept_event()
	elif event is InputEventMouseMotion and _pressed:
		if not _dragging and absf(event.position.x - _press_position.x) > 3:
			_dragging = true
			Input.mouse_mode = Input.MOUSE_MODE_CAPTURED
		if _dragging:
			var speed := 0.1 if event.shift_pressed else 1.0
			_drag_accum += event.relative.x * speed
			var target := _drag_start_value + _drag_accum * step
			if event.ctrl_pressed: target = snappedf(target, step * 10)
			_set(target, false)
		accept_event()
	elif event is InputEventKey and event.pressed and has_focus():
		if event.keycode in [KEY_ENTER, KEY_KP_ENTER, KEY_F2]:
			_begin_text_edit()
			accept_event()
		elif event.keycode == KEY_UP:
			_set(value + step, true)
			accept_event()
		elif event.keycode == KEY_DOWN:
			_set(value - step, true)
			accept_event()

func _begin_text_edit() -> void:
	if _editor != null: return
	_editor = LineEdit.new()
	_editor.text = _text()
	_editor.set_anchors_and_offsets_preset(Control.PRESET_FULL_RECT)
	_editor.select_all_on_focus = true
	add_child(_editor)
	_editor.grab_focus()
	_editor.select_all()
	_editor.text_submitted.connect(func(_t): _finish_text_edit(true))
	_editor.focus_exited.connect(func(): _finish_text_edit(true))
	_editor.gui_input.connect(func(e):
		if e is InputEventKey and e.pressed and e.keycode == KEY_ESCAPE:
			_finish_text_edit(false)
			accept_event())

func _finish_text_edit(apply: bool) -> void:
	if _editor == null: return
	var text := _editor.text.strip_edges()
	var editor := _editor
	_editor = null
	editor.queue_free()
	if apply and not text.is_empty():
		# Like Godot, accept expressions such as "2*3" or "1/3".
		var expression := Expression.new()
		if expression.parse(text.replace(",", ".")) == OK:
			var result = expression.execute()
			if not expression.has_execute_failed() and (result is float or result is int) and is_finite(float(result)):
				_set(float(result), true)
	grab_focus()
	queue_redraw()
