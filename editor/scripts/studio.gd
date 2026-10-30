extends Control

const Document := preload("res://scripts/scene_document.gd")
const Project := preload("res://scripts/project.gd")
const Style := preload("res://scripts/theme.gd")
const Icons := preload("res://scripts/icons.gd")
const SceneDock := preload("res://scripts/scene_dock.gd")
const FileSystemDock := preload("res://scripts/filesystem_dock.gd")
const Inspector := preload("res://scripts/inspector.gd")
const Viewport3D := preload("res://scripts/viewport_3d.gd")
const ScriptEditor := preload("res://scripts/script_editor.gd")
const CreateNodeDialog := preload("res://scripts/create_node_dialog.gd")
const AttachScriptDialog := preload("res://scripts/attach_script_dialog.gd")
const SignalsDock := preload("res://scripts/signals_dock.gd")
const ConnectDialog := preload("res://scripts/connect_dialog.gd")
const NewSceneDialog := preload("res://scripts/new_scene_dialog.gd")
const AnimationPanel := preload("res://scripts/animation_panel.gd")
const ProjectManager := preload("res://scripts/project_manager.gd")
const SerialMonitor := preload("res://scripts/serial_monitor.gd")
const ProjectSettingsDialog := preload("res://scripts/project_settings_dialog.gd")
const Hardware := preload("res://scripts/hardware.gd")
const Actions := preload("res://scripts/input_map.gd")
const VERSION := "0.6.0"
const SETTINGS := "user://editor.cfg"

enum Screen { VIEW_3D, SCRIPT, GAME, VIEW_2D }

var bridge: Object
var project = Project.new()
var settings := ConfigFile.new()
var tabs := [] # {path, doc, saved, undo, redo, selected, view}
var current := -1
var screen := Screen.VIEW_3D
var playing := false
var paused := false
var play_title := ""
var pause_on_error := true
var needs_render := true
var _merge_key := ""
var _switching := false
var _script_cache := {} # path -> {mtime, info}
var _action_keys := [] # Godot keycodes per Input Map action, in bit order
var _live_timer := 0.0
var _job_id := -1
var _job_prefix := ""
var _job_offset := 0
var _job_poll := 0.0
var _job_action := ""
var _serial_was_open := false
var _smoke := false
var _export_path := "" # --export=<file>: export the main scene and quit without saving the editor settings
var _screenshot := ""
var _pending_quit := false
var _pending_close := -1
var _pending_delete := 0
var _quick_action := Callable()

# UI
var scene_dock: SceneDock
var filesystem: FileSystemDock
var inspector: Inspector
var signals_dock: SignalsDock
var animation_panel: AnimationPanel
var _repose := false # the next upload shows the animation pose being edited again
var right_tabs: TabBar # Inspector | Node, as in Godot
var viewport: Viewport3D
var script_editor: ScriptEditor
var game_view: TextureRect
var game_label: Label
var scene_tabs: TabBar
var screens: Control
var screen_buttons := {} # Screen -> its button
var bottom_tabs: TabBar
var bottom_card: PanelContainer
var output: RichTextLabel
var errors_list: ItemList
var status_label: Label
var project_label: Label
var mode_label: Label
var stats_label: Label
var play_button: Button
var play_scene_button: Button
var pause_button: Button
var stop_button: Button
var step_button: Button
var target_select: OptionButton # PC, <board> · Debug, <board> · Release
var serial_monitor: SerialMonitor
var build_button: Button
var flash_button: Button
var main_split: HSplitContainer
var center_split: VSplitContainer
# Dialogs
var create_dialog: CreateNodeDialog
var attach_dialog: AttachScriptDialog
var connect_dialog: ConnectDialog
var new_scene_dialog: NewSceneDialog
var project_manager: ProjectManager
var save_dialog: FileDialog
var export_dialog: FileDialog
var error_dialog: AcceptDialog
var confirm_dialog: ConfirmationDialog
var compress_dialog: ConfirmationDialog
var quit_dialog: ConfirmationDialog
var close_dialog: ConfirmationDialog
var delete_dialog: ConfirmationDialog
var quick_dialog: ConfirmationDialog
var quick_filter: LineEdit
var quick_list: ItemList
var project_settings: ProjectSettingsDialog
var help_dialog: AcceptDialog

func _ready() -> void:
	get_tree().auto_accept_quit = false
	settings.load(SETTINGS)
	_apply_scale(float(settings.get_value("editor", "scale", 0.0)))
	theme = Style.build()
	get_window().min_size = Vector2i(1100, 700)
	_build_ui()
	_build_dialogs()
	for arg in OS.get_cmdline_user_args():
		if arg == "--smoke-test": _smoke = true
		elif arg.begins_with("--screenshot="): _screenshot = arg.trim_prefix("--screenshot=")
		elif arg.begins_with("--export="): _export_path = arg.trim_prefix("--export=")
	if not ClassDB.class_exists("JetBridge"):
		var extension = load("res://native/jet_bridge.gdextension")
		if extension == null or not ClassDB.class_exists("JetBridge"):
			_fail("C++ bridge missing. Run ./jet-studio build before starting the editor.")
			set_process(false)
			return
	bridge = ClassDB.instantiate("JetBridge")
	viewport.bridge = bridge
	Hardware.load_catalog(_root_dir())
	if ClassDB.class_exists("JetSerial"):
		serial_monitor.serial = ClassDB.instantiate("JetSerial")
		serial_monitor.set_preferences(str(settings.get_value("serial", "port", "")), int(settings.get_value("serial", "baud", 115200)))
	script_editor.bridge = bridge
	script_editor.class_db = bridge.class_db()
	signals_dock.class_db = script_editor.class_db
	connect_dialog.class_db = script_editor.class_db
	_log("Jet Studio %s  ·  Godot UI + C++ runtime + Jet renderer  ·  JetScript" % VERSION, Style.ACCENT)
	_log("3D view: middle button orbits · Shift+middle pans · right + WASD/QE flies · wheel zooms · F focuses · Alt+left = middle")
	if _smoke:
		# Watchdog, so a script error in the test can't leave the process running.
		get_tree().create_timer(90.0).timeout.connect(func():
			printerr("SMOKE FAIL: timeout")
			get_tree().quit(2))
		_run_smoke.call_deferred()
		return
	var folder := ""
	for arg in OS.get_cmdline_user_args():
		if arg.begins_with("--project="): folder = arg.trim_prefix("--project=")
	if folder.is_empty(): folder = str(settings.get_value("projects", "last", ""))
	if folder.is_empty() or not Project.is_project(folder): folder = _example_folder()
	_open_project(folder)
	if not _export_path.is_empty():
		# ./jet-studio export: write the main scene of --project to a .jetpack and quit.
		get_tree().quit(0 if _export_to(_export_path) else 1)

func _apply_scale(value: float) -> void:
	var scale := value
	if scale <= 0.0:
		scale = DisplayServer.screen_get_scale()
		var dpi := DisplayServer.screen_get_dpi()
		if scale <= 1.0 and dpi >= 192: scale = 2.0
		elif scale <= 1.0 and dpi >= 144: scale = 1.5
	get_window().content_scale_factor = clampf(scale, 0.75, 3.0)

## The folder with tools/, firmware/ and examples/: the repo, or a release whose bin/ holds the executable with the editor inside.
func _root_dir() -> String:
	var editor := ProjectSettings.globalize_path("res://")
	if editor.is_absolute_path(): return editor.trim_suffix("/").get_base_dir()
	return OS.get_executable_path().get_base_dir().get_base_dir()

func _example_folder() -> String:
	return _root_dir().path_join("examples/first_world")

# ------------------------------------------------------------------ UI

func _flat(icon: String, tooltip: String, action: Callable, toggle: bool = false) -> Button:
	var button := Button.new()
	button.theme_type_variation = "FlatButton"
	button.icon = Icons.get_icon(icon)
	button.tooltip_text = tooltip
	button.toggle_mode = toggle
	button.focus_mode = Control.FOCUS_NONE
	if toggle: button.toggled.connect(func(_on): action.call())
	else: button.pressed.connect(action)
	return button

func _dock(title: String, content: Control, extra: Control = null) -> PanelContainer:
	var panel := PanelContainer.new()
	panel.theme_type_variation = "Card"
	var box := VBoxContainer.new()
	box.add_theme_constant_override("separation", 8)
	panel.add_child(box)
	var head := HBoxContainer.new()
	box.add_child(head)
	var label := Label.new()
	label.text = title.to_upper()
	label.theme_type_variation = "DockTitle"
	label.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	head.add_child(label)
	if extra: head.add_child(extra)
	content.size_flags_vertical = Control.SIZE_EXPAND_FILL
	box.add_child(content)
	return panel

func _build_ui() -> void:
	var background := ColorRect.new()
	background.color = Style.BG
	background.set_anchors_and_offsets_preset(Control.PRESET_FULL_RECT)
	add_child(background)
	var margin := MarginContainer.new()
	margin.set_anchors_and_offsets_preset(Control.PRESET_FULL_RECT)
	for side in ["left", "right", "top", "bottom"]: margin.add_theme_constant_override("margin_" + side, 10)
	add_child(margin)
	var layout := VBoxContainer.new()
	layout.add_theme_constant_override("separation", 8)
	margin.add_child(layout)
	# Header: brand + menus | main screens | play controls | target
	var bar := HBoxContainer.new()
	bar.add_theme_constant_override("separation", 6)
	layout.add_child(bar)
	var logo := TextureRect.new()
	logo.texture = load_image_texture("res://assets/emblem.png")
	logo.custom_minimum_size = Vector2(26, 26)
	logo.expand_mode = TextureRect.EXPAND_IGNORE_SIZE
	logo.stretch_mode = TextureRect.STRETCH_KEEP_ASPECT_CENTERED
	logo.tooltip_text = "Jet Studio " + VERSION
	bar.add_child(logo)
	var brand := Label.new()
	brand.text = "JET STUDIO"
	brand.theme_type_variation = "BrandLabel"
	bar.add_child(brand)
	project_label = Label.new()
	project_label.add_theme_color_override("font_color", Style.MUTED)
	project_label.add_theme_font_size_override("font_size", 13)
	# Clip long project names, otherwise they widen the window.
	project_label.clip_text = true
	project_label.text_overrun_behavior = TextServer.OVERRUN_TRIM_ELLIPSIS
	project_label.custom_minimum_size.x = 60
	project_label.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	project_label.size_flags_stretch_ratio = 0.6
	bar.add_child(project_label)
	var gap := Control.new()
	gap.custom_minimum_size.x = 10
	bar.add_child(gap)
	_menu(bar, "Scene", [
		["New Scene  (Ctrl+N)", "New", _new_scene],
		["Open Scene...  (Ctrl+O)", "Load", func(): _quick_open("scene")],
		[],
		["Save Scene  (Ctrl+S)", "Save", func(): _save_scene()],
		["Save Scene As...  (Ctrl+Shift+S)", "Save", func(): _save_scene_as()],
		["Save All  (Ctrl+Shift+Alt+S)", "Save", _save_all],
		[],
		["Close Scene  (Ctrl+Shift+W)", "Close", func(): _close_tab(current)],
		[],
		["Undo  (Ctrl+Z)", "", _undo],
		["Redo  (Ctrl+Shift+Z)", "", _redo],
		[],
		["Quit  (Ctrl+Q)", "", _request_quit],
	])
	_menu(bar, "Project", [
		["Project Settings...", "Tools", _show_project_settings],
		[],
		["Export .jetpack...", "Save", func(): export_dialog.popup_centered_ratio(0.6)],
		["Open Project Folder", "Folder", func(): OS.shell_open(project.root)],
		["Reload Project", "Reload", func(): _open_project(project.root)],
		[],
		["Project Manager...", "Filesystem", _show_project_manager],
	])
	_menu(bar, "Debug", [
		["Run External Player (SDL)", "PlayCustom", func(): _external_player(false)],
		["Run Device Emulator (firmware rendering)", "PlayCustom", func(): _external_player(true)],
		["Pause on Script Error", "", func(): pause_on_error = not pause_on_error; _log("Pause on error: " + ("on" if pause_on_error else "off"))],
	])
	_menu(bar, "Editor", [
		["Interface Scale: Auto", "", func(): _set_scale(0.0)],
		["Interface Scale: 100%", "", func(): _set_scale(1.0)],
		["Interface Scale: 125%", "", func(): _set_scale(1.25)],
		["Interface Scale: 150%", "", func(): _set_scale(1.5)],
		["Interface Scale: 200%", "", func(): _set_scale(2.0)],
		[],
		["Show / Hide Bottom Panel", "", func(): bottom_card.visible = not bottom_card.visible],
	])
	_menu(bar, "Help", [
		["JetScript Reference", "Help", func(): OS.shell_open(_root_dir().path_join("docs/jetscript.md"))],
		["3D View Controls and Shortcuts", "Keyboard", func(): help_dialog.popup_centered(Vector2i(640, 520))],
		[],
		["About Jet Studio", "", func(): _alert("Jet Studio %s\nGodot-style editor for 3D games on microcontrollers.\nUI: Godot (MIT) · Renderer: Jet (MIT) · Icons: Godot editor (MIT)\nEditor, runtime and JetScript: Jet Studio (MIT)." % VERSION)],
	])
	var left_spacer := Control.new()
	left_spacer.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	bar.add_child(left_spacer)
	var segments := PanelContainer.new()
	segments.theme_type_variation = "SegmentPanel"
	bar.add_child(segments)
	var segment_row := HBoxContainer.new()
	segment_row.add_theme_constant_override("separation", 2)
	segments.add_child(segment_row)
	var group := ButtonGroup.new()
	for item in [["2D", "Node2D", Screen.VIEW_2D, "2D View (Ctrl+F1)"], ["3D", "Node3D", Screen.VIEW_3D, "3D View (Ctrl+F2)"],
			["Script", "Script", Screen.SCRIPT, "Script Editor (Ctrl+F3)"], ["Game", "Game", Screen.GAME, "Running Game (Ctrl+F4)"]]:
		var button := Button.new()
		button.theme_type_variation = "MainScreenButton"
		button.text = item[0]
		button.icon = Icons.get_icon(item[1])
		button.toggle_mode = true
		button.button_group = group
		button.tooltip_text = item[3]
		button.focus_mode = Control.FOCUS_NONE
		var id: int = item[2]
		button.pressed.connect(func(): _set_screen(id))
		segment_row.add_child(button)
		screen_buttons[id] = button
	var right_spacer := Control.new()
	right_spacer.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	bar.add_child(right_spacer)
	play_button = Button.new()
	play_button.theme_type_variation = "AccentButton"
	play_button.text = "Play"
	play_button.icon = Icons.get_icon("Play")
	play_button.tooltip_text = "Run the project's main scene (F5)"
	play_button.focus_mode = Control.FOCUS_NONE
	play_button.pressed.connect(func(): _play(false))
	bar.add_child(play_button)
	pause_button = _flat("Pause", "Pause / Resume (F7)", _toggle_pause)
	bar.add_child(pause_button)
	step_button = _flat("NextFrame", "Next Frame (while paused)", _step)
	bar.add_child(step_button)
	stop_button = _flat("Stop", "Stop the running game (F8)", _stop)
	bar.add_child(stop_button)
	play_scene_button = _flat("PlayScene", "Run the current scene (F6)", func(): _play(true))
	bar.add_child(play_scene_button)
	bar.add_child(VSeparator.new())
	target_select = OptionButton.new()
	target_select.add_icon_item(Icons.get_icon("Game"), "PC")
	# _apply_hardware() puts the project's board name in these texts and in the tooltip.
	target_select.add_icon_item(Icons.get_icon("Tools"), "Board · Debug")
	target_select.add_icon_item(Icons.get_icon("Tools"), "Board · Release")
	target_select.focus_mode = Control.FOCUS_NONE
	target_select.item_selected.connect(func(i):
		project.config.target = "device" if i > 0 else "desktop"
		if i > 0: project.config.profile = "release" if i == 2 else "debug"
		project.save()
		_update_controls())
	bar.add_child(target_select)
	build_button = Button.new()
	build_button.text = "Build"
	build_button.icon = Icons.get_icon("Tools")
	build_button.tooltip_text = "Build the PC player or the firmware of the main scene"
	build_button.focus_mode = Control.FOCUS_NONE
	build_button.pressed.connect(func(): _start_task("build"))
	bar.add_child(build_button)
	flash_button = Button.new()
	flash_button.text = "Flash"
	flash_button.icon = Icons.get_icon("ArrowDown")
	flash_button.tooltip_text = "Build and upload the firmware to the board"
	flash_button.focus_mode = Control.FOCUS_NONE
	flash_button.pressed.connect(func(): _start_task("flash"))
	bar.add_child(flash_button)
	# Docks and main screen
	main_split = HSplitContainer.new()
	main_split.size_flags_vertical = Control.SIZE_EXPAND_FILL
	main_split.split_offset = 270
	layout.add_child(main_split)
	var left := VSplitContainer.new()
	left.custom_minimum_size.x = 230
	left.split_offset = 330
	main_split.add_child(left)
	scene_dock = SceneDock.new()
	left.add_child(_dock("Scene", scene_dock))
	filesystem = FileSystemDock.new()
	filesystem.project = project
	left.add_child(_dock("FileSystem", filesystem))
	var right_split := HSplitContainer.new()
	right_split.split_offset = -330
	main_split.add_child(right_split)
	center_split = VSplitContainer.new()
	center_split.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	center_split.split_offset = -150
	right_split.add_child(center_split)
	var center_card := PanelContainer.new()
	center_card.theme_type_variation = "Card"
	center_card.size_flags_vertical = Control.SIZE_EXPAND_FILL
	center_split.add_child(center_card)
	var center := VBoxContainer.new()
	center.add_theme_constant_override("separation", 6)
	center_card.add_child(center)
	var tab_row := HBoxContainer.new()
	center.add_child(tab_row)
	scene_tabs = TabBar.new()
	scene_tabs.tab_close_display_policy = TabBar.CLOSE_BUTTON_SHOW_ALWAYS
	scene_tabs.drag_to_rearrange_enabled = false
	scene_tabs.focus_mode = Control.FOCUS_NONE
	scene_tabs.clip_tabs = true
	scene_tabs.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	scene_tabs.tab_changed.connect(func(i): if not _switching: _switch_tab(i))
	scene_tabs.tab_close_pressed.connect(_close_tab)
	tab_row.add_child(scene_tabs)
	tab_row.add_child(_flat("Add", "New Scene (Ctrl+N)", func(): _new_scene()))
	mode_label = Label.new()
	mode_label.add_theme_font_override("font", Style.bold_font)
	mode_label.add_theme_font_size_override("font_size", 11)
	tab_row.add_child(mode_label)
	screens = PanelContainer.new()
	screens.size_flags_vertical = Control.SIZE_EXPAND_FILL
	center.add_child(screens)
	viewport = Viewport3D.new()
	screens.add_child(viewport)
	script_editor = ScriptEditor.new()
	script_editor.project = project
	script_editor.docs = _root_dir().path_join("docs")
	screens.add_child(script_editor)
	var game := VBoxContainer.new()
	screens.add_child(game)
	game_label = Label.new()
	game_label.add_theme_color_override("font_color", Style.MUTED)
	game_label.add_theme_font_size_override("font_size", 12)
	game_label.text = "Press Play (F5) to run the game. Keyboard: arrows/WASD, Space/Enter, Esc."
	game.add_child(game_label)
	var game_area := PanelContainer.new()
	game_area.theme_type_variation = "ViewportFrame"
	game_area.size_flags_vertical = Control.SIZE_EXPAND_FILL
	game.add_child(game_area)
	game_view = TextureRect.new()
	game_view.expand_mode = TextureRect.EXPAND_IGNORE_SIZE
	game_view.stretch_mode = TextureRect.STRETCH_KEEP_ASPECT_CENTERED
	game_view.texture_filter = CanvasItem.TEXTURE_FILTER_NEAREST
	game_view.focus_mode = Control.FOCUS_CLICK
	game_area.add_child(game_view)
	bottom_card = PanelContainer.new()
	bottom_card.theme_type_variation = "Card"
	bottom_card.custom_minimum_size.y = 120
	center_split.add_child(bottom_card)
	var bottom := VBoxContainer.new()
	bottom_card.add_child(bottom)
	var bottom_bar := HBoxContainer.new()
	bottom.add_child(bottom_bar)
	bottom_tabs = TabBar.new()
	bottom_tabs.focus_mode = Control.FOCUS_NONE
	bottom_tabs.add_tab("Output")
	bottom_tabs.add_tab("Debugger")
	bottom_tabs.add_tab("Serial")
	bottom_tabs.add_tab("Animation")
	bottom_tabs.clip_tabs = false # show every tab, the stats take the rest of the bar
	bottom_bar.add_child(bottom_tabs)
	# Board stats (Debug firmware only), visible from every tab.
	var stats_gap := Control.new()
	stats_gap.custom_minimum_size.x = 12
	bottom_bar.add_child(stats_gap)
	var device_stats := Label.new()
	device_stats.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	device_stats.clip_text = true
	device_stats.text_overrun_behavior = TextServer.OVERRUN_TRIM_ELLIPSIS
	device_stats.custom_minimum_size.x = 40
	device_stats.mouse_filter = Control.MOUSE_FILTER_PASS
	device_stats.add_theme_font_override("font", Style.code_font)
	device_stats.add_theme_font_size_override("font_size", 12)
	device_stats.add_theme_color_override("font_color", Style.ACCENT)
	device_stats.tooltip_text = "From the Debug firmware on the board, every 60 frames: frames per second, frame and script time,\ntriangles drawn, free memory (internal and PSRAM)"
	bottom_bar.add_child(device_stats)
	var bottom_stack := PanelContainer.new()
	bottom_stack.size_flags_vertical = Control.SIZE_EXPAND_FILL
	bottom.add_child(bottom_stack)
	var output_box := VBoxContainer.new()
	bottom_stack.add_child(output_box)
	output = RichTextLabel.new()
	output.scroll_following = true
	output.selection_enabled = true
	output.context_menu_enabled = true
	output.size_flags_vertical = Control.SIZE_EXPAND_FILL
	output.meta_clicked.connect(_on_meta)
	output_box.add_child(output)
	var debugger := VBoxContainer.new()
	debugger.visible = false
	bottom_stack.add_child(debugger)
	serial_monitor = SerialMonitor.new()
	serial_monitor.visible = false
	serial_monitor.stats_changed.connect(func(text): device_stats.text = text)
	bottom_stack.add_child(serial_monitor)
	animation_panel = AnimationPanel.new()
	animation_panel.visible = false
	bottom_stack.add_child(animation_panel)
	bottom_tabs.tab_changed.connect(func(i):
		output_box.visible = i == 0
		debugger.visible = i == 1
		serial_monitor.visible = i == 2
		animation_panel.visible = i == 3
		if i == 3 and bottom_card.size.y < 300: center_split.split_offset -= int(300 - bottom_card.size.y)
		_refresh_inspector() # key icons only show with the Animation tab
		if i == 2 and not serial_monitor.is_connected_to_board(): serial_monitor.refresh_ports())
	var debug_title := Label.new()
	debug_title.text = "Script compile and runtime errors (double-click: go to line)"
	debug_title.add_theme_color_override("font_color", Style.MUTED)
	debug_title.add_theme_font_size_override("font_size", 12)
	debugger.add_child(debug_title)
	errors_list = ItemList.new()
	errors_list.size_flags_vertical = Control.SIZE_EXPAND_FILL
	errors_list.item_activated.connect(func(i):
		var meta: Dictionary = errors_list.get_item_metadata(i)
		if not str(meta.get("path", "")).is_empty(): _open_script_at(meta.path, int(meta.line)))
	debugger.add_child(errors_list)
	var clear_button := _flat("Clear", "Clear Panel", func():
		if bottom_tabs.current_tab == 2:
			serial_monitor.clear()
			return
		output.clear()
		errors_list.clear()
		_update_debugger_tab())
	bottom_bar.add_child(clear_button)
	inspector = Inspector.new()
	inspector.custom_minimum_size.x = 300
	signals_dock = SignalsDock.new()
	signals_dock.visible = false
	signals_dock.method_exists = _method_exists
	var right_card := PanelContainer.new()
	right_card.theme_type_variation = "Card"
	right_split.add_child(right_card)
	var right_box := VBoxContainer.new()
	right_box.add_theme_constant_override("separation", 8)
	right_card.add_child(right_box)
	right_tabs = TabBar.new()
	right_tabs.focus_mode = Control.FOCUS_NONE
	right_tabs.add_tab("Inspector")
	right_tabs.add_tab("Signals")
	right_tabs.set_tab_tooltip(1, "The selected node's signals and their connections")
	right_tabs.tab_changed.connect(func(i):
		inspector.visible = i == 0
		signals_dock.visible = i == 1
		_refresh_inspector())
	right_box.add_child(right_tabs)
	for dock in [inspector, signals_dock]:
		dock.size_flags_vertical = Control.SIZE_EXPAND_FILL
		right_box.add_child(dock)
	var footer := HBoxContainer.new()
	footer.add_theme_constant_override("separation", 14)
	layout.add_child(footer)
	status_label = Label.new()
	status_label.add_theme_color_override("font_color", Style.MUTED)
	status_label.add_theme_font_size_override("font_size", 12)
	status_label.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	status_label.clip_text = true
	footer.add_child(status_label)
	stats_label = Label.new()
	stats_label.add_theme_color_override("font_color", Style.MUTED)
	stats_label.add_theme_font_size_override("font_size", 12)
	footer.add_child(stats_label)
	var version := Label.new()
	version.text = "PROTOTYPE " + VERSION
	version.add_theme_color_override("font_color", Style.ACCENT)
	version.add_theme_font_override("font", Style.bold_font)
	version.add_theme_font_size_override("font_size", 11)
	footer.add_child(version)
	# Signals
	scene_dock.node_selected.connect(_select)
	scene_dock.add_requested.connect(func(parent): _open_create("add", parent))
	scene_dock.change_type_requested.connect(func(id):
		var node := Document.find(_root_node(), id)
		if not node.is_empty(): _open_create("change", id, "Change Type of %s" % node.name, "Change", str(node.type)))
	scene_dock.rename_requested.connect(_rename_node)
	scene_dock.move_requested.connect(_move_node)
	scene_dock.delete_requested.connect(_request_delete)
	scene_dock.duplicate_requested.connect(_duplicate_node)
	scene_dock.script_requested.connect(_script_action_for)
	scene_dock.visibility_toggled.connect(_toggle_visibility)
	scene_dock.copy_path_requested.connect(func(id):
		DisplayServer.clipboard_set(Document.relative_path(_root_node(), int(_root_node().id), id))
		_status("Node path copied to clipboard."))
	scene_dock.file_dropped.connect(_file_dropped)
	scene_dock.reorder_requested.connect(_reorder_node)
	filesystem.open_requested.connect(_open_file)
	filesystem.new_script_requested.connect(func(folder): _new_script_file(folder))
	filesystem.new_scene_requested.connect(func(folder): _new_scene(folder))
	filesystem.file_moved.connect(_file_moved)
	filesystem.file_removed.connect(func(path): script_editor.file_removed(path); _log("Deleted: " + path))
	filesystem.main_scene_requested.connect(func(path):
		project.config.main_scene = path
		project.save()
		filesystem.refresh()
		_log("Main scene: " + path, Style.ACCENT))
	filesystem.message.connect(func(text, is_error): _log(text, Style.ERROR if is_error else Style.MUTED))
	signals_dock.connect_requested.connect(_connect_requested)
	signals_dock.disconnect_requested.connect(_remove_connection)
	signals_dock.method_requested.connect(_goto_method)
	scene_dock.signals_requested.connect(func(id):
		_select(id)
		right_tabs.current_tab = 1)
	animation_panel.apply = func(merge, final, mutate):
		var id := animation_panel.player_id
		_repose = true
		_change(merge, final, func():
			var player := Document.find(_root_node(), id)
			if not player.is_empty(): mutate.call(player))
	animation_panel.scene_root = func(): return _root_node() if current >= 0 else {}
	animation_panel.animatable = _animatable_properties
	animation_panel.preview_requested.connect(func(id, name, at):
		if bridge != null and not playing:
			bridge.preview_animation(id, name, at)
			needs_render = true)
	inspector.keyable = func(node, key):
		return animation_panel.visible and not animation_panel.animation.is_empty() and not _animatable(node, key).is_empty()
	inspector.key_requested.connect(_key_property)
	inspector.property_edited.connect(_property_edited)
	inspector.script_property_edited.connect(_script_property_edited)
	inspector.script_action.connect(func(action): _script_action_for(_tab().selected if current >= 0 else 0, action))
	inspector.resolve_path = func(path): return project.to_abs(path)
	inspector.texture_requested.connect(func(key): _quick_open("texture", func(path): _property_edited(key, path, true)))
	inspector.model_requested.connect(func(key): _quick_open("model", func(path): _property_edited(key, path, true)))
	inspector.model_parts = func(path): return _model_info(str(path)).get("parts", []) if not str(path).is_empty() else []
	inspector.import_applied.connect(_import_applied)
	inspector.imported_size = _texture_size
	filesystem.file_selected.connect(_file_selected)
	viewport.file_dropped.connect(_file_dropped)
	serial_monitor.script_link.connect(_open_script_at)
	serial_monitor.decode = _decode_firmware_addresses
	serial_monitor.connection_changed.connect(func(connected):
		bottom_tabs.set_tab_icon(2, Icons.get_icon("StatusSuccess") if connected else null))
	serial_monitor.preferences_changed.connect(func(port, baud):
		settings.set_value("serial", "port", port)
		settings.set_value("serial", "baud", baud)
		if not _smoke and _export_path.is_empty(): settings.save(SETTINGS))
	viewport.model_dropped.connect(func(path, at): if current >= 0: _instantiate_model(int(_root_node().id), path, at))
	viewport.control_moved.connect(_control_moved)
	viewport.canvas_edited.connect(_canvas_edited)
	viewport.texture_size = _texture_size
	viewport.mesh_bounds = _mesh_bounds
	viewport.control_rect = func(node): return Document.control_rect(_root_node(), node, _texture_size(str(node.get("texture", ""))))
	viewport.camera_changed.connect(func(): needs_render = true)
	viewport.node_picked.connect(func(id): if current >= 0: _select(id if id > 0 else 0))
	viewport.transform_edited.connect(_transform_edited)
	viewport.delete_requested.connect(func(): if current >= 0: _request_delete(_tab().selected))
	viewport.duplicate_requested.connect(func(): if current >= 0: _duplicate_node(_tab().selected))
	script_editor.saved.connect(func(path):
		_script_cache.erase(path)
		_log("Script saved: " + path)
		_upload())
	script_editor.script_info_changed.connect(func(path, _info):
		if current >= 0 and not playing:
			var node := Document.find(_root_node(), _tab().selected)
			# The Signals dock marks connections whose method is missing from any script.
			if (not node.is_empty() and str(node.get("script", "")) == path) or signals_dock.visible: _refresh_inspector())
	script_editor.new_script_requested.connect(func(): _new_script_file("res://scripts"))
	script_editor.message.connect(func(text, is_error): _log(text, Style.ERROR if is_error else Style.MUTED))
	_set_screen(Screen.VIEW_3D)

