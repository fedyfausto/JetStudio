# JetScript — language reference

JetScript is the gameplay language of Jet Studio. Its syntax and semantics follow
**GDScript (Godot 4)** as closely as a microcontroller allows. The same compiler and
virtual machine run inside the editor (Play), in the desktop player and on the
ESP32 firmware, so a script behaves identically everywhere.

Script files use the `.jet` extension and live in the project folder (`res://`).
A script is attached to a node; the node is `self`.

```gdscript
extends MeshInstance3D

@export var speed := 90.0            # editable in the Inspector
@export_range(0, 10, 0.5) var jump := 2.0
var elapsed := 0.0
@onready var target = $"../Target"

func _ready():
	print("Hello from ", name)

func _process(delta):
	elapsed += delta
	rotation_degrees.y += speed * delta
	position.y = sin(elapsed * 2.0) * 0.5
	if Input.is_action_pressed("ui_right"):
		position.x += 2.0 * delta
```

## Differences from GDScript

| Topic | GDScript | JetScript |
| --- | --- | --- |
| `float` | 64-bit | **32-bit** (ESP32-S3 FPU is single precision) |
| `int` | 64-bit | 64-bit |
| Classes | `class_name`, inner classes, `preload` | not supported yet |
| Signals, `await`, lambdas | supported | not supported yet (clear compile error) |
| Typed arrays, `Callable` | supported | not supported |
| Static typing | checked | annotations accepted; `int`/`float` variables convert on assignment, other mismatches are runtime errors |

Unsupported constructs produce a compile error naming the feature, never a crash.

## Lexical structure

- Comments start with `#` and run to end of line.
- Blocks are defined by indentation (tabs or spaces; a block must be consistent).
  A `:` ends the header of every block. A block may be written on the same line:
  `if x: return`.
- Inside `()`, `[]`, `{}` newlines and indentation are ignored. A trailing `\`
  continues a line.
- Literals: `null`, `true`, `false`, integers (`42`, `0xFF`, `0b1010`, `1_000`),
  floats (`1.5`, `.5`, `1e3`, `2.5e-2`), strings in `"..."` or `'...'` with escapes
  `\n \t \r \\ \" \' \uXXXX`.
- `$Name`, `$Path/To/Node`, `$"Path with spaces"` are shorthand for `get_node("...")`.
- Identifiers: `[A-Za-z_][A-Za-z0-9_]*`.

Keywords: `if elif else for while break continue pass return match var const func
extends and or not in is self signal class class_name static enum await yield
preload breakpoint super as void`. The ones not supported yet are reserved and
produce an explanatory compile error.

## Top level

```gdscript
extends Node3D                 # optional, must be first statement; default base: Node
const MAX_SPEED = 10.0
enum State { IDLE, RUN = 5, JUMP }   # State.IDLE == 0, State.RUN == 5, State.JUMP == 6
enum { A, B }                        # unnamed enum: A and B become constants
@export var health := 3
@export_range(0, 100) var volume = 50
@export_range(0.0, 1.0, 0.05) var ratio := 0.5
@onready var label = $Label
var velocity := Vector3.ZERO
var counter: int = 0
func name(arg1, arg2 := 2, arg3: float = 1.0) -> void:
	pass
```

- Member variables are per instance. Initializers run when the instance is created,
  in declaration order; `@onready` initializers run immediately before `_ready()`.
- `@export` members appear in the editor Inspector; their type comes from the type
  annotation, else from the initializer. Exportable types: `bool`, `int`, `float`,
  `String`, `Vector3`, `Color`. The scene may override the initial value.
- `const` values must be constant expressions (literals, operators, other
  constants, `Vector3(...)`/`Color(...)` with constant args, built-in constants).

## Lifecycle callbacks (called by the engine)

