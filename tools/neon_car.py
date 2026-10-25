#!/usr/bin/env python3
"""Builds examples/neon_car, a port of CubeCoders' "Neon Motorworks" demo (JetExamples/esp32-neon-car, MIT).

The car model and livery have no licence, so they never go in the repo.
--import-car copies them from the local JetExamples checkout into examples/neon_car/car/, which Git ignores.
The demo uses Jet units (1 m = 100) in a left-handed space, so z gets negated when converting to the editor's metres.
"""
import argparse
import json
import math
from pathlib import Path
import shutil
import struct
import zlib

ROOT = Path(__file__).resolve().parents[1]
PROJECT = ROOT / "examples/neon_car"
SOURCE = ROOT / "third_party/JetExamples/esp32-neon-car/assets"
CAR_FILES = ["Nascar Intel - Separate Windows.obj", "Nascar Intel - Separate Windows.mtl", "outrun.png"]
CAR_OBJ = "res://car/" + CAR_FILES[0]
CAR_TEXTURE = "res://car/outrun.png"
ENVIRONMENT = "res://textures/workshop_env.png"

# Demo palette (Workshop.hpp).
FLOOR, WALL, WALL_ALT, FRAME = 0x19283A, 0x141C2F, 0x231733, 0x26354A
CYAN, PINK, WHITE = 0x32D7EC, 0xE542BC, 0xA4C5D8
GRID, DECK, SHADOW = 0x2D4E60, 0x2A3C51, 0x121D2D

ORBIT_SCRIPT = '''extends Camera3D
## Orbit camera of the "Neon Motorworks" demo: one turn every 20 s, radius and height slowly breathing.

@export var period := 20.0
@export var target := Vector3(0.0, 1.75, 0.0)
var time := 0.0


func _process(delta: float) -> void:
	time = fmod(time + delta, 3600.0)
	var angle := 0.7 + time * TAU / period
	var radius := 14.6 + 0.8 * sin(time * TAU / 40.0)
	position = Vector3(radius * sin(angle), 4.8 + 0.6 * sin(angle * 2.0), -radius * cos(angle))
	look_at(target)
'''


class Scene:
    def __init__(self):
        self.next_id = 1

    def node(self, type_name, name, children=None, **props):
        node = {"id": self.next_id, "name": name, "type": type_name}
        self.next_id += 1
        for key, value in props.items():
            node[key] = [float(v) for v in value] if isinstance(value, (list, tuple)) else value
        node["children"] = children or []
        return node

    def unlit(self, name, mesh, position, scale, color, rotation=(0, 0, 0), **props):
        # Like in the demo, the room is a background band: drawn first and never over the car, so no depth-sort fights.
        return self.node("MeshInstance3D", name, mesh=mesh, position=position, rotation=rotation, scale=scale,
                         color=color, shading="Unlit", draw_order="Background", **props)


def godot(x, y, z):
    """Demo point in Jet units (left-handed) to metres in Godot space."""
    return (round(x / 100, 4), round(y / 100, 4), round(-z / 100, 4))