var _add_parent := 0 # the node the Create Node dialog is for
var _create_mode := "add" # what it is for: add (a child), root (of a new scene) or change (Change Type)
var _new_scene_folder := ""

func _on_type_chosen(type: String) -> void:
	match _create_mode:
		"root": _create_scene(type, _new_scene_folder)
		"change": _change_type(_add_parent, type)
		_: _add_node(type, _add_parent)

func _open_create(mode: String, id: int, title: String = "Create New Node", ok: String = "Create", current_type: String = "") -> void:
	_create_mode = mode
	_add_parent = id
	create_dialog.open(title, ok, current_type)

func load_image_texture(path: String) -> Texture2D:
	var image := Image.load_from_file(path)
	if image == null or image.is_empty(): return null
	image.generate_mipmaps()
	return ImageTexture.create_from_image(image)

func _menu(bar: HBoxContainer, title: String, items: Array) -> void:
	var menu := MenuButton.new()
	menu.text = title
	menu.switch_on_hover = true
	menu.focus_mode = Control.FOCUS_NONE
	var popup := menu.get_popup()
	var actions := {}
	for item in items:
		if item.is_empty():
			popup.add_separator()
			continue
		var id := actions.size()
		if item[1].is_empty(): popup.add_item(item[0], id)
		else: popup.add_icon_item(Icons.get_icon(item[1]), item[0], id)
		actions[id] = item[2]
	popup.id_pressed.connect(func(id): actions[id].call())
	bar.add_child(menu)

func _build_dialogs() -> void:
	create_dialog = CreateNodeDialog.new()
	create_dialog.type_chosen.connect(_on_type_chosen)
	new_scene_dialog = NewSceneDialog.new()
	new_scene_dialog.root_chosen.connect(func(type):
		if type.is_empty(): _open_create("root", 0, "Pick the Root Node of the Scene", "Create")
		else: _create_scene(type, _new_scene_folder))
	add_child(new_scene_dialog)
	add_child(create_dialog)
	attach_dialog = AttachScriptDialog.new()
	attach_dialog.project = project
	attach_dialog.script_chosen.connect(_on_attach_chosen)
	add_child(attach_dialog)
	connect_dialog = ConnectDialog.new()
	connect_dialog.script_functions = func(path): return _script_info(path).get("functions", []).map(func(f): return str(f.name))
	connect_dialog.connection_chosen.connect(func(target_id, method, flags): _add_connection(_connecting["signal"], _connecting.arguments, target_id, method, flags))
	add_child(connect_dialog)
	project_manager = ProjectManager.new()
	project_manager.example_folder = _example_folder()
	project_manager.open_requested.connect(_open_project)
	project_manager.create_requested.connect(_create_project)
	project_manager.forget_requested.connect(func(folder):
		var recent: Array = settings.get_value("projects", "recent", [])
		recent.erase(folder)
		settings.set_value("projects", "recent", recent)
		settings.save(SETTINGS)
		project_manager.open(recent))
	add_child(project_manager)
	save_dialog = FileDialog.new()
	save_dialog.file_mode = FileDialog.FILE_MODE_SAVE_FILE
	save_dialog.access = FileDialog.ACCESS_FILESYSTEM
	save_dialog.title = "Save Scene"
	save_dialog.filters = PackedStringArray(["*.jetscene ; Jet Studio Scene"])
	save_dialog.file_selected.connect(func(path):
		var res: String = project.to_res(path)
		if not res.begins_with("res://"):
			_fail("Save the scene inside the project folder:\n" + project.root)
			return
		if res.get_extension() != "jetscene": res += ".jetscene"
		_save_scene(current, res))
	add_child(save_dialog)
	export_dialog = FileDialog.new()
	export_dialog.file_mode = FileDialog.FILE_MODE_SAVE_FILE
	export_dialog.access = FileDialog.ACCESS_FILESYSTEM
	export_dialog.title = "Export Runtime Package of the Main Scene"
	export_dialog.filters = PackedStringArray(["*.jetpack ; Jet Runtime Package"])
	export_dialog.current_file = "game.jetpack"
	export_dialog.file_selected.connect(func(path): _export_to(path))
	add_child(export_dialog)
	error_dialog = AcceptDialog.new()
	error_dialog.title = "Jet Studio"
	add_child(error_dialog)
	confirm_dialog = ConfirmationDialog.new()
	confirm_dialog.cancel_button_text = "Cancel"
	add_child(confirm_dialog)
	compress_dialog = ConfirmationDialog.new()
	compress_dialog.title = "Compress Textures"
	compress_dialog.confirmed.connect(_apply_compression)
	compress_dialog.canceled.connect(func():
		# Declined after an import: don't ask again for these files.
		if not _after_compression.is_valid():
			for path in _compression: _kept_textures[path] = true
		_compression = {})
	add_child(compress_dialog)
	quit_dialog = ConfirmationDialog.new()
	quit_dialog.cancel_button_text = "Cancel"
	quit_dialog.title = "Unsaved Changes"
	quit_dialog.ok_button_text = "Save & Quit"
	quit_dialog.add_button("Quit Without Saving", true, "discard")
	quit_dialog.confirmed.connect(func():
		if _save_all(): _quit())
	quit_dialog.custom_action.connect(func(action):
		if action == "discard":
			quit_dialog.hide()
			_quit())
	add_child(quit_dialog)
	close_dialog = ConfirmationDialog.new()
	close_dialog.cancel_button_text = "Cancel"
	close_dialog.title = "Unsaved Changes"
	close_dialog.ok_button_text = "Save & Close"
	close_dialog.add_button("Close Without Saving", true, "discard")
	close_dialog.confirmed.connect(func():
		if _save_scene(_pending_close): _close_tab(_pending_close, true))
	close_dialog.custom_action.connect(func(action):
		if action == "discard":
			close_dialog.hide()
			_close_tab(_pending_close, true))
	add_child(close_dialog)
	delete_dialog = ConfirmationDialog.new()
	delete_dialog.cancel_button_text = "Cancel"
	delete_dialog.title = "Delete Node"
	delete_dialog.ok_button_text = "Delete"
	delete_dialog.confirmed.connect(func(): _delete_node(_pending_delete))
	add_child(delete_dialog)
	quick_dialog = ConfirmationDialog.new()
	quick_dialog.cancel_button_text = "Cancel"
	quick_dialog.ok_button_text = "Open"
	var quick_box := VBoxContainer.new()
	quick_dialog.add_child(quick_box)
	quick_filter = LineEdit.new()
	quick_filter.placeholder_text = "Search"
	quick_filter.right_icon = Icons.get_icon("Search")
	quick_box.add_child(quick_filter)
	quick_list = ItemList.new()
	quick_list.custom_minimum_size = Vector2(480, 320)
	quick_list.size_flags_vertical = Control.SIZE_EXPAND_FILL
	quick_list.item_activated.connect(func(_i): quick_dialog.hide(); _quick_confirm())
	quick_box.add_child(quick_list)
	quick_dialog.register_text_enter(quick_filter)
	quick_dialog.confirmed.connect(_quick_confirm)
	add_child(quick_dialog)
	project_settings = ProjectSettingsDialog.new()
	project_settings.applied.connect(func():
		project_label.text = "/  " + project.project_name()
		_apply_hardware()
		filesystem.refresh()
		_refresh_title())
	add_child(project_settings)
	help_dialog = AcceptDialog.new()
	help_dialog.title = "Controls and Shortcuts"
	var help := RichTextLabel.new()
	help.bbcode_enabled = true
	help.custom_minimum_size = Vector2(600, 440)
	help.text = """[b]3D View (as in Godot)[/b]
Middle button + drag: orbit around the point of interest
Shift + middle button: pan the view · Ctrl + middle button: zoom
Hold right button: look around · with W A S D / Q E fly (Shift faster, wheel = speed)
Wheel: zoom · Alt + left button: emulates the middle button (touchpad)
Left click: select · F: focus the selection · Numpad 1/3/7: front/right/top views
Q select · W move · E rotate · R scale · T local space · Y snap

[b]Scene[/b]
Ctrl+A add node · Ctrl+D duplicate · Del delete · F2 rename · drag nodes to change their parent
Drag a .jet file from the FileSystem onto a node to attach the script

[b]General[/b]
F5 run main scene · F6 run current scene · F7 pause · F8 stop
Ctrl+S save (the scene, or the script in the Script screen) · Ctrl+Z / Ctrl+Shift+Z undo/redo
Ctrl+F2 3D view · Ctrl+F3 script · Ctrl+F4 game

[b]Script Editor[/b]
Ctrl+Space completion · Ctrl+F find · F3 find next · Ctrl+L go to line · Ctrl+K toggle comment
"""
	help_dialog.add_child(help)
	add_child(help_dialog)

# ------------------------------------------------------------------ logging

func _log(message: String, color: Color = Style.MUTED) -> void:
	print("[Jet Studio] " + message)
	if output == null: return
	output.push_color(color)
	output.add_text(message + "\n")
	output.pop()

func _log_link(message: String, path: String, line: int, color: Color) -> void:
	print("[Jet Studio] %s (%s:%d)" % [message, path, line])
	output.push_color(color)
	output.add_text(message + "  ")
	output.push_meta("%s:%d" % [path, line])
	output.push_underline()
	output.add_text("%s:%d" % [path, line])
	output.pop()
	output.pop()
	output.add_text("\n")
	output.pop()

func _on_meta(meta: Variant) -> void:
	var text := str(meta)
	var colon := text.rfind(":")
	if colon > 6: _open_script_at(text.substr(0, colon), int(text.substr(colon + 1)))

func _status(text: String) -> void:
	status_label.text = text

func _fail(message: String) -> void:
	_log(message, Style.ERROR)
	error_dialog.dialog_text = message
	error_dialog.popup_centered()

func _alert(message: String) -> void:
	error_dialog.dialog_text = message
	error_dialog.popup_centered()

func _debug_error(message: String, path: String, line: int) -> void:
	var text := "%s  —  %s:%d" % [message, path, line] if not path.is_empty() else message
	var index := errors_list.add_item(text, Icons.get_icon("StatusError"))
	errors_list.set_item_metadata(index, {"path": path, "line": line})
	errors_list.set_item_tooltip(index, text)
	_update_debugger_tab()

func _update_debugger_tab() -> void:
	var count := errors_list.item_count
	bottom_tabs.set_tab_title(1, "Debugger (%d)" % count if count > 0 else "Debugger")
	bottom_tabs.set_tab_icon(1, Icons.get_icon("StatusError") if count > 0 else null)

# ------------------------------------------------------------------ project

func _open_project(folder: String) -> void:
	if playing: _stop()
	if not project.root.is_empty(): _save_state()
	var error: String = project.open(folder)
	if not error.is_empty():
		_fail(error)
		if project.root.is_empty(): _show_project_manager()
		return
	tabs.clear()
	current = -1
	_switching = true
	while scene_tabs.tab_count > 0: scene_tabs.remove_tab(0)
	_switching = false
	script_editor.close_all()
	_script_cache.clear()
	errors_list.clear()
	_update_debugger_tab()
	filesystem.refresh()
	var release: bool = project.config.get("profile", "debug") == "release"
	# "nano" is the device target of projects saved before the STM32 boards.
	target_select.select((2 if release else 1) if project.config.get("target", "desktop") in ["device", "nano"] else 0)
	_apply_hardware()
	var recent: Array = settings.get_value("projects", "recent", [])
	recent.erase(project.root)
	recent.push_front(project.root)
	settings.set_value("projects", "recent", recent.slice(0, 12))
	settings.set_value("projects", "last", project.root)
	if not _smoke and _export_path.is_empty(): settings.save(SETTINGS)
	_log("Project opened: %s  (%s)" % [project.project_name(), project.root], Style.ACCENT)
	project_label.text = "/  " + project.project_name()
	var state := _load_state()
	for path in state.get("scenes", []):
		if project.exists(path): _open_scene(path, false)
	if tabs.is_empty():
		if not project.main_scene().is_empty() and project.exists(project.main_scene()): _open_scene(project.main_scene(), false)
		else: _create_scene("Node3D")
	var views: Dictionary = state.get("views", {})
	for tab in tabs:
		if views.has(tab.path): tab.view = views[tab.path]
	_switch_tab(clampi(int(state.get("current", 0)), 0, tabs.size() - 1))
	for path in state.get("scripts", []):
		if project.exists(path): script_editor.open_script(path)
	_set_screen(int(state.get("screen", Screen.VIEW_3D)) if int(state.get("screen", 0)) != Screen.GAME else Screen.VIEW_3D)
	_view_for_scene()

