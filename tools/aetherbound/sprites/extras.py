"""Rowan's glowing veins (the emissive sheet of rowan_bound) and his
broken sword."""
import json
import os

import numpy as np
from PIL import Image

import pixel
import render
from render import ball, box, seg, frame, rx, ry, rz, xf
from rig import PAL, M, G_HEAD, G_ARML, G_ARMR, G_WEAPON

VIOLET = (155, 92, 255)
CORE = (226, 204, 255)
SKINS = (M["skin"], M["skin_pale"], M["skin_tan"])


def _line(a, b):
    """Whole pixels from a to b (Bresenham)."""
    x0, y0, x1, y1 = int(np.floor(a[0])), int(np.floor(a[1])), int(np.floor(b[0])), int(np.floor(b[1]))
    dx, dy = abs(x1 - x0), -abs(y1 - y0)
    sx, sy = (1 if x0 < x1 else -1), (1 if y0 < y1 else -1)
    err = dx + dy
    out = []
    while True:
        out.append((x0, y0))
        if x0 == x1 and y0 == y1:
            return out
        e2 = 2 * err
        if e2 >= dy:
            err += dy
            x0 += sx
        if e2 <= dx:
            err += dx
            y0 += sy


def veins(px, marks, view):
    """Violet veins up the arms, the neck and the face, as an emissive frame.
    They follow the bones, so they move with the body, and show only where
    that part of him is in view."""
    n = render.SIZE
    out = np.zeros((n, n, 4), np.uint8)
    ground = px["ground"]

    def P(p):
        x, y, _ = render.project(view, p, ground)
        return np.array([x, y])

    def stroke(points, ok, core_at=()):
        for i in range(len(points) - 1):
            for x, y in _line(points[i], points[i + 1]):
                if 0 <= x < n and 0 <= y < n and ok(x, y):
                    out[y, x] = VIOLET + (255,)
        for p in core_at:
            x, y = int(np.floor(p[0])), int(np.floor(p[1]))
            if 0 <= x < n and 0 <= y < n and ok(x, y):
                out[y, x] = CORE + (255,)

    for side, grp in (("L", G_ARML), ("R", G_ARMR)):
        sh, el, hand = (P(p) for p in marks["joints"][side])
        ok = lambda x, y, g=grp: px["solid"][y, x] and px["grp"][y, x] == g
        pts = []
        for a, b, steps in ((hand, el, 3), (el, sh, 3)):
            d = b - a
            length = np.linalg.norm(d)
            perp = np.array([-d[1], d[0]]) / max(length, 1e-6)
            for i in range(steps):
                t = i / steps
                wig = (0.0, 0.9, -0.9)[i] if length > 3 else 0.0
                pts.append(a + d * t + perp * wig)
        pts.append(sh)
        stroke(pts, ok, core_at=(hand, el))
    # neck and face
    hr, hc, rad = marks["head_rot"], marks["head"], marks["radius"]
    skin = lambda x, y: px["solid"][y, x] and px["grp"][y, x] == G_HEAD and px["mat"][y, x] in SKINS

    def H(x, y, z):
        v = np.array([x, y, z], float)
        v = v / np.linalg.norm(v) * rad * 0.99
        return hc + hr @ v

    for sg in (1, -1):
        if (view @ (hr @ np.array([sg * 0.75, -0.3, 0.6])))[2] < 0.1:
            continue
        pts = [P(marks["neck"] + hr @ np.array([sg * 1.5, -0.5, 1.0])), P(H(sg * 4.2, -6.2, 4.6)),
               P(H(sg * 5.6, -4.2, 5.2)), P(H(sg * 5.4, -2.4, 5.6)), P(H(sg * 6.4, -0.6, 4.6))]
        stroke(pts, skin, core_at=(pts[2],))
        stroke([P(H(sg * 5.6, -4.2, 5.2)), P(H(sg * 3.6, -4.6, 6.8))], skin)
    return out


