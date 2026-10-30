extends RefCounted
## .jetscene v2: a tree of node dicts with Godot's classes and conventions (metres, Y up, -Z forward, degrees, YXZ).

const VERSION := 2
const TYPES := ["Node", "Node3D", "MeshInstance3D", "Sprite3D", "Camera3D", "DirectionalLight3D", "WorldEnvironment",
	"Node2D", "Sprite2D", "AnimatedSprite2D", "Camera2D", "StaticBody2D", "CharacterBody2D", "RigidBody2D", "Area2D", "CollisionShape2D", "Control", "Label", "TextureRect", "ColorRect", "StaticBody3D", "CharacterBody3D", "RigidBody3D", "Area3D", "CollisionShape3D",
	"CPUParticles3D", "Timer", "AnimationPlayer"]
const BASE := {"Node": "", "Node3D": "Node", "MeshInstance3D": "Node3D", "Camera3D": "Node3D",
	"DirectionalLight3D": "Node3D", "WorldEnvironment": "Node", "Control": "Node", "Label": "Control",
	"TextureRect": "Control", "ColorRect": "Control", "CollisionObject3D": "Node3D", "PhysicsBody3D": "CollisionObject3D",
	"StaticBody3D": "PhysicsBody3D", "CharacterBody3D": "PhysicsBody3D", "RigidBody3D": "PhysicsBody3D",
	"Area3D": "CollisionObject3D", "CollisionShape3D": "Node3D", "CPUParticles3D": "Node3D", "Sprite3D": "Node3D",
	"Node2D": "Node", "Sprite2D": "Node2D", "AnimatedSprite2D": "Node2D", "Camera2D": "Node2D",
	"CollisionObject2D": "Node2D", "PhysicsBody2D": "CollisionObject2D", "StaticBody2D": "PhysicsBody2D",
	"CharacterBody2D": "PhysicsBody2D", "RigidBody2D": "PhysicsBody2D", "Area2D": "CollisionObject2D", "CollisionShape2D": "Node2D",
	"Timer": "Node", "AnimationPlayer": "Node"}
## Base classes shown in the class tree but not instantiable.
const ABSTRACT := ["CollisionObject3D", "PhysicsBody3D", "CollisionObject2D", "PhysicsBody2D"]
const BODIES := ["StaticBody3D", "CharacterBody3D", "RigidBody3D", "Area3D", "StaticBody2D", "CharacterBody2D", "RigidBody2D", "Area2D"]
const DESCRIPTIONS := {
	"Node": "Base node. Useful to organize the scene or for global logic scripts.",
	"Node3D": "3D object with a transform (position, rotation, scale). Base of all 3D nodes.",
	"MeshInstance3D": "Shows a primitive mesh drawn by the Jet renderer.",
	"Sprite3D": "A PNG or one frame of a sprite sheet in the 3D world (trees, characters, cars). With Billboard it always faces the camera.",
	"Camera3D": "The game's point of view. The 'current' camera is used in Play and on the device.",
	"DirectionalLight3D": "Directional light (sun). Shines along its own -Z axis.",
	"WorldEnvironment": "Background color and ambient light of the scene.",
	"Node2D": "2D object with a position in canvas pixels (y down), a rotation and a scale. Base of the 2D game nodes; drawn over the 3D scene, under the Controls.",
	"Sprite2D": "A PNG or one frame of a sprite sheet in the 2D world. z_index and the tree decide what is in front.",
	"AnimatedSprite2D": "A sprite sheet with named animations (frame ranges and speed): play(\"run\") from scripts, or autoplay.",
	"Camera2D": "Moves the 2D view: its position is shown at the screen centre, zoom scales. Without one, the canvas origin is the top-left corner.",
	"CollisionObject2D": "Base of 2D bodies and areas: child collision shapes, layer and mask.",
	"PhysicsBody2D": "Base of the 2D physics bodies that collide with each other.",
	"StaticBody2D": "Static 2D body (ground, walls, platforms). Scripts can move it.",
	"CharacterBody2D": "2D character moved by its script with velocity and move_and_slide(): slides along walls, knows when it is on the floor (y down, gravity is up to the script).",
	"RigidBody2D": "2D physics body: gravity (980 px/s²), mass, bounce, friction, collisions (arcade: rectangles don't tip over, circles roll).",
	"Area2D": "2D region that detects bodies and other areas: the body_entered and area_entered signals (or _on_body_entered(body) in its script), get_overlapping_bodies().",
	"CollisionShape2D": "Collision shape (Rectangle, Circle, Capsule) of a 2D body or area: add it as a child. One Way makes a platform you can jump through.",
	"Control": "Base of the 2D nodes drawn over the 3D scene, in screen pixels (320×240). On its own it groups a HUD: children are placed relative to it, hidden and faded with it.",
	"Label": "Text in the runtime's bitmap font (titles, scores, HUD). Can be changed from scripts.",
	"TextureRect": "PNG image drawn over the scene, with integer scaling and transparency.",
	"ColorRect": "Solid or semi-transparent color rectangle (fades, bars, panels).",
	"CollisionObject3D": "Base of bodies and areas: child collision shapes, layer and mask.",
	"PhysicsBody3D": "Base of the physics bodies that collide with each other.",
	"StaticBody3D": "Static body (floors, walls, platforms). Scripts can move it.",
	"CharacterBody3D": "Character moved by its script with velocity and move_and_slide(): slides along walls, knows when it is on the floor.",
	"RigidBody3D": "Physics body: gravity, mass, bounce, friction, collisions (arcade physics: boxes don't tip over, spheres roll).",
	"Area3D": "Region that detects bodies and areas: the body_entered and body_exited signals (or _on_body_entered(body) in its script), get_overlapping_bodies().",
	"CollisionShape3D": "Collision shape (Box, Sphere, Capsule) of a body or an area: add it as a child.",
	"CPUParticles3D": "Emits particles (sparks, splashes, fountains) drawn by Jet's particle system. Visible in Play.",
	"AnimationPlayer": "Plays animations of the properties of the nodes around it, and calls their methods at given times. Edit them in the Animation panel at the bottom; the key icons in the Inspector add keys.",
	"Timer": "Counts down Wait Time and emits timeout: connect it in the Signals dock, or in a script ($Timer.timeout.connect(_on_timeout)). One Shot stops after the first time.",
}
const MESHES := ["Box", "Sphere", "Plane", "Pyramid", "Cylinder", "Capsule", "Quad", "Torus", "File"]
const MESH_ICONS := {"Box": "BoxMesh", "Sphere": "SphereMesh", "Plane": "PlaneMesh", "Pyramid": "PrismMesh",
	"Cylinder": "CylinderMesh", "Capsule": "CapsuleMesh", "Quad": "QuadMesh", "Torus": "TorusMesh", "File": "ArrayMesh"}
const SHADING := ["Gouraud", "Flat", "Unlit", "Phong", "Additive", "Water"]
const IMAGE_EXTENSIONS := ["png"]
const MODEL_EXTENSIONS := ["obj"]
## Bitmap font cell of the runtime (runtime/font_data.hpp), for 2D gizmos.
const FONT_SIZE := Vector2i(7, 15)
const SCREEN := Vector2i(320, 240)
## Local bounds of the unit primitives (Godot default mesh sizes), for gizmos.
const MESH_BOUNDS := {
	"Box": [Vector3(-0.5, -0.5, -0.5), Vector3(0.5, 0.5, 0.5)],
	"Sphere": [Vector3(-0.5, -0.5, -0.5), Vector3(0.5, 0.5, 0.5)],
	"Plane": [Vector3(-1, 0, -1), Vector3(1, 0, 1)],
	"Pyramid": [Vector3(-0.5, -0.5, -0.5), Vector3(0.5, 0.5, 0.5)],
	"Cylinder": [Vector3(-0.5, -1, -0.5), Vector3(0.5, 1, 0.5)],
	"Capsule": [Vector3(-0.5, -1, -0.5), Vector3(0.5, 1, 0.5)],
	"Quad": [Vector3(-0.5, -0.5, 0), Vector3(0.5, 0.5, 0)],
	"Torus": [Vector3(-1, -0.25, -1), Vector3(1, 0.25, 1)],
}

