extends SceneTree
# Packs the editor project into a .pck that tools/manage.py appends to the release executable.
# Usage: godot --headless --path editor --script tools/pack_editor.gd -- out.pck

# Not needed at run time: the bridge library sits next to the executable, the SVGs are only sources.
const SKIPPED := ["res://bin", "res://assets/src", "res://game.jetpack"]

var _count := 0


func _init() -> void:
	var out: String = OS.get_cmdline_user_args()[0]
	var packer := PCKPacker.new()
	if packer.pck_start(out) != OK:
		printerr("Cannot write " + out)
		quit(1)
		return
	_add_folder(packer, "res://")
	var classes := "res://.godot/global_script_class_cache.cfg"
	if FileAccess.file_exists(classes): _add(packer, classes)
	if packer.flush() != OK:
		printerr("Cannot write " + out)
		quit(1)
		return
	print("Packed %d files into %s" % [_count, out])
	quit()


func _add_folder(packer: PCKPacker, folder: String) -> void:
	for file in DirAccess.get_files_at(folder):
		_add(packer, folder.path_join(file))
	for sub in DirAccess.get_directories_at(folder):
		var path := folder.path_join(sub)
		if not sub.begins_with(".") and not path in SKIPPED: _add_folder(packer, path)


func _add(packer: PCKPacker, path: String) -> void:
	if path in SKIPPED: return
	packer.add_file(path, ProjectSettings.globalize_path(path))
	_count += 1
