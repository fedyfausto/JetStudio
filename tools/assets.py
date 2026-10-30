#!/usr/bin/env python3
"""Regenerates the editor assets: logos, Godot editor icons (MIT) and fonts.

Our UI-only Godot build has no SVG module, so SVGs are rasterized here with librsvg.
Needs python3-gi, gir1.2-rsvg-2.0, python3-cairo, python3-pil and fonts-dejavu-core.
"""
from pathlib import Path
import shutil

import cairo
import gi

gi.require_version("Rsvg", "2.0")
from gi.repository import Rsvg  # noqa: E402

ROOT = Path(__file__).resolve().parents[1]
GODOT = ROOT / "third_party/godot"
EDITOR = ROOT / "editor"

# Godot editor icons we use; the 16 px originals are rendered at 2x.
ICONS = """
Node Node3D MeshInstance3D Camera3D DirectionalLight3D WorldEnvironment Environment
BoxMesh SphereMesh PlaneMesh PrismMesh CylinderMesh CapsuleMesh Mesh
Folder File PackedScene Script ScriptCreate ScriptRemove ScriptExtend FolderCreate New Load Save Reload Rename
Duplicate Remove Filesystem ShowInFileSystem Add Instance GuiVisibilityVisible GuiVisibilityHidden Search Lock Unlock
Reparent MoveUp MoveDown CopyNodePath TripleBar Close Play PlayScene PlayCustom Pause Stop NextFrame ToolSelect
ToolMove ToolRotate ToolScale Object Snap SnapGrid Grid GuiTreeArrowDown GuiTreeArrowRight StatusError
StatusWarning StatusSuccess Error Warning Info Debug DebugNext Clear Help Tools Edit Popup MemberMethod
MemberProperty MemberConstant Vector3 float int bool String Color Signal Groups Collapse Expand Pin Back Forward
History ExternalLink GuiChecked GuiUnchecked CodeFoldDownArrow CodeFoldedRightArrow Game Keyboard Mouse
ViewportSpeed Viewport GizmoCamera3D GizmoDirectionalLight Editor3DHandle EditorPivot ToolAddNode NewRoot
CloseScene Sort Filesystem FileList FileTree Favorites ArrowLeft ArrowRight ArrowUp ArrowDown Bookmark
Breakpoint ErrorWarning ImportCheck ImportFail Tools Export ImageTexture Image Texture2D PanoramaSkyMaterial
ProceduralSkyMaterial Sky StandardMaterial3D GuiToggleOn GuiToggleOff GuiRadioChecked GuiRadioUnchecked
GuiCheckedDisabled GuiUncheckedDisabled Control CanvasItem Label TextureRect ColorRect QuadMesh TorusMesh ArrayMesh
MeshItem Vector2 Font StaticBody3D CharacterBody3D RigidBody3D Area3D CollisionShape3D CPUParticles3D NodeWarning BoxShape3D SphereShape3D
CapsuleShape3D CollisionObject3D PhysicsBody3D Sprite3D AnimatedSprite3D Node2D Sprite2D AnimatedSprite2D Camera2D
StaticBody2D CharacterBody2D RigidBody2D Area2D CollisionShape2D CollisionObject2D PhysicsBody2D RectangleShape2D CircleShape2D CapsuleShape2D
Timer Slot Signals AnimationPlayer Animation Key KeyValue KeyCall PlayBackwards PlayStart PlayStartBackwards Loop PingPongLoop AutoPlay InterpLinear InterpRaw
""".split()


def intrinsic_size(svg: Path):
    handle = Rsvg.Handle.new_from_file(str(svg))
    ok, width, height = handle.get_intrinsic_size_in_pixels()
    return (round(width), round(height)) if ok else (16, 16)


def render(svg: Path, png: Path, width: int, height: int):
    handle = Rsvg.Handle.new_from_file(str(svg))
    surface = cairo.ImageSurface(cairo.FORMAT_ARGB32, width, height)
    context = cairo.Context(surface)
    viewport = Rsvg.Rectangle()
    viewport.x, viewport.y, viewport.width, viewport.height = 0, 0, width, height
    handle.render_document(context, viewport)
    png.parent.mkdir(parents=True, exist_ok=True)
    surface.write_to_png(str(png))


THEME_ICONS = EDITOR / "theme_icons"


