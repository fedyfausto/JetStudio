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
const VERSION := "0.7.1"
const SETTINGS := "user://editor.cfg"

enum Screen { VIEW_3D, SCRIPT, GAME, VIEW_2D }

var bridge: Object
var project = Project.new()
var settings := ConfigFile.new()
# {path, doc, saved, undo, redo, selected, view}
var tabs := []
var current := -1
var screen := Screen.VIEW_3D
var playing := false
var paused := false
var playTitle := ""
var pauseOnError := true
var needsRender := true
var _pythonCommand := ""
var _mergeKey := ""
var _switching := false
# path -> {mtime, info}
var _scriptCache := {}
# Godot keycodes per Input Map action, in bit order
var _actionKeys := []
var _liveTimer := 0.0
var _jobId := -1
var _jobPrefix := ""
var _jobOffset := 0
var _jobPoll := 0.0
var _jobAction := ""
var _serialWasOpen := false
var _smoke := false
# --export=<file>: export the main scene and quit without saving the editor settings
var _exportPath := ""
var _screenshot := ""
var _pendingQuit := false
var _pendingClose := -1
var _pendingDelete := 0
var _quickAction := Callable()

# UI
var scene_dock: SceneDock
var filesystem: FileSystemDock
var inspector: Inspector
var signals_dock: SignalsDock
var animation_panel: AnimationPanel
# the next upload shows the animation pose being edited again
var _repose := false
# Inspector | Node, as in Godot
var rightTabs: TabBar
var viewport: Viewport3D
var script_editor: ScriptEditor
var gameView: TextureRect
var gameLabel: Label
var sceneTabs: TabBar
var screens: Control
# Screen -> its button
var screenButtons := {}
var bottomTabs: TabBar
var bottomCard: PanelContainer
var output: RichTextLabel
var errorsList: ItemList
var statusLabel: Label
var projectLabel: Label
var modeLabel: Label
var statsLabel: Label
var playButton: Button
var playSceneButton: Button
var pauseButton: Button
var stopButton: Button
var stepButton: Button
# PC, <board> · Debug, <board> · Release
var targetSelect: OptionButton
var serial_monitor: SerialMonitor
var buildButton: Button
var flashButton: Button
var mainSplit: HSplitContainer
var centerSplit: VSplitContainer
# Dialogs
var createDialog: CreateNodeDialog
var attachDialog: AttachScriptDialog
var connect_dialog: ConnectDialog
var new_scene_dialog: NewSceneDialog
var project_manager: ProjectManager
var saveDialog: FileDialog
var exportDialog: FileDialog
var errorDialog: AcceptDialog
var confirmDialog: ConfirmationDialog
var compressDialog: ConfirmationDialog
var quitDialog: ConfirmationDialog
var closeDialog: ConfirmationDialog
var deleteDialog: ConfirmationDialog
var quickDialog: ConfirmationDialog
var quickFilter: LineEdit
var quickList: ItemList
var projectSettings: ProjectSettingsDialog
var helpDialog: AcceptDialog


func _ready():
	get_tree().auto_accept_quit = false
	settings.load(SETTINGS)
	_applyScale(float(settings.get_value("editor", "scale", 0.0)))
	theme = Style.build()
	get_window().min_size = Vector2i(1100, 700)
	_buildUi()
	_buildDialogs()

	for arg in OS.get_cmdline_user_args():
		if arg == "--smoke-test": _smoke = true
		elif arg.begins_with("--screenshot="): _screenshot = arg.trim_prefix("--screenshot=")
		elif arg.begins_with("--export="): _exportPath = arg.trim_prefix("--export=")

	if not ClassDB.class_exists("JetBridge"):
		var extension = load("res://native/jet_bridge.gdextension")

		if extension == null or not ClassDB.class_exists("JetBridge"):
			_fail("C++ bridge missing. Run ./jet-studio build before starting the editor.")
			set_process(false)
			return

	bridge = ClassDB.instantiate("JetBridge")
	viewport.bridge = bridge
	Hardware.loadCatalog(_rootDir())

	if ClassDB.class_exists("JetSerial"):
		serial_monitor.serial = ClassDB.instantiate("JetSerial")
		serial_monitor.setPreferences(str(settings.get_value("serial", "port", "")), int(settings.get_value("serial", "baud", 115200)))

	script_editor.bridge = bridge
	script_editor.class_db = bridge.class_db()
	signals_dock.class_db = script_editor.class_db
	connect_dialog.class_db = script_editor.class_db
	_log("Jet Studio %s  ·  Godot UI + C++ runtime + Jet renderer  ·  JetScript" % VERSION, Style.ACCENT)
	_log("3D view: middle button orbits · Shift+middle pans · right + WASD/QE flies · wheel zooms · F focuses · Alt+left = middle")

	if _smoke:
		#watchdog, so a script error in the test can not leave the process running.
		get_tree().create_timer(90.0).timeout.connect(func():
			printerr("SMOKE FAIL: timeout")
			get_tree().quit(2))
		_runSmoke.call_deferred()
		return

	var folder := ""

	for arg in OS.get_cmdline_user_args():
		if arg.begins_with("--project="): folder = arg.trim_prefix("--project=")

	if folder.is_empty(): folder = str(settings.get_value("projects", "last", ""))

	if folder.is_empty() or not Project.isProject(folder): folder = _exampleFolder()

	_openProject(folder)

	if not _exportPath.is_empty():
		#./jet-studio export: write the main scene of --project to a .jetpack and quit.
		get_tree().quit(0 if _exportTo(_exportPath) else 1)


func _applyScale(value: float):
	var scale := value

	if scale <= 0.0:
		scale = DisplayServer.screen_get_scale()
		var dpi := DisplayServer.screen_get_dpi()

		if scale <= 1.0 and dpi >= 192: scale = 2.0
		elif scale <= 1.0 and dpi >= 144: scale = 1.5

	get_window().content_scale_factor = clampf(scale, 0.75, 3.0)


# The folder with tools/, firmware/ and examples/: the repo, or a release whose bin/ holds the executable with the editor inside.
func _rootDir() -> String:
	var editor := ProjectSettings.globalize_path("res://")

	if editor.is_absolute_path(): return editor.trim_suffix("/").get_base_dir()
	#Linux keeps the executable in bin/, Windows at the top of the folder.
	var folder := OS.get_executable_path().get_base_dir()
	return folder if DirAccess.dir_exists_absolute(folder.path_join("examples")) else folder.get_base_dir()


# The Python that runs tools/tasks.py: python3, or on Windows python (python.org) or the py launcher.
func _python() -> String:
	if OS.get_name() != "Windows": return "python3"

	if _pythonCommand.is_empty():
		_pythonCommand = "python"

		for candidate in ["python", "py"]:
			if OS.execute(candidate, PackedStringArray(["--version"])) == 0:
				_pythonCommand = candidate
				break

	return _pythonCommand


func _exampleFolder() -> String:
	return _rootDir().path_join("examples/first_world")

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


func _buildUi():
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
	#header: brand + menus | main screens | play controls | target
	var bar := HBoxContainer.new()
	bar.add_theme_constant_override("separation", 6)
	layout.add_child(bar)
	var logo := TextureRect.new()
	logo.texture = loadImageTexture("res://assets/emblem.png")
	logo.custom_minimum_size = Vector2(26, 26)
	logo.expand_mode = TextureRect.EXPAND_IGNORE_SIZE
	logo.stretch_mode = TextureRect.STRETCH_KEEP_ASPECT_CENTERED
	logo.tooltip_text = "Jet Studio " + VERSION
	bar.add_child(logo)
	var brand := Label.new()
	brand.text = "JET STUDIO"
	brand.theme_type_variation = "BrandLabel"
	bar.add_child(brand)
	projectLabel = Label.new()
	projectLabel.add_theme_color_override("font_color", Style.MUTED)
	projectLabel.add_theme_font_size_override("font_size", 13)
	#clip long project names, otherwise they widen the window.
	projectLabel.clip_text = true
	projectLabel.text_overrun_behavior = TextServer.OVERRUN_TRIM_ELLIPSIS
	projectLabel.custom_minimum_size.x = 60
	projectLabel.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	projectLabel.size_flags_stretch_ratio = 0.6
	bar.add_child(projectLabel)
	var gap := Control.new()
	gap.custom_minimum_size.x = 10
	bar.add_child(gap)
	_menu(bar, "Scene", [
		["New Scene  (Ctrl+N)", "New", _newScene],
		["Open Scene...  (Ctrl+O)", "Load", func(): _quickOpen("scene")],
		[],
		["Save Scene  (Ctrl+S)", "Save", func(): _saveScene()],
		["Save Scene As...  (Ctrl+Shift+S)", "Save", func(): _saveSceneAs()],
		["Save All  (Ctrl+Shift+Alt+S)", "Save", _saveAll],
		[],
		["Close Scene  (Ctrl+Shift+W)", "Close", func(): _closeTab(current)],
		[],
		["Undo  (Ctrl+Z)", "", _undo],
		["Redo  (Ctrl+Shift+Z)", "", _redo],
		[],
		["Quit  (Ctrl+Q)", "", _requestQuit],
	])
	_menu(bar, "Project", [
		["Project Settings...", "Tools", _showProjectSettings],
		[],
		["Export .jetpack...", "Save", func(): exportDialog.popup_centered_ratio(0.6)],
		["Open Project Folder", "Folder", func(): OS.shell_open(project.root)],
		["Reload Project", "Reload", func(): _openProject(project.root)],
		[],
		["Project Manager...", "Filesystem", _showProjectManager],
	])
	_menu(bar, "Debug", [
		["Run External Player (SDL)", "PlayCustom", func(): _externalPlayer(false)],
		["Run Device Emulator (firmware rendering)", "PlayCustom", func(): _externalPlayer(true)],
		["Pause on Script Error", "", func(): pauseOnError = not pauseOnError; _log("Pause on error: " + ("on" if pauseOnError else "off"))],
	])
	_menu(bar, "Editor", [
		["Interface Scale: Auto", "", func(): _setScale(0.0)],
		["Interface Scale: 100%", "", func(): _setScale(1.0)],
		["Interface Scale: 125%", "", func(): _setScale(1.25)],
		["Interface Scale: 150%", "", func(): _setScale(1.5)],
		["Interface Scale: 200%", "", func(): _setScale(2.0)],
		[],
		["Show / Hide Bottom Panel", "", func(): bottomCard.visible = not bottomCard.visible],
	])
	_menu(bar, "Help", [
		["JetScript Reference", "Help", func(): OS.shell_open(_rootDir().path_join("docs/jetscript.md"))],
		["3D View Controls and Shortcuts", "Keyboard", func(): helpDialog.popup_centered(Vector2i(640, 520))],
		[],
		["About Jet Studio", "", func(): _alert("Jet Studio %s\nGodot-style editor for 3D games on microcontrollers.\nUI: Godot (MIT) · Renderer: Jet (MIT) · Icons: Godot editor (MIT)\nEditor, runtime and JetScript: Jet Studio (MIT)." % VERSION)],
	])
	var leftSpacer := Control.new()
	leftSpacer.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	bar.add_child(leftSpacer)
	var segments := PanelContainer.new()
	segments.theme_type_variation = "SegmentPanel"
	bar.add_child(segments)
	var segmentRow := HBoxContainer.new()
	segmentRow.add_theme_constant_override("separation", 2)
	segments.add_child(segmentRow)
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
		button.pressed.connect(func(): _setScreen(id))
		segmentRow.add_child(button)
		screenButtons[id] = button

	var rightSpacer := Control.new()
	rightSpacer.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	bar.add_child(rightSpacer)
	playButton = Button.new()
	playButton.theme_type_variation = "AccentButton"
	playButton.text = "Play"
	playButton.icon = Icons.get_icon("Play")
	playButton.tooltip_text = "Run the project's main scene (F5)"
	playButton.focus_mode = Control.FOCUS_NONE
	playButton.pressed.connect(func(): _play(false))
	bar.add_child(playButton)
	pauseButton = _flat("Pause", "Pause / Resume (F7)", _togglePause)
	bar.add_child(pauseButton)
	stepButton = _flat("NextFrame", "Next Frame (while paused)", _step)
	bar.add_child(stepButton)
	stopButton = _flat("Stop", "Stop the running game (F8)", _stop)
	bar.add_child(stopButton)
	playSceneButton = _flat("PlayScene", "Run the current scene (F6)", func(): _play(true))
	bar.add_child(playSceneButton)
	bar.add_child(VSeparator.new())
	targetSelect = OptionButton.new()
	targetSelect.add_icon_item(Icons.get_icon("Game"), "PC")
	#_applyHardware() puts the project's board name in these texts and in the tooltip.
	targetSelect.add_icon_item(Icons.get_icon("Tools"), "Board · Debug")
	targetSelect.add_icon_item(Icons.get_icon("Tools"), "Board · Release")
	targetSelect.focus_mode = Control.FOCUS_NONE
	targetSelect.item_selected.connect(func(i):
		project.config.target = "device" if i > 0 else "desktop"
		if i > 0: project.config.profile = "release" if i == 2 else "debug"
		project.save()
		_updateControls())
	bar.add_child(targetSelect)
	buildButton = Button.new()
	buildButton.text = "Build"
	buildButton.icon = Icons.get_icon("Tools")
	buildButton.tooltip_text = "Build the PC player or the firmware of the main scene"
	buildButton.focus_mode = Control.FOCUS_NONE
	buildButton.pressed.connect(func(): _startTask("build"))
	bar.add_child(buildButton)
	flashButton = Button.new()
	flashButton.text = "Flash"
	flashButton.icon = Icons.get_icon("ArrowDown")
	flashButton.tooltip_text = "Build and upload the firmware to the board"
	flashButton.focus_mode = Control.FOCUS_NONE
	flashButton.pressed.connect(func(): _startTask("flash"))
	bar.add_child(flashButton)
	#docks and main screen
	mainSplit = HSplitContainer.new()
	mainSplit.size_flags_vertical = Control.SIZE_EXPAND_FILL
	mainSplit.split_offset = 270
	layout.add_child(mainSplit)
	var left := VSplitContainer.new()
	left.custom_minimum_size.x = 230
	left.split_offset = 330
	mainSplit.add_child(left)
	scene_dock = SceneDock.new()
	left.add_child(_dock("Scene", scene_dock))
	filesystem = FileSystemDock.new()
	filesystem.project = project
	left.add_child(_dock("FileSystem", filesystem))
	var rightSplit := HSplitContainer.new()
	rightSplit.split_offset = -330
	mainSplit.add_child(rightSplit)
	centerSplit = VSplitContainer.new()
	centerSplit.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	centerSplit.split_offset = -150
	rightSplit.add_child(centerSplit)
	var centerCard := PanelContainer.new()
	centerCard.theme_type_variation = "Card"
	centerCard.size_flags_vertical = Control.SIZE_EXPAND_FILL
	centerSplit.add_child(centerCard)
	var center := VBoxContainer.new()
	center.add_theme_constant_override("separation", 6)
	centerCard.add_child(center)
	var tabRow := HBoxContainer.new()
	center.add_child(tabRow)
	sceneTabs = TabBar.new()
	sceneTabs.tab_close_display_policy = TabBar.CLOSE_BUTTON_SHOW_ALWAYS
	sceneTabs.drag_to_rearrange_enabled = false
	sceneTabs.focus_mode = Control.FOCUS_NONE
	sceneTabs.clip_tabs = true
	sceneTabs.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	sceneTabs.tab_changed.connect(func(i): if not _switching: _switchTab(i))
	sceneTabs.tab_close_pressed.connect(_closeTab)
	tabRow.add_child(sceneTabs)
	tabRow.add_child(_flat("Add", "New Scene (Ctrl+N)", func(): _newScene()))
	modeLabel = Label.new()
	modeLabel.add_theme_font_override("font", Style.bold_font)
	modeLabel.add_theme_font_size_override("font_size", 11)
	tabRow.add_child(modeLabel)
	screens = PanelContainer.new()
	screens.size_flags_vertical = Control.SIZE_EXPAND_FILL
	center.add_child(screens)
	viewport = Viewport3D.new()
	screens.add_child(viewport)
	script_editor = ScriptEditor.new()
	script_editor.project = project
	script_editor.docs = _rootDir().path_join("docs")
	screens.add_child(script_editor)
	var game := VBoxContainer.new()
	screens.add_child(game)
	gameLabel = Label.new()
	gameLabel.add_theme_color_override("font_color", Style.MUTED)
	gameLabel.add_theme_font_size_override("font_size", 12)
	gameLabel.text = "Press Play (F5) to run the game. Keyboard: arrows/WASD, Space/Enter, Esc."
	game.add_child(gameLabel)
	var gameArea := PanelContainer.new()
	gameArea.theme_type_variation = "ViewportFrame"
	gameArea.size_flags_vertical = Control.SIZE_EXPAND_FILL
	game.add_child(gameArea)
	gameView = TextureRect.new()
	gameView.expand_mode = TextureRect.EXPAND_IGNORE_SIZE
	gameView.stretch_mode = TextureRect.STRETCH_KEEP_ASPECT_CENTERED
	gameView.texture_filter = CanvasItem.TEXTURE_FILTER_NEAREST
	gameView.focus_mode = Control.FOCUS_CLICK
	gameArea.add_child(gameView)
	bottomCard = PanelContainer.new()
	bottomCard.theme_type_variation = "Card"
	bottomCard.custom_minimum_size.y = 120
	centerSplit.add_child(bottomCard)
	var bottom := VBoxContainer.new()
	bottomCard.add_child(bottom)
	var bottomBar := HBoxContainer.new()
	bottom.add_child(bottomBar)
	bottomTabs = TabBar.new()
	bottomTabs.focus_mode = Control.FOCUS_NONE
	bottomTabs.add_tab("Output")
	bottomTabs.add_tab("Debugger")
	bottomTabs.add_tab("Serial")
	bottomTabs.add_tab("Animation")
	#show every tab, the stats take the rest of the bar
	bottomTabs.clip_tabs = false
	bottomBar.add_child(bottomTabs)
	#board stats (Debug firmware only), visible from every tab.
	var statsGap := Control.new()
	statsGap.custom_minimum_size.x = 12
	bottomBar.add_child(statsGap)
	var deviceStats := Label.new()
	deviceStats.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	deviceStats.clip_text = true
	deviceStats.text_overrun_behavior = TextServer.OVERRUN_TRIM_ELLIPSIS
	deviceStats.custom_minimum_size.x = 40
	deviceStats.mouse_filter = Control.MOUSE_FILTER_PASS
	deviceStats.add_theme_font_override("font", Style.codeFont)
	deviceStats.add_theme_font_size_override("font_size", 12)
	deviceStats.add_theme_color_override("font_color", Style.ACCENT)
	deviceStats.tooltip_text = "From the Debug firmware on the board, every 60 frames: frames per second, frame and script time,\ntriangles drawn, free memory (internal and PSRAM)"
	bottomBar.add_child(deviceStats)
	var bottomStack := PanelContainer.new()
	bottomStack.size_flags_vertical = Control.SIZE_EXPAND_FILL
	bottom.add_child(bottomStack)
	var outputBox := VBoxContainer.new()
	bottomStack.add_child(outputBox)
	output = RichTextLabel.new()
	output.scroll_following = true
	output.selection_enabled = true
	output.context_menu_enabled = true
	output.size_flags_vertical = Control.SIZE_EXPAND_FILL
	output.meta_clicked.connect(_onMeta)
	outputBox.add_child(output)
	var debugger := VBoxContainer.new()
	debugger.visible = false
	bottomStack.add_child(debugger)
	serial_monitor = SerialMonitor.new()
	serial_monitor.visible = false
	serial_monitor.stats_changed.connect(func(text): deviceStats.text = text)
	bottomStack.add_child(serial_monitor)
	animation_panel = AnimationPanel.new()
	animation_panel.visible = false
	bottomStack.add_child(animation_panel)
	bottomTabs.tab_changed.connect(func(i):
		outputBox.visible = i == 0
		debugger.visible = i == 1
		serial_monitor.visible = i == 2
		animation_panel.visible = i == 3
		if i == 3 and bottomCard.size.y < 300: centerSplit.split_offset -= int(300 - bottomCard.size.y)
		#key icons only show with the Animation tab
		_refreshInspector()
		if i == 2 and not serial_monitor.isConnectedToBoard(): serial_monitor.refreshPorts())
	var debugTitle := Label.new()
	debugTitle.text = "Script compile and runtime errors (double-click: go to line)"
	debugTitle.add_theme_color_override("font_color", Style.MUTED)
	debugTitle.add_theme_font_size_override("font_size", 12)
	debugger.add_child(debugTitle)
	errorsList = ItemList.new()
	errorsList.size_flags_vertical = Control.SIZE_EXPAND_FILL
	errorsList.item_activated.connect(func(i):
		var meta: Dictionary = errorsList.get_item_metadata(i)
		if not str(meta.get("path", "")).is_empty(): _openScriptAt(meta.path, int(meta.line)))
	debugger.add_child(errorsList)
	var clearButton := _flat("Clear", "Clear Panel", func():
		if bottomTabs.current_tab == 2:
			serial_monitor.clear()
			return
		output.clear()
		errorsList.clear()
		_updateDebuggerTab())
	bottomBar.add_child(clearButton)
	inspector = Inspector.new()
	inspector.custom_minimum_size.x = 300
	signals_dock = SignalsDock.new()
	signals_dock.visible = false
	signals_dock.methodExists = _methodExists
	var rightCard := PanelContainer.new()
	rightCard.theme_type_variation = "Card"
	rightSplit.add_child(rightCard)
	var rightBox := VBoxContainer.new()
	rightBox.add_theme_constant_override("separation", 8)
	rightCard.add_child(rightBox)
	rightTabs = TabBar.new()
	rightTabs.focus_mode = Control.FOCUS_NONE
	rightTabs.add_tab("Inspector")
	rightTabs.add_tab("Signals")
	rightTabs.set_tab_tooltip(1, "The selected node's signals and their connections")
	rightTabs.tab_changed.connect(func(i):
		inspector.visible = i == 0
		signals_dock.visible = i == 1
		_refreshInspector())
	rightBox.add_child(rightTabs)

	for dock in [inspector, signals_dock]:
		dock.size_flags_vertical = Control.SIZE_EXPAND_FILL
		rightBox.add_child(dock)

	var footer := HBoxContainer.new()
	footer.add_theme_constant_override("separation", 14)
	layout.add_child(footer)
	statusLabel = Label.new()
	statusLabel.add_theme_color_override("font_color", Style.MUTED)
	statusLabel.add_theme_font_size_override("font_size", 12)
	statusLabel.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	statusLabel.clip_text = true
	footer.add_child(statusLabel)
	statsLabel = Label.new()
	statsLabel.add_theme_color_override("font_color", Style.MUTED)
	statsLabel.add_theme_font_size_override("font_size", 12)
	footer.add_child(statsLabel)
	var version := Label.new()
	version.text = "PROTOTYPE " + VERSION
	version.add_theme_color_override("font_color", Style.ACCENT)
	version.add_theme_font_override("font", Style.bold_font)
	version.add_theme_font_size_override("font_size", 11)
	footer.add_child(version)
	#signals
	scene_dock.node_selected.connect(_select)
	scene_dock.add_requested.connect(func(parent): _openCreate("add", parent))
	scene_dock.change_type_requested.connect(func(id):
		var node := Document.find(_rootNode(), id)
		if not node.is_empty(): _openCreate("change", id, "Change Type of %s" % node.name, "Change", str(node.type)))
	scene_dock.rename_requested.connect(_renameNode)
	scene_dock.move_requested.connect(_moveNode)
	scene_dock.delete_requested.connect(_requestDelete)
	scene_dock.duplicate_requested.connect(_duplicateNode)
	scene_dock.script_requested.connect(_scriptActionFor)
	scene_dock.visibility_toggled.connect(_toggleVisibility)
	scene_dock.copy_path_requested.connect(func(id):
		DisplayServer.clipboard_set(Document.relativePath(_rootNode(), int(_rootNode().id), id))
		_status("Node path copied to clipboard."))
	scene_dock.file_dropped.connect(_fileDropped)
	scene_dock.reorder_requested.connect(_reorderNode)
	filesystem.open_requested.connect(_openFile)
	filesystem.new_script_requested.connect(func(folder): _newScriptFile(folder))
	filesystem.new_scene_requested.connect(func(folder): _newScene(folder))
	filesystem.fileMoved.connect(_fileMoved)
	filesystem.file_removed.connect(func(path): script_editor.file_removed(path); _log("Deleted: " + path))
	filesystem.main_scene_requested.connect(func(path):
		project.config.main_scene = path
		project.save()
		filesystem.refresh()
		_log("Main scene: " + path, Style.ACCENT))
	filesystem.message.connect(func(text, isError): _log(text, Style.ERROR if isError else Style.MUTED))
	signals_dock.connect_requested.connect(_connectRequested)
	signals_dock.disconnect_requested.connect(_removeConnection)
	signals_dock.method_requested.connect(_gotoMethod)
	scene_dock.signals_requested.connect(func(id):
		_select(id)
		rightTabs.current_tab = 1)
	animation_panel.apply = func(merge, final, mutate):
		var id := animation_panel.playerId
		_repose = true
		_change(merge, final, func():
			var player := Document.find(_rootNode(), id)
			if not player.is_empty(): mutate.call(player))
	animation_panel.sceneRoot = func(): return _rootNode() if current >= 0 else {}
	animation_panel.animatable = _animatableProperties
	animation_panel.preview_requested.connect(func(id, name, at):
		if bridge != null and not playing:
			bridge.preview_animation(id, name, at)
			needsRender = true)
	inspector.keyable = func(node, key):
		return animation_panel.visible and not animation_panel.animation.is_empty() and not _animatable(node, key).is_empty()

	inspector.key_requested.connect(_keyProperty)
	inspector.property_edited.connect(_propertyEdited)
	inspector.script_property_edited.connect(_scriptPropertyEdited)
	inspector.script_action.connect(func(action): _scriptActionFor(_tab().selected if current >= 0 else 0, action))
	inspector.resolvePath = func(path): return project.toAbs(path)
	inspector.texture_requested.connect(func(key): _quickOpen("texture", func(path): _propertyEdited(key, path, true)))
	inspector.model_requested.connect(func(key): _quickOpen("model", func(path): _propertyEdited(key, path, true)))
	inspector.modelParts = func(path): return _modelInfo(str(path)).get("parts", []) if not str(path).is_empty() else []
	inspector.import_applied.connect(_importApplied)
	inspector.importedSize = _textureSize
	filesystem.file_selected.connect(_fileSelected)
	viewport.file_dropped.connect(_fileDropped)
	serial_monitor.script_link.connect(_openScriptAt)
	serial_monitor.decode = _decodeFirmwareAddresses
	serial_monitor.connection_changed.connect(func(connected):
		bottomTabs.set_tab_icon(2, Icons.get_icon("StatusSuccess") if connected else null))
	serial_monitor.preferences_changed.connect(func(port, baud):
		settings.set_value("serial", "port", port)
		settings.set_value("serial", "baud", baud)
		if not _smoke and _exportPath.is_empty(): settings.save(SETTINGS))
	viewport.model_dropped.connect(func(path, at): if current >= 0: _instantiateModel(int(_rootNode().id), path, at))
	viewport.control_moved.connect(_controlMoved)
	viewport.canvas_edited.connect(_canvasEdited)
	viewport.textureSize = _textureSize
	viewport.meshBounds = _meshBounds
	viewport.controlRect = func(node): return Document.controlRect(_rootNode(), node, _textureSize(str(node.get("texture", ""))))
	viewport.camera_changed.connect(func(): needsRender = true)
	viewport.node_picked.connect(func(id): if current >= 0: _select(id if id > 0 else 0))
	viewport.transform_edited.connect(_transformEdited)
	viewport.delete_requested.connect(func(): if current >= 0: _requestDelete(_tab().selected))
	viewport.duplicate_requested.connect(func(): if current >= 0: _duplicateNode(_tab().selected))
	script_editor.saved.connect(func(path):
		_scriptCache.erase(path)
		_log("Script saved: " + path)
		_upload())
	script_editor.script_info_changed.connect(func(path, _info):
		if current >= 0 and not playing:
			var node := Document.find(_rootNode(), _tab().selected)
			#the Signals dock marks connections whose method is missing from any script.
			if (not node.is_empty() and str(node.get("script", "")) == path) or signals_dock.visible: _refreshInspector())
	script_editor.new_script_requested.connect(func(): _newScriptFile("res://scripts"))
	script_editor.message.connect(func(text, isError): _log(text, Style.ERROR if isError else Style.MUTED))
	_setScreen(Screen.VIEW_3D)

