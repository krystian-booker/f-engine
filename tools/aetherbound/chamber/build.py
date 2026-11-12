#!/usr/bin/env python3
"""Builds the chamber and the ruins of Aetherbound: Prologue.

    /tmp/claude-0/bl/venv/bin/python tools/aetherbound/chamber/build.py [chamber] [ruins] [--fast] [--fx-only]
                                                                    [--json-only] [--keep-textures]

Writes samples/aetherbound/maps/{chamber,ruins}/{geometry.glb,map.json,grade.cube}
and the particle sprites in samples/aetherbound/fx/chamber/. Everything is
procedural with fixed seeds, so a rebuild gives the same maps. --fast skips the
ambient-occlusion bake (for layout work), --json-only rewrites map.json
without touching geometry.glb, --keep-textures keeps the painted PNGs (they
are embedded in the .glb) in a temporary folder to look at.

Modules: layout (where things stand), kit (mesh patches on trim sheets),
paint/textures/floors (pixel-art textures), scene (geometry), blend (Blender:
materials, AO bake into vertex colours, glTF export), mapjson (grid, points,
lights, emitters, environment), fxsprites (particle sprites), shots (test
renders in the engine).
"""
import os
import shutil
import sys
import tempfile
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
ROOT = os.path.dirname(os.path.dirname(os.path.dirname(HERE)))
GAME = os.path.join(ROOT, "samples", "aetherbound")
SCRATCH = "/tmp/claude-0/-home-user-f-engine/c427e0fc-76b4-5426-b107-9b52cd2adb5c/scratchpad/chamber"


def main():
    args = sys.argv[1:]
    fast = "--fast" in args
    names = [a for a in args if not a.startswith("--")] or ["chamber", "ruins"]
    import fxsprites
    fxsprites.write_fx(os.path.join(GAME, "fx", "chamber"))
    if "--fx-only" in args:
        return
    import textures, floors, scene, blend, mapjson
    for name in names:
        started = time.time()
        ruined = name == "ruins"
        folder = os.path.join(GAME, "maps", name)
        os.makedirs(folder, exist_ok=True)
        work = tempfile.mkdtemp(prefix=f"aether-{name}-", dir=SCRATCH if os.path.isdir(SCRATCH) else None)
        textures.write_trims(work)
        floors.write_floor(work, ruined)
        builder = scene.build(ruined)
        if "--json-only" not in args:
            blend.export(builder.m, work, os.path.join(folder, "geometry.glb"), ao=not fast,
                         emissive=scene.EMISSIVE[name])
        document, walk = mapjson.write_map(folder, ruined, builder)
        if "--keep-textures" in args:
            print("textures kept in", work)
        else:
            shutil.rmtree(work, ignore_errors=True)
        size = sum(os.path.getsize(os.path.join(folder, f)) for f in os.listdir(folder)) / 1e6
        print(f"{name}: {builder.m.triangles()} triangles, {len(document['lights'])} lights, "
              f"{len(document['emitters'])} emitters, {size:.1f} MB, {time.time() - started:.0f} s")
        for row in walk:
            print("   " + "".join(".#"[not w] for w in row))


if __name__ == "__main__":
    main()
