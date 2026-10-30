#!/usr/bin/env python3
"""Bundle the runtime, compiled shaders, the engine's Lua library and one game
folder into a standalone distribution for one target OS.

The game and lualib ship as .pak archives (written by `fengine_assetc pack`):
game.pak and lualib.pak stand in for the folders game/ and lualib/. Files the
GPU loaders open directly stay loose in game/: shaders compiled from the
game's shaders/*.sc and cooked .ktx textures.

Layouts:
  Windows, Linux   bin/<runtime>, bin/shaders, bin/lualib.pak, game.pak, game/,
                   play.cmd or play
  macOS            <Title>.app (Contents/MacOS/fengine; shaders, lualib.pak,
                   game.pak and game/ in Contents/Resources)
Every layout also holds licenses/, README.txt and manifest.json (SHA-256 of
every file and of every pak entry), which is checked against the output and
the ZIP before finishing.
"""
import argparse
import hashlib
import json
import os
import plistlib
import re
import shlex
import shutil
import struct
import subprocess
import sys
import tempfile
import zipfile
import zlib
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
# Every engine shader the build compiles (shaders/vs_*.sc and fs_*.sc).
REQUIRED_SHADERS = {path.stem for pattern in ("vs_*.sc", "fs_*.sc") for path in (ROOT / "shaders").glob(pattern)}
SKIPPED_SUFFIXES = {".tmp", ".bak"}
LOOSE_SUFFIXES = {".ktx"}
MACHO_MAGIC = {b"\xcf\xfa\xed\xfe", b"\xce\xfa\xed\xfe", b"\xca\xfe\xba\xbe", b"\xbe\xba\xfe\xca"}
# shaderc --platform and --profile for each backend folder.
SHADER_TARGETS = {"metal": ("osx", "metal"), "dx11": ("windows", "s_5_0"), "spirv": ("linux", "spirv"),
                  "glsl": ("linux", "330")}


def copy_tree(source, destination, skip_parts=(), skip_top=(), skip_suffixes=()):
    """Copy files, skipping hidden files, editor leftovers, `skip_parts` folders
    anywhere, `skip_top` entries at the top level and `skip_suffixes` files."""
    for path in sorted(source.rglob("*")):
        relative = path.relative_to(source)
        if (any(part.startswith(".") or part in skip_parts for part in relative.parts) or
                relative.parts[0] in skip_top or path.suffix in SKIPPED_SUFFIXES or path.suffix in skip_suffixes):
            continue
        if path.is_symlink() and not path.resolve().is_relative_to(source):
            raise ValueError(f"Symlink escapes {source.name}: {relative}")
        if path.is_file():
            target = destination / relative
            target.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(path, target)


def check_shaders(shaders):
    backends = [folder for folder in shaders.iterdir() if folder.is_dir()]
    if not backends:
        raise ValueError("No compiled backend shaders were found")
    for backend in backends:
        missing = REQUIRED_SHADERS - {file.stem for file in backend.glob("*.bin")}
        if missing:
            raise ValueError(f"Incomplete {backend.name} shaders: {', '.join(sorted(missing))}")
    return sorted(backend.name for backend in backends)


def read_pak(path):
    """Every entry of a .pak archive (see engine/core/Pack.h), checked like the engine does."""
    data = path.read_bytes()
    if len(data) < 32 or data[:4] != b"FPAK":
        raise ValueError(f"{path} is not a pak archive")
    version, index_offset, index_size, count, index_crc = struct.unpack_from("<IQQII", data, 4)
    index = data[index_offset:index_offset + index_size]
    if version != 1 or len(index) != index_size or zlib.crc32(index) != index_crc:
        raise ValueError(f"{path} has a damaged index")
    entries, at, previous = {}, 0, ""
    for _ in range(count):
        (length,) = struct.unpack_from("<H", index, at)
        name = index[at + 2:at + 2 + length].decode("utf-8")
        at += 2 + length
        method, offset, stored_size, size, crc = struct.unpack_from("<BQQQI", index, at)
        at += 29
        parts = name.split("/")
        if (name <= previous or name.startswith("/") or "\\" in name or ":" in name or
                any(part in ("", ".", "..") for part in parts)):
            raise ValueError(f"{path}: bad entry name {name!r}")
        stored = data[offset:offset + stored_size]
        content = zlib.decompress(stored) if method == 1 else stored
        if method not in (0, 1) or len(content) != size or zlib.crc32(content) != crc:
            raise ValueError(f"{path}: damaged entry {name}")
        entries[name] = content
        previous = name
    if at != len(index):
        raise ValueError(f"{path}: trailing index bytes")
    return entries


