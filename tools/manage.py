#!/usr/bin/env python3
"""Development commands behind ./jet-studio."""
import argparse
import json
import os
from pathlib import Path
import shutil
import struct
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]
GODOT = ROOT / "third_party/godot/bin/godot.linuxbsd.template_debug.x86_64"
RELEASE_GODOT = ROOT / "third_party/godot/bin/godot.linuxbsd.template_release.x86_64"
LOCK = ROOT / "dependencies.lock.json"


# Terminals inside snap apps (VS Code, for one) export the snap's GLib/GTK paths.
# Under Wayland libdecor's GTK plugin then loads the wrong settings schema and aborts, so drop them.
SNAP_VARIABLES = ["GSETTINGS_SCHEMA_DIR", "GTK_PATH", "GTK_EXE_PREFIX", "GIO_MODULE_DIR", "GDK_PIXBUF_MODULE_FILE",
                  "GDK_PIXBUF_MODULEDIR", "GTK_IM_MODULE_FILE", "LOCPATH"]


def clean_snap_environment():
    # Don't rely on $SNAP, some terminals keep the snap paths without it.
    # VS Code's snap stashes the user's original values in NAME_VSCODE_SNAP_ORIG.
    for saved in [key for key in os.environ if key.endswith("_VSCODE_SNAP_ORIG")]:
        original = os.environ.pop(saved)
        name = saved[: -len("_VSCODE_SNAP_ORIG")]
        if original:
            os.environ[name] = original
        else:
            os.environ.pop(name, None)
    for name in SNAP_VARIABLES:
        if "/snap/" in os.environ.get(name, ""):
            os.environ.pop(name)
    for name in ["XDG_DATA_DIRS", "XDG_CONFIG_DIRS"]:
        if name in os.environ:
            os.environ[name] = ":".join(part for part in os.environ[name].split(":") if "/snap/" not in part)


def display_arguments():
    """Native Wayland keeps text sharp on HiDPI; under XWayland the compositor upscales a low-res window."""
    choice = os.environ.get("JET_STUDIO_DISPLAY", "auto")
    if choice == "auto":
        choice = "wayland" if os.environ.get("WAYLAND_DISPLAY") else "x11"
    return ["--display-driver", choice]


def prepare_desktop():
    # Keep the dev editor's settings and logs in .cache/ instead of the user's home.
    for name, folder in [("XDG_DATA_HOME", "data"), ("XDG_CONFIG_HOME", "config"), ("XDG_CACHE_HOME", "cache")]:
        path = ROOT / ".cache" / folder
        path.mkdir(parents=True, exist_ok=True)
        os.environ[name] = str(path)
    cache = ROOT / "editor/.godot"
    cache.mkdir(exist_ok=True)
    classes = cache / "global_script_class_cache.cfg"
    if not classes.exists():
        classes.write_text("list=[]\n")


def run(command, cwd=ROOT):
    print("+", " ".join(map(str, command)), flush=True)
    subprocess.run(list(map(str, command)), cwd=cwd, check=True)


def setup(update=False):
    dependencies = json.loads(LOCK.read_text())
    for name, dependency in dependencies.items():
        path = ROOT / ("firmware/components" if name == "LovyanGFX" else "third_party") / name
        if not (path / ".git").exists():
            if path.exists() and any(path.iterdir()):
                raise RuntimeError(f"{path} exists without a Git checkout; inspect it before updating")
            path.mkdir(parents=True, exist_ok=True)
            run(["git", "init", "-q", path])
        revision = "master" if update and name in ("godot", "godot-cpp") else dependency["revision"]
        current = subprocess.run(["git", "-C", str(path), "rev-parse", "HEAD"], capture_output=True, text=True)
        if not update and current.stdout.strip() == revision:
            continue
        status = subprocess.check_output(["git", "-C", str(path), "status", "--porcelain"], text=True)
        if status.strip():
            raise RuntimeError(f"Local changes in {path}; refusing to overwrite them")
        run(["git", "-C", path, "fetch", "--depth", "1", dependency["url"], revision])
        run(["git", "-C", path, "checkout", "--detach", "FETCH_HEAD"])
        dependency["revision"] = subprocess.check_output(["git", "-C", str(path), "rev-parse", "HEAD"], text=True).strip()
    LOCK.write_text(json.dumps(dependencies, indent=2) + "\n")