## Import settings stored next to each asset in "<file>.import" (as in Godot).
const IMPORT_OPTIONS := {
	"texture": [
		{"key": "max_size", "label": "Max Size", "type": "enum", "default": "Off", "options": ["Off", "1024", "512", "256", "128", "64", "32"],
			"tooltip": "Shrinks the image if its longest side exceeds this value (keeping the aspect ratio)."},
		{"key": "palette", "type": "enum", "default": "Auto", "options": ["Auto", "Quantize", "RGB565"],
			"tooltip": "Auto: 8-bit palette if the image already has at most 256 colors, otherwise RGB565.\nQuantize: always reduces to 256 colors (half the memory, small loss).\nRGB565: always 16 bits per pixel."},
		{"key": "flip_v", "label": "Flip V", "type": "bool", "default": false,
			"tooltip": "Flips the image vertically (some model textures are saved upside down)."},
	],
	"model": [
		{"key": "scale", "type": "float", "default": 1.0, "min": 0.0001, "max": 1000.0, "step": 0.0001,
			"tooltip": "Factor from file units to meters (e.g. 0.01 for models in centimeters)."},
		{"key": "mirror_x", "label": "Mirror X", "type": "bool", "default": false,
			"tooltip": "Mirrors the model on the X axis (exports with a flipped coordinate system)."},
		{"key": "flip_faces", "label": "Flip Faces", "type": "bool", "default": false,
			"tooltip": "Reverses the winding of faces and normals (models that look inside out or hollow)."},
		{"key": "flip_v", "label": "Flip V", "type": "bool", "default": false,
			"tooltip": "Don't flip the V coordinate of the UVs (textures already stored top to bottom)."},
	],
}

static func import_kind(path: String) -> String:
	var extension := path.get_extension().to_lower()
	if extension in IMAGE_EXTENSIONS: return "texture"
	if extension in MODEL_EXTENSIONS: return "model"
	return ""

static func import_defaults(kind: String) -> Dictionary:
	var out := {}
	for prop in IMPORT_OPTIONS.get(kind, []): out[prop.key] = prop.default
	return out

