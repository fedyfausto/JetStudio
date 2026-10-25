extends RefCounted
## Stored as "input_map" in project.jetproject. Action i is bit i of the game's input; pin -1 means no board button.
## The six ui_* actions always come first. tools/tasks.py reads the same list to generate generated_inputs.hpp.

const DEFAULTS := [
	{"name": "ui_left", "keys": ["Left", "A"]},
	{"name": "ui_right", "keys": ["Right", "D"]},
	{"name": "ui_up", "keys": ["Up", "W"]},
	{"name": "ui_down", "keys": ["Down", "S"]},
	{"name": "ui_accept", "keys": ["Space", "Enter"]},
	{"name": "ui_cancel", "keys": ["Escape", "Backspace"]},
]
const MAX_ACTIONS := 32
const MAX_KEYS := 8

static func is_builtin(name: String) -> bool:
	for entry in DEFAULTS:
		if entry.name == name: return true
	return false

## Letters, digits and _ only: scripts use the name, e.g. Input.is_action_pressed("jump").
static func valid_name(name: String) -> bool:
	if name.is_empty() or name.length() > 64: return false
	for c in name:
		if not (c == "_" or (c >= "a" and c <= "z") or (c >= "A" and c <= "Z") or (c >= "0" and c <= "9")): return false
	return true

## Actions in bit order. Projects older than the Input Map only have "input_pins" ({action: pin}) for the built-ins.
static func of(config: Dictionary) -> Array:
	var stored: Array = config.get("input_map", [])
	var legacy: Dictionary = config.get("input_pins", {})
	var by_name := {}
	for entry in stored:
		if entry is Dictionary: by_name[str(entry.get("name", ""))] = entry
	var out := []
	for builtin in DEFAULTS:
		var entry: Dictionary = by_name.get(builtin.name, {})
		out.append({"name": builtin.name, "keys": _keys(entry.get("keys", builtin.keys)),
			"pin": int(entry.get("pin", legacy.get(builtin.name, -1)))})
	for entry in stored:
		if not entry is Dictionary: continue
		var name := str(entry.get("name", ""))
		if is_builtin(name) or not valid_name(name) or out.size() >= MAX_ACTIONS: continue
		out.append({"name": name, "keys": _keys(entry.get("keys", [])), "pin": int(entry.get("pin", -1))})
	return out

static func _keys(value) -> Array:
	var out := []
	if value is Array:
		for key in value:
			if str(key) != "" and out.size() < MAX_KEYS and not str(key) in out: out.append(str(key))
	return out

## [{name, keys}] for the bridge, and through it the package.
static func bridge_actions(actions: Array) -> Array:
	var out := []
	for action in actions: out.append({"name": action.name, "keys": PackedStringArray(action.keys)})
	return out

## Godot keycodes of each action's keys, for the editor's game view.
static func key_codes(actions: Array) -> Array:
	var out := []
	for action in actions:
		var codes := []
		for key in action.keys:
			var code := OS.find_keycode_from_string(str(key))
			if code != KEY_NONE: codes.append(code)
		out.append(codes)
	return out

## {action: pin} of the actions wired to the board.
static func pins(actions: Array) -> Dictionary:
	var out := {}
	for action in actions:
		if int(action.pin) >= 0: out[action.name] = int(action.pin)
	return out