| Callback | When |
| --- | --- |
| `_ready()` | once, after the whole scene is created; children before parents |
| `_process(delta)` | every frame, parents before children, `delta` in seconds |
| `_physics_process(delta)` | at a fixed 60 Hz (up to 4 times per frame), before `_process`; move bodies here |
| `_on_body_entered(body)` | Area3D scripts: a body started overlapping the area (after the physics step) |
| `_on_body_exited(body)` | Area3D scripts: a body stopped overlapping the area |

## Statements

`var`, `const`, expression statements, assignment (`=`, `+=`, `-=`, `*=`, `/=`,
`%=`, `&=`, `|=`, `^=`, `<<=`, `>>=`), `if/elif/else`, `while`, `for x in iterable`,
`break`, `continue`, `pass`, `return [expr]`, `match`.

`for` iterates over: an `int` n (0..n-1), `range(...)`, an Array (elements),
a Dictionary (keys), a String (one-character strings).

`match` supports literal patterns, constant names, `_` wildcard and several
comma-separated patterns per branch:

```gdscript
match state:
	State.IDLE, State.RUN:
		pass
	7:
		print("seven")
	_:
		print("other")
```

## Expressions and operators (lowest to highest precedence)

| Operators | Notes |
| --- | --- |
| `x if cond else y` | conditional expression |
| `or`, `\|\|` | short-circuit |
| `and`, `&&` | short-circuit |
| `not`, `!` | |
| `in`, `not in` | Array/Dictionary keys/String substring |
| `is` | `x is int`, `node is Node3D` (built-in types and engine classes) |
| `== != < > <= >=` | `int` and `float` compare numerically |
| `\|` `^` `&` | ints only |
| `<< >>` | ints only |
| `+ -` | numbers, `Vector3`, `Color`; `+` concatenates `String`s and Arrays |
| `* / %` | `int / int` truncates toward zero; `%` on Strings formats |
| `**` | power |
| unary `- + ~` | |
| `x.member`, `x.method()`, `x[i]`, `f()` | |

Division or modulo of ints by zero is a runtime error. Float division by zero
follows IEEE rules.

Assigning to a member of a value-type property writes the property back, as in
GDScript: `position.x += 1` reads `position`, changes `x` and assigns `position`.
This also works through nodes: `$Cube.position.y = 2`.

## Types

| Type | Notes |
| --- | --- |
| `null` | |
| `bool` | |
| `int` | 64-bit |
| `float` | 32-bit |
| `String` | immutable, UTF-8 |
| `Vector3` | `x y z`; arithmetic with vectors and scalars |
| `Color` | `r g b` (0..1); `a` reads as 1.0 |
| `Array` | reference type, mixed values, negative indices count from the end |
| `Dictionary` | reference type, insertion ordered, keys: `int float bool String Vector3` |
| Objects | engine nodes and singletons (`Input`) |

### Vector3

Constructor `Vector3()`, `Vector3(x, y, z)`. Constants `ZERO ONE UP DOWN LEFT RIGHT
FORWARD BACK` (`FORWARD` is `(0, 0, -1)`, as in Godot). Methods: `length()`,
`length_squared()`, `normalized()`, `dot(v)`, `cross(v)`, `distance_to(v)`,
`distance_squared_to(v)`, `lerp(v, t)`, `move_toward(v, delta)`, `abs()`,
`rotated(axis, angle)`, `angle_to(v)`, `is_zero_approx()`, `is_equal_approx(v)`.

### Color

`Color(r, g, b)`, `Color(r, g, b, a)` (alpha ignored), `Color("#ff8800")`,
`Color("ff8800")`. Constants `BLACK WHITE RED GREEN BLUE YELLOW CYAN MAGENTA ORANGE
GRAY`. Methods: `lerp(c, t)`, `to_html()`. Arithmetic like `Vector3`.

### String

`length()`, `to_upper()`, `to_lower()`, `begins_with(s)`, `ends_with(s)`,
`contains(s)`, `find(s)`, `substr(from, len = -1)`, `split(delim)`, `strip_edges()`,
`replace(what, with)`, `to_int()`, `to_float()`, `is_empty()`, indexing `s[i]`.
Formatting: `"%s scored %d (%.2f%%)" % [name, points, ratio]`; a single value may
be used without an array. Specifiers: `%s %d %i %f %.Nf %x %X %%`.