def make_pak(assetc, folder, archive):
    """Pack `folder` and check the archive holds exactly its files."""
    subprocess.run([str(assetc), "pack", str(folder), str(archive)], check=True, stdout=subprocess.DEVNULL)
    expected = {file.relative_to(folder).as_posix(): file.read_bytes()
                for file in folder.rglob("*") if file.is_file()}
    if read_pak(archive) != expected:
        raise ValueError(f"{archive.name} does not hold exactly the files of {folder.name}")


def compile_game_shaders(game, backends, args, output):
    """Compile <game>/shaders/{vs,fs}_*.sc into output/<backend>/<name>.bin;
    without sources, copy precompiled <game>/shaders/<backend>/*.bin."""
    folder = game / "shaders"
    sources = sorted(folder.glob("vs_*.sc")) + sorted(folder.glob("fs_*.sc"))
    if not sources:
        for backend in backends:
            for binary in sorted((folder / backend).glob("*.bin")):
                (output / backend).mkdir(parents=True, exist_ok=True)
                shutil.copy2(binary, output / backend / binary.name)
        return
    if not args.shaderc:
        raise ValueError(f"{folder} has shader sources; pass --shaderc to compile them")
    varying = folder / "varying.def.sc"
    if not varying.is_file():
        varying = ROOT / "shaders" / "varying.def.sc"
    includes = [folder, ROOT / "shaders"] + [path.resolve(strict=True) for path in args.shader_include]
    for backend in backends:
        if backend not in SHADER_TARGETS:
            raise ValueError(f"No shaderc settings for the {backend} backend")
        platform, profile = SHADER_TARGETS[backend]
        (output / backend).mkdir(parents=True, exist_ok=True)
        for source in sources:
            command = [str(args.shaderc.resolve(strict=True)), "-f", str(source),
                       "-o", str(output / backend / (source.stem + ".bin")),
                       "--type", "vertex" if source.name.startswith("vs_") else "fragment",
                       "--platform", platform, "--profile", profile, "--varyingdef", str(varying)]
            for include in includes:
                command += ["-i", str(include)]
            subprocess.run(command, check=True)


def cook_textures(assetc, folder, loose):
    """Run `fengine_assetc cook` over the game and move the cooked .ktx files to `loose`."""
    usage = subprocess.run([str(assetc)], capture_output=True, encoding="utf-8").stdout
    if "fengine_assetc cook" not in usage:
        raise ValueError(f"{assetc.name} has no cook command")
    subprocess.run([str(assetc), "cook", str(folder)], check=True)
    for cooked in sorted(folder.rglob("*.ktx")):
        target = loose / cooked.relative_to(folder)
        target.parent.mkdir(parents=True, exist_ok=True)
        shutil.move(str(cooked), target)


def detect_platform(runtime):
    if runtime.suffix.lower() == ".exe":
        return "windows"
    with runtime.open("rb") as file:
        magic = file.read(4)
    if magic in MACHO_MAGIC:
        return "macos"
    if magic == b"\x7fELF":
        return "linux"
    raise ValueError(f"Unrecognised runtime executable format: {runtime}")


def host_platform():
    return {"darwin": "macos", "win32": "windows"}.get(sys.platform, "linux")


def write_launcher(stage, runtime, platform):
    if platform == "windows":
        # `start` returns at once, so no console window stays open.
        (stage / "play.cmd").write_text(f'@start "" "%~dp0bin\\{runtime.name}" "%~dp0game" %*\r\n',
                                        encoding="utf-8", newline="")
    else:
        launcher = stage / "play"
        launcher.write_text('#!/bin/sh\nengine_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)\n'
                            f'exec "$engine_dir/bin/{runtime.name}" "$engine_dir/game" "$@"\n',
                            encoding="utf-8", newline="")
        launcher.chmod(0o755)


def game_id(config):
    """game.json's stable id (see docs/authoring.md), derived from a plain title as the engine does."""
    explicit = config.get("id")
    if explicit is not None:
        if not isinstance(explicit, str) or not re.fullmatch(r"[a-z0-9_-]{1,64}", explicit):
            raise ValueError(f"game.json id must be 1 to 64 of a-z, 0-9, '_' and '-': {explicit!r}")
        return explicit
    title = config.get("title", "f-engine game")
    identifier = game_id(config)
    if re.search(r"[^A-Za-z0-9_ -]", title):
        raise ValueError(f"The title {title!r} has characters outside a-z, 0-9, '_', '-' and spaces: "
                         "game.json needs an \"id\"")
    derived = re.sub(r" +", "-", title.lower().strip()).strip("-")
    if not re.fullmatch(r"[a-z0-9_-]{1,64}", derived):
        raise ValueError(f"No game id can be derived from the title {title!r}: game.json needs an \"id\"")
    return derived


