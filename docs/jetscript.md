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
| Classes | `class_name`, inner classes | not supported yet |
| `preload()` | a constant, loaded with the script | a function: the scene is looked up when the line runs |
| `await`, lambdas | supported | not supported yet (clear compile error) |
| Signals | declared, connected in the editor or in scripts | the same; `CONNECT_DEFERRED` and `Callable.bind()` not supported yet |
| Typed arrays | supported | not supported |
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
signal hit(damage)                   # see Signals
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
| `_on_body_entered(body)` | Area3D/Area2D scripts: a body started overlapping the area (after the physics step) |
| `_on_body_exited(body)` | Area3D/Area2D scripts: a body stopped overlapping the area |
| `_on_area_entered(area)` | Area3D/Area2D scripts: another area in its mask started overlapping it |
| `_on_area_exited(area)` | Area3D/Area2D scripts: that area stopped overlapping it |

The area callbacks come with the `body_entered`, `body_exited`, `area_entered` and `area_exited`
signals (see Signals): an area's script that connects one of them to its own `_on_...` method gets
one call, not two.

## Signals

As in Godot 4. A script declares its signals at the top level; the engine's nodes have their own.

```gdscript
extends Area2D

signal destroyed(enemy)          # argument names are documentation
var hp := 2

func _on_area_entered(bullet):
	hp -= 1
	if hp == 0:
		destroyed.emit(self)     # or emit_signal("destroyed", self)
```

```gdscript
func spawn():
	var enemy = enemy_scene.instantiate()
	add_child(enemy)
	enemy.destroyed.connect(_on_enemy_destroyed)
	$Timer.timeout.connect(_on_timeout, CONNECT_ONE_SHOT)

func _on_enemy_destroyed(enemy):
	score += 100
```

- A function or an engine method named without calling it is a `Callable` (`_on_timeout`,
  `$Door.hide`, `Callable(node, "method")`); a signal named the same way is a `Signal` value.
- `Signal`: `emit(...)`, `connect(callable, flags = 0)` (returns `OK`), `disconnect(callable)`,
  `is_connected(callable)`, `get_name()`, `get_object()`, `is_null()`.
- `Callable`: `call(...)`, `callv(array)`, `get_object()`, `get_method()`, `is_valid()`, `is_null()`.
- Every object: `connect(signal, callable, flags = 0)`, `disconnect(signal, callable)`,
  `is_connected(signal, callable)`, `emit_signal(signal, ...)`, `has_signal(signal)`.
- Flags: `CONNECT_ONE_SHOT` (disconnects after the first emission) and `CONNECT_PERSIST`
  (accepted, it means nothing at run time). `CONNECT_DEFERRED` is refused for now: callables
  always run when the signal is emitted.
- The callables run in the order they were connected, with the signal's arguments. An error in
  one of them is reported and the others still run.
- Connecting a signal twice to the same callable, connecting a signal the object doesn't have,
  disconnecting what isn't connected or emitting an unknown signal are runtime errors.
- Engine signals: `body_entered(body)`, `body_exited(body)`, `area_entered(area)`,
  `area_exited(area)` (Area3D, Area2D), `timeout()` (Timer), `animation_finished()`
  (AnimatedSprite2D, at the end of an animation that doesn't loop), `finished()` (CPUParticles3D,
  at the end of a one-shot cycle).
- The connections belong to the VM, not to the nodes: they cost nothing until used, and a freed
  node's connections go with it.
- Connections made in the editor's Signals dock are saved with the scene and made when it starts or is
  instantiated, after the scripts' member initializers and before `_ready()`. One whose method is
  missing is reported in the Output and skipped.

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
| `Vector2` | `x y`; 2D positions and directions (y grows downwards) |
| `Vector3` | `x y z`; arithmetic with vectors and scalars |
| `Color` | `r g b` (0..1); `a` reads as 1.0 |
| `Array` | reference type, mixed values, negative indices count from the end |
| `Dictionary` | reference type, insertion ordered, keys: `int float bool String Vector2 Vector3` |
| Objects | engine nodes and singletons (`Input`) |
| `Callable` | an object's method, by name (see Signals) |
| `Signal` | an object's signal, by name (see Signals) |

### Vector3

Constructor `Vector3()`, `Vector3(x, y, z)`. Constants `ZERO ONE UP DOWN LEFT RIGHT
FORWARD BACK` (`FORWARD` is `(0, 0, -1)`, as in Godot). Methods: `length()`,
`length_squared()`, `normalized()`, `dot(v)`, `cross(v)`, `distance_to(v)`,
`distance_squared_to(v)`, `lerp(v, t)`, `move_toward(v, delta)`, `abs()`,
`rotated(axis, angle)`, `angle_to(v)`, `is_zero_approx()`, `is_equal_approx(v)`.

### Vector2

