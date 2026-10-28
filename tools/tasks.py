#!/usr/bin/env python3
"""Builds and flashes started by the editor, run as a separate process so the UI stays responsive."""
import argparse
import json
import os
from pathlib import Path
import shutil
import struct
import subprocess
import sys

import hardware

ROOT = Path(__file__).resolve().parents[1]


def generate_scene(pack, firmware):
    data = pack.read_bytes()
    # Just a sanity check, the device validates the whole package anyway.
    if len(data) < 12 or data[:4] != b"JETP":
        raise RuntimeError("Not a .jetpack file: export the project again")
    version = struct.unpack_from("<I", data, 4)[0]
    if version < 14:
        # The editor exports with the bridge it loaded at startup, so an old version means a stale editor.
        raise RuntimeError(f"The package is version {version}, the firmware reads version 14: Jet Studio was updated "
                           "while it was running. Restart Jet Studio and build again")
    if version != 14:
        raise RuntimeError(f"Unknown .jetpack version {version} (expected 14): update Jet Studio")
    if len(data) > 1024 * 1024:
        raise RuntimeError("Scene package larger than 1 MiB")
    lines = ["#pragma once", "// Generated from the editor scene. Do not edit.", "#include <cstdint>",
             "static constexpr uint8_t scene_data[] = {"]
    for offset in range(0, len(data), 16):
        lines.append("    " + ", ".join(f"0x{byte:02x}" for byte in data[offset:offset+16]) + ",")
    lines.append("};\n")
    hardware.write_if_changed(firmware / "src/generated_scene.hpp", "\n".join(lines))
    print(f"Scene embedded: {len(data)} bytes", flush=True)


# Godot's built-in actions come first in the Input Map, same as in editor/scripts/input_map.gd.
BUILTIN_ACTIONS = ["ui_left", "ui_right", "ui_up", "ui_down", "ui_accept", "ui_cancel"]


def input_map(config):
    """[(action, pin)] in bit order, built the same way as in the editor."""
    stored = [a for a in config.get("input_map", []) if isinstance(a, dict)]
    legacy = config.get("input_pins", {})  # old project format
    by_name = {str(a.get("name", "")): a for a in stored}
    out = [(name, int(by_name.get(name, {}).get("pin", legacy.get(name, -1)))) for name in BUILTIN_ACTIONS]
    for action in stored:
        name = str(action.get("name", ""))
        if name not in BUILTIN_ACTIONS and name and len(out) < 32:
            out.append((name, int(action.get("pin", -1))))
    return out


def generate_inputs(config, firmware, board):
    """Writes generated_inputs.hpp; returns the [(action, pin)] that are wired, for the hardware check."""
    actions = input_map(config)
    pins = [pin for _, pin in actions]
    used = [p for p in pins if p >= 0]
    if len(used) != len(set(used)):
        raise RuntimeError("Two actions use the same pin")
    active_high = sum(1 << i for i, pin in enumerate(pins) if pin >= 0 and pin in board.get("active_high", []))
    content = ("#pragma once\n#include <cstdint>\n// Generated from the project's Input Map (project.jetproject). Do not edit.\n"
               "// One pin per action, in package order (bit i = action i). -1 means no button.\n"
               "// ESP32: GPIO number. STM32: PinName (port * 16 + pin).\n"
               "// " + ", ".join(name for name, _ in actions) + "\n"
               "// Buttons connect the pin to GND (internal pull-up). Bit i of input_active_high: the board's own button,\n"
               "// pressed = high (it has its own pull-down).\n"
               f"static constexpr int input_pins[{len(pins)}] = {{{', '.join(map(str, pins))}}};\n"
               f"static constexpr uint32_t input_active_high = 0x{active_high:x};\n")
    hardware.write_if_changed(firmware / "src/generated_inputs.hpp", content)
    print("Buttons: " + (", ".join(f"{a}={hardware.pin_label(board, p)}" for a, p in actions if p >= 0) or "none"), flush=True)
    return [(a, p) for a, p in actions if p >= 0]


def pack_header(pack):
    """Reads (width, height) and the action names from a .jetpack header."""
    data = pack.read_bytes()
    name_length = struct.unpack_from("<I", data, 8)[0]
    offset = 12 + name_length
    width, height = struct.unpack_from("<HH", data, offset)
    offset += 4
    count = data[offset]
    offset += 1
    names = []
    for _ in range(count):
        length = struct.unpack_from("<I", data, offset)[0]
        names.append(data[offset + 4:offset + 4 + length].decode("utf-8"))
        offset += 4 + length
        keys = data[offset]
        offset += 1
        for _ in range(keys):
            offset += 4 + struct.unpack_from("<I", data, offset)[0]
    return (width, height), names


