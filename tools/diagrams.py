#!/usr/bin/env python3
"""Draws docs/architecture.svg and docs/esp32-pipeline.svg (the README diagrams)."""
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
BG, CARD, EDGE = "#10141c", "#171d29", "#2a3446"
TEXT, MUTED = "#e2e7f0", "#8a93a6"
TEAL, ORANGE, BLUE, PURPLE, PINK = "#78d5bc", "#f0a45b", "#7aa2f7", "#c792ea", "#f07c9a"
SANS = "Inter, 'Segoe UI', Helvetica, Arial, sans-serif"
MONO = "'JetBrains Mono', Consolas, 'DejaVu Sans Mono', monospace"
out = []


def add(s):
    out.append(s)


def text(x, y, s, size=14, color=TEXT, weight=400, anchor="start", mono=False, spacing=0):
    s = s.replace("&", "&amp;").replace("<", "&lt;").replace(">", "&gt;")
    ls = f' letter-spacing="{spacing}"' if spacing else ""
    add(f'<text x="{x}" y="{y}" font-family="{MONO if mono else SANS}" font-size="{size}" font-weight="{weight}" '
        f'fill="{color}" text-anchor="{anchor}"{ls}>{s}</text>')


def card(x, y, w, h, title, color, note=""):
    add(f'<rect x="{x}" y="{y}" width="{w}" height="{h}" rx="16" fill="{CARD}" stroke="{EDGE}" stroke-width="1.5"/>')
    add(f'<rect x="{x + 20}" y="{y - 1.5}" width="{w - 40}" height="3" rx="1.5" fill="{color}"/>')
    text(x + 20, y + 32, title.upper(), 12, color, 700, spacing=1.6)
    if note:
        text(x + w - 20, y + 32, note, 12, MUTED, anchor="end", mono=True)


def chip(x, y, w, h, title, sub, color):
    add(f'<rect x="{x}" y="{y}" width="{w}" height="{h}" rx="10" fill="{color}" fill-opacity="0.10" '
        f'stroke="{color}" stroke-opacity="0.55" stroke-width="1.2"/>')
    text(x + 14, y + 24, title, 15, TEXT, 600)
    for i, line in enumerate(sub):
        text(x + 14, y + 44 + i * 17, line, 12, MUTED, mono=True)


def arrow(d, color, label=None, lx=0, ly=0, dashed=False):
    dash = ' stroke-dasharray="6 6"' if dashed else ""
    add(f'<path d="{d}" fill="none" stroke="{color}" stroke-width="2.2"{dash} marker-end="url(#head-{color[1:]})"/>')
    if label:
        width = len(label) * 7.2 + 16
        add(f'<rect x="{lx - width / 2}" y="{ly - 14}" width="{width}" height="21" rx="10.5" fill="{BG}" '
            f'stroke="{color}" stroke-opacity="0.6"/>')
        text(lx, ly + 1, label, 12, color, 600, "middle", mono=True)


