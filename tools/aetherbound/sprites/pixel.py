"""Ray-cast buffers to pixel art: one sample set per pixel becomes one
palette colour, with hard alpha, cel shading in a few tones per material,
contact lines between body parts and a dark outline."""
import numpy as np
from render import SIZE, SS

# Light in view space (x right, y up, z towards the viewer): upper left, in front.
LIGHT = np.array([-0.52, 0.62, 0.59])
LIGHT = LIGHT / np.linalg.norm(LIGHT)

OUTLINE, SHADOW, MID, LIGHTT, HIGH = 0, 1, 2, 3, 4


def hexrgb(h):
    h = h.lstrip("#")
    return tuple(int(h[i:i + 2], 16) for i in (0, 2, 4))


class Palette:
    """Named materials, each a ramp of five colours: outline, shadow, mid,
    light, highlight. `gloss` is the N.L above which the highlight shows
    (2 = never); `bands` are the N.L thresholds of mid and light."""

    def __init__(self):
        self.names, self.ramps, self.gloss, self.bands, self.index = [], [], [], [], {}

    def add(self, name, ramp, gloss=2.0, bands=(-0.08, 0.38)):
        self.index[name] = len(self.names)
        self.names.append(name)
        self.ramps.append([hexrgb(c) for c in ramp])
        self.gloss.append(gloss)
        self.bands.append(bands)
        return self.index[name]

    def __getitem__(self, name):
        return self.index[name]

    def arrays(self):
        return (np.array(self.ramps, np.uint8), np.array(self.gloss), np.array(self.bands))


def _mode(keys, weight):
    """Per pixel, the key with the largest total weight. keys: (P, K) ints
    (-1 = empty), weight: (P, K)."""
    best = np.full(keys.shape[0], -1, np.int64)
    best_w = np.zeros(keys.shape[0])
    for k in np.unique(keys):
        if k < 0:
            continue
        w = np.where(keys == k, weight, 0).sum(axis=1)
        better = w > best_w
        best = np.where(better, k, best)
        best_w = np.where(better, w, best_w)
    return best


def _shift(a, dy, dx, fill):
    out = np.full_like(a, fill)
    h, w = a.shape[:2]
    ys, yd = (slice(0, h - dy), slice(dy, h)) if dy >= 0 else (slice(-dy, h), slice(0, h + dy))
    xs, xd = (slice(0, w - dx), slice(dx, w)) if dx >= 0 else (slice(-dx, w), slice(0, w + dx))
    out[yd, xd] = a[ys, xs]
    return out


NEIGH = ((0, 1), (0, -1), (1, 0), (-1, 0))