def bundle_identifier(slug):
    """A reverse-DNS identifier from the game's id."""
    slug = re.sub(r"[^a-z0-9-]+", "-", slug).strip("-") or "game"
    return f"org.fengine.{slug}"


def make_app(stage, title, identifier, version, runtime):
    """A macOS application bundle; the runtime plays Contents/Resources/game."""
    app = stage / f"{title}.app"
    contents = app / "Contents"
    (contents / "MacOS").mkdir(parents=True)
    resources = contents / "Resources"
    resources.mkdir()
    shutil.copy2(runtime, contents / "MacOS" / "fengine")
    info = {"CFBundleName": title, "CFBundleDisplayName": title, "CFBundleIdentifier": bundle_identifier(identifier),
            "CFBundleExecutable": "fengine", "CFBundlePackageType": "APPL", "CFBundleVersion": version,
            "CFBundleShortVersionString": version, "LSMinimumSystemVersion": "11.0",
            "NSHighResolutionCapable": True}
    with (contents / "Info.plist").open("wb") as file:
        plistlib.dump(info, file)
    return resources


def sign_app(app, identity, notarize_profile):
    """Sign the bundle: with a Developer ID (hardened runtime, timestamped) and
    optionally notarise and staple it, or ad hoc so it launches after being
    moved or unzipped."""
    if identity:
        subprocess.run(["codesign", "--force", "--deep", "--options", "runtime", "--timestamp",
                        "--sign", identity, str(app)], check=True)
    elif sys.platform == "darwin" and shutil.which("codesign"):
        subprocess.run(["codesign", "--force", "--deep", "--sign", "-", str(app)], check=True)
    if notarize_profile:
        with tempfile.TemporaryDirectory(prefix="fengine-notarize-") as folder:
            upload = Path(folder) / (app.stem + ".zip")
            subprocess.run(["ditto", "-c", "-k", "--keepParent", str(app), str(upload)], check=True)
            subprocess.run(["xcrun", "notarytool", "submit", str(upload), "--keychain-profile", notarize_profile,
                            "--wait"], check=True)
        subprocess.run(["xcrun", "stapler", "staple", str(app)], check=True)


def sign_windows(executable_dir, signtool):
    """Run the --signtool command on every executable and DLL."""
    files = sorted(path for path in executable_dir.iterdir() if path.suffix.lower() in (".exe", ".dll"))
    subprocess.run(shlex.split(signtool, posix=os.name != "nt") + [str(path) for path in files], check=True)


def png(width, height, rgb):
    """A solid-colour PNG (the AppImage icon)."""
    def chunk(kind, payload):
        return struct.pack(">I", len(payload)) + kind + payload + struct.pack(">I", zlib.crc32(kind + payload))
    rows = b"".join(b"\0" + bytes(rgb) * width for _ in range(height))
    return (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 2, 0, 0, 0)) +
            chunk(b"IDAT", zlib.compress(rows)) + chunk(b"IEND", b""))


def make_appimage(stage, title, identifier, runtime, appimagetool, work):
    """<id>-x86_64.AppImage from the bin/ layout: AppDir/usr holds bin/ and the game."""
    appdir = work / "AppDir"
    usr = appdir / "usr"
    shutil.copytree(stage / "bin", usr / "bin")
    shutil.copy2(stage / "game.pak", usr / "game.pak")
    if (stage / "game").is_dir():
        shutil.copytree(stage / "game", usr / "game")
    slug = bundle_identifier(identifier).rsplit(".", 1)[1]
    app_run = appdir / "AppRun"
    app_run.write_text(f'#!/bin/sh\nexec "$(dirname "$(readlink -f "$0")")/usr/bin/{runtime.name}" "$@"\n',
                       encoding="utf-8", newline="")
    app_run.chmod(0o755)
    (appdir / f"{slug}.desktop").write_text(f"[Desktop Entry]\nType=Application\nName={title}\nExec={runtime.name}\n"
                                            f"Icon={slug}\nCategories=Game;\n", encoding="utf-8", newline="")
    (appdir / f"{slug}.png").write_bytes(png(256, 256, (40, 48, 72)))
    image = stage / f"{slug}-x86_64.AppImage"
    subprocess.run([str(appimagetool), str(appdir), str(image)], check=True, env={**os.environ, "ARCH": "x86_64"})


