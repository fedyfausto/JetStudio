extends RefCounted
# A folder with a project.jetproject file, used as res:// like a Godot project.

const FILE := "project.jetproject"
const SCENE_EXT := "jetscene"
const SCRIPT_EXT := "jet"
const IGNORED := [".git", ".jetstudio", ".godot", "build", "exports"]

# absolute folder, without trailing slash
var root := ""
var config := {}


static func isProject(folder: String) -> bool:
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
	return writeText(root.path_join(FILE), JSON.stringify(config, "\t") + "\n")


func projectName() -> String:
	return str(config.get("name", root.get_file()))


func main_scene() -> String:
	return str(config.get("main_scene", ""))


func toAbs(resPath: String) -> String:
	if resPath.begins_with("res://"): return root.path_join(resPath.trim_prefix("res://"))

	return resPath


func toRes(absPath: String) -> String:
	if absPath == root: return "res://"

	if absPath.begins_with(root + "/"): return "res://" + absPath.trim_prefix(root + "/")

	return absPath


func exists(resPath: String) -> bool:
	return FileAccess.file_exists(toAbs(resPath)) or DirAccess.dir_exists_absolute(toAbs(resPath))


func read(resPath: String) -> String:
	return FileAccess.get_file_as_string(toAbs(resPath))


# Writes a .tmp file and renames it over the target, so a failed write never leaves half a file.
static func writeText(path: String, text: String) -> String:
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


func write(resPath: String, text: String) -> String:
	return writeText(toAbs(resPath), text)


# Recursive listing: [{"path": "res://...", "dir": bool, "children": [...]}].
func scan(resDir: String = "res://") -> Dictionary:
	var entry := {"path": resDir, "dir": true, "children": []}
	var dir := DirAccess.open(toAbs(resDir))

	if dir == null: return entry

	dir.include_hidden = false
	var folders := Array(dir.get_directories())
	var files := Array(dir.get_files())
	folders.sort_custom(func(a, b): return a.naturalnocasecmp_to(b) < 0)
	files.sort_custom(func(a, b): return a.naturalnocasecmp_to(b) < 0)

	for folder in folders:
		if folder in IGNORED or folder.begins_with("."): continue

		entry.children.append(scan(resDir.path_join(folder)))

	for file in files:
		if file.ends_with(".tmp") or file.begins_with("."): continue

		entry.children.append({"path": resDir.path_join(file), "dir": false})

	return entry


# Every .jet file in the project (res:// paths).
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


static func create(folder: String, projectTitle: String, sceneText: String, scriptText: String) -> String:
	if isProject(folder): return "A project already exists in " + folder

	var dir := DirAccess.open(folder)

	if dir != null and (dir.get_files().size() > 0 or dir.get_directories().size() > 0):
		return "The folder is not empty: choose an empty or new one."

	DirAccess.make_dir_recursive_absolute(folder.path_join("scripts"))
	var configText := JSON.stringify({"name": projectTitle, "main_scene": "res://main.jetscene", "target": "desktop"}, "\t") + "\n"

	for item in [[FILE, configText], ["main.jetscene", sceneText], ["scripts/spin.jet", scriptText]]:
		var error := writeText(folder.path_join(item[0]), item[1])

		if not error.is_empty(): return error

	return ""