# the node the Create Node dialog is for
var _addParent := 0
# what it is for: add (a child), root (of a new scene) or change (Change Type)
var _createMode := "add"
var _newSceneFolder := ""


func _onTypeChosen(type: String):
	match _createMode:
		"root": _createScene(type, _newSceneFolder)
		"change": _changeType(_addParent, type)
		_: _addNode(type, _addParent)


func _openCreate(mode: String, id: int, title: String = "Create New Node", ok: String = "Create", currentType: String = ""):
	_createMode = mode
	_addParent = id
	createDialog.open(title, ok, currentType)


func loadImageTexture(path: String) -> Texture2D:
	var image := Image.load_from_file(path)

	if image == null or image.is_empty(): return null

	image.generate_mipmaps()
	return ImageTexture.create_from_image(image)


func _menu(bar: HBoxContainer, title: String, items: Array):
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


func _buildDialogs():
	createDialog = CreateNodeDialog.new()
	createDialog.type_chosen.connect(_onTypeChosen)
	new_scene_dialog = NewSceneDialog.new()
	new_scene_dialog.root_chosen.connect(func(type):
		if type.is_empty(): _openCreate("root", 0, "Pick the Root Node of the Scene", "Create")
		else: _createScene(type, _newSceneFolder))
	add_child(new_scene_dialog)
	add_child(createDialog)
	attachDialog = AttachScriptDialog.new()
	attachDialog.project = project
	attachDialog.script_chosen.connect(_onAttachChosen)
	add_child(attachDialog)
	connect_dialog = ConnectDialog.new()
	connect_dialog.scriptFunctions = func(path): return _scriptInfo(path).get("functions", []).map(func(f): return str(f.name))
	connect_dialog.connection_chosen.connect(func(targetId, method, flags): _addConnection(_connecting["signal"], _connecting.arguments, targetId, method, flags))
	add_child(connect_dialog)
	project_manager = ProjectManager.new()
	project_manager.exampleFolder = _exampleFolder()
	project_manager.open_requested.connect(_openProject)
	project_manager.create_requested.connect(_createProject)
	project_manager.forget_requested.connect(func(folder):
		var recent: Array = settings.get_value("projects", "recent", [])
		recent.erase(folder)
		settings.set_value("projects", "recent", recent)
		settings.save(SETTINGS)
		project_manager.open(recent))
	add_child(project_manager)
	saveDialog = FileDialog.new()
	saveDialog.file_mode = FileDialog.FILE_MODE_SAVE_FILE
	saveDialog.access = FileDialog.ACCESS_FILESYSTEM
	saveDialog.title = "Save Scene"
	saveDialog.filters = PackedStringArray(["*.jetscene ; Jet Studio Scene"])
	saveDialog.file_selected.connect(func(path):
		var res: String = project.toRes(path)
		if not res.begins_with("res://"):
			_fail("Save the scene inside the project folder:\n" + project.root)
			return
		if res.get_extension() != "jetscene": res += ".jetscene"
		_saveScene(current, res))
	add_child(saveDialog)
	exportDialog = FileDialog.new()
	exportDialog.file_mode = FileDialog.FILE_MODE_SAVE_FILE
	exportDialog.access = FileDialog.ACCESS_FILESYSTEM
	exportDialog.title = "Export Runtime Package of the Main Scene"
	exportDialog.filters = PackedStringArray(["*.jetpack ; Jet Runtime Package"])
	exportDialog.current_file = "game.jetpack"
	exportDialog.file_selected.connect(func(path): _exportTo(path))
	add_child(exportDialog)
	errorDialog = AcceptDialog.new()
	errorDialog.title = "Jet Studio"
	add_child(errorDialog)
	confirmDialog = ConfirmationDialog.new()
	confirmDialog.cancel_button_text = "Cancel"
	add_child(confirmDialog)
	compressDialog = ConfirmationDialog.new()
	compressDialog.title = "Compress Textures"
	compressDialog.confirmed.connect(_applyCompression)
	compressDialog.canceled.connect(func():
		#declined after an import: do not ask again for these files.
		if not _afterCompression.is_valid():
			for path in _compression: _keptTextures[path] = true
		_compression = {})
	add_child(compressDialog)
	quitDialog = ConfirmationDialog.new()
	quitDialog.cancel_button_text = "Cancel"
	quitDialog.title = "Unsaved Changes"
	quitDialog.ok_button_text = "Save & Quit"
	quitDialog.add_button("Quit Without Saving", true, "discard")
	quitDialog.confirmed.connect(func():
		if _saveAll(): _quit())
	quitDialog.custom_action.connect(func(action):
		if action == "discard":
			quitDialog.hide()
			_quit())
	add_child(quitDialog)
	closeDialog = ConfirmationDialog.new()
	closeDialog.cancel_button_text = "Cancel"
	closeDialog.title = "Unsaved Changes"
	closeDialog.ok_button_text = "Save & Close"
	closeDialog.add_button("Close Without Saving", true, "discard")
	closeDialog.confirmed.connect(func():
		if _saveScene(_pendingClose): _closeTab(_pendingClose, true))
	closeDialog.custom_action.connect(func(action):
		if action == "discard":
			closeDialog.hide()
			_closeTab(_pendingClose, true))
	add_child(closeDialog)
	deleteDialog = ConfirmationDialog.new()
	deleteDialog.cancel_button_text = "Cancel"
	deleteDialog.title = "Delete Node"
	deleteDialog.ok_button_text = "Delete"
	deleteDialog.confirmed.connect(func(): _deleteNode(_pendingDelete))
	add_child(deleteDialog)
	quickDialog = ConfirmationDialog.new()
	quickDialog.cancel_button_text = "Cancel"
	quickDialog.ok_button_text = "Open"
	var quickBox := VBoxContainer.new()
	quickDialog.add_child(quickBox)
	quickFilter = LineEdit.new()
	quickFilter.placeholder_text = "Search"
	quickFilter.right_icon = Icons.get_icon("Search")
	quickBox.add_child(quickFilter)
	quickList = ItemList.new()
	quickList.custom_minimum_size = Vector2(480, 320)
	quickList.size_flags_vertical = Control.SIZE_EXPAND_FILL
	quickList.item_activated.connect(func(_i): quickDialog.hide(); _quickConfirm())
	quickBox.add_child(quickList)
	quickDialog.register_text_enter(quickFilter)
	quickDialog.confirmed.connect(_quickConfirm)
	add_child(quickDialog)
	projectSettings = ProjectSettingsDialog.new()
	projectSettings.applied.connect(func():
		projectLabel.text = "/  " + project.projectName()
		_applyHardware()
		filesystem.refresh()
		_refreshTitle())
	add_child(projectSettings)
	helpDialog = AcceptDialog.new()
	helpDialog.title = "Controls and Shortcuts"
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
	helpDialog.add_child(help)
	add_child(helpDialog)

func _log(message: String, color: Color = Style.MUTED):
	print("[Jet Studio] " + message)

	if output == null: return

	output.push_color(color)
	output.add_text(message + "\n")
	output.pop()


func _logLink(message: String, path: String, line: int, color: Color):
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


func _onMeta(meta: Variant):
	var text := str(meta)
	var colon := text.rfind(":")

	if colon > 6: _openScriptAt(text.substr(0, colon), int(text.substr(colon + 1)))


func _status(text: String):
	statusLabel.text = text


func _fail(message: String):
	_log(message, Style.ERROR)
	errorDialog.dialog_text = message
	errorDialog.popup_centered()


func _alert(message: String):
	errorDialog.dialog_text = message
	errorDialog.popup_centered()


func _debugError(message: String, path: String, line: int):
	var text := "%s  —  %s:%d" % [message, path, line] if not path.is_empty() else message
	var index := errorsList.add_item(text, Icons.get_icon("StatusError"))
	errorsList.set_item_metadata(index, {"path": path, "line": line})
	errorsList.set_item_tooltip(index, text)
	_updateDebuggerTab()


func _updateDebuggerTab():
	var count := errorsList.item_count
	bottomTabs.set_tab_title(1, "Debugger (%d)" % count if count > 0 else "Debugger")
	bottomTabs.set_tab_icon(1, Icons.get_icon("StatusError") if count > 0 else null)

func _openProject(folder: String):
	if playing: _stop()

	if not project.root.is_empty(): _saveState()

	var error: String = project.open(folder)

	if not error.is_empty():
		_fail(error)

		if project.root.is_empty(): _showProjectManager()

		return

	tabs.clear()
	current = -1
	_switching = true

	while sceneTabs.tab_count > 0: sceneTabs.remove_tab(0)

	_switching = false
	script_editor.closeAll()
	_scriptCache.clear()
	errorsList.clear()
	_updateDebuggerTab()
	filesystem.refresh()
	var release: bool = project.config.get("profile", "debug") == "release"
	#"nano" is the device target of projects saved before the STM32 boards.
	targetSelect.select((2 if release else 1) if project.config.get("target", "desktop") in ["device", "nano"] else 0)
	_applyHardware()
	var recent: Array = settings.get_value("projects", "recent", [])
	recent.erase(project.root)
	recent.push_front(project.root)
	settings.set_value("projects", "recent", recent.slice(0, 12))
	settings.set_value("projects", "last", project.root)

	if not _smoke and _exportPath.is_empty(): settings.save(SETTINGS)

	_log("Project opened: %s  (%s)" % [project.projectName(), project.root], Style.ACCENT)
	projectLabel.text = "/  " + project.projectName()
	var state := _loadState()

	for path in state.get("scenes", []):
		if project.exists(path): _openScene(path, false)

	if tabs.is_empty():
		if not project.main_scene().is_empty() and project.exists(project.main_scene()): _openScene(project.main_scene(), false)
		else: _createScene("Node3D")

	var views: Dictionary = state.get("views", {})

	for tab in tabs:
		if views.has(tab.path): tab.view = views[tab.path]

	_switchTab(clampi(int(state.get("current", 0)), 0, tabs.size() - 1))

	for path in state.get("scripts", []):
		if project.exists(path): script_editor.openScript(path)

	_setScreen(int(state.get("screen", Screen.VIEW_3D)) if int(state.get("screen", 0)) != Screen.GAME else Screen.VIEW_3D)
	_viewForScene()


func _statePath() -> String:
	return project.root.path_join(".jetstudio/editor_state.json")


func _loadState() -> Dictionary:
	var data = JSON.parse_string(FileAccess.get_file_as_string(_statePath())) if FileAccess.file_exists(_statePath()) else null
	return data if data is Dictionary else {}


func _saveState():
	if project.root.is_empty(): return

	if current >= 0: _tab().view = viewport.saveState()

	var scenes := []
	var views := {}

	for tab in tabs:
		if not tab.path.is_empty():
			scenes.append(tab.path)
			views[tab.path] = tab.view

	var state := {"scenes": scenes, "current": maxi(current, 0), "views": views, "screen": screen,
		"scripts": script_editor._order.duplicate()}
	Project.writeText(_statePath(), JSON.stringify(state, "\t"))

const STARTER_SCRIPT := "extends MeshInstance3D\n\n# Rotation speed in degrees per second (editable in the Inspector).\n@export var speed := 45.0\n\n\nfunc _process(delta: float) -> void:\n\trotation_degrees.y += speed * delta\n"


func _createProject(folder: String, title: String):
	var doc := Document.example()
	var cube := Document.find(doc.root, 6)
	cube.script = "res://scripts/spin.jet"
	cube.script_properties = {}
	var scene := JSON.stringify(doc, "\t") + "\n"
	var error := Project.create(folder, title, scene, STARTER_SCRIPT)

	if not error.is_empty():
		_fail(error)
		return

	_openProject(folder)


func _showProjectManager():
	project_manager.open(settings.get_value("projects", "recent", []))


func _showProjectSettings():
	projectSettings.open(project)