def refresh_sdkconfig(environment):
    """ESP-IDF only reads sdkconfig.defaults when creating sdkconfig.<env>, so delete it when a default changed."""
    firmware = ROOT / "firmware"
    generated = firmware / f"sdkconfig.{environment}"
    board = environment.removesuffix("_release")
    defaults = [firmware / "sdkconfig.defaults", firmware / "boards" / f"{board}.defaults"]
    if environment.endswith("_release"):
        defaults.append(firmware / "sdkconfig.release")
    if generated.exists() and any(d.stat().st_mtime > generated.stat().st_mtime for d in defaults):
        print(f"Profile settings changed: regenerating {generated.name}", flush=True)
        generated.unlink()


def pio_executable():
    executable = shutil.which("pio")
    if executable:
        return executable
    local = Path.home() / ".platformio/penv/bin/pio"
    if local.is_file():
        return str(local)
    raise RuntimeError("PlatformIO Core is missing. Install it before building the firmware.")


def execute(args):
    if args.target == "desktop":
        if args.action == "flash":
            raise RuntimeError("Select a board target for flashing")
        if not (ROOT / "CMakeLists.txt").exists():
            print("Release package: the PC players are already built.", flush=True)
            return 0
        commands = [["cmake", "-S", str(ROOT), "-B", str(ROOT / "build"), "-G", "Ninja", "-DCMAKE_BUILD_TYPE=Release"],
                    ["cmake", "--build", str(ROOT / "build"), "--target", "jet_player", "jet_player_device", "-j", "4"]]
        cwd = ROOT
    else:
        if not (ROOT / "firmware/components/LovyanGFX/CMakeLists.txt").exists():
            raise RuntimeError("LovyanGFX missing. Run ./jet-studio setup")
        config = json.loads((args.project / "project.jetproject").read_text()) if args.project else {}
        _, profile, _ = hardware.settings(config)
        firmware = hardware.firmware_dir(profile)
        generate_scene(args.pack, firmware)
        buttons = generate_inputs(config, firmware, profile)
        board, size, name, firmware = hardware.generate(config, buttons)
        resolution, actions = pack_header(args.pack)
        if tuple(resolution) != tuple(size):
            raise RuntimeError(f"The scene is {resolution[0]}x{resolution[1]} but the display is "
                               f"{size[0]}x{size[1]}: export the project again with its hardware settings")
        if actions != [action for action, _ in input_map(config)]:
            raise RuntimeError("The package's Input Map differs from the project's: export the project again")
        environment = board + ("_release" if args.profile == "release" else "")
        esp32 = firmware == hardware.FIRMWARE
        if esp32:
            refresh_sdkconfig(environment)
        print(f"Hardware: {name}, {size[0]}x{size[1]}", flush=True)
        print(f"Profile: {args.profile} ({environment})", flush=True)
        command = [pio_executable(), "run", "-e", environment]
        if args.action == "flash": command += ["-t", "upload"]
        if args.port and esp32: command += ["--upload-port", args.port]  # STM32: ST-LINK, no port
        commands, cwd = [command], firmware
    for command in commands:
        print("+", " ".join(command), flush=True)
        result = subprocess.run(command, cwd=cwd)
        if result.returncode:
            return result.returncode
    return 0


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("action", choices=["build", "flash"])
    # "nano" is the old name of "device".
    parser.add_argument("--target", choices=["desktop", "device", "nano"], default="desktop")
    parser.add_argument("--pack", type=Path, required=True)
    parser.add_argument("--port")
    parser.add_argument("--profile", choices=["debug", "release"], default="debug",
                        help="Firmware: debug keeps the serial log, release removes console and logs")
    parser.add_argument("--project", type=Path, help="Project folder: hardware and button pins from project.jetproject")
    parser.add_argument("--job", type=Path)
    args = parser.parse_args()
    result = 1
    stream = None
    if args.job:
        args.job.parent.mkdir(parents=True, exist_ok=True)
        stream = open(str(args.job) + ".log", "w", buffering=1)
        os.dup2(stream.fileno(), 1)
        os.dup2(stream.fileno(), 2)
    try:
        result = execute(args)
    except Exception as error:
        print(f"ERROR: {error}", flush=True)
    finally:
        sys.stdout.flush()
        sys.stderr.flush()
        if args.job:
            path = Path(str(args.job) + ".json")
            temporary = path.with_suffix(".tmp")
            temporary.write_text(json.dumps({"returncode": result}))
            temporary.replace(path)
    return result


if __name__ == "__main__":
    sys.exit(main())