### Array

Literal `[1, 2, 3]`. `size()`, `is_empty()`, `append(v)`, `push_back(v)`,
`push_front(v)`, `pop_back()`, `pop_front()`, `insert(i, v)`, `remove_at(i)`,
`erase(v)`, `has(v)`, `find(v)`, `clear()`, `front()`, `back()`, `duplicate()`,
`reverse()`, `sort()` (numbers or strings), `pick_random()`. Index get/set.

### Dictionary

Literal `{"a": 1, 2: "b"}`. `size()`, `is_empty()`, `has(k)`, `get(k, default = null)`,
`keys()`, `values()`, `erase(k)`, `clear()`, `duplicate()`. Index get/set; reading
a missing key is a runtime error (use `get`).

## Global functions and constants

`print(...)`, `printerr(...)`, `str(...)`, `int(x)`, `float(x)`, `bool(x)`, `len(x)`,
`range(end)`, `range(begin, end)`, `range(begin, end, step)`, `abs`, `sign`, `floor`,
`ceil`, `round`, `sqrt`, `pow`, `exp`, `log`, `sin`, `cos`, `tan`, `asin`, `acos`,
`atan`, `atan2`, `deg_to_rad`, `rad_to_deg`, `min(...)`, `max(...)`, `clamp`,
`clampf`, `clampi`, `lerp`, `lerpf`, `inverse_lerp`, `move_toward`, `wrapf`, `wrapi`,
`fmod`, `fposmod`, `posmod`, `snapped`, `is_equal_approx`, `is_zero_approx`,
`randomize()`, `seed(n)`, `randf()`, `randi()`, `randf_range(a, b)`,
`randi_range(a, b)`, `is_instance_valid(obj)`, `typeof(x)` (returns a type name String).
Constants: `PI`, `TAU`, `INF`, `NAN`.

## Engine API

The engine classes form this hierarchy. Properties use Godot names.

- **Node**: `name` (read only), `get_node(path)`, `get_node_or_null(path)`,
  `get_parent()`, `get_children()`, `get_child(i)`, `get_child_count()`,
  `find_child(name)`, `queue_free()`, `get_tree()`.
- **Node3D** (extends Node): `position`, `rotation` (radians), `rotation_degrees`,
  `scale`, `visible`, `global_position`, `translate(offset)`, `rotate_x(rad)`,
  `rotate_y(rad)`, `rotate_z(rad)`, `look_at(target)`, `show()`, `hide()`,
  `is_visible_in_tree()`.
- **MeshInstance3D** (extends Node3D): `color`, `alpha`, `uv_offset`, `uv_scale`
  (Vector3, x and y used), `perspective_correct`, `texture_filter` (0 nearest, 1 linear).
- **Sprite3D** (extends Node3D): `frame`, `hframes`, `vframes`, `flip_h`, `flip_v`, `pixel_size`,
  `modulate`. One frame of its texture's sprite sheet (left to right, then top to bottom) as a quad,
  unlit and double-sided; setting `frame` animates it. Billboard is set in the Inspector.