func _applyHardware():
	var actions := Actions.of(project.config)

	if bridge != null:
		bridge.set_input_map(Actions.bridgeActions(actions))
		bridge.set_autoloads(project.config.get("autoload", []))
		#scripts may use autoload names that just appeared or went away
		_scriptCache.clear()

	_actionKeys = Actions.keyCodes(actions)
	var hw := Hardware.settings(project.config)
	var size := Hardware.resolution(hw)

	if bridge != null: bridge.set_resolution(size.x, size.y)

	viewport.set_resolution(size)
	var board: Dictionary = Hardware.boards.get(hw.board, {})
	var short := str(board.get("short", "Device"))
	targetSelect.set_item_text(1, short + " · Debug")
	targetSelect.set_item_text(2, short + " · Release")
	var display := Hardware.displayOf(hw)
	targetSelect.tooltip_text = "Build and Flash target.\n%s + %s, %d×%d (Project > Project Settings > Hardware).\n" % [board.get("name", "?"), display.get("name", "?"), size.x, size.y] \
		+ "Debug: serial log, script print(), errors with file and line, FPS and memory (Serial panel).\n" \
		+ "Release: no console, logs or assertions: smaller, lighter firmware."

	flashButton.tooltip_text = "Build and upload the firmware to %s %s" % [board.get("name", "?"),
		"with the ST-LINK (the board's USB)" if Hardware.platform(hw) == "stm32" else "over USB"]
	_upload()


func _setScale(value: float):
	settings.set_value("editor", "scale", value)
	settings.save(SETTINGS)
	_applyScale(value)

func _tab() -> Dictionary:
	return tabs[current]


func _rootNode() -> Dictionary:
	return tabs[current].doc.root if current >= 0 else {}


func _tabTitle(tab: Dictionary) -> String:
	var title: String = tab.path.get_file().get_basename() if not tab.path.is_empty() else "[unsaved]"
	return title + ("(*)" if _dirty(tab) else "")


func _dirty(tab: Dictionary) -> bool:
	return JSON.stringify(tab.doc) != tab.saved


func _addTab(path: String, doc: Dictionary, saved: String):
	var root: Dictionary = doc.root
	tabs.append({"path": path, "doc": doc, "saved": saved, "undo": [], "redo": [], "selected": int(root.id), "view": {}})
	_switching = true
	sceneTabs.add_tab(_tabTitle(tabs[-1]), Icons.forNode(str(root.type)))
	_switching = false
	_switchTab(tabs.size() - 1)


func _switchTab(index: int):
	if index < 0 or index >= tabs.size(): return

	if playing: _stop()

	if current >= 0 and current < tabs.size(): tabs[current].view = viewport.saveState()

	current = index
	_mergeKey = ""
	_switching = true
	sceneTabs.current_tab = index
	_switching = false
	animation_panel.edit(0)

	if tabs[index].view.is_empty(): viewport.resetView()
	else: viewport.loadState(tabs[index].view)
	_viewForScene()
	_upload()
	_refreshAll()


# As in Godot, a 2D or UI scene shows in the 2D view and a 3D one in the 3D view.
func _viewForScene():
	if current < 0 or not screen in [Screen.VIEW_2D, Screen.VIEW_3D]: return

	var rootType := str(_rootNode().type)

	if Document.is2D(rootType) or Document.isControl(rootType): _setScreen(Screen.VIEW_2D)
	elif Document.is3D(rootType): _setScreen(Screen.VIEW_3D)


# Asks for the root, as Godot's Scene dock does for a new scene.
func _newScene(folder: String = ""):
	_newSceneFolder = folder
	new_scene_dialog.open()


func _createScene(type: String, folder: String = ""):
	var doc := Document.newScene(type, type)

	if type == "Node3D":
		#light and environment, so the first meshes are not drawn in the dark.
		var env := Document.newNode("WorldEnvironment", 2, "WorldEnvironment")
		var sun := Document.newNode("DirectionalLight3D", 3, "Sun")
		sun.rotation = [-50.0, -35.0, 0.0]
		doc.root.children = [env, sun]
	elif type == "Control":
			doc.root.size = [viewport.W, viewport.H]

	_addTab("", doc, "")

	if not folder.is_empty(): saveDialog.current_dir = project.toAbs(folder)

	_log("New scene with a %s root. Add nodes with the + button of the Scene dock (Ctrl+A)." % type)


# Godot's Change Type: name, children, script and the shared properties stay; connections of signals it no longer has go.
func _changeType(id: int, newType: String):
	var node := Document.find(_rootNode(), id)

	if playing or node.is_empty() or str(node.type) == newType: return

	var oldType := str(node.type)
	_change("", true, func():
		var n := Document.find(_rootNode(), id)
		Document.changeType(n, newType)
		var kept := _signalNames(n)
		_tab().doc.connections = Document.connections(_tab().doc).filter(func(c): return int(c.from) != id or str(c["signal"]) in kept)
	, true)
	if id == int(_rootNode().id): sceneTabs.set_tab_icon(current, Icons.forNode(newType))

	_status("%s is now a %s (was %s)." % [node.name, newType, oldType])
	var path := str(node.get("script", ""))
	var base := str(_scriptInfo(path).get("base", "")) if not path.is_empty() else ""

	if not base.is_empty() and not Document.inherits(newType, base):
		_log("%s's script extends %s, which a %s is not: change the script's extends or detach it." % [node.name, base, newType], Style.WARNING)


func _animatable(node: Dictionary, key: String) -> Dictionary:
	if node.is_empty(): return {}

	var prop := Document.propertyDef(str(node.type), key)

	if prop.is_empty(): return {}
	#3D rotation is stored in degrees
	var property := "rotation_degrees" if key == "rotation" and Document.is3D(str(node.type)) else key
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

				return {"key": key, "label": Document.labelFor(prop), "property": property, "type": type}

		cls = str(found.base)

	return {}


func _animatableProperties(node: Dictionary) -> Array:
	var out := []

	for cls in Document.classChain(str(node.get("type", ""))):
		for prop in Document.PROPERTIES.get(cls, []):
			var info := _animatable(node, str(prop.key))

			if not info.is_empty(): out.append(info)

	return out


func _keyProperty(key: String):
	var node := Document.find(_rootNode(), _tab().selected)
	var info := _animatable(node, key)
	var player := Document.find(_rootNode(), animation_panel.playerId)

	if info.is_empty() or player.is_empty(): return

	var parent := Document.parentOf(_rootNode(), int(player.id))
	var path := Document.relativePath(_rootNode(), int(parent.id), int(node.id)) if not parent.is_empty() else "."
	var value = node.get(key)
	animation_panel.insertKey(path, str(info.property), key, str(info.type), value.duplicate() if value is Array else value)
	_status("Key: %s:%s at %.2f s." % [node.name, info.property, animation_panel.time])


# The signals a node has: its script's and its classes'.
func _signalNames(node: Dictionary) -> Array:
	var names := []

	for sig in _scriptInfo(str(node.get("script", ""))).get("signals", []): names.append(str(sig.name))

	var cls := str(node.type)

	while not cls.is_empty():
		var found := {}

		for c in script_editor.class_db:
			if c.name == cls: found = c

		if found.is_empty(): break

		for sig in found.get("signals", []): names.append(str(sig.name))

		cls = str(found.base)

	return names


func _openFile(path: String):
	match path.get_extension():
		"jetscene": _openScene(path)
		"jet": _openScriptAt(path, 0)
		_: OS.shell_open(project.toAbs(path))


func _openScene(path: String, notify: bool = true):
	for i in tabs.size():
		if tabs[i].path == path:
			_switchTab(i)
			return

	var text: String = project.read(path)
	var json := JSON.new()

	if json.parse(text) != OK:
		_fail("%s: invalid JSON at line %d: %s" % [path, json.get_error_line(), json.get_error_message()])
		return

	var data = json.data
	var saved := ""

	if data is Dictionary and int(data.get("version", 0)) == 1:
		var migrated := Document.migrateV1(data)
		data = migrated[0]
		_log("%s converted from format 1 to format 2: save it to update the file." % path, Style.WARNING)

		for note in migrated[1]: _log("  " + note, Style.WARNING)

	var error := Document.validate(data)

	if not error.is_empty():
		_fail("%s: %s" % [path, error])
		return

	Document.normalize(data)

	if int(json.data.get("version", 0)) == 2: saved = JSON.stringify(data)

	_addTab(path, data, saved)

	if notify: _log("Scene opened: " + path, Style.ACCENT)


func _saveScene(index: int = -1, path: String = "") -> bool:
	if index < 0: index = current

	if index < 0: return false

	var tab: Dictionary = tabs[index]

	if path.is_empty(): path = tab.path

	if path.is_empty():
		current = index
		_saveSceneAs()
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

	_refreshTitle()
	filesystem.refresh()
	_log("Scene saved: " + path, Style.ACCENT)
	return true


func _saveSceneAs():
	if current < 0: return

	var tab := _tab()
	saveDialog.current_dir = project.toAbs(tab.path.get_base_dir()) if not tab.path.is_empty() else project.root
	saveDialog.current_file = (tab.path.get_file() if not tab.path.is_empty() else str(tab.doc.root.name).to_snake_case() + ".jetscene")
	saveDialog.popup_centered_ratio(0.6)


func _saveAll() -> bool:
	var ok := script_editor.saveAll()

	for i in tabs.size():
		if _dirty(tabs[i]) or tabs[i].path.is_empty():
			if tabs[i].path.is_empty():
				_switchTab(i)
				_saveSceneAs()
				return false

			ok = _saveScene(i) and ok

	return ok


func _closeTab(index: int, force: bool = false):
	if index < 0 or index >= tabs.size(): return

	if not force and _dirty(tabs[index]):
		_pendingClose = index
		closeDialog.dialog_text = "Save changes to \"%s\" before closing?" % _tabTitle(tabs[index]).trim_suffix("(*)")
		closeDialog.popup_centered()
		return

	if playing: _stop()

	tabs.remove_at(index)
	_switching = true
	sceneTabs.remove_tab(index)
	_switching = false
	current = -1

	if tabs.is_empty(): _createScene("Node3D")
	else: _switchTab(mini(index, tabs.size() - 1))


func _refreshTitle():
	if current < 0: return

	for i in tabs.size(): sceneTabs.set_tab_title(i, _tabTitle(tabs[i]))

	var tab := _tab()
	get_window().title = "%s - %s - Jet Studio" % [_tabTitle(tab), project.projectName()]
	var unsaved := _dirty(tab)
	_status(("Unsaved changes · " if unsaved else "") + (tab.path if not tab.path.is_empty() else "Scene not saved yet"))

const BOOT_SPLASH := "res://.jetstudio/boot_splash.jetscene"
const BOOT_SPLASH_SCRIPT := "res://.jetstudio/boot_splash.jet"
const BOOT_SPLASH_LOGO := "res://.jetstudio/boot_splash_logo.png"


func _bootSplashDoc(main: String) -> Dictionary:
	var size := Hardware.resolution(Hardware.settings(project.config))
	var titleScale := 2 if size.x >= 160 else 1
	var top := (size.y - (64 + 10 + 15 + 6 + 15 * titleScale)) / 2
	var madeWith := "made with"
	var title := "JET STUDIO"
	var root := {"id": 1, "name": "BootSplash", "type": "Node3D", "script": BOOT_SPLASH_SCRIPT, "script_properties": {}, "children": [
		{"id": 2, "name": "WorldEnvironment", "type": "WorldEnvironment", "background_color": 0x10141c, "children": []},
		{"id": 3, "name": "Logo", "type": "TextureRect", "texture": BOOT_SPLASH_LOGO, "position": [(size.x - 64) / 2, top], "children": []},
		{"id": 4, "name": "MadeWith", "type": "Label", "text": madeWith, "color": 0x8a93a6,
			"position": [(size.x - madeWith.length() * 7) / 2, top + 74], "children": []},
		{"id": 5, "name": "Title", "type": "Label", "text": title, "color": 0xe2e7f0, "text_scale": titleScale,
			"position": [(size.x - title.length() * 7 * titleScale) / 2, top + 95], "children": []},
	]}
	var doc := {"format": "jetscene", "version": 2, "root": root}
	Document.normalize(doc)
	return doc


func _bootSplashScript(main: String) -> String:
	return """extends Node3D

var time := 0.0
@onready var parts := [get_node("Logo"), get_node("MadeWith"), get_node("Title")]


func _process(delta: float):
	time += delta
	var fade := clampf(minf(time, 2.2 - time) / 0.4, 0.0, 1.0)
	for part in parts:
		part.alpha = fade
	if time >= 2.2:
		get_tree().change_scene_to_file("%s")
""" % main


func _bootSplashLogo() -> Image:
	return Image.load_from_file("res://assets/emblem.png")

# The project's other scenes, so change_scene_to_file() finds them in Play and in the package.
func _otherScenes(except: String) -> Dictionary:
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

			var doc := _sceneDocFor(path)

			if doc.is_empty(): _log("Scene skipped, it can not be read: " + path, Style.WARNING)
			else: out[path] = doc

	return out


func _allSources(doc: Dictionary, others: Dictionary) -> Dictionary:
	var sources := _sceneSources(doc)

	for other in others.values(): sources.merge(_sceneSources(other))

	sources.merge(_autoloadSources())
	return sources


func _autoloadSources() -> Dictionary:
	var sources := {}

	for entry in project.config.get("autoload", []):
		var path := str(entry.get("path", ""))
		var text = script_editor.sourceOf(path)

		if text == null: text = project.read(path) if project.exists(path) else ""

		sources[path] = text

	return sources


func _allAssets(doc: Dictionary, others: Dictionary) -> Dictionary:
	var assets := _sceneAssets(doc)

	for other in others.values():
		var more := _sceneAssets(other)
		assets.textures.merge(more.textures)
		assets.models.merge(more.models)

	return assets


func _sceneSources(doc: Dictionary) -> Dictionary:
	var sources := {}
	Document.walk(doc.root, func(n, _p, _d):
		var path := str(n.get("script", ""))
		if path.is_empty() or sources.has(path): return
		var text = script_editor.sourceOf(path)
		if text == null: text = project.read(path) if project.exists(path) else ""
		sources[path] = text)
	return sources

# absolute path -> {stamp, image, palette}
var _imageCache := {}
# res path -> {obj_stamp, obj, mtl_file, mtl_stamp, mtl, options, info_stamp, info}
var _modelCache := {}
# res path -> {mtime, settings}
var _importCache := {}
# the largest package the firmware build embeds (tools/tasks.py)
const PACK_LIMIT := 1024 * 1024
# above this, adding a texture offers to compress it
const HEAVY_TEXTURE := 96 * 1024
const BOARD_TEXTURE_SIDE := 256
# above this, adding a model warns about the frame rate on the board
const BOARD_TRIANGLES := 3000
# textures of the last exported package
var _packTextures := []
# its RAM estimate on the board (bridge.memory_estimate)
var _packMemory := {}
# textures the user chose to keep as they are, this session
var _keptTextures := {}
# res path -> import settings waiting for the user's answer
var _compression := {}
var _afterCompression := Callable()


func _importFile(path: String) -> String:
	return project.toAbs(path) + ".import"


func _fileStamp(file: String) -> int:
	return FileAccess.get_modified_time(file) if FileAccess.file_exists(file) else 0


# Reads "<file>.import" next to a PNG or OBJ, or returns the defaults.
func _importSettings(path: String) -> Dictionary:
	var file := _importFile(path)
	var mtime := _fileStamp(file)
	var cached: Dictionary = _importCache.get(path, {})

	if not cached.is_empty() and cached.mtime == mtime: return cached.settings

	var settings := Document.importDefaults(Document.importKind(path))

	if mtime > 0:
		var data = JSON.parse_string(FileAccess.get_file_as_string(file))
		var params = data.get("params") if data is Dictionary else null

		if params is Dictionary:
			for key in settings:
				var value = params.get(key)

				if value == null: continue

				if settings[key] is float and (value is float or value is int): settings[key] = float(value)
				elif typeof(value) == typeof(settings[key]): settings[key] = value

	_importCache[path] = {"mtime": mtime, "settings": settings}
	return settings


func _textureImage(path: String) -> Image:
	var file: String = project.toAbs(path)

	if path.is_empty() or not FileAccess.file_exists(file): return null

	var settings := _importSettings(path)
	var stamp := "%d:%s" % [FileAccess.get_modified_time(file), JSON.stringify(settings)]
	var cached: Dictionary = _imageCache.get(file, {})

	if cached.get("stamp", "") != stamp:
		var image := _importedImage(file, settings)

		if image == null: return null

		cached = {"stamp": stamp, "image": image, "palette": settings.palette, "palette_fps": int(settings.get("palette_fps", 0))}
		_imageCache[file] = cached

	return cached.image


func _importedImage(file: String, settings: Dictionary) -> Image:
	var image := Image.load_from_file(file)

	if image == null or image.is_empty(): return null

	if settings.flip_v: image.flip_y()

	var limit := int(settings.max_size) if str(settings.max_size).is_valid_int() else 0
	var size := image.get_size()

	if limit > 0 and maxi(size.x, size.y) > limit:
		var k := float(limit) / maxi(size.x, size.y)
		image.resize(maxi(1, roundi(size.x * k)), maxi(1, roundi(size.y * k)), Image.INTERPOLATE_LANCZOS)

	return image


# {size, bytes, indexed, colors} of a PNG imported with these settings.
func _textureCost(path: String, settings: Dictionary) -> Dictionary:
	var image := _textureImage(path) if settings == _importSettings(path) else _importedImage(project.toAbs(path), settings)

	if image == null or bridge == null: return {}

	var info: Dictionary = bridge.texture_info(image, settings.palette)

	if not info.is_empty(): info.size = image.get_size()

	return info


func _textureText(cost: Dictionary) -> String:
	return "%d×%d, %s, %s" % [cost.size.x, cost.size.y, ("%d colors" % cost.colors) if cost.indexed else "RGB565",
		String.humanize_size(int(cost.bytes))]


# What suits the board when a texture is added: at most 256 px and 256 colors (unless RGB565 was chosen on purpose).
func _boardTexture(settings: Dictionary, cost: Dictionary) -> Dictionary:
	var out := settings.duplicate()

	if maxi(cost.size.x, cost.size.y) > BOARD_TEXTURE_SIDE: out.max_size = str(BOARD_TEXTURE_SIDE)

	if not cost.indexed and settings.palette != "RGB565": out.palette = "Quantize"

	return out


# One step smaller: 256 colors first, then the next Max Size below the current side. Empty when it can not shrink.
func _smallerTexture(settings: Dictionary, cost: Dictionary) -> Dictionary:
	var out := settings.duplicate()

	if not cost.indexed:
		out.palette = "Quantize"
		return out

	for step in ["1024", "512", "256", "128", "64", "32"]:
		if int(step) < maxi(cost.size.x, cost.size.y):
			out.max_size = step
			return out

	return {}


# After a model or an image is added: offers to compress the textures that are heavy for the board.
func _offerCompression(paths: Array):
	var plan := {}
	var lines := []

	for path in paths:
		if plan.has(path) or _keptTextures.has(path): continue

		var settings := _importSettings(path)
		var cost := _textureCost(path, settings)

		if cost.is_empty() or int(cost.bytes) <= HEAVY_TEXTURE: continue

		var lighter := _boardTexture(settings, cost)
		var after := _textureCost(path, lighter)

		if after.is_empty() or int(after.bytes) >= int(cost.bytes): continue

		plan[path] = lighter
		lines.append("• %s: %s → %s" % [path.get_file(), _textureText(cost), _textureText(after)])

	if plan.is_empty(): return

	_askCompression(plan, "These textures are heavy for the board:\n%s\n\nCompress them? Only the import settings change, the PNG files stay as they are." % "\n".join(lines),
		"Compress", "Keep Original", Callable())


# The import settings that bring the package under the board's limit, shrinking the largest texture first.
func _packPlan(packSize: int) -> Dictionary:
	var entries := {}

	for path in _packTextures:
		var settings := _importSettings(path)
		var cost := _textureCost(path, settings)

		if not cost.is_empty(): entries[path] = {"settings": settings, "cost": cost, "before": cost, "done": false}

	var size := packSize

	while size > PACK_LIMIT:
		var largest := ""

		for path in entries:
			if not entries[path].done and (largest.is_empty() or int(entries[path].cost.bytes) > int(entries[largest].cost.bytes)):
				largest = path

		if largest.is_empty(): break

		var entry: Dictionary = entries[largest]
		var smaller := _smallerTexture(entry.settings, entry.cost)
		var cost := _textureCost(largest, smaller) if not smaller.is_empty() else {}

		if cost.is_empty() or int(cost.bytes) >= int(entry.cost.bytes):
			entry.done = true
			continue

		size -= int(entry.cost.bytes) - int(cost.bytes)
		entry.settings = smaller
		entry.cost = cost

	var plan := {}

	for path in entries:
		if entries[path].settings != _importSettings(path): plan[path] = entries[path]

	return {"size": size, "plan": plan}