def build_godot(jobs, target="template_debug"):
    if not (ROOT / "third_party/godot/SConstruct").exists():
        raise RuntimeError("Run ./jet-studio setup first")
    run(["scons", "platform=linuxbsd", f"target={target}", "dev_build=no",
         "debug_symbols=no", "optimize=speed", "disable_3d=yes", "disable_path_overrides=no",
         "vulkan=no", "opengl3=yes", "modules_enabled_by_default=no",
         "module_gdscript_enabled=yes", "module_freetype_enabled=yes",
         "module_text_server_fb_enabled=yes", f"-j{jobs}"], ROOT / "third_party/godot")


def build(jobs):
    run(["cmake", "-S", ROOT, "-B", ROOT / "build", "-G", "Ninja", "-DCMAKE_BUILD_TYPE=Release"])
    run(["cmake", "--build", ROOT / "build", "-j", jobs])


def version():
    for line in (ROOT / "editor/scripts/studio.gd").read_text().splitlines():
        if line.startswith("const VERSION"):
            return line.split('"')[1]
    raise RuntimeError("VERSION not found in studio.gd")


def skip_in_release(folder, names):
    skipped = {".git", ".github", ".godot", ".pio", "__pycache__", ".jetstudio", "game.jetpack"}
    if Path(folder).name == "LovyanGFX":
        kept = {"src", "CMakeLists.txt", "boards.cmake", "component.mk", "library.json", "library.properties", "license.txt"}
        return [name for name in names if name not in kept]
    # sdkconfig.<board> files are generated per machine by ESP-IDF.
    return [name for name in names if name in skipped
            or (name.startswith("sdkconfig.") and name not in ("sdkconfig.defaults", "sdkconfig.release"))]


def embed_pack(executable, pck):
    """Appends a .pck the way Godot's exporter embeds it: padding, package, its size and the GDPC magic."""
    data = pck.read_bytes()
    with open(executable, "r+b") as f:
        f.seek(0, os.SEEK_END)
        f.write(b"\0" * (-f.tell() % 8))
        start = f.tell()
        f.write(data)
        f.write(b"\0" * (-(f.tell() + 12) % 8))
        f.write(struct.pack("<Q", f.tell() - start) + b"GDPC")


def release(jobs):
    """Portable folder and tarball in dist/, laid out like the repo so the editor and tasks.py find everything."""
    build_godot(jobs, "template_release")
    build(jobs)
    run(["ctest", "--test-dir", ROOT / "build", "--output-on-failure"])
    name = f"jet-studio-{version()}-linux-x86_64"
    out = ROOT / "dist" / name
    if out.exists():
        shutil.rmtree(out)
    out.mkdir(parents=True)

    # No neon_car: its car model and livery are not ours to redistribute.
    for path in ["LICENSE", "README.md", "docs", "hardware", "runtime", "cmake/JetSources.cmake", "firmware", "firmware_stm32",
                 "tools/tasks.py", "tools/hardware.py", "third_party/Jet/src", "third_party/Jet/LICENSE",
                 "examples/first_world", "examples/jetrun", "examples/physics_demo", "examples/textured_boxes"]:
        source, target = ROOT / path, out / path
        target.parent.mkdir(parents=True, exist_ok=True)
        if source.is_dir():
            shutil.copytree(source, target, ignore=skip_in_release)
        else:
            shutil.copy2(source, target)
    # The editor travels inside the executable; Godot finds the bridge library next to it.
    (out / "bin").mkdir()
    executable = out / "bin/jet-studio.x86_64"
    shutil.copy2(RELEASE_GODOT, executable)
    run(["strip", executable])
    prepare_desktop()
    pck = out / "editor.pck"
    run([RELEASE_GODOT, "--headless", "--path", ROOT / "editor", "--script", ROOT / "tools/pack_editor.gd", "--", pck])
    embed_pack(executable, pck)
    pck.unlink()
    shutil.copy2(ROOT / "editor/bin/libjet_bridge.so", out / "bin/libjet_bridge.so")
    run(["strip", "--strip-unneeded", out / "bin/libjet_bridge.so"])
    shutil.copy2(ROOT / "tools/launcher.sh", out / "jet-studio")
    (out / "build").mkdir()
    for player in ["jet_player", "jet_player_device"]:
        shutil.copy2(ROOT / "build" / player, out / "build" / player)
        run(["strip", out / "build" / player])

    licenses = out / "licenses"
    licenses.mkdir()
    shutil.copy2(ROOT / "third_party/godot/LICENSE.txt", licenses / "Godot.LICENSE.txt")
    shutil.copy2(ROOT / "third_party/godot/COPYRIGHT.txt", licenses / "Godot.COPYRIGHT.txt")
    shutil.copy2(ROOT / "third_party/godot-cpp/LICENSE.md", licenses / "godot-cpp.LICENSE.md")

    archive = shutil.make_archive(str(out), "gztar", root_dir=out.parent, base_dir=name)
    size = Path(archive).stat().st_size / (1 << 20)
    print(f"Release: {out}\nArchive: {archive} ({size:.0f} MiB)", flush=True)