## Inspector schema: per class, its own properties (inherited ones come from BASE).
const PROPERTIES := {
	"Node3D": [
		{"key": "position", "type": "vector3", "default": [0.0, 0.0, 0.0], "group": "Transform", "step": 0.01, "suffix": "m",
			"tooltip": "Position relative to the parent node."},
		{"key": "rotation", "type": "vector3", "default": [0.0, 0.0, 0.0], "group": "Transform", "step": 0.1, "suffix": "°",
			"tooltip": "Rotation in degrees (YXZ order, as in Godot)."},
		{"key": "scale", "type": "vector3", "default": [1.0, 1.0, 1.0], "group": "Transform", "step": 0.01, "min": -1000.0, "max": 1000.0,
			"tooltip": "Scale relative to the parent."},
		{"key": "visible", "type": "bool", "default": true, "group": "Visibility",
			"tooltip": "When off, the node and its children are not drawn."},
	],
	"MeshInstance3D": [
		{"key": "mesh", "type": "enum", "default": "Box", "options": MESHES, "group": "Mesh",
			"tooltip": "Primitive to draw (Godot default sizes: 1 m cube, 0.5 m radius sphere...)."},
		{"key": "segments", "type": "int", "default": 12, "min": 3, "max": 48, "group": "Mesh", "visible_if": {"mesh": ["Sphere", "Cylinder", "Capsule"]},
			"tooltip": "Subdivisions: higher = smoother, but more triangles to draw on the microcontroller."},
		{"key": "mesh_file", "label": "File", "type": "model", "default": "", "group": "Mesh", "visible_if": {"mesh": ["File"]},
			"tooltip": "OBJ model of the project. Dragging it into the scene creates one node per part."},
		{"key": "mesh_part", "label": "Part", "type": "part", "default": "", "group": "Mesh", "visible_if": {"mesh": ["File"]},
			"tooltip": "Part of the model drawn by this node (one OBJ object per material)."},
		{"key": "color", "label": "Albedo Color", "type": "color", "default": 0xe6a45c, "group": "Material",
			"tooltip": "Material color; with a texture, it multiplies the texture (as in Godot)."},
		{"key": "albedo_texture", "label": "Albedo Texture", "type": "texture", "default": "", "group": "Material",
			"tooltip": "PNG image of the project. With at most 256 colors it becomes a palette texture (half the memory, lossless)."},
		{"key": "texture_filter", "label": "Filter", "type": "enum", "default": "Nearest", "options": ["Nearest", "Linear"], "group": "Material",
			"visible_if": {"albedo_texture": "*"}, "tooltip": "Linear = bilinear: smoother, four reads per pixel."},
		{"key": "texture_repeat", "label": "Repeat", "type": "bool", "default": true, "group": "Material", "visible_if": {"albedo_texture": "*"}},
		{"key": "uv_scale", "label": "UV Scale", "type": "vector3", "default": [1.0, 1.0, 1.0], "group": "Material", "step": 0.01,
			"visible_if": {"albedo_texture": "*"}, "tooltip": "Texture repetitions (x, y)."},
		{"key": "uv_offset", "label": "UV Offset", "type": "vector3", "default": [0.0, 0.0, 0.0], "group": "Material", "step": 0.01,
			"visible_if": {"albedo_texture": "*"}, "tooltip": "Texture offset (x, y); can be animated from scripts for scrolling textures."},
		{"key": "perspective_correct", "label": "Perspective UV", "type": "bool", "default": true, "group": "Material",
			"visible_if": {"albedo_texture": "*"}, "tooltip": "Off = affine mapping: faster, but the texture warps on slanted faces."},
		{"key": "shading", "type": "enum", "default": "Gouraud", "options": SHADING, "group": "Material",
			"tooltip": "Gouraud: per-vertex lighting. Flat: per face. Unlit: no lighting. Phong: per pixel, with highlights.\nAdditive: glow that adds up (neon, beams). Water: sky reflection with ripples."},
		{"key": "diffuse", "type": "float", "default": 1.0, "min": 0.0, "max": 1.0, "step": 0.01, "group": "Material",
			"visible_if": {"shading": ["Gouraud", "Flat", "Phong"]}, "tooltip": "How much diffuse light the surface reflects (lower = darker and glossier)."},
		{"key": "reflection", "label": "Reflection", "type": "bool", "default": false, "group": "Material",
			"visible_if": {"albedo_texture": "*"},
			"tooltip": "Uses the texture as a reflected panorama (chrome, glass): UVs follow the camera and normals every frame."},
		{"key": "alpha", "label": "Transparency", "type": "float", "default": 1.0, "min": 0.0, "max": 1.0, "step": 0.01, "group": "Material",
			"tooltip": "Opacity (1 = opaque). Below 1, the surface doesn't hide what is behind it."},
		{"key": "specular", "type": "float", "default": 0.5, "min": 0.0, "max": 1.0, "step": 0.01, "group": "Material",
			"visible_if": {"shading": ["Phong", "Water"]}, "tooltip": "Phong: highlight intensity. Water: ripple amplitude."},
		{"key": "shininess", "type": "int", "default": 32, "min": 0, "max": 255, "group": "Material", "visible_if": {"shading": ["Phong"]},
			"tooltip": "Size of the Phong highlight (higher = smaller and sharper)."},
		{"key": "cull_mode", "type": "enum", "default": "Back", "options": ["Back", "Front", "Disabled"], "group": "Geometry",
			"tooltip": "Faces not to draw. Back is the fastest for closed objects; Disabled for planes visible from both sides."},
		{"key": "draw_order", "label": "Draw Order", "type": "enum", "default": "Default", "options": ["Default", "Background", "On Top"], "group": "Geometry",
			"tooltip": "Background: drawn first, in scene order, and never covers the rest (room, floor, sky).\nOn Top: drawn last, over everything.\nOn the device, Default meshes are sorted by distance instead of using a z-buffer:\nmarking large surfaces as Background avoids sorting errors and is faster."},
		{"key": "billboard", "type": "bool", "default": false, "group": "Geometry",
			"tooltip": "The mesh always turns to face the camera (holograms, halos, distant trees)."},
	],
	"Camera3D": [
		{"key": "fov", "label": "FOV", "type": "float", "default": 75.0, "min": 1.0, "max": 179.0, "step": 0.1, "suffix": "°",
			"tooltip": "Vertical field of view in degrees."},
		{"key": "near", "type": "float", "default": 0.05, "min": 0.001, "max": 10.0, "step": 0.001, "suffix": "m"},
		{"key": "far", "type": "float", "default": 100.0, "min": 0.01, "max": 10000.0, "step": 0.1, "suffix": "m"},
		{"key": "current", "type": "bool", "default": false,
			"tooltip": "The camera used by the game. If none is 'current', the first one in the scene is used."},
	],
	"DirectionalLight3D": [
		{"key": "light_color", "label": "Color", "type": "color", "default": 0xffffff, "group": "Light"},
		{"key": "light_energy", "label": "Energy", "type": "float", "default": 1.0, "min": 0.0, "max": 16.0, "step": 0.01, "group": "Light"},
	],
	"WorldEnvironment": [
		{"key": "background_mode", "label": "Mode", "type": "enum", "default": "Color", "options": ["Color", "Gradient", "Panorama"], "group": "Background",
			"tooltip": "Color: solid color. Gradient: sky and ground with a horizon that follows the camera (like ProceduralSkyMaterial).\nPanorama: equirectangular image around the scene (like PanoramaSkyMaterial)."},
		{"key": "background_color", "label": "Color", "type": "color", "default": 0x161c28, "group": "Background", "visible_if": {"background_mode": ["Color"]}},
		{"key": "sky_top_color", "label": "Sky Top", "type": "color", "default": 0x0a1a3a, "group": "Background", "visible_if": {"background_mode": ["Gradient", "Panorama"]}},
		{"key": "sky_horizon_color", "label": "Sky Horizon", "type": "color", "default": 0x3d6aa8, "group": "Background", "visible_if": {"background_mode": ["Gradient", "Panorama"]}},
		{"key": "ground_horizon_color", "label": "Ground Horizon", "type": "color", "default": 0x2a3442, "group": "Background", "visible_if": {"background_mode": ["Gradient", "Panorama"]}},
		{"key": "ground_bottom_color", "label": "Ground Bottom", "type": "color", "default": 0x10141c, "group": "Background", "visible_if": {"background_mode": ["Gradient", "Panorama"]}},
		{"key": "sky_texture", "label": "Panorama", "type": "texture", "default": "", "group": "Background", "visible_if": {"background_mode": ["Panorama"]},
			"tooltip": "Equirectangular image (2:1). With Panorama, the gradient colors are used for water reflections."},
		{"key": "ambient_color", "label": "Color", "type": "color", "default": 0x4e586e, "group": "Ambient Light"},
	],
	"CollisionObject3D": [
		{"key": "collision_layer", "label": "Layer", "type": "int", "default": 1, "min": 0, "max": 2147483647, "group": "Collision",
			"tooltip": "Layers it belongs to (bits: 1 = layer 1, 2 = layer 2, 4 = layer 3; 3 = layers 1 and 2)."},
		{"key": "collision_mask", "label": "Mask", "type": "int", "default": 1, "min": 0, "max": 2147483647, "group": "Collision",
			"tooltip": "Layers it detects or collides with (same bits as Layer)."},
	],
	"StaticBody3D": [
		{"key": "friction", "type": "float", "default": 1.0, "min": 0.0, "max": 10.0, "step": 0.01, "group": "Physics Material"},
		{"key": "bounce", "type": "float", "default": 0.0, "min": 0.0, "max": 1.0, "step": 0.01, "group": "Physics Material"},
	],
	"CharacterBody3D": [
		{"key": "floor_max_angle", "label": "Floor Max Angle", "type": "float", "default": 45.0, "min": 0.0, "max": 89.0, "step": 0.1, "suffix": "°",
			"group": "Floor", "tooltip": "Steepest slope still treated as floor (anything steeper is a wall to slide along)."},
	],
	"RigidBody3D": [
		{"key": "mass", "type": "float", "default": 1.0, "min": 0.001, "max": 10000.0, "step": 0.01, "suffix": "kg", "group": "Mass"},
		{"key": "gravity_scale", "type": "float", "default": 1.0, "min": -10.0, "max": 10.0, "step": 0.01, "group": "Mass"},
		{"key": "friction", "type": "float", "default": 1.0, "min": 0.0, "max": 10.0, "step": 0.01, "group": "Physics Material"},
		{"key": "bounce", "type": "float", "default": 0.0, "min": 0.0, "max": 1.0, "step": 0.01, "group": "Physics Material"},
		{"key": "linear_velocity", "type": "vector3", "default": [0.0, 0.0, 0.0], "step": 0.01, "suffix": "m/s", "group": "Linear"},
		{"key": "linear_damp", "type": "float", "default": 0.0, "min": 0.0, "max": 100.0, "step": 0.01, "group": "Linear"},
		{"key": "angular_velocity", "type": "vector3", "default": [0.0, 0.0, 0.0], "step": 0.01, "suffix": "rad/s", "group": "Angular"},
		{"key": "angular_damp", "type": "float", "default": 0.0, "min": 0.0, "max": 100.0, "step": 0.01, "group": "Angular"},
		{"key": "freeze", "type": "bool", "default": false, "group": "Deactivation", "tooltip": "Frozen: the body does not move while this is on."},
	],
	"Area3D": [
		{"key": "monitoring", "type": "bool", "default": true, "tooltip": "Detects bodies entering and exiting."},
	],
	"CollisionShape3D": [
		{"key": "shape", "type": "enum", "default": "Box", "options": ["Box", "Sphere", "Capsule"], "group": "Shape"},
		{"key": "size", "type": "vector3", "default": [1.0, 1.0, 1.0], "step": 0.01, "min": 0.001, "max": 1000.0, "suffix": "m", "group": "Shape",
			"visible_if": {"shape": ["Box"]}},
		{"key": "radius", "type": "float", "default": 0.5, "min": 0.001, "max": 1000.0, "step": 0.01, "suffix": "m", "group": "Shape",
			"visible_if": {"shape": ["Sphere", "Capsule"]}},
		{"key": "height", "type": "float", "default": 2.0, "min": 0.001, "max": 1000.0, "step": 0.01, "suffix": "m", "group": "Shape",
			"visible_if": {"shape": ["Capsule"]}, "tooltip": "Total height of the capsule (caps included), as in Godot."},
		{"key": "disabled", "type": "bool", "default": false},
	],
	"CollisionObject2D": [
		{"key": "collision_layer", "label": "Layer", "type": "int", "default": 1, "min": 0, "max": 2147483647, "group": "Collision",
			"tooltip": "Layers it belongs to (bits: 1 = layer 1, 2 = layer 2, 4 = layer 3; 3 = layers 1 and 2)."},
		{"key": "collision_mask", "label": "Mask", "type": "int", "default": 1, "min": 0, "max": 2147483647, "group": "Collision",
			"tooltip": "Layers it detects or collides with (same bits as Layer)."},
	],
	"StaticBody2D": [
		{"key": "friction", "type": "float", "default": 1.0, "min": 0.0, "max": 10.0, "step": 0.01, "group": "Physics Material"},
		{"key": "bounce", "type": "float", "default": 0.0, "min": 0.0, "max": 1.0, "step": 0.01, "group": "Physics Material"},
	],
	"CharacterBody2D": [
		{"key": "floor_max_angle", "label": "Floor Max Angle", "type": "float", "default": 45.0, "min": 0.0, "max": 89.0, "step": 0.1, "suffix": "°",
			"group": "Floor", "tooltip": "Steepest slope still treated as floor (anything steeper is a wall to slide along)."},
	],
	"RigidBody2D": [
		{"key": "mass", "type": "float", "default": 1.0, "min": 0.001, "max": 10000.0, "step": 0.01, "suffix": "kg", "group": "Mass"},
		{"key": "gravity_scale", "type": "float", "default": 1.0, "min": -10.0, "max": 10.0, "step": 0.01, "group": "Mass"},
		{"key": "friction", "type": "float", "default": 1.0, "min": 0.0, "max": 10.0, "step": 0.01, "group": "Physics Material"},
		{"key": "bounce", "type": "float", "default": 0.0, "min": 0.0, "max": 1.0, "step": 0.01, "group": "Physics Material"},
		{"key": "linear_velocity", "type": "vector2", "default": [0.0, 0.0], "step": 1.0, "suffix": "px/s", "group": "Linear"},
		{"key": "linear_damp", "type": "float", "default": 0.0, "min": 0.0, "max": 100.0, "step": 0.01, "group": "Linear"},
		{"key": "angular_velocity", "type": "float", "default": 0.0, "min": -100.0, "max": 100.0, "step": 0.01, "suffix": "rad/s", "group": "Angular"},
		{"key": "angular_damp", "type": "float", "default": 0.0, "min": 0.0, "max": 100.0, "step": 0.01, "group": "Angular"},
		{"key": "freeze", "type": "bool", "default": false, "group": "Deactivation", "tooltip": "Frozen: the body does not move while this is on."},
	],
	"Area2D": [
		{"key": "monitoring", "type": "bool", "default": true, "tooltip": "Detects bodies and areas entering and exiting."},
	],
	"CollisionShape2D": [
		{"key": "shape", "type": "enum", "default": "Rectangle", "options": ["Rectangle", "Circle", "Capsule"], "group": "Shape"},
		{"key": "size", "type": "vector2", "default": [20.0, 20.0], "step": 1.0, "min": 0.1, "max": 100000.0, "suffix": "px", "group": "Shape",
			"visible_if": {"shape": ["Rectangle"]}},
		{"key": "radius", "type": "float", "default": 10.0, "min": 0.1, "max": 100000.0, "step": 1.0, "suffix": "px", "group": "Shape",
			"visible_if": {"shape": ["Circle", "Capsule"]}},
		{"key": "height", "type": "float", "default": 30.0, "min": 0.1, "max": 100000.0, "step": 1.0, "suffix": "px", "group": "Shape",
			"visible_if": {"shape": ["Capsule"]}, "tooltip": "Total height of the capsule (caps included), as in Godot."},
		{"key": "one_way_collision", "label": "One Way Collision", "type": "bool", "default": false,
			"tooltip": "Bodies only land on it from above (its -y side) and pass through from below: jump-through platforms."},
		{"key": "disabled", "type": "bool", "default": false},
	],
	"AnimationPlayer": [
		{"key": "speed_scale", "label": "Speed Scale", "type": "float", "default": 1.0, "min": -100.0, "max": 100.0, "step": 0.01,
			"tooltip": "Multiplies the speed of every animation (negative: backwards)."},
	],
	"Timer": [
		{"key": "wait_time", "type": "float", "default": 1.0, "min": 0.001, "max": 100000.0, "step": 0.001, "suffix": "s",
			"tooltip": "Seconds between start() (or Autostart) and the timeout signal."},
		{"key": "one_shot", "type": "bool", "default": false, "tooltip": "Stops after the first timeout; otherwise it starts again."},
		{"key": "autostart", "type": "bool", "default": false, "tooltip": "Starts when the scene starts (or when the node enters the tree)."},
	],
	"CPUParticles3D": [
		{"key": "emitting", "type": "bool", "default": true, "tooltip": "Emits particles (with One Shot, turns false at the end of the cycle)."},
		{"key": "amount", "type": "int", "default": 8, "min": 1, "max": 200,
			"tooltip": "Particles per cycle (Lifetime), at most 200. Each takes about 40 bytes of RAM."},
		{"key": "lifetime", "type": "float", "default": 1.0, "min": 0.01, "max": 600.0, "step": 0.01, "suffix": "s", "group": "Time"},
		{"key": "one_shot", "type": "bool", "default": false, "group": "Time", "tooltip": "A single cycle, then Emitting turns false (restart() to repeat)."},
		{"key": "explosiveness", "type": "float", "default": 0.0, "min": 0.0, "max": 1.0, "step": 0.01, "group": "Time",
			"tooltip": "0 = continuous emission, 1 = all particles at once at the start of the cycle."},
		{"key": "lifetime_randomness", "type": "float", "default": 0.0, "min": 0.0, "max": 1.0, "step": 0.01, "group": "Time"},
		{"key": "emission_shape", "type": "enum", "default": "Point", "options": ["Point", "Sphere", "Box"], "group": "Emission Shape"},
		{"key": "emission_sphere_radius", "label": "Sphere Radius", "type": "float", "default": 1.0, "min": 0.0, "max": 1000.0, "step": 0.01,
			"suffix": "m", "group": "Emission Shape", "visible_if": {"emission_shape": ["Sphere"]}},
		{"key": "emission_box_extents", "label": "Box Extents", "type": "vector3", "default": [1.0, 1.0, 1.0], "step": 0.01, "min": 0.0, "max": 1000.0,
			"suffix": "m", "group": "Emission Shape", "visible_if": {"emission_shape": ["Box"]}, "tooltip": "Half the size of the box, as in Godot."},
		{"key": "direction", "type": "vector3", "default": [1.0, 0.0, 0.0], "step": 0.01, "min": -1000.0, "max": 1000.0, "group": "Direction"},
		{"key": "spread", "type": "float", "default": 45.0, "min": 0.0, "max": 180.0, "step": 0.1, "suffix": "°", "group": "Direction",
			"tooltip": "Half-angle of the cone around Direction."},
		{"key": "gravity", "type": "vector3", "default": [0.0, -9.8, 0.0], "step": 0.01, "min": -1000.0, "max": 1000.0, "suffix": "m/s²", "group": "Gravity"},
		{"key": "initial_velocity_min", "label": "Velocity Min", "type": "float", "default": 0.0, "min": 0.0, "max": 1000.0, "step": 0.01,
			"suffix": "m/s", "group": "Initial Velocity"},
		{"key": "initial_velocity_max", "label": "Velocity Max", "type": "float", "default": 0.0, "min": 0.0, "max": 1000.0, "step": 0.01,
			"suffix": "m/s", "group": "Initial Velocity",
			"tooltip": "Starting speed along Direction (within the Spread cone). With 0, particles start at rest\nand move only by gravity: all on the same line."},
		{"key": "damping_min", "label": "Min", "type": "float", "default": 0.0, "min": 0.0, "max": 1000.0, "step": 0.01, "suffix": "m/s²", "group": "Damping"},
		{"key": "damping_max", "label": "Max", "type": "float", "default": 0.0, "min": 0.0, "max": 1000.0, "step": 0.01, "suffix": "m/s²", "group": "Damping",
			"tooltip": "Speed lost every second: particles slow down and stop (smoke, dust)."},
		{"key": "particle_style", "label": "Style", "type": "enum", "default": "Sparks", "options": ["Sparks", "Water", "Puff"], "group": "Drawing",
			"tooltip": "Sparks and Water: the styles of Jet's particle system (streaks along the velocity; Jet colors:\nwhite-hot -> blue, foam -> blue). Puff: round puffs that grow and fade out,\nwith the color and opacity chosen here (smoke, dust, steam)."},
		{"key": "additive", "type": "bool", "default": true, "group": "Drawing", "visible_if": {"particle_style": ["Sparks"]},
			"tooltip": "Additive sparks (light adds up where they overlap)."},
		{"key": "no_depth_test", "label": "No Depth Test", "type": "bool", "default": false, "group": "Drawing",
			"tooltip": "Always draws the particles over the scene, even behind meshes (like Godot's material flag).\nOff: opaque meshes in front hide them; on the device this costs a depth map\n(about 1 ms per frame with the neon_car car)."},
		{"key": "color", "label": "Color", "type": "color", "default": 0xd8d8d8, "group": "Drawing", "visible_if": {"particle_style": ["Puff"]},
			"tooltip": "Color of newly emitted puffs."},
		{"key": "color_end", "label": "Color End", "type": "color", "default": 0x404040, "group": "Drawing", "visible_if": {"particle_style": ["Puff"]},
			"tooltip": "Color at the end of life (blended in between)."},
		{"key": "alpha", "label": "Opacity", "type": "float", "default": 0.6, "min": 0.0, "max": 1.0, "step": 0.01, "group": "Drawing",
			"visible_if": {"particle_style": ["Puff"]}, "tooltip": "Initial opacity: puffs fade in quickly, then fade out to 0 at the end of life."},
		{"key": "puff_size", "label": "Size", "type": "float", "default": 0.2, "min": 0.0, "max": 100.0, "step": 0.01, "suffix": "m", "group": "Drawing",
			"visible_if": {"particle_style": ["Puff"]}, "tooltip": "Diameter when emitted."},
		{"key": "puff_size_end", "label": "Size End", "type": "float", "default": 0.6, "min": 0.0, "max": 100.0, "step": 0.01, "suffix": "m", "group": "Drawing",
			"visible_if": {"particle_style": ["Puff"]}, "tooltip": "Diameter at the end of life: the smoke spreads out."},
	],
	"Node2D": [
		{"key": "position", "type": "vector2", "default": [0.0, 0.0], "step": 1.0, "suffix": "px", "group": "Transform",
			"tooltip": "Position in canvas pixels, relative to a 2D parent (y grows downwards)."},
		{"key": "rotation_degrees", "label": "Rotation", "type": "float", "default": 0.0, "min": -360.0, "max": 360.0, "step": 0.1, "suffix": "°",
			"group": "Transform", "tooltip": "Clockwise on the screen, as in Godot (scripts use rotation in radians)."},
		{"key": "scale", "type": "vector2", "default": [1.0, 1.0], "step": 0.01, "group": "Transform", "tooltip": "Negative values flip."},
		{"key": "z_index", "label": "Z Index", "type": "int", "default": 0, "min": -4096, "max": 4096, "group": "Ordering",
			"tooltip": "Higher is drawn in front, added to the parent's. Same z: the later node in the tree is in front."},
		{"key": "visible", "type": "bool", "default": true, "group": "Visibility"},
		{"key": "modulate", "type": "color", "default": 0xffffff, "group": "Visibility", "tooltip": "Color that multiplies the texture."},
		{"key": "alpha", "label": "Opacity", "type": "float", "default": 1.0, "min": 0.0, "max": 1.0, "step": 0.01, "group": "Visibility",
			"tooltip": "Opacity, inherited by the 2D children (Godot's modulate.a)."},
	],
	"Sprite2D": [
		{"key": "texture", "type": "texture", "default": "", "group": "Sprite",
			"tooltip": "PNG image or sprite sheet. Transparent pixels are not drawn."},
		{"key": "centered", "type": "bool", "default": true, "group": "Sprite", "tooltip": "The frame's centre on the position; off: its top-left corner."},
		{"key": "offset", "type": "vector2", "default": [0.0, 0.0], "step": 1.0, "suffix": "px", "group": "Sprite"},
		{"key": "flip_h", "label": "Flip H", "type": "bool", "default": false, "group": "Sprite"},
		{"key": "flip_v", "label": "Flip V", "type": "bool", "default": false, "group": "Sprite"},
		{"key": "hframes", "type": "int", "default": 1, "min": 1, "max": 256, "group": "Animation", "tooltip": "Columns of the sprite sheet."},
		{"key": "vframes", "type": "int", "default": 1, "min": 1, "max": 256, "group": "Animation", "tooltip": "Rows of the sprite sheet."},
		{"key": "frame", "type": "int", "default": 0, "min": 0, "max": 65535, "group": "Animation",
			"tooltip": "Frame shown, left to right then top to bottom."},
	],
	"AnimatedSprite2D": [
		{"key": "texture", "type": "texture", "default": "", "group": "Sprite Sheet", "tooltip": "The sprite sheet with every frame."},
		{"key": "hframes", "type": "int", "default": 1, "min": 1, "max": 256, "group": "Sprite Sheet", "tooltip": "Columns of the sprite sheet."},
		{"key": "vframes", "type": "int", "default": 1, "min": 1, "max": 256, "group": "Sprite Sheet", "tooltip": "Rows of the sprite sheet."},
		{"key": "animations", "type": "text", "default": "", "group": "Animation",
			"tooltip": "One per line or comma: name, frames, frames per second, \"once\" if it doesn't loop.\nidle 0-3 8\nrun 4-9 12\ndie 10-14 10 once"},
		{"key": "animation", "type": "text", "default": "", "group": "Animation", "tooltip": "Animation shown at the start (empty: the first)."},
		{"key": "autoplay", "type": "bool", "default": true, "group": "Animation"},
		{"key": "speed_scale", "label": "Speed Scale", "type": "float", "default": 1.0, "min": -10.0, "max": 10.0, "step": 0.1, "group": "Animation"},
		{"key": "frame", "type": "int", "default": 0, "min": 0, "max": 65535, "group": "Animation"},
		{"key": "centered", "type": "bool", "default": true, "group": "Offset"},
		{"key": "offset", "type": "vector2", "default": [0.0, 0.0], "step": 1.0, "suffix": "px", "group": "Offset"},
		{"key": "flip_h", "label": "Flip H", "type": "bool", "default": false, "group": "Offset"},
		{"key": "flip_v", "label": "Flip V", "type": "bool", "default": false, "group": "Offset"},
	],
	"Camera2D": [
		{"key": "enabled", "type": "bool", "default": true, "tooltip": "The first enabled Camera2D in the tree is the current one."},
		{"key": "offset", "type": "vector2", "default": [0.0, 0.0], "step": 1.0, "suffix": "px", "tooltip": "Added to the position: shakes, look-ahead."},
		{"key": "zoom", "type": "vector2", "default": [1.0, 1.0], "step": 0.1, "min": 0.01, "max": 100.0, "tooltip": "2 = everything twice as big."},
	],
	"Control": [
		{"key": "position", "type": "vector2", "default": [0.0, 0.0], "group": "Layout", "step": 1.0, "suffix": "px",
			"tooltip": "Top-left corner in pixels, relative to the parent Control (or the screen)."},
		{"key": "visible", "type": "bool", "default": true, "group": "Visibility"},
		{"key": "alpha", "label": "Opacity", "type": "float", "default": 1.0, "min": 0.0, "max": 1.0, "step": 0.01, "group": "Visibility",
			"tooltip": "Opacity of the node (like modulate.a in Godot)."},
	],
	"Label": [
		{"key": "text", "type": "text", "default": "Label",
			"tooltip": "Text; press Enter for a new line. 7×15 bitmap font with Latin accented letters."},
		{"key": "color", "label": "Font Color", "type": "color", "default": 0xffffff},
		{"key": "text_scale", "label": "Scale", "type": "int", "default": 1, "min": 1, "max": 8, "suffix": "×",
			"tooltip": "Integer scaling of the characters (1 = 7×15 pixels)."},
	],
	"Sprite3D": [
		{"key": "texture", "type": "texture", "default": "", "group": "Sprite",
			"tooltip": "PNG image or sprite sheet. Transparent pixels are not drawn."},
		{"key": "pixel_size", "label": "Pixel Size", "type": "float", "default": 0.01, "min": 0.0001, "max": 100.0, "step": 0.001, "suffix": "m",
			"group": "Sprite", "tooltip": "Size of one texel: at 0.01 a 64 px frame is 0.64 m wide."},
		{"key": "modulate", "type": "color", "default": 0xffffff, "group": "Sprite", "tooltip": "Color that multiplies the texture."},
		{"key": "alpha", "label": "Opacity", "type": "float", "default": 1.0, "min": 0.0, "max": 1.0, "step": 0.01, "group": "Sprite"},
		{"key": "flip_h", "label": "Flip H", "type": "bool", "default": false, "group": "Sprite"},
		{"key": "flip_v", "label": "Flip V", "type": "bool", "default": false, "group": "Sprite"},
		{"key": "billboard", "type": "bool", "default": false, "group": "Sprite", "tooltip": "Always faces the camera (Godot's Billboard Enabled)."},
		{"key": "texture_filter", "label": "Filter", "type": "enum", "default": "Nearest", "options": ["Nearest", "Linear"], "group": "Sprite"},
		{"key": "draw_order", "label": "Draw Order", "type": "enum", "default": "Default", "options": ["Default", "Background", "On Top"], "group": "Sprite",
			"tooltip": "Background: drawn first, never covers the rest. On Top: drawn over everything."},
		{"key": "hframes", "type": "int", "default": 1, "min": 1, "max": 256, "group": "Animation", "tooltip": "Columns of the sprite sheet."},
		{"key": "vframes", "type": "int", "default": 1, "min": 1, "max": 256, "group": "Animation", "tooltip": "Rows of the sprite sheet."},
		{"key": "frame", "type": "int", "default": 0, "min": 0, "max": 65535, "group": "Animation",
			"tooltip": "Frame shown, left to right then top to bottom. Scripts change it to animate."},
	],
	"TextureRect": [
		{"key": "texture", "type": "texture", "default": ""},
		{"key": "color", "label": "Modulate", "type": "color", "default": 0xffffff, "tooltip": "Color that multiplies the image."},
		{"key": "size", "type": "vector2", "default": [0.0, 0.0], "step": 1.0, "min": 0.0, "max": 4096.0, "suffix": "px",
			"tooltip": "Drawn size; 0 = texture size times the scale."},
		{"key": "text_scale", "label": "Scale", "type": "int", "default": 1, "min": 1, "max": 8, "suffix": "×",
			"tooltip": "Integer scaling when the size is 0."},
	],
	"ColorRect": [
		{"key": "color", "type": "color", "default": 0xffffff},
		{"key": "size", "type": "vector2", "default": [64.0, 64.0], "step": 1.0, "min": 0.0, "max": 4096.0, "suffix": "px"},
	],
}