func _state_path() -> String:
	return project.root.path_join(".jetstudio/editor_state.json")

func _load_state() -> Dictionary:
	var data = JSON.parse_string(FileAccess.get_file_as_string(_state_path())) if FileAccess.file_exists(_state_path()) else null
	return data if data is Dictionary else {}

func _save_state() -> void:
	if project.root.is_empty(): return
	if current >= 0: _tab().view = viewport.save_state()
	var scenes := []
	var views := {}
	for tab in tabs:
		if not tab.path.is_empty():
			scenes.append(tab.path)
			views[tab.path] = tab.view
	var state := {"scenes": scenes, "current": maxi(current, 0), "views": views, "screen": screen,
		"scripts": script_editor._order.duplicate()}
	Project.write_text(_state_path(), JSON.stringify(state, "\t"))

const STARTER_SCRIPT := "extends MeshInstance3D\n\n# Rotation speed in degrees per second (editable in the Inspector).\n@export var speed := 45.0\n\n\nfunc _process(delta: float) -> void:\n\trotation_degrees.y += speed * delta\n"

func _create_project(folder: String, title: String) -> void:
	var doc := Document.example()
	var cube := Document.find(doc.root, 6)
	cube.script = "res://scripts/spin.jet"
	cube.script_properties = {}
	var scene := JSON.stringify(doc, "\t") + "\n"
	var error := Project.create(folder, title, scene, STARTER_SCRIPT)
	if not error.is_empty():
		_fail(error)
		return
	_open_project(folder)

func _show_project_manager() -> void:
	project_manager.open(settings.get_value("projects", "recent", []))

func _show_project_settings() -> void:
	project_settings.open(project)

func _apply_hardware() -> void:
	var actions := Actions.of(project.config)
	if bridge != null:
		bridge.set_input_map(Actions.bridge_actions(actions))
		bridge.set_autoloads(project.config.get("autoload", []))
		_script_cache.clear() # scripts may use autoload names that just appeared or went away
	_action_keys = Actions.key_codes(actions)
	var hw := Hardware.settings(project.config)
	var size := Hardware.resolution(hw)
	if bridge != null: bridge.set_resolution(size.x, size.y)
	viewport.set_resolution(size)
	var board: Dictionary = Hardware.boards.get(hw.board, {})
	var short := str(board.get("short", "Device"))
	target_select.set_item_text(1, short + " · Debug")
	target_select.set_item_text(2, short + " · Release")
	var display := Hardware.display_of(hw)
	target_select.tooltip_text = "Build and Flash target.\n%s + %s, %d×%d (Project > Project Settings > Hardware).\n" % [board.get("name", "?"), display.get("name", "?"), size.x, size.y] \
		+ "Debug: serial log, script print(), errors with file and line, FPS and memory (Serial panel).\n" \
		+ "Release: no console, logs or assertions: smaller, lighter firmware."
	flash_button.tooltip_text = "Build and upload the firmware to %s %s" % [board.get("name", "?"),
		"with the ST-LINK (the board's USB)" if Hardware.platform(hw) == "stm32" else "over USB"]
	_upload()

func _set_scale(value: float) -> void:
	settings.set_value("editor", "scale", value)
	settings.save(SETTINGS)
	_apply_scale(value)

# ------------------------------------------------------------------ scene tabs

func _tab() -> Dictionary:
	return tabs[current]

func _root_node() -> Dictionary:
	return tabs[current].doc.root if current >= 0 else {}

func _tab_title(tab: Dictionary) -> String:
	var title: String = tab.path.get_file().get_basename() if not tab.path.is_empty() else "[unsaved]"
	return title + ("(*)" if _dirty(tab) else "")

func _dirty(tab: Dictionary) -> bool:
	return JSON.stringify(tab.doc) != tab.saved

func _add_tab(path: String, doc: Dictionary, saved: String) -> void:
	var root: Dictionary = doc.root
	tabs.append({"path": path, "doc": doc, "saved": saved, "undo": [], "redo": [], "selected": int(root.id), "view": {}})
	_switching = true
	scene_tabs.add_tab(_tab_title(tabs[-1]), Icons.for_node(str(root.type)))
	_switching = false
	_switch_tab(tabs.size() - 1)

func _switch_tab(index: int) -> void:
	if index < 0 or index >= tabs.size(): return
	if playing: _stop()
	if current >= 0 and current < tabs.size(): tabs[current].view = viewport.save_state()
	current = index
	_merge_key = ""
	_switching = true
	scene_tabs.current_tab = index
	_switching = false
	animation_panel.edit(0)
	if tabs[index].view.is_empty(): viewport.reset_view()
	else: viewport.load_state(tabs[index].view)
	_view_for_scene()
	_upload()
	_refresh_all()

## Asks for the root, as Godot's Scene dock does for a new scene.
## As in Godot, a 2D or UI scene shows in the 2D view and a 3D one in the 3D view.
func _view_for_scene() -> void:
	if current < 0 or not screen in [Screen.VIEW_2D, Screen.VIEW_3D]: return
	var root_type := str(_root_node().type)
	if Document.is_2d(root_type) or Document.is_control(root_type): _set_screen(Screen.VIEW_2D)
	elif Document.is_3d(root_type): _set_screen(Screen.VIEW_3D)

func _new_scene(folder: String = "") -> void:
	_new_scene_folder = folder
	new_scene_dialog.open()

func _create_scene(type: String, folder: String = "") -> void:
	var doc := Document.new_scene(type, type)
	if type == "Node3D":
		# Light and environment, so the first meshes aren't drawn in the dark.
		var env := Document.new_node("WorldEnvironment", 2, "WorldEnvironment")
		var sun := Document.new_node("DirectionalLight3D", 3, "Sun")
		sun.rotation = [-50.0, -35.0, 0.0]
		doc.root.children = [env, sun]
	elif type == "Control":
		doc.root.size = [viewport.W, viewport.H] # the whole screen
	_add_tab("", doc, "")
	if not folder.is_empty(): save_dialog.current_dir = project.to_abs(folder)
	_log("New scene with a %s root. Add nodes with the + button of the Scene dock (Ctrl+A)." % type)

## Godot's Change Type: name, children, script and the shared properties stay; connections of signals it no longer has go.
func _change_type(id: int, new_type: String) -> void:
	var node := Document.find(_root_node(), id)
	if playing or node.is_empty() or str(node.type) == new_type: return
	var old_type := str(node.type)
	_change("", true, func():
		var n := Document.find(_root_node(), id)
		Document.change_type(n, new_type)
		var kept := _signal_names(n)
		_tab().doc.connections = Document.connections(_tab().doc).filter(func(c): return int(c.from) != id or str(c["signal"]) in kept)
	, true)
	if id == int(_root_node().id): scene_tabs.set_tab_icon(current, Icons.for_node(new_type))
	_status("%s is now a %s (was %s)." % [node.name, new_type, old_type])
	var path := str(node.get("script", ""))
	var base := str(_script_info(path).get("base", "")) if not path.is_empty() else ""
	if not base.is_empty() and not Document.inherits(new_type, base):
		_log("%s's script extends %s, which a %s isn't: change the script's extends or detach it." % [node.name, base, new_type], Style.WARNING)

func _animatable(node: Dictionary, key: String) -> Dictionary:
	if node.is_empty(): return {}
	var prop := Document.property_def(str(node.type), key)
	if prop.is_empty(): return {}
	# 3D rotation is stored in degrees
	var property := "rotation_degrees" if key == "rotation" and Document.is_3d(str(node.type)) else key
	var cls := str(node.type)
	while not cls.is_empty():
		var found := {}
		for c in script_editor.class_db:
			if c.name == cls: found = c
		if found.is_empty(): return {}
		for p in found.properties:
			if str(p.name) == property and not bool(p.get("read_only", false)):
				var type := str(prop.type)
				if not type in ["float", "int", "bool", "vector2", "vector3", "color"]: type = "string"
				return {"key": key, "label": Document.label_for(prop), "property": property, "type": type}
		cls = str(found.base)
	return {}

func _animatable_properties(node: Dictionary) -> Array:
	var out := []
	for cls in Document.class_chain(str(node.get("type", ""))):
		for prop in Document.PROPERTIES.get(cls, []):
			var info := _animatable(node, str(prop.key))
			if not info.is_empty(): out.append(info)
	return out

func _key_property(key: String) -> void:
	var node := Document.find(_root_node(), _tab().selected)
	var info := _animatable(node, key)
	var player := Document.find(_root_node(), animation_panel.player_id)
	if info.is_empty() or player.is_empty(): return
	var parent := Document.parent_of(_root_node(), int(player.id))
	var path := Document.relative_path(_root_node(), int(parent.id), int(node.id)) if not parent.is_empty() else "."
	var value = node.get(key)
	animation_panel.insert_key(path, str(info.property), key, str(info.type), value.duplicate() if value is Array else value)
	_status("Key: %s:%s at %.2f s." % [node.name, info.property, animation_panel.time])

## The signals a node has: its script's and its classes'.
func _signal_names(node: Dictionary) -> Array:
	var names := []
	for sig in _script_info(str(node.get("script", ""))).get("signals", []): names.append(str(sig.name))
	var cls := str(node.type)
	while not cls.is_empty():
		var found := {}
		for c in script_editor.class_db:
			if c.name == cls: found = c
		if found.is_empty(): break
		for sig in found.get("signals", []): names.append(str(sig.name))
		cls = str(found.base)
	return names

func _open_file(path: String) -> void:
	match path.get_extension():
		"jetscene": _open_scene(path)
		"jet": _open_script_at(path, 0)
		_: OS.shell_open(project.to_abs(path))

func _open_scene(path: String, notify: bool = true) -> void:
	for i in tabs.size():
		if tabs[i].path == path:
			_switch_tab(i)
			return
	var text: String = project.read(path)
	var json := JSON.new()
	if json.parse(text) != OK:
		_fail("%s: invalid JSON at line %d: %s" % [path, json.get_error_line(), json.get_error_message()])
		return
	var data = json.data
	var saved := ""
	if data is Dictionary and int(data.get("version", 0)) == 1:
		var migrated := Document.migrate_v1(data)
		data = migrated[0]
		_log("%s converted from format 1 to format 2: save it to update the file." % path, Style.WARNING)
		for note in migrated[1]: _log("  " + note, Style.WARNING)
	var error := Document.validate(data)
	if not error.is_empty():
		_fail("%s: %s" % [path, error])
		return
	Document.normalize(data)
	if int(json.data.get("version", 0)) == 2: saved = JSON.stringify(data)
	_add_tab(path, data, saved)
	if notify: _log("Scene opened: " + path, Style.ACCENT)

func _save_scene(index: int = -1, path: String = "") -> bool:
	if index < 0: index = current
	if index < 0: return false
	var tab: Dictionary = tabs[index]
	if path.is_empty(): path = tab.path
	if path.is_empty():
		current = index
		_save_scene_as()
		return false
	var doc: Dictionary = tab.doc
	doc.format = "jetscene"
	doc.version = Document.VERSION
	var error: String = project.write(path, JSON.stringify(doc, "\t") + "\n")
	if not error.is_empty():
		_fail(error)
		return false
	tab.path = path
	tab.saved = JSON.stringify(doc)
	if project.main_scene().is_empty():
		project.config.main_scene = path
		project.save()
	_refresh_title()
	filesystem.refresh()
	_log("Scene saved: " + path, Style.ACCENT)
	return true

func _save_scene_as() -> void:
	if current < 0: return
	var tab := _tab()
	save_dialog.current_dir = project.to_abs(tab.path.get_base_dir()) if not tab.path.is_empty() else project.root
	save_dialog.current_file = (tab.path.get_file() if not tab.path.is_empty() else str(tab.doc.root.name).to_snake_case() + ".jetscene")
	save_dialog.popup_centered_ratio(0.6)

func _save_all() -> bool:
	var ok := script_editor.save_all()
	for i in tabs.size():
		if _dirty(tabs[i]) or tabs[i].path.is_empty():
			if tabs[i].path.is_empty():
				_switch_tab(i)
				_save_scene_as()
				return false
			ok = _save_scene(i) and ok
	return ok

func _close_tab(index: int, force: bool = false) -> void:
	if index < 0 or index >= tabs.size(): return
	if not force and _dirty(tabs[index]):
		_pending_close = index
		close_dialog.dialog_text = "Save changes to \"%s\" before closing?" % _tab_title(tabs[index]).trim_suffix("(*)")
		close_dialog.popup_centered()
		return
	if playing: _stop()
	tabs.remove_at(index)
	_switching = true
	scene_tabs.remove_tab(index)
	_switching = false
	current = -1
	if tabs.is_empty(): _create_scene("Node3D")
	else: _switch_tab(mini(index, tabs.size() - 1))

func _refresh_title() -> void:
	if current < 0: return
	for i in tabs.size(): scene_tabs.set_tab_title(i, _tab_title(tabs[i]))
	var tab := _tab()
	get_window().title = "%s - %s - Jet Studio" % [_tab_title(tab), project.project_name()]
	var unsaved := _dirty(tab)
	_status(("Unsaved changes · " if unsaved else "") + (tab.path if not tab.path.is_empty() else "Scene not saved yet"))

# ------------------------------------------------------------------ refresh

const BOOT_SPLASH := "res://.jetstudio/boot_splash.jetscene"
const BOOT_SPLASH_SCRIPT := "res://.jetstudio/boot_splash.jet"
const BOOT_SPLASH_LOGO := "res://.jetstudio/boot_splash_logo.png"

func _boot_splash_doc(main: String) -> Dictionary:
	var size := Hardware.resolution(Hardware.settings(project.config))
	var title_scale := 2 if size.x >= 160 else 1
	var top := (size.y - (64 + 10 + 15 + 6 + 15 * title_scale)) / 2
	var made_with := "made with"
	var title := "JET STUDIO"
	var root := {"id": 1, "name": "BootSplash", "type": "Node3D", "script": BOOT_SPLASH_SCRIPT, "script_properties": {}, "children": [
		{"id": 2, "name": "WorldEnvironment", "type": "WorldEnvironment", "background_color": 0x10141c, "children": []},
		{"id": 3, "name": "Logo", "type": "TextureRect", "texture": BOOT_SPLASH_LOGO, "position": [(size.x - 64) / 2, top], "children": []},
		{"id": 4, "name": "MadeWith", "type": "Label", "text": made_with, "color": 0x8a93a6,
			"position": [(size.x - made_with.length() * 7) / 2, top + 74], "children": []},
		{"id": 5, "name": "Title", "type": "Label", "text": title, "color": 0xe2e7f0, "text_scale": title_scale,
			"position": [(size.x - title.length() * 7 * title_scale) / 2, top + 95], "children": []},
	]}
	var doc := {"format": "jetscene", "version": 2, "root": root}
	Document.normalize(doc)
	return doc

func _boot_splash_script(main: String) -> String:
	return """extends Node3D

var time := 0.0
@onready var parts := [get_node("Logo"), get_node("MadeWith"), get_node("Title")]


func _process(delta: float) -> void:
	time += delta
	var fade := clampf(minf(time, 2.2 - time) / 0.4, 0.0, 1.0)
	for part in parts:
		part.alpha = fade
	if time >= 2.2:
		get_tree().change_scene_to_file("%s")
""" % main

func _boot_splash_logo() -> Image:
	return Image.load_from_file("res://assets/emblem.png")

# The project's other scenes, so change_scene_to_file() finds them in Play and in the package.
func _other_scenes(except: String) -> Dictionary:
	var out := {}
	var stack: Array = [project.scan()]
	while not stack.is_empty():
		var entry: Dictionary = stack.pop_back()
		for child in entry.get("children", []):
			if child.dir:
				stack.append(child)
				continue
			var path := str(child.path)
			if path.get_extension() != "jetscene" or path == except: continue
			var doc := _scene_doc_for(path)
			if doc.is_empty(): _log("Scene skipped, it can't be read: " + path, Style.WARNING)
			else: out[path] = doc
	return out

func _all_sources(doc: Dictionary, others: Dictionary) -> Dictionary:
	var sources := _scene_sources(doc)
	for other in others.values(): sources.merge(_scene_sources(other))
	sources.merge(_autoload_sources())
	return sources

func _autoload_sources() -> Dictionary:
	var sources := {}
	for entry in project.config.get("autoload", []):
		var path := str(entry.get("path", ""))
		var text = script_editor.source_of(path)
		if text == null: text = project.read(path) if project.exists(path) else ""
		sources[path] = text
	return sources

func _all_assets(doc: Dictionary, others: Dictionary) -> Dictionary:
	var assets := _scene_assets(doc)
	for other in others.values():
		var more := _scene_assets(other)
		assets.textures.merge(more.textures)
		assets.models.merge(more.models)
	return assets

func _scene_sources(doc: Dictionary) -> Dictionary:
	var sources := {}
	Document.walk(doc.root, func(n, _p, _d):
		var path := str(n.get("script", ""))
		if path.is_empty() or sources.has(path): return
		var text = script_editor.source_of(path)
		if text == null: text = project.read(path) if project.exists(path) else ""
		sources[path] = text)
	return sources

# ------------------------------------------------------------------ assets and import settings

var _image_cache := {} # absolute path -> {stamp, image, palette}
var _model_cache := {} # res path -> {obj_stamp, obj, mtl_file, mtl_stamp, mtl, options, info_stamp, info}
var _import_cache := {} # res path -> {mtime, settings}
const PACK_LIMIT := 1024 * 1024 # the largest package the firmware build embeds (tools/tasks.py)
const HEAVY_TEXTURE := 96 * 1024 # above this, adding a texture offers to compress it
const BOARD_TEXTURE_SIDE := 256
const BOARD_TRIANGLES := 3000 # above this, adding a model warns about the frame rate on the board
var _pack_textures := [] # textures of the last exported package
var _pack_memory := {} # its RAM estimate on the board (bridge.memory_estimate)
var _kept_textures := {} # textures the user chose to keep as they are, this session
var _compression := {} # res path -> import settings waiting for the user's answer
var _after_compression := Callable()

func _import_file(path: String) -> String:
	return project.to_abs(path) + ".import"

func _file_stamp(file: String) -> int:
	return FileAccess.get_modified_time(file) if FileAccess.file_exists(file) else 0

## Reads "<file>.import" next to a PNG or OBJ, or returns the defaults.
func _import_settings(path: String) -> Dictionary:
	var file := _import_file(path)
	var mtime := _file_stamp(file)
	var cached: Dictionary = _import_cache.get(path, {})
	if not cached.is_empty() and cached.mtime == mtime: return cached.settings
	var settings := Document.import_defaults(Document.import_kind(path))
	if mtime > 0:
		var data = JSON.parse_string(FileAccess.get_file_as_string(file))
		var params = data.get("params") if data is Dictionary else null
		if params is Dictionary:
			for key in settings:
				var value = params.get(key)
				if value == null: continue
				if settings[key] is float and (value is float or value is int): settings[key] = float(value)
				elif typeof(value) == typeof(settings[key]): settings[key] = value
	_import_cache[path] = {"mtime": mtime, "settings": settings}
	return settings

func _texture_image(path: String) -> Image:
	var file: String = project.to_abs(path)
	if path.is_empty() or not FileAccess.file_exists(file): return null
	var settings := _import_settings(path)
	var stamp := "%d:%s" % [FileAccess.get_modified_time(file), JSON.stringify(settings)]
	var cached: Dictionary = _image_cache.get(file, {})
	if cached.get("stamp", "") != stamp:
		var image := _imported_image(file, settings)
		if image == null: return null
		cached = {"stamp": stamp, "image": image, "palette": settings.palette}
		_image_cache[file] = cached
	return cached.image

func _imported_image(file: String, settings: Dictionary) -> Image:
	var image := Image.load_from_file(file)
	if image == null or image.is_empty(): return null
	if settings.flip_v: image.flip_y()
	var limit := int(settings.max_size) if str(settings.max_size).is_valid_int() else 0
	var size := image.get_size()
	if limit > 0 and maxi(size.x, size.y) > limit:
		var k := float(limit) / maxi(size.x, size.y)
		image.resize(maxi(1, roundi(size.x * k)), maxi(1, roundi(size.y * k)), Image.INTERPOLATE_LANCZOS)
	return image

# ------------------------------------------------------------------ texture budget for the board

## {size, bytes, indexed, colors} of a PNG imported with these settings.
func _texture_cost(path: String, settings: Dictionary) -> Dictionary:
	var image := _texture_image(path) if settings == _import_settings(path) else _imported_image(project.to_abs(path), settings)
	if image == null or bridge == null: return {}
	var info: Dictionary = bridge.texture_info(image, settings.palette)
	if not info.is_empty(): info.size = image.get_size()
	return info