def build_theme_icons():
    """Rasterizes the icons of Godot's built-in theme (arrows, check boxes, sliders...) at 2x.

    Godot builds them from SVG at startup, which our build can't do.
    The mapping is default_theme.cpp's, with the overrides from the editor's theme_modern.cpp on top.
    """
    import json
    import re
    default_source = (GODOT / "scene/theme/default_theme.cpp").read_text()
    types = set(re.findall(r'theme->set_\w+\("[^"]+", "(\w+)"', default_source))
    mapping = {}
    for item, type_name, name in re.findall(r'theme->set_icon\("(\w+)", "(\w+)", icons\["(\w+)"\]\)', default_source):
        mapping[(item, type_name)] = ("default", name)
    editor_source = (GODOT / "editor/themes/theme_modern.cpp").read_text()
    pattern = r'set_icon\("(\w+)", "(\w+)", p_theme->get_icon\(SNAME\("(\w+)"\), EditorStringName\(EditorIcons\)\)'
    for item, type_name, name in re.findall(pattern, editor_source):
        if type_name in types and (GODOT / "editor/icons" / f"{name}.svg").exists():
            mapping[(item, type_name)] = ("editor", name)
    if THEME_ICONS.exists():
        shutil.rmtree(THEME_ICONS)
    icons = {}
    for source, name in sorted(set(mapping.values())):
        svg = GODOT / ("scene/theme/icons" if source == "default" else "editor/icons") / f"{name}.svg"
        file = f"{'default_' if source == 'default' else ''}{name}.png"
        if not svg.exists():
            continue
        width, height = intrinsic_size(svg)
        render(svg, THEME_ICONS / file, width * 2, height * 2)
        icons[name if source == "editor" else "default_" + name] = {"file": file, "size": [width, height]}
    items = sorted([item, type_name, name if source == "editor" else "default_" + name]
                   for (item, type_name), (source, name) in mapping.items()
                   if (name if source == "editor" else "default_" + name) in icons)
    (THEME_ICONS / "theme_icons.json").write_text(json.dumps({"icons": icons, "items": items}, indent=1) + "\n")
    shutil.copy(GODOT / "LICENSE.txt", THEME_ICONS / "LICENSE.Godot.txt")
    return len(items)


FONT_SOURCE = Path("/usr/share/fonts/truetype/dejavu/DejaVuSansMono-Bold.ttf")


def build_font():
    """1-bit 7x15 font for the runtime's 2D Label nodes (ASCII and Latin-1)."""
    from PIL import Image, ImageDraw, ImageFont
    font = ImageFont.truetype(str(FONT_SOURCE), 12)
    codes = list(range(32, 127)) + list(range(160, 256))
    rows = []
    for code in codes:
        image = Image.new("L", (7, 15), 0)
        ImageDraw.Draw(image).text((0, 0), chr(code), font=font, fill=255)
        pixels = image.load()
        for y in range(15):
            rows.append(sum(1 << (6 - x) for x in range(7) if pixels[x, y] > 110))
    lines = []
    for i, code in enumerate(codes):
        glyph = ", ".join(f"0x{v:02x}" for v in rows[i * 15:(i + 1) * 15])
        # A trailing backslash would continue the C++ comment onto the next line.
        label = "backslash" if code == 92 else (chr(code) if code > 32 and code != 173 else "")
        lines.append(f"    {{{glyph}}}, // {code} {label}".rstrip())
    (ROOT / "runtime/font_data.hpp").write_text(
        "#pragma once\n// Generated by tools/assets.py from DejaVu Sans Mono Bold 12 px (Bitstream Vera licence, see runtime/LICENSE.DejaVu.txt).\n"
        "// 7x15 cells, one byte per row, bit 6 = left.\n"
        "#include <cstdint>\n\nnamespace jetstudio::font {\nconstexpr int Width = 7, Height = 15;\n"
        "// Glyphs for code points 32..126, then 160..255.\n"
        f"constexpr uint8_t Glyphs[{len(codes)}][15] = {{\n" + "\n".join(lines) + "\n};\n} // namespace jetstudio::font\n")


def main():
    source = EDITOR / "assets/src"
    render(source / "boot_splash.svg", EDITOR / "assets/boot_splash.png", 820, 300)
    render(source / "app_icon.svg", EDITOR / "assets/app_icon.png", 256, 256)
    render(source / "emblem.svg", EDITOR / "assets/emblem.png", 64, 64)
    missing = []
    for name in sorted(set(ICONS)):
        svg = GODOT / "editor/icons" / f"{name}.svg"
        if not svg.exists():
            missing.append(name)
            continue
        render(svg, EDITOR / "icons" / f"{name}.png", 32, 32)
    shutil.copy(GODOT / "LICENSE.txt", EDITOR / "icons/LICENSE.Godot.txt")
    fonts = GODOT / "thirdparty/fonts"
    for name in ["Inter_Regular.woff2", "Inter_Bold.woff2", "JetBrainsMono_Regular.woff2",
                 "LICENSE.Inter.txt", "LICENSE.JetBrainsMono.txt"]:
        shutil.copy(fonts / name, EDITOR / "fonts" / name)
    theme_items = build_theme_icons()
    build_font()
    shutil.copy("/usr/share/doc/fonts-dejavu-core/copyright", ROOT / "runtime/LICENSE.DejaVu.txt")
    print(f"Assets regenerated; {theme_items} theme icons; icons not found in Godot: {', '.join(missing) or 'none'}")


if __name__ == "__main__":
    main()