static func class_chain(type: String) -> Array:
	var chain := []
	var current := type
	while not current.is_empty():
		chain.append(current)
		current = BASE.get(current, "")
	return chain

static func inherits(type: String, base: String) -> bool:
	return base in class_chain(type)

static func is_3d(type: String) -> bool:
	return inherits(type, "Node3D")

static func is_2d(type: String) -> bool:
	return inherits(type, "Node2D")

static func is_control(type: String) -> bool:
	return inherits(type, "Control")

static func property_def(type: String, key: String) -> Dictionary:
	for cls in class_chain(type):
		for prop in PROPERTIES.get(cls, []):
			if prop.key == key: return prop
	return {}

static func default_value(type: String, key: String) -> Variant:
	var prop := property_def(type, key)
	return prop.get("default") if not prop.is_empty() else null

## Keys whose value decides which other properties the Inspector shows.
static func condition_keys() -> Array:
	var keys := []
	for cls in PROPERTIES:
		for prop in PROPERTIES[cls]:
			for key in prop.get("visible_if", {}):
				if not key in keys: keys.append(key)
	return keys

static func property_visible(prop: Dictionary, node: Dictionary) -> bool:
	var conditions: Dictionary = prop.get("visible_if", {})
	for key in conditions:
		var value = node.get(key)
		var wanted = conditions[key]
		if wanted is String and wanted == "*":
			if value == null or str(value).is_empty(): return false
		elif not str(value) in wanted:
			return false
	return true

