extends RefCounted
## A folder with a project.jetproject file, used as res:// like a Godot project.

const FILE := "project.jetproject"
const SCENE_EXT := "jetscene"
const SCRIPT_EXT := "jet"
const IGNORED := [".git", ".jetstudio", ".godot", "build", "exports"]

var root := "" # absolute folder, without trailing slash
var config := {}

static func is_project(folder: String) -> bool:
	return FileAccess.file_exists(folder.path_join(FILE))

func open(folder: String) -> String:
	var text := FileAccess.get_file_as_string(folder.path_join(FILE))
	if text.is_empty() and FileAccess.get_open_error() != OK: return "Project not found in " + folder
	var data = JSON.parse_string(text)
	if not data is Dictionary: return "Invalid project.jetproject."
	root = folder.trim_suffix("/")
	config = data
	config.merge({"name": folder.get_file(), "main_scene": "", "target": "desktop"})
	return ""

func save() -> String:
	return write_text(root.path_join(FILE), JSON.stringify(config, "\t") + "\n")

func project_name() -> String:
	return str(config.get("name", root.get_file()))

func main_scene() -> String:
	return str(config.get("main_scene", ""))

func to_abs(res_path: String) -> String:
	if res_path.begins_with("res://"): return root.path_join(res_path.trim_prefix("res://"))
	return res_path

func to_res(abs_path: String) -> String:
	if abs_path == root: return "res://"
	if abs_path.begins_with(root + "/"): return "res://" + abs_path.trim_prefix(root + "/")
	return abs_path

func exists(res_path: String) -> bool:
	return FileAccess.file_exists(to_abs(res_path)) or DirAccess.dir_exists_absolute(to_abs(res_path))

func read(res_path: String) -> String:
	return FileAccess.get_file_as_string(to_abs(res_path))

## Writes a .tmp file and renames it over the target, so a failed write never leaves half a file.
static func write_text(path: String, text: String) -> String:
	DirAccess.make_dir_recursive_absolute(path.get_base_dir())
	var file := FileAccess.open(path + ".tmp", FileAccess.WRITE)
	if file == null: return "Could not write " + path
	file.store_string(text)
	file.flush()
	var error := file.get_error()
	file.close()
	if error != OK or DirAccess.rename_absolute(path + ".tmp", path) != OK:
		DirAccess.remove_absolute(path + ".tmp")
		return "Write failed: " + path
	return ""

func write(res_path: String, text: String) -> String:
	return write_text(to_abs(res_path), text)

## Recursive listing: [{"path": "res://...", "dir": bool, "children": [...]}].
func scan(res_dir: String = "res://") -> Dictionary:
	var entry := {"path": res_dir, "dir": true, "children": []}
	var dir := DirAccess.open(to_abs(res_dir))
	if dir == null: return entry
	dir.include_hidden = false
	var folders := Array(dir.get_directories())
	var files := Array(dir.get_files())
	folders.sort_custom(func(a, b): return a.naturalnocasecmp_to(b) < 0)
	files.sort_custom(func(a, b): return a.naturalnocasecmp_to(b) < 0)
	for folder in folders:
		if folder in IGNORED or folder.begins_with("."): continue
		entry.children.append(scan(res_dir.path_join(folder)))
	for file in files:
		if file.ends_with(".tmp") or file.begins_with("."): continue
		entry.children.append({"path": res_dir.path_join(file), "dir": false})
	return entry

## Every .jet file in the project (res:// paths).
func scripts() -> Array:
	var found := []
	var stack := [scan()]
	while not stack.is_empty():
		var entry: Dictionary = stack.pop_back()
		for child in entry.get("children", []):
			if child.dir: stack.append(child)
			elif str(child.path).get_extension() == SCRIPT_EXT: found.append(child.path)
	found.sort()
	return found

static func create(folder: String, project_title: String, scene_text: String, script_text: String) -> String:
	if is_project(folder): return "A project already exists in " + folder
	var dir := DirAccess.open(folder)
	if dir != null and (dir.get_files().size() > 0 or dir.get_directories().size() > 0):
		return "The folder is not empty: choose an empty or new one."
	DirAccess.make_dir_recursive_absolute(folder.path_join("scripts"))
	var config_text := JSON.stringify({"name": project_title, "main_scene": "res://main.jetscene", "target": "desktop"}, "\t") + "\n"
	for item in [[FILE, config_text], ["main.jetscene", scene_text], ["scripts/spin.jet", script_text]]:
		var error := write_text(folder.path_join(item[0]), item[1])
		if not error.is_empty(): return error
	return ""