def file_hashes(folder):
    return {file.relative_to(folder).as_posix(): hashlib.sha256(file.read_bytes()).hexdigest()
            for file in sorted(folder.rglob("*")) if file.is_file() and file.name != "manifest.json"}


def pak_hashes(folder):
    return {pak.relative_to(folder).as_posix(): {name: hashlib.sha256(content).hexdigest()
                                                  for name, content in read_pak(pak).items()}
            for pak in sorted(folder.rglob("*.pak"))}


def verify(output, archive=None):
    """Check that the folder (and the ZIP) hold exactly the files the manifest
    lists, and every pak exactly the entries it lists."""
    manifest = json.loads((output / "manifest.json").read_text(encoding="utf-8"))
    if file_hashes(output) != manifest["files"]:
        raise ValueError(f"{output} does not match its manifest")
    if pak_hashes(output) != manifest["paks"]:
        raise ValueError(f"{output}: pak contents do not match the manifest")
    if archive:
        with zipfile.ZipFile(archive) as bundle:
            prefix = output.name + "/"
            names = {name[len(prefix):]: name for name in bundle.namelist()
                     if name.startswith(prefix) and not name.endswith("/")}
            names.pop("manifest.json", None)
            if set(names) != set(manifest["files"]):
                raise ValueError(f"{archive} file list does not match the manifest")
            for relative, name in names.items():
                if hashlib.sha256(bundle.read(name)).hexdigest() != manifest["files"][relative]:
                    raise ValueError(f"{archive}: {relative} does not match the manifest")


def self_check(output, title, runtime, platform):
    """Run the packaged runtime's --check on the packaged game."""
    if platform != host_platform():
        raise ValueError(f"--self-check needs a {platform} host")
    if platform == "macos":
        resources = output / f"{title}.app" / "Contents" / "Resources"
        command = [output / f"{title}.app" / "Contents" / "MacOS" / "fengine", "--check", resources / "game"]
    else:
        command = [output / "bin" / runtime.name, "--check", output / "game"]
    subprocess.run([str(part) for part in command], check=True)