## Node keys holding res:// paths (renamed together with the files).
const PATH_KEYS := ["script", "albedo_texture", "sky_texture", "texture", "mesh_file"]

## res:// paths of every texture used by a document (albedo, sky, TextureRect).
static func texture_paths(root: Dictionary) -> Array:
	var paths := []
	walk(root, func(n, _p, _d):
		for key in ["albedo_texture", "sky_texture", "texture"]:
			var path := str(n.get(key, ""))
			if not path.is_empty() and not path in paths: paths.append(path))
	return paths

static func model_paths(root: Dictionary) -> Array:
	var paths := []
	walk(root, func(n, _p, _d):
		var path := str(n.get("mesh_file", ""))
		if str(n.get("mesh", "")) == "File" and not path.is_empty() and not path in paths: paths.append(path))
	return paths

## Top-left corner of a Control in screen pixels (Control parents add up, as in Godot).
static func control_origin(root: Dictionary, id: int) -> Vector2:
	var origin := Vector2.ZERO
	for node in lineage(root, id):
		if not is_control(str(node.type)):
			origin = Vector2.ZERO
			continue
		var p: Array = node.get("position", [0, 0])
		origin += Vector2(float(p[0]), float(p[1]))
	return origin

## Screen rectangle of a Control; `texture_size` is the TextureRect's image size.
static func control_rect(root: Dictionary, node: Dictionary, texture_size: Vector2i = Vector2i.ZERO) -> Rect2:
	var origin := control_origin(root, int(node.id))
	var scale := maxi(1, int(node.get("text_scale", 1)))
	var size := Vector2.ZERO
	match str(node.type):
		"Label":
			var lines := str(node.get("text", "")).split("\n")
			var longest := 0
			for line in lines: longest = maxi(longest, line.length())
			size = Vector2(longest * FONT_SIZE.x, lines.size() * FONT_SIZE.y) * scale
		_:
			var s: Array = node.get("size", [0, 0])
			size = Vector2(float(s[0]), float(s[1]))
			if str(node.type) == "TextureRect":
				if size.x <= 0: size.x = texture_size.x * scale
				if size.y <= 0: size.y = texture_size.y * scale
	return Rect2(origin, size)

