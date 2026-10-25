extends RefCounted

const Icons := preload("res://scripts/icons.gd")

const BG := Color("10141c")
const BASE := Color("191f2a") # cards (docks, panels)
const DARK := Color("141923") # lists and trees inside cards
const INPUT := Color("111722")
const LIGHT := Color("252e3b") # buttons, headers
const HOVER := Color("354254")
const PRESSED := Color("3c665f")
const BORDER := Color("2b3747")
const INPUT_BORDER := Color("303b4c")
const ACCENT := Color("78d5bc")
const ACCENT_DARK := Color("3f8c7a")
const ORANGE := Color("f0a45b")
const SELECT := Color("304d4b")
const TEXT := Color("dde5f0")
const LABEL := Color("b4bfce")
const MUTED := Color("8995a7")
const DISABLED := Color("566175")
const ERROR := Color("f39898")
const WARNING := Color("ffcf6a")
const SUCCESS := Color("8be0a8")
const AXIS := [Color("e8667a"), Color("8fd16a"), Color("6aa5f0")]
const VIEWPORT := Color("0b1018")
const RADIUS := 6

static var main_font: Font
static var bold_font: Font
static var code_font: Font

static func box(color: Color, radius: int = RADIUS, border: Color = Color.TRANSPARENT, margin: Vector4 = Vector4(10, 6, 10, 6)) -> StyleBoxFlat:
	var style := StyleBoxFlat.new()
	style.bg_color = color
	style.set_corner_radius_all(radius)
	if border.a > 0:
		style.border_color = border
		style.set_border_width_all(1)
	style.content_margin_left = margin.x
	style.content_margin_top = margin.y
	style.content_margin_right = margin.z
	style.content_margin_bottom = margin.w
	style.anti_aliasing = radius > 0
	style.corner_detail = 6
	return style

static func empty(margin: Vector4 = Vector4.ZERO) -> StyleBoxEmpty:
	var style := StyleBoxEmpty.new()
	style.content_margin_left = margin.x
	style.content_margin_top = margin.y
	style.content_margin_right = margin.z
	style.content_margin_bottom = margin.w
	return style

static func _font(path: String) -> Font:
	var file := FontFile.new()
	if file.load_dynamic_font(path) != OK: return null
	file.antialiasing = TextServer.FONT_ANTIALIASING_GRAY
	file.hinting = TextServer.HINTING_LIGHT
	file.subpixel_positioning = TextServer.SUBPIXEL_POSITIONING_AUTO
	return file

static func load_fonts() -> void:
	main_font = _font("res://fonts/Inter_Regular.woff2")
	bold_font = _font("res://fonts/Inter_Bold.woff2")
	code_font = _font("res://fonts/JetBrainsMono_Regular.woff2")
	if main_font == null: main_font = ThemeDB.fallback_font
	if bold_font == null: bold_font = main_font
	if code_font == null: code_font = main_font

## This UI-only build can't draw SVG, so tools/assets.py rasterizes Godot's theme icons into res://theme_icons.
static func _apply_theme_icons(t: Theme) -> void:
	var data = JSON.parse_string(FileAccess.get_file_as_string("res://theme_icons/theme_icons.json"))
	if not data is Dictionary: return
	var textures := {}
	for item in data.get("items", []):
		var icon_name: String = item[2]
		if not textures.has(icon_name):
			var info: Dictionary = data.icons.get(icon_name, {})
			textures[icon_name] = Icons.theme_icon(str(info.get("file", "")), info.get("size", [16, 16]))
		if textures[icon_name] != null: t.set_icon(item[0], item[1], textures[icon_name])