def overview():
    W, H = 1200, 820
    add(f'<svg xmlns="http://www.w3.org/2000/svg" width="{W}" height="{H}" viewBox="0 0 {W} {H}">')
    add('<defs>')
    for c in (TEAL, ORANGE, BLUE, PURPLE, PINK, MUTED):
        add(f'<marker id="head-{c[1:]}" viewBox="0 0 10 10" refX="8.5" refY="5" markerWidth="7" markerHeight="7" orient="auto-start-reverse">'
            f'<path d="M0,0 L10,5 L0,10 z" fill="{c}"/></marker>')
    add(f'<linearGradient id="glow" x1="0" y1="0" x2="1" y2="1"><stop offset="0" stop-color="{ORANGE}"/><stop offset="1" stop-color="#e0823a"/></linearGradient>')
    add('</defs>')
    add(f'<rect width="{W}" height="{H}" rx="22" fill="{BG}"/>')

    # Logo and title
    add(f'<g transform="translate(40 34)"><rect x="0" y="0" width="26" height="26" rx="4" transform="rotate(45 13 13)" fill="none" stroke="{TEAL}" stroke-width="3"/>'
        f'<rect x="7" y="7" width="12" height="12" rx="2" transform="rotate(45 13 13)" fill="{TEAL}"/></g>')
    text(84, 56, "JET STUDIO", 24, TEXT, 800, spacing=3)
    text(292, 56, "architecture", 24, MUTED, 400)
    text(W - 40, 56, "one C++17 runtime · Jet renderer · everywhere", 13, MUTED, anchor="end")

    card(40, 90, 520, 300, "Editor", TEAL, "Godot 4 template")
    chip(64, 140, 472, 92, "GDScript app", ["editor/scripts · docks, Inspector, 3D view,", "script editor, Project Settings, Serial"], TEAL)
    chip(64, 274, 472, 92, "JetBridge  (GDExtension)", ["native/ · scene → Description, edit view,", "Play, compile, export_pack()  ·  JetSerial"], TEAL)
    arrow("M300,236 L300,268", TEAL)
    arrow("M282,268 L282,238", TEAL)

    card(640, 90, 520, 300, "runtime/  (C++17, no Godot types)", BLUE)
    chip(664, 140, 228, 104, "Runtime", ["scenes · autoloads", "physics · particles", "ScriptHost (Godot API)"], BLUE)
    chip(908, 140, 228, 104, "JetScript", ["lexer · parser · compiler", "→ bytecode → VM", "(devices: VM only)"], BLUE)
    chip(664, 260, 228, 106, "Jet renderer", ["third_party/Jet", "fixed-point raster", "RGB565 framebuffer"], BLUE)
    chip(908, 260, 228, 106, "Package codec", ["encode_pack()", "decode_pack()", "OBJ / PNG import"], BLUE)
    arrow("M560,320 L634,320", TEAL, "edit view · Play", 597, 300)

    # .jetpack cylinder
    add(f'<g transform="translate(520 446)">'
        f'<path d="M0,18 C0,4 160,4 160,18 L160,74 C160,88 0,88 0,74 Z" fill="url(#glow)"/>'
        f'<ellipse cx="80" cy="18" rx="80" ry="13" fill="#f6c08a"/>'
        f'</g>')
    text(600, 512, ".jetpack", 20, BG, 800, "middle", mono=True)
    text(600, 556, "scenes · autoloads · bytecode · textures · meshes · Input Map", 12, MUTED, "middle")
    arrow("M1022,390 L1022,420 L690,480", ORANGE, "export_pack()", 860, 440)

    card(40, 600, 340, 190, "tools/", PURPLE, "python")
    chip(64, 650, 292, 58, "tasks.py", ["package + headers → firmware build"], PURPLE)
    chip(64, 718, 292, 58, "hardware.py", ["boards & displays → generated headers"], PURPLE)
    arrow("M520,490 L300,490 L300,594", ORANGE)
    arrow("M180,390 L180,594", PURPLE, "Build / Flash", 180, 492)

    card(440, 600, 720, 190, "targets", PINK)
    text(548, 632, "VM-only runtime  ·  serial log → Serial panel", 12, MUTED, mono=True)
    chip(464, 650, 214, 126, "ESP32-S3 firmware", ["ESP-IDF · 2 cores", "half-width fields", "DMA scanout · PSRAM"], PINK)
    chip(694, 650, 214, 126, "STM32 F4/F7", ["Arduino core", "single core", "LovyanGFX SPI"], PINK)
    chip(924, 650, 212, 126, "Desktop player", ["jet_player (SDL2)", "jet_player_device", "= device emulator"], PINK)
    arrow("M356,690 L458,690", PURPLE)
    arrow("M420,690 L420,806 L801,806 L801,782", PURPLE)
    arrow("M682,505 L1030,505 L1030,644", ORANGE, "players", 900, 505)