# The game's RAM on the board, from the last export: an estimate, logged before building.
# Without PSRAM it says whether it fits with both field buffers, with one (slower), or not at all.
func _reportMemory():
	if _packMemory.is_empty(): return

	var hw := Hardware.settings(project.config)
	var board: Dictionary = Hardware.boards.get(str(hw.board), {})
	var heaviest := {}

	for scene in _packMemory.scenes:
		if heaviest.is_empty() or int(scene.total) > int(heaviest.total): heaviest = scene

	var shared: int = int(_packMemory.engine) + int(_packMemory.frames) + int(_packMemory.textures) + int(_packMemory.scripts)
	var need := shared + int(heaviest.get("total", 0))
	var field := int(_packMemory.field)
	var buffers := int(_packMemory.frames) + field

	var kb := func(bytes: int) -> String: return "%d KB" % ceili(bytes / 1024.0)
	var detail := "textures %s, scripts %s, frame buffers %s; heaviest scene %s: nodes %s, meshes %s, triangle queues %s, particles %s" % [
		kb.call(int(_packMemory.textures)), kb.call(int(_packMemory.scripts)), kb.call(buffers),
		str(heaviest.get("scene", "")).get_file(), kb.call(int(heaviest.get("nodes", 0))), kb.call(int(heaviest.get("meshes", 0))),
		kb.call(int(heaviest.get("triangles", 0))), kb.call(int(heaviest.get("particles", 0)))]
	var extra := "Plus what scripts create while playing (spawned scenes, arrays)."

	#the ESP32 keeps the frame buffers in internal RAM and loads the scene into PSRAM.
	if board.has("psram"):
		var megabytes := int(str(board.psram).to_int())
		var inPsram := need + field - buffers

		if inPsram <= megabytes * 1024 * 1024:
			_log("RAM (estimate): about %s in PSRAM (of %d MB) and %s of frame buffers in internal RAM (%s). %s" % [
				kb.call(inPsram), megabytes, kb.call(buffers), detail, extra], Style.MUTED)
		else:
			_log("RAM (estimate): about %s, the board has %d MB of PSRAM: the game probably will not start (%s)." % [
				kb.call(inPsram), megabytes, detail], Style.ERROR)

		return

	#what is left of the RAM for the heap: the firmware's own variables and the stack take about 32 KB.
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
		_log("RAM (estimate): about %s, the board has %s: the game probably will not start (%s). Use smaller textures, fewer nodes or simpler meshes." % [
			kb.call(need), kb.call(available), detail], Style.ERROR)


# Stops a board build whose package is over the limit, offering the compression that makes it fit.
func _fitsBoard(packPath: String, action: String) -> bool:
	var size := FileAccess.get_file_as_bytes(packPath).size()

	if size <= PACK_LIMIT: return true

	var result := _packPlan(size)
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
		lines.append("• %s: %s → %s" % [path.get_file(), _textureText(entry.before), _textureText(entry.cost)])

	_askCompression(settings, "The game takes %s, the board holds %s.\nCompressing these textures brings it to %s:\n%s\n\nOnly the import settings change, the PNG files stay as they are." % [
		String.humanize_size(size), limit, String.humanize_size(result.size), "\n".join(lines)],
		"Compress and %s" % action.capitalize(), "Cancel", func(): _startTask(action))
	return false


func _askCompression(plan: Dictionary, text: String, ok: String, cancel: String, then: Callable):
	_compression = plan
	_afterCompression = then
	compressDialog.dialog_text = text
	compressDialog.ok_button_text = ok
	compressDialog.cancel_button_text = cancel
	compressDialog.popup_centered()


func _applyCompression():
	for path in _compression:
		if not _saveImport(path, _compression[path]): return

		_log("Compressed for the board: %s (%s)" % [path, _textureText(_textureCost(path, _compression[path]))], Style.ACCENT)

	_compression = {}
	_upload()
	_refreshAll()

	if _afterCompression.is_valid(): _afterCompression.call()


func _textureSize(path: String) -> Vector2i:
	var image := _textureImage(path)
	return image.get_size() if image != null else Vector2i.ZERO


func _modelData(path: String) -> Dictionary:
	var file: String = project.toAbs(path)

	if path.is_empty() or not FileAccess.file_exists(file): return {}

	var settings := _importSettings(path)
	var obj_stamp := "%d:%s" % [FileAccess.get_modified_time(file), JSON.stringify(settings)]
	var cached: Dictionary = _modelCache.get(path, {})

	if cached.get("obj_stamp", "") != obj_stamp:
		var text := FileAccess.get_file_as_string(file)
		var mtl_file := ""
		var at := text.find("mtllib ")

		if at >= 0 and (at == 0 or text[at - 1] == "\n"):
			var end := text.find("\n", at)

			if end < 0: end = text.length()

			mtl_file = file.get_base_dir().path_join(text.substr(at + 7, end - at - 7).strip_edges())

		cached = {"obj_stamp": obj_stamp, "obj": text, "mtl_file": mtl_file, "mtl_stamp": -1, "mtl": "", "options": settings}
		_modelCache[path] = cached

	var mtl_stamp := _fileStamp(cached.mtl_file) if not cached.mtl_file.is_empty() else 0

	if cached.mtl_stamp != mtl_stamp:
		cached.mtl = FileAccess.get_file_as_string(cached.mtl_file) if mtl_stamp > 0 else ""
		cached.mtl_stamp = mtl_stamp

	return {"obj": cached.obj, "mtl": cached.mtl, "options": cached.options}


# {error, parts, materials} from the bridge, cached per file version.
func _modelInfo(path: String) -> Dictionary:
	var data := _modelData(path)

	if data.is_empty() or bridge == null: return {}

	var cached: Dictionary = _modelCache[path]
	var stamp := "%s|%d" % [cached.obj_stamp, cached.mtl_stamp]

	if cached.get("info_stamp", "") != stamp:
		cached.info = bridge.import_model(data.obj, data.mtl, data.options)
		cached.info_stamp = stamp

	return cached.info


func _meshBounds(node: Dictionary) -> Array:
	if str(node.type) == "Sprite3D":
		#one frame of the texture, pixel_size metres per texel, centred like Godot's.
		var size := Vector2(_textureSize(str(node.get("texture", ""))))
		size = size / Vector2(maxi(1, int(node.get("hframes", 1))), maxi(1, int(node.get("vframes", 1)))) * float(node.get("pixel_size", 0.01))

		if size == Vector2.ZERO: return []

		return [Vector3(-size.x, -size.y, 0) * 0.5, Vector3(size.x, size.y, 0) * 0.5]

	for part in _modelInfo(str(node.get("mesh_file", ""))).get("parts", []):
		if str(part.name) == str(node.get("mesh_part", "")):
			var box: AABB = part.aabb
			return [box.position, box.end]

	return []


func _sceneAssets(doc: Dictionary) -> Dictionary:
	var textures := {}

	for path in Document.texturePaths(doc.root):
		var image := _textureImage(path)

		if image != null:
			var cached: Dictionary = _imageCache[project.toAbs(path)]
			textures[path] = {"image": image, "palette": cached.palette, "palette_fps": cached.palette_fps}

	var models := {}

	for path in Document.modelPaths(doc.root):
		var data := _modelData(path)

		if not data.is_empty(): models[path] = data

	return {"textures": textures, "models": models}


# Everything that blocks an export: missing or unreadable files, missing model parts.
func _missingAssets(doc: Dictionary) -> Array:
	var missing := []

	for path in Document.texturePaths(doc.root):
		if _textureImage(path) == null: missing.append(path + " (PNG missing or unreadable)")

	var parts := {}

	for path in Document.modelPaths(doc.root):
		var info := _modelInfo(path)

		if info.is_empty(): missing.append(path + " (file missing)")
		elif not str(info.error).is_empty(): missing.append("%s (%s)" % [path, info.error])
		else: parts[path] = info.parts.map(func(part): return str(part.name))
	Document.walk(doc.root, func(n, _p, _d):
		var path := str(n.get("mesh_file", ""))
		if str(n.get("mesh", "")) == "File" and parts.has(path) and not str(n.get("mesh_part", "")) in parts[path]:
			missing.append("%s: part \"%s\" not found in %s" % [n.name, n.get("mesh_part", ""), path]))
	return missing


func _importSummary(path: String) -> String:
	if Document.importKind(path) == "texture":
		var original := Image.load_from_file(project.toAbs(path))
		var image := _textureImage(path)

		if original == null or image == null: return "Unreadable image (PNG only)."

		var info: Dictionary = bridge.texture_info(image, _importSettings(path).palette) if bridge else {}
		var text := "Original %d × %d px, imported %d × %d px." % [original.get_width(), original.get_height(), image.get_width(), image.get_height()]

		if not info.is_empty():
			text += "\n%s · %d colors · %s in memory." % ["8-bit palette" if info.indexed else "RGB565", int(info.colors),
				String.humanize_size(int(info.bytes))]

		return text

	var model := _modelInfo(path)

	if model.is_empty(): return "Unreadable file."

	if not str(model.error).is_empty(): return "Error: " + str(model.error)

	var triangles := 0

	for part in model.parts: triangles += int(part.triangles)

	var names: Array = model.parts.map(func(part): return str(part.name))
	return "%d parts, %d triangles, %d materials.\nParts: %s\nDrag the file into the scene to create one node per part." % [
		model.parts.size(), triangles, model.materials.size(), ", ".join(names)]


func _fileSelected(path: String):
	if Document.importKind(path).is_empty() or not FileAccess.file_exists(project.toAbs(path)): return

	#the import settings are in the Inspector
	rightTabs.current_tab = 0
	inspector.editImport(path, _importSettings(path), _importSummary(path))


func _saveImport(path: String, settings: Dictionary) -> bool:
	var error := Project.writeText(_importFile(path), JSON.stringify({"importer": Document.importKind(path), "params": settings}, "\t") + "\n")

	if not error.is_empty():
		_fail(error)
		return false

	_importCache[path] = {"mtime": _fileStamp(_importFile(path)), "settings": settings}
	return true


func _importApplied(path: String, settings: Dictionary):
	if not _saveImport(path, settings): return

	_log("Reimported: " + path, Style.ACCENT)
	_upload()
	_refreshAll()
	inspector.editImport(path, settings, _importSummary(path))


func _upload():
	if bridge == null or current < 0 or playing: return

	#editing needs only this scene
	bridge.set_scenes(_tab().path, {})
	var sources := _sceneSources(_tab().doc)
	sources.merge(_autoloadSources())
	var error: String = bridge.load_scene(_tab().doc, sources, _sceneAssets(_tab().doc))

	if not error.is_empty(): _log("Invalid scene: " + error, Style.ERROR)
	#as in Godot, an Inspector or gizmo edit shows the edited value until the time moves again
	if _repose and animation_panel.visible and animation_panel.playerId > 0 and not animation_panel.animation.is_empty():
		bridge.preview_animation(animation_panel.playerId, animation_panel.animation, animation_panel.time)

	_repose = false
	needsRender = true


func _refreshAll():
	if current < 0: return

	var tab := _tab()

	if Document.find(tab.doc.root, tab.selected).is_empty(): tab.selected = int(tab.doc.root.id)

	scene_dock.connections = Document.connections(tab.doc)
	scene_dock.rebuild(tab.doc.root, tab.selected, playing)
	animation_panel.read_only = playing
	animation_panel.refresh()
	_refreshInspector()
	viewport.setScene(tab.doc.root, tab.selected, playing)
	var names := []
	Document.walk(tab.doc.root, func(n, _p, _d): names.append(str(n.name)))
	script_editor.setSceneNodeNames(names)
	var connected := {}

	for c in Document.connections(tab.doc):
		var source := Document.find(tab.doc.root, int(c.from))
		var path := str(Document.find(tab.doc.root, int(c.to)).get("script", ""))

		if path.is_empty() or source.is_empty(): continue

		if not connected.has(path): connected[path] = {}

		connected[path][str(c.method)] = "%s.%s" % [source.name, c["signal"]]

	script_editor.setConnectedMethods(connected)
	_refreshTitle()
	_updateControls()
	needsRender = true


func _refreshInspector():
	if current < 0: return

	var node := Document.find(_rootNode(), _tab().selected)
	var info := _scriptInfo(str(node.get("script", "")))

	if inspector.visible: inspector.edit(node, info, playing)

	if signals_dock.visible: signals_dock.edit(_tab().doc, node, info, playing)


func _scriptInfo(path: String) -> Dictionary:
	if path.is_empty() or bridge == null: return {}

	var live: Dictionary = script_editor.infoOf(path)

	if not live.is_empty(): return live

	var file: String = project.toAbs(path)

	if not FileAccess.file_exists(file): return {"ok": false, "errors": [{"line": 0, "column": 0, "message": "File not found"}]}

	var mtime := FileAccess.get_modified_time(file)

	if _scriptCache.has(path) and _scriptCache[path].mtime == mtime: return _scriptCache[path].info

	var info: Dictionary = bridge.compile_script(FileAccess.get_file_as_string(file), path)
	_scriptCache[path] = {"mtime": mtime, "info": info}
	return info


func _select(id: int):
	if current < 0: return

	_tab().selected = id

	if str(Document.find(_rootNode(), id).get("type", "")) == "AnimationPlayer":
		animation_panel.edit(id)
		bottomTabs.current_tab = 3

	scene_dock.select(id)
	_refreshInspector()
	viewport.setScene(_rootNode(), id, playing)


func _setScreen(value: int):
	screen = value
	viewport.visible = value == Screen.VIEW_3D or value == Screen.VIEW_2D

	if viewport.visible: viewport.setMode2D(value == Screen.VIEW_2D)

	script_editor.visible = value == Screen.SCRIPT
	gameView.get_parent().get_parent().visible = value == Screen.GAME
	screenButtons[value].button_pressed = true

	if value == Screen.GAME: gameView.grab_focus()

	needsRender = true


func _updateControls():
	var busy := _jobId > 0
	playButton.disabled = playing or bridge == null
	playSceneButton.disabled = playing or bridge == null
	pauseButton.disabled = not playing
	pauseButton.button_pressed = paused
	stopButton.disabled = not playing
	stepButton.disabled = not playing or not paused
	buildButton.disabled = busy or playing
	flashButton.disabled = busy or playing or targetSelect.selected == 0
	targetSelect.disabled = busy
	modeLabel.text = "●  PAUSED" if paused else ("●  GAME" if playing else "●  SCENE")
	modeLabel.add_theme_color_override("font_color", Style.ORANGE if playing else Style.ACCENT)


# Edits with the same merge key (a drag, a colour picker) make one undo step until final is true.
func _change(mergeKey: String, final: bool, mutate: Callable, structural: bool = false):
	if playing or current < 0: return

	var tab := _tab()

	if mergeKey.is_empty() or mergeKey != _mergeKey:
		tab.undo.append(tab.doc.duplicate(true))

		if tab.undo.size() > 200: tab.undo.pop_front()

		tab.redo.clear()

	_mergeKey = "" if final else mergeKey
	mutate.call()
	_upload()

	if structural:
		_refreshAll()
	else:
		var node := Document.find(tab.doc.root, tab.selected)
		inspector.refreshValues(node)
		viewport.setScene(tab.doc.root, tab.selected, playing)
		_refreshTitle()


func _undo():
	if playing or current < 0 or _tab().undo.is_empty(): return

	var tab := _tab()
	tab.redo.append(tab.doc.duplicate(true))
	tab.doc = tab.undo.pop_back()
	_mergeKey = ""
	_repose = true
	_upload()
	_refreshAll()
	_status("Undo.")


func _redo():
	if playing or current < 0 or _tab().redo.is_empty(): return

	var tab := _tab()
	tab.undo.append(tab.doc.duplicate(true))
	tab.doc = tab.redo.pop_back()
	_mergeKey = ""
	_repose = true
	_upload()
	_refreshAll()
	_status("Redo.")


func _propertyEdited(key: String, value: Variant, final: bool):
	var id: int = _tab().selected
	_change("prop:%d:%s" % [id, key], final, func():
		var node := Document.find(_rootNode(), id)
		if node.is_empty(): return
		node[key] = value
		if key == "mesh_file":
			#keep the current part if the new model has it, otherwise use its first part.
			var names: Array = _modelInfo(str(value)).get("parts", []).map(func(part): return str(part.name))
			if not names.is_empty() and not str(node.get("mesh_part", "")) in names: node.mesh_part = names[0]
	, key in ["visible", "mesh", "current"] or key in Document.conditionKeys())


func _scriptPropertyEdited(varName: String, value: Variant, final: bool):
	var id: int = _tab().selected
	_change("script:%d:%s" % [id, varName], final, func():
		var node := Document.find(_rootNode(), id)
		if node.is_empty(): return
		if not node.has("script_properties") or not node.script_properties is Dictionary: node.script_properties = {}
		if value == null: node.script_properties.erase(varName)
		else: node.script_properties[varName] = value
	, value == null)


func _controlMoved(id: int, position: Vector2, final: bool):
	_change("move2d:%d" % id, final, func():
		var node := Document.find(_rootNode(), id)
		if not node.is_empty(): node.position = [position.x, position.y])


func _canvasEdited(id: int, values: Dictionary, final: bool):
	_change("canvas:%d" % id, final, func():
		var node := Document.find(_rootNode(), id)
		if node.is_empty(): return
		for key in values: node[key] = values[key])


func _transformEdited(id: int, local: Transform3D, final: bool):
	_change("gizmo:%d" % id, final, func():
		var node := Document.find(_rootNode(), id)
		if not node.is_empty(): Document.set_transform(node, local))


func _addNode(type: String, parentId: int):
	if current < 0: return

	var root := _rootNode()
	var parent := Document.find(root, parentId if parentId > 0 else int(root.id))

	if parent.is_empty(): parent = root

	if Document.count(root) >= 256:
		_fail("The runtime supports at most 256 nodes per scene.")
		return

	var id := Document.nextId(root)
	var node := Document.newNode(type, id, Document.uniqueName(parent, type))
	_change("", true, func():
		Document.children(Document.find(_rootNode(), int(parent.id))).append(node)
		_tab().selected = id
	, true)
	_status("Added %s under %s." % [type, parent.name])


func _renameNode(id: int, newName: String):
	var node := Document.find(_rootNode(), id)

	if node.is_empty() or newName == str(node.name):
		_refreshAll()
		return

	if not Document.validName(newName):
		_fail("Invalid name: \"%s\".\nDo not use . : @ / \" %%" % newName)
		_refreshAll()
		return

	var parent := Document.parentOf(_rootNode(), id)
	var finalName := Document.uniqueName(parent, newName, id) if not parent.is_empty() else newName
	_change("", true, func(): Document.find(_rootNode(), id).name = finalName, true)


func _requestDelete(id: int):
	if playing or current < 0 or id <= 0: return

	var node := Document.find(_rootNode(), id)

	if node.is_empty(): return

	if id == int(_rootNode().id):
		_fail("The root node cannot be deleted.")
		return

	_pendingDelete = id
	var descendants := Document.count(node) - 1
	deleteDialog.dialog_text = "Delete node \"%s\"%s?" % [node.name, (" and its %d descendants" % descendants) if descendants > 0 else ""]
	deleteDialog.popup_centered()


func _deleteNode(id: int):
	var parent := Document.parentOf(_rootNode(), id)

	if parent.is_empty(): return
	_change("", true, func():
		var p := Document.parentOf(_rootNode(), id)
		Document.children(p).erase(Document.find(_rootNode(), id))
		#as in Godot, the deleted nodes' connections go with them
		Document.pruneConnections(_tab().doc)
		_tab().selected = int(p.id)
	, true)

# the signal the Connect dialog is for: {signal, arguments}
var _connecting := {}


func _connectRequested(signalName: String, arguments: Array):
	if playing or current < 0: return

	_connecting = {"signal": signalName, "arguments": arguments}
	connect_dialog.open(_tab().doc, Document.find(_rootNode(), _tab().selected), signalName)


# Connects the selected node's signal; a missing method is added to the receiver's script, which opens there (Godot does the same).
func _addConnection(signalName: String, arguments: Array, targetId: int, method: String, flags: int):
	var fromId: int = _tab().selected

	if Document.findConnection(_tab().doc, fromId, signalName, targetId, method) >= 0: return
	_change("", true, func():
		Document.connections(_tab().doc).append({"from": fromId, "signal": signalName, "to": targetId, "method": method, "flags": flags})
	, true)
	var target := Document.find(_rootNode(), targetId)
	_status("Connected %s.%s to %s.%s()." % [Document.find(_rootNode(), fromId).name, signalName, target.name, method])
	var path := str(target.get("script", ""))

	if path.is_empty(): return

	var line := 0

	for f in _scriptInfo(path).get("functions", []):
		if str(f.name) == method: line = int(f.line)

	if line == 0: line = script_editor.addFunction(path, method, arguments)

	_openScriptAt(path, line)


