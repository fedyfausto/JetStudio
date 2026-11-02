#!/usr/bin/env python3
# Development commands behind ./jet-studio.
import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import struct
import subprocess
import sys
import tarfile
import urllib.request

ROOT = Path(__file__).resolve().parents[1]
GODOT = ROOT / "third_party/godot/bin/godot.linuxbsd.template_debug.x86_64"
RELEASE_GODOT = ROOT / "third_party/godot/bin/godot.linuxbsd.template_release.x86_64"
LOCK = ROOT / "dependencies.lock.json"
PATCHES = ROOT / "tools/patches"


# Terminals inside snap apps (VS Code, for one) export the snap's GLib/GTK paths.
# Under Wayland libdecor's GTK plugin then loads the wrong settings schema and aborts, so drop them.
SNAP_VARIABLES = ["GSETTINGS_SCHEMA_DIR", "GTK_PATH", "GTK_EXE_PREFIX", "GIO_MODULE_DIR", "GDK_PIXBUF_MODULE_FILE",
                  "GDK_PIXBUF_MODULEDIR", "GTK_IM_MODULE_FILE", "LOCPATH"]


def cleanSnapEnvironment():
    # Do not rely on $SNAP, some terminals keep the snap paths without it.
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


# Native Wayland keeps text sharp on HiDPI; under XWayland the compositor upscales a low-res window.
def displayArguments():
    choice = os.environ.get("JET_STUDIO_DISPLAY", "auto")

    if choice == "auto":
        choice = "wayland" if os.environ.get("WAYLAND_DISPLAY") else "x11"

    return ["--display-driver", choice]


def prepareDesktop():
    # Keep the dev editor's settings and logs in .cache/ instead of the user's home.
    for name, folder in [("XDG_DATA_HOME", "data"), ("XDG_CONFIG_HOME", "config"), ("XDG_CACHE_HOME", "cache")]:
        path = ROOT / ".cache" / folder
        path.mkdir(parents=True, exist_ok=True)
        os.environ[name] = str(path)

    cache = ROOT / "editor/.godot"
    cache.mkdir(exist_ok=True)
    classes = cache / "global_script_class_cache.cfg"

    if not classes.exists():
        classes.write_text("list=[]\n", encoding="utf-8")


def run(command, cwd=ROOT):
    print("+", " ".join(map(str, command)), flush=True)
    subprocess.run(list(map(str, command)), cwd=cwd, check=True)


# Our fixes to a dependency (tools/patches/<name>-*.patch), kept until upstream has them.
def patch(path, patches, reverse=False):
    for item in reversed(patches) if reverse else patches:
        command = ["git", "-C", str(path), "apply"] + (["-R"] if reverse else []) + [str(item)]
        undo = ["git", "-C", str(path), "apply"] + ([] if reverse else ["-R"]) + ["--check", str(item)]

        if subprocess.run(undo, capture_output=True).returncode == 0:
            #already applied (or already removed)
            continue

        run(command)


def setup(update=False):
    dependencies = json.loads(LOCK.read_text(encoding="utf-8"))

    for name, dependency in dependencies.items():
        if "sha256" in dependency:
            #an archive, downloaded by the build that needs it
            continue

        path = ROOT / ("firmware/components" if name == "LovyanGFX" else "third_party") / name
        patches = sorted(PATCHES.glob(f"{name}-*.patch"))

        if not (path / ".git").exists():
            if path.exists() and any(path.iterdir()):
                raise RuntimeError(f"{path} exists without a Git checkout; inspect it before updating")

            path.mkdir(parents=True, exist_ok=True)
            run(["git", "init", "-q", path])

        revision = "master" if update and name in ("godot", "godot-cpp") else dependency["revision"]
        current = subprocess.run(["git", "-C", str(path), "rev-parse", "HEAD"], capture_output=True, text=True)

        if not update and current.stdout.strip() == revision:
            patch(path, patches)
            continue

        if current.returncode == 0:
            #our patches are not local changes
            patch(path, patches, reverse=True)

        status = subprocess.check_output(["git", "-C", str(path), "status", "--porcelain"], text=True)

        if status.strip():
            raise RuntimeError(f"Local changes in {path}; refusing to overwrite them")

        run(["git", "-C", path, "fetch", "--depth", "1", dependency["url"], revision])
        run(["git", "-C", path, "checkout", "--detach", "FETCH_HEAD"])
        dependency["revision"] = subprocess.check_output(["git", "-C", str(path), "rev-parse", "HEAD"], text=True).strip()
        patch(path, patches)

    LOCK.write_text(json.dumps(dependencies, indent=2) + "\n", encoding="utf-8")