# --------------------------------------------------------------- the sword
def _blade(wf, y0, y1, tip=False, hilt=False, jag=0):
    r = wf[:3, :3]

    def W(x, y, z):
        return xf(wf, (x, y, z))

    def bm(q):
        # scorched: soot creeps over the steel
        t = q[1] * jag  # towards the break
        soot = (t > 0.55 - 0.25 * np.sin(q[0] * 4.0)) | ((np.sin(q[1] * 3.1 + 1.3) > 0.8) & (q[0] < 0.1))
        return np.where(soot, M["charcoal"], np.where(q[0] > 0.1, M["blade"], M["blade_d"]))

    out = []
    if hilt:
        out += [ball(W(0, -2.9, 0), 1.05, M["charcoal_l"], G_WEAPON, thin=True),
                seg(W(0, -2.2, 0), W(0, 1.6, 0), 0.8, 0.8, M["crimson_d"], G_WEAPON, thin=True),
                box(W(0, 2.2, 0), (2.9, 0.65, 0.85), M["charcoal_l"], G_WEAPON, rot=r, thin=True)]
    out.append(box(W(0, (y0 + y1) / 2, 0), (1.2, (y1 - y0) / 2, 0.5), bm, G_WEAPON, rot=r, thin=True))
    if tip:
        out.append(seg(W(0, y1 - 0.2, 0), W(0, y1 + 2.4, 0), 1.2, 0.15, bm, G_WEAPON, xhint=r[:, 0], sz=0.42, thin=True))
    # the jagged break: a shard left standing on one edge
    yb = y1 if jag > 0 else y0
    out.append(seg(W(0.55 * jag, yb - 0.2 * jag, 0), W(0.9 * jag, yb + 1.6 * jag, 0), 0.6, 0.1, bm, G_WEAPON,
                   xhint=r[:, 0], sz=0.5, thin=True))
    return out


def _shot(prims, yaw, pitch, ground, box_):
    px = pixel.reduce(render.cast(prims, yaw, pitch=pitch, ground=ground))
    pixel.shade(px, PAL)
    rgba = pixel.colour(px, PAL)
    pixel.outline(rgba, px, PAL)
    x, y, w, h = box_
    return rgba[y:y + h, x:x + w]


def _write(out, name, image, pivot):
    Image.fromarray(image).save(os.path.join(out, name + ".png"), optimize=True)
    h, w = image.shape[:2]
    anim = {"atlas": name + ".png", "pixels_per_unit": 32, "pivot": pivot,
            "clips": {"idle": {"loop": True, "directions": [[{"rect": [0, 0, w, h], "duration": 1.0}]]}}}
    with open(os.path.join(out, name + ".anim.json"), "w") as f:
        json.dump(anim, f, separators=(",", ":"))
        f.write("\n")


def swords(out):
    os.makedirs(out, exist_ok=True)
    # Lying flat, seen from straight above: the hilt half to the left, the
    # point half fallen a little askew to its right.
    flat = np.array([[0.0, 1.0, 0.0], [0.0, 0.0, 1.0], [1.0, 0.0, 0.0]])
    a = frame((-16.0, 0.6, 1.2), ry(7) @ flat)
    b = frame((4.6, 0.6, -2.0), ry(-14) @ flat)
    prims = _blade(a, 2.8, 12.6, hilt=True, jag=1) + _blade(b, 0.0, 9.6, tip=True, jag=-1)
    image = _shot(prims, 0.0, 90.0, 32.0, (8, 20, 48, 24))
    _write(out, "sword_broken", image, [0.5, 0.5])
    # The hilt half, upright.
    k = 1.25
    c = frame((0.0, 4.6 * k, 0.0), rz(4) * k)
    prims = _blade(c, 2.8, 12.6, hilt=True, jag=1)
    image = _shot(prims, 0.0, 0.0, 62.0, (20, 32, 24, 32))
    _write(out, "sword_half", image, [0.5, 1])