func _texture_text(cost: Dictionary) -> String:
	return "%d×%d, %s, %s" % [cost.size.x, cost.size.y, ("%d colors" % cost.colors) if cost.indexed else "RGB565",
		String.humanize_size(int(cost.bytes))]

## What suits the board when a texture is added: at most 256 px and 256 colors (unless RGB565 was chosen on purpose).
func _board_texture(settings: Dictionary, cost: Dictionary) -> Dictionary:
	var out := settings.duplicate()
	if maxi(cost.size.x, cost.size.y) > BOARD_TEXTURE_SIDE: out.max_size = str(BOARD_TEXTURE_SIDE)
	if not cost.indexed and settings.palette != "RGB565": out.palette = "Quantize"
	return out

## One step smaller: 256 colors first, then the next Max Size below the current side. Empty when it can't shrink.
func _smaller_texture(settings: Dictionary, cost: Dictionary) -> Dictionary:
	var out := settings.duplicate()
	if not cost.indexed:
		out.palette = "Quantize"
		return out
	for step in ["1024", "512", "256", "128", "64", "32"]:
		if int(step) < maxi(cost.size.x, cost.size.y):
			out.max_size = step
			return out
	return {}

## After a model or an image is added: offers to compress the textures that are heavy for the board.
func _offer_compression(paths: Array) -> void:
	var plan := {}
	var lines := []
	for path in paths:
		if plan.has(path) or _kept_textures.has(path): continue
		var settings := _import_settings(path)
		var cost := _texture_cost(path, settings)
		if cost.is_empty() or int(cost.bytes) <= HEAVY_TEXTURE: continue
		var lighter := _board_texture(settings, cost)
		var after := _texture_cost(path, lighter)
		if after.is_empty() or int(after.bytes) >= int(cost.bytes): continue
		plan[path] = lighter
		lines.append("• %s: %s → %s" % [path.get_file(), _texture_text(cost), _texture_text(after)])
	if plan.is_empty(): return
	_ask_compression(plan, "These textures are heavy for the board:\n%s\n\nCompress them? Only the import settings change, the PNG files stay as they are." % "\n".join(lines),
		"Compress", "Keep Original", Callable())

## The import settings that bring the package under the board's limit, shrinking the largest texture first.
func _pack_plan(pack_size: int) -> Dictionary:
	var entries := {}
	for path in _pack_textures:
		var settings := _import_settings(path)
		var cost := _texture_cost(path, settings)
		if not cost.is_empty(): entries[path] = {"settings": settings, "cost": cost, "before": cost, "done": false}
	var size := pack_size
	while size > PACK_LIMIT:
		var largest := ""
		for path in entries:
			if not entries[path].done and (largest.is_empty() or int(entries[path].cost.bytes) > int(entries[largest].cost.bytes)):
				largest = path
		if largest.is_empty(): break
		var entry: Dictionary = entries[largest]
		var smaller := _smaller_texture(entry.settings, entry.cost)
		var cost := _texture_cost(largest, smaller) if not smaller.is_empty() else {}
		if cost.is_empty() or int(cost.bytes) >= int(entry.cost.bytes):
			entry.done = true
			continue
		size -= int(entry.cost.bytes) - int(cost.bytes)
		entry.settings = smaller
		entry.cost = cost
	var plan := {}
	for path in entries:
		if entries[path].settings != _import_settings(path): plan[path] = entries[path]
	return {"size": size, "plan": plan}

## The game's RAM on the board, from the last export: an estimate, logged before building.
## Without PSRAM it says whether it fits with both field buffers, with one (slower), or not at all.
func _report_memory() -> void:
	if _pack_memory.is_empty(): return
	var hw := Hardware.settings(project.config)
	var board: Dictionary = Hardware.boards.get(str(hw.board), {})
	var heaviest := {}
	for scene in _pack_memory.scenes:
		if heaviest.is_empty() or int(scene.total) > int(heaviest.total): heaviest = scene
	var shared: int = int(_pack_memory.engine) + int(_pack_memory.frames) + int(_pack_memory.textures) + int(_pack_memory.scripts)
	var need := shared + int(heaviest.get("total", 0))
	var field := int(_pack_memory.field)
	var buffers := int(_pack_memory.frames) + field

	var kb := func(bytes: int) -> String: return "%d KB" % ceili(bytes / 1024.0)
	var detail := "textures %s, scripts %s, frame buffers %s; heaviest scene %s: nodes %s, meshes %s, triangle queues %s, particles %s" % [
		kb.call(int(_pack_memory.textures)), kb.call(int(_pack_memory.scripts)), kb.call(buffers),
		str(heaviest.get("scene", "")).get_file(), kb.call(int(heaviest.get("nodes", 0))), kb.call(int(heaviest.get("meshes", 0))),
		kb.call(int(heaviest.get("triangles", 0))), kb.call(int(heaviest.get("particles", 0)))]
	var extra := "Plus what scripts create while playing (spawned scenes, arrays)."

	# The ESP32 keeps the frame buffers in internal RAM and loads the scene into PSRAM.
	if board.has("psram"):
		var megabytes := int(str(board.psram).to_int())
		var in_psram := need + field - buffers
		if in_psram <= megabytes * 1024 * 1024:
			_log("RAM (estimate): about %s in PSRAM (of %d MB) and %s of frame buffers in internal RAM (%s). %s" % [
				kb.call(in_psram), megabytes, kb.call(buffers), detail, extra], Style.MUTED)
		else:
			_log("RAM (estimate): about %s, the board has %d MB of PSRAM: the game probably won't start (%s)." % [
				kb.call(in_psram), megabytes, detail], Style.ERROR)
		return

	# What's left of the RAM for the heap: the firmware's own variables and the stack take about 32 KB.
	var ram := int(str(board.get("ram", "0")).to_int()) * 1024
	var available := ram - 32 * 1024
	if need + field <= available * 9 / 10:
		_log("RAM (estimate): about %s of %s (%s). %s" % [kb.call(need + field), kb.call(available), detail, extra], Style.MUTED)
	elif need + field <= available:
		_log("RAM (estimate): about %s of %s, close to the limit (%s). %s If it runs out, drawing waits for the display (slower)." % [
			kb.call(need + field), kb.call(available), detail, extra], Style.WARNING)
	elif need <= available:
		_log("RAM (estimate): about %s of %s: it fits with one field buffer, so drawing waits for the display (slower). %s." % [
			kb.call(need), kb.call(available), detail], Style.WARNING)
	else:
		_log("RAM (estimate): about %s, the board has %s: the game probably won't start (%s). Use smaller textures, fewer nodes or simpler meshes." % [
			kb.call(need), kb.call(available), detail], Style.ERROR)

## Stops a board build whose package is over the limit, offering the compression that makes it fit.
func _fits_board(pack_path: String, action: String) -> bool:
	var size := FileAccess.get_file_as_bytes(pack_path).size()
	if size <= PACK_LIMIT: return true
	var result := _pack_plan(size)
	var limit := String.humanize_size(PACK_LIMIT)
	if result.plan.is_empty() or result.size > PACK_LIMIT:
		_fail("The game takes %s, the board holds %s.\nEven with smaller textures it would take %s: use fewer or simpler models (fewer triangles and parts)." % [
			String.humanize_size(size), limit, String.humanize_size(result.size)])
		return false
	var settings := {}
	var lines := []
	for path in result.plan:
		var entry: Dictionary = result.plan[path]
		settings[path] = entry.settings
		lines.append("• %s: %s → %s" % [path.get_file(), _texture_text(entry.before), _texture_text(entry.cost)])
	_ask_compression(settings, "The game takes %s, the board holds %s.\nCompressing these textures brings it to %s:\n%s\n\nOnly the import settings change, the PNG files stay as they are." % [
		String.humanize_size(size), limit, String.humanize_size(result.size), "\n".join(lines)],
		"Compress and %s" % action.capitalize(), "Cancel", func(): _start_task(action))
	return false

func _ask_compression(plan: Dictionary, text: String, ok: String, cancel: String, then: Callable) -> void:
	_compression = plan
	_after_compression = then
	compress_dialog.dialog_text = text
	compress_dialog.ok_button_text = ok
	compress_dialog.cancel_button_text = cancel
	compress_dialog.popup_centered()

func _apply_compression() -> void:
	for path in _compression:
		if not _save_import(path, _compression[path]): return
		_log("Compressed for the board: %s (%s)" % [path, _texture_text(_texture_cost(path, _compression[path]))], Style.ACCENT)
	_compression = {}
	_upload()
	_refresh_all()
	if _after_compression.is_valid(): _after_compression.call()

func _texture_size(path: String) -> Vector2i:
	var image := _texture_image(path)
	return image.get_size() if image != null else Vector2i.ZERO

func _model_data(path: String) -> Dictionary:
	var file: String = project.to_abs(path)
	if path.is_empty() or not FileAccess.file_exists(file): return {}
	var settings := _import_settings(path)
	var obj_stamp := "%d:%s" % [FileAccess.get_modified_time(file), JSON.stringify(settings)]
	var cached: Dictionary = _model_cache.get(path, {})
	if cached.get("obj_stamp", "") != obj_stamp:
		var text := FileAccess.get_file_as_string(file)
		var mtl_file := ""
		var at := text.find("mtllib ")
		if at >= 0 and (at == 0 or text[at - 1] == "\n"):
			var end := text.find("\n", at)
			if end < 0: end = text.length()
			mtl_file = file.get_base_dir().path_join(text.substr(at + 7, end - at - 7).strip_edges())
		cached = {"obj_stamp": obj_stamp, "obj": text, "mtl_file": mtl_file, "mtl_stamp": -1, "mtl": "", "options": settings}
		_model_cache[path] = cached
	var mtl_stamp := _file_stamp(cached.mtl_file) if not cached.mtl_file.is_empty() else 0
	if cached.mtl_stamp != mtl_stamp:
		cached.mtl = FileAccess.get_file_as_string(cached.mtl_file) if mtl_stamp > 0 else ""
		cached.mtl_stamp = mtl_stamp
	return {"obj": cached.obj, "mtl": cached.mtl, "options": cached.options}

## {error, parts, materials} from the bridge, cached per file version.
func _model_info(path: String) -> Dictionary:
	var data := _model_data(path)
	if data.is_empty() or bridge == null: return {}
	var cached: Dictionary = _model_cache[path]
	var stamp := "%s|%d" % [cached.obj_stamp, cached.mtl_stamp]
	if cached.get("info_stamp", "") != stamp:
		cached.info = bridge.import_model(data.obj, data.mtl, data.options)
		cached.info_stamp = stamp
	return cached.info

func _mesh_bounds(node: Dictionary) -> Array:
	if str(node.type) == "Sprite3D":
		# One frame of the texture, pixel_size metres per texel, centred like Godot's.
		var size := Vector2(_texture_size(str(node.get("texture", ""))))
		size = size / Vector2(maxi(1, int(node.get("hframes", 1))), maxi(1, int(node.get("vframes", 1)))) * float(node.get("pixel_size", 0.01))
		if size == Vector2.ZERO: return []
		return [Vector3(-size.x, -size.y, 0) * 0.5, Vector3(size.x, size.y, 0) * 0.5]
	for part in _model_info(str(node.get("mesh_file", ""))).get("parts", []):
		if str(part.name) == str(node.get("mesh_part", "")):
			var box: AABB = part.aabb
			return [box.position, box.end]
	return []

func _scene_assets(doc: Dictionary) -> Dictionary:
	var textures := {}
	for path in Document.texture_paths(doc.root):
		var image := _texture_image(path)
		if image != null: textures[path] = {"image": image, "palette": _image_cache[project.to_abs(path)].palette}
	var models := {}
	for path in Document.model_paths(doc.root):
		var data := _model_data(path)
		if not data.is_empty(): models[path] = data
	return {"textures": textures, "models": models}

## Everything that blocks an export: missing or unreadable files, missing model parts.
func _missing_assets(doc: Dictionary) -> Array:
	var missing := []
	for path in Document.texture_paths(doc.root):
		if _texture_image(path) == null: missing.append(path + " (PNG missing or unreadable)")
	var parts := {}
	for path in Document.model_paths(doc.root):
		var info := _model_info(path)
		if info.is_empty(): missing.append(path + " (file missing)")
		elif not str(info.error).is_empty(): missing.append("%s (%s)" % [path, info.error])
		else: parts[path] = info.parts.map(func(part): return str(part.name))
	Document.walk(doc.root, func(n, _p, _d):
		var path := str(n.get("mesh_file", ""))
		if str(n.get("mesh", "")) == "File" and parts.has(path) and not str(n.get("mesh_part", "")) in parts[path]:
			missing.append("%s: part \"%s\" not found in %s" % [n.name, n.get("mesh_part", ""), path]))
	return missing

func _import_summary(path: String) -> String:
	if Document.import_kind(path) == "texture":
		var original := Image.load_from_file(project.to_abs(path))
		var image := _texture_image(path)
		if original == null or image == null: return "Unreadable image (PNG only)."
		var info: Dictionary = bridge.texture_info(image, _import_settings(path).palette) if bridge else {}
		var text := "Original %d × %d px, imported %d × %d px." % [original.get_width(), original.get_height(), image.get_width(), image.get_height()]
		if not info.is_empty():
			text += "\n%s · %d colors · %s in memory." % ["8-bit palette" if info.indexed else "RGB565", int(info.colors),
				String.humanize_size(int(info.bytes))]
		return text
	var model := _model_info(path)
	if model.is_empty(): return "Unreadable file."
	if not str(model.error).is_empty(): return "Error: " + str(model.error)
	var triangles := 0
	for part in model.parts: triangles += int(part.triangles)
	var names: Array = model.parts.map(func(part): return str(part.name))
	return "%d parts, %d triangles, %d materials.\nParts: %s\nDrag the file into the scene to create one node per part." % [
		model.parts.size(), triangles, model.materials.size(), ", ".join(names)]

func _file_selected(path: String) -> void:
	if Document.import_kind(path).is_empty() or not FileAccess.file_exists(project.to_abs(path)): return
	right_tabs.current_tab = 0 # the import settings are in the Inspector
	inspector.edit_import(path, _import_settings(path), _import_summary(path))

func _save_import(path: String, settings: Dictionary) -> bool:
	var error := Project.write_text(_import_file(path), JSON.stringify({"importer": Document.import_kind(path), "params": settings}, "\t") + "\n")
	if not error.is_empty():
		_fail(error)
		return false
	_import_cache[path] = {"mtime": _file_stamp(_import_file(path)), "settings": settings}
	return true

func _import_applied(path: String, settings: Dictionary) -> void:
	if not _save_import(path, settings): return
	_log("Reimported: " + path, Style.ACCENT)
	_upload()
	_refresh_all()
	inspector.edit_import(path, settings, _import_summary(path))

func _upload() -> void:
	if bridge == null or current < 0 or playing: return
	bridge.set_scenes(_tab().path, {}) # editing needs only this scene
	var sources := _scene_sources(_tab().doc)
	sources.merge(_autoload_sources())
	var error: String = bridge.load_scene(_tab().doc, sources, _scene_assets(_tab().doc))
	if not error.is_empty(): _log("Invalid scene: " + error, Style.ERROR)
	# as in Godot, an Inspector or gizmo edit shows the edited value until the time moves again
	if _repose and animation_panel.visible and animation_panel.player_id > 0 and not animation_panel.animation.is_empty():
		bridge.preview_animation(animation_panel.player_id, animation_panel.animation, animation_panel.time)
	_repose = false
	needs_render = true

func _refresh_all() -> void:
	if current < 0: return
	var tab := _tab()
	if Document.find(tab.doc.root, tab.selected).is_empty(): tab.selected = int(tab.doc.root.id)
	scene_dock.connections = Document.connections(tab.doc)
	scene_dock.rebuild(tab.doc.root, tab.selected, playing)
	animation_panel.read_only = playing
	animation_panel.refresh()
	_refresh_inspector()
	viewport.set_scene(tab.doc.root, tab.selected, playing)
	var names := []
	Document.walk(tab.doc.root, func(n, _p, _d): names.append(str(n.name)))
	script_editor.set_scene_node_names(names)
	var connected := {}
	for c in Document.connections(tab.doc):
		var source := Document.find(tab.doc.root, int(c.from))
		var path := str(Document.find(tab.doc.root, int(c.to)).get("script", ""))
		if path.is_empty() or source.is_empty(): continue
		if not connected.has(path): connected[path] = {}
		connected[path][str(c.method)] = "%s.%s" % [source.name, c["signal"]]
	script_editor.set_connected_methods(connected)
	_refresh_title()
	_update_controls()
	needs_render = true

func _refresh_inspector() -> void:
	if current < 0: return
	var node := Document.find(_root_node(), _tab().selected)
	var info := _script_info(str(node.get("script", "")))
	if inspector.visible: inspector.edit(node, info, playing)
	if signals_dock.visible: signals_dock.edit(_tab().doc, node, info, playing)

func _script_info(path: String) -> Dictionary:
	if path.is_empty() or bridge == null: return {}
	var live: Dictionary = script_editor.info_of(path)
	if not live.is_empty(): return live
	var file: String = project.to_abs(path)
	if not FileAccess.file_exists(file): return {"ok": false, "errors": [{"line": 0, "column": 0, "message": "File not found"}]}
	var mtime := FileAccess.get_modified_time(file)
	if _script_cache.has(path) and _script_cache[path].mtime == mtime: return _script_cache[path].info
	var info: Dictionary = bridge.compile_script(FileAccess.get_file_as_string(file), path)
	_script_cache[path] = {"mtime": mtime, "info": info}
	return info

func _select(id: int) -> void:
	if current < 0: return
	_tab().selected = id
	if str(Document.find(_root_node(), id).get("type", "")) == "AnimationPlayer":
		animation_panel.edit(id)
		bottom_tabs.current_tab = 3
	scene_dock.select(id)
	_refresh_inspector()
	viewport.set_scene(_root_node(), id, playing)

func _set_screen(value: int) -> void:
	screen = value
	viewport.visible = value == Screen.VIEW_3D or value == Screen.VIEW_2D
	if viewport.visible: viewport.set_mode_2d(value == Screen.VIEW_2D)
	script_editor.visible = value == Screen.SCRIPT
	game_view.get_parent().get_parent().visible = value == Screen.GAME
	screen_buttons[value].button_pressed = true
	if value == Screen.GAME: game_view.grab_focus()
	needs_render = true

func _update_controls() -> void:
	var busy := _job_id > 0
	play_button.disabled = playing or bridge == null
	play_scene_button.disabled = playing or bridge == null
	pause_button.disabled = not playing
	pause_button.button_pressed = paused
	stop_button.disabled = not playing
	step_button.disabled = not playing or not paused
	build_button.disabled = busy or playing
	flash_button.disabled = busy or playing or target_select.selected == 0
	target_select.disabled = busy
	mode_label.text = "●  PAUSED" if paused else ("●  GAME" if playing else "●  SCENE")
	mode_label.add_theme_color_override("font_color", Style.ORANGE if playing else Style.ACCENT)

# ------------------------------------------------------------------ editing with undo

## Edits with the same merge key (a drag, a colour picker) make one undo step until final is true.
func _change(merge_key: String, final: bool, mutate: Callable, structural: bool = false) -> void:
	if playing or current < 0: return
	var tab := _tab()
	if merge_key.is_empty() or merge_key != _merge_key:
		tab.undo.append(tab.doc.duplicate(true))
		if tab.undo.size() > 200: tab.undo.pop_front()
		tab.redo.clear()
	_merge_key = "" if final else merge_key
	mutate.call()
	_upload()
	if structural:
		_refresh_all()
	else:
		var node := Document.find(tab.doc.root, tab.selected)
		inspector.refresh_values(node)
		viewport.set_scene(tab.doc.root, tab.selected, playing)
		_refresh_title()

func _undo() -> void:
	if playing or current < 0 or _tab().undo.is_empty(): return
	var tab := _tab()
	tab.redo.append(tab.doc.duplicate(true))
	tab.doc = tab.undo.pop_back()
	_merge_key = ""
	_repose = true
	_upload()
	_refresh_all()
	_status("Undo.")

func _redo() -> void:
	if playing or current < 0 or _tab().redo.is_empty(): return
	var tab := _tab()
	tab.undo.append(tab.doc.duplicate(true))
	tab.doc = tab.redo.pop_back()
	_merge_key = ""
	_repose = true
	_upload()
	_refresh_all()
	_status("Redo.")

func _property_edited(key: String, value: Variant, final: bool) -> void:
	var id: int = _tab().selected
	_change("prop:%d:%s" % [id, key], final, func():
		var node := Document.find(_root_node(), id)
		if node.is_empty(): return
		node[key] = value
		if key == "mesh_file":
			# Keep the current part if the new model has it, otherwise use its first part.
			var names: Array = _model_info(str(value)).get("parts", []).map(func(part): return str(part.name))
			if not names.is_empty() and not str(node.get("mesh_part", "")) in names: node.mesh_part = names[0]
	, key in ["visible", "mesh", "current"] or key in Document.condition_keys())

func _script_property_edited(var_name: String, value: Variant, final: bool) -> void:
	var id: int = _tab().selected
	_change("script:%d:%s" % [id, var_name], final, func():
		var node := Document.find(_root_node(), id)
		if node.is_empty(): return
		if not node.has("script_properties") or not node.script_properties is Dictionary: node.script_properties = {}
		if value == null: node.script_properties.erase(var_name)
		else: node.script_properties[var_name] = value
	, value == null)

func _control_moved(id: int, position: Vector2, final: bool) -> void:
	_change("move2d:%d" % id, final, func():
		var node := Document.find(_root_node(), id)
		if not node.is_empty(): node.position = [position.x, position.y])

