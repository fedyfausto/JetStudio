#!/usr/bin/env python3
"""Builds examples/physics_demo, a small level that shows off the physics nodes, particles and an autoload."""
import json
import math
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
PROJECT = ROOT / "examples/physics_demo"

SCRIPTS = {
    "player.jet": '''extends CharacterBody3D
## Godot's CharacterBody3D template: arrows/WASD move, Space jumps.

@export var speed := 4.0
@export var jump_velocity := 5.0
const GRAVITY := 9.8


func _physics_process(delta: float) -> void:
	if not is_on_floor():
		velocity.y -= GRAVITY * delta
	if Input.is_action_just_pressed("ui_accept") and is_on_floor():
		velocity.y = jump_velocity
	velocity.x = Input.get_axis("ui_left", "ui_right") * speed
	velocity.z = Input.get_axis("ui_up", "ui_down") * speed
	move_and_slide()
	if position.y < -10.0:
		position = Vector3(0.0, 2.0, 4.0) # fell off: back to the start
		velocity = Vector3.ZERO
''',
    "follow_camera.jet": '''extends Camera3D
## Follows the player from behind and above.

@export var offset := Vector3(0.0, 5.0, 8.0)
@onready var target = get_node("../Player")


func _process(delta: float) -> void:
	position = position.lerp(target.position + offset, min(1.0, 4.0 * delta))
	look_at(target.position + Vector3(0.0, 1.0, 0.0))
''',
    "coin.jet": '''extends Area3D
## Spins, and disappears when the player touches it.

@onready var hud = get_node("/root/World/HUD")
@onready var sparks = get_node("/root/World/PickupSparks")


func _process(delta: float) -> void:
	rotate_y(3.0 * delta)


func _on_body_entered(body) -> void:
	if body.name == "Player":
		hud.add_coin()
		# The sparks live outside the coin so they survive queue_free().
		sparks.global_position = global_position
		sparks.restart()
		queue_free()
''',
    "hud.jet": '''extends Label
## Coin counter.

@export var total := 5
var coins := 0
var finished := 0.0


func _ready() -> void:
	text = "COINS 0/%d" % total


func _process(delta: float) -> void:
	if coins < total:
		return
	finished += delta
	if finished > 2.0:
		Game.wins += 1
		get_tree().change_scene_to_file("res://win.jetscene")


func add_coin() -> void:
	coins += 1
	text = "COINS %d/%d" % [coins, total]
	if coins == total:
		text += "  WELL DONE!"
''',
    "win.jet": '''extends Node3D
## The end screen: Space goes back to the level.

@onready var coin = get_node("Coin")


func _ready() -> void:
	get_node("Wins").text = "Wins: %d" % Game.wins


func _process(delta: float) -> void:
	coin.rotate_y(2.0 * delta)
	if Input.is_action_just_pressed("ui_accept"):
		get_tree().change_scene_to_file("res://main.jetscene")
''',
    "game.jet": '''extends Node
## Autoload (Project Settings > Autoload), so it survives scene changes.

var wins := 0
''',
    "platform.jet": '''extends StaticBody3D
## Moving platform; a player standing on it rides along.

@export var distance := 2.5
@export var period := 6.0
var time := 0.0
var start := Vector3.ZERO


func _ready() -> void:
	start = position


func _physics_process(delta: float) -> void:
	time += delta
	position = start + Vector3(sin(time * TAU / period) * distance, 0.0, 0.0)
''',
}


class Scene:
    def __init__(self):
        self.next_id = 1

    def node(self, type_name, name, children=(), **props):
        node = {"id": self.next_id, "name": name, "type": type_name}
        self.next_id += 1
        for key, value in props.items():
            node[key] = [float(v) for v in value] if isinstance(value, (list, tuple)) else value
        node["children"] = list(children)
        return node

    def mesh(self, mesh, color, scale=(1, 1, 1), **props):
        return self.node("MeshInstance3D", "Mesh", mesh=mesh, color=color, scale=scale, **props)

    def box_body(self, type_name, name, position, size, color, rotation=(0, 0, 0), mesh_props=None, **props):
        return self.node(type_name, name, [self.node("CollisionShape3D", "CollisionShape3D", shape="Box", size=size),
                                           self.mesh("Box", color, size, **(mesh_props or {}))],
                         position=position, rotation=rotation, **props)


