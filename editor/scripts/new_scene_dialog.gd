extends ConfirmationDialog
## Godot's "Create Root Node" for a new scene: 2D Scene, 3D Scene, User Interface, or Other Node (any type).

signal root_chosen(type: String) # "" for Other Node

const Style := preload("res://scripts/theme.gd")
const Icons := preload("res://scripts/icons.gd")

func _ready() -> void:
	title = "New Scene"
	cancel_button_text = "Cancel"
	get_ok_button().visible = false
	var box := VBoxContainer.new()
	box.add_theme_constant_override("separation", 6)
	add_child(box)
	var label := Label.new()
	label.text = "Create Root Node:"
	label.theme_type_variation = "PropertyLabel"
	box.add_child(label)
	for choice in [["2D Scene", "Node2D", "Node2D", "A 2D world in canvas pixels: sprites, 2D physics, Camera2D."],
			["3D Scene", "Node3D", "Node3D", "A 3D world in metres, with a light and an environment."],
			["User Interface", "Control", "Control", "Controls over the game: menus, HUD, title screens."],
			["Other Node", "Add", "", "Any node type as the root."]]:
		var button := Button.new()
		button.text = choice[0]
		button.icon = Icons.get_icon(choice[1])
		button.tooltip_text = choice[3]
		button.alignment = HORIZONTAL_ALIGNMENT_LEFT
		button.custom_minimum_size = Vector2(300, 36)
		var type: String = choice[2]
		button.pressed.connect(func():
			hide()
			root_chosen.emit(type))
		box.add_child(button)

func open() -> void:
	popup_centered(Vector2i(340, 0))