Constructor `Vector2()`, `Vector2(x, y)`. Constants `ZERO ONE UP DOWN LEFT RIGHT`, with Godot's 2D
directions (`UP` is `(0, -1)`). Methods: `length()`, `length_squared()`, `normalized()`, `dot(v)`,
`cross(v)` (a float), `distance_to(v)`, `distance_squared_to(v)`, `direction_to(v)`, `lerp(v, t)`,
`move_toward(v, delta)`, `abs()`, `floor()`, `ceil()`, `round()`, `sign()`, `rotated(angle)`,
`angle()`, `angle_to(v)`, `angle_to_point(v)`, `orthogonal()`, `aspect()`, `limit_length(len)`,
`is_zero_approx()`, `is_equal_approx(v)`. Angles are radians, clockwise on the screen.

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
`randi_range(a, b)`, `is_instance_valid(obj)`, `typeof(x)` (returns a type name String),
`load(path)` and `preload(path)` (a scene of the project as a `PackedScene`; `null` and an error if there is none).
Constants: `PI`, `TAU`, `INF`, `NAN`, `OK` (0), `FAILED` (1), `ERR_CANT_OPEN` (19),
`ERR_INVALID_PARAMETER` (31), `CONNECT_DEFERRED`, `CONNECT_PERSIST`, `CONNECT_ONE_SHOT`,
`CONNECT_REFERENCE_COUNTED`.

## Engine API

The engine classes form this hierarchy. Properties use Godot names.

- **Node**: `name` (read only), `get_node(path)`, `get_node_or_null(path)`,
  `get_parent()`, `get_children()`, `get_child(i)`, `get_child_count()`,
  `find_child(name)`, `queue_free()`, `get_tree()`, `add_child(node)`, `remove_child(node)`.
  `add_child()` puts a node made by `instantiate()` (or taken out with `remove_child()`) under this one;
  if this node is in the scene, the new subtree enters it and gets `_ready()`, children first. A sibling
  with the same name gets a number (`Bullet`, `Bullet2`...). Nodes outside the scene don't run or show.
  `queue_free()` releases the node and its children at the end of the frame.
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
  `has_overlapping_bodies()`, `overlaps_body(body)`, `get_overlapping_areas()`,
  `has_overlapping_areas()`, `overlaps_area(area)`. With `monitoring` off it detects nothing,
  but other areas still detect it.