- **Camera3D** (extends Node3D): `fov`, `near`, `far`, `current`, `make_current()`.
- **DirectionalLight3D** (extends Node3D): `light_color`, `light_energy`.
- **WorldEnvironment** (extends Node): `background_color`, `ambient_color`.
- **CollisionObject3D** (extends Node3D; bodies and areas): `collision_layer`,
  `collision_mask` (bit masks; two objects collide when either mask has a bit of
  the other's layer, as in Godot).
- **StaticBody3D** (extends CollisionObject3D): `friction`, `bounce`. Setting its
  `position` from a script moves it (moving platforms carry characters).
- **CharacterBody3D** (extends CollisionObject3D): `velocity`, `floor_max_angle`
  (radians), `move_and_slide()` (true if it touched something), `is_on_floor()`,
  `is_on_wall()`, `is_on_ceiling()`, `get_floor_normal()`.
- **RigidBody3D** (extends CollisionObject3D): `linear_velocity`,
  `angular_velocity`, `mass`, `gravity_scale`, `freeze`, `sleeping`, `friction`,
  `bounce`, `apply_central_impulse(v)`, `apply_impulse(v, position)` (the position
  is ignored: boxes do not tumble), `apply_central_force(v)` (for the next step).
- **Area3D** (extends CollisionObject3D): `monitoring`, `get_overlapping_bodies()`,
  `has_overlapping_bodies()`, `overlaps_body(body)`.
- **CollisionShape3D** (extends Node3D): `disabled`.
- **CPUParticles3D** (extends Node3D; drawn by Jet's particle system): `emitting`,
  `amount` (int 1-200), `lifetime`, `one_shot`, `explosiveness`, `lifetime_randomness`,
  `direction`, `spread` (degrees), `gravity`, `initial_velocity_min`,
  `initial_velocity_max`, `damping_min`, `damping_max`, `color`, `alpha` (Puff style),
  `restart()`. With `one_shot` `emitting` becomes false at the
  end of the cycle; setting it to true starts a new cycle. Particles live in world
  space: moving the node does not move the ones already emitted.
- **Control** (extends Node; 2D nodes drawn over the 3D scene, in screen pixels
  of the display, 320×240 unless the project hardware sets another resolution): `position` and `size` (Vector3, x and y used;
  position is relative to a parent Control), `visible`, `alpha`, `show()`, `hide()`. A plain Control draws
  nothing: it groups other 2D nodes (a HUD), which are hidden with it and inherit its `alpha`.
- **Label** (extends Control): `text`, `color`. Built-in 7×15 bitmap font
  (ASCII and Latin-1 accented letters), `\n` starts a new line.
- **TextureRect** (extends Control): `color` (modulate).
- **ColorRect** (extends Control): `color`.
- **Autoloads** (Project Settings > Autoload): scripts that live across scene changes.
  Any script reaches one by its name as a global (`Game.score += 1`) or as
  `get_node("/root/Game")`. Their `_ready()` runs before the first scene's.
- **SceneTree** (from any node: `get_tree()`): `change_scene_to_file(path)` and
  `reload_current_scene()` return `OK` (0) or `ERR_CANT_OPEN` (19) when the project has no
  such scene; the change happens at the end of the frame. `current_scene` is the root node of
  the running scene.
- **Input** (singleton): `is_action_pressed(a)`, `is_action_just_pressed(a)`,
  `is_action_just_released(a)`, `get_axis(negative, positive)`.
  Actions: the project's Input Map (Project Settings > Input Map): the built-in
  `ui_left ui_right ui_up ui_down ui_accept ui_cancel` (arrows/WASD, Space or
  Enter, Escape or Backspace) plus the project's own, each with its PC keys and
  board button. An unknown name is an error, as in Godot.

Paths for `get_node` are relative to the node (`"Child"`, `"Child/Grandchild"`,
`".."`, `"../Sibling"`) or absolute from the scene root (`"/root/World/Cube"`
or `"/World/Cube"`).

## Compilation and bytecode

Scripts are compiled to bytecode for a stack virtual machine; there is no
translation to C++. The editor compiles while you type (errors and warnings) and
again at every Play, in a few milliseconds. Exported packages (`.jetpack`) carry
the bytecode, not the source: the desktop player and the firmware contain only
the VM (built with `JETSCRIPT_NO_COMPILER`). Bytecode files are versioned, carry
a fingerprint of the instruction set and builtin tables and a CRC-32; loading
resolves engine member names again and verifies every operand and jump target.
Bytecode from a different JetScript build is refused: export the project again.

## Errors

Compile errors report line and column and are shown in the script editor while
typing. A runtime error (for example calling a method on `null`) stops the
current callback, is reported with file, line and stack trace, and pauses the
game in the editor. An instruction budget protects against infinite loops: a
single callback that executes too many instructions is aborted with an error.