static func build() -> Theme:
	load_fonts()
	var t := Theme.new()
	t.default_font = main_font
	t.default_font_size = 14
	for type in ["Label", "Button", "MenuButton", "OptionButton", "CheckBox", "CheckButton", "LinkButton", "ColorPickerButton"]:
		t.set_color("font_color", type, TEXT)
		t.set_color("font_hover_color", type, Color.WHITE)
		t.set_color("font_pressed_color", type, ACCENT)
		t.set_color("font_hover_pressed_color", type, ACCENT)
		t.set_color("font_focus_color", type, TEXT)
		t.set_color("font_disabled_color", type, DISABLED)
		t.set_color("icon_normal_color", type, Color(1, 1, 1, 0.9))
		t.set_color("icon_hover_color", type, Color.WHITE)
		t.set_color("icon_pressed_color", type, ACCENT.lightened(0.3))
		t.set_color("icon_disabled_color", type, Color(1, 1, 1, 0.3))
		t.set_constant("icon_max_width", type, 16)
		t.set_constant("h_separation", type, 6)
	for type in ["Button", "OptionButton", "ColorPickerButton"]:
		t.set_stylebox("normal", type, box(LIGHT))
		t.set_stylebox("hover", type, box(HOVER))
		t.set_stylebox("pressed", type, box(PRESSED))
		t.set_stylebox("hover_pressed", type, box(PRESSED.lightened(0.08)))
		t.set_stylebox("disabled", type, box(Color("1c2330")))
		t.set_stylebox("focus", type, box(Color.TRANSPARENT, RADIUS, ACCENT_DARK))
	# Toolbar buttons: transparent until hovered.
	var flat := Vector4(7, 5, 7, 5)
	t.set_type_variation("FlatButton", "Button")
	t.set_stylebox("normal", "FlatButton", empty(flat))
	t.set_stylebox("hover", "FlatButton", box(HOVER, RADIUS, Color.TRANSPARENT, flat))
	t.set_stylebox("pressed", "FlatButton", box(SELECT, RADIUS, Color.TRANSPARENT, flat))
	t.set_stylebox("hover_pressed", "FlatButton", box(SELECT.lightened(0.08), RADIUS, Color.TRANSPARENT, flat))
	t.set_stylebox("disabled", "FlatButton", empty(flat))
	t.set_stylebox("focus", "FlatButton", empty())
	# Main screen selector: a segmented pill control.
	t.set_type_variation("MainScreenButton", "FlatButton")
	t.set_font("font", "MainScreenButton", bold_font)
	t.set_font_size("font_size", "MainScreenButton", 13)
	t.set_color("font_color", "MainScreenButton", MUTED)
	t.set_color("font_pressed_color", "MainScreenButton", ACCENT)
	var segment := Vector4(14, 5, 14, 5)
	t.set_stylebox("normal", "MainScreenButton", empty(segment))
	t.set_stylebox("hover", "MainScreenButton", box(LIGHT, RADIUS, Color.TRANSPARENT, segment))
	t.set_stylebox("pressed", "MainScreenButton", box(SELECT, RADIUS, Color.TRANSPARENT, segment))
	t.set_stylebox("hover_pressed", "MainScreenButton", box(SELECT, RADIUS, Color.TRANSPARENT, segment))
	# Primary action (Play).
	t.set_type_variation("AccentButton", "Button")
	t.set_color("font_color", "AccentButton", ACCENT)
	t.set_font("font", "AccentButton", bold_font)
	t.set_stylebox("normal", "AccentButton", box(Color("1f3a39"), RADIUS, Color("2e5753"), Vector4(12, 5, 12, 5)))
	t.set_stylebox("hover", "AccentButton", box(Color("27504c"), RADIUS, ACCENT_DARK, Vector4(12, 5, 12, 5)))
	t.set_stylebox("pressed", "AccentButton", box(PRESSED, RADIUS, ACCENT_DARK, Vector4(12, 5, 12, 5)))
	t.set_stylebox("disabled", "AccentButton", box(Color("1c2330"), RADIUS, Color.TRANSPARENT, Vector4(12, 5, 12, 5)))
	for type in ["MenuButton"]:
		t.set_stylebox("normal", type, empty(Vector4(9, 5, 9, 5)))
		t.set_stylebox("hover", type, box(LIGHT, RADIUS, Color.TRANSPARENT, Vector4(9, 5, 9, 5)))
		t.set_stylebox("pressed", type, box(LIGHT, RADIUS, Color.TRANSPARENT, Vector4(9, 5, 9, 5)))
		t.set_stylebox("disabled", type, empty(Vector4(9, 5, 9, 5)))
		t.set_stylebox("focus", type, empty())
		t.set_color("font_color", type, LABEL)
	for type in ["CheckBox", "CheckButton"]:
		for state in ["normal", "hover", "pressed", "hover_pressed", "disabled", "focus"]:
			t.set_stylebox(state, type, empty(Vector4(4, 2, 4, 2)))
	_apply_theme_icons(t)
	t.set_color("font_pressed_color", "CheckBox", TEXT)
	for type in ["LineEdit", "SpinBox"]:
		t.set_stylebox("normal", type, box(INPUT, RADIUS, INPUT_BORDER, Vector4(8, 4, 8, 4)))
		t.set_stylebox("focus", type, box(Color.TRANSPARENT, RADIUS, ACCENT_DARK, Vector4(8, 4, 8, 4)))
		t.set_stylebox("read_only", type, box(INPUT.lightened(0.02), RADIUS, Color.TRANSPARENT, Vector4(8, 4, 8, 4)))
		t.set_color("font_color", type, TEXT)
		t.set_color("font_uneditable_color", type, MUTED)
		t.set_color("font_placeholder_color", type, DISABLED)
		t.set_color("caret_color", type, ACCENT)
		t.set_color("selection_color", type, SELECT)
	t.set_stylebox("normal", "TextEdit", box(INPUT, RADIUS, INPUT_BORDER, Vector4(8, 4, 8, 4)))
	t.set_stylebox("focus", "TextEdit", box(Color.TRANSPARENT, RADIUS, ACCENT_DARK, Vector4(8, 4, 8, 4)))
	t.set_color("font_color", "TextEdit", TEXT)
	t.set_color("caret_color", "TextEdit", ACCENT)
	t.set_color("selection_color", "TextEdit", SELECT)
	for type in ["Tree", "ItemList"]:
		t.set_stylebox("panel", type, box(DARK, RADIUS, Color.TRANSPARENT, Vector4(4, 4, 4, 4)))
		t.set_stylebox("focus", type, empty())
		t.set_stylebox("selected", type, box(SELECT, 4))
		t.set_stylebox("selected_focus", type, box(SELECT, 4, ACCENT_DARK))
		t.set_stylebox("hovered", type, box(LIGHT, 4))
		t.set_stylebox("hovered_selected", type, box(SELECT.lightened(0.06), 4))
		t.set_stylebox("cursor", type, empty())
		t.set_stylebox("cursor_unfocused", type, empty())
		t.set_color("font_color", type, TEXT)
		t.set_color("font_selected_color", type, Color.WHITE)
		t.set_color("font_hovered_color", type, Color.WHITE)
		t.set_color("guide_color", type, Color(1, 1, 1, 0.04))
		t.set_constant("icon_max_width", type, 16)
		t.set_constant("v_separation", type, 3)
		t.set_constant("h_separation", type, 6)
	t.set_color("drop_position_color", "Tree", ACCENT)
	t.set_color("relationship_line_color", "Tree", Color(1, 1, 1, 0.10))
	t.set_constant("draw_relationship_lines", "Tree", 1)
	t.set_constant("item_margin", "Tree", 14)
	t.set_stylebox("title_button_normal", "Tree", box(LIGHT, 0))
	t.set_stylebox("custom_button", "Tree", empty(Vector4(2, 0, 2, 0)))
	t.set_stylebox("custom_button_hover", "Tree", box(HOVER, 4))
	t.set_stylebox("custom_button_pressed", "Tree", box(PRESSED, 4))
	t.set_stylebox("panel", "PanelContainer", empty())
	t.set_stylebox("panel", "Panel", box(BASE, 0))
	# Cards: every dock is a rounded panel on the dark background, with gaps.
	t.set_type_variation("Card", "PanelContainer")
	t.set_stylebox("panel", "Card", box(BASE, 8, Color.TRANSPARENT, Vector4(10, 9, 10, 10)))
	t.set_type_variation("ViewportFrame", "PanelContainer")
	t.set_stylebox("panel", "ViewportFrame", box(VIEWPORT, RADIUS, BORDER, Vector4(1, 1, 1, 1)))
	t.set_type_variation("SegmentPanel", "PanelContainer")
	t.set_stylebox("panel", "SegmentPanel", box(BASE, 8, Color.TRANSPARENT, Vector4(3, 3, 3, 3)))
	for type in ["PopupMenu"]:
		t.set_stylebox("panel", type, box(Color("1d2431"), 8, BORDER, Vector4(6, 6, 6, 6)))
		t.set_stylebox("hover", type, box(SELECT, 4))
		t.set_stylebox("separator", type, box(BORDER, 0, Color.TRANSPARENT, Vector4(0, 1, 0, 1)))
		t.set_color("font_color", type, TEXT)
		t.set_color("font_hover_color", type, Color.WHITE)
		t.set_color("font_disabled_color", type, DISABLED)
		t.set_color("font_separator_color", type, MUTED)
		t.set_color("font_accelerator_color", type, MUTED)
		t.set_constant("icon_max_width", type, 16)
		t.set_constant("v_separation", type, 8)
		t.set_constant("h_separation", type, 8)
	t.set_stylebox("panel", "PopupPanel", box(Color("1d2431"), 8, BORDER, Vector4(8, 8, 8, 8)))
	t.set_stylebox("panel", "TooltipPanel", box(Color("222a38"), RADIUS, BORDER, Vector4(10, 6, 10, 6)))
	t.set_color("font_color", "TooltipLabel", TEXT)
	# Embedded dialog frame: the title bar lives in the top expand margin (as in Godot).
	for item in [["embedded_border", Color("1d2431")], ["embedded_unfocused_border", BASE]]:
		var frame := box(item[1], 10, BORDER)
		frame.expand_margin_left = 10
		frame.expand_margin_top = 34
		frame.expand_margin_right = 10
		frame.expand_margin_bottom = 10
		frame.shadow_color = Color(0, 0, 0, 0.35)
		frame.shadow_size = 12
		t.set_stylebox(item[0], "Window", frame)
	t.set_constant("title_height", "Window", 34)
	t.set_font("title_font", "Window", bold_font)
	t.set_color("title_color", "Window", TEXT)
	t.set_stylebox("panel", "AcceptDialog", box(Color("1d2431"), 0, Color.TRANSPARENT, Vector4(12, 10, 12, 10)))
	for type in ["TabContainer", "TabBar"]:
		var tab := Vector4(14, 6, 14, 6)
		t.set_stylebox("tab_selected", type, box(SELECT, RADIUS, Color.TRANSPARENT, tab))
		t.set_stylebox("tab_unselected", type, empty(tab))
		t.set_stylebox("tab_hovered", type, box(LIGHT, RADIUS, Color.TRANSPARENT, tab))
		t.set_stylebox("tab_disabled", type, empty(tab))
		t.set_stylebox("tab_focus", type, empty())
		t.set_color("font_selected_color", type, ACCENT)
		t.set_color("font_unselected_color", type, MUTED)
		t.set_color("font_hovered_color", type, TEXT)
		t.set_constant("icon_max_width", type, 16)
		t.set_constant("h_separation", type, 6)
		t.set_font("font", type, bold_font)
		t.set_font_size("font_size", type, 13)
	t.set_stylebox("panel", "TabContainer", empty(Vector4(0, 6, 0, 0)))
	t.set_stylebox("tabbar_background", "TabContainer", empty())
	for type in ["HSplitContainer", "VSplitContainer", "SplitContainer"]:
		t.set_constant("separation", type, 8)
		t.set_constant("minimum_grab_thickness", type, 8)
		t.set_constant("autohide", type, 1)
	for type in ["VScrollBar", "HScrollBar"]:
		t.set_stylebox("scroll", type, empty(Vector4(2, 2, 2, 2)))
		t.set_stylebox("scroll_focus", type, empty(Vector4(2, 2, 2, 2)))
		t.set_stylebox("grabber", type, box(Color(1, 1, 1, 0.14), 4))
		t.set_stylebox("grabber_highlight", type, box(Color(1, 1, 1, 0.24), 4))
		t.set_stylebox("grabber_pressed", type, box(Color(1, 1, 1, 0.32), 4))
	t.set_constant("separation", "HBoxContainer", 6)
	t.set_constant("separation", "VBoxContainer", 6)
	t.set_constant("separation", "HSeparator", 10)
	t.set_constant("separation", "VSeparator", 10)
	t.set_stylebox("separator", "HSeparator", box(BORDER, 0, Color.TRANSPARENT, Vector4.ZERO))
	t.set_stylebox("separator", "VSeparator", box(BORDER, 0, Color.TRANSPARENT, Vector4.ZERO))
	t.set_color("default_color", "RichTextLabel", TEXT)
	t.set_font("normal_font", "RichTextLabel", code_font)
	t.set_font("mono_font", "RichTextLabel", code_font)
	t.set_font_size("normal_font_size", "RichTextLabel", 12)
	t.set_font_size("mono_font_size", "RichTextLabel", 12)
	t.set_stylebox("normal", "RichTextLabel", empty(Vector4(6, 4, 6, 4)))
	t.set_stylebox("focus", "RichTextLabel", empty(Vector4(6, 4, 6, 4)))
	t.set_type_variation("PropertyLabel", "Label")
	t.set_color("font_color", "PropertyLabel", LABEL)
	t.set_font_size("font_size", "PropertyLabel", 13)
	t.set_type_variation("HeaderLabel", "Label")
	t.set_font("font", "HeaderLabel", bold_font)
	t.set_font_size("font_size", "HeaderLabel", 13)
	# v1-style section titles: small, uppercase, muted.
	t.set_type_variation("DockTitle", "Label")
	t.set_font("font", "DockTitle", bold_font)
	t.set_font_size("font_size", "DockTitle", 11)
	t.set_color("font_color", "DockTitle", MUTED)
	t.set_type_variation("BrandLabel", "Label")
	t.set_font("font", "BrandLabel", bold_font)
	t.set_font_size("font_size", "BrandLabel", 16)
	t.set_color("font_color", "BrandLabel", TEXT)
	return t