def build_scene():
    s = Scene()
    root_id = s.next_id
    s.next_id += 1
    nodes = [
        s.node("WorldEnvironment", "WorldEnvironment", background_mode="Gradient", sky_top_color=0x1b3a6b,
               sky_horizon_color=0x7fb0e0, ground_horizon_color=0x3a4a3a, ground_bottom_color=0x1a221a, ambient_color=0x55606e),
        s.node("DirectionalLight3D", "Sun", rotation=(-55, -30, 0), light_energy=1.0),
        s.node("Camera3D", "Camera3D", position=(0, 7, 12), rotation=(-25, 0, 0), fov=60.0, current=True,
               script="res://scripts/follow_camera.jet", script_properties={}),
        s.box_body("StaticBody3D", "Floor", (0, -0.5, 0), (20, 1, 20), 0x3d5a3d,
                   mesh_props={"draw_order": "Background"}),  # big floor: drawn first (no z-buffer on the device)
        s.box_body("StaticBody3D", "WallNorth", (0, 1, -10), (20, 3, 1), 0x6b5a4a),
        s.box_body("StaticBody3D", "WallWest", (-10, 1, 0), (1, 3, 20), 0x6b5a4a),
        s.box_body("StaticBody3D", "Ramp", (6, 0.9, -2), (6, 0.4, 3), 0x8a7a5a, rotation=(0, 0, 18)),
        s.box_body("StaticBody3D", "Ledge", (10.5, 1.8, -2), (4, 0.4, 3), 0x8a7a5a),
        s.node("StaticBody3D", "Platform", [s.node("CollisionShape3D", "CollisionShape3D", shape="Box", size=(3, 0.3, 3)),
                                            s.mesh("Box", 0x4a7ab0, (3, 0.3, 3))],
               position=(-3, 0.75, -6), script="res://scripts/platform.jet", script_properties={}),
    ]
    for i, (x, y, z) in enumerate([(-3, 0.5, 2), (-3, 1.5, 2), (-1.9, 0.5, 2), (-4.1, 0.5, 2)]):
        nodes.append(s.box_body("RigidBody3D", f"Crate{i + 1}", (x, y, z), (1, 1, 1), 0xb07a3a, mass=2.0))
    for i, (x, z, color) in enumerate([(2, 3, 0xe04848), (3, 1, 0x48a0e0), (1, 0, 0xe0c848)]):
        nodes.append(s.node("RigidBody3D", f"Ball{i + 1}", [s.node("CollisionShape3D", "CollisionShape3D", shape="Sphere", radius=0.4),
                                                           s.mesh("Sphere", color, (0.8, 0.8, 0.8), segments=16)],
                            position=(x, 2 + i, z), bounce=0.7, mass=0.5))
    # The coin above the platform can only be reached by jumping from the platform.
    for i, (x, y, z) in enumerate([(4, 0.8, 4), (-6, 0.8, -3), (10.5, 2.8, -2), (-3, 3.5, -6), (7, 0.8, 6)]):
        nodes.append(s.node("Area3D", f"Coin{i + 1}", [s.node("CollisionShape3D", "CollisionShape3D", shape="Sphere", radius=0.5),
                                                      s.mesh("Torus", 0xffd040, (0.45, 0.45, 0.45), rotation=(90, 0, 0),
                                                             shading="Phong", specular=0.9)],
                            position=(x, y, z), script="res://scripts/coin.jet", script_properties={}))
    # PickupSparks is one-shot; coin.jet moves it and calls restart().
    nodes.append(s.node("CPUParticles3D", "Fountain", position=(2.5, 0.2, -6.5), amount=80, lifetime=1.2, direction=(0, 1, 0),
                        spread=10.0, initial_velocity_min=4.5, initial_velocity_max=5.5, lifetime_randomness=0.3,
                        particle_style="Puff", color=0xa8d8ff, color_end=0x3a7bd5, alpha=0.9, puff_size=0.3,
                        puff_size_end=0.2, emission_shape="Sphere", emission_sphere_radius=0.15))
    nodes.append(s.node("MeshInstance3D", "FountainBase", position=(2.5, 0.1, -6.5), mesh="Cylinder", color=0x6b7a8a,
                        scale=(1.2, 0.2, 1.2)))
    nodes.append(s.node("CPUParticles3D", "PickupSparks", position=(0, -5, 0), emitting=False, one_shot=True, amount=40,
                        lifetime=0.6, explosiveness=1.0, direction=(0, 1, 0), spread=180.0, initial_velocity_min=2.0,
                        initial_velocity_max=4.0, gravity=(0, -6, 0), particle_style="Sparks", additive=True))
    nodes.append(s.node("CharacterBody3D", "Player", [s.node("CollisionShape3D", "CollisionShape3D", shape="Capsule", radius=0.4, height=1.6),
                                                     s.mesh("Capsule", 0xf0f0f0, (0.8, 0.8, 0.8), shading="Phong")],
                        position=(0, 2, 4), script="res://scripts/player.jet", script_properties={}))
    nodes.append(s.node("Label", "HUD", position=(8, 6), text="COINS 0/5", color=0xffffff, text_scale=2,
                        script="res://scripts/hud.jet", script_properties={}))
    nodes.append(s.node("Label", "Help", position=(8, 220), text="Arrows/WASD: move  Space: jump", color=0xc8d8e8))
    root = {"id": root_id, "name": "World", "type": "Node3D", "children": nodes}
    return {"format": "jetscene", "version": 2, "root": root}