func _removeConnection(index: int):
	if playing or current < 0 or index < 0 or index >= Document.connections(_tab().doc).size(): return

	var c: Dictionary = Document.connections(_tab().doc)[index]
	_change("", true, func(): Document.connections(_tab().doc).remove_at(index), true)
	_status("Disconnected %s from %s()." % [c["signal"], c.method])


func _gotoMethod(targetId: int, method: String):
	var path := str(Document.find(_rootNode(), targetId).get("script", ""))

	if path.is_empty(): return

	for f in _scriptInfo(path).get("functions", []):
		if str(f.name) == method:
			_openScriptAt(path, int(f.line))
			return

	_openScriptAt(path, 0)


# For the Signals dock: a connection's method is in the receiver's script, or is one of its engine methods.
func _methodExists(target: Dictionary, method: String) -> bool:
	if target.is_empty(): return false

	var path := str(target.get("script", ""))

	if not path.is_empty():
		for f in _scriptInfo(path).get("functions", []):
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


func _duplicateNode(id: int):
	if playing or current < 0 or id <= 0 or id == int(_rootNode().id): return

	var root := _rootNode()
	var node := Document.find(root, id)
	var parent := Document.parentOf(root, id)

	if node.is_empty() or parent.is_empty(): return

	if Document.count(root) + Document.count(node) > 256:
		_fail("The runtime supports at most 256 nodes per scene.")
		return

	var copy := Document.copyWithNewIds(node, Document.nextId(root))
	copy.name = Document.uniqueName(parent, str(node.name))
	#as in Godot, the copies keep the connections of the nodes they copy.
	var newIds := {}
	var old := []
	Document.walk(node, func(n, _p, _d): old.append(int(n.id)))
	var index := [0]
	Document.walk(copy, func(n, _p, _d):
		newIds[old[index[0]]] = int(n.id)
		index[0] += 1)
	_change("", true, func():
		var p := Document.parentOf(_rootNode(), id)
		var list := Document.children(p)
		list.insert(list.find(Document.find(_rootNode(), id)) + 1, copy)
		var links := Document.connections(_tab().doc)
		for c in links.duplicate():
			if newIds.has(int(c.from)):
				var twin: Dictionary = c.duplicate()
				twin.from = newIds[int(c.from)]
				twin.to = newIds.get(int(c.to), int(c.to))
				links.append(twin)
		_tab().selected = int(copy.id)
	, true)


func _moveNode(id: int, parentId: int, index: int):
	var root := _rootNode()

	if id == int(root.id) or Document.isAncestor(root, id, parentId) or id == parentId: return

	var node := Document.find(root, id)
	var newParent := Document.find(root, parentId)

	if node.is_empty() or newParent.is_empty(): return

	var global := Document.global_transform(root, id)
	_change("", true, func():
		var r := _rootNode()
		var n := Document.find(r, id)
		var oldParent := Document.parentOf(r, id)
		var target := Document.find(r, parentId)
		var oldList := Document.children(oldParent)
		var oldIndex := oldList.find(n)
		oldList.remove_at(oldIndex)
		var list := Document.children(target)
		var at := index
		if at < 0 or at > list.size(): at = list.size()
		if oldParent == target and oldIndex < at: at -= 1
		if int(oldParent.id) != int(target.id): n.name = Document.uniqueName(target, str(n.name), id)
		list.insert(clampi(at, 0, list.size()), n)
		#keep the global transform, as Godot does when reparenting.
		if Document.is3D(str(n.type)):
			var parentGlobal := Document.global_transform(r, int(target.id)) if Document.is3D(str(target.type)) else Transform3D.IDENTITY
			Document.set_transform(n, parentGlobal.affine_inverse() * global)
		_tab().selected = id
	, true)


func _reorderNode(id: int, delta: int):
	var parent := Document.parentOf(_rootNode(), id)

	if parent.is_empty(): return

	var list := Document.children(parent)
	var index := list.find(Document.find(_rootNode(), id))
	var target := index + delta

	if target < 0 or target >= list.size(): return
	_change("", true, func():
		var p := Document.parentOf(_rootNode(), id)
		var l := Document.children(p)
		var item = l.pop_at(index)
		l.insert(target, item)
	, true)


func _toggleVisibility(id: int):
	_change("", true, func():
		var node := Document.find(_rootNode(), id)
		node.visible = not bool(node.get("visible", true))
	, true)

func _scriptActionFor(id: int, action: String):
	if current < 0 or id <= 0: return

	var node := Document.find(_rootNode(), id)

	if node.is_empty(): return

	match action:
		"open":
			if not str(node.get("script", "")).is_empty(): _openScriptAt(str(node.script), 0)
		"attach":
			if playing: return

			var folder := "res://scripts" if project.exists("res://scripts") else "res://"
			attachDialog.open(str(node.type), folder.path_join(str(node.name).to_snake_case() + ".jet"))
		"load":
			if playing: return

			_quickOpen("script", func(path): _attachScript(id, path))
		"detach":
			if playing: return
			_change("", true, func():
				var n := Document.find(_rootNode(), id)
				n.erase("script")
				n.erase("script_properties")
			, true)


func _onAttachChosen(path: String, create: bool, base: String, template: bool):
	var id: int = _tab().selected

	if create:
		var text: String = script_editor.newScriptText(base) if template else "extends %s\n" % base
		var error: String = project.write(path, text)

		if not error.is_empty():
			_fail(error)
			return

		filesystem.refresh()
		_log("Script created: " + path, Style.ACCENT)

	_attachScript(id, path)
	_openScriptAt(path, 0)


func _fileDropped(id: int, path: String):
	if playing or current < 0: return

	if path.get_extension() == "jet":
		_attachScript(id, path)
		return

	if Document.importKind(path) == "model":
		_instantiateModel(id, path)
		return

	if not path.get_extension().to_lower() in Document.IMAGE_EXTENSIONS: return

	var node := Document.find(_rootNode(), id)

	if node.is_empty(): return

	match str(node.type):
		"MeshInstance3D":
			_change("", true, func():
				var n := Document.find(_rootNode(), id)
				n.albedo_texture = path
				#Godot's default albedo is white, so the texture shows its own colours.
				if int(n.get("color", 0)) == int(Document.default_value("MeshInstance3D", "color")): n.color = 0xffffff
			, true)
			_status("Texture %s applied to %s." % [path.get_file(), node.name])
		"WorldEnvironment":
			_change("", true, func():
				var n := Document.find(_rootNode(), id)
				n.sky_texture = path
				n.background_mode = "Panorama"
			, true)
			_status("Panorama %s set." % path.get_file())
		"TextureRect", "Sprite3D", "Sprite2D", "AnimatedSprite2D":
			_change("", true, func(): Document.find(_rootNode(), id).texture = path, true)
			_status("Texture %s applied to %s." % [path.get_file(), node.name])
		_:
			_status("Drag images onto a MeshInstance3D or a WorldEnvironment.")

	_select(id)

	if str(node.type) in ["MeshInstance3D", "WorldEnvironment", "TextureRect", "Sprite3D", "Sprite2D", "AnimatedSprite2D"]: _offerCompression([path])


