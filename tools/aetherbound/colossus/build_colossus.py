#!/usr/bin/env python3
"""Builds samples/aetherbound/models/colossus.glb and colossus.json.

    /tmp/claude-0/bl/venv/bin/python tools/aetherbound/colossus/build_colossus.py [--fast] [--static]

--fast skips the ambient-occlusion bake, --static skips the animations (for looking at the mesh).
COLOSSUS_CACHE=<folder> keeps the occlusion between runs, COLOSSUS_DEBUG=<folder> dumps the atlases as PNG.
tools/aetherbound/colossus/verify.py checks the result against the brief.
"""
import json
import math
import os
import sys
import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
ROOT = os.path.dirname(os.path.dirname(os.path.dirname(HERE)))
OUT = os.path.join(ROOT, "samples", "aetherbound", "models")

import bpy  # noqa: E402
import blend  # noqa: E402
import colossus_model as model  # noqa: E402
import paint  # noqa: E402

BODY_SIZE = (1024, 512)
CRYSTAL_SIZE = (512, 256)


def paint_atlases(B, info, ao=None):
    W, H = BODY_SIZE
    albedo = np.zeros((H, W, 3)); height = np.zeros((H, W)); emis = np.zeros((H, W, 3))
    covered = np.zeros((H, W), dtype=bool)
    cw, chh = CRYSTAL_SIZE
    c_alb = np.zeros((chh, cw, 3)); c_emis = np.zeros((chh, cw, 3))
    ctx = dict(wounds=info["wounds"])
    for c in B.charts:
        if c.atlas == "body":
            a, h, e = paint.paint_body(c, ctx)
            albedo[c.y:c.y + c.h, c.x:c.x + c.w] = a
            height[c.y:c.y + c.h, c.x:c.x + c.w] = h
            emis[c.y:c.y + c.h, c.x:c.x + c.w] = e
            covered[c.y:c.y + c.h, c.x:c.x + c.w] = True
        elif c.atlas == "crystal":
            a, e = paint.paint_crystal(c, ctx)
            c_alb[c.y:c.y + c.h, c.x:c.x + c.w] = a
            c_emis[c.y:c.y + c.h, c.x:c.x + c.w] = e
    normal = np.zeros((H, W, 3)); normal[...] = (0.5, 0.5, 1.0)
    for c in B.charts:                                    # per chart, so slopes never cross chart borders
        if c.atlas == "body":
            normal[c.y:c.y + c.h, c.x:c.x + c.w] = paint.normal_from_height(height[c.y:c.y + c.h, c.x:c.x + c.w])
    if ao is not None:
        # posterise the occlusion so it keeps the texture's hard steps
        lv = np.array([0.42, 0.58, 0.74, 0.88, 1.0])
        q = lv[np.clip(np.digitize(ao, [0.35, 0.55, 0.72, 0.88]), 0, 4)]
        albedo = albedo * q[..., None]
    return albedo, normal, emis, c_alb, c_emis


def main():
    fast = "--fast" in sys.argv
    static = "--static" in sys.argv
    scene = blend.reset_scene(30)
    B, info = model.build(32.0)
    fill = B.pack({"body": BODY_SIZE, "crystal": CRYSTAL_SIZE})
    print("vertices", len(B.pos), "triangles", len(B.tris), "charts", len(B.charts), "atlas", fill)

    ao = None
    if not fast:
        import ao as aomod
        ao = aomod.compute(B, BODY_SIZE, cache_dir=os.environ.get("COLOSSUS_CACHE"))
    albedo, normal, emis, c_alb, c_emis = paint_atlases(B, info, ao)
    dbg = os.environ.get("COLOSSUS_DEBUG")
    if dbg:
        from PIL import Image
        os.makedirs(dbg, exist_ok=True)
        for n, a in (("albedo", albedo), ("normal", normal), ("emissive", emis), ("crystal", c_alb), ("crystal_e", c_emis)):
            Image.fromarray((np.clip(a, 0, 1) * 255 + 0.5).astype(np.uint8)).save(os.path.join(dbg, n + ".png"))
    body = blend.make_material("colossus_body", blend.np_image("colossus_albedo", albedo),
                               blend.np_image("colossus_normal", normal, srgb=False),
                               blend.np_image("colossus_glow", emis), emissive_strength=5.0)
    crystal = blend.make_material("aetherite", blend.np_image("aetherite_albedo", c_alb), None,
                                  blend.np_image("aetherite_glow", c_emis), emissive_strength=5.0)
    eyes = blend.make_material("colossus_eyes", base_color=(0.2, 0.1, 0.35), emissive_color=(0.62, 0.36, 1.0), emissive_strength=7.0)
    mesh = blend.make_mesh("colossus_mesh", B, [("body", body), ("crystal", crystal), ("eyes", eyes)], scene)
    arm = blend.make_armature("colossus", model.SKEL, scene)
    blend.skin(mesh, arm, B)

    points = {}
    if not static:
        import colossus_anim as anim
        points, baked, rig = anim.key_all(arm, model.SKEL, info)
        print("height of the body at idle:", round(anim.top_of(B, rig, baked["idle"][0]), 2))
        for name, worst in anim.floor_report(B, rig, baked).items():
            print(f"floor {name}:", ", ".join(f"{bone} {z:.2f} at {t:.2f} s" for z, bone, t in worst) or "clear")
    blend.reset_pose(arm)
    os.makedirs(OUT, exist_ok=True)
    path = os.path.join(OUT, "colossus.glb")
    blend.export_glb(path, animations=not static)
    rep = blend.glb_report(path)
    print(json.dumps(rep, indent=1))
    if points:
        json.dump(points, open(os.path.join(OUT, "colossus.json"), "w"), indent=2)
        print(json.dumps(points, indent=1))


if __name__ == "__main__":
    main()
