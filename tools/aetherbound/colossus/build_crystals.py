#!/usr/bin/env python3
"""Builds samples/aetherbound/models/aether_crystal.glb (the great floating crystal, 14 units tall,
animation `idle`: a 20 s loop, one slow turn and two bobs, with drifting satellite shards) and
aether_shard.glb (one loose 0.5-unit shard for debris).

    /tmp/claude-0/bl/venv/bin/python tools/aetherbound/colossus/build_crystals.py
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
import paint  # noqa: E402
from meshkit import Builder, vec, unit  # noqa: E402

FPS = 30


def paint_atlas(B, size):
    W, H = size
    alb = np.zeros((H, W, 3)); emis = np.zeros((H, W, 3))
    for c in B.charts:
        a, e = paint.paint_crystal(c, {})
        alb[c.y:c.y + c.h, c.x:c.x + c.w] = a
        emis[c.y:c.y + c.h, c.x:c.x + c.w] = e
    return alb, emis


def material(name, alb, emis, strength):
    return blend.make_material(name, blend.np_image(name + "_albedo", alb), None, blend.np_image(name + "_glow", emis),
                               emissive_strength=strength)


def great_crystal():
    scene = blend.reset_scene(FPS)
    rng = np.random.RandomState(5)
    size = (512, 512)
    parts = []                                      # (object name, Builder, pivot)

    # the heart: one long double-terminated crystal with a sheaf of lesser ones grown along it
    B = Builder(14.0)
    B.set(mat="crystal", cls="crystal")
    tilt = unit(vec(0.06, 0.03, 1.0))
    B.set(params=dict(seed=1, hot=0.5, hot_width=0.75, vein=0.03, gain=1.05))
    B.shard(-tilt * 7.0, tilt, 14.0, 1.55, 6, tip=0.26, butt=0.24, seed=1)
    n = 17
    for i in range(n):
        up = 1 if i % 2 == 0 else -1               # alternately growing up and down the heart
        a = i * 2.39996 + rng.uniform(-0.3, 0.3)
        radial = vec(math.cos(a), math.sin(a), 0)
        h = up * rng.uniform(0.2, 3.2)
        ln = rng.uniform(4.5, 8.8) * (0.7 if i > 8 else 1.0)
        rad = ln * rng.uniform(0.085, 0.11)
        lean = rng.uniform(0.16, 0.42) + (0.3 if i > 8 else 0.0)
        d = unit(tilt * up + radial * lean)
        base = tilt * (h - up * ln * 0.3) + radial * rng.uniform(0.55, 0.95)
        B.set(params=dict(seed=10 + i, hot=0.75, hot_width=0.8, vein=0.03, gain=0.95))
        B.shard(base, d, ln, rad, 6 if i < 6 else 5, tip=0.34, butt=0.2, seed=10 + i)
    for i in range(8):                             # a ruff of small points round the waist
        a = i * 0.785 + rng.uniform(-0.25, 0.25)
        radial = vec(math.cos(a), math.sin(a), 0)
        up = rng.choice([-1, 1])
        d = unit(radial + tilt * up * rng.uniform(0.2, 0.9))
        ln = rng.uniform(1.6, 2.8)
        B.set(params=dict(seed=40 + i, hot=0.9, hot_width=0.9, vein=0.03, gain=1.1))
        B.shard(tilt * rng.uniform(-1.6, 1.6) + radial * 0.9, d, ln, ln * 0.11, 5, tip=0.4, butt=0.15, seed=40 + i)
    # exactly 14 units from the lowest point to the highest, centred on the origin
    P = np.array(B.pos)
    lo, hi = P[:, 2].min(), P[:, 2].max()
    k = 14.0 / (hi - lo)
    B.pos = [(p - vec(0, 0, (lo + hi) / 2)) * k for p in B.pos]
    for ch in B.charts:
        ch.grid_p = (ch.grid_p - vec(0, 0, (lo + hi) / 2)) * k
    parts.append(("aether_crystal", B, vec(0, 0, 0)))

    # satellites: loose shards that hang in the air around it
    sats = []
    for i in range(4):
        a = i * math.pi / 2 + 0.6
        r = rng.uniform(3.4, 4.4)
        z = (-1) ** i * rng.uniform(1.5, 4.5)
        ln = rng.uniform(1.8, 3.0)
        pivot = vec(math.cos(a) * r, math.sin(a) * r, z)
        Bs = Builder(14.0)
        Bs.set(mat="crystal", cls="crystal", params=dict(seed=60 + i, hot=0.5, hot_width=0.9, vein=0.03, gain=1.1))
        d = unit(vec(rng.uniform(-0.3, 0.3), rng.uniform(-0.3, 0.3), 1.0))
        Bs.shard(-d * ln / 2, d, ln, ln * 0.12, 5, tip=0.35, butt=0.25, seed=60 + i)
        parts.append((f"satellite_{i + 1}", Bs, pivot))
        sats.append((pivot, rng.uniform(0, 6.28)))

    # one atlas for all of them
    all_charts = [c for _, b, _ in parts for c in b.charts]
    packer = Builder(14.0)
    packer.charts = all_charts
    fill = packer.pack({"crystal": size})
    for _, b, _ in parts:
        b.sizes = packer.sizes
    alb, emis = paint_atlas(packer, size)
    mat = material("aetherite", alb, emis, 5.0)
    objs = []
    verts = 0
    for name, b, pivot in parts:
        ob = blend.make_mesh(name, b, [("crystal", mat)], scene)
        ob.location = pivot
        ob.rotation_mode = 'XYZ'
        objs.append(ob)
        verts += len(b.pos)
    for ob in objs[1:]:
        ob.parent = objs[0]
    print("crystal vertices", verts, "atlas", fill)

    # idle: 20 s; the cluster turns once and bobs twice, the satellites drift on their own beat
    n = 20 * FPS
    tau = 2 * math.pi
    for k, ob in enumerate(objs):
        ob.animation_data_create()
        act = bpy.data.actions.new("idle" if k == 0 else f"idle_{k}")
        ob.animation_data.action = act
        for f in range(n + 1):
            t = f / n
            if k == 0:
                ob.location = (0, 0, 0.4 * math.sin(tau * 2 * t))
                ob.rotation_euler = (math.radians(1.5) * math.sin(tau * t), math.radians(1.5) * math.cos(tau * t) - math.radians(1.5), tau * t)
            else:
                pivot, ph = sats[k - 1]
                ob.location = (pivot[0], pivot[1], pivot[2] + 0.35 * math.sin(tau * 3 * t + ph))
                ob.rotation_euler = (0.25 * math.sin(tau * 2 * t + ph), 0.25 * math.cos(tau * 2 * t + ph) - 0.25 * math.cos(ph) * 0, -tau * 2 * t)
            ob.keyframe_insert("location", frame=f)
            ob.keyframe_insert("rotation_euler", frame=f)
        tr = ob.animation_data.nla_tracks.new()
        tr.name = "idle"
        tr.strips.new("idle", 0, act)
        ob.animation_data.action = None
        ob.location = (0, 0, 0) if k == 0 else sats[k - 1][0]
        ob.rotation_euler = (0, 0, 0)
    path = os.path.join(OUT, "aether_crystal.glb")
    blend.export_glb(path)
    print(json.dumps(blend.glb_report(path), indent=1))


def loose_shard():
    scene = blend.reset_scene(FPS)
    B = Builder(48.0)
    B.set(mat="crystal", cls="crystal", params=dict(seed=7, hot=0.5, hot_width=0.9, vein=0.03, gain=1.1))
    d = unit(vec(0.0, 0.0, 1.0))
    B.shard(-d * 0.25, d, 0.5, 0.075, 5, tip=0.36, butt=0.24, seed=7)
    size = (64, 32)
    B.pack({"crystal": size})
    alb, emis = paint_atlas(B, size)
    mat = material("aetherite", alb, emis, 5.0)
    blend.make_mesh("aether_shard", B, [("crystal", mat)], scene)
    path = os.path.join(OUT, "aether_shard.glb")
    blend.export_glb(path, animations=False)
    print("shard vertices", len(B.pos), json.dumps(blend.glb_report(path)))


if __name__ == "__main__":
    os.makedirs(OUT, exist_ok=True)
    great_crystal()
    loose_shard()