def reduce(buffers):
    """Samples to pixels. Returns dict: solid (bool), grp, mat (int), depth,
    normal (unit, view space)."""
    depth, grp, mat, normal, thin = buffers
    n = SIZE
    k = SS * SS

    def blocks(a):
        tail = a.shape[2:]
        return a.reshape(n, SS, n, SS, *tail).swapaxes(1, 2).reshape(n * n, k, *tail)

    d, g, m, nr, th = blocks(depth), blocks(grp), blocks(mat), blocks(normal), blocks(thin)
    covered = g >= 0
    count = covered.sum(axis=1)
    thin_count = (covered & th).sum(axis=1)
    solid = (count * 2 >= k) | (thin_count * 4 >= k)
    keys = np.where(covered, g.astype(np.int64) * 1024 + m, -1)
    # Thin things (blades, shafts) and nearer surfaces win ties.
    zmax = np.where(covered, d, -1e9).max(axis=1, keepdims=True)
    weight = 1.0 + 1.2 * th + 0.05 * np.clip((d - zmax) / 4.0 + 1, 0, 1)
    key = _mode(keys, np.where(covered, weight, 0))
    win = keys == key[:, None]
    wn = np.maximum(win.sum(axis=1), 1)
    pd = np.where(win, d, 0).sum(axis=1) / wn
    pn = np.where(win[:, :, None], nr, 0).sum(axis=1)
    pn /= np.maximum(np.linalg.norm(pn, axis=1), 1e-9)[:, None]
    out = {
        "solid": solid.reshape(n, n),
        "grp": np.where(solid, key // 1024, -1).reshape(n, n),
        "mat": np.where(solid, key % 1024, -1).reshape(n, n),
        "depth": np.where(solid, pd, -1e9).reshape(n, n),
        "normal": pn.reshape(n, n, 3),
    }
    return out


def shade(px, palette, flat_groups=(), no_line_groups=(), far_groups=(), line_gap=1.6, hard_gap=3.5):
    """Tone per pixel (1..4), then contact lines (tone 0 or 1)."""
    ramps, gloss, bands = palette.arrays()
    solid, grp, mat, depth, normal = px["solid"], px["grp"], px["mat"], px["depth"], px["normal"]
    m = np.where(solid, mat, 0)
    lam = normal @ LIGHT
    tone = np.full(solid.shape, MID)
    tone = np.where(lam < bands[m][..., 0], SHADOW, tone)
    tone = np.where(lam > bands[m][..., 1], LIGHTT, tone)
    tone = np.where(lam > gloss[m], HIGH, tone)
    # Cast shadows in screen space: something nearer, up and to the left.
    shadowed = np.zeros(solid.shape, bool)
    for dy, dx, gap in ((1, 1, 2.2), (2, 2, 3.2), (1, 0, 2.6), (2, 1, 3.4), (3, 2, 4.4)):
        occ = _shift(depth, dy, dx, -1e9)
        og = _shift(grp, dy, dx, -1)
        shadowed |= (occ > depth + gap) & (og != grp) & (og >= 0)
    for g in flat_groups:
        shadowed &= grp != g
    tone = np.where(shadowed & (tone > SHADOW), tone - 1, tone)
    for g in far_groups:
        tone = np.where((grp == g) & (tone > SHADOW), tone - 1, tone)
    # Remove single-pixel tone islands inside a material.
    for _ in range(2):
        same = np.zeros(solid.shape, int)
        votes = np.zeros(solid.shape + (5,), int)
        for dy, dx in NEIGH:
            nm, nt = _shift(mat, dy, dx, -1), _shift(tone, dy, dx, 0)
            ng = _shift(grp, dy, dx, -1)
            match = (nm == mat) & (ng == grp)
            same += match & (nt == tone)
            for t in range(1, 5):
                votes[..., t] += match & (nt == t)
        lonely = solid & (same == 0) & (votes.sum(axis=-1) >= 2)
        tone = np.where(lonely, votes.argmax(axis=-1), tone)
    # Contact lines: the farther pixel of two parts darkens.
    line = np.zeros(solid.shape, int)
    for dy, dx in NEIGH:
        nd, ng = _shift(depth, dy, dx, -1e9), _shift(grp, dy, dx, -1)
        gap = nd - depth
        edge = solid & (ng >= 0) & (ng != grp)
        line = np.maximum(line, np.where(edge & (gap > hard_gap), 2, np.where(edge & (gap > line_gap), 1, 0)))
    for g in no_line_groups:
        line = np.where(grp == g, 0, line)
    tone = np.where(line == 2, OUTLINE, np.where((line == 1), np.minimum(tone, SHADOW), tone))
    tone = np.where(solid, tone, -1)
    px["tone"] = tone
    return px


def colour(px, palette):
    """RGBA image with the outline added, and the outline mask."""
    ramps, _, _ = palette.arrays()
    solid, mat, tone = px["solid"], px["mat"], px["tone"]
    rgba = np.zeros(solid.shape + (4,), np.uint8)
    rgba[solid, :3] = ramps[mat[solid], tone[solid]]
    rgba[solid, 3] = 255
    return rgba


def outline(rgba, px, palette, skip_groups=()):
    """Add the 1 px outline around the solid pixels; returns the mask of
    outline pixels. Each takes the outline colour of the material it touches
    (the nearest to the viewer when several)."""
    ramps, _, _ = palette.arrays()
    solid = rgba[..., 3] > 0
    mat, depth = px["mat"], px["depth"]
    best = np.full(solid.shape, -1e9)
    pick = np.full(solid.shape, -1)
    for dy, dx in NEIGH:
        ns, nm, nd = _shift(solid, dy, dx, False), _shift(mat, dy, dx, -1), _shift(depth, dy, dx, -1e9)
        ng = _shift(px["grp"], dy, dx, -1)
        for g in skip_groups:
            ns = ns & (ng != g)
        take = ns & (nm >= 0) & (nd > best)
        best = np.where(take, nd, best)
        pick = np.where(take, nm, pick)
    edge = ~solid & (pick >= 0)
    rgba[edge, :3] = ramps[pick[edge], OUTLINE]
    rgba[edge, 3] = 255
    px["edge"], px["edge_mat"], px["edge_depth"] = edge, pick, best
    return edge


def normal_image(px, flatten=0.35):
    """Tangent-space normal map of a frame (RGBA, alpha as the colour's)."""
    solid, edge, normal = px["solid"], px["edge"], px["normal"].copy()
    # Outline pixels take the normal of the solid pixel they hug, turned outwards.
    acc = np.zeros_like(normal)
    for dy, dx in NEIGH:
        ns = _shift(solid, dy, dx, False)
        nn = _shift(normal, dy, dx, 0.0)
        acc += np.where(ns[..., None], nn, 0) + np.where(ns[..., None], np.array([-dx, dy, 0.0]) * 0.8, 0)
    normal = np.where(edge[..., None], acc, normal)
    normal = normal * (1 - flatten) + np.array([0, 0, 1.0]) * flatten
    normal /= np.maximum(np.linalg.norm(normal, axis=-1), 1e-9)[..., None]
    img = np.zeros(solid.shape + (4,), np.uint8)
    on = solid | edge
    img[on, :3] = np.clip((normal[on] * 0.5 + 0.5) * 255 + 0.5, 0, 255).astype(np.uint8)
    img[~on, :3] = (128, 128, 255)
    img[..., 3] = 255
    return img