func _canvas_edited(id: int, values: Dictionary, final: bool) -> void:
	_change("canvas:%d" % id, final, func():
		var node := Document.find(_root_node(), id)
		if node.is_empty(): return
		for key in values: node[key] = values[key])

func _transform_edited(id: int, local: Transform3D, final: bool) -> void:
	_change("gizmo:%d" % id, final, func():
		var node := Document.find(_root_node(), id)
		if not node.is_empty(): Document.set_transform(node, local))

func _add_node(type: String, parent_id: int) -> void:
	if current < 0: return
	var root := _root_node()
	var parent := Document.find(root, parent_id if parent_id > 0 else int(root.id))
	if parent.is_empty(): parent = root
	if Document.count(root) >= 256:
		_fail("The runtime supports at most 256 nodes per scene.")
		return
	var id := Document.next_id(root)
	var node := Document.new_node(type, id, Document.unique_name(parent, type))
	_change("", true, func():
		Document.children(Document.find(_root_node(), int(parent.id))).append(node)
		_tab().selected = id
	, true)
	_status("Added %s under %s." % [type, parent.name])

func _rename_node(id: int, new_name: String) -> void:
	var node := Document.find(_root_node(), id)
	if node.is_empty() or new_name == str(node.name):
		_refresh_all()
		return
	if not Document.valid_name(new_name):
		_fail("Invalid name: \"%s\".\nDo not use . : @ / \" %%" % new_name)
		_refresh_all()
		return
	var parent := Document.parent_of(_root_node(), id)
	var final_name := Document.unique_name(parent, new_name, id) if not parent.is_empty() else new_name
	_change("", true, func(): Document.find(_root_node(), id).name = final_name, true)

func _request_delete(id: int) -> void:
	if playing or current < 0 or id <= 0: return
	var node := Document.find(_root_node(), id)
	if node.is_empty(): return
	if id == int(_root_node().id):
		_fail("The root node cannot be deleted.")
		return
	_pending_delete = id
	var descendants := Document.count(node) - 1
	delete_dialog.dialog_text = "Delete node \"%s\"%s?" % [node.name, (" and its %d descendants" % descendants) if descendants > 0 else ""]
	delete_dialog.popup_centered()

func _delete_node(id: int) -> void:
	var parent := Document.parent_of(_root_node(), id)
	if parent.is_empty(): return
	_change("", true, func():
		var p := Document.parent_of(_root_node(), id)
		Document.children(p).erase(Document.find(_root_node(), id))
		Document.prune_connections(_tab().doc) # as in Godot, the deleted nodes' connections go with them
		_tab().selected = int(p.id)
	, true)

# ------------------------------------------------------------------ signal connections (Signals dock)

var _connecting := {} # the signal the Connect dialog is for: {signal, arguments}

func _connect_requested(signal_name: String, arguments: Array) -> void:
	if playing or current < 0: return
	_connecting = {"signal": signal_name, "arguments": arguments}
	connect_dialog.open(_tab().doc, Document.find(_root_node(), _tab().selected), signal_name)

## Connects the selected node's signal; a missing method is added to the receiver's script, which opens there (Godot does the same).
func _add_connection(signal_name: String, arguments: Array, target_id: int, method: String, flags: int) -> void:
	var from_id: int = _tab().selected
	if Document.find_connection(_tab().doc, from_id, signal_name, target_id, method) >= 0: return
	_change("", true, func():
		Document.connections(_tab().doc).append({"from": from_id, "signal": signal_name, "to": target_id, "method": method, "flags": flags})
	, true)
	var target := Document.find(_root_node(), target_id)
	_status("Connected %s.%s to %s.%s()." % [Document.find(_root_node(), from_id).name, signal_name, target.name, method])
	var path := str(target.get("script", ""))
	if path.is_empty(): return
	var line := 0
	for f in _script_info(path).get("functions", []):
		if str(f.name) == method: line = int(f.line)
	if line == 0: line = script_editor.add_function(path, method, arguments)
	_open_script_at(path, line)

func _remove_connection(index: int) -> void:
	if playing or current < 0 or index < 0 or index >= Document.connections(_tab().doc).size(): return
	var c: Dictionary = Document.connections(_tab().doc)[index]
	_change("", true, func(): Document.connections(_tab().doc).remove_at(index), true)
	_status("Disconnected %s from %s()." % [c["signal"], c.method])

func _goto_method(target_id: int, method: String) -> void:
	var path := str(Document.find(_root_node(), target_id).get("script", ""))
	if path.is_empty(): return
	for f in _script_info(path).get("functions", []):
		if str(f.name) == method:
			_open_script_at(path, int(f.line))
			return
	_open_script_at(path, 0)

## For the Signals dock: a connection's method is in the receiver's script, or is one of its engine methods.
func _method_exists(target: Dictionary, method: String) -> bool:
	if target.is_empty(): return false
	var path := str(target.get("script", ""))
	if not path.is_empty():
		for f in _script_info(path).get("functions", []):
			if str(f.name) == method: return true
	var cls := str(target.type)
	while not cls.is_empty():
		var found := {}
		for c in script_editor.class_db:
			if c.name == cls: found = c
		if found.is_empty(): return false
		for m in found.methods:
			if str(m.name) == method: return true
		cls = str(found.base)
	return false

func _duplicate_node(id: int) -> void:
	if playing or current < 0 or id <= 0 or id == int(_root_node().id): return
	var root := _root_node()
	var node := Document.find(root, id)
	var parent := Document.parent_of(root, id)
	if node.is_empty() or parent.is_empty(): return
	if Document.count(root) + Document.count(node) > 256:
		_fail("The runtime supports at most 256 nodes per scene.")
		return
	var copy := Document.copy_with_new_ids(node, Document.next_id(root))
	copy.name = Document.unique_name(parent, str(node.name))
	# As in Godot, the copies keep the connections of the nodes they copy.
	var new_ids := {}
	var old := []
	Document.walk(node, func(n, _p, _d): old.append(int(n.id)))
	var index := [0]
	Document.walk(copy, func(n, _p, _d):
		new_ids[old[index[0]]] = int(n.id)
		index[0] += 1)
	_change("", true, func():
		var p := Document.parent_of(_root_node(), id)
		var list := Document.children(p)
		list.insert(list.find(Document.find(_root_node(), id)) + 1, copy)
		var links := Document.connections(_tab().doc)
		for c in links.duplicate():
			if new_ids.has(int(c.from)):
				var twin: Dictionary = c.duplicate()
				twin.from = new_ids[int(c.from)]
				twin.to = new_ids.get(int(c.to), int(c.to))
				links.append(twin)
		_tab().selected = int(copy.id)
	, true)

func _move_node(id: int, parent_id: int, index: int) -> void:
	var root := _root_node()
	if id == int(root.id) or Document.is_ancestor(root, id, parent_id) or id == parent_id: return
	var node := Document.find(root, id)
	var new_parent := Document.find(root, parent_id)
	if node.is_empty() or new_parent.is_empty(): return
	var global := Document.global_transform(root, id)
	_change("", true, func():
		var r := _root_node()
		var n := Document.find(r, id)
		var old_parent := Document.parent_of(r, id)
		var target := Document.find(r, parent_id)
		var old_list := Document.children(old_parent)
		var old_index := old_list.find(n)
		old_list.remove_at(old_index)
		var list := Document.children(target)
		var at := index
		if at < 0 or at > list.size(): at = list.size()
		if old_parent == target and old_index < at: at -= 1
		if int(old_parent.id) != int(target.id): n.name = Document.unique_name(target, str(n.name), id)
		list.insert(clampi(at, 0, list.size()), n)
		# Keep the global transform, as Godot does when reparenting.
		if Document.is_3d(str(n.type)):
			var parent_global := Document.global_transform(r, int(target.id)) if Document.is_3d(str(target.type)) else Transform3D.IDENTITY
			Document.set_transform(n, parent_global.affine_inverse() * global)
		_tab().selected = id
	, true)

func _reorder_node(id: int, delta: int) -> void:
	var parent := Document.parent_of(_root_node(), id)
	if parent.is_empty(): return
	var list := Document.children(parent)
	var index := list.find(Document.find(_root_node(), id))
	var target := index + delta
	if target < 0 or target >= list.size(): return
	_change("", true, func():
		var p := Document.parent_of(_root_node(), id)
		var l := Document.children(p)
		var item = l.pop_at(index)
		l.insert(target, item)
	, true)

func _toggle_visibility(id: int) -> void:
	_change("", true, func():
		var node := Document.find(_root_node(), id)
		node.visible = not bool(node.get("visible", true))
	, true)

# ------------------------------------------------------------------ scripts

func _script_action_for(id: int, action: String) -> void:
	if current < 0 or id <= 0: return
	var node := Document.find(_root_node(), id)
	if node.is_empty(): return
	match action:
		"open":
			if not str(node.get("script", "")).is_empty(): _open_script_at(str(node.script), 0)
		"attach":
			if playing: return
			var folder := "res://scripts" if project.exists("res://scripts") else "res://"
			attach_dialog.open(str(node.type), folder.path_join(str(node.name).to_snake_case() + ".jet"))
		"load":
			if playing: return
			_quick_open("script", func(path): _attach_script(id, path))
		"detach":
			if playing: return
			_change("", true, func():
				var n := Document.find(_root_node(), id)
				n.erase("script")
				n.erase("script_properties")
			, true)

func _on_attach_chosen(path: String, create: bool, base: String, template: bool) -> void:
	var id: int = _tab().selected
	if create:
		var text: String = script_editor.new_script_text(base) if template else "extends %s\n" % base
		var error: String = project.write(path, text)
		if not error.is_empty():
			_fail(error)
			return
		filesystem.refresh()
		_log("Script created: " + path, Style.ACCENT)
	_attach_script(id, path)
	_open_script_at(path, 0)

func _file_dropped(id: int, path: String) -> void:
	if playing or current < 0: return
	if path.get_extension() == "jet":
		_attach_script(id, path)
		return
	if Document.import_kind(path) == "model":
		_instantiate_model(id, path)
		return
	if not path.get_extension().to_lower() in Document.IMAGE_EXTENSIONS: return
	var node := Document.find(_root_node(), id)
	if node.is_empty(): return
	match str(node.type):
		"MeshInstance3D":
			_change("", true, func():
				var n := Document.find(_root_node(), id)
				n.albedo_texture = path
				# Godot's default albedo is white, so the texture shows its own colours.
				if int(n.get("color", 0)) == int(Document.default_value("MeshInstance3D", "color")): n.color = 0xffffff
			, true)
			_status("Texture %s applied to %s." % [path.get_file(), node.name])
		"WorldEnvironment":
			_change("", true, func():
				var n := Document.find(_root_node(), id)
				n.sky_texture = path
				n.background_mode = "Panorama"
			, true)
			_status("Panorama %s set." % path.get_file())
		"TextureRect", "Sprite3D", "Sprite2D", "AnimatedSprite2D":
			_change("", true, func(): Document.find(_root_node(), id).texture = path, true)
			_status("Texture %s applied to %s." % [path.get_file(), node.name])
		_:
			_status("Drag images onto a MeshInstance3D or a WorldEnvironment.")
	_select(id)
	if str(node.type) in ["MeshInstance3D", "WorldEnvironment", "TextureRect", "Sprite3D", "Sprite2D", "AnimatedSprite2D"]: _offer_compression([path])

## A Node3D with one MeshInstance3D per OBJ part and the MTL materials, like Godot's imported scenes.
func _instantiate_model(parent_id: int, path: String, at: Variant = null) -> void:
	if playing or current < 0: return
	var info := _model_info(path)
	if info.is_empty():
		_fail("Unreadable model: " + path)
		return
	if not str(info.error).is_empty():
		_fail("Import of %s failed:\n%s\nCheck the import settings (select the file in the FileSystem dock)." % [path, info.error])
		return
	var root := _root_node()
	var parent := Document.find(root, parent_id)
	if parent.is_empty() or Document.is_control(str(parent.type)): parent = root
	if Document.count(root) + info.parts.size() + 1 > 256:
		_fail("The model has %d parts: the scene would exceed 256 nodes." % info.parts.size())
		return
	var materials := {}
	for material in info.materials: materials[str(material.name)] = material
	var mtl_file: String = _model_cache[path].mtl_file
	var texture_dir: String = project.to_res(mtl_file.get_base_dir()) if not mtl_file.is_empty() else path.get_base_dir()
	var next := Document.next_id(root)
	var group := Document.new_node("Node3D", next, Document.unique_name(parent, Document.sanitize_name(path.get_file().get_basename())))
	if at is Vector3: group.position = [at.x, at.y, at.z]
	var notes := []
	for part in info.parts:
		next += 1
		var mesh := Document.new_node("MeshInstance3D", next, Document.unique_name(group, str(part.name)))
		mesh.mesh = "File"
		mesh.mesh_file = path
		mesh.mesh_part = str(part.name)
		var material: Dictionary = materials.get(str(part.material), {})
		mesh.color = int(material.get("color", 0xffffff))
		var texture := str(material.get("texture", "")).replace("\\", "/")
		if not texture.is_empty():
			var candidate := texture_dir.path_join(texture)
			if candidate.get_extension().to_lower() in Document.IMAGE_EXTENSIONS and project.exists(candidate): mesh.albedo_texture = candidate
			elif not texture in notes: notes.append(texture)
		if float(material.get("alpha", 1.0)) < 1.0: mesh.alpha = float(material.alpha)
		if float(material.get("shininess", 0.0)) > 1.0:
			mesh.shading = "Phong"
			mesh.shininess = clampi(int(material.shininess), 1, 255)
		Document.children(group).append(mesh)
	_change("", true, func():
		Document.children(Document.find(_root_node(), int(parent.id))).append(group)
		_tab().selected = int(group.id)
	, true)
	_status("Model %s: %d parts under %s." % [path.get_file(), info.parts.size(), parent.name])
	for texture in notes:
		_log("Material texture \"%s\" not found (it needs a PNG next to the MTL file): assign it in the Inspector." % texture, Style.WARNING)

	var triangles := 0
	for part in info.parts: triangles += int(part.triangles)
	if triangles > BOARD_TRIANGLES:
		_log("%s has %d triangles: on the board each one costs frame time, so simplify it in the 3D modelling program if the game slows down." % [
			path.get_file(), triangles], Style.WARNING)
	var textures := []
	for mesh in Document.children(group):
		if not str(mesh.get("albedo_texture", "")).is_empty(): textures.append(str(mesh.albedo_texture))
	_offer_compression(textures)

func _attach_script(id: int, path: String) -> void:
	if playing: return
	var node := Document.find(_root_node(), id)
	if node.is_empty(): return
	var info := _script_info(path)
	var base := str(info.get("base", ""))
	if not base.is_empty() and not Document.inherits(str(node.type), base):
		_fail("Script %s extends %s: it cannot be attached to a %s node." % [path, base, node.type])
		return
	_change("", true, func():
		var n := Document.find(_root_node(), id)
		n.script = path
		if not n.has("script_properties"): n.script_properties = {}
	, true)
	_status("Script %s attached to %s." % [path, node.name])

func _new_script_file(folder: String) -> void:
	var base := "Node3D"
	var suggested := "new_script"
	if current >= 0:
		var node := Document.find(_root_node(), _tab().selected)
		if not node.is_empty():
			base = str(node.type)
			suggested = str(node.name).to_snake_case()
	if not project.exists(folder): folder = "res://"
	attach_dialog.open(base, folder.path_join(suggested + ".jet"))

func _open_script_at(path: String, line: int) -> void:
	if script_editor.open_script(path, line): _set_screen(Screen.SCRIPT)

func _file_moved(old_path: String, new_path: String) -> void:
	script_editor.file_moved(old_path, new_path)
	var changed := [0]
	for tab in tabs:
		Document.walk(tab.doc.root, func(n, _p, _d):
			for key in Document.PATH_KEYS:
				var path := str(n.get(key, ""))
				if path.is_empty(): continue
				if path == old_path or path.begins_with(old_path + "/"):
					n[key] = new_path + path.trim_prefix(old_path)
					changed[0] += 1)
		if tab.path == old_path or tab.path.begins_with(old_path + "/"): tab.path = new_path + tab.path.trim_prefix(old_path)
	if project.main_scene() == old_path:
		project.config.main_scene = new_path
		project.save()
	_log("Renamed %s → %s%s" % [old_path, new_path, (" (updated %d references in the open scenes: save them)" % changed[0]) if changed[0] > 0 else ""])
	_upload()
	_refresh_all()

# ------------------------------------------------------------------ quick open

func _quick_open(kind: String, action: Callable = Callable()) -> void:
	var extension := {"scene": "jetscene", "script": "jet", "texture": "png", "model": "obj"}.get(kind, "jet")
	if action.is_valid():
		_quick_action = action
	elif kind == "scene":
		_quick_action = func(path): _open_scene(path)
	else:
		_quick_action = func(path): _open_script_at(path, 0)
	quick_dialog.title = {"scene": "Open Scene", "script": "Choose a Script", "texture": "Choose a Texture (PNG)",
		"model": "Choose a Model (OBJ)"}.get(kind, "Open")
	quick_filter.text = ""
	var files := []
	var stack: Array = [project.scan()]
	while not stack.is_empty():
		var entry: Dictionary = stack.pop_back()
		for child in entry.get("children", []):
			if child.dir: stack.append(child)
			elif str(child.path).get_extension() == extension: files.append(child.path)
	files.sort()
	var fill := func():
		quick_list.clear()
		var text := quick_filter.text.strip_edges().to_lower()
		for path in files:
			if text.is_empty() or text in path.to_lower():
				var i := quick_list.add_item(path, Icons.for_file(path))
				quick_list.set_item_metadata(i, path)
		if quick_list.item_count > 0: quick_list.select(0)
	for connection in quick_filter.text_changed.get_connections(): quick_filter.text_changed.disconnect(connection.callable)
	quick_filter.text_changed.connect(func(_t): fill.call())
	fill.call()
	quick_dialog.popup_centered(Vector2i(520, 420))
	quick_filter.grab_focus()

func _quick_confirm() -> void:
	var selected := quick_list.get_selected_items()
	if selected.is_empty() or not _quick_action.is_valid(): return
	_quick_action.call(str(quick_list.get_item_metadata(selected[0])))

# ------------------------------------------------------------------ play

func _input_mask() -> int:
	if screen != Screen.GAME or not get_window().has_focus(): return 0
	var focus := get_viewport().gui_get_focus_owner()
	if focus is LineEdit or focus is TextEdit: return 0
	# bit i = Input Map action i
	var mask := 0
	for i in _action_keys.size():
		for code in _action_keys[i]:
			if Input.is_key_pressed(code):
				mask |= 1 << i
				break
	return mask

func _scene_doc_for(path: String) -> Dictionary:
	for tab in tabs:
		if tab.path == path: return tab.doc
	var data = JSON.parse_string(project.read(path))
	if not data is Dictionary: return {}
	if int(data.get("version", 0)) == 1: data = Document.migrate_v1(data)[0]
	if not Document.validate(data).is_empty(): return {}
	Document.normalize(data)
	return data

func _play(current_scene: bool) -> void:
	if bridge == null or playing or current < 0: return
	script_editor.save_all()
	var doc: Dictionary = _tab().doc
	var title := _tab_title(_tab()).trim_suffix("(*)")
	if not current_scene:
		var main: String = project.main_scene()
		if main.is_empty() or not project.exists(main):
			if _tab().path.is_empty():
				_fail("No main scene: save the current scene (it becomes the main scene) or set one in Project > Project Settings.")
				return
			project.config.main_scene = _tab().path
			project.save()
			main = _tab().path
			_log("Main scene set: " + main, Style.ACCENT)
		doc = _scene_doc_for(main)
		title = main.get_file().get_basename()
		if doc.is_empty():
			_fail("Cannot read the main scene " + main)
			return
	var path: String = _tab().path if current_scene else project.main_scene()
	var others := _other_scenes(path)
	bridge.set_scenes(path, others)
	var error: String = bridge.load_scene(doc, _all_sources(doc, others), _all_assets(doc, others))
	if not error.is_empty():
		_fail("Invalid scene: " + error)
		_upload()
		return
	var errors: Array = bridge.play_start()
	if not errors.is_empty():
		errors_list.clear()
		for e in errors:
			_log_link("Compile error: " + str(e.message), str(e.path), int(e.line), Style.ERROR)
			_debug_error(str(e.message), str(e.path), int(e.line))
		bottom_tabs.current_tab = 1
		_open_script_at(str(errors[0].path), int(errors[0].line))
		_upload()
		return
	playing = true
	paused = false
	play_title = title
	errors_list.clear()
	_update_debugger_tab()
	script_editor.clear_runtime_error()
	_log("▶ Running scene \"%s\" · C++ runtime + JetScript · changes made while playing are temporary" % title, Style.ACCENT)
	# Godot's configuration warnings, so a body without shapes isn't a mystery.
	Document.walk(doc.root, func(n, _p, _d):
		for warning in Document.configuration_warnings(doc.root, n):
			_log("⚠ %s (%s): %s" % [n.name, n.type, warning.replace("\n", " ")], Style.WARNING))
	_set_screen(Screen.GAME)
	get_viewport().gui_release_focus()
	game_label.text = "▶ %s  ·  arrows/WASD, Space/Enter, Esc  ·  F7 pause · F8 stop" % title
	_drain_log()
	_refresh_all()

func _toggle_pause() -> void:
	if not playing: return
	paused = not paused
	_log("Ⅱ Paused" if paused else "▶ Resumed")
	_update_controls()