def build_win_scene():
    s = Scene()
    root_id = s.next_id
    s.next_id += 1
    nodes = [
        s.node("WorldEnvironment", "WorldEnvironment", background_mode="Gradient", sky_top_color=0x101a3a,
               sky_horizon_color=0x3a5a9a, ground_horizon_color=0x3a5a9a, ground_bottom_color=0x101a3a, ambient_color=0x55606e),
        s.node("DirectionalLight3D", "Sun", rotation=(-40, -30, 0), light_energy=1.2),
        s.node("Camera3D", "Camera3D", position=(0, 0, 4), fov=60.0, current=True),
        s.node("MeshInstance3D", "Coin", mesh="Torus", color=0xffd040, rotation=(90, 0, 0), shading="Phong", specular=0.9),
        s.node("Label", "Title", position=(56, 30), text="WELL DONE!", color=0xffffff, text_scale=3),
        s.node("Label", "Wins", position=(124, 180), text="Wins: 0", color=0xffd040),
        s.node("Label", "Hint", position=(92, 204), text="Space: play again", color=0xc8d8e8),
    ]
    root = {"id": root_id, "name": "Win", "type": "Node3D", "script": "res://scripts/win.jet", "script_properties": {},
            "children": nodes}
    return {"format": "jetscene", "version": 2, "root": root}


def main():
    (PROJECT / "scripts").mkdir(parents=True, exist_ok=True)
    settings = PROJECT / "project.jetproject"
    # Keep the target, hardware and Input Map chosen in the editor.
    config = json.loads(settings.read_text()) if settings.exists() else {
        "main_scene": "res://main.jetscene", "name": "Physics demo", "target": "desktop"}
    config.setdefault("autoload", [{"name": "Game", "path": "res://scripts/game.jet"}])
    settings.write_text(json.dumps(config, indent="\t") + "\n")
    for name, text in SCRIPTS.items():
        (PROJECT / "scripts" / name).write_text(text)
    (PROJECT / "main.jetscene").write_text(json.dumps(build_scene(), indent="\t") + "\n")
    (PROJECT / "win.jetscene").write_text(json.dumps(build_win_scene(), indent="\t") + "\n")
    print(f"Generated {PROJECT.relative_to(ROOT)}")


if __name__ == "__main__":
    main()
