#!/usr/bin/env python3
"""Bundle the runtime, compiled shaders, the engine's Lua library and one game
folder into a standalone distribution for one target OS."""
import argparse
import hashlib
import json
import shutil
import subprocess
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
REQUIRED_SHADERS = {"vs_world", "fs_world", "fs_sprite", "fs_water", "vs_shadow", "fs_shadow", "vs_sky", "fs_sky",
                    "fs_overlay", "vs_screen", "fs_post", "fs_bloom", "vs_ui", "fs_ui"}
SKIPPED_SUFFIXES = {".tmp", ".bak"}


def copy_tree(source, destination, skip_parts=()):
    """Copy files, skipping hidden files, editor leftovers and `skip_parts` folders."""
    for path in source.rglob("*"):
        relative = path.relative_to(source)
        if any(part.startswith(".") or part in skip_parts for part in relative.parts) or path.suffix in SKIPPED_SUFFIXES:
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


def write_launchers(stage, runtime):
    if runtime.suffix.lower() == ".exe":
        (stage / "play.cmd").write_text(f'@echo off\r\n"%~dp0bin\\{runtime.name}" "%~dp0game" %*\r\n')
        return
    launcher = stage / "play"
    launcher.write_text('#!/bin/sh\nengine_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)\n'
                        f'exec "$engine_dir/bin/{runtime.name}" "$engine_dir/game" "$@"\n')
    launcher.chmod(0o755)


def package(args):
    game = args.game.resolve(strict=True)
    for required in ("game.json", "main.lua"):
        if not (game / required).is_file():
            raise ValueError(f"Missing game file: {required}")
    config = json.loads((game / "game.json").read_text())
    if args.validator:
        subprocess.run([str(args.validator.resolve()), "--check", str(game)], check=True)
    runtime = args.runtime.resolve(strict=True)
    shaders = args.shaders.resolve(strict=True)
    lualib = args.lualib.resolve(strict=True)
    if not (lualib / "fe" / "init.lua").is_file():
        raise ValueError(f"Not an engine Lua library: {lualib}")
    check_shaders(shaders)
    output = args.output.resolve()
    if output.is_relative_to(game):
        raise ValueError("The distribution directory must be outside the source game folder")
    if output.exists():
        raise ValueError(f"Output already exists: {output}; choose a fresh directory")
    output.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="fengine-package-", dir=output.parent) as temporary:
        stage = Path(temporary) / "bundle"
        (stage / "bin").mkdir(parents=True)
        shutil.copy2(runtime, stage / "bin" / runtime.name)
        for dependency in args.runtime_dependency:
            source = dependency.resolve(strict=True)
            if source.name == runtime.name or (stage / "bin" / source.name).exists():
                raise ValueError(f"Duplicate runtime dependency: {source.name}")
            shutil.copy2(source, stage / "bin" / source.name)
        shutil.copytree(shaders, stage / "bin" / "shaders")
        copy_tree(lualib, stage / "bin" / "lualib", skip_parts=("tests",))
        # Tests and replays are development material; saves are per-user.
        copy_tree(game, stage / "game", skip_parts=("saves", "tests", "replays"))
        if args.licenses:
            shutil.copytree(args.licenses.resolve(strict=True), stage / "licenses")
        write_launchers(stage, runtime)
        (stage / "README.txt").write_text(f'{config.get("title", "f-engine game")}\n\n'
                                          'Run play (macOS/Linux) or play.cmd (Windows).\n'
                                          'Saves and settings use the platform user-data directory.\n')
        manifest = {"title": config.get("title"), "files": {}}
        for file in sorted(stage.rglob("*")):
            if file.is_file():
                manifest["files"][file.relative_to(stage).as_posix()] = hashlib.sha256(file.read_bytes()).hexdigest()
        (stage / "manifest.json").write_text(json.dumps(manifest, indent=2) + "\n")
        shutil.move(str(stage), output)
    if args.zip:
        shutil.make_archive(str(output), "zip", output.parent, output.name)
    print(output)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("runtime", "shaders", "game", "output"):
        parser.add_argument("--" + name, type=Path, required=True)
    parser.add_argument("--lualib", type=Path, default=ROOT / "lualib", help="Engine Lua library (default: the repo's)")
    parser.add_argument("--validator", type=Path, help="fengine_headless, to run --check on the game first")
    parser.add_argument("--licenses", type=Path)
    parser.add_argument("--runtime-dependency", type=Path, action="append", default=[],
                        help="Additional runtime DLL/library to place beside the executable (repeatable)")
    parser.add_argument("--zip", action="store_true")
    args = parser.parse_args()
    try:
        package(args)
    except (OSError, ValueError, subprocess.CalledProcessError) as error:
        parser.exit(1, f"Packaging failed: {error}\n")


if __name__ == "__main__":
    main()