func _step() -> void:
	if not playing or not paused: return
	bridge.set_input(_input_mask())
	bridge.update(1.0 / 60.0)
	_drain_log()
	needs_render = true

func _stop() -> void:
	if not playing: return
	bridge.play_stop()
	_drain_log()
	playing = false
	paused = false
	script_editor.clear_runtime_error()
	game_label.text = "Game stopped. Press Play (F5) to run it."
	_log("■ Stop · scene state restored")
	if screen == Screen.GAME: _set_screen(Screen.VIEW_3D)
	_upload()
	_refresh_all()

func _drain_log() -> void:
	for line in bridge.take_log():
		var text := str(line.text)
		if line.error:
			if not str(line.path).is_empty():
				_log_link(text, str(line.path), int(line.line), Style.ERROR)
				const prefix := "Script error" # set by runtime/runtime.cpp
				if text.begins_with(prefix): _debug_error(text.trim_prefix(prefix + ": "), str(line.path), int(line.line))
				if pause_on_error and playing and not paused and text.begins_with(prefix):
					paused = true
					_update_controls()
					bottom_tabs.current_tab = 1
					script_editor.show_runtime_error(str(line.path), int(line.line), text.trim_prefix(prefix + ": "))
					_set_screen(Screen.SCRIPT)
					_log("Ⅱ Game paused on the error (F7 to continue, F8 to stop).", Style.WARNING)
			else:
				_log(text, Style.ERROR)
		else:
			_log(text, Style.TEXT)

func _process(delta: float) -> void:
	if bridge == null: return
	if playing and not paused:
		bridge.set_input(_input_mask())
		bridge.update(delta)
		_drain_log()
		needs_render = true
		_live_timer += delta
		if _live_timer > 0.1:
			_live_timer = 0.0
			_show_live_values()
	if needs_render and screen != Screen.SCRIPT: _render()
	if _job_id > 0:
		_job_poll += delta
		if _job_poll > 0.25:
			_job_poll = 0.0
			_poll_task()
	if not _screenshot.is_empty() and not _smoke and current >= 0:
		var path := _screenshot
		_screenshot = ""
		_capture_and_quit.call_deferred(path)

func _render() -> void:
	needs_render = false
	var game := playing and screen == Screen.GAME
	bridge.use_editor_camera(not game)
	bridge.set_editor_canvas(screen == Screen.VIEW_2D, viewport.canvas_zoom, viewport.canvas_origin)
	if not game: bridge.set_editor_camera(viewport.view_origin, viewport.view_rotation(), viewport.view_fov)
	var texture: Texture2D = bridge.render_frame()
	if game: game_view.texture = texture
	else: viewport.set_frame(texture)
	var stats: Dictionary = bridge.statistics()
	var text := "%d nodes · %d meshes · %d triangles · render %.2f ms" % [stats.nodes, stats.meshes, stats.triangles, stats.render_ms]
	if playing: text += " · script %.2f ms" % stats.script_ms
	if int(stats.get("particles", 0)) > 0: text += " · %d particles" % int(stats.particles)
	stats_label.text = text
	viewport.stats.text = "%d triangles" % stats.triangles

func _show_live_values() -> void:
	if current < 0: return
	var node := Document.find(_root_node(), _tab().selected)
	if node.is_empty(): return
	var live: Dictionary = bridge.node_state(int(node.id))
	if live.is_empty(): return
	var shown := node.duplicate()
	for key in ["position", "rotation", "scale"]:
		if live.has(key) and Document.is_3d(str(node.type)):
			var v: Vector3 = live[key]
			shown[key] = [v.x, v.y, v.z]
	if live.has("position") and (Document.is_control(str(node.type)) or Document.is_2d(str(node.type))):
		shown.position = [live.position.x, live.position.y]
	if Document.is_2d(str(node.type)):
		if live.has("rotation"): shown.rotation_degrees = live.rotation.z
		if live.has("scale"): shown.scale = [live.scale.x, live.scale.y]
		if live.has("frame"): shown.frame = live.frame
	if live.has("visible"): shown.visible = live.visible
	inspector.refresh_values(shown)

# ------------------------------------------------------------------ export, build, flash

func _scene_pack(path: String) -> PackedByteArray:
	var doc := _scene_doc_for(path)
	if doc.is_empty():
		_fail("Scene not found or invalid: " + path)
		return PackedByteArray()
	var others := _other_scenes(path)
	var missing := _missing_assets(doc)
	for other in others.values(): missing.append_array(_missing_assets(other))
	if not missing.is_empty():
		_fail("Missing or invalid resources:\n" + "\n".join(missing))
		return PackedByteArray()

	# The package starts with the "Made with Jet Studio" scene, which then loads the main scene.
	var start := path
	var splash := bool(project.config.get("boot_splash", true))
	if splash:
		others[path] = doc
		doc = _boot_splash_doc(path)
		start = BOOT_SPLASH

	var sources := _all_sources(doc, others)
	if splash: sources[BOOT_SPLASH_SCRIPT] = _boot_splash_script(path)
	var failed := false
	for script_path in sources:
		var info: Dictionary = bridge.compile_script(sources[script_path], script_path)
		for e in info.get("errors", []):
			_log_link("Compile error: " + str(e.message), script_path, int(e.line), Style.ERROR)
			_debug_error(str(e.message), script_path, int(e.line))
			failed = true
	if failed:
		bottom_tabs.current_tab = 1
		return PackedByteArray()
	var assets := _all_assets(doc, others)
	_pack_textures = assets.textures.keys()
	if splash: assets.textures[BOOT_SPLASH_LOGO] = {"image": _boot_splash_logo(), "palette": "Auto"}
	bridge.set_scenes(start, others)
	var error: String = bridge.load_scene(doc, sources, assets)
	var pack: PackedByteArray = bridge.export_pack() if error.is_empty() else PackedByteArray()
	if not error.is_empty(): _fail("Invalid scene: " + error)
	elif not pack.is_empty():
		# The STM32 firmware reads palette textures from the flash instead of copying them.
		_pack_memory = bridge.memory_estimate(Hardware.platform(Hardware.settings(project.config)) == "stm32")
	_upload()
	return pack

func _main_scene_or_current() -> String:
	var main: String = project.main_scene()
	if not main.is_empty() and project.exists(main): return main
	if current >= 0 and not _tab().path.is_empty(): return _tab().path
	return ""

func _export_to(path: String) -> bool:
	var scene := _main_scene_or_current()
	if scene.is_empty():
		_fail("Save the scene before exporting it.")
		return false
	script_editor.save_all()
	var pack := _scene_pack(scene)
	if pack.is_empty(): return false
	if path.get_extension() != "jetpack": path += ".jetpack"
	DirAccess.make_dir_recursive_absolute(path.get_base_dir())
	var file := FileAccess.open(path, FileAccess.WRITE)
	if file == null:
		_fail("Cannot export: " + path)
		return false
	file.store_buffer(pack)
	file.close()
	_log("Runtime package: %s · %d bytes (%s)" % [path, pack.size(), scene], Style.ACCENT)
	return true

## device runs the emulator: the player built with the firmware's Jet config (half-width fields, no z-buffer).
func _external_player(device: bool) -> void:
	var executable := _root_dir().path_join("build/jet_player_device" if device else "build/jet_player")
	if not FileAccess.file_exists(executable):
		_fail("%s missing. Select the PC target and press Build." % ("Device emulator" if device else "Desktop player"))
		return
	var pack := _root_dir().path_join(".cache/jobs/player.jetpack")
	if not _export_to(pack): return
	if OS.create_process(executable, PackedStringArray([pack])) < 0: _fail("Failed to start the player.")
	elif device: _log("Device emulator started: shows the firmware rendering (half-width fields, sorting without z-buffer) · P: pause · Esc: quit", Style.ACCENT)
	else: _log("External C++ player started · P: pause · Esc: quit", Style.ACCENT)

func _start_task(action: String) -> void:
	if _job_id > 0 or playing: return
	var folder := _root_dir().path_join(".cache/jobs")
	DirAccess.make_dir_recursive_absolute(folder)
	_job_prefix = folder.path_join(str(Time.get_ticks_msec()))
	if not _export_to(_job_prefix + ".jetpack"): return
	_job_offset = 0
	var target := "device" if target_select.selected > 0 else "desktop"
	if target == "device" and not _fits_board(_job_prefix + ".jetpack", action): return
	if target == "device": _report_memory()
	var profile := _profile()
	# The uploader needs the USB port; the Serial monitor gets it back after the flash.
	if action == "flash": _serial_was_open = serial_monitor.suspend()
	_job_action = action
	_job_id = OS.create_process("python3", PackedStringArray([_root_dir().path_join("tools/tasks.py"), action, "--target", target,
		"--profile", profile, "--pack", _job_prefix + ".jetpack", "--project", project.root, "--job", _job_prefix]))
	if _job_id < 0:
		_fail("Cannot start Python for the build.")
		_job_finished(false)
		return
	_log("%s · %s%s · process %d" % [action.to_upper(), target, (" · " + profile) if target == "device" else "", _job_id], Style.ACCENT)
	bottom_tabs.current_tab = 0
	_update_controls()

func _poll_task() -> void:
	var log_path := _job_prefix + ".log"
	if FileAccess.file_exists(log_path):
		var file := FileAccess.open(log_path, FileAccess.READ)
		file.seek(_job_offset)
		var chunk := file.get_buffer(file.get_length() - _job_offset).get_string_from_utf8()
		_job_offset = file.get_position()
		if not chunk.is_empty(): _log(chunk.trim_suffix("\n"))
	if FileAccess.file_exists(_job_prefix + ".json"):
		var result: Variant = JSON.parse_string(FileAccess.get_file_as_string(_job_prefix + ".json"))
		var ok: bool = result is Dictionary and int(result.get("returncode", -1)) == 0
		if ok: _log("Task completed.", Style.SUCCESS)
		else: _log("Task failed: check the output above.", Style.ERROR)
		_job_id = -1
		_job_finished(ok)
		_update_controls()
	elif not OS.is_process_running(_job_id):
		_log("Process ended without a result: check the log.", Style.ERROR)
		_job_id = -1
		_job_finished(false)
		_update_controls()

## Backtrace addresses -> [[function, file:line], ...] via addr2line on the Debug ELF. Only ESP32 prints backtraces.
func _decode_firmware_addresses(addresses: PackedStringArray) -> Array:
	var hw := Hardware.settings(project.config) if project != null else {}
	if hw.is_empty() or Hardware.platform(hw) != "esp32": return []
	var elf := _root_dir().path_join("firmware/.pio/build/%s/firmware.elf" % hw.board)
	var tool := ""
	var packages := OS.get_environment("HOME").path_join(".platformio/packages")
	for toolchain in ["toolchain-xtensa-esp-elf", "toolchain-xtensa-esp32s3"]:
		var candidate := packages.path_join(toolchain).path_join("bin/xtensa-esp32s3-elf-addr2line")
		if FileAccess.file_exists(candidate):
			tool = candidate
			break
	if tool.is_empty() or not FileAccess.file_exists(elf): return []
	var output := []
	if OS.execute(tool, PackedStringArray(["-pfiC", "-e", elf]) + addresses, output, true) != 0 or output.is_empty(): return []
	var frames := []
	var root := _root_dir() + "/"
	var framework := RegEx.create_from_string("\\S*/framework-espidf[^/]*/")
	for line in str(output[0]).split("\n", false):
		var inlined := line.strip_edges().begins_with("(inlined by)")
		var text := line.strip_edges().trim_prefix("(inlined by) ")
		var at := text.rfind(" at ")
		var function := _short_function(text.substr(0, at) if at > 0 else text)
		var location := text.substr(at + 4).replace(root, "") if at > 0 else ""
		location = framework.sub(location, "esp-idf/")
		frames.append([("   inline " if inlined else "") + function, location if location != "??:0" and location != "??:?" else "?"])
	return frames

## "ns::f<int>(Args const&) const" -> "ns::f() const"
static func _short_function(name: String) -> String:
	var out := ""
	var depth := 0
	for c in name.replace("(anonymous namespace)::", ""):
		if c == "(" or c == "<":
			if depth == 0: out += c
			depth += 1
		elif c == ")" or c == ">":
			depth = maxi(depth - 1, 0)
			if depth == 0: out += c
		elif depth == 0:
			out += c
	return out.replace("<>", "").replace("()()", "()")

func _profile() -> String:
	return "release" if target_select.selected == 2 else "debug"

## The Serial monitor reconnects after a flash; with a Debug firmware it also opens to show the boot log.
func _job_finished(ok: bool) -> void:
	if _job_action != "flash": return
	var show := ok and _profile() == "debug"
	serial_monitor.resume(_serial_was_open or show)
	if show: bottom_tabs.current_tab = 2
	elif ok: _log("Release firmware: no log on the serial port.", Style.MUTED)

# ------------------------------------------------------------------ shortcuts and quit

func _input(event: InputEvent) -> void:
	if _is_game_key(event): get_viewport().set_input_as_handled()

func _is_game_key(event: InputEvent) -> bool:
	if not playing or screen != Screen.GAME or not event is InputEventKey: return false
	var focus := get_viewport().gui_get_focus_owner()
	if focus is LineEdit or focus is TextEdit: return false
	for keys in _action_keys:
		if event.keycode in keys: return true
	return false

func _shortcut_input(event: InputEvent) -> void:
	if not event is InputEventKey or not event.pressed or event.echo: return
	var key: int = event.keycode
	var ctrl: bool = event.ctrl_pressed or event.meta_pressed
	var handled := true
	if key == KEY_F5 and not ctrl: _play(false)
	elif key == KEY_F6 and not ctrl: _play(true)
	elif key == KEY_F7 and not ctrl: _toggle_pause()
	elif key == KEY_F8 and not ctrl: _stop()
	elif ctrl and key == KEY_F1: _set_screen(Screen.VIEW_2D)
	elif ctrl and key == KEY_F2: _set_screen(Screen.VIEW_3D)
	elif ctrl and key == KEY_F3: _set_screen(Screen.SCRIPT)
	elif ctrl and key == KEY_F4: _set_screen(Screen.GAME)
	elif ctrl and event.shift_pressed and event.alt_pressed and key == KEY_S: _save_all()
	elif ctrl and event.shift_pressed and key == KEY_S: _save_scene_as()
	elif ctrl and key == KEY_S:
		if screen == Screen.SCRIPT and not script_editor.current_path().is_empty(): script_editor.save()
		else: _save_scene()
	elif ctrl and key == KEY_N: _new_scene()
	elif ctrl and key == KEY_O: _quick_open("scene")
	elif ctrl and key == KEY_Q: _request_quit()
	elif ctrl and event.shift_pressed and key == KEY_W: _close_tab(current)
	elif screen == Screen.SCRIPT and ctrl and key == KEY_W: script_editor.close_script(script_editor.current_path())
	elif screen == Screen.SCRIPT and ctrl and key == KEY_F: script_editor.show_find()
	elif screen == Screen.SCRIPT and ctrl and key == KEY_L: script_editor._on_search_menu(3)
	elif screen == Screen.SCRIPT and ctrl and key == KEY_K: script_editor.toggle_comment()
	else: handled = false
	if handled: get_viewport().set_input_as_handled()

func _unhandled_key_input(event: InputEvent) -> void:
	if not event is InputEventKey or not event.pressed: return
	var ctrl: bool = event.ctrl_pressed or event.meta_pressed
	if ctrl and event.keycode == KEY_Z:
		if event.shift_pressed: _redo()
		else: _undo()
	elif ctrl and event.keycode == KEY_Y: _redo()
	elif ctrl and event.keycode == KEY_A and screen in [Screen.VIEW_2D, Screen.VIEW_3D] and current >= 0:
		_open_create("add", _tab().selected)
	else: return
	get_viewport().set_input_as_handled()

func _notification(what: int) -> void:
	if what == NOTIFICATION_WM_CLOSE_REQUEST: _request_quit()
	elif what == NOTIFICATION_APPLICATION_FOCUS_IN and script_editor != null and not project.root.is_empty():
		script_editor.check_external_changes()
		_script_cache.clear()
		if current >= 0 and not playing:
			_upload()
			_refresh_inspector()

func _request_quit() -> void:
	var unsaved := script_editor.has_unsaved()
	for tab in tabs:
		if _dirty(tab): unsaved = true
	if unsaved:
		quit_dialog.dialog_text = "Some scenes or scripts have unsaved changes.\nSave before quitting?"
		quit_dialog.popup_centered()
	else:
		_quit()

func _quit() -> void:
	if playing: _stop()
	_save_state()
	get_tree().quit()

func _capture_and_quit(path: String) -> void:
	# Test-only arguments that set up the state to capture.
	for arg in OS.get_cmdline_user_args():
		if arg.begins_with("--select="): _select(int(arg.trim_prefix("--select=")))
		elif arg.begins_with("--open-script="): # path, or path:line
			var target := arg.trim_prefix("--open-script=")
			var line := target.get_slice(":", target.get_slice_count(":") - 1)
			if line.is_valid_int(): _open_script_at(target.trim_suffix(":" + line), int(line))
			else: _open_script_at(target, 0)
		elif arg.begins_with("--inspect-file="): _file_selected(arg.trim_prefix("--inspect-file="))
		elif arg.begins_with("--bottom-tab="): bottom_tabs.current_tab = int(arg.trim_prefix("--bottom-tab="))
		elif arg.begins_with("--right-tab="): right_tabs.current_tab = int(arg.trim_prefix("--right-tab="))
		elif arg.begins_with("--dialog=connect:"): _connect_requested(arg.trim_prefix("--dialog=connect:"), [])
		elif arg == "--serial-sample":
			for line in ["I (412) JetStudio: Scene loaded: 104 nodes, 92 meshes, 1 scripts; 320x240 RGB565; buffers 307200 bytes",
					"I (2911) JetStudio: Frame 38.20 ms (scripts 1.10 ms); 362 raster tris; free heap 241388, PSRAM 7612044",
					"\u001b[0;33mW (3100) JetStudio: texture res://car/outrun.png: non power of two\u001b[0m",
					"\u001b[0;31mE (3150) JetStudio: res://scripts/orbit.jet:12: Invalid operands \"Vector3\" and \"int\" for \"+\" operator.\u001b[0m",
					"I (5390) JetStudio: Frame 39.00 ms (scripts 1.20 ms); 360 raster tris; free heap 241200, PSRAM 7612044",
					"\u001b[0;31mE (30763) task_wdt: Task watchdog got triggered.\u001b[0m",
					"Backtrace: 0x4038085A:0x3FC9ED40 0x40381DAD:0x3FC9ED60 0x4037DE71:0x3FCB93E0 0x4037FF32:0x3FCB96A0 0x4203671B:0x3FCB96E0 0x42037278:0x3FCB9840 0x42005F05:0x3FCB9860 0x42001B71:0x3FCB9880"]:
				serial_monitor.feed((line + "\r\n").to_utf8_buffer())
		elif arg.begins_with("--screen="): _set_screen(int(arg.trim_prefix("--screen=")))
		elif arg.begins_with("--tool="): viewport._set_tool(int(arg.trim_prefix("--tool=")))
		elif arg == "--play": _play(false)
		elif arg == "--dialog=create": _open_create("add", _tab().selected)
		elif arg == "--dialog=new-scene": _new_scene()
		elif arg == "--dialog=change-type":
			var node := Document.find(_root_node(), _tab().selected)
			_open_create("change", int(node.id), "Change Type of %s" % node.name, "Change", str(node.type))
		elif arg == "--dialog=attach": _script_action_for(_tab().selected, "attach")
		elif arg == "--dialog=projects": _show_project_manager()
		elif arg == "--dialog=help": help_dialog.popup_centered(Vector2i(640, 520))
		elif arg == "--dialog=settings": _show_project_settings()
		elif arg == "--dialog=hardware": project_settings.open(project, 1)
		elif arg == "--dialog=input": project_settings.open(project, 2)
		elif arg == "--dialog=autoload": project_settings.open(project, 3)
	for i in 10: await get_tree().process_frame
	await _capture(path)
	get_tree().quit()

func _capture(path: String) -> void:
	await RenderingServer.frame_post_draw
	await RenderingServer.frame_post_draw
	var error := get_viewport().get_texture().get_image().save_png(path)
	if error != OK: push_error("Screenshot not saved: " + str(error))

# ------------------------------------------------------------------ smoke test

var _smoke_failed := false

func _check(condition: bool, message: String) -> void:
	if OS.has_environment("JET_SMOKE_VERBOSE"): print("  check: ", message, " -> ", condition)
	if condition: return
	_smoke_failed = true
	printerr("SMOKE FAIL: " + message)

func _frame() -> PackedByteArray:
	_render()
	return bridge.frame_bytes()

func _remove_tree(folder: String) -> void:
	var dir := DirAccess.open(folder)
	if dir == null: return
	for sub in dir.get_directories(): _remove_tree(folder.path_join(sub))
	for file in dir.get_files(): DirAccess.remove_absolute(folder.path_join(file))
	DirAccess.remove_absolute(folder)