# A Node3D with one MeshInstance3D per OBJ part and the MTL materials, like Godot's imported scenes.
func _instantiateModel(parentId: int, path: String, at: Variant = null):
	if playing or current < 0: return

	var info := _modelInfo(path)

	if info.is_empty():
		_fail("Unreadable model: " + path)
		return

	if not str(info.error).is_empty():
		_fail("Import of %s failed:\n%s\nCheck the import settings (select the file in the FileSystem dock)." % [path, info.error])
		return

	var root := _rootNode()
	var parent := Document.find(root, parentId)

	if parent.is_empty() or Document.isControl(str(parent.type)): parent = root

	if Document.count(root) + info.parts.size() + 1 > 256:
		_fail("The model has %d parts: the scene would exceed 256 nodes." % info.parts.size())
		return

	var materials := {}

	for material in info.materials: materials[str(material.name)] = material

	var mtl_file: String = _modelCache[path].mtl_file
	var textureDir: String = project.toRes(mtl_file.get_base_dir()) if not mtl_file.is_empty() else path.get_base_dir()
	var next := Document.nextId(root)
	var group := Document.newNode("Node3D", next, Document.uniqueName(parent, Document.sanitizeName(path.get_file().get_basename())))

	if at is Vector3: group.position = [at.x, at.y, at.z]

	var notes := []

	for part in info.parts:
		next += 1
		var mesh := Document.newNode("MeshInstance3D", next, Document.uniqueName(group, str(part.name)))
		mesh.mesh = "File"
		mesh.mesh_file = path
		mesh.mesh_part = str(part.name)
		var material: Dictionary = materials.get(str(part.material), {})
		mesh.color = int(material.get("color", 0xffffff))
		var texture := str(material.get("texture", "")).replace("\\", "/")

		if not texture.is_empty():
			var candidate := textureDir.path_join(texture)

			if candidate.get_extension().to_lower() in Document.IMAGE_EXTENSIONS and project.exists(candidate): mesh.albedo_texture = candidate
			elif not texture in notes: notes.append(texture)

		if float(material.get("alpha", 1.0)) < 1.0: mesh.alpha = float(material.alpha)

		if float(material.get("shininess", 0.0)) > 1.0:
			mesh.shading = "Phong"
			mesh.shininess = clampi(int(material.shininess), 1, 255)

		Document.children(group).append(mesh)
	_change("", true, func():
		Document.children(Document.find(_rootNode(), int(parent.id))).append(group)
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

	_offerCompression(textures)


func _attachScript(id: int, path: String):
	if playing: return

	var node := Document.find(_rootNode(), id)

	if node.is_empty(): return

	var info := _scriptInfo(path)
	var base := str(info.get("base", ""))

	if not base.is_empty() and not Document.inherits(str(node.type), base):
		_fail("Script %s extends %s: it cannot be attached to a %s node." % [path, base, node.type])
		return
	_change("", true, func():
		var n := Document.find(_rootNode(), id)
		n.script = path
		if not n.has("script_properties"): n.script_properties = {}
	, true)
	_status("Script %s attached to %s." % [path, node.name])


func _newScriptFile(folder: String):
	var base := "Node3D"
	var suggested := "new_script"

	if current >= 0:
		var node := Document.find(_rootNode(), _tab().selected)

		if not node.is_empty():
			base = str(node.type)
			suggested = str(node.name).to_snake_case()

	if not project.exists(folder): folder = "res://"

	attachDialog.open(base, folder.path_join(suggested + ".jet"))


func _openScriptAt(path: String, line: int):
	if script_editor.openScript(path, line): _setScreen(Screen.SCRIPT)


func _fileMoved(oldPath: String, newPath: String):
	script_editor.fileMoved(oldPath, newPath)
	var changed := [0]

	for tab in tabs:
		Document.walk(tab.doc.root, func(n, _p, _d):
			for key in Document.PATH_KEYS:
				var path := str(n.get(key, ""))
				if path.is_empty(): continue
				if path == oldPath or path.begins_with(oldPath + "/"):
					n[key] = newPath + path.trim_prefix(oldPath)
					changed[0] += 1)
		if tab.path == oldPath or tab.path.begins_with(oldPath + "/"): tab.path = newPath + tab.path.trim_prefix(oldPath)

	if project.main_scene() == oldPath:
		project.config.main_scene = newPath
		project.save()

	_log("Renamed %s → %s%s" % [oldPath, newPath, (" (updated %d references in the open scenes: save them)" % changed[0]) if changed[0] > 0 else ""])
	_upload()
	_refreshAll()

func _quickOpen(kind: String, action: Callable = Callable()):
	var extension := {"scene": "jetscene", "script": "jet", "texture": "png", "model": "obj"}.get(kind, "jet")

	if action.is_valid():
		_quickAction = action
	elif kind == "scene":
		_quickAction = func(path): _openScene(path)
	else:
		_quickAction = func(path): _openScriptAt(path, 0)

	quickDialog.title = {"scene": "Open Scene", "script": "Choose a Script", "texture": "Choose a Texture (PNG)",
		"model": "Choose a Model (OBJ)"}.get(kind, "Open")
	quickFilter.text = ""
	var files := []
	var stack: Array = [project.scan()]

	while not stack.is_empty():
		var entry: Dictionary = stack.pop_back()

		for child in entry.get("children", []):
			if child.dir: stack.append(child)
			elif str(child.path).get_extension() == extension: files.append(child.path)

	files.sort()
	var fill := func():
		quickList.clear()
		var text := quickFilter.text.strip_edges().to_lower()

		for path in files:
			if text.is_empty() or text in path.to_lower():
				var i := quickList.add_item(path, Icons.forFile(path))
				quickList.set_item_metadata(i, path)

		if quickList.item_count > 0: quickList.select(0)

	for connection in quickFilter.text_changed.get_connections(): quickFilter.text_changed.disconnect(connection.callable)

	quickFilter.text_changed.connect(func(_t): fill.call())
	fill.call()
	quickDialog.popup_centered(Vector2i(520, 420))
	quickFilter.grab_focus()


func _quickConfirm():
	var selected := quickList.get_selected_items()

	if selected.is_empty() or not _quickAction.is_valid(): return

	_quickAction.call(str(quickList.get_item_metadata(selected[0])))

func _inputMask() -> int:
	if screen != Screen.GAME or not get_window().has_focus(): return 0

	var focus := get_viewport().gui_get_focus_owner()

	if focus is LineEdit or focus is TextEdit: return 0
	#bit i = Input Map action i
	var mask := 0

	for i in _actionKeys.size():
		for code in _actionKeys[i]:
			if Input.is_key_pressed(code):
				mask |= 1 << i
				break

	return mask


func _sceneDocFor(path: String) -> Dictionary:
	for tab in tabs:
		if tab.path == path: return tab.doc

	var data = JSON.parse_string(project.read(path))

	if not data is Dictionary: return {}

	if int(data.get("version", 0)) == 1: data = Document.migrateV1(data)[0]

	if not Document.validate(data).is_empty(): return {}

	Document.normalize(data)
	return data


func _play(current_scene: bool):
	if bridge == null or playing or current < 0: return

	script_editor.saveAll()
	var doc: Dictionary = _tab().doc
	var title := _tabTitle(_tab()).trim_suffix("(*)")

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

		doc = _sceneDocFor(main)
		title = main.get_file().get_basename()

		if doc.is_empty():
			_fail("Cannot read the main scene " + main)
			return

	var path: String = _tab().path if current_scene else project.main_scene()
	var others := _otherScenes(path)
	bridge.set_scenes(path, others)
	var error: String = bridge.load_scene(doc, _allSources(doc, others), _allAssets(doc, others))

	if not error.is_empty():
		_fail("Invalid scene: " + error)
		_upload()
		return

	var errors: Array = bridge.play_start()

	if not errors.is_empty():
		errorsList.clear()

		for e in errors:
			_logLink("Compile error: " + str(e.message), str(e.path), int(e.line), Style.ERROR)
			_debugError(str(e.message), str(e.path), int(e.line))

		bottomTabs.current_tab = 1
		_openScriptAt(str(errors[0].path), int(errors[0].line))
		_upload()
		return

	playing = true
	paused = false
	playTitle = title
	errorsList.clear()
	_updateDebuggerTab()
	script_editor.clearRuntimeError()
	_log("▶ Running scene \"%s\" · C++ runtime + JetScript · changes made while playing are temporary" % title, Style.ACCENT)
	#Godot's configuration warnings, so a body without shapes is not a mystery.
	Document.walk(doc.root, func(n, _p, _d):
		for warning in Document.configurationWarnings(doc.root, n):
			_log("⚠ %s (%s): %s" % [n.name, n.type, warning.replace("\n", " ")], Style.WARNING))
	_setScreen(Screen.GAME)
	get_viewport().gui_release_focus()
	gameLabel.text = "▶ %s  ·  arrows/WASD, Space/Enter, Esc  ·  F7 pause · F8 stop" % title
	_drainLog()
	_refreshAll()


func _togglePause():
	if not playing: return

	paused = not paused
	_log("Ⅱ Paused" if paused else "▶ Resumed")
	_updateControls()


func _step():
	if not playing or not paused: return

	bridge.set_input(_inputMask())
	bridge.update(1.0 / 60.0)
	_drainLog()
	needsRender = true


func _stop():
	if not playing: return

	bridge.play_stop()
	_drainLog()
	playing = false
	paused = false
	script_editor.clearRuntimeError()
	gameLabel.text = "Game stopped. Press Play (F5) to run it."
	_log("■ Stop · scene state restored")

	if screen == Screen.GAME: _setScreen(Screen.VIEW_3D)

	_upload()
	_refreshAll()


func _drainLog():
	for line in bridge.take_log():
		var text := str(line.text)

		if line.error:
			if not str(line.path).is_empty():
				_logLink(text, str(line.path), int(line.line), Style.ERROR)
				#set by runtime/runtime.cpp
				const prefix := "Script error"

				if text.begins_with(prefix): _debugError(text.trim_prefix(prefix + ": "), str(line.path), int(line.line))

				if pauseOnError and playing and not paused and text.begins_with(prefix):
					paused = true
					_updateControls()
					bottomTabs.current_tab = 1
					script_editor.showRuntimeError(str(line.path), int(line.line), text.trim_prefix(prefix + ": "))
					_setScreen(Screen.SCRIPT)
					_log("Ⅱ Game paused on the error (F7 to continue, F8 to stop).", Style.WARNING)
			else:
				_log(text, Style.ERROR)
		else:
			_log(text, Style.TEXT)


func _process(delta: float):
	if bridge == null: return

	if playing and not paused:
		bridge.set_input(_inputMask())
		bridge.update(delta)
		_drainLog()
		needsRender = true
		_liveTimer += delta

		if _liveTimer > 0.1:
			_liveTimer = 0.0
			_showLiveValues()

	if needsRender and screen != Screen.SCRIPT: _render()

	if _jobId > 0:
		_jobPoll += delta

		if _jobPoll > 0.25:
			_jobPoll = 0.0
			_pollTask()

	if not _screenshot.is_empty() and not _smoke and current >= 0:
		var path := _screenshot
		_screenshot = ""
		_captureAndQuit.call_deferred(path)


func _render():
	needsRender = false
	var game := playing and screen == Screen.GAME
	bridge.use_editor_camera(not game)
	bridge.set_editor_canvas(screen == Screen.VIEW_2D, viewport.canvas_zoom, viewport.canvas_origin)

	if not game: bridge.set_editor_camera(viewport.viewOrigin, viewport.viewRotation(), viewport.viewFov)

	var texture: Texture2D = bridge.render_frame()

	if game: gameView.texture = texture
	else: viewport.set_frame(texture)
	var stats: Dictionary = bridge.statistics()
	var text := "%d nodes · %d meshes · %d triangles · render %.2f ms" % [stats.nodes, stats.meshes, stats.triangles, stats.render_ms]

	if playing: text += " · script %.2f ms" % stats.script_ms

	if int(stats.get("particles", 0)) > 0: text += " · %d particles" % int(stats.particles)

	statsLabel.text = text
	viewport.stats.text = "%d triangles" % stats.triangles


func _showLiveValues():
	if current < 0: return

	var node := Document.find(_rootNode(), _tab().selected)

	if node.is_empty(): return

	var live: Dictionary = bridge.node_state(int(node.id))

	if live.is_empty(): return

	var shown := node.duplicate()

	for key in ["position", "rotation", "scale"]:
		if live.has(key) and Document.is3D(str(node.type)):
			var v: Vector3 = live[key]
			shown[key] = [v.x, v.y, v.z]

	if live.has("position") and (Document.isControl(str(node.type)) or Document.is2D(str(node.type))):
		shown.position = [live.position.x, live.position.y]

	if Document.is2D(str(node.type)):
		if live.has("rotation"): shown.rotation_degrees = live.rotation.z

		if live.has("scale"): shown.scale = [live.scale.x, live.scale.y]

		if live.has("frame"): shown.frame = live.frame

	if live.has("visible"): shown.visible = live.visible

	inspector.refreshValues(shown)

func _scenePack(path: String) -> PackedByteArray:
	var doc := _sceneDocFor(path)

	if doc.is_empty():
		_fail("Scene not found or invalid: " + path)
		return PackedByteArray()

	var others := _otherScenes(path)
	var missing := _missingAssets(doc)

	for other in others.values(): missing.append_array(_missingAssets(other))

	if not missing.is_empty():
		_fail("Missing or invalid resources:\n" + "\n".join(missing))
		return PackedByteArray()

	#the package starts with the "Made with Jet Studio" scene, which then loads the main scene.
	var start := path
	var splash := bool(project.config.get("boot_splash", true))

	if splash:
		others[path] = doc
		doc = _bootSplashDoc(path)
		start = BOOT_SPLASH

	var sources := _allSources(doc, others)

	if splash: sources[BOOT_SPLASH_SCRIPT] = _bootSplashScript(path)

	var failed := false

	for scriptPath in sources:
		var info: Dictionary = bridge.compile_script(sources[scriptPath], scriptPath)

		for e in info.get("errors", []):
			_logLink("Compile error: " + str(e.message), scriptPath, int(e.line), Style.ERROR)
			_debugError(str(e.message), scriptPath, int(e.line))
			failed = true

	if failed:
		bottomTabs.current_tab = 1
		return PackedByteArray()

	var assets := _allAssets(doc, others)
	_packTextures = assets.textures.keys()

	if splash: assets.textures[BOOT_SPLASH_LOGO] = {"image": _bootSplashLogo(), "palette": "Auto"}

	bridge.set_scenes(start, others)
	var error: String = bridge.load_scene(doc, sources, assets)
	var pack: PackedByteArray = bridge.export_pack() if error.is_empty() else PackedByteArray()

	if not error.is_empty(): _fail("Invalid scene: " + error)
	elif not pack.is_empty():
		#the STM32 firmware reads palette textures from the flash instead of copying them.
		_packMemory = bridge.memory_estimate(Hardware.platform(Hardware.settings(project.config)) == "stm32")

	_upload()
	return pack


func _mainSceneOrCurrent() -> String:
	var main: String = project.main_scene()

	if not main.is_empty() and project.exists(main): return main

	if current >= 0 and not _tab().path.is_empty(): return _tab().path

	return ""


func _exportTo(path: String) -> bool:
	var scene := _mainSceneOrCurrent()

	if scene.is_empty():
		_fail("Save the scene before exporting it.")
		return false

	script_editor.saveAll()
	var pack := _scenePack(scene)

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


# device runs the emulator: the player built with the firmware's Jet config (half-width fields, no z-buffer).
func _externalPlayer(device: bool):
	var executable := _rootDir().path_join("build/jet_player_device" if device else "build/jet_player")

	if OS.get_name() == "Windows": executable += ".exe"

	if not FileAccess.file_exists(executable):
		_fail("%s missing. Select the PC target and press Build." % ("Device emulator" if device else "Desktop player"))
		return

	var pack := _rootDir().path_join(".cache/jobs/player.jetpack")

	if not _exportTo(pack): return

	if OS.create_process(executable, PackedStringArray([pack])) < 0: _fail("Failed to start the player.")
	elif device: _log("Device emulator started: shows the firmware rendering (half-width fields, sorting without z-buffer) · P: pause · Esc: quit", Style.ACCENT)
	else: _log("External C++ player started · P: pause · Esc: quit", Style.ACCENT)


func _startTask(action: String):
	if _jobId > 0 or playing: return

	var folder := _rootDir().path_join(".cache/jobs")
	DirAccess.make_dir_recursive_absolute(folder)
	_jobPrefix = folder.path_join(str(Time.get_ticks_msec()))

	if not _exportTo(_jobPrefix + ".jetpack"): return

	_jobOffset = 0
	var target := "device" if targetSelect.selected > 0 else "desktop"

	if target == "device" and not _fitsBoard(_jobPrefix + ".jetpack", action): return

	if target == "device": _reportMemory()

	var profile := _profile()
	#the uploader needs the USB port; the Serial monitor gets it back after the flash.
	if action == "flash": _serialWasOpen = serial_monitor.suspend()

	_jobAction = action
	_jobId = OS.create_process(_python(), PackedStringArray([_rootDir().path_join("tools/tasks.py"), action, "--target", target,
		"--profile", profile, "--pack", _jobPrefix + ".jetpack", "--project", project.root, "--job", _jobPrefix]))

	if _jobId < 0:
		_fail("Cannot start Python for the build. Install Python 3 and PlatformIO Core (pip install platformio).")
		_jobFinished(false)
		return

	_log("%s · %s%s · process %d" % [action.to_upper(), target, (" · " + profile) if target == "device" else "", _jobId], Style.ACCENT)
	bottomTabs.current_tab = 0
	_updateControls()


func _pollTask():
	var logPath := _jobPrefix + ".log"

	if FileAccess.file_exists(logPath):
		var file := FileAccess.open(logPath, FileAccess.READ)
		file.seek(_jobOffset)
		var chunk := file.get_buffer(file.get_length() - _jobOffset).get_string_from_utf8()
		_jobOffset = file.get_position()

		if not chunk.is_empty(): _log(chunk.trim_suffix("\n"))

	if FileAccess.file_exists(_jobPrefix + ".json"):
		var result: Variant = JSON.parse_string(FileAccess.get_file_as_string(_jobPrefix + ".json"))
		var ok: bool = result is Dictionary and int(result.get("returncode", -1)) == 0

		if ok: _log("Task completed.", Style.SUCCESS)
		else: _log("Task failed: check the output above.", Style.ERROR)
		_jobId = -1
		_jobFinished(ok)
		_updateControls()
	elif not OS.is_process_running(_jobId):
		_log("Process ended without a result: check the log.", Style.ERROR)
		_jobId = -1
		_jobFinished(false)
		_updateControls()


# Backtrace addresses -> [[function, file:line], ...] via addr2line on the Debug ELF. Only ESP32 prints backtraces.
func _decodeFirmwareAddresses(addresses: PackedStringArray) -> Array:
	var hw := Hardware.settings(project.config) if project != null else {}

	if hw.is_empty() or Hardware.platform(hw) != "esp32": return []

	var elf := _rootDir().path_join("firmware/.pio/build/%s/firmware.elf" % hw.board)
	var tool := ""
	var windows := OS.get_name() == "Windows"
	var packages := OS.get_environment("USERPROFILE" if windows else "HOME").path_join(".platformio/packages")

	for toolchain in ["toolchain-xtensa-esp-elf", "toolchain-xtensa-esp32s3"]:
		var candidate := packages.path_join(toolchain).path_join("bin/xtensa-esp32s3-elf-addr2line" + (".exe" if windows else ""))

		if FileAccess.file_exists(candidate):
			tool = candidate
			break

	if tool.is_empty() or not FileAccess.file_exists(elf): return []

	var output := []

	if OS.execute(tool, PackedStringArray(["-pfiC", "-e", elf]) + addresses, output, true) != 0 or output.is_empty(): return []

	var frames := []
	var root := _rootDir() + "/"
	var framework := RegEx.create_from_string("\\S*/framework-espidf[^/]*/")

	for line in str(output[0]).split("\n", false):
		var inlined := line.strip_edges().begins_with("(inlined by)")
		var text := line.strip_edges().trim_prefix("(inlined by) ")
		var at := text.rfind(" at ")
		var function := _shortFunction(text.substr(0, at) if at > 0 else text)
		var location := text.substr(at + 4).replace(root, "") if at > 0 else ""
		location = framework.sub(location, "esp-idf/")
		frames.append([("   inline " if inlined else "") + function, location if location != "??:0" and location != "??:?" else "?"])

	return frames


# "ns::f<int>(Args const&) const" -> "ns::f() const"
static func _shortFunction(name: String) -> String:
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
	return "release" if targetSelect.selected == 2 else "debug"


# The Serial monitor reconnects after a flash; with a Debug firmware it also opens to show the boot log.
func _jobFinished(ok: bool):
	if _jobAction != "flash": return

	var show := ok and _profile() == "debug"
	serial_monitor.resume(_serialWasOpen or show)

	if show: bottomTabs.current_tab = 2
	elif ok: _log("Release firmware: no log on the serial port.", Style.MUTED)

func _input(event: InputEvent):
	if _isGameKey(event): get_viewport().set_input_as_handled()


func _isGameKey(event: InputEvent) -> bool:
	if not playing or screen != Screen.GAME or not event is InputEventKey: return false

	var focus := get_viewport().gui_get_focus_owner()

	if focus is LineEdit or focus is TextEdit: return false

	for keys in _actionKeys:
		if event.keycode in keys: return true

	return false


func _shortcut_input(event: InputEvent):
	if not event is InputEventKey or not event.pressed or event.echo: return

	var key: int = event.keycode
	var ctrl: bool = event.ctrl_pressed or event.meta_pressed
	var handled := true

	if key == KEY_F5 and not ctrl: _play(false)
	elif key == KEY_F6 and not ctrl: _play(true)
	elif key == KEY_F7 and not ctrl: _togglePause()
	elif key == KEY_F8 and not ctrl: _stop()
	elif ctrl and key == KEY_F1: _setScreen(Screen.VIEW_2D)
	elif ctrl and key == KEY_F2: _setScreen(Screen.VIEW_3D)
	elif ctrl and key == KEY_F3: _setScreen(Screen.SCRIPT)
	elif ctrl and key == KEY_F4: _setScreen(Screen.GAME)
	elif ctrl and event.shift_pressed and event.alt_pressed and key == KEY_S: _saveAll()
	elif ctrl and event.shift_pressed and key == KEY_S: _saveSceneAs()
	elif ctrl and key == KEY_S:
		if screen == Screen.SCRIPT and not script_editor.current_path().is_empty(): script_editor.save()
		else: _saveScene()
	elif ctrl and key == KEY_N: _newScene()
	elif ctrl and key == KEY_O: _quickOpen("scene")
	elif ctrl and key == KEY_Q: _requestQuit()
	elif ctrl and event.shift_pressed and key == KEY_W: _closeTab(current)
	elif screen == Screen.SCRIPT and ctrl and key == KEY_W: script_editor.closeScript(script_editor.current_path())
	elif screen == Screen.SCRIPT and ctrl and key == KEY_F: script_editor.showFind()
	elif screen == Screen.SCRIPT and ctrl and key == KEY_L: script_editor._onSearchMenu(3)
	elif screen == Screen.SCRIPT and ctrl and key == KEY_K: script_editor.toggleComment()
	else: handled = false

	if handled: get_viewport().set_input_as_handled()


func _unhandled_key_input(event: InputEvent):
	if not event is InputEventKey or not event.pressed: return

	var ctrl: bool = event.ctrl_pressed or event.meta_pressed

	if ctrl and event.keycode == KEY_Z:
		if event.shift_pressed: _redo()
		else: _undo()
	elif ctrl and event.keycode == KEY_Y: _redo()
	elif ctrl and event.keycode == KEY_A and screen in [Screen.VIEW_2D, Screen.VIEW_3D] and current >= 0:
		_openCreate("add", _tab().selected)
	else: return
	get_viewport().set_input_as_handled()


func _notification(what: int):
	if what == NOTIFICATION_WM_CLOSE_REQUEST: _requestQuit()
	elif what == NOTIFICATION_APPLICATION_FOCUS_IN and script_editor != null and not project.root.is_empty():
		script_editor.checkExternalChanges()
		_scriptCache.clear()

		if current >= 0 and not playing:
			_upload()
			_refreshInspector()


func _requestQuit():
	var unsaved := script_editor.hasUnsaved()

	for tab in tabs:
		if _dirty(tab): unsaved = true

	if unsaved:
		quitDialog.dialog_text = "Some scenes or scripts have unsaved changes.\nSave before quitting?"
		quitDialog.popup_centered()
	else:
		_quit()


func _quit():
	if playing: _stop()

	_saveState()
	get_tree().quit()


func _captureAndQuit(path: String):
	#test-only arguments that set up the state to capture.
	for arg in OS.get_cmdline_user_args():
		if arg.begins_with("--select="): _select(int(arg.trim_prefix("--select=")))
		#path, or path:line
		elif arg.begins_with("--open-script="):
			var target := arg.trim_prefix("--open-script=")
			var line := target.get_slice(":", target.get_slice_count(":") - 1)

			if line.is_valid_int(): _openScriptAt(target.trim_suffix(":" + line), int(line))
			else: _openScriptAt(target, 0)
		elif arg.begins_with("--inspect-file="): _fileSelected(arg.trim_prefix("--inspect-file="))
		elif arg.begins_with("--bottom-tab="): bottomTabs.current_tab = int(arg.trim_prefix("--bottom-tab="))
		elif arg.begins_with("--right-tab="): rightTabs.current_tab = int(arg.trim_prefix("--right-tab="))
		elif arg.begins_with("--dialog=connect:"): _connectRequested(arg.trim_prefix("--dialog=connect:"), [])
		elif arg == "--serial-sample":
			for line in ["I (412) JetStudio: Scene loaded: 104 nodes, 92 meshes, 1 scripts; 320x240 RGB565; buffers 307200 bytes",
					"I (2911) JetStudio: Frame 38.20 ms (scripts 1.10 ms); 362 raster tris; free heap 241388, PSRAM 7612044",
					"\u001b[0;33mW (3100) JetStudio: texture res://car/outrun.png: non power of two\u001b[0m",
					"\u001b[0;31mE (3150) JetStudio: res://scripts/orbit.jet:12: Invalid operands \"Vector3\" and \"int\" for \"+\" operator.\u001b[0m",
					"I (5390) JetStudio: Frame 39.00 ms (scripts 1.20 ms); 360 raster tris; free heap 241200, PSRAM 7612044",
					"\u001b[0;31mE (30763) task_wdt: Task watchdog got triggered.\u001b[0m",
					"Backtrace: 0x4038085A:0x3FC9ED40 0x40381DAD:0x3FC9ED60 0x4037DE71:0x3FCB93E0 0x4037FF32:0x3FCB96A0 0x4203671B:0x3FCB96E0 0x42037278:0x3FCB9840 0x42005F05:0x3FCB9860 0x42001B71:0x3FCB9880"]:
				serial_monitor.feed((line + "\r\n").to_utf8_buffer())
		elif arg.begins_with("--screen="): _setScreen(int(arg.trim_prefix("--screen=")))
		elif arg.begins_with("--tool="): viewport._setTool(int(arg.trim_prefix("--tool=")))
		elif arg == "--play": _play(false)
		elif arg == "--dialog=create": _openCreate("add", _tab().selected)
		elif arg == "--dialog=new-scene": _newScene()
		elif arg == "--dialog=change-type":
			var node := Document.find(_rootNode(), _tab().selected)
			_openCreate("change", int(node.id), "Change Type of %s" % node.name, "Change", str(node.type))
		elif arg == "--dialog=attach": _scriptActionFor(_tab().selected, "attach")
		elif arg == "--dialog=projects": _showProjectManager()
		elif arg == "--dialog=help": helpDialog.popup_centered(Vector2i(640, 520))
		elif arg == "--dialog=settings": _showProjectSettings()
		elif arg == "--dialog=hardware": projectSettings.open(project, 1)
		elif arg == "--dialog=input": projectSettings.open(project, 2)
		elif arg == "--dialog=autoload": projectSettings.open(project, 3)

	for i in 10: await get_tree().process_frame

	await _capture(path)
	get_tree().quit()


func _capture(path: String):
	await RenderingServer.frame_post_draw
	await RenderingServer.frame_post_draw
	var error := get_viewport().get_texture().get_image().save_png(path)

	if error != OK: push_error("Screenshot not saved: " + str(error))

var _smokeFailed := false


func _check(condition: bool, message: String):
	if OS.has_environment("JET_SMOKE_VERBOSE"): print("  check: ", message, " -> ", condition)

	if condition: return

	_smokeFailed = true
	printerr("SMOKE FAIL: " + message)


# The LineEdit a Tree edits its cells with (an internal child).
func _internalLineEdit(node: Node) -> LineEdit:
	for child in node.get_children(true):
		if child is LineEdit: return child

		var found := _internalLineEdit(child)

		if found != null: return found

	return null


func _frame() -> PackedByteArray:
	_render()
	return bridge.frame_bytes()


func _removeTree(folder: String):
	var dir := DirAccess.open(folder)

	if dir == null: return

	for sub in dir.get_directories(): _removeTree(folder.path_join(sub))

	for file in dir.get_files(): DirAccess.remove_absolute(folder.path_join(file))

	DirAccess.remove_absolute(folder)


# End-to-end check: Godot UI -> GDExtension -> runtime -> Jet.
func _runSmoke():
	var folder := _rootDir().path_join(".cache/tests/project")
	_removeTree(folder)
	_createProject(folder, "Smoke")
	_check(project.root == folder and tabs.size() == 1, "new project opens its main scene")
	var root := _rootNode()
	_check(Document.count(root) == 6, "starter scene has 6 nodes")
	var blank := _frame()
	_check(blank.size() == 320 * 240 * 3, "frame size")
	#add / undo / redo
	_addNode("Node3D", int(root.id))
	_check(Document.count(_rootNode()) == 7, "add node")
	_undo()
	_check(Document.count(_rootNode()) == 6, "undo add")
	_redo()
	_check(Document.count(_rootNode()) == 7, "redo add")
	_undo()
	#inspector edit and undo
	_select(6)
	var before := _frame()
	_propertyEdited("position", [1.5, 0.5, 0.0], true)
	_check(_frame() != before, "inspector edit changes the frame")
	_undo()
	_check(_frame() == before, "undo restores the frame")
	#View > Display Wireframe
	viewport.setWireframe(true)
	var wires := _frame()
	viewport.setWireframe(false)
	_check(wires != before and _frame() == before, "Display Wireframe draws the edges and switches back")
	#gizmo drag
	var steps: int = _tab().undo.size()
	var t := Document.localTransform(Document.find(_rootNode(), 6))
	_transformEdited(6, t.translated(Vector3(0.2, 0, 0)), false)
	_transformEdited(6, t.translated(Vector3(0.4, 0, 0)), false)
	_transformEdited(6, t.translated(Vector3(0.6, 0, 0)), true)
	_check(_tab().undo.size() == steps + 1, "gizmo drag merges into one undo step")
	_check(is_equal_approx(float(Document.find(_rootNode(), 6).position[0]), 0.6), "gizmo drag applies the last value")
	_undo()
	#reparenting
	_addNode("Node3D", int(_rootNode().id))
	var pivotId := Document.nextId(_rootNode()) - 1
	_change("", true, func():
		var pivot := Document.find(_rootNode(), pivotId)
		pivot.position = [2.0, 0.0, 1.0]
		pivot.rotation = [0.0, 45.0, 0.0])
	var globalBefore := Document.global_transform(_rootNode(), 6)
	_moveNode(6, pivotId, -1)
	_check(int(Document.parentOf(_rootNode(), 6).id) == pivotId, "cube reparented under the pivot")
	var globalAfter := Document.global_transform(_rootNode(), 6)
	_check(globalAfter.origin.is_equal_approx(globalBefore.origin), "reparent keeps the global position")

	for i in 3: _undo()

	_check(Document.count(_rootNode()) == 6 and int(Document.parentOf(_rootNode(), 6).id) == int(_rootNode().id), "undo reparent, edit and add")
	#scene dock
	_renameNode(6, "Ground")
	_check(str(Document.find(_rootNode(), 6).name) == "Ground2", "rename keeps sibling names unique")
	_renameNode(6, "bad/name")
	_check(str(Document.find(_rootNode(), 6).name) == "Ground2", "invalid names are rejected")
	errorDialog.hide()
	#rename in the Scene dock through the Tree's own line editor: the tree is rebuilt only after its signal (Windows crash, PR #2)
	scene_dock.select(6)
	scene_dock._beginRename()
	await get_tree().process_frame
	var lineEdit := _internalLineEdit(scene_dock.tree)

	if lineEdit != null: lineEdit.text_submitted.emit("Docked")

	_check(lineEdit != null and str(Document.find(_rootNode(), 6).name) == "Ground2", "the Scene dock waits for the Tree's signal to end")
	await get_tree().process_frame
	_check(str(Document.find(_rootNode(), 6).name) == "Docked", "renaming from the Scene dock")
	_undo()
	_check(str(Document.find(_rootNode(), 6).name) == "Ground2", "undo the Scene dock rename")
	_duplicateNode(6)
	_check(Document.count(_rootNode()) == 7 and Document.find(_rootNode(), _tab().selected).name == "Ground3", "duplicate with unique name")
	_undo()
	_undo()
	_toggleVisibility(6)
	_check(not Document.find(_rootNode(), 6).visible, "visibility toggle")
	_undo()
	var orderBefore: int = Document.children(_rootNode()).find(Document.find(_rootNode(), 6))
	_reorderNode(6, -1)
	_check(Document.children(_rootNode()).find(Document.find(_rootNode(), 6)) == orderBefore - 1, "move up among siblings")
	_undo()
	_check(str(Document.find(_rootNode(), 6).name) == "Cube", "undo chain restores the original")
	#scripts and exported variables
	var scriptPath := "res://scripts/smoke.jet"
	project.write(scriptPath, "extends MeshInstance3D\n@export var speed := 90.0\nvar frames := 0\n\nfunc _ready():\n\tprint(\"smoke ready \", name)\n\nfunc _process(delta):\n\tframes += 1\n\trotation_degrees.y += speed * delta\n")
	_attachScript(6, scriptPath)
	_check(str(Document.find(_rootNode(), 6).get("script", "")) == scriptPath, "script attached")
	var info := _scriptInfo(scriptPath)
	_check(info.get("ok", false) and info.exports.size() == 1 and info.exports[0].name == "speed", "exported variable visible to the Inspector")
	_select(6)
	_scriptPropertyEdited("speed", 120.0, true)
	_check(float(Document.find(_rootNode(), 6).script_properties.speed) == 120.0, "script property override")
	var bad := bridge.compile_script("extends Node3D\nfunc _process(delta):\n\tposition.x += nope\n", "res://bad.jet") as Dictionary
	_check(not bad.ok and int(bad.errors[0].line) == 3, "compile errors carry line numbers")
	#script file rename
	DirAccess.rename_absolute(project.toAbs(scriptPath), project.toAbs("res://scripts/renamed.jet"))
	_fileMoved(scriptPath, "res://scripts/renamed.jet")
	_check(str(Document.find(_rootNode(), 6).script) == "res://scripts/renamed.jet", "file rename updates node references")
	DirAccess.rename_absolute(project.toAbs("res://scripts/renamed.jet"), project.toAbs(scriptPath))
	_fileMoved("res://scripts/renamed.jet", scriptPath)
	#play / stop
	var editFrame := _frame()
	_play(true)
	_check(playing, "play starts")
	var arrow := InputEventKey.new()
	arrow.keycode = KEY_DOWN
	arrow.pressed = true
	var stopKey := InputEventKey.new()
	stopKey.keycode = KEY_F8
	stopKey.pressed = true
	_check(_isGameKey(arrow) and not _isGameKey(stopKey), "while playing, action keys are kept from the editor's controls (not F5-F8)")
	_check("smoke ready Cube" in output.get_parsed_text(), "print() appears in the Output panel")
	var first := _frame()

	for i in 12: _process(0.05)

	_check(_frame() != first, "scripts animate the game")
	_stop()
	_check(not playing and _frame() == editFrame, "stop restores the edited scene")
	#runtime errors
	project.write(scriptPath, "extends MeshInstance3D\nvar target = null\nfunc _process(delta):\n\ttarget.position.x = 1\n")
	_scriptCache.clear()
	_play(true)

	for i in 3: _process(0.05)

	_check(paused and errorsList.item_count > 0, "runtime error pauses the game and lists the error")
	_check(errorsList.item_count > 0 and int(errorsList.get_item_metadata(0).line) == 4, "runtime error line")
	_stop()
	#save, reopen, export
	_check(_saveScene(), "save scene")
	_openProject(folder)
	_check(Document.count(_rootNode()) == 6 and str(Document.find(_rootNode(), 6).get("script", "")) == scriptPath, "saved scene reopens with its script")
	project.write(scriptPath, "extends MeshInstance3D\n@export var speed := 90.0\nfunc _process(delta):\n\trotation_degrees.y += speed * delta\n")
	var packPath := _rootDir().path_join(".cache/tests/smoke.jetpack")
	_check(_exportTo(packPath), "export .jetpack")
	var bytes := FileAccess.get_file_as_bytes(packPath)
	_check(bytes.size() > 100 and bytes.slice(0, 5) == PackedByteArray([74, 69, 84, 80, 20]), "package header JETP v20")
	#Visibility Range comes from GeometryInstance3D, as in Godot; Fog is on the WorldEnvironment
	var inherited := []

	for cls in Document.classChain("Sprite3D") + Document.classChain("WorldEnvironment"):
		for prop in Document.PROPERTIES.get(cls, []): inherited.append(prop.key)

	_check("visibility_range_end" in inherited and "fog_depth_end" in inherited and Document.inherits("MeshInstance3D", "GeometryInstance3D")
		and "GeometryInstance3D" in Document.ABSTRACT, "Visibility Range and Fog in the Inspector")
	#2D nodes
	_addNode("ColorRect", int(_rootNode().id))
	var rectId: int = _tab().selected
	var before2D := _frame()
	_controlMoved(rectId, Vector2(30, 20), false)
	_controlMoved(rectId, Vector2(40, 30), true)
	_check(_frame() != before2D and Document.find(_rootNode(), rectId).position == [40.0, 30.0], "ColorRect moved by dragging")
	_check(viewport._controlAt(Vector2(45, 35)) == rectId and bridge.pick(45, 35) == rectId, "2D nodes are picked in the viewport")
	_addNode("Label", int(_rootNode().id))
	var labelFrame := _frame()
	_propertyEdited("text", "SCORE 10", true)
	_check(_frame() != labelFrame, "Label text renders over the scene")
	var labelId: int = _tab().selected
	var labelItem := scene_dock.tree.get_selected()
	_check(labelItem != null and labelItem.get_button_by_id(0, SceneDock.BUTTON_VISIBLE) >= 0, "Control nodes have the visibility toggle in the Scene tree")
	var shownFrame := _frame()
	_toggleVisibility(labelId)
	_check(_frame() != shownFrame and not Document.find(_rootNode(), labelId).visible, "hiding a Label from the Scene tree removes it from the view")
	_toggleVisibility(labelId)
	_addNode("Control", int(_rootNode().id))
	var groupId: int = _tab().selected
	_propertyEdited("position", [0.0, 120.0], true)
	_addNode("Label", groupId)
	_propertyEdited("text", "GROUPED", true)
	var grouped := _frame()
	_toggleVisibility(groupId)
	_check(_frame() != grouped, "a plain Control groups 2D nodes: hiding it hides its children")
	_toggleVisibility(groupId)
	#physics
	_addNode("StaticBody3D", int(_rootNode().id))
	var floorId: int = _tab().selected
	_addNode("CollisionShape3D", floorId)
	_propertyEdited("size", [10.0, 1.0, 10.0], true)
	_addNode("RigidBody3D", int(_rootNode().id))
	var bodyId: int = _tab().selected
	_propertyEdited("position", [0.0, 3.0, 0.0], true)
	_addNode("CollisionShape3D", bodyId)
	_propertyEdited("shape", "Sphere", true)
	#smoke.jet was rewritten in the same second as the error test, so its mtime can not invalidate the caches.
	script_editor.closeAll()
	_scriptCache.clear()
	_play(true)

	for i in 40: _process(0.05)

	var fallen: Dictionary = bridge.node_state(bodyId)
	_stop()
	_check(not fallen.is_empty() and absf(fallen.position.y - 1.0) < 0.05, "a RigidBody3D falls and rests on a StaticBody3D")
	_check(_saveScene(), "save scene with physics nodes")
	_openProject(folder)
	var floorShape: Dictionary = Document.children(Document.find(_rootNode(), floorId))[0]
	_check(floorShape.get("size") == [10.0, 1.0, 10.0], "CollisionShape3D size survives save and reopen")
	var lonely := {"id": 900, "name": "Ball", "type": "RigidBody3D", "children": [{"id": 901, "name": "Mesh", "type": "MeshInstance3D", "children": []}]}
	var stray := {"id": 902, "name": "Shape", "type": "CollisionShape3D", "children": []}
	var testRoot := {"id": 899, "name": "R", "type": "Node3D", "children": [lonely, stray]}
	_check(Document.configurationWarnings(testRoot, lonely).size() == 1 and Document.configurationWarnings(testRoot, stray).size() == 1
		and Document.configurationWarnings(_rootNode(), Document.find(_rootNode(), bodyId)).is_empty(),
		"configuration warnings: body without shape, shape without body")

	#2D physics: a ball falls on a ground; shapes draw their outline
	_addNode("StaticBody2D", int(_rootNode().id))
	var groundId: int = _tab().selected
	_propertyEdited("position", [160.0, 200.0], true)
	_addNode("CollisionShape2D", groundId)
	var groundShapeId: int = _tab().selected
	_propertyEdited("size", [300.0, 20.0], true)
	_addNode("RigidBody2D", int(_rootNode().id))
	var ballId: int = _tab().selected
	_propertyEdited("position", [160.0, 100.0], true)
	_addNode("CollisionShape2D", ballId)
	_propertyEdited("shape", "Circle", true)
	_frame()
	var outline: PackedVector2Array = bridge.shape_outline(groundShapeId)
	_check(outline.size() == 4 and outline[0].is_equal_approx(Vector2(10, 190)) and outline[2].is_equal_approx(Vector2(310, 210)),
		"CollisionShape2D outline on the canvas")
	_play(true)

	for i in 40: _process(0.05)

	var ball: Dictionary = bridge.node_state(ballId)
	_stop()
	_check(not ball.is_empty() and absf(ball.position.y - 180.0) < 1.5, "a RigidBody2D falls and rests on a StaticBody2D")
	_setScreen(Screen.VIEW_2D)
	viewport.canvas_zoom = 2.0
	viewport.canvas_origin = Vector2.ZERO
	_render()
	var outline2D: PackedVector2Array = bridge.shape_outline(groundShapeId)
	_check(viewport.mode_2d and outline2D.size() == 4 and outline2D[0].is_equal_approx(Vector2(20, 380)),
		"the 2D view shows the canvas at its own zoom")

	#2D gizmos, driven as a drag from one view point to another
	var drag2D := func(id: int, handle: Dictionary, from: Vector2, to: Vector2):
		viewport._beginEdit2D(id, handle, from)
		viewport._updateEdit2D(to, true, false, false)
		viewport._nav = viewport.Nav.NONE
		viewport._drag = {}
	var handle2D := func(kind: String, key: String, value: Variant) -> Dictionary:
		for h in viewport._handles2D():
			if h.kind == kind and h.get(key) == value: return h

		return {}

	var groundAt := viewport.canvasToArea(Vector2(160, 200))
	_select(groundId)
	viewport._setTool(viewport.Tool.ROTATE)
	drag2D.call(groundId, handle2D.call("rotate", "kind", "rotate"), groundAt + Vector2(60, 0), groundAt + Vector2(0, 60))
	_check(is_equal_approx(float(Document.find(_rootNode(), groundId).rotation_degrees), 90.0), "the rotate gizmo turns a Node2D clockwise")
	_select(groundShapeId)
	viewport._setTool(viewport.Tool.MOVE)
	drag2D.call(groundShapeId, {"kind": "move", "axis": -1}, groundAt, viewport.canvasToArea(Vector2(170, 200)))
	_check(Document.find(_rootNode(), groundShapeId).position == [0.0, -10.0], "moving a child of a rotated Node2D follows the cursor")
	_undo()
	_undo()
	_select(groundId)
	viewport._setTool(viewport.Tool.SCALE)
	var scaleX: Dictionary = handle2D.call("scale", "axis", 0)
	drag2D.call(groundId, scaleX, scaleX.get("at", Vector2.ZERO), groundAt + scaleX.get("dir", Vector2.ZERO) * 120.0)
	_check(Document.find(_rootNode(), groundId).scale == [2.0, 1.0], "the scale gizmo's X handle scales one axis")
	_undo()
	_select(groundShapeId)
	var rightEdge: Dictionary = handle2D.call("shape", "side", Vector2i(1, 0))
	drag2D.call(groundShapeId, rightEdge, rightEdge.get("at", Vector2.ZERO), viewport.canvasToArea(Vector2(330, 200)))
	_check(Document.find(_rootNode(), groundShapeId).size == [340.0, 20.0], "CollisionShape2D handles resize the shape")
	_undo()
	_addNode("ColorRect", int(_rootNode().id))
	var panelId: int = _tab().selected
	viewport._setTool(viewport.Tool.SELECT)
	var corner: Dictionary = handle2D.call("resize", "side", Vector2i(1, 1))
	drag2D.call(panelId, corner, corner.get("at", Vector2.ZERO), viewport.canvasToArea(Vector2(80, 72)))
	viewport._nudge2D(KEY_RIGHT, false)
	var panel: Dictionary = Document.find(_rootNode(), panelId)
	_check(panel.size == [80.0, 72.0] and panel.position == [1.0, 0.0], "Control resize handles and arrow-key nudging")
	_deleteNode(panelId)
	viewport._setTool(viewport.Tool.MOVE)
	_setScreen(Screen.VIEW_3D)
	var shape_type := str(Document.find(_rootNode(), groundId).type)
	_changeType(groundId, "Area2D")
	var retyped: Dictionary = Document.find(_rootNode(), groundId)
	_check(str(retyped.type) == "Area2D" and retyped.has("monitoring") and not retyped.has("friction")
		and retyped.position == [160.0, 200.0] and Document.children(retyped).size() == 1, "Change Type keeps name, children and shared properties")
	_undo()
	_check(str(Document.find(_rootNode(), groundId).type) == shape_type, "Change Type can be undone")
	var lonely2D := {"id": 903, "name": "Zone", "type": "Area2D", "children": []}
	var warnings2D := Document.configurationWarnings(testRoot, lonely2D)
	_check(warnings2D.size() == 1 and "CollisionShape2D" in warnings2D[0]
		and Document.configurationWarnings(_rootNode(), Document.find(_rootNode(), ballId)).is_empty(),
		"2D bodies want a CollisionShape2D")
	_deleteNode(ballId)
	_deleteNode(groundId)
	#new scenes ask for their root, as Godot's Scene dock does
	var tab_count := tabs.size()
	_newScene()
	new_scene_dialog.hide()
	new_scene_dialog.root_chosen.emit("Node2D")
	_check(tabs.size() == tab_count + 1 and str(_rootNode().type) == "Node2D" and screen == Screen.VIEW_2D, "a new 2D scene opens in the 2D view")
	_closeTab(current, true)
	_newScene()
	new_scene_dialog.hide()
	new_scene_dialog.root_chosen.emit("Control")
	_check(str(_rootNode().type) == "Control" and _rootNode().size == [viewport.W, viewport.H], "a User Interface scene fills the screen")
	_closeTab(current, true)
	_setScreen(Screen.VIEW_3D)
	#timer and signals
	_addNode("Timer", int(_rootNode().id))
	var timerId: int = _tab().selected
	_propertyEdited("wait_time", 0.1, true)
	_propertyEdited("autostart", true, true)
	var timerScript := "res://scripts/smoke_timer.jet"
	project.write(timerScript, "extends Timer\nsignal fired(count)\nvar count := 0\nfunc _ready():\n\ttimeout.connect(_on_timeout)\n"
		+ "\tfired.connect(_on_fired)\nfunc _on_timeout():\n\tcount += 1\n\tfired.emit(count)\nfunc _on_fired(n):\n\tif n == 2:\n\t\tprint(\"smoke timeout \", wait_time)\n")
	_attachScript(timerId, timerScript)
	var timerInfo := _scriptInfo(timerScript)
	var timerClass: Array = bridge.class_db().filter(func(c): return c.name == "Timer")
	_check(timerInfo.get("ok", false) and timerInfo.signals.size() == 1 and timerInfo.signals[0].name == "fired"
		and timerClass.size() == 1 and timerClass[0].signals[0].name == "timeout", "script and engine signals reach the completion")
	_play(true)

	for i in 6: _process(0.05)

	var timedOut := "smoke timeout 0.1" in output.get_parsed_text()
	_stop()
	_check(timedOut, "a Timer emits timeout, a script signal passes it on")
	_deleteNode(timerId)
	#signals connected in the Signals dock: the Connect dialog's name, the stub added to the script, Play, undo, deletion
	_addNode("Timer", int(_rootNode().id))
	var tickerId: int = _tab().selected
	_renameNode(tickerId, "Ticker")
	_propertyEdited("wait_time", 0.1, true)
	_propertyEdited("autostart", true, true)
	_addNode("Node", int(_rootNode().id))
	var receiverId: int = _tab().selected
	_renameNode(receiverId, "Receiver")
	var receiverScript := "res://scripts/smoke_receiver.jet"
	project.write(receiverScript, "extends Node\n")
	_attachScript(receiverId, receiverScript)
	_select(tickerId)
	rightTabs.current_tab = 1
	_connectRequested("timeout", [])
	var receiverItem: TreeItem = connect_dialog._tree.get_root()

	while receiverItem != null and int(receiverItem.get_metadata(0)) != receiverId: receiverItem = receiverItem.get_next_in_tree()

	if receiverItem != null: receiverItem.select(0)

	var proposed: String = connect_dialog._method.text
	connect_dialog.hide()
	_check(proposed == "_on_ticker_timeout" and not connect_dialog.get_ok_button().disabled, "the Connect dialog proposes Godot's method name")
	_addConnection("timeout", [], receiverId, proposed, 0)
	var stub = script_editor.sourceOf(receiverScript)
	_check(Document.connections(_tab().doc).size() == 1 and stub != null and "func _on_ticker_timeout() -> void:" in stub
		and screen == Screen.SCRIPT, "connecting adds the method to the receiver's script and opens it")
	var dockTree: Tree = signals_dock._tree
	var timeoutItem: TreeItem = dockTree.get_root().get_child(0).get_child(0) if dockTree.get_root() != null else null
	_check(timeoutItem != null and timeoutItem.get_text(0) == "timeout()" and timeoutItem.get_child_count() == 1
		and "Receiver :: _on_ticker_timeout()" in timeoutItem.get_child(0).get_text(0), "the Signals dock lists the signal and its connection")
	script_editor._scripts[receiverScript].code.text = stub.replace("pass # Replace with function body.", "print(\"smoke connected\")")
	script_editor.save(receiverScript)
	_setScreen(Screen.VIEW_3D)
	_play(true)

	for i in 4: _process(0.05)

	var connected := "smoke connected" in output.get_parsed_text()
	_stop()
	_check(connected, "a connection made in the editor works in Play")
	var connectionsPack := _rootDir().path_join(".cache/tests/connections.jetpack")
	var connectionsBytes := FileAccess.get_file_as_bytes(connectionsPack) if _exportTo(connectionsPack) else PackedByteArray()
	_check(connectionsBytes.hex_encode().contains("_on_ticker_timeout".to_utf8_buffer().hex_encode()), "the package carries the connection")
	_undo()
	_check(Document.connections(_tab().doc).is_empty(), "undo removes the connection")
	_redo()
	_duplicateNode(tickerId)
	_check(Document.connections(_tab().doc).size() == 2, "a duplicated node keeps its connections")
	_undo()
	_deleteNode(receiverId)
	_check(Document.connections(_tab().doc).is_empty(), "deleting the receiver drops its connections")
	_deleteNode(tickerId)
	rightTabs.current_tab = 0

	#AnimationPlayer: the Animation panel, the Inspector's key icons, the preview and Play
	_addNode("MeshInstance3D", int(_rootNode().id))
	var moverId: int = _tab().selected
	_renameNode(moverId, "Mover")
	_addNode("AnimationPlayer", int(_rootNode().id))
	var animPlayerId: int = _tab().selected
	_select(animPlayerId)
	_check(bottomTabs.current_tab == 3 and animation_panel.playerId == animPlayerId, "selecting an AnimationPlayer opens the Animation panel")
	animation_panel._onEditMenu(0)
	_check(Document.find(_rootNode(), animPlayerId).get("animations", []).size() == 1 and animation_panel.animation == "new_animation",
		"Animation > New adds an animation")

	_select(moverId)
	_check(inspector.keyable.call(Document.find(_rootNode(), moverId), "position"), "the Inspector shows key icons while an animation is edited")
	_keyProperty("position")
	animation_panel._setTime(1.0)
	_propertyEdited("position", [2.0, 0.0, 0.0], true)
	_keyProperty("position")
	var positionTrack: Dictionary = Document.find(_rootNode(), animPlayerId).animations[0].tracks[0]
	_check(positionTrack.path == "Mover" and positionTrack.property == "position" and positionTrack.keys.size() == 2
		and positionTrack.keys[1].value == [2.0, 0.0, 0.0], "key icons add a track and its keys")
	animation_panel._setTime(0.5)
	var posed: Dictionary = bridge.node_state(moverId)
	_check(not posed.is_empty() and absf(posed.position.x - 1.0) < 0.01 and Document.find(_rootNode(), moverId).position == [2.0, 0.0, 0.0],
		"moving the time previews the pose without changing the scene")

	animation_panel._menuTarget = {"method": false}
	animation_panel._showNodeMenu()
	var moverIndex := -1

	for i in animation_panel._nodeMenu.item_count:
		if int(animation_panel._nodeMenu.get_item_metadata(i)) == moverId: moverIndex = i

	animation_panel._nodeMenu.hide()
	animation_panel._onNodeChosen(moverIndex)
	animation_panel._propertyMenu.hide()
	var choices: Array = animation_panel._menuTarget.get("choices", [])
	_check(choices.any(func(c): return c.property == "rotation_degrees") and not choices.any(func(c): return c.property == "mesh"),
		"Add Track offers the properties the runtime can animate")
	animation_panel._menuTarget = {"method": true}
	animation_panel._onNodeChosen(moverIndex)
	animation_panel._insertAt(1, 0.5)
	animation_panel._selected = Vector2i(1, 0)
	animation_panel.refresh()
	animation_panel._setKeyField(1, 0, "method", "hit")
	animation_panel._setKeyField(1, 0, "args", [7])
	var callKey: Dictionary = Document.find(_rootNode(), animPlayerId).animations[0].tracks[1].keys[0]
	_check(callKey.method == "hit" and callKey.args == [7] and animation_panel._keyBox.get_child_count() > 0, "a Call Method track with a key and its arguments")

	animation_panel._autoplay.button_pressed = true
	animation_panel._toggleAutoplay()
	var moverScript := "res://scripts/smoke_mover.jet"
	project.write(moverScript, "extends MeshInstance3D\nfunc hit(n):\n\tprint(\"smoke hit \", n)\n")
	_attachScript(moverId, moverScript)
	_play(true)

	for i in 5: _process(0.1)

	var halfway: Dictionary = bridge.node_state(moverId)

	for i in 5: _process(0.1)

	var hit := "smoke hit 7" in output.get_parsed_text()
	_stop()
	_check(hit and not halfway.is_empty() and absf(halfway.position.x - 1.0) < 0.25, "an autoplay animation moves the node and calls its method in Play")
	var animPack := _rootDir().path_join(".cache/tests/animation.jetpack")
	_check(_exportTo(animPack), "export with an AnimationPlayer")

	_select(animPlayerId)
	var invalidBefore := output.get_parsed_text().count("Invalid scene")
	animation_panel._length.value = 0.25
	_check(Document.find(_rootNode(), animPlayerId).animations[0].length == 0.25 and output.get_parsed_text().count("Invalid scene") == invalidBefore,
		"a shorter length keeps the scene valid (later keys are left out)")
	_undo()
	_deleteNode(animPlayerId)
	_deleteNode(moverId)
	bottomTabs.current_tab = 0
	_check(animation_panel.playerId == 0, "deleting the AnimationPlayer clears the panel")

	#particles
	_addNode("CPUParticles3D", int(_rootNode().id))
	_propertyEdited("position", [0.0, 1.5, 0.0], true)
	_propertyEdited("direction", [0.0, 1.0, 0.0], true)
	_propertyEdited("initial_velocity_max", 2.0, true)
	_propertyEdited("amount", 40, true)
	_play(true)

	for i in 20: _process(0.05)

	var liveParticles := int(bridge.statistics().get("particles", 0))
	_stop()
	_check(liveParticles > 10, "CPUParticles3D emits particles while playing")
	#OBJ import
	var cube := "mtllib cube.mtl\nv -0.5 -0.5 0.5\nv 0.5 -0.5 0.5\nv 0.5 0.5 0.5\nv -0.5 0.5 0.5\nv -0.5 -0.5 -0.5\nv 0.5 -0.5 -0.5\nv 0.5 0.5 -0.5\nv -0.5 0.5 -0.5\nvt 0 0\nvt 1 0\nvt 1 1\nvt 0 1\n" \
		+ "o Body\nusemtl Paint\nf 1/1 2/2 3/3 4/4\nf 6/1 5/2 8/3 7/4\nf 2/1 6/2 7/3 3/4\nf 5/1 1/2 4/3 8/4\no Lid\nusemtl Decal\nf 4/1 3/2 7/3 8/4\nf 5/1 6/2 2/3 1/4\n"

	project.write("res://models/cube.obj", cube)
	project.write("res://models/cube.mtl", "newmtl Paint\nKd 0.2 0.8 0.4\nnewmtl Decal\nKd 1 1 1\nmap_Kd decal.png\n")
	var decal := Image.create(64, 64, false, Image.FORMAT_RGB8)

	for y in 64:
		for x in 64: decal.set_pixel(x, y, Color(x / 64.0, y / 64.0, 0.5))

	decal.save_png(project.toAbs("res://models/decal.png"))
	var modelFrame := _frame()
	var countBefore := Document.count(_rootNode())
	_instantiateModel(int(_rootNode().id), "res://models/cube.obj", Vector3(1, 0, 0))
	var group := Document.find(_rootNode(), _tab().selected)
	_check(Document.count(_rootNode()) == countBefore + 3 and str(group.name) == "cube" and Document.children(group).size() == 2,
		"OBJ dropped in the scene creates a node per part")
	var lid: Dictionary = Document.children(group)[1] if Document.children(group).size() == 2 else {}
	_check(str(lid.get("mesh_part", "")) == "Lid" and str(lid.get("albedo_texture", "")) == "res://models/decal.png"
		and int(Document.children(group)[0].color) == 0x33cc66, "parts take the MTL colour and texture")
	_check(_frame() != modelFrame and _missingAssets(_tab().doc).is_empty(), "imported model renders")
	var small := _meshBounds(lid)
	_importApplied("res://models/cube.obj", {"scale": 2.0, "mirror_x": false, "flip_faces": false, "flip_v": false})
	var large := _meshBounds(lid)
	_check(small.size() == 2 and large.size() == 2 and is_equal_approx(large[1].x, 2.0 * small[1].x), "import settings rescale the model")
	_importApplied("res://models/decal.png", {"max_size": "32", "palette": "Quantize", "flip_v": false})
	_check(_textureSize("res://models/decal.png") == Vector2i(32, 32), "texture import reduces the size")
	_check(FileAccess.file_exists(project.toAbs("res://models/decal.png.import")), "import settings saved next to the file")
	var modelPack := _rootDir().path_join(".cache/tests/smoke_model.jetpack")
	_check(_exportTo(modelPack) and FileAccess.get_file_as_bytes(modelPack).size() > bytes.size() + 1024, "export ships models and textures")
	_change("", true, func(): Document.children(Document.find(_rootNode(), int(group.id)))[1].mesh_part = "Nope")
	_check(not _missingAssets(_tab().doc).is_empty(), "missing model parts block the export")
	_undo()
	#texture budget for the board
	var tile := Image.create(32, 32, false, Image.FORMAT_RGB8)

	for y in 32:
		for x in 32: tile.set_pixel(x, y, Color(x / 31.0, y / 31.0, ((x * y) % 32) / 31.0))

	tile.resize(512, 512, Image.INTERPOLATE_NEAREST)
	tile.save_png(project.toAbs("res://models/heavy.png"))
	_fileDropped(int(lid.id), "res://models/heavy.png")
	_check(compressDialog.visible and _compression.has("res://models/heavy.png"), "a heavy texture offers compression for the board")
	compressDialog.hide()
	_applyCompression()
	var light := _textureCost("res://models/heavy.png", _importSettings("res://models/heavy.png"))
	_check(light.get("size") == Vector2i(256, 256) and light.get("indexed", false), "compression imports it at 256 px with 256 colors")
	tile.resize(1024, 1024, Image.INTERPOLATE_NEAREST)
	tile.save_png(project.toAbs("res://models/huge.png"))
	_keptTextures["res://models/huge.png"] = true
	_fileDropped(int(lid.id), "res://models/huge.png")
	var bigPack := _rootDir().path_join(".cache/tests/smoke_big.jetpack")
	_check(_exportTo(bigPack) and not _fitsBoard(bigPack, "build") and compressDialog.visible and _compression.has("res://models/huge.png"),
		"a package over 1 MiB stops the board build and offers compression")
	compressDialog.hide()
	_afterCompression = Callable()
	_applyCompression()
	_check(_exportTo(bigPack) and _fitsBoard(bigPack, "build"), "after compression the package fits the board")
	_undo()
	_undo()
	#Sprite3D
	var beforeSprite := _frame()
	var spriteId := Document.nextId(_rootNode())
	_change("", true, func():
		var sprite := Document.newNode("Sprite3D", spriteId, "Sign")
		sprite.texture = "res://models/decal.png"
		sprite.position = [0.0, 1.5, 0.0]
		sprite.pixel_size = 0.05
		Document.children(_rootNode()).append(sprite))
	_check(_frame() != beforeSprite and _missingAssets(_tab().doc).is_empty()
		and _meshBounds(Document.find(_rootNode(), spriteId)).size() == 2, "Sprite3D draws its texture and has selection bounds")
	_undo()
	#2D nodes
	var before2DSprite := _frame()
	_addNode("Node2D", int(_rootNode().id))
	var holderId: int = _tab().selected
	_propertyEdited("position", [200.0, 150.0], true)
	_addNode("Sprite2D", holderId)
	var shipId: int = _tab().selected
	_propertyEdited("texture", "res://models/decal.png", true)
	_propertyEdited("position", [20.0, 20.0], true)
	var shipItem := scene_dock.tree.get_selected()
	_check(shipItem != null and shipItem.get_button_by_id(0, SceneDock.BUTTON_VISIBLE) >= 0, "2D nodes have the visibility toggle")
	var drawnFrame := _frame()
	var drawn: Rect2i = bridge.drawn_rect(shipId)
	_check(drawnFrame != before2DSprite and drawn.has_area() and drawn.get_center().distance_to(Vector2(220, 170)) < 2
		and viewport._controlAt(Vector2(220, 170)) == shipId, "Sprite2D draws under its Node2D parent and is picked in the viewport")
	viewport._beginMove2D(shipId, viewport._imageRect().position)
	viewport._updateMove2D(viewport._imageRect().position + Vector2(10, 0) * viewport._imageRect().size.x / viewport.W, true)
	#as when the button is released
	viewport._nav = viewport.Nav.NONE
	viewport._drag = {}
	_check(Document.find(_rootNode(), shipId).position[0] > 20.0, "dragging moves a Sprite2D")
	_deleteNode(holderId)
	#hardware
	_check(Hardware.boards.has("nano_esp32") and Hardware.displays.has("st7796_320x480"), "hardware profiles are loaded")
	project.config.hardware = {"board": "esp32s3_devkitc_n8r8", "display": "st7796_320x480", "orientation": "landscape"}
	_applyHardware()
	_check(_frame().size() == 480 * 320 * 3 and viewport.W == 480.0 and targetSelect.get_item_text(1).begins_with("S3 N8R8"),
		"a 480x320 display sets the resolution and the board name")
	var widePack := _rootDir().path_join(".cache/tests/smoke_wide.jetpack")
	var wide := FileAccess.get_file_as_bytes(widePack) if _exportTo(widePack) else PackedByteArray()
	_check(wide.size() > 16 and wide.decode_u16(12 + wide.decode_u32(8)) == 480 and wide.decode_u16(14 + wide.decode_u32(8)) == 320,
		"the package carries the resolution")
	project.config.hardware.orientation = "portrait"
	_applyHardware()
	_check(_frame().size() == 320 * 480 * 3, "portrait orientation swaps the resolution")
	var conflict := Hardware.settings({"hardware": {"board": "nano_esp32", "pins": {"cs": 9}}})
	_check(not Hardware.problems(conflict).is_empty() and Hardware.problems(Hardware.settings({})).is_empty(), "pin conflicts are reported")
	#the Input Map
	projectSettings.open(project, 2)
	projectSettings._newAction.text = "jump"
	projectSettings._addAction()
	projectSettings._actions[6].keys = ["J"]
	projectSettings._actions[6].pin = 5
	projectSettings._actions[0].pin = 6
	projectSettings._check()
	var settingsOk: bool = not projectSettings.get_ok_button().disabled
	projectSettings._apply()
	projectSettings.hide()
	var saved: Array = project.config.get("input_map", [])
	_check(settingsOk and saved.size() == 7 and saved[6].name == "jump" and saved[6].keys == ["J"] and int(saved[0].pin) == 6
		and not project.config.has("input_pins"), "Input Map: actions added in Project Settings with keys and pins")
	_check(_actionKeys.size() == 7 and KEY_J in _actionKeys[6], "the game view reads the Input Map keys")
	var inputPack := _rootDir().path_join(".cache/tests/input.jetpack")
	var inputBytes := FileAccess.get_file_as_bytes(inputPack) if _exportTo(inputPack) else PackedByteArray()
	_check(inputBytes.size() > 16 and inputBytes.decode_u8(16 + inputBytes.decode_u32(8)) == 7, "the package carries the Input Map")
	project.config.erase("input_map")
	_applyHardware()
	#STM32 boards
	var stm32 := Hardware.settings({"hardware": {"board": "nucleo_f767zi"}})
	#D2 · PF15, no SPI
	var wrongSck := Hardware.settings({"hardware": {"board": "nucleo_f767zi", "pins": {"sck": 0x5F}}})
	var stm32Ok := true

	for id in Hardware.boards:
		if Hardware.platform({"board": id}) == "stm32": stm32Ok = stm32Ok and Hardware.problems(Hardware.settings({"hardware": {"board": id}})).is_empty()

	_check(Hardware.platform(stm32) == "stm32" and Hardware.pin_label(stm32, 0x05) == "D13 · PA5" and stm32Ok
		and Hardware.problems(wrongSck).is_empty() and Hardware.softwareSpi(wrongSck) and not Hardware.softwareSpi(stm32),
		"STM32 boards: pin labels, SPI pins and software SPI elsewhere")
	#D11, once JP6 is off
	var f746 := Hardware.settings({"hardware": {"board": "nucleo_f746zg", "pins": {"mosi": 7}}})
	var active_high: Array = Hardware.boards["nucleo_f746zg"].get("active_high", []).map(func(pin): return int(pin))
	_check(Hardware.pin_label(f746, 45) == "B1 USER · PC13" and active_high == [45]
		and not Hardware.softwareSpi(f746) and Hardware.problems(f746).is_empty(),
		"Nucleo-144: the B1 USER button (pressed = high) and MOSI on D11 through the SPI peripheral")

	#the RAM estimate logged before a board build: the F746's heap, the Nano's PSRAM.
	var memoryText := func(hardware: Dictionary) -> String:
		project.config["hardware"] = hardware
		_applyHardware()

		if not _exportTo(inputPack): return ""

		var logged := output.get_parsed_text().length()
		_reportMemory()
		return output.get_parsed_text().substr(logged)

	var stm32Memory: String = memoryText.call({"board": "nucleo_f746zg", "pins": {"mosi": 7}})
	var psramMemory: String = memoryText.call({"board": "nano_esp32"})
	_check(stm32Memory.begins_with("RAM (estimate): about ") and stm32Memory.contains(" of 288 KB")
		and psramMemory.contains(" in PSRAM (of 8 MB)") and int(_packMemory.field) == 160 * 120 * 2,
		"RAM estimate for boards without PSRAM (fits or not) and with it")
	project.config.erase("hardware")
	_applyHardware()
	_check(_frame().size() == 320 * 240 * 3, "default hardware is 320x240 again")
	#serial monitor, fed with fake bytes (no board needed)
	serial_monitor.clear()
	serial_monitor.feed("\u001b[0;32mI (1000) JetStudio: Frame 40.00 ms (scripts 2.50 ms); 362 raster tris; free heap 200000, PSRAM 8000000\u001b[0m\r\n".to_utf8_buffer())
	serial_monitor.feed("I (3400) JetStudio: Frame 39.00 ms (scripts 2.40 ms); 360 raster tris; free heap 199000, PSRAM 7990000\r\n".to_utf8_buffer())
	_check(serial_monitor.statsText().begins_with("25.0 FPS · 39.0 ms (script 2.4)"), "serial monitor shows FPS and frame time of the board")
	serial_monitor.feed("\u001b[0;31mE (3500) JetStudio: res://scripts/smoke.jet:4: Invalid".to_utf8_buffer())
	serial_monitor.feed(" access è\u001b[0m\r\n".to_utf8_buffer())
	_check("E (3500) JetStudio: res://scripts/smoke.jet:4: Invalid access è" in serial_monitor._output.get_parsed_text(),
		"serial lines split across reads are joined, UTF-8 kept, colour codes removed")
	_check(serial_monitor.serial != null and serial_monitor.serial.list_ports() is Array, "serial ports are listed")
	_check(not str(serial_monitor.serial.open("/dev/jet-studio-missing", 115200)).is_empty(), "a missing port reports an error")
	var realDecode := serial_monitor.decode
	serial_monitor.decode = func(addresses): return [["game()", "firmware/src/main.cpp:64"]] if addresses == PackedStringArray(["0x42001B71"]) else []
	serial_monitor.feed("Backtrace: 0x42001B71:0x3FCB9880\r\n".to_utf8_buffer())
	_check("↳ game()  firmware/src/main.cpp:64" in serial_monitor._output.get_parsed_text(), "crash backtraces are decoded to source lines")
	serial_monitor.decode = realDecode
	_check(_shortFunction("auto Renderer::R::draw<true, true>(Renderer::V const&, bool)::{lambda(auto:1)#1}::operator()<std::integral_constant<bool, true> >(std::integral_constant<bool, true>) const")
		== "auto Renderer::R::draw()::{lambda()#1}::operator() const", "backtrace function names are shortened")
	#change_scene_to_file(), autoloads and the scenes in the package
	project.write("res://scripts/game.jet", "extends Node\nvar score := 41\n")
	project.config.autoload = [{"name": "Game", "path": "res://scripts/game.jet"}]
	_applyHardware()
	project.write("res://scripts/level2.jet", "extends Node3D\nfunc _ready():\n\tprint(\"smoke level2 \", get_tree().current_scene.name, \" \", Game.score)\n")
	project.write("res://scripts/go.jet", "extends Node3D\nfunc _ready():\n\tGame.score += 1\n\tget_tree().change_scene_to_file(\"res://level2.jetscene\")\n")
	project.write("res://level2.jetscene", JSON.stringify({"version": 2, "root":
		{"id": 1, "name": "Level2", "type": "Node3D", "script": "res://scripts/level2.jet", "children": []}}))
	_scriptCache.clear()
	_rootNode()["script"] = "res://scripts/go.jet"
	_upload()
	_play(true)

	for i in 3: _process(0.05)

	var changed := "smoke level2 Level2 42" in output.get_parsed_text()
	_stop()
	_check(changed and str(_rootNode().name) != "Level2",
		"change_scene_to_file() loads another scene, the Game autoload keeps its state; Stop comes back")
	var scenesPack := _rootDir().path_join(".cache/tests/scenes.jetpack")
	var has := func(bytes: PackedByteArray, text: String): return bytes.hex_encode().contains(text.to_utf8_buffer().hex_encode())
	var withSplash := FileAccess.get_file_as_bytes(scenesPack) if _exportTo(scenesPack) else PackedByteArray()
	project.config.boot_splash = false
	var withoutSplash := FileAccess.get_file_as_bytes(scenesPack) if _exportTo(scenesPack) else PackedByteArray()
	project.config.erase("boot_splash")
	_check(has.call(withSplash, "res://level2.jetscene") and has.call(withSplash, BOOT_SPLASH)
		and has.call(withoutSplash, "res://level2.jetscene") and not has.call(withoutSplash, BOOT_SPLASH),
		"the package carries the other scenes and, unless turned off, the boot splash")
	_rootNode().erase("script")
	project.config.erase("autoload")
	_applyHardware()
	#example project
	_openProject(_exampleFolder())
	_check(current >= 0 and not _dirty(_tab()), "an opened scene has no unsaved changes")
	_play(false)
	_check(playing, "example project plays")

	for i in 5: _process(0.05)

	_stop()

	if not _screenshot.is_empty():
		_setScreen(Screen.VIEW_3D)
		await _capture(_screenshot)

	if _smokeFailed:
		get_tree().quit(1)
		return

	print("PASS: Godot UI -> GDExtension -> runtime -> Jet; nodes, undo, hierarchy, scripts, play/stop, errors, save, export, 2D, physics, 2D physics, 2D view, 2D gizmos, new scene roots, Change Type, Timer, signals, Signals dock connections, Animation panel, particles, Input Map, scenes, autoloads, boot splash, OBJ import, serial monitor, wireframe view")
	get_tree().quit()