static func label_for(prop: Dictionary) -> String:
	if prop.has("label"): return prop.label
	return str(prop.key).capitalize()

static func new_node(type: String, id: int, node_name: String = "") -> Dictionary:
	var node := {"id": id, "name": node_name if not node_name.is_empty() else type, "type": type, "children": []}
	for cls in class_chain(type):
		for prop in PROPERTIES.get(cls, []):
			var value = prop.default
			node[prop.key] = value.duplicate() if value is Array else value
	return node

## Godot-style configuration warnings for setups that silently do nothing, like a body without shapes.
static func configuration_warnings(root: Dictionary, node: Dictionary) -> PackedStringArray:
	var out := PackedStringArray()
	var type := str(node.type)
	var dimension := "2D" if is_2d(type) else "3D"
	if type in BODIES and not _has_own_shape(node):
		out.append("This node has no shape, so it can't collide or interact with other objects.\n"
			+ "Consider adding a CollisionShape%s as a child to define its shape." % dimension)
	elif type in ["CollisionShape3D", "CollisionShape2D"]:
		# Its owner: the nearest body or area above it of the same kind (2D or 3D).
		var owner_found := false
		var parent := parent_of(root, int(node.id))
		while not parent.is_empty():
			if str(parent.type) in BODIES and is_2d(str(parent.type)) == is_2d(type):
				owner_found = true
				break
			parent = parent_of(root, int(parent.id))
		if not owner_found:
			out.append("%s only gives a shape to a body or an area: use it as a child of\n" % type
				+ "StaticBody{0}, CharacterBody{0}, RigidBody{0} or Area{0}.".format([dimension]))
	return out

## A collision shape below `body` that belongs to it (not to a nested body), 2D for 2D bodies.
static func _has_own_shape(body: Dictionary) -> bool:
	var shape := "CollisionShape2D" if is_2d(str(body.type)) else "CollisionShape3D"
	var stack: Array = children(body).duplicate()
	while not stack.is_empty():
		var n: Dictionary = stack.pop_back()
		if str(n.type) == shape: return true
		if not str(n.type) in BODIES: stack.append_array(children(n))
	return false

static func children(node: Dictionary) -> Array:
	if not node.has("children"): node["children"] = []
	return node.children