- **CollisionShape3D** (extends Node3D): `disabled`.
- **CPUParticles3D** (extends Node3D; drawn by Jet's particle system): `emitting`,
  `amount` (int 1-200, about 40 bytes of RAM each), `lifetime`, `one_shot`, `explosiveness`, `lifetime_randomness`,
  `direction`, `spread` (degrees), `gravity`, `initial_velocity_min`,
  `initial_velocity_max`, `damping_min`, `damping_max`, `color`, `alpha` (Puff style),
  `restart()`. With `one_shot` `emitting` becomes false at the
  end of the cycle; setting it to true starts a new cycle. Particles live in world
  space: moving the node does not move the ones already emitted.
- **Node2D** (extends Node; drawn over the 3D scene and under the Controls, in canvas pixels with y
  growing downwards): `position`, `global_position`, `scale` (Vector2), `rotation` (radians, clockwise),
  `rotation_degrees`, `z_index`, `visible`, `modulate`, `alpha` (Godot's `modulate.a`, inherited by the
  2D children), `translate(v)`, `rotate(radians)`, `look_at(point)`, `show()`, `hide()`,
  `is_visible_in_tree()`. A 2D node under a 2D parent moves, turns and hides with it. Higher `z_index`
  (added to the parent's) is drawn in front; with the same z, the later node in the tree.
- **Sprite2D** (extends Node2D): `frame`, `hframes`, `vframes`, `flip_h`, `flip_v`, `centered`,
  `offset` (Vector2, in the node's scaled space). One frame of its texture's sprite sheet.
- **AnimatedSprite2D** (extends Node2D): `play(name = "")`, `stop()`, `is_playing()`, `animation`,
  `frame`, `speed_scale`, `flip_h`, `flip_v`, `centered`, `offset`. Its animations are frame ranges of
  one sprite sheet, written in the Inspector as `idle 0-3 8, run 4-9 12, die 10-14 10 once` (name,
  frames, frames per second, `once` when it doesn't loop); with Autoplay the first (or the chosen) one
  starts on its own.
- **Camera2D** (extends Node2D): `offset`, `zoom` (Vector2), `enabled`, `make_current()`. The current
  Camera2D shows its position at the screen centre; without one the canvas origin is the top-left corner.
- **2D physics**: the 3D physics nodes' 2D versions, in canvas pixels with y down. **CollisionObject2D**
  (extends Node2D): `collision_layer`, `collision_mask`. **StaticBody2D**: `friction`, `bounce`.
  **CharacterBody2D**: `velocity` (Vector2), `floor_max_angle`, `move_and_slide()`, `is_on_floor()`,
  `is_on_wall()`, `is_on_ceiling()`, `get_floor_normal()` (Vector2; `Vector2.UP` on flat ground).
  **RigidBody2D**: `linear_velocity` (Vector2), `angular_velocity` (float, rad/s), `mass`,
  `gravity_scale` (gravity is 980 px/s² down), `freeze`, `sleeping`, `friction`, `bounce`,
  `apply_central_impulse(v)`, `apply_impulse(v, position)`, `apply_central_force(v)` with Vector2.
  **Area2D**: as Area3D. **CollisionShape2D** (extends Node2D): `disabled`, `one_way_collision`
  (bodies land on it from its -y side and pass through from below). A 2D shape belongs to the nearest
  2D body or area above it and never meets 3D shapes.
- **Timer** (extends Node): `wait_time` (seconds), `one_shot`, `autostart`, `paused`, `time_left`
  (read only), `start(time_sec = -1)` (a positive time becomes the new `wait_time`), `stop()`,
  `is_stopped()`. Emits `timeout` when the time is up, at most once per frame; a repeating Timer
  starts again with what was left over. Autostart starts it when it enters the scene.
- **Tween** (from `create_tween()` on any node, which it dies with, or `get_tree().create_tween()`), as in
  Godot 4. Each `tween_*` call is a step that starts when the previous one ends:

  ```gdscript
  var tween = create_tween()
  tween.tween_property($Ship, "position", Vector2(160, 120), 0.5).set_trans(Tween.TRANS_BACK).set_ease(Tween.EASE_OUT)
  tween.tween_property($Ship, "modulate", Color.RED, 0.2)
  tween.tween_interval(1.0)
  tween.tween_callback($Ship.queue_free)
  ```

  Tween: `tween_property(object, property, final_value, duration)` (`"position:x"` tweens one component),
  `tween_interval(time)`, `tween_callback(callable)`, `tween_method(callable, from, to, duration)` (calls it with
  the value each frame), `set_parallel(parallel = true)`, `parallel()` (the next tweener joins the previous one),
  `chain()`, `set_loops(loops = 0)` (0: forever), `set_trans()`, `set_ease()` (defaults for the next tweeners),
  `set_speed_scale()`, `bind_node()`, `pause()`, `play()`, `stop()` (back to the start), `kill()`, `is_running()`,
  `is_valid()`, `get_total_elapsed_time()`; signals `finished`, `step_finished(idx)`, `loop_finished(loop_count)`.
  PropertyTweener: `from(value)`, `from_current()`, `as_relative()`, `set_trans()`, `set_ease()`, `set_delay()`;
  MethodTweener: `set_trans()`, `set_ease()`, `set_delay()`; CallbackTweener: `set_delay()`.
  Transitions `Tween.TRANS_LINEAR SINE QUINT QUART QUAD EXPO ELASTIC CUBIC CIRC BOUNCE BACK SPRING`, eases
  `Tween.EASE_IN EASE_OUT EASE_IN_OUT EASE_OUT_IN`. Numbers, Vector2, Vector3 and Color are interpolated, ints stay
  ints. Tweens run after the scripts' `_process()`; once finished or killed they are no longer valid, and adding to
  a tween that has started is an error (`stop()` it first), as in Godot.
- **AnimationPlayer** (extends Node), as in Godot 4: `play(name = "", custom_blend = -1, custom_speed = 1.0,
  from_end = false)` (no name: the current one, or the autoplay one; the same animation resumes where it was),
  `play_backwards(name = "")`, `stop(keep_state = false)` (back to the start), `pause()`, `is_playing()`,
  `seek(seconds, update = false)`, `has_animation(name)`, `get_animation_list()`, `current_animation`,
  `current_animation_position` and `current_animation_length` (read only), `autoplay`, `speed_scale`. Signals
  `animation_started(anim_name)` and `animation_finished(anim_name)` (not for looping animations). Track paths
  start from the player's parent (Godot's default root node); a property track sets an engine property
  (`"position:x"` animates one component) or a script variable; method keys run after the frame's animations,
  with the other deferred calls. Blending and the RESET animation are not supported.
- **Control** (extends Node; 2D nodes drawn over the 3D scene, in screen pixels
  of the display, 320×240 unless the project hardware sets another resolution): `position` and `size` (Vector2, a Vector3
  is accepted too; position is relative to a parent Control), `visible`, `alpha`, `show()`, `hide()`. A plain Control draws
  nothing: it groups other 2D nodes (a HUD), which are hidden with it and inherit its `alpha`.
- **Label** (extends Control): `text`, `color`. Built-in 7×15 bitmap font
  (ASCII and Latin-1 accented letters), `\n` starts a new line.
- **TextureRect** (extends Control): `color` (modulate).
- **ColorRect** (extends Control): `color`.
- **Autoloads** (Project Settings > Autoload): scripts that live across scene changes.
  Any script reaches one by its name as a global (`Game.score += 1`) or as
  `get_node("/root/Game")`. Their `_ready()` runs before the first scene's.
- **PackedScene** (from `load()`/`preload()`): `instantiate()` returns the root of a new copy of the scene,
  outside the tree, with its scripts' member variables set (`_ready()` waits for `add_child()`):

  ```gdscript
  var bullet_scene = preload("res://bullet.jetscene")

  func shoot():
      var bullet = bullet_scene.instantiate()
      bullet.position = position
      get_parent().add_child(bullet)
  ```
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