def main():
    parser = argparse.ArgumentParser(description="Jet Studio development commands")
    parser.add_argument("action", choices=["setup", "godot", "build", "run", "test", "firmware", "flash", "assets",
                                                "examples", "export", "release"])
    parser.add_argument("--update", action="store_true", help="Fetch latest Godot and godot-cpp master; record revisions")
    parser.add_argument("-j", "--jobs", type=int, default=min(os.cpu_count() or 4, 10))
    parser.add_argument("--pack", type=Path)
    parser.add_argument("--project", type=Path, help="export: project whose main scene goes to --pack; firmware/flash: "
                        "project whose hardware settings (board, display, pins) are used")
    parser.add_argument("--port")
    parser.add_argument("--headless", action="store_true")
    parser.add_argument("--screenshot", type=Path)
    args = parser.parse_args()
    if args.action == "setup": setup(args.update)
    elif args.action == "godot": build_godot(args.jobs)
    elif args.action == "build": build(args.jobs)
    elif args.action == "release": release(args.jobs)
    elif args.action == "assets": run([sys.executable, ROOT / "tools/assets.py"])
    elif args.action == "examples":
        run([sys.executable, ROOT / "tools/neon_car.py", "--import-car"])
        run([sys.executable, ROOT / "tools/physics_demo.py"])
    elif args.action in ("run", "test", "export"):
        prepare_desktop()
        clean_snap_environment()
        executable = Path(os.environ.get("JET_STUDIO_GODOT", GODOT))
        if not executable.is_file():
            raise RuntimeError("Godot source build missing. Run ./jet-studio godot")
        if args.action == "test": run(["ctest", "--test-dir", ROOT / "build", "--output-on-failure"])
        command = [executable, "--path", ROOT / "editor"]
        if args.headless: command.append("--headless")
        else: command += display_arguments()
        if args.action == "export":
            if not args.project or not args.pack:
                raise RuntimeError("Usage: ./jet-studio export --project path/to/project --pack out.jetpack")
            run([executable, "--path", ROOT / "editor", "--headless", "--", f"--project={args.project.resolve()}",
                 f"--export={args.pack.resolve()}"])
            return
        if args.action == "test" or args.screenshot:
            command.append("--")
            if args.action == "test": command.append("--smoke-test")
            if args.screenshot: command.append(f"--screenshot={args.screenshot.resolve()}")
        if args.action == "test":
            result = subprocess.run(list(map(str, command)), cwd=ROOT, text=True, capture_output=True, timeout=120)
            print(result.stdout, end="")
            print(result.stderr, end="", file=sys.stderr)
            if result.returncode or "PASS: Godot UI" not in result.stdout or "SCRIPT ERROR" in result.stderr:
                raise RuntimeError("Godot UI integration test failed")
        else:
            run(command)
    else:
        if not args.pack:
            raise RuntimeError("Specify --pack path/to/scene.jetpack exported by the editor")
        command = [sys.executable, ROOT / "tools/tasks.py", "flash" if args.action == "flash" else "build",
                   "--target", "device", "--pack", args.pack.resolve()]
        if args.port: command += ["--port", args.port]
        if args.project: command += ["--project", args.project.resolve()]  # its hardware and buttons
        run(command)


if __name__ == "__main__":
    try:
        main()
    except (RuntimeError, subprocess.CalledProcessError, subprocess.TimeoutExpired) as error:
        print(f"ERROR: {error}", file=sys.stderr)
        sys.exit(1)