## Depth-first visit; the callable receives (node, parent, depth).
static func walk(root: Dictionary, visitor: Callable) -> void:
	var stack := [[root, {}, 0]]
	while not stack.is_empty():
		var item: Array = stack.pop_back()
		visitor.call(item[0], item[1], item[2])
		var list := children(item[0])
		for i in range(list.size() - 1, -1, -1):
			stack.append([list[i], item[0], item[2] + 1])

static func find(root: Dictionary, id: int) -> Dictionary:
	var result := {}
	var stack := [root]
	while not stack.is_empty():
		var node: Dictionary = stack.pop_back()
		if int(node.id) == id: return node
		stack.append_array(children(node))
	return result

static func parent_of(root: Dictionary, id: int) -> Dictionary:
	var stack := [root]
	while not stack.is_empty():
		var node: Dictionary = stack.pop_back()
		for child in children(node):
			if int(child.id) == id: return node
			stack.append(child)
	return {}

## Nodes from the root down to `id` (inclusive); empty if not found.
static func lineage(root: Dictionary, id: int) -> Array:
	var chain := []
	var current := find(root, id)
	while not current.is_empty():
		chain.push_front(current)
		current = parent_of(root, int(current.id))
	return chain

static func is_ancestor(root: Dictionary, ancestor_id: int, id: int) -> bool:
	for node in lineage(root, id):
		if int(node.id) == ancestor_id and ancestor_id != id: return true
	return false

static func next_id(root: Dictionary) -> int:
	var top := [0]
	walk(root, func(n, _p, _d): top[0] = maxi(top[0], int(n.id)))
	return top[0] + 1

static func count(root: Dictionary) -> int:
	var total := [0]
	walk(root, func(_n, _p, _d): total[0] += 1)
	return total[0]

static func valid_name(text: String) -> bool:
	if text.strip_edges().is_empty() or text.length() > 80: return false
	for c in [".", ":", "@", "/", "\"", "%"]:
		if c in text: return false
	return true

static func sanitize_name(text: String) -> String:
	var clean := text.strip_edges()
	for c in [".", ":", "@", "/", "\"", "%"]:
		clean = clean.replace(c, "")
	return clean.left(80) if not clean.is_empty() else "Node"

## Godot-style unique sibling name: Cube, Cube2, Cube3...
static func unique_name(parent: Dictionary, wanted: String, ignore_id: int = -1) -> String:
	var used := {}
	for child in children(parent):
		if int(child.id) != ignore_id: used[str(child.name)] = true
	if not used.has(wanted): return wanted
	var base := wanted
	var number := 2
	var regex := RegEx.create_from_string("^(.*?)(\\d+)$")
	var found := regex.search(wanted)
	if found:
		base = found.get_string(1)
		number = int(found.get_string(2)) + 1
	while used.has(base + str(number)): number += 1
	return base + str(number)

## Deep copy with fresh ids (for duplicate/paste).
static func copy_with_new_ids(node: Dictionary, first_id: int) -> Dictionary:
	var copy: Dictionary = node.duplicate(true)
	var next := [first_id]
	walk(copy, func(n, _p, _d):
		n.id = next[0]
		next[0] += 1)
	return copy

## Godot NodePath from `from_id` to `to_id` ("Child", "../Sibling", ".").
static func relative_path(root: Dictionary, from_id: int, to_id: int) -> String:
	var a := lineage(root, from_id)
	var b := lineage(root, to_id)
	var common := 0
	while common < a.size() and common < b.size() and int(a[common].id) == int(b[common].id): common += 1
	var parts := []
	for i in a.size() - common: parts.append("..")
	for i in range(common, b.size()): parts.append(str(b[i].name))
	return "/".join(parts) if not parts.is_empty() else "."

static func local_transform(node: Dictionary) -> Transform3D:
	if not is_3d(str(node.type)): return Transform3D.IDENTITY
	var r: Array = node.get("rotation", [0, 0, 0])
	var s: Array = node.get("scale", [1, 1, 1])
	var p: Array = node.get("position", [0, 0, 0])
	var rotation := Vector3(deg_to_rad(r[0]), deg_to_rad(r[1]), deg_to_rad(r[2]))
	var basis := Basis.from_euler(rotation).scaled_local(Vector3(s[0], s[1], s[2]))
	return Transform3D(basis, Vector3(p[0], p[1], p[2]))

## World transform, composing Node3D ancestors only (as Godot does).
static func global_transform(root: Dictionary, id: int) -> Transform3D:
	var result := Transform3D.IDENTITY
	for node in lineage(root, id):
		if is_3d(str(node.type)): result = result * local_transform(node)
		else: result = Transform3D.IDENTITY
	return result

static func set_transform(node: Dictionary, local: Transform3D) -> void:
	var scale := local.basis.get_scale()
	var euler := local.basis.orthonormalized().get_euler()
	node.position = [local.origin.x, local.origin.y, local.origin.z]
	node.rotation = [rad_to_deg(euler.x), rad_to_deg(euler.y), rad_to_deg(euler.z)]
	node.scale = [scale.x, scale.y, scale.z]

static func new_scene(root_type: String = "Node3D", root_name: String = "World") -> Dictionary:
	var root := new_node(root_type, 1, root_name)
	return {"format": "jetscene", "version": VERSION, "root": root}

## Starter scene for new projects.
static func example() -> Dictionary:
	var doc := new_scene("Node3D", "World")
	var root: Dictionary = doc.root
	var env := new_node("WorldEnvironment", 2, "WorldEnvironment")
	var sun := new_node("DirectionalLight3D", 3, "Sun")
	sun.rotation = [-50.0, -35.0, 0.0]
	var camera := new_node("Camera3D", 4, "Camera3D")
	camera.position = [0.0, 2.5, 5.0]
	camera.rotation = [-22.0, 0.0, 0.0]
	camera.current = true
	var ground := new_node("MeshInstance3D", 5, "Ground")
	ground.mesh = "Plane"
	ground.scale = [4.0, 1.0, 4.0]
	ground.color = 0x344253
	var cube := new_node("MeshInstance3D", 6, "Cube")
	cube.position = [0.0, 0.5, 0.0]
	cube.rotation = [0.0, 25.0, 0.0]
	cube.color = 0xf0a45b
	root.children = [env, sun, camera, ground, cube]
	return doc

static func _vector(value: Variant, size: int = 3) -> bool:
	if not value is Array or value.size() != size: return false
	for item in value:
		if not (item is float or item is int): return false
	return true

## Structural validation of a parsed file; the runtime checks values again.
static func validate(doc: Variant) -> String:
	if not doc is Dictionary: return "The file must contain a JSON object."
	if int(doc.get("version", 0)) != VERSION: return "Unsupported scene version."
	if not doc.get("root") is Dictionary: return "Missing root node."
	var problem := [""]
	var ids := {}
	walk(doc.root, func(n, _p, _d):
		if not problem[0].is_empty(): return
		if not (n.get("id") is float or n.get("id") is int) or int(n.id) < 1: problem[0] = "Node without a valid id."
		elif ids.has(int(n.id)): problem[0] = "Duplicate node id: %d" % int(n.id)
		elif not n.get("name") is String or not valid_name(n.name): problem[0] = "Invalid node name."
		elif not n.get("type") in TYPES: problem[0] = "Unsupported node type: %s" % str(n.get("type"))
		elif n.has("children") and not n.children is Array: problem[0] = "Invalid children in %s." % n.name
		else:
			ids[int(n.id)] = true
			var size := 2 if is_control(str(n.type)) or is_2d(str(n.type)) else 3
			for key in ["position", "rotation", "scale", "size", "offset", "zoom"]:
				if n.has(key) and not _vector(n[key], size): problem[0] = "Invalid %s vector in %s." % [key, n.name]
			for child in n.get("children", []):
				if not child is Dictionary: problem[0] = "Invalid child in %s." % n.name)
	return problem[0]

