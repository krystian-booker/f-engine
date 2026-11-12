#!/usr/bin/env python3
"""Builds the fortress map of Aetherbound: Prologue.

    /tmp/claude-0/bl/venv/bin/python tools/aetherbound/fortress/build.py [--no-ao] [--no-check]

Writes samples/aetherbound/maps/fortress/ (geometry.glb, map.json,
textures/, sky cards, grade.cube) and samples/aetherbound/fx/fortress/
(particle sprite sheets). Everything is generated; nothing is downloaded.
"""
import json
import os
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
ROOT = os.path.dirname(os.path.dirname(os.path.dirname(HERE)))
GAME = os.path.join(ROOT, "samples", "aetherbound")
OUT = os.path.join(GAME, "maps", "fortress")
FX = os.path.join(GAME, "fx", "fortress")

import numpy as np  # noqa: E402
import textures  # noqa: E402
import fx  # noqa: E402
import layout  # noqa: E402
import blender_io  # noqa: E402
import verify  # noqa: E402


def grid_json(S):
    W, D = layout.W, layout.D
    height = [" ".join(f"{S.height[z, x]:.4g}" for x in range(W)) for z in range(D)]
    walk = ["".join("1" if S.walk[z, x] else "0" for x in range(W)) for z in range(D)]
    ramp = ["".join(S.ramp[z]) for z in range(D)]
    rise = [" ".join(f"{S.rise[z, x]:.6g}" for x in range(W)) for z in range(D)]
    return {"width": W, "depth": D, "tags": [],
            "layers": [{"name": "ground", "height": height, "walk": walk, "ramp": ramp, "rise": rise}]}


def write_map(S, env):
    doc = {
        "format": 2, "id": "fortress", "name": "Aetherite Fortress", "geometry": "geometry.glb",
        "grid": grid_json(S), "points": S.points, "placements": [], "zones": S.zones,
        "lights": S.lights, "emitters": S.emitters, "colliders": [],
        "environment": env,
        "props": {"music": "fortress", "camera": {"distance": 13, "pitch": 27, "fov": 30}},
    }
    with open(os.path.join(OUT, "map.json"), "w") as f:
        json.dump(doc, f, indent=1)
    return doc


def main():
    t0 = time.time()
    ao = "--no-ao" not in sys.argv
    os.makedirs(OUT, exist_ok=True)
    texdir = os.path.join(OUT, "textures")
    textures.build(texdir)
    # sky cards and the LUT live next to map.json; the rest is embedded in the glb
    for name in ("sky_ridge.png", "sky_skyline.png", "sky_smoke.png", "sky_glow.png", "grade.cube"):
        os.replace(os.path.join(texdir, name), os.path.join(OUT, name))
    fx.build(FX, with_figures="--figures" in sys.argv or not os.path.exists(
        os.path.join(GAME, "sprites", "cael.anim.json")))
    print(f"textures and sprites: {time.time() - t0:.1f}s")

    S = layout.build_scene()
    arrays = S.kit.arrays()
    print(f"scene: {S.kit.triangle_count()} triangles, {len(S.lights)} lights, {len(S.emitters)} emitters "
          f"({time.time() - t0:.1f}s)")
    if "--no-check" not in sys.argv:
        verify.report(S, arrays)
    glb = os.path.join(OUT, "geometry.glb")
    stats = blender_io.build(arrays, texdir, glb, ao=ao)
    print("exported", blender_io.glb_info(glb), stats)
    write_map(S, layout.environment(S))
    size = sum(os.path.getsize(os.path.join(dp, f)) for dp, _, fs in os.walk(OUT) for f in fs)
    print(f"maps/fortress: {size / 1e6:.2f} MB; done in {time.time() - t0:.1f}s")


if __name__ == "__main__":
    main()