def buildGodot(jobs, target="template_debug", platform="linuxbsd"):
    if not (ROOT / "third_party/godot/SConstruct").exists():
        raise RuntimeError("Run ./jet-studio setup first")

    extra = ["use_mingw=yes", f"mingw_prefix={mingw()}", "d3d12=no", "angle=no"] if platform == "windows" else []
    run(["scons", f"platform={platform}", f"target={target}", "arch=x86_64", "dev_build=no",
         "debug_symbols=no", "optimize=speed", "disable_3d=yes", "disable_path_overrides=no",
         "vulkan=no", "opengl3=yes", "modules_enabled_by_default=no",
         "module_gdscript_enabled=yes", "module_freetype_enabled=yes",
         "module_text_server_fb_enabled=yes", *extra, f"-j{jobs}"], ROOT / "third_party/godot")


# A folder whose bin/ has the MinGW-w64 tools with the posix thread model, for Godot's mingw_prefix.
def mingw():
    if not Path("/usr/bin/x86_64-w64-mingw32-gcc").exists():
        raise RuntimeError("MinGW-w64 is missing: sudo apt install mingw-w64")

    folder = ROOT / ".tools/mingw"
    (folder / "bin").mkdir(parents=True, exist_ok=True)

    for tool in Path("/usr/bin").glob("x86_64-w64-mingw32-*"):
        if tool.name.endswith(("-posix", "-win32")):
            continue

        posix = tool.with_name(tool.name + "-posix")
        link = folder / "bin" / tool.name
        link.unlink(missing_ok=True)
        link.symlink_to(posix if posix.exists() else tool)

    return folder


# An archive from dependencies.lock.json, checked against its SHA-256 and unpacked into third_party/.
def download(name):
    dependency = json.loads(LOCK.read_text(encoding="utf-8"))[name]
    archive = ROOT / ".cache/downloads" / dependency["url"].rsplit("/", 1)[1]

    if not archive.exists():
        archive.parent.mkdir(parents=True, exist_ok=True)
        print(f"Downloading {dependency['url']}", flush=True)

        with urllib.request.urlopen(dependency["url"], timeout=60) as response, open(archive, "wb") as f:
            shutil.copyfileobj(response, f)

    if hashlib.sha256(archive.read_bytes()).hexdigest() != dependency["sha256"]:
        archive.unlink()
        raise RuntimeError(f"{archive.name}: checksum mismatch, download removed")

    target = ROOT / "third_party" / name

    if not target.exists():
        if not hasattr(tarfile, "data_filter"):
            raise RuntimeError("Python 3.12 or newer is needed to unpack the download safely")

        #unpacked beside it and renamed, so an interrupted run does not leave half a folder behind
        partial = target.with_name(target.name + ".partial")
        shutil.rmtree(partial, ignore_errors=True)

        with tarfile.open(archive) as tar:
            tar.extractall(partial, filter="data")

        partial.rename(target)

    folders = [path for path in target.iterdir() if path.is_dir()]

    if len(folders) != 1:
        raise RuntimeError(f"{target} should hold the archive's one folder: delete it and build again")

    return folders[0]


def build(jobs):
    run(["cmake", "-S", ROOT, "-B", ROOT / "build", "-G", "Ninja", "-DCMAKE_BUILD_TYPE=Release"])
    run(["cmake", "--build", ROOT / "build", "-j", jobs])


