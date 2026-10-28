#!/usr/bin/env python3
"""Bundle a runtime, compiled shaders and a data-only game for one target OS."""
import argparse
import hashlib
import json
from pathlib import Path
import shutil
import subprocess
import tempfile


def package(args):
    game=args.game.resolve(strict=True)
    config=json.loads((game/"game.json").read_text())
    for required in ("database.json","initial_state.json","timelines.json","scripts/rules.lua"):
        if not (game/required).is_file():
            raise ValueError(f"Missing game file: {required}")
    start=game/"maps"/config["start_map"]/"map.json"
    if not start.is_file() or not start.resolve().is_relative_to(game):
        raise ValueError("Invalid or missing start map")
    if args.validator:
        subprocess.run([str(args.validator.resolve()),"validate",str(game)],check=True)
    runtime=args.runtime.resolve(strict=True)
    shaders=args.shaders.resolve(strict=True)
    if not list(shaders.glob("*/*.bin")):
        raise ValueError("No compiled backend shaders were found")
    required_shaders={"vs_world","fs_world","vs_shadow","fs_shadow","vs_screen","fs_post","fs_bloom","vs_ui","fs_ui"}
    for backend in shaders.iterdir():
        if backend.is_dir():
            missing=required_shaders-{file.stem for file in backend.glob("*.bin")}
            if missing:raise ValueError(f"Incomplete {backend.name} shaders: {', '.join(sorted(missing))}")
    output=args.output.resolve()
    if output.is_relative_to(game):
        raise ValueError("The distribution directory must be outside the source game folder")
    if output.exists():
        raise ValueError(f"Output already exists: {output}; choose a fresh directory")
    output.parent.mkdir(parents=True,exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="fengine-package-",dir=output.parent) as temporary:
        stage=Path(temporary)/"bundle"
        (stage/"bin").mkdir(parents=True)
        shutil.copy2(runtime,stage/"bin"/runtime.name)
        for dependency in args.runtime_dependency:
            source=dependency.resolve(strict=True)
            if source.name==runtime.name or (stage/"bin"/source.name).exists():
                raise ValueError(f"Duplicate runtime dependency: {source.name}")
            shutil.copy2(source,stage/"bin"/source.name)
        shutil.copytree(shaders,stage/"bin/shaders")
        for source in game.rglob("*"):
            relative=source.relative_to(game)
            if any(part.startswith(".") or part=="saves" for part in relative.parts) or source.suffix in (".tmp",".bak"):
                continue
            if source.is_symlink() and not source.resolve().is_relative_to(game):
                raise ValueError(f"Game symlink escapes the data folder: {relative}")
            if source.is_file():
                destination=stage/"game"/relative
                destination.parent.mkdir(parents=True,exist_ok=True)
                shutil.copy2(source,destination)
        if args.licenses:
            shutil.copytree(args.licenses.resolve(strict=True),stage/"licenses")
        if runtime.suffix.lower()==".exe":
            (stage/"play.cmd").write_text(f'@echo off\r\n"%~dp0bin\\{runtime.name}" "%~dp0game" %*\r\n')
        else:
            launcher=stage/"play"
            launcher.write_text('#!/bin/sh\nengine_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)\n'
                                f'exec "$engine_dir/bin/{runtime.name}" "$engine_dir/game" "$@"\n')
            launcher.chmod(0o755)
        (stage/"README.txt").write_text(f'{config.get("title","f-engine game")}\n\n'
            'Run play (macOS/Linux) or play.cmd (Windows).\n'
            'Arrow keys/WASD: move; Z/Enter: confirm; X/Escape: cancel; Tab: menu.\n'
            'Saves and settings use the platform user-data directory.\n')
        manifest={"title":config.get("title"),"files":{}}
        for file in sorted(stage.rglob("*")):
            if file.is_file():manifest["files"][file.relative_to(stage).as_posix()]=hashlib.sha256(file.read_bytes()).hexdigest()
        (stage/"manifest.json").write_text(json.dumps(manifest,indent=2)+'\n')
        shutil.move(str(stage),output)
    if args.zip:
        shutil.make_archive(str(output),"zip",output.parent,output.name)
    print(output)


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    for name in ("runtime","shaders","game","output"):
        parser.add_argument("--"+name,type=Path,required=True)
    parser.add_argument("--validator",type=Path)
    parser.add_argument("--licenses",type=Path)
    parser.add_argument("--runtime-dependency",type=Path,action="append",default=[],help="Additional runtime DLL/library to place beside the executable (repeatable)")
    parser.add_argument("--zip",action="store_true")
    args=parser.parse_args()
    try:package(args)
    except (OSError,ValueError,subprocess.CalledProcessError) as error:parser.exit(1,f"Packaging failed: {error}\n")


if __name__=="__main__":main()