def pipeline():
    W, H = 1200, 520
    X0, MS = 250, 28.0        # timeline start, pixels per millisecond
    LANES = [("core 1", "jet_game", TEAL), ("core 0", "jet_raster", BLUE), ("core 0", "jet_present", ORANGE), ("GDMA", "SPI → display", PINK)]
    LANE_Y, LANE_H, GAP = 150, 44, 16

    def lane_y(i): return LANE_Y + i * (LANE_H + GAP)

    def block(lane, t0, t1, label, color, opacity=0.9, text_color=BG):
        x, y, w = X0 + t0 * MS, lane_y(lane), (t1 - t0) * MS
        add(f'<rect x="{x + 1}" y="{y}" width="{w - 2}" height="{LANE_H}" rx="8" fill="{color}" fill-opacity="{opacity}"/>')
        if label: text(x + w / 2, y + LANE_H / 2 + 5, label, 12.5, text_color, 600, "middle")

    add(f'<svg xmlns="http://www.w3.org/2000/svg" width="{W}" height="{H}" viewBox="0 0 {W} {H}">')
    add('<defs><marker id="head" viewBox="0 0 10 10" refX="8.5" refY="5" markerWidth="7" markerHeight="7" orient="auto">'
        f'<path d="M0,0 L10,5 L0,10 z" fill="{TEXT}"/></marker></defs>')
    add(f'<rect width="{W}" height="{H}" rx="22" fill="{BG}"/>')
    text(40, 56, "ESP32-S3 frame pipeline", 24, TEXT, 800)
    text(W - 40, 56, "neon_car on the Nano: ~16 ms per field, ~62 fields/s", 13, MUTED, anchor="end")

    for i, (core, task, color) in enumerate(LANES):
        y = lane_y(i)
        add(f'<rect x="40" y="{y}" width="{W - 80}" height="{LANE_H}" rx="10" fill="{CARD}" stroke="{EDGE}"/>')
        add(f'<circle cx="60" cy="{y + LANE_H / 2}" r="5" fill="{color}"/>')
        text(76, y + 20, core, 12, MUTED, mono=True)
        text(76, y + 36, task, 14, TEXT, 600, mono=True)

    # Frame boundaries
    for f, t in enumerate((0, 16, 32)):
        x = X0 + t * MS
        add(f'<line x1="{x}" y1="{LANE_Y - 26}" x2="{x}" y2="{lane_y(3) + LANE_H + 8}" stroke="{EDGE}" stroke-width="2" stroke-dasharray="4 6"/>')
    for f, t in enumerate((0, 16)):
        x = X0 + t * MS + 8 * MS
        text(x, LANE_Y - 14, f"frame N{'' if f == 0 else '+1'} · field {'odd' if f == 0 else 'even'} rows", 13, MUTED, 600, "middle")

    for f, t in enumerate((0, 16)):
        block(0, t + 0.0, t + 2.4, "update", TEAL)
        block(0, t + 2.4, t + 10.5, "frame setup · upper band", BLUE)
        block(1, t + 3.2, t + 10.5, "lower band (adaptive split)", BLUE)
        block(0, t + 10.5, t + 12.0, "join", MUTED, 0.35, TEXT)
        previous = "N-1" if f == 0 else "N"
        block(2, t + 0.3, t + 16, f"rows of field {previous}: ×2 columns, byte swap, HUD", ORANGE, 0.85)
        block(3, t + 0.5, t + 16, f"field {previous} → panel (SPI 40 MHz)", PINK, 0.85)

    # Hand-off: field N goes to the present task
    x = X0 + 12.0 * MS
    add(f'<path d="M{x},{lane_y(0) + LANE_H} C{x},{lane_y(2) - 20} {X0 + 16.3 * MS},{lane_y(2) - 30} {X0 + 16.3 * MS},{lane_y(2) - 2}" '
        f'fill="none" stroke="{TEXT}" stroke-width="2" marker-end="url(#head)"/>')
    text(x - 8, lane_y(1) + 27, "hand-off", 12, TEXT, 600, "end")

    notes = [
        (TEAL, "update(): fixed 60 Hz physics, _process, scene changes, transforms"),
        (BLUE, "Jet rasterizes half-width interlaced fields, no z-buffer (depth-sorted triangles)"),
        (ORANGE, "two field buffers in internal DMA RAM: one is drawn while the other is sent"),
        (PINK, "the GDMA queue streams rows; 2D nodes are composited per row at full resolution"),
    ]
    for i, (color, note) in enumerate(notes):
        y = lane_y(3) + LANE_H + 48 + i * 22
        add(f'<rect x="40" y="{y - 10}" width="12" height="12" rx="3" fill="{color}"/>')
        text(62, y + 1, note, 13, MUTED)


def save(name, draw):
    out.clear()
    draw()
    add("</svg>")
    (ROOT / "docs" / name).write_text("\n".join(out) + "\n")
    print(f"Wrote docs/{name}")


if __name__ == "__main__":
    save("architecture.svg", overview)
    save("esp32-pipeline.svg", pipeline)