## Integer ids and colours come back from JSON as floats.
static func normalize(doc: Dictionary) -> void:
	if not doc.get("connections") is Array: doc.connections = []
	walk(doc.root, func(n, _p, _d):
		for key in ["id", "color", "segments", "light_color", "background_color", "ambient_color", "shininess", "text_scale",
				"collision_layer", "collision_mask", "amount", "color_end", "z_index", "hframes", "vframes", "frame", "modulate",
				"sky_top_color", "sky_horizon_color", "ground_horizon_color", "ground_bottom_color"]:
			if n.has(key) and (n[key] is float): n[key] = int(n[key])
		for type_name in class_chain(str(n.type)):
			for prop in PROPERTIES.get(type_name, []):
				if not n.has(prop.key):
					n[prop.key] = prop.default.duplicate() if prop.default is Array else prop.default
		children(n))

## Converts a v1 scene (flat Jet-space objects). Returns [document, notes], notes being what couldn't be converted.
static func migrate_v1(old: Dictionary) -> Array:
	var notes := []
	var mirror := func(v: Array) -> Vector3: return Vector3(float(v[0]), float(v[1]), -float(v[2]))
	var doc := new_scene("Node3D", sanitize_name(str(old.get("name", "World"))))
	var root: Dictionary = doc.root
	var next := 2
	var env := new_node("WorldEnvironment", next, "WorldEnvironment")
	env.background_color = int(old.get("background", 0x161c28))
	root.children.append(env)
	next += 1
	var sun_angles: Array = old.get("sun", [35.0, 50.0, 0.0])
	var azimuth := deg_to_rad(float(sun_angles[0]))
	var elevation := deg_to_rad(float(sun_angles[1]))
	var toward := Vector3(cos(elevation) * cos(azimuth), sin(elevation), -cos(elevation) * sin(azimuth))
	var sun := new_node("DirectionalLight3D", next, "Sun")
	if absf(toward.normalized().dot(Vector3.UP)) < 0.999:
		var e := Basis.looking_at(-toward, Vector3.UP).get_euler()
		sun.rotation = [rad_to_deg(e.x), rad_to_deg(e.y), rad_to_deg(e.z)]
	else:
		sun.rotation = [-90.0, 0.0, 0.0]
	root.children.append(sun)
	next += 1
	var cam: Dictionary = old.get("camera", {})
	var camera := new_node("Camera3D", next, "Camera3D")
	var eye: Vector3 = mirror.call(cam.get("position", [5.0, 4.0, -8.0]))
	var target: Vector3 = mirror.call(cam.get("target", [0.0, 0.5, 0.0]))
	var look := Basis.looking_at(target - eye, Vector3.UP).get_euler()
	camera.position = [eye.x, eye.y, eye.z]
	camera.rotation = [rad_to_deg(look.x), rad_to_deg(look.y), rad_to_deg(look.z)]
	var horizontal := deg_to_rad(float(cam.get("fov", 65.0)))
	camera.fov = rad_to_deg(2.0 * atan(tan(horizontal * 0.5) * 240.0 / 320.0))
	camera.current = true
	root.children.append(camera)
	next += 1
	var shapes := ["Box", "Sphere", "Plane", "Pyramid"]
	var used := {}
	for item in old.get("nodes", []):
		var node := new_node("MeshInstance3D", next, unique_name(root, sanitize_name(str(item.get("name", "Mesh")))))
		next += 1
		var shape := clampi(int(item.get("shape", 0)), 0, 3)
		node.mesh = shapes[shape]
		var factor := 1.0 if shape == 2 else 2.0 # v1 cube/sphere/pyramid were 2 m wide
		var s: Array = item.get("scale", [1, 1, 1])
		var scale := Vector3(float(s[0]), float(s[1]), float(s[2])) * factor
		var r: Array = item.get("rotation", [0, 0, 0])
		# v1 rotation: Jet order (X, then Y, then Z) in mirrored space.
		var jet := Basis.from_euler(Vector3(deg_to_rad(r[0]), deg_to_rad(r[1]), deg_to_rad(r[2])), EULER_ORDER_XYZ)
		var basis := Basis(Vector3(jet.x.x, jet.x.y, -jet.x.z), Vector3(jet.y.x, jet.y.y, -jet.y.z), Vector3(-jet.z.x, -jet.z.y, jet.z.z))
		var position: Vector3 = mirror.call(item.get("position", [0, 0, 0]))
		if shape == 3: position += basis * Vector3(0, scale.y * 0.5, 0) # v1 pyramid pivot was its base
		var euler := basis.get_euler()
		node.position = [position.x, position.y, position.z]
		node.rotation = [rad_to_deg(euler.x), rad_to_deg(euler.y), rad_to_deg(euler.z)]
		node.scale = [scale.x, scale.y, scale.z]
		node.color = int(item.get("color", 0xe6a45c))
		node.visible = bool(item.get("visible", true))
		if absf(float(item.get("spin", 0.0))) > 0.0:
			notes.append("%s: SpinBehaviour (%.0f°/s) no longer exists, use a JetScript script." % [node.name, float(item.spin)])
		root.children.append(node)
	return [doc, notes]

# ------------------------------------------------------------------ signal connections

## The signal connections made in the editor, Godot's [connection] entries: {from, signal, to, method, flags}, nodes by id.
static func connections(doc: Dictionary) -> Array:
	if not doc.get("connections") is Array: doc.connections = []
	return doc.connections

## Drops the connections of nodes that are no longer in the scene.
static func prune_connections(doc: Dictionary) -> void:
	var ids := {}
	walk(doc.root, func(n, _p, _d): ids[int(n.id)] = true)
	doc.connections = connections(doc).filter(func(c): return ids.has(int(c.from)) and ids.has(int(c.to)))

static func find_connection(doc: Dictionary, from: int, signal_name: String, to: int, method: String) -> int:
	var list := connections(doc)
	for i in list.size():
		var c: Dictionary = list[i]
		if int(c.from) == from and str(c["signal"]) == signal_name and int(c.to) == to and str(c.method) == method: return i
	return -1

## Godot's default method names: _on_<node>_<signal>, or _on_<signal> when a node connects to itself.
static func callback_name(source: Dictionary, target: Dictionary, signal_name: String) -> String:
	if int(source.id) == int(target.id): return "_on_" + signal_name
	var node_name := ""
	for c in str(source.name).to_snake_case():
		node_name += c if c == "_" or c.is_valid_identifier() or c.is_valid_int() else "_"
	return "_on_%s_%s" % [node_name, signal_name]

## A 2D node's transform on the canvas, through its 2D parents (a 2D node under anything else starts a canvas).
static func canvas_transform(root: Dictionary, id: int) -> Transform2D:
	var t := Transform2D.IDENTITY
	for node in lineage(root, id):
		if not is_2d(str(node.type)):
			t = Transform2D.IDENTITY
			continue
		var p: Array = node.get("position", [0.0, 0.0])
		var s: Array = node.get("scale", [1.0, 1.0])
		t = t * Transform2D(deg_to_rad(float(node.get("rotation_degrees", 0.0))), Vector2(float(s[0]), float(s[1])), 0.0,
			Vector2(float(p[0]), float(p[1])))
	return t

## Whether a stored value suits a property definition (a node that changed type keeps only the ones that do).
static func fits(prop: Dictionary, value: Variant) -> bool:
	match str(prop.get("type", "")):
		"vector2": return value is Array and value.size() == 2
		"vector3": return value is Array and value.size() == 3
		"float", "int", "color": return value is float or value is int
		"bool": return value is bool
		"enum": return value is String and value in prop.get("options", [])
		"text", "texture", "model", "part": return value is String
	return true

## Godot's Change Type: the node keeps its id, name, children, script and the properties the new type has too;
## the others take the new type's defaults.
static func change_type(node: Dictionary, new_type: String) -> void:
	var fresh := new_node(new_type, int(node.id), str(node.name))
	for key in node.keys():
		if key in ["id", "name", "type", "children", "script", "script_properties"]: continue
		var prop := property_def(new_type, key)
		if prop.is_empty(): node.erase(key)
		elif not fits(prop, node[key]): node[key] = fresh[key]
	for key in fresh:
		if not node.has(key): node[key] = fresh[key]
	node.type = new_type