# The bridge DLL, the players and the tests for Windows in build-windows/; ctest runs them through Wine.
def buildWindows(jobs):
    sdl = download("SDL2-mingw") / "x86_64-w64-mingw32"
    os.environ["PATH"] = f"{mingw() / 'bin'}{os.pathsep}{os.environ['PATH']}"
    folder = ROOT / "build-windows"
    run(["cmake", "-S", ROOT, "-B", folder, "-G", "Ninja", "-DCMAKE_BUILD_TYPE=Release",
         f"-DCMAKE_TOOLCHAIN_FILE={ROOT / 'cmake/mingw-w64.cmake'}", f"-DSDL2_DIR={sdl / 'lib/cmake/SDL2'}"])
    run(["cmake", "--build", folder, "-j", jobs])
    shutil.copy2(sdl / "bin/SDL2.dll", folder / "SDL2.dll")
    return sdl


def version():
    for line in (ROOT / "editor/scripts/studio.gd").read_text(encoding="utf-8").splitlines():
        if line.startswith("const VERSION"):
            return line.split('"')[1]

    raise RuntimeError("VERSION not found in studio.gd")


def skipInRelease(folder, names):
    skipped = {".git", ".github", ".godot", ".pio", "__pycache__", ".jetstudio", "game.jetpack"}

    if Path(folder).name == "LovyanGFX":
        kept = {"src", "CMakeLists.txt", "boards.cmake", "component.mk", "library.json", "library.properties", "license.txt"}
        return [name for name in names if name not in kept]
    #sdkconfig.<board> files are generated per machine by ESP-IDF.
    return [name for name in names if name in skipped
            or (name.startswith("sdkconfig.") and not name.startswith("sdkconfig.defaults") and name != "sdkconfig.release")]


# Appends a .pck the way Godot's exporter embeds it: padding, package, its size and the GDPC magic.
def embedPack(executable, pck):
    data = pck.read_bytes()

    with open(executable, "r+b") as f:
        f.seek(0, os.SEEK_END)
        f.write(b"\0" * (-f.tell() % 8))
        start = f.tell()
        f.write(data)
        f.write(b"\0" * (-(f.tell() + 12) % 8))
        f.write(struct.pack("<Q", f.tell() - start) + b"GDPC")


# What both releases carry besides the binaries, laid out like the repo.
def copyReleaseFiles(out):
    # No neon_car: its car model and livery are not ours to redistribute.
    for path in ["LICENSE", "README.md", "docs", "hardware", "runtime", "cmake/JetSources.cmake", "firmware", "firmware_stm32",
                 "tools/tasks.py", "tools/hardware.py", "third_party/Jet/src", "third_party/Jet/LICENSE",
                 "examples/first_world", "examples/jet_blaster", "examples/jetrun", "examples/physics_demo",
                 "examples/textured_boxes"]:
        source, target = ROOT / path, out / path
        target.parent.mkdir(parents=True, exist_ok=True)

        if source.is_dir():
            shutil.copytree(source, target, ignore=skipInRelease)
        else:
            shutil.copy2(source, target)

    licenses = out / "licenses"
    licenses.mkdir()
    shutil.copy2(ROOT / "third_party/godot/LICENSE.txt", licenses / "Godot.LICENSE.txt")
    shutil.copy2(ROOT / "third_party/godot/COPYRIGHT.txt", licenses / "Godot.COPYRIGHT.txt")
    shutil.copy2(ROOT / "third_party/godot-cpp/LICENSE.md", licenses / "godot-cpp.LICENSE.md")


# The editor project packed and appended to a Godot template, so one file starts Jet Studio.
def packEditor(executable):
    prepareDesktop()
    packer = RELEASE_GODOT if RELEASE_GODOT.exists() else GODOT
    pck = executable.with_suffix(".pck")
    run([packer, "--headless", "--path", ROOT / "editor", "--script", ROOT / "tools/pack_editor.gd", "--", pck])
    embedPack(executable, pck)
    pck.unlink()