def package(args):
    game = args.game.resolve(strict=True)
    for required in ("game.json", "main.lua"):
        if not (game / required).is_file():
            raise ValueError(f"Missing game file: {required}")
    config = json.loads((game / "game.json").read_text(encoding="utf-8"))
    title = config.get("title", "f-engine game")
    identifier = game_id(config)
    if args.validator:
        subprocess.run([str(args.validator.resolve()), "--check", str(game)], check=True)
    runtime = args.runtime.resolve(strict=True)
    assetc = args.assetc.resolve(strict=True)
    platform = args.platform if args.platform != "auto" else detect_platform(runtime)
    shaders = args.shaders.resolve(strict=True)
    lualib = args.lualib.resolve(strict=True)
    if not (lualib / "fe" / "init.lua").is_file():
        raise ValueError(f"Not an engine Lua library: {lualib}")
    backends = check_shaders(shaders)
    if (args.sign_identity and platform != "macos") or (args.notarize_profile and not args.sign_identity):
        raise ValueError("--sign-identity is for macOS packages; --notarize-profile needs it")
    if args.signtool and platform != "windows":
        raise ValueError("--signtool is for Windows packages")
    appimagetool = None
    if args.appimage:
        appimagetool = shutil.which(str(args.appimagetool))
        if platform != "linux" or not appimagetool:
            raise ValueError("--appimage needs a Linux package and appimagetool")
    output = args.output.resolve()
    if output.is_relative_to(game):
        raise ValueError("The distribution directory must be outside the source game folder")
    if output.exists():
        raise ValueError(f"Output already exists: {output}; choose a fresh directory")
    output.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="fengine-package-", dir=output.parent) as temporary:
        work = Path(temporary)
        sources, loose = work / "sources", work / "loose"
        # Tests and replays are development material; saves are per-user;
        # shader sources are compiled below.
        copy_tree(game, sources / "game", skip_parts=("saves", "tests", "replays"), skip_top=("shaders",),
                  skip_suffixes=LOOSE_SUFFIXES)
        copy_tree(lualib, sources / "lualib", skip_parts=("tests",))
        compile_game_shaders(game, backends, args, loose / "shaders")
        if args.cook:
            cook_textures(assetc, sources / "game", loose)
        stage = work / "bundle"
        stage.mkdir()
        if platform == "macos":
            if "/" in title or title.startswith("."):
                raise ValueError(f"The game title can't name an app bundle: {title!r}")
            engine = game_dir = make_app(stage, title, identifier, str(config.get("version", "1.0")), runtime)
            executable_dir = engine.parent / "MacOS"
        else:
            engine = executable_dir = stage / "bin"
            engine.mkdir()
            game_dir = stage
            shutil.copy2(runtime, engine / runtime.name)
        for dependency in args.runtime_dependency:
            source = dependency.resolve(strict=True)
            if source.name == runtime.name or (executable_dir / source.name).exists():
                raise ValueError(f"Duplicate runtime dependency: {source.name}")
            shutil.copy2(source, executable_dir / source.name)
        shutil.copytree(shaders, engine / "shaders")
        make_pak(assetc, sources / "lualib", engine / "lualib.pak")
        make_pak(assetc, sources / "game", game_dir / "game.pak")
        if loose.is_dir():
            shutil.copytree(loose, game_dir / "game")
        if args.licenses:
            shutil.copytree(args.licenses.resolve(strict=True), stage / "licenses")
        if platform == "macos":
            sign_app(stage / f"{title}.app", args.sign_identity, args.notarize_profile)
            how = f"Open {title}.app."
        else:
            if args.signtool:
                sign_windows(executable_dir, args.signtool)
            write_launcher(stage, runtime, platform)
            how = "Run play.cmd." if platform == "windows" else "Run ./play."
        if appimagetool:
            make_appimage(stage, title, identifier, runtime, appimagetool, work)
        (stage / "README.txt").write_text(f"{title}\n\n{how}\n"
                                          "Saves and settings use the platform user-data directory.\n",
                                          encoding="utf-8")
        manifest = {"title": title, "platform": platform, "files": file_hashes(stage), "paks": pak_hashes(stage)}
        (stage / "manifest.json").write_text(json.dumps(manifest, indent=2) + "\n", encoding="utf-8")
        shutil.move(str(stage), output)
    archive = None
    if args.zip:
        archive = Path(shutil.make_archive(str(output), "zip", output.parent, output.name))
    verify(output, archive)
    if args.self_check:
        self_check(output, title, runtime, platform)
    print(output)


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    for name in ("runtime", "shaders", "game", "output"):
        parser.add_argument("--" + name, type=Path, required=True)
    parser.add_argument("--assetc", type=Path, required=True, help="fengine_assetc, which writes the .pak archives")
    parser.add_argument("--platform", choices=("auto", "windows", "macos", "linux"), default="auto",
                        help="Target layout (default: from the runtime executable's format)")
    parser.add_argument("--lualib", type=Path, default=ROOT / "lualib", help="Engine Lua library (default: the repo's)")
    parser.add_argument("--validator", type=Path, help="fengine_headless, to run --check on the game first")
    parser.add_argument("--licenses", type=Path)
    parser.add_argument("--runtime-dependency", type=Path, action="append", default=[],
                        help="Additional runtime DLL/library to place beside the executable (repeatable)")
    parser.add_argument("--shaderc", type=Path, help="bgfx shaderc, for games with shaders/*.sc")
    parser.add_argument("--shader-include", type=Path, action="append", default=[],
                        help="Extra include folder for game shaders, e.g. <build>/shader-include (repeatable)")
    parser.add_argument("--cook", action="store_true", help="Cook textures with `fengine_assetc cook`")
    parser.add_argument("--sign-identity", help="macOS: codesign identity (Developer ID Application: ...)")
    parser.add_argument("--notarize-profile", help="macOS: notarytool keychain profile; notarises and staples")
    parser.add_argument("--signtool", help="Windows: signing command run with every .exe and .dll appended, "
                                           "e.g. \"signtool sign /fd SHA256 /a /tr <url> /td SHA256\"")
    parser.add_argument("--appimage", action="store_true", help="Linux: also build an AppImage")
    parser.add_argument("--appimagetool", type=Path, default=Path("appimagetool"))
    parser.add_argument("--self-check", action="store_true",
                        help="Run the packaged runtime's --check on the packaged game (same-OS packages)")
    parser.add_argument("--zip", action="store_true")
    args = parser.parse_args()
    try:
        package(args)
    except (OSError, ValueError, subprocess.CalledProcessError, zipfile.BadZipFile, zlib.error) as error:
        parser.exit(1, f"Packaging failed: {error}\n")


if __name__ == "__main__":
    main()
