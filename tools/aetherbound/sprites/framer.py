"""One posed character, one direction -> one 64x64 pixel-art frame."""
import numpy as np
import render
import pixel
from rig import PAL, M, solve, G_HEAD, G_HAIR, G_WEAPON, G_FX, G_CAPE, G_ARML, G_ARMR, G_LEGL, G_LEGR
from chars import CHARS, dress
from pixel import hexrgb

YAWS = (0.0, -90.0, 180.0, 90.0)   # south, west, north, east
SKINS = (M["skin"], M["skin_pale"], M["skin_tan"])


def _put(rgba, x, y, rgb, px, need_skin=True):
    if 0 <= x < render.SIZE and 0 <= y < render.SIZE:
        if need_skin and not (px["solid"][y, x] and px["mat"][y, x] in SKINS and px["grp"][y, x] == G_HEAD):
            return False
        rgba[y, x, :3] = rgb
        rgba[y, x, 3] = 255
        return True
    return False


def face(rgba, px, marks, view, c, p, ground):
    """Eyes, brows and mouth, placed on whole pixels."""
    ramps = PAL.arrays()[0]
    skin = ramps[M[c["skin"]]]
    eye_rgb = hexrgb(c["eye"])
    dark = (34, 22, 38)
    k = marks["scale"]
    seen = []
    for (pos, nrm), sg in zip(marks["eyes"], (1, -1)):
        nv = view @ nrm
        x, y, _ = render.project(view, pos, ground)
        seen.append((x, y, nv, sg))
    front = [e for e in seen if e[2][2] > 0.12]
    if not front:
        return
    if c["costume"] in ("guard", "lancer"):
        # Closed helm: two embers of eyes in the slit.
        for x, y, nv, sg in front:
            xi, yi = int(np.floor(x)), int(np.floor(y))
            if 0 <= xi < render.SIZE and 0 <= yi < render.SIZE and px["mat"][yi, xi] == M["visor"]:
                rgba[yi, xi, :3] = (255, 170, 70) if p["eyes"] != "closed" else (120, 60, 40)
        return
    both = len(front) == 2 and abs(front[0][2][0] + front[1][2][0]) < 0.75
    state = p["eyes"]
    if both:
        # Keep the pair symmetrical about the rounded midline.
        mx = (front[0][0] + front[1][0]) / 2
        my = (front[0][1] + front[1][1]) / 2
        half = abs(front[0][0] - front[1][0]) / 2
        mid = int(np.floor(mx + 0.5))
        gap = max(2, int(round(half - 0.6)))
        row = int(np.floor(my))
        lean = front[0][2][0] + front[1][2][0]
        shift = 1 if lean > 0.3 else (-1 if lean < -0.3 else 0)
        places = [(mid + shift + gap, 1), (mid + shift - gap - 1, -1)]
    else:
        e = max(front, key=lambda t: t[2][2])
        row = int(np.floor(e[1]))
        # A profile eye sits a little inside the face's leading edge.
        fwd = view @ marks["head_rot"][:, 2]
        hx, _, _ = render.project(view, marks["head"], ground)
        sgn = 1 if fwd[0] > 0 else -1
        ex = hx + sgn * marks["radius"] * (0.62 if abs(fwd[0]) > 0.8 else 0.5 * abs(fwd[0]) + 0.2)
        places = [(int(np.floor(ex)), sgn)]
    wide = 2 if both else 1
    for x0, outer in places:
        cols = [x0, x0 + outer] if wide == 2 else [x0]
        if state == "open":
            for i, cx in enumerate(cols):
                _put(rgba, cx, row - 1, dark, px)
                _put(rgba, cx, row, dark if i == 0 or wide == 1 else eye_rgb, px)
                _put(rgba, cx, row + 1, eye_rgb if (i == 0 or wide == 1) else skin[4], px)
        elif state == "closed":
            for cx in cols + ([x0 - outer] if wide == 1 else []):
                _put(rgba, cx, row + 1, dark, px)
        elif state == "hurt":
            _put(rgba, cols[0], row, dark, px)
            _put(rgba, cols[-1] if wide == 2 else cols[0], row + (0 if wide == 1 else -1), dark, px)
            if wide == 2:
                _put(rgba, cols[0] - outer, row + 1, dark, px)
        if c["costume"] == "darius" and state != "closed":
            for cx in cols + [cols[-1] + outer]:
                _put(rgba, cx, row - 2, ramps[M["hair_iron"]][1], px)
    if both and c["costume"] != "darius":
        x, y, _ = render.project(view, marks["mouth"], ground)
        mx, my = int(np.floor((front[0][0] + front[1][0]) / 2 + 0.5)) + shift, int(np.floor(y))
        m = p["mouth"]
        if m == "closed":
            _put(rgba, mx - 1, my, skin[1], px)
            _put(rgba, mx, my, skin[1], px)
        elif m == "open":
            for dx in (-1, 0):
                _put(rgba, mx + dx, my, (120, 40, 50), px)
                _put(rgba, mx + dx, my + 1, skin[1], px)
        elif m == "grit":
            for dx in (-2, -1, 0, 1):
                _put(rgba, mx + dx, my, skin[1], px)


def render_frame(name, p, direction, want_normal=False):
    """Returns (rgba, px, marks, view)."""
    c = CHARS[name]
    s = solve(p, c["build"])
    prims, marks = dress(c, s)
    if p["mirror_e"] and direction == 3:
        # A staff carried at the side would cross the face when its hand is
        # nearest the viewer: the east row shows the mirror image instead.
        flip = np.diag([-1.0, 1.0, 1.0, 1.0])
        f3 = flip[:3, :3]
        for q in prims:
            q.m = flip @ q.m
        marks["head"] = f3 @ marks["head"]
        marks["head_rot"] = f3 @ marks["head_rot"]
        marks["eyes"] = [(f3 @ pos, f3 @ nrm) for pos, nrm in marks["eyes"]]
        marks["mouth"] = f3 @ marks["mouth"]
        marks["neck"] = f3 @ marks["neck"]
        marks["knees"] = {n: f3 @ v for n, v in marks["knees"].items()}
        marks["joints"] = {n: tuple(f3 @ v for v in t) for n, t in marks["joints"].items()}
    yaw = YAWS[direction] + (p["lie"] if direction in (0, 2) else 0.0)
    view = render.view_matrix(yaw)
    lift = p["lift"] if isinstance(p["lift"], tuple) else (p["lift"], p["lift"])
    ground = render.GROUND - lift[0 if direction in (0, 2) else 1]
    px = pixel.reduce(render.cast(prims, yaw, ground=ground))
    px["ground"] = ground
    so = px["solid"]
    px["clipped"] = "".join(n for n, hit in (("top", so[0].any()), ("bottom", so[-1].any()), ("left", so[:, 0].any()),
                                             ("right", so[:, -1].any())) if hit)
    # The limbs on the far side of a profile go a tone darker.
    far = []
    dz = (view @ (marks["knees"]["L"] - marks["knees"]["R"]))[2]
    if abs(dz) > 2.2:
        far.append(G_LEGR if dz > 0 else G_LEGL)
    dz = (view @ (marks["joints"]["L"][0] - marks["joints"]["R"][0]))[2]
    if abs(dz) > 5.0:
        far.append(G_ARMR if dz > 0 else G_ARML)
    pixel.shade(px, PAL, flat_groups=(G_FX,), no_line_groups=(G_FX,), far_groups=far)
    rgba = pixel.colour(px, PAL)
    face(rgba, px, marks, view, c, p, ground)
    pixel.outline(rgba, px, PAL, skip_groups=(G_FX,))
    return rgba, px, marks, view