def build_scene():
    s = Scene()
    root_id = s.next_id
    s.next_id += 1
    environment = s.node("WorldEnvironment", "WorldEnvironment", background_mode="Color", background_color=0x0F1827,
                         ambient_color=0x827B96)
    # Jet light: azimuth 220°, elevation 52° (towards the light) = Godot rotation (-52°, -50°, 0).
    sun = s.node("DirectionalLight3D", "KeyLight", rotation=(-52, -50, 0), light_color=0xCDE7FF, light_energy=1.16)
    # The demo's 62° horizontal FOV on 4:3 is 48.5° vertical; near 96 and far 6000 Jet units.
    # Start at the orbit's first frame, the script moves the camera from there.
    eye = (14.6 * math.sin(0.7), 4.8 + 0.6 * math.sin(1.4), -14.6 * math.cos(0.7))
    look = (-eye[0], 1.75 - eye[1], -eye[2])
    yaw = math.degrees(math.atan2(-look[0], -look[2]))
    pitch = math.degrees(math.atan2(look[1], math.hypot(look[0], look[2])))
    camera = s.node("Camera3D", "OrbitCamera", position=[round(c, 3) for c in eye], rotation=(round(pitch, 2), round(yaw, 2), 0),
                    fov=48.5, near=0.96, far=60.0, current=True, script="res://scripts/orbit.jet", script_properties={})

    room = [s.unlit("Floor", "Plane", (0, 0, 0), (23, 1, 23), FLOOR, cull_mode="Disabled")]
    walls = []
    for i in range(8):
        mid = 2 * math.pi * (i + 0.5) / 8
        radius = 1900 * math.cos(math.pi / 8)
        walls.append(s.unlit(f"Wall{i + 1}", "Quad", godot(radius * math.sin(mid), 500, radius * math.cos(mid)),
                             (2 * 19 * math.sin(math.pi / 8), 10, 1), WALL if i % 2 else WALL_ALT,
                             rotation=(0, round(-math.degrees(mid), 3), 0), cull_mode="Disabled"))
    room.append(s.node("Node3D", "Walls", walls))
    lines = []
    for n in range(-4, 5):
        lines.append(s.unlit(f"LineX{n + 5}", "Plane", (n * 4.4, 0.02, 0), (0.02, 1, 18), GRID))
        lines.append(s.unlit(f"LineZ{n + 5}", "Plane", (0, 0.02, -n * 4.4), (18, 1, 0.02), GRID))
    room.append(s.node("Node3D", "FloorGrid", lines))
    # Octagonal service platform with a neon rim (cyan/pink in 45° arcs) and a contact shadow.
    room.append(s.unlit("Platform", "Cylinder", (0, 0.09, 0), (14.4, 0.09, 14.4), DECK, segments=32))
    rim = []
    for i in range(32):
        mid = 2 * math.pi * (i + 0.5) / 32
        radius = 689.5
        rim.append(s.unlit(f"Neon{i + 1}", "Box", godot(radius * math.sin(mid), 19, radius * math.cos(mid)),
                           (1.37, 0.012, 0.09), CYAN if i % 8 < 4 else PINK, rotation=(0, round(-math.degrees(mid), 3), 0)))
    room.append(s.node("Node3D", "NeonRim", rim))
    room.append(s.unlit("Shadow", "Plane", (0, 0.2, 0), (1.8, 1, 4.65), SHADOW))
    ribs = []
    for i in range(8):
        a = 2 * math.pi * i / 8
        x, z = 1780 * math.sin(a), 1780 * math.cos(a)
        ribs.append(s.unlit(f"Frame{i + 1}", "Box", godot(x, 425, z), (0.45, 8.5, 0.45), FRAME))
        ribs.append(s.unlit(f"Tube{i + 1}", "Box", godot(x * 0.992, 435, z * 0.992), (0.13, 6.3, 0.13), PINK if i % 2 else CYAN))
        ribs.append(s.unlit(f"Panel{i + 1}", "Box", godot(x * 0.86, 855, z * 0.86), (2.2, 0.15, 1.2), WHITE))
    room.append(s.node("Node3D", "Ribs", ribs))

    # One node per OBJ object; the import options live in car/*.import.
    def part(name, **props):
        return s.node("MeshInstance3D", name, mesh="File", mesh_file=CAR_OBJ, mesh_part=name, color=0xFFFFFF,
                      perspective_correct=True, **props)
    body = part("Nascar", albedo_texture=CAR_TEXTURE, shading="Phong", diffuse=0.65, specular=0.8, shininess=32)
    windows = [part(name, albedo_texture=ENVIRONMENT, shading="Unlit", reflection=True)
               for name in ("Front_Window", "Rear_Window")]
    wheels = [part(name, albedo_texture=CAR_TEXTURE, shading="Unlit")
              for name in ("Nascar_wheel", "Nascar_wheel_001", "Nascar_wheel_002", "Nascar_wheel_003")]
    # The demo turns the model half a turn (its X mirror on top of Jet's Z flip).
    car = s.node("Node3D", "Car", [body] + windows + wheels, position=(0, 0.52, 0), rotation=(0, 180, 0))

    title = s.node("Label", "Title", position=(12, 10), text="NEON MOTORWORKS", color=0xDEF5FF, text_scale=2)
    caption = s.node("Label", "Caption", position=(12, 216), text="OBJ / PHONG PAINT / CHROME GLASS", color=0x8ED5ED)
    root = {"id": root_id, "name": "Workshop", "type": "Node3D",
            "children": [environment, sun, camera, s.node("Node3D", "Room", room), car, title, caption]}
    return {"format": "jetscene", "version": 2, "root": root}


