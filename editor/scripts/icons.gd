extends RefCounted
## Godot's editor icons (MIT), rasterized by tools/assets.py and loaded without the importer.

static var _cache := {}

static func get_icon(icon_name: String) -> Texture2D:
	if _cache.has(icon_name): return _cache[icon_name]
	var texture: Texture2D = null
	var path := "res://icons/%s.png" % icon_name
	if FileAccess.file_exists(path):
		var image := Image.load_from_file(path)
		if image != null and not image.is_empty():
			image.generate_mipmaps()
			var icon := ImageTexture.create_from_image(image)
			# The PNGs are 32 px: shown at 16 they stay sharp at 2x scale, and mipmaps handle 1x.
			icon.set_size_override(Vector2i(16, 16))
			texture = icon
	_cache[icon_name] = texture
	return texture

## Theme icon rasterized at 2x (res://theme_icons), drawn at its native size.
static func theme_icon(file: String, size: Array) -> Texture2D:
	var image := Image.load_from_file("res://theme_icons/" + file) if not file.is_empty() else null
	if image == null or image.is_empty(): return null
	image.generate_mipmaps()
	var icon := ImageTexture.create_from_image(image)
	icon.set_size_override(Vector2i(int(size[0]), int(size[1])))
	return icon

static func for_node(type: String) -> Texture2D:
	return get_icon(type)

static func for_file(path: String) -> Texture2D:
	match path.get_extension():
		"jetscene": return get_icon("PackedScene")
		"jet": return get_icon("Script")
		"jetproject": return get_icon("Tools")
		"jetpack": return get_icon("Object")
		"png": return get_icon("ImageTexture")
		"obj": return get_icon("ArrayMesh")
		"mtl": return get_icon("StandardMaterial3D")
	return get_icon("File")