# Windows x86_64 folder and zip in dist/, cross-built with MinGW-w64. jet-studio.exe sits at the top.
def releaseWindows(jobs):
    buildGodot(jobs, "template_release", "windows")
    sdl = buildWindows(jobs)
    name = f"jet-studio-{version()}-windows-x86_64"
    out = ROOT / "dist" / name

    if out.exists():
        shutil.rmtree(out)

    out.mkdir(parents=True)
    copyReleaseFiles(out)
    shutil.copy2(sdl.parent / "LICENSE.txt", out / "licenses/SDL2.LICENSE.txt")

    strip = mingw() / "bin/x86_64-w64-mingw32-strip"
    executable = out / "jet-studio.exe"
    shutil.copy2(ROOT / "third_party/godot/bin/godot.windows.template_release.x86_64.exe", executable)
    run([strip, executable])
    packEditor(executable)
    #exported Godot projects load a GDExtension found next to the executable.
    shutil.copy2(ROOT / "editor/bin/jet_bridge.dll", out / "jet_bridge.dll")
    run([strip, "--strip-unneeded", out / "jet_bridge.dll"])
    (out / "build").mkdir()

    for player in ["jet_player.exe", "jet_player_device.exe"]:
        shutil.copy2(ROOT / "build-windows" / player, out / "build" / player)
        run([strip, out / "build" / player])

    shutil.copy2(sdl / "bin/SDL2.dll", out / "build/SDL2.dll")

    archive = shutil.make_archive(str(out), "zip", root_dir=out.parent, base_dir=name)
    size = Path(archive).stat().st_size / (1 << 20)
    print(f"Release: {out}\nArchive: {archive} ({size:.0f} MiB)", flush=True)


# Portable folder and tarball in dist/, laid out like the repo so the editor and tasks.py find everything.
def release(jobs):
    buildGodot(jobs, "template_release")
    build(jobs)
    run(["ctest", "--test-dir", ROOT / "build", "--output-on-failure"])
    name = f"jet-studio-{version()}-linux-x86_64"
    out = ROOT / "dist" / name

    if out.exists():
        shutil.rmtree(out)

    out.mkdir(parents=True)

    copyReleaseFiles(out)
    #the editor travels inside the executable; Godot finds the bridge library next to it.
    (out / "bin").mkdir()
    executable = out / "bin/jet-studio.x86_64"
    shutil.copy2(RELEASE_GODOT, executable)
    run(["strip", executable])
    packEditor(executable)
    shutil.copy2(ROOT / "editor/bin/libjet_bridge.so", out / "bin/libjet_bridge.so")
    run(["strip", "--strip-unneeded", out / "bin/libjet_bridge.so"])
    shutil.copy2(ROOT / "tools/launcher.sh", out / "jet-studio")
    (out / "build").mkdir()

    for player in ["jet_player", "jet_player_device"]:
        shutil.copy2(ROOT / "build" / player, out / "build" / player)
        run(["strip", out / "build" / player])

    archive = shutil.make_archive(str(out), "gztar", root_dir=out.parent, base_dir=name)
    size = Path(archive).stat().st_size / (1 << 20)
    print(f"Release: {out}\nArchive: {archive} ({size:.0f} MiB)", flush=True)


def main():
    parser = argparse.ArgumentParser(description="Jet Studio development commands")
    parser.add_argument("action", choices=["setup", "godot", "build", "run", "test", "firmware", "flash", "assets",
                                                "examples", "export", "release", "release-windows"])
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
    elif args.action == "godot": buildGodot(args.jobs)
    elif args.action == "build": build(args.jobs)
    elif args.action == "release": release(args.jobs)
    elif args.action == "release-windows": releaseWindows(args.jobs)
    elif args.action == "assets": run([sys.executable, ROOT / "tools/assets.py"])
    elif args.action == "examples":
        run([sys.executable, ROOT / "tools/neon_car.py", "--import-car"])
        run([sys.executable, ROOT / "tools/physics_demo.py"])
    elif args.action in ("run", "test", "export"):
        prepareDesktop()
        cleanSnapEnvironment()
        executable = Path(os.environ.get("JET_STUDIO_GODOT", GODOT))

        if not executable.is_file():
            raise RuntimeError("Godot source build missing. Run ./jet-studio godot")

        if args.action == "test": run(["ctest", "--test-dir", ROOT / "build", "--output-on-failure"])

        command = [executable, "--path", ROOT / "editor"]

        if args.headless: command.append("--headless")
        else: command += displayArguments()

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
        #its hardware and buttons
        if args.project: command += ["--project", args.project.resolve()]

        run(command)


if __name__ == "__main__":
    try:
        main()
    except (RuntimeError, OSError, subprocess.CalledProcessError, subprocess.TimeoutExpired) as error:
        print(f"ERROR: {error}", file=sys.stderr)
        sys.exit(1)