def write_png(path, width, height, rows):
    raw = b"".join(b"\0" + bytes(c for pixel in row for c in pixel) for row in rows)
    chunk = lambda kind, data: struct.pack(">I", len(data)) + kind + data + struct.pack(">I", zlib.crc32(kind + data))
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 2, 0, 0, 0))
                     + chunk(b"IDAT", zlib.compress(raw, 9)) + chunk(b"IEND", b""))


def workshop_panorama():
    """128x64 equirectangular workshop for the chrome windows: neon ribs, ceiling panels and a floor glow."""
    rows = []
    for y in range(64):
        v = (y + 0.5) / 64
        row = []
        for x in range(128):
            u = (x + 0.5) / 128
            color = [22, 34, 54] if v < 0.5 else [12, 21, 35]
            for i in range(8):
                du = abs((u - (0.5 + i / 8) % 1 + 0.5) % 1 - 0.5)
                core = max(0.0, 1 - du / 0.016) * max(0.0, 1 - abs(v - 0.34) / 0.24)
                halo = max(0.0, 1 - du / 0.055) * 0.28 * max(0.0, 1 - abs(v - 0.35) / 0.32)
                tint = (55, 225, 255) if i % 2 == 0 else (240, 45, 195)
                for c in range(3):
                    color[c] += tint[c] * (core + halo)
            if int(u * 16) % 2 == 0:
                panel = max(0.0, 1 - abs(v - 0.22) / 0.024)
                for c in range(3):
                    color[c] += panel * 205
            glow = max(0.0, 1 - abs(v - 0.62) / 0.028)
            for c, amount in enumerate((30, 50, 85)):
                color[c] += glow * amount
            row.append(tuple(min(255, int(c)) for c in color))
        rows.append(row)
    return rows


def generate():
    (PROJECT / "scripts").mkdir(parents=True, exist_ok=True)
    settings = PROJECT / "project.jetproject"
    if not settings.exists():  # keep the target/profile chosen in the editor
        settings.write_text(json.dumps({"main_scene": "res://main.jetscene", "name": "Neon car", "target": "desktop"}, indent="\t") + "\n")
    (PROJECT / "scripts/orbit.jet").write_text(ORBIT_SCRIPT)
    (PROJECT / "main.jetscene").write_text(json.dumps(build_scene(), indent="\t") + "\n")
    write_png(PROJECT / "textures/workshop_env.png", 128, 64, workshop_panorama())
    print(f"Generated {PROJECT.relative_to(ROOT)}")


def import_car():
    missing = [name for name in CAR_FILES if not (SOURCE / name).is_file()]
    if missing:
        raise SystemExit(f"JetExamples checkout missing ({', '.join(missing)}): run ./jet-studio setup first")
    target = PROJECT / "car"
    target.mkdir(parents=True, exist_ok=True)
    for name in CAR_FILES:
        shutil.copy2(SOURCE / name, target / name)
    # Same conversion as the demo: 1 OBJ unit = 2 Jet units = 0.02 m.
    # The model's faces point inwards, hence flip_faces; the livery goes down to 256x256 and 256 colours.
    imports = {CAR_FILES[0]: {"importer": "model", "params": {"scale": 0.02, "mirror_x": False, "flip_faces": True, "flip_v": False}},
               "outrun.png": {"importer": "texture", "params": {"max_size": "256", "palette": "Quantize", "flip_v": False}}}
    for name, data in imports.items():
        (target / (name + ".import")).write_text(json.dumps(data, indent="\t") + "\n")
    print(f"Car copied to {target.relative_to(ROOT)} (local only: the model and livery have no licence, do not commit them)")


def main():
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("--import-car", action="store_true", help="copy the car from third_party/JetExamples (local use only)")
    parser.add_argument("--no-generate", action="store_true", help="keep the committed scene and textures")
    args = parser.parse_args()
    if not args.no_generate:
        generate()
    if args.import_car:
        import_car()


if __name__ == "__main__":
    main()
