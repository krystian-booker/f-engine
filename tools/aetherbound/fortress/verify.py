"""Checks of the generated fortress against its brief, on the mesh itself:
floor heights under every walkable cell, connectivity and path width,
and sight lines from the field and battle cameras to the party."""
import math
from collections import deque
import numpy as np
from mathutils import Vector
import blender_io
import layout


def _ray(bvh, a, b):
    """True when something lies between a and b."""
    a, b = Vector(a), Vector(b)
    d = b - a
    L = d.length
    hit = bvh.ray_cast(a, d / L, L - 0.06)
    return hit[0] is not None


def floor_check(S, bvh):
    bad = []
    worst = 0.0
    for z in range(layout.D):
        for x in range(layout.W):
            if not S.walk[z, x]:
                continue
            for fx in (0.25, 0.5, 0.75):
                for fz in (0.25, 0.5, 0.75):
                    px, pz = x + fx, z + fz
                    g = S.ground(px, pz)
                    hit = bvh.ray_cast(Vector((px, g + 2.2, pz)), Vector((0, -1, 0)), 12.0)
                    if hit[0] is None:
                        bad.append((x, z, "hole"))
                        continue
                    dy = hit[0].y - g
                    worst = max(worst, abs(dy))
                    if abs(dy) > 0.2:
                        bad.append((x, z, round(dy, 2)))
    return bad, worst


def connectivity(S):
    start = (int(S.points["start"]["position"][0]), int(S.points["start"]["position"][2]))
    goal = (int(S.points["door"]["position"][0]), int(S.points["door"]["position"][2]))
    seen = {start}
    queue = deque([start])
    while queue:
        x, z = queue.popleft()
        for dx, dz in ((1, 0), (-1, 0), (0, 1), (0, -1)):
            n = (x + dx, z + dz)
            if 0 <= n[0] < layout.W and 0 <= n[1] < layout.D and S.walk[n[1], n[0]] and n not in seen:
                if abs(S.ground(n[0] + 0.5, n[1] + 0.5) - S.ground(x + 0.5, z + 0.5)) < 0.6:
                    seen.add(n)
                    queue.append(n)
    total = int(S.walk.sum())
    # every walkable cell lies in some 3 x 3 block of walkable cells (bites out of the bridge edge excepted)
    thin = []
    for z in range(layout.D):
        for x in range(layout.W):
            if not S.walk[z, x]:
                continue
            ok = False
            for oz in (-2, -1, 0):
                for ox in (-2, -1, 0):
                    x0, z0 = x + ox, z + oz
                    if x0 >= 0 and z0 >= 0 and x0 + 3 <= layout.W and z0 + 3 <= layout.D and \
                            S.walk[z0:z0 + 3, x0:x0 + 3].all():
                        ok = True
            if not ok:
                thin.append((x, z))
    return goal in seen, len(seen), total, thin


def camera_pos(target, yaw=0.0, pitch=27.0, distance=13.0):
    p, y = math.radians(pitch), math.radians(yaw)
    # yaw 0 looks north (-z) from the south
    return (target[0] - math.sin(y) * math.cos(p) * distance, target[1] + math.sin(p) * distance,
            target[2] + math.cos(y) * math.cos(p) * distance)


def field_occlusion(S, bvh):
    """For the party standing in each walkable cell: is the field camera's
    view of their feet / body blocked?"""
    rows = []
    feet = body = 0
    for z in range(layout.D):
        row = ""
        for x in range(layout.W):
            if not S.walk[z, x]:
                row += " "
                continue
            p = (x + 0.5, S.ground(x + 0.5, z + 0.5), z + 0.5)
            cam = camera_pos(p)
            f = _ray(bvh, cam, (p[0], p[1] + 0.25, p[2]))
            b = _ray(bvh, cam, (p[0], p[1] + 0.8, p[2])) or _ray(bvh, cam, (p[0], p[1] + 1.4, p[2]))
            if b:
                body += 1
                row += "X"
            elif f:
                feet += 1
                row += "f"
            else:
                row += "."
        rows.append(row)
    return feet, body, rows


def battle_occlusion(S, bvh, name, yaw, pitch=17.0, distance=13.0):
    c = S.points[f"{name}_centre"]["position"]
    cam = camera_pos((c[0], c[1] + 0.8, c[2]), yaw, pitch, distance)
    blocked = []
    for key, pt in S.points.items():
        if key.startswith(name + "_") and key != name + "_centre":
            p = pt["position"]
            for h in (0.1, 0.8, 1.45):
                if _ray(bvh, cam, (p[0], p[1] + h, p[2])):
                    blocked.append((key, h))
    inside = bvh.ray_cast(Vector(cam), Vector((0, 1, 0)), 60.0)[0] is not None
    return cam, blocked, inside


def report(S, arrays, verbose=True):
    bvh = blender_io.bvh_of(arrays)
    bad, worst = floor_check(S, bvh)
    ok, reached, total, thin = connectivity(S)
    feet, body, rows = field_occlusion(S, bvh)
    print(f"verify: floor mismatches {len(bad)} (worst {worst:.2f}); start->door {'ok' if ok else 'BROKEN'}, "
          f"{reached}/{total} cells reached; cells not in a 3x3 block: {len(thin)}")
    if bad and verbose:
        print("  floor:", bad[:24])
    if thin and verbose:
        print("  thin:", thin[:24])
    print(f"verify: field camera: feet hidden in {feet} cells, body hidden in {body} cells")
    if (feet or body) and verbose:
        for z, row in enumerate(rows):
            if "X" in row or "f" in row:
                print(f"  z{z:02d} {row}")
    for name in ("enc1", "enc2"):
        for yaw in (0, 180):
            cam, blocked, inside = battle_occlusion(S, bvh, name, yaw)
            print(f"verify: battle {name} yaw {yaw}: camera at ({cam[0]:.1f}, {cam[1]:.1f}, {cam[2]:.1f})"
                  f"{' UNDER GEOMETRY' if inside else ''}; blocked: {blocked if blocked else 'none'}")
    return bvh