## End-to-end check: Godot UI -> GDExtension -> runtime -> Jet.
func _run_smoke() -> void:
	var folder := _root_dir().path_join(".cache/tests/project")
	_remove_tree(folder)
	_create_project(folder, "Smoke")
	_check(project.root == folder and tabs.size() == 1, "new project opens its main scene")
	var root := _root_node()
	_check(Document.count(root) == 6, "starter scene has 6 nodes")
	var blank := _frame()
	_check(blank.size() == 320 * 240 * 3, "frame size")
	# Add / undo / redo
	_add_node("Node3D", int(root.id))
	_check(Document.count(_root_node()) == 7, "add node")
	_undo()
	_check(Document.count(_root_node()) == 6, "undo add")
	_redo()
	_check(Document.count(_root_node()) == 7, "redo add")
	_undo()
	# Inspector edit and undo
	_select(6)
	var before := _frame()
	_property_edited("position", [1.5, 0.5, 0.0], true)
	_check(_frame() != before, "inspector edit changes the frame")
	_undo()
	_check(_frame() == before, "undo restores the frame")
	# Gizmo drag
	var steps: int = _tab().undo.size()
	var t := Document.local_transform(Document.find(_root_node(), 6))
	_transform_edited(6, t.translated(Vector3(0.2, 0, 0)), false)
	_transform_edited(6, t.translated(Vector3(0.4, 0, 0)), false)
	_transform_edited(6, t.translated(Vector3(0.6, 0, 0)), true)
	_check(_tab().undo.size() == steps + 1, "gizmo drag merges into one undo step")
	_check(is_equal_approx(float(Document.find(_root_node(), 6).position[0]), 0.6), "gizmo drag applies the last value")
	_undo()
	# Reparenting
	_add_node("Node3D", int(_root_node().id))
	var pivot_id := Document.next_id(_root_node()) - 1
	_change("", true, func():
		var pivot := Document.find(_root_node(), pivot_id)
		pivot.position = [2.0, 0.0, 1.0]
		pivot.rotation = [0.0, 45.0, 0.0])
	var global_before := Document.global_transform(_root_node(), 6)
	_move_node(6, pivot_id, -1)
	_check(int(Document.parent_of(_root_node(), 6).id) == pivot_id, "cube reparented under the pivot")
	var global_after := Document.global_transform(_root_node(), 6)
	_check(global_after.origin.is_equal_approx(global_before.origin), "reparent keeps the global position")
	for i in 3: _undo()
	_check(Document.count(_root_node()) == 6 and int(Document.parent_of(_root_node(), 6).id) == int(_root_node().id), "undo reparent, edit and add")
	# Scene dock
	_rename_node(6, "Ground")
	_check(str(Document.find(_root_node(), 6).name) == "Ground2", "rename keeps sibling names unique")
	_rename_node(6, "bad/name")
	_check(str(Document.find(_root_node(), 6).name) == "Ground2", "invalid names are rejected")
	error_dialog.hide()
	_duplicate_node(6)
	_check(Document.count(_root_node()) == 7 and Document.find(_root_node(), _tab().selected).name == "Ground3", "duplicate with unique name")
	_undo()
	_undo()
	_toggle_visibility(6)
	_check(not Document.find(_root_node(), 6).visible, "visibility toggle")
	_undo()
	var order_before: int = Document.children(_root_node()).find(Document.find(_root_node(), 6))
	_reorder_node(6, -1)
	_check(Document.children(_root_node()).find(Document.find(_root_node(), 6)) == order_before - 1, "move up among siblings")
	_undo()
	_check(str(Document.find(_root_node(), 6).name) == "Cube", "undo chain restores the original")
	# Scripts and exported variables
	var script_path := "res://scripts/smoke.jet"
	project.write(script_path, "extends MeshInstance3D\n@export var speed := 90.0\nvar frames := 0\n\nfunc _ready():\n\tprint(\"smoke ready \", name)\n\nfunc _process(delta):\n\tframes += 1\n\trotation_degrees.y += speed * delta\n")
	_attach_script(6, script_path)
	_check(str(Document.find(_root_node(), 6).get("script", "")) == script_path, "script attached")
	var info := _script_info(script_path)
	_check(info.get("ok", false) and info.exports.size() == 1 and info.exports[0].name == "speed", "exported variable visible to the Inspector")
	_select(6)
	_script_property_edited("speed", 120.0, true)
	_check(float(Document.find(_root_node(), 6).script_properties.speed) == 120.0, "script property override")
	var bad := bridge.compile_script("extends Node3D\nfunc _process(delta):\n\tposition.x += nope\n", "res://bad.jet") as Dictionary
	_check(not bad.ok and int(bad.errors[0].line) == 3, "compile errors carry line numbers")
	# Script file rename
	DirAccess.rename_absolute(project.to_abs(script_path), project.to_abs("res://scripts/renamed.jet"))
	_file_moved(script_path, "res://scripts/renamed.jet")
	_check(str(Document.find(_root_node(), 6).script) == "res://scripts/renamed.jet", "file rename updates node references")
	DirAccess.rename_absolute(project.to_abs("res://scripts/renamed.jet"), project.to_abs(script_path))
	_file_moved("res://scripts/renamed.jet", script_path)
	# Play / stop
	var edit_frame := _frame()
	_play(true)
	_check(playing, "play starts")
	var arrow := InputEventKey.new()
	arrow.keycode = KEY_DOWN
	arrow.pressed = true
	var stop_key := InputEventKey.new()
	stop_key.keycode = KEY_F8
	stop_key.pressed = true
	_check(_is_game_key(arrow) and not _is_game_key(stop_key), "while playing, action keys are kept from the editor's controls (not F5-F8)")
	_check("smoke ready Cube" in output.get_parsed_text(), "print() appears in the Output panel")
	var first := _frame()
	for i in 12: _process(0.05)
	_check(_frame() != first, "scripts animate the game")
	_stop()
	_check(not playing and _frame() == edit_frame, "stop restores the edited scene")
	# Runtime errors
	project.write(script_path, "extends MeshInstance3D\nvar target = null\nfunc _process(delta):\n\ttarget.position.x = 1\n")
	_script_cache.clear()
	_play(true)
	for i in 3: _process(0.05)
	_check(paused and errors_list.item_count > 0, "runtime error pauses the game and lists the error")
	_check(errors_list.item_count > 0 and int(errors_list.get_item_metadata(0).line) == 4, "runtime error line")
	_stop()
	# Save, reopen, export
	_check(_save_scene(), "save scene")
	_open_project(folder)
	_check(Document.count(_root_node()) == 6 and str(Document.find(_root_node(), 6).get("script", "")) == script_path, "saved scene reopens with its script")
	project.write(script_path, "extends MeshInstance3D\n@export var speed := 90.0\nfunc _process(delta):\n\trotation_degrees.y += speed * delta\n")
	var pack_path := _root_dir().path_join(".cache/tests/smoke.jetpack")
	_check(_export_to(pack_path), "export .jetpack")
	var bytes := FileAccess.get_file_as_bytes(pack_path)
	_check(bytes.size() > 100 and bytes.slice(0, 5) == PackedByteArray([74, 69, 84, 80, 17]), "package header JETP v17")
	# 2D nodes
	_add_node("ColorRect", int(_root_node().id))
	var rect_id: int = _tab().selected
	var before_2d := _frame()
	_control_moved(rect_id, Vector2(30, 20), false)
	_control_moved(rect_id, Vector2(40, 30), true)
	_check(_frame() != before_2d and Document.find(_root_node(), rect_id).position == [40.0, 30.0], "ColorRect moved by dragging")
	_check(viewport._control_at(Vector2(45, 35)) == rect_id and bridge.pick(45, 35) == rect_id, "2D nodes are picked in the viewport")
	_add_node("Label", int(_root_node().id))
	var label_frame := _frame()
	_property_edited("text", "SCORE 10", true)
	_check(_frame() != label_frame, "Label text renders over the scene")
	var label_id: int = _tab().selected
	var label_item := scene_dock.tree.get_selected()
	_check(label_item != null and label_item.get_button_by_id(0, SceneDock.BUTTON_VISIBLE) >= 0, "Control nodes have the visibility toggle in the Scene tree")
	var shown_frame := _frame()
	_toggle_visibility(label_id)
	_check(_frame() != shown_frame and not Document.find(_root_node(), label_id).visible, "hiding a Label from the Scene tree removes it from the view")
	_toggle_visibility(label_id)
	_add_node("Control", int(_root_node().id))
	var group_id: int = _tab().selected
	_property_edited("position", [0.0, 120.0], true)
	_add_node("Label", group_id)
	_property_edited("text", "GROUPED", true)
	var grouped := _frame()
	_toggle_visibility(group_id)
	_check(_frame() != grouped, "a plain Control groups 2D nodes: hiding it hides its children")
	_toggle_visibility(group_id)
	# Physics
	_add_node("StaticBody3D", int(_root_node().id))
	var floor_id: int = _tab().selected
	_add_node("CollisionShape3D", floor_id)
	_property_edited("size", [10.0, 1.0, 10.0], true)
	_add_node("RigidBody3D", int(_root_node().id))
	var body_id: int = _tab().selected
	_property_edited("position", [0.0, 3.0, 0.0], true)
	_add_node("CollisionShape3D", body_id)
	_property_edited("shape", "Sphere", true)
	# smoke.jet was rewritten in the same second as the error test, so its mtime can't invalidate the caches.
	script_editor.close_all()
	_script_cache.clear()
	_play(true)
	for i in 40: _process(0.05)
	var fallen: Dictionary = bridge.node_state(body_id)
	_stop()
	_check(not fallen.is_empty() and absf(fallen.position.y - 1.0) < 0.05, "a RigidBody3D falls and rests on a StaticBody3D")
	_check(_save_scene(), "save scene with physics nodes")
	_open_project(folder)
	var floor_shape: Dictionary = Document.children(Document.find(_root_node(), floor_id))[0]
	_check(floor_shape.get("size") == [10.0, 1.0, 10.0], "CollisionShape3D size survives save and reopen")
	var lonely := {"id": 900, "name": "Ball", "type": "RigidBody3D", "children": [{"id": 901, "name": "Mesh", "type": "MeshInstance3D", "children": []}]}
	var stray := {"id": 902, "name": "Shape", "type": "CollisionShape3D", "children": []}
	var test_root := {"id": 899, "name": "R", "type": "Node3D", "children": [lonely, stray]}
	_check(Document.configuration_warnings(test_root, lonely).size() == 1 and Document.configuration_warnings(test_root, stray).size() == 1
		and Document.configuration_warnings(_root_node(), Document.find(_root_node(), body_id)).is_empty(),
		"configuration warnings: body without shape, shape without body")

	# 2D physics: a ball falls on a ground; shapes draw their outline
	_add_node("StaticBody2D", int(_root_node().id))
	var ground_id: int = _tab().selected
	_property_edited("position", [160.0, 200.0], true)
	_add_node("CollisionShape2D", ground_id)
	var ground_shape_id: int = _tab().selected
	_property_edited("size", [300.0, 20.0], true)
	_add_node("RigidBody2D", int(_root_node().id))
	var ball_id: int = _tab().selected
	_property_edited("position", [160.0, 100.0], true)
	_add_node("CollisionShape2D", ball_id)
	_property_edited("shape", "Circle", true)
	_frame()
	var outline: PackedVector2Array = bridge.shape_outline(ground_shape_id)
	_check(outline.size() == 4 and outline[0].is_equal_approx(Vector2(10, 190)) and outline[2].is_equal_approx(Vector2(310, 210)),
		"CollisionShape2D outline on the canvas")
	_play(true)
	for i in 40: _process(0.05)
	var ball: Dictionary = bridge.node_state(ball_id)
	_stop()
	_check(not ball.is_empty() and absf(ball.position.y - 180.0) < 1.5, "a RigidBody2D falls and rests on a StaticBody2D")
	_set_screen(Screen.VIEW_2D)
	viewport.canvas_zoom = 2.0
	viewport.canvas_origin = Vector2.ZERO
	_render()
	var outline_2d: PackedVector2Array = bridge.shape_outline(ground_shape_id)
	_check(viewport.mode_2d and outline_2d.size() == 4 and outline_2d[0].is_equal_approx(Vector2(20, 380)),
		"the 2D view shows the canvas at its own zoom")

	# 2D gizmos, driven as a drag from one view point to another
	var drag_2d := func(id: int, handle: Dictionary, from: Vector2, to: Vector2):
		viewport._begin_edit_2d(id, handle, from)
		viewport._update_edit_2d(to, true, false, false)
		viewport._nav = viewport.Nav.NONE
		viewport._drag = {}
	var handle_2d := func(kind: String, key: String, value: Variant) -> Dictionary:
		for h in viewport._handles_2d():
			if h.kind == kind and h.get(key) == value: return h
		return {}
	var ground_at := viewport.canvas_to_area(Vector2(160, 200))
	_select(ground_id)
	viewport._set_tool(viewport.Tool.ROTATE)
	drag_2d.call(ground_id, handle_2d.call("rotate", "kind", "rotate"), ground_at + Vector2(60, 0), ground_at + Vector2(0, 60))
	_check(is_equal_approx(float(Document.find(_root_node(), ground_id).rotation_degrees), 90.0), "the rotate gizmo turns a Node2D clockwise")
	_select(ground_shape_id)
	viewport._set_tool(viewport.Tool.MOVE)
	drag_2d.call(ground_shape_id, {"kind": "move", "axis": -1}, ground_at, viewport.canvas_to_area(Vector2(170, 200)))
	_check(Document.find(_root_node(), ground_shape_id).position == [0.0, -10.0], "moving a child of a rotated Node2D follows the cursor")
	_undo()
	_undo()
	_select(ground_id)
	viewport._set_tool(viewport.Tool.SCALE)
	var scale_x: Dictionary = handle_2d.call("scale", "axis", 0)
	drag_2d.call(ground_id, scale_x, scale_x.get("at", Vector2.ZERO), ground_at + scale_x.get("dir", Vector2.ZERO) * 120.0)
	_check(Document.find(_root_node(), ground_id).scale == [2.0, 1.0], "the scale gizmo's X handle scales one axis")
	_undo()
	_select(ground_shape_id)
	var right_edge: Dictionary = handle_2d.call("shape", "side", Vector2i(1, 0))
	drag_2d.call(ground_shape_id, right_edge, right_edge.get("at", Vector2.ZERO), viewport.canvas_to_area(Vector2(330, 200)))
	_check(Document.find(_root_node(), ground_shape_id).size == [340.0, 20.0], "CollisionShape2D handles resize the shape")
	_undo()
	_add_node("ColorRect", int(_root_node().id))
	var panel_id: int = _tab().selected
	viewport._set_tool(viewport.Tool.SELECT)
	var corner: Dictionary = handle_2d.call("resize", "side", Vector2i(1, 1))
	drag_2d.call(panel_id, corner, corner.get("at", Vector2.ZERO), viewport.canvas_to_area(Vector2(80, 72)))
	viewport._nudge_2d(KEY_RIGHT, false)
	var panel: Dictionary = Document.find(_root_node(), panel_id)
	_check(panel.size == [80.0, 72.0] and panel.position == [1.0, 0.0], "Control resize handles and arrow-key nudging")
	_delete_node(panel_id)
	viewport._set_tool(viewport.Tool.MOVE)
	_set_screen(Screen.VIEW_3D)
	var shape_type := str(Document.find(_root_node(), ground_id).type)
	_change_type(ground_id, "Area2D")
	var retyped: Dictionary = Document.find(_root_node(), ground_id)
	_check(str(retyped.type) == "Area2D" and retyped.has("monitoring") and not retyped.has("friction")
		and retyped.position == [160.0, 200.0] and Document.children(retyped).size() == 1, "Change Type keeps name, children and shared properties")
	_undo()
	_check(str(Document.find(_root_node(), ground_id).type) == shape_type, "Change Type can be undone")
	var lonely_2d := {"id": 903, "name": "Zone", "type": "Area2D", "children": []}
	var warnings_2d := Document.configuration_warnings(test_root, lonely_2d)
	_check(warnings_2d.size() == 1 and "CollisionShape2D" in warnings_2d[0]
		and Document.configuration_warnings(_root_node(), Document.find(_root_node(), ball_id)).is_empty(),
		"2D bodies want a CollisionShape2D")
	_delete_node(ball_id)
	_delete_node(ground_id)
	# New scenes ask for their root, as Godot's Scene dock does
	var tab_count := tabs.size()
	_new_scene()
	new_scene_dialog.hide()
	new_scene_dialog.root_chosen.emit("Node2D")
	_check(tabs.size() == tab_count + 1 and str(_root_node().type) == "Node2D" and screen == Screen.VIEW_2D, "a new 2D scene opens in the 2D view")
	_close_tab(current, true)
	_new_scene()
	new_scene_dialog.hide()
	new_scene_dialog.root_chosen.emit("Control")
	_check(str(_root_node().type) == "Control" and _root_node().size == [viewport.W, viewport.H], "a User Interface scene fills the screen")
	_close_tab(current, true)
	_set_screen(Screen.VIEW_3D)
	# Timer and signals
	_add_node("Timer", int(_root_node().id))
	var timer_id: int = _tab().selected
	_property_edited("wait_time", 0.1, true)
	_property_edited("autostart", true, true)
	var timer_script := "res://scripts/smoke_timer.jet"
	project.write(timer_script, "extends Timer\nsignal fired(count)\nvar count := 0\nfunc _ready():\n\ttimeout.connect(_on_timeout)\n"
		+ "\tfired.connect(_on_fired)\nfunc _on_timeout():\n\tcount += 1\n\tfired.emit(count)\nfunc _on_fired(n):\n\tif n == 2:\n\t\tprint(\"smoke timeout \", wait_time)\n")
	_attach_script(timer_id, timer_script)
	var timer_info := _script_info(timer_script)
	var timer_class: Array = bridge.class_db().filter(func(c): return c.name == "Timer")
	_check(timer_info.get("ok", false) and timer_info.signals.size() == 1 and timer_info.signals[0].name == "fired"
		and timer_class.size() == 1 and timer_class[0].signals[0].name == "timeout", "script and engine signals reach the completion")
	_play(true)
	for i in 6: _process(0.05)
	var timed_out := "smoke timeout 0.1" in output.get_parsed_text()
	_stop()
	_check(timed_out, "a Timer emits timeout, a script signal passes it on")
	_delete_node(timer_id)
	# Signals connected in the Signals dock: the Connect dialog's name, the stub added to the script, Play, undo, deletion
	_add_node("Timer", int(_root_node().id))
	var ticker_id: int = _tab().selected
	_rename_node(ticker_id, "Ticker")
	_property_edited("wait_time", 0.1, true)
	_property_edited("autostart", true, true)
	_add_node("Node", int(_root_node().id))
	var receiver_id: int = _tab().selected
	_rename_node(receiver_id, "Receiver")
	var receiver_script := "res://scripts/smoke_receiver.jet"
	project.write(receiver_script, "extends Node\n")
	_attach_script(receiver_id, receiver_script)
	_select(ticker_id)
	right_tabs.current_tab = 1
	_connect_requested("timeout", [])
	var receiver_item: TreeItem = connect_dialog._tree.get_root()
	while receiver_item != null and int(receiver_item.get_metadata(0)) != receiver_id: receiver_item = receiver_item.get_next_in_tree()
	if receiver_item != null: receiver_item.select(0)
	var proposed: String = connect_dialog._method.text
	connect_dialog.hide()
	_check(proposed == "_on_ticker_timeout" and not connect_dialog.get_ok_button().disabled, "the Connect dialog proposes Godot's method name")
	_add_connection("timeout", [], receiver_id, proposed, 0)
	var stub = script_editor.source_of(receiver_script)
	_check(Document.connections(_tab().doc).size() == 1 and stub != null and "func _on_ticker_timeout() -> void:" in stub
		and screen == Screen.SCRIPT, "connecting adds the method to the receiver's script and opens it")
	var dock_tree: Tree = signals_dock._tree
	var timeout_item: TreeItem = dock_tree.get_root().get_child(0).get_child(0) if dock_tree.get_root() != null else null
	_check(timeout_item != null and timeout_item.get_text(0) == "timeout()" and timeout_item.get_child_count() == 1
		and "Receiver :: _on_ticker_timeout()" in timeout_item.get_child(0).get_text(0), "the Signals dock lists the signal and its connection")
	script_editor._scripts[receiver_script].code.text = stub.replace("pass # Replace with function body.", "print(\"smoke connected\")")
	script_editor.save(receiver_script)
	_set_screen(Screen.VIEW_3D)
	_play(true)
	for i in 4: _process(0.05)
	var connected := "smoke connected" in output.get_parsed_text()
	_stop()
	_check(connected, "a connection made in the editor works in Play")
	var connections_pack := _root_dir().path_join(".cache/tests/connections.jetpack")
	var connections_bytes := FileAccess.get_file_as_bytes(connections_pack) if _export_to(connections_pack) else PackedByteArray()
	_check(connections_bytes.hex_encode().contains("_on_ticker_timeout".to_utf8_buffer().hex_encode()), "the package carries the connection")
	_undo()
	_check(Document.connections(_tab().doc).is_empty(), "undo removes the connection")
	_redo()
	_duplicate_node(ticker_id)
	_check(Document.connections(_tab().doc).size() == 2, "a duplicated node keeps its connections")
	_undo()
	_delete_node(receiver_id)
	_check(Document.connections(_tab().doc).is_empty(), "deleting the receiver drops its connections")
	_delete_node(ticker_id)
	right_tabs.current_tab = 0

	# AnimationPlayer: the Animation panel, the Inspector's key icons, the preview and Play
	_add_node("MeshInstance3D", int(_root_node().id))
	var mover_id: int = _tab().selected
	_rename_node(mover_id, "Mover")
	_add_node("AnimationPlayer", int(_root_node().id))
	var anim_player_id: int = _tab().selected
	_select(anim_player_id)
	_check(bottom_tabs.current_tab == 3 and animation_panel.player_id == anim_player_id, "selecting an AnimationPlayer opens the Animation panel")
	animation_panel._on_edit_menu(0)
	_check(Document.find(_root_node(), anim_player_id).get("animations", []).size() == 1 and animation_panel.animation == "new_animation",
		"Animation > New adds an animation")

	_select(mover_id)
	_check(inspector.keyable.call(Document.find(_root_node(), mover_id), "position"), "the Inspector shows key icons while an animation is edited")
	_key_property("position")
	animation_panel._set_time(1.0)
	_property_edited("position", [2.0, 0.0, 0.0], true)
	_key_property("position")
	var position_track: Dictionary = Document.find(_root_node(), anim_player_id).animations[0].tracks[0]
	_check(position_track.path == "Mover" and position_track.property == "position" and position_track.keys.size() == 2
		and position_track.keys[1].value == [2.0, 0.0, 0.0], "key icons add a track and its keys")
	animation_panel._set_time(0.5)
	var posed: Dictionary = bridge.node_state(mover_id)
	_check(not posed.is_empty() and absf(posed.position.x - 1.0) < 0.01 and Document.find(_root_node(), mover_id).position == [2.0, 0.0, 0.0],
		"moving the time previews the pose without changing the scene")

	animation_panel._menu_target = {"method": false}
	animation_panel._show_node_menu()
	var mover_index := -1
	for i in animation_panel._node_menu.item_count:
		if int(animation_panel._node_menu.get_item_metadata(i)) == mover_id: mover_index = i
	animation_panel._node_menu.hide()
	animation_panel._on_node_chosen(mover_index)
	animation_panel._property_menu.hide()
	var choices: Array = animation_panel._menu_target.get("choices", [])
	_check(choices.any(func(c): return c.property == "rotation_degrees") and not choices.any(func(c): return c.property == "mesh"),
		"Add Track offers the properties the runtime can animate")
	animation_panel._menu_target = {"method": true}
	animation_panel._on_node_chosen(mover_index)
	animation_panel._insert_at(1, 0.5)
	animation_panel._selected = Vector2i(1, 0)
	animation_panel.refresh()
	animation_panel._set_key_field(1, 0, "method", "hit")
	animation_panel._set_key_field(1, 0, "args", [7])
	var call_key: Dictionary = Document.find(_root_node(), anim_player_id).animations[0].tracks[1].keys[0]
	_check(call_key.method == "hit" and call_key.args == [7] and animation_panel._key_box.get_child_count() > 0, "a Call Method track with a key and its arguments")

	animation_panel._autoplay.button_pressed = true
	animation_panel._toggle_autoplay()
	var mover_script := "res://scripts/smoke_mover.jet"
	project.write(mover_script, "extends MeshInstance3D\nfunc hit(n):\n\tprint(\"smoke hit \", n)\n")
	_attach_script(mover_id, mover_script)
	_play(true)
	for i in 5: _process(0.1)
	var halfway: Dictionary = bridge.node_state(mover_id)
	for i in 5: _process(0.1)
	var hit := "smoke hit 7" in output.get_parsed_text()
	_stop()
	_check(hit and not halfway.is_empty() and absf(halfway.position.x - 1.0) < 0.25, "an autoplay animation moves the node and calls its method in Play")
	var anim_pack := _root_dir().path_join(".cache/tests/animation.jetpack")
	_check(_export_to(anim_pack), "export with an AnimationPlayer")

	_select(anim_player_id)
	var invalid_before := output.get_parsed_text().count("Invalid scene")
	animation_panel._length.value = 0.25
	_check(Document.find(_root_node(), anim_player_id).animations[0].length == 0.25 and output.get_parsed_text().count("Invalid scene") == invalid_before,
		"a shorter length keeps the scene valid (later keys are left out)")
	_undo()
	_delete_node(anim_player_id)
	_delete_node(mover_id)
	bottom_tabs.current_tab = 0
	_check(animation_panel.player_id == 0, "deleting the AnimationPlayer clears the panel")

	# Particles
	_add_node("CPUParticles3D", int(_root_node().id))
	_property_edited("position", [0.0, 1.5, 0.0], true)
	_property_edited("direction", [0.0, 1.0, 0.0], true)
	_property_edited("initial_velocity_max", 2.0, true)
	_property_edited("amount", 40, true)
	_play(true)
	for i in 20: _process(0.05)
	var live_particles := int(bridge.statistics().get("particles", 0))
	_stop()
	_check(live_particles > 10, "CPUParticles3D emits particles while playing")
	# OBJ import
	var cube := "mtllib cube.mtl\nv -0.5 -0.5 0.5\nv 0.5 -0.5 0.5\nv 0.5 0.5 0.5\nv -0.5 0.5 0.5\nv -0.5 -0.5 -0.5\nv 0.5 -0.5 -0.5\nv 0.5 0.5 -0.5\nv -0.5 0.5 -0.5\nvt 0 0\nvt 1 0\nvt 1 1\nvt 0 1\n" \
		+ "o Body\nusemtl Paint\nf 1/1 2/2 3/3 4/4\nf 6/1 5/2 8/3 7/4\nf 2/1 6/2 7/3 3/4\nf 5/1 1/2 4/3 8/4\no Lid\nusemtl Decal\nf 4/1 3/2 7/3 8/4\nf 5/1 6/2 2/3 1/4\n"
	project.write("res://models/cube.obj", cube)
	project.write("res://models/cube.mtl", "newmtl Paint\nKd 0.2 0.8 0.4\nnewmtl Decal\nKd 1 1 1\nmap_Kd decal.png\n")
	var decal := Image.create(64, 64, false, Image.FORMAT_RGB8)
	for y in 64:
		for x in 64: decal.set_pixel(x, y, Color(x / 64.0, y / 64.0, 0.5))
	decal.save_png(project.to_abs("res://models/decal.png"))
	var model_frame := _frame()
	var count_before := Document.count(_root_node())
	_instantiate_model(int(_root_node().id), "res://models/cube.obj", Vector3(1, 0, 0))
	var group := Document.find(_root_node(), _tab().selected)
	_check(Document.count(_root_node()) == count_before + 3 and str(group.name) == "cube" and Document.children(group).size() == 2,
		"OBJ dropped in the scene creates a node per part")
	var lid: Dictionary = Document.children(group)[1] if Document.children(group).size() == 2 else {}
	_check(str(lid.get("mesh_part", "")) == "Lid" and str(lid.get("albedo_texture", "")) == "res://models/decal.png"
		and int(Document.children(group)[0].color) == 0x33cc66, "parts take the MTL colour and texture")
	_check(_frame() != model_frame and _missing_assets(_tab().doc).is_empty(), "imported model renders")
	var small := _mesh_bounds(lid)
	_import_applied("res://models/cube.obj", {"scale": 2.0, "mirror_x": false, "flip_faces": false, "flip_v": false})
	var large := _mesh_bounds(lid)
	_check(small.size() == 2 and large.size() == 2 and is_equal_approx(large[1].x, 2.0 * small[1].x), "import settings rescale the model")
	_import_applied("res://models/decal.png", {"max_size": "32", "palette": "Quantize", "flip_v": false})
	_check(_texture_size("res://models/decal.png") == Vector2i(32, 32), "texture import reduces the size")
	_check(FileAccess.file_exists(project.to_abs("res://models/decal.png.import")), "import settings saved next to the file")
	var model_pack := _root_dir().path_join(".cache/tests/smoke_model.jetpack")
	_check(_export_to(model_pack) and FileAccess.get_file_as_bytes(model_pack).size() > bytes.size() + 1024, "export ships models and textures")
	_change("", true, func(): Document.children(Document.find(_root_node(), int(group.id)))[1].mesh_part = "Nope")
	_check(not _missing_assets(_tab().doc).is_empty(), "missing model parts block the export")
	_undo()
	# Texture budget for the board
	var tile := Image.create(32, 32, false, Image.FORMAT_RGB8)
	for y in 32:
		for x in 32: tile.set_pixel(x, y, Color(x / 31.0, y / 31.0, ((x * y) % 32) / 31.0))
	tile.resize(512, 512, Image.INTERPOLATE_NEAREST)
	tile.save_png(project.to_abs("res://models/heavy.png"))
	_file_dropped(int(lid.id), "res://models/heavy.png")
	_check(compress_dialog.visible and _compression.has("res://models/heavy.png"), "a heavy texture offers compression for the board")
	compress_dialog.hide()
	_apply_compression()
	var light := _texture_cost("res://models/heavy.png", _import_settings("res://models/heavy.png"))
	_check(light.get("size") == Vector2i(256, 256) and light.get("indexed", false), "compression imports it at 256 px with 256 colors")
	tile.resize(1024, 1024, Image.INTERPOLATE_NEAREST)
	tile.save_png(project.to_abs("res://models/huge.png"))
	_kept_textures["res://models/huge.png"] = true
	_file_dropped(int(lid.id), "res://models/huge.png")
	var big_pack := _root_dir().path_join(".cache/tests/smoke_big.jetpack")
	_check(_export_to(big_pack) and not _fits_board(big_pack, "build") and compress_dialog.visible and _compression.has("res://models/huge.png"),
		"a package over 1 MiB stops the board build and offers compression")
	compress_dialog.hide()
	_after_compression = Callable()
	_apply_compression()
	_check(_export_to(big_pack) and _fits_board(big_pack, "build"), "after compression the package fits the board")
	_undo()
	_undo()
	# Sprite3D
	var before_sprite := _frame()
	var sprite_id := Document.next_id(_root_node())
	_change("", true, func():
		var sprite := Document.new_node("Sprite3D", sprite_id, "Sign")
		sprite.texture = "res://models/decal.png"
		sprite.position = [0.0, 1.5, 0.0]
		sprite.pixel_size = 0.05
		Document.children(_root_node()).append(sprite))
	_check(_frame() != before_sprite and _missing_assets(_tab().doc).is_empty()
		and _mesh_bounds(Document.find(_root_node(), sprite_id)).size() == 2, "Sprite3D draws its texture and has selection bounds")
	_undo()
	# 2D nodes
	var before_2d_sprite := _frame()
	_add_node("Node2D", int(_root_node().id))
	var holder_id: int = _tab().selected
	_property_edited("position", [200.0, 150.0], true)
	_add_node("Sprite2D", holder_id)
	var ship_id: int = _tab().selected
	_property_edited("texture", "res://models/decal.png", true)
	_property_edited("position", [20.0, 20.0], true)
	var ship_item := scene_dock.tree.get_selected()
	_check(ship_item != null and ship_item.get_button_by_id(0, SceneDock.BUTTON_VISIBLE) >= 0, "2D nodes have the visibility toggle")
	var drawn_frame := _frame()
	var drawn: Rect2i = bridge.drawn_rect(ship_id)
	_check(drawn_frame != before_2d_sprite and drawn.has_area() and drawn.get_center().distance_to(Vector2(220, 170)) < 2
		and viewport._control_at(Vector2(220, 170)) == ship_id, "Sprite2D draws under its Node2D parent and is picked in the viewport")
	viewport._begin_move_2d(ship_id, viewport._image_rect().position)
	viewport._update_move_2d(viewport._image_rect().position + Vector2(10, 0) * viewport._image_rect().size.x / viewport.W, true)
	viewport._nav = viewport.Nav.NONE # as when the button is released
	viewport._drag = {}
	_check(Document.find(_root_node(), ship_id).position[0] > 20.0, "dragging moves a Sprite2D")
	_delete_node(holder_id)
	# Hardware
	_check(Hardware.boards.has("nano_esp32") and Hardware.displays.has("st7796_320x480"), "hardware profiles are loaded")
	project.config.hardware = {"board": "esp32s3_devkitc_n8r8", "display": "st7796_320x480", "orientation": "landscape"}
	_apply_hardware()
	_check(_frame().size() == 480 * 320 * 3 and viewport.W == 480.0 and target_select.get_item_text(1).begins_with("S3 N8R8"),
		"a 480x320 display sets the resolution and the board name")
	var wide_pack := _root_dir().path_join(".cache/tests/smoke_wide.jetpack")
	var wide := FileAccess.get_file_as_bytes(wide_pack) if _export_to(wide_pack) else PackedByteArray()
	_check(wide.size() > 16 and wide.decode_u16(12 + wide.decode_u32(8)) == 480 and wide.decode_u16(14 + wide.decode_u32(8)) == 320,
		"the package carries the resolution")
	project.config.hardware.orientation = "portrait"
	_apply_hardware()
	_check(_frame().size() == 320 * 480 * 3, "portrait orientation swaps the resolution")
	var conflict := Hardware.settings({"hardware": {"board": "nano_esp32", "pins": {"cs": 9}}})
	_check(not Hardware.problems(conflict).is_empty() and Hardware.problems(Hardware.settings({})).is_empty(), "pin conflicts are reported")
	# Input Map
	project_settings.open(project, 2)
	project_settings._new_action.text = "jump"
	project_settings._add_action()
	project_settings._actions[6].keys = ["J"]
	project_settings._actions[6].pin = 5
	project_settings._actions[0].pin = 6
	project_settings._check()
	var settings_ok: bool = not project_settings.get_ok_button().disabled
	project_settings._apply()
	project_settings.hide()
	var saved: Array = project.config.get("input_map", [])
	_check(settings_ok and saved.size() == 7 and saved[6].name == "jump" and saved[6].keys == ["J"] and int(saved[0].pin) == 6
		and not project.config.has("input_pins"), "Input Map: actions added in Project Settings with keys and pins")
	_check(_action_keys.size() == 7 and KEY_J in _action_keys[6], "the game view reads the Input Map keys")
	var input_pack := _root_dir().path_join(".cache/tests/input.jetpack")
	var input_bytes := FileAccess.get_file_as_bytes(input_pack) if _export_to(input_pack) else PackedByteArray()
	_check(input_bytes.size() > 16 and input_bytes.decode_u8(16 + input_bytes.decode_u32(8)) == 7, "the package carries the Input Map")
	project.config.erase("input_map")
	_apply_hardware()
	# STM32 boards
	var stm32 := Hardware.settings({"hardware": {"board": "nucleo_f767zi"}})
	var wrong_sck := Hardware.settings({"hardware": {"board": "nucleo_f767zi", "pins": {"sck": 0x5F}}}) # D2 · PF15, no SPI
	var stm32_ok := true
	for id in Hardware.boards:
		if Hardware.platform({"board": id}) == "stm32": stm32_ok = stm32_ok and Hardware.problems(Hardware.settings({"hardware": {"board": id}})).is_empty()
	_check(Hardware.platform(stm32) == "stm32" and Hardware.pin_label(stm32, 0x05) == "D13 · PA5" and stm32_ok
		and Hardware.problems(wrong_sck).is_empty() and Hardware.software_spi(wrong_sck) and not Hardware.software_spi(stm32),
		"STM32 boards: pin labels, SPI pins and software SPI elsewhere")
	var f746 := Hardware.settings({"hardware": {"board": "nucleo_f746zg", "pins": {"mosi": 7}}}) # D11, once JP6 is off
	var active_high: Array = Hardware.boards["nucleo_f746zg"].get("active_high", []).map(func(pin): return int(pin))
	_check(Hardware.pin_label(f746, 45) == "B1 USER · PC13" and active_high == [45]
		and not Hardware.software_spi(f746) and Hardware.problems(f746).is_empty(),
		"Nucleo-144: the B1 USER button (pressed = high) and MOSI on D11 through the SPI peripheral")

	# The RAM estimate logged before a board build: the F746's heap, the Nano's PSRAM.
	var memory_text := func(hardware: Dictionary) -> String:
		project.config["hardware"] = hardware
		_apply_hardware()
		if not _export_to(input_pack): return ""
		var logged := output.get_parsed_text().length()
		_report_memory()
		return output.get_parsed_text().substr(logged)
	var stm32_memory: String = memory_text.call({"board": "nucleo_f746zg", "pins": {"mosi": 7}})
	var psram_memory: String = memory_text.call({"board": "nano_esp32"})
	_check(stm32_memory.begins_with("RAM (estimate): about ") and stm32_memory.contains(" of 288 KB")
		and psram_memory.contains(" in PSRAM (of 8 MB)") and int(_pack_memory.field) == 160 * 120 * 2,
		"RAM estimate for boards without PSRAM (fits or not) and with it")
	project.config.erase("hardware")
	_apply_hardware()
	_check(_frame().size() == 320 * 240 * 3, "default hardware is 320x240 again")
	# Serial monitor, fed with fake bytes (no board needed)
	serial_monitor.clear()
	serial_monitor.feed("\u001b[0;32mI (1000) JetStudio: Frame 40.00 ms (scripts 2.50 ms); 362 raster tris; free heap 200000, PSRAM 8000000\u001b[0m\r\n".to_utf8_buffer())
	serial_monitor.feed("I (3400) JetStudio: Frame 39.00 ms (scripts 2.40 ms); 360 raster tris; free heap 199000, PSRAM 7990000\r\n".to_utf8_buffer())
	_check(serial_monitor.stats_text().begins_with("25.0 FPS · 39.0 ms (script 2.4)"), "serial monitor shows FPS and frame time of the board")
	serial_monitor.feed("\u001b[0;31mE (3500) JetStudio: res://scripts/smoke.jet:4: Invalid".to_utf8_buffer())
	serial_monitor.feed(" access è\u001b[0m\r\n".to_utf8_buffer())
	_check("E (3500) JetStudio: res://scripts/smoke.jet:4: Invalid access è" in serial_monitor._output.get_parsed_text(),
		"serial lines split across reads are joined, UTF-8 kept, colour codes removed")
	_check(serial_monitor.serial != null and serial_monitor.serial.list_ports() is Array, "serial ports are listed")
	_check(not str(serial_monitor.serial.open("/dev/jet-studio-missing", 115200)).is_empty(), "a missing port reports an error")
	var real_decode := serial_monitor.decode
	serial_monitor.decode = func(addresses): return [["game()", "firmware/src/main.cpp:64"]] if addresses == PackedStringArray(["0x42001B71"]) else []
	serial_monitor.feed("Backtrace: 0x42001B71:0x3FCB9880\r\n".to_utf8_buffer())
	_check("↳ game()  firmware/src/main.cpp:64" in serial_monitor._output.get_parsed_text(), "crash backtraces are decoded to source lines")
	serial_monitor.decode = real_decode
	_check(_short_function("auto Renderer::R::draw<true, true>(Renderer::V const&, bool)::{lambda(auto:1)#1}::operator()<std::integral_constant<bool, true> >(std::integral_constant<bool, true>) const")
		== "auto Renderer::R::draw()::{lambda()#1}::operator() const", "backtrace function names are shortened")
	# change_scene_to_file(), autoloads and the scenes in the package
	project.write("res://scripts/game.jet", "extends Node\nvar score := 41\n")
	project.config.autoload = [{"name": "Game", "path": "res://scripts/game.jet"}]
	_apply_hardware()
	project.write("res://scripts/level2.jet", "extends Node3D\nfunc _ready():\n\tprint(\"smoke level2 \", get_tree().current_scene.name, \" \", Game.score)\n")
	project.write("res://scripts/go.jet", "extends Node3D\nfunc _ready():\n\tGame.score += 1\n\tget_tree().change_scene_to_file(\"res://level2.jetscene\")\n")
	project.write("res://level2.jetscene", JSON.stringify({"version": 2, "root":
		{"id": 1, "name": "Level2", "type": "Node3D", "script": "res://scripts/level2.jet", "children": []}}))
	_script_cache.clear()
	_root_node()["script"] = "res://scripts/go.jet"
	_upload()
	_play(true)
	for i in 3: _process(0.05)
	var changed := "smoke level2 Level2 42" in output.get_parsed_text()
	_stop()
	_check(changed and str(_root_node().name) != "Level2",
		"change_scene_to_file() loads another scene, the Game autoload keeps its state; Stop comes back")
	var scenes_pack := _root_dir().path_join(".cache/tests/scenes.jetpack")
	var has := func(bytes: PackedByteArray, text: String): return bytes.hex_encode().contains(text.to_utf8_buffer().hex_encode())
	var with_splash := FileAccess.get_file_as_bytes(scenes_pack) if _export_to(scenes_pack) else PackedByteArray()
	project.config.boot_splash = false
	var without_splash := FileAccess.get_file_as_bytes(scenes_pack) if _export_to(scenes_pack) else PackedByteArray()
	project.config.erase("boot_splash")
	_check(has.call(with_splash, "res://level2.jetscene") and has.call(with_splash, BOOT_SPLASH)
		and has.call(without_splash, "res://level2.jetscene") and not has.call(without_splash, BOOT_SPLASH),
		"the package carries the other scenes and, unless turned off, the boot splash")
	_root_node().erase("script")
	project.config.erase("autoload")
	_apply_hardware()
	# Example project
	_open_project(_example_folder())
	_check(current >= 0 and not _dirty(_tab()), "an opened scene has no unsaved changes")
	_play(false)
	_check(playing, "example project plays")
	for i in 5: _process(0.05)
	_stop()
	if not _screenshot.is_empty():
		_set_screen(Screen.VIEW_3D)
		await _capture(_screenshot)
	if _smoke_failed:
		get_tree().quit(1)
		return
	print("PASS: Godot UI -> GDExtension -> runtime -> Jet; nodes, undo, hierarchy, scripts, play/stop, errors, save, export, 2D, physics, 2D physics, 2D view, 2D gizmos, new scene roots, Change Type, Timer, signals, Signals dock connections, Animation panel, particles, Input Map, scenes, autoloads, boot splash, OBJ import, serial monitor")
	get_tree().quit()
