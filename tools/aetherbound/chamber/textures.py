"""The two trim sheets (stone, metal): albedo, normal and emissive images, and
the bands the geometry maps onto. 256 x 256, wrapping in U, 16 px per unit
(the chain and small-prop bands are used at 32).
"""
import math
import numpy as np
from kit import Band
from paint import (STONE, SLATE, WARM, DIRT, BRONZE, VERDIGRIS, GOLD, IRON, AETHER, FIRE, vnoise, fbm, bevel_light,
                   normal_map, tones, save, voronoi, crack)

W = 256

# name: (y0, height)
STONE_BANDS = {"ashlar": (0, 64), "big": (64, 32), "mould": (96, 16), "trim": (112, 16), "flute": (128, 64),
               "rough": (192, 32), "brick": (224, 32)}
METAL_BANDS = {"plate": (0, 32), "pipe": (32, 16), "pipe_v": (48, 16), "tank": (64, 32), "grate": (96, 16),
               "chain": (112, 8), "rod": (120, 8), "gear": (128, 32), "tube": (160, 16), "hot": (176, 16),
               "iron": (192, 32), "gold": (224, 16), "coal": (240, 16)}


def band(name, ppu=16):
    if name in STONE_BANDS:
        return Band("stone", *STONE_BANDS[name], ppu)
    return Band("metal", *METAL_BANDS[name], ppu)


def courses(h, w, course_h, widths, rng, stagger=True):
    """Blocks in running bond: returns (block id, distance to the block's edge)."""
    ident = np.zeros((h, w), int)
    dist = np.zeros((h, w))
    yy, xx = np.mgrid[0:h, 0:w]
    count = 0
    for row, y0 in enumerate(range(0, h, course_h)):
        edges = [0]
        while edges[-1] < w:
            edges.append(edges[-1] + int(rng.choice(widths)))
        edges[-1] = w
        if len(edges) > 2 and edges[-1] - edges[-2] < 10:
            edges.pop(-2)
        offset = (int(rng.integers(4, 20)) if stagger else 0) + (row % 2) * (widths[0] // 2 if stagger else 0)
        x = (np.arange(w) - offset) % w
        which = np.searchsorted(edges, x, side="right") - 1
        left = x - np.array(edges)[which]
        right = np.array(edges)[which + 1] - 1 - x
        rows = slice(y0, y0 + course_h)
        ident[rows] = count + which[None, :]
        vertical = np.minimum(yy[rows] - y0, y0 + course_h - 1 - yy[rows])
        dist[rows] = np.minimum(np.minimum(left, right)[None, :], vertical)
        count += len(edges)
    return ident, dist


def stone_block_band(h, course_h, widths, rng, base=4, rough=1.0, chips=6, cracks=3):
    ident, dist = courses(h, W, course_h, widths, rng)
    per_block = rng.normal(0, 0.55, ident.max() + 1)[ident]
    height = np.clip(dist / 1.6, 0, 1)
    mottle = fbm(h, W, 12, rng, 3) - 0.5
    grain = vnoise(h, W, 3, rng) - 0.5
    height = height * (0.82 + 0.5 * mottle) + 0.05 * grain
    cracked = np.zeros((h, W), bool)
    for _ in range(cracks):
        crack(cracked, rng, rng.uniform(0, h), rng.uniform(0, W), rng.uniform(10, 30), 0.7)
    chipped = np.zeros((h, W), bool)
    for _ in range(chips):
        cy, cx = rng.integers(0, h), rng.integers(0, W)
        yy, xx = np.mgrid[0:h, 0:W]
        blob = (np.abs(yy - cy) + np.abs((xx - cx + W // 2) % W - W // 2) < rng.integers(2, 5)) & (dist < 4)
        chipped |= blob
    height = np.where(cracked, 0.15, height)
    height = np.where(chipped, height * 0.45, height)
    light, _, _ = bevel_light(height)
    tone = base + per_block + rough * 2.2 * mottle + 3.2 * light
    tone = np.where(dist < 1, 1.0 + 0.6 * per_block, tone)  # mortar
    tone = np.where(cracked, 1.0, tone)
    tone = np.where(chipped, tone - 1.0, tone)
    return tone, height


def paint_stone(rng):
    tone = np.zeros((256, W))
    height = np.zeros((256, W))
    palette = np.zeros((256, W), int)  # 0 stone, 1 slate, 2 warm, 3 dirt

    def put(name, t, h, pal=0):
        y0, hh = STONE_BANDS[name]
        tone[y0:y0 + hh], height[y0:y0 + hh], palette[y0:y0 + hh] = t, h, pal

    put("ashlar", *stone_block_band(64, 16, [32, 24, 40, 32], rng, base=3.9))
    put("big", *stone_block_band(32, 32, [48, 40, 56], rng, base=3.6, rough=1.5, chips=10, cracks=4))
    put("brick", *stone_block_band(32, 8, [16, 16, 12, 20], rng, base=3.0, rough=1.3, chips=14, cracks=3), pal=1)

    # Moulding: stacked beads and a cove, with a joint every 64 px.
    profile = np.array([0.2, 0.75, 1.0, 0.95, 0.6, 0.35, 0.3, 0.5, 0.85, 1.0, 1.0, 0.9, 0.55, 0.25, 0.1, 0.0])
    h = np.repeat(profile[:, None], W, axis=1) + 0.06 * (vnoise(16, W, 4, rng) - 0.5)
    joints = (np.arange(W) % 64 == 17)[None, :]
    h = np.where(joints, h * 0.4, h)
    light, _, _ = bevel_light(h)
    t = 4.2 + 3.4 * light + 1.2 * (fbm(16, W, 16, rng) - 0.5)
    t[profile < 0.3] -= 1.2
    put("mould", t, h, 2)

    # Trim: a fascia over dentils.
    h = np.zeros((16, W))
    h[0:7] = 0.9
    h[7] = 0.3
    dent = (np.arange(W) % 8) < 6
    h[8:15] = np.where(dent, 1.0, 0.2)[None, :]
    h[15] = 0.1
    h += 0.05 * (vnoise(16, W, 3, rng) - 0.5)
    light, _, _ = bevel_light(h)
    t = 4.0 + 3.2 * light + 1.2 * (fbm(16, W, 14, rng) - 0.5)
    t[h < 0.35] -= 1.6
    put("trim", t, h, 2)

    # Fluted shaft: grooves 8 px apart, drum joints every 32 px.
    x = np.arange(W) % 8
    groove = np.array([1.0, 0.75, 0.4, 0.15, 0.1, 0.3, 0.65, 1.0])[x]
    h = np.repeat(groove[None, :], 64, axis=0)
    h[[0, 32]] *= 0.3
    h[[1, 33]] *= 0.8
    wear = fbm(64, W, 14, rng) - 0.5
    h += 0.08 * (vnoise(64, W, 3, rng) - 0.5)
    cracked = np.zeros((64, W), bool)
    for _ in range(5):
        crack(cracked, rng, rng.uniform(0, 64), rng.uniform(0, W), rng.uniform(8, 26), 0.5, direction=rng.choice([1.2, 1.9]))
    h = np.where(cracked, 0.1, h)
    light, _, _ = bevel_light(h, wrap_y=True)
    t = 4.3 + 3.0 * light + 1.8 * wear
    t[[0, 32]] = 1.5
    t = np.where(cracked, 1.2, t)
    put("flute", t, h, 0)

    # Rough rubble: irregular stones.
    ident, dist = voronoi(32, W, 46, rng)
    per = rng.normal(0, 0.9, ident.max() + 1)[ident]
    h = np.clip(dist / 2.5, 0, 1) ** 0.7 * (0.75 + 0.25 * rng.random(ident.max() + 1)[ident])
    h += 0.1 * (vnoise(32, W, 3, rng) - 0.5)
    light, _, _ = bevel_light(h, wrap_y=True)
    t = 3.6 + per + 3.6 * light + 1.0 * (fbm(32, W, 10, rng) - 0.5)
    t = np.where(dist < 0.8, 0.8, t)
    put("rough", t, h, 0)

    ramps = np.stack([STONE, SLATE, WARM, DIRT])
    index = np.clip(np.rint(tone), 0, 7).astype(int)
    colour = ramps[palette, index]
    return colour, normal_map(height, 2.4, wrap_x=True)


def paint_metal(rng):
    tone = np.full((256, W), 3.0)
    height = np.full((256, W), 0.5)
    mat = np.zeros((256, W), int)  # 0 bronze 1 verdigris 2 gold 3 iron 4 aether 5 fire
    alpha = np.full((256, W), 255.0)
    emissive = np.zeros((256, W, 3))
    xx = np.arange(W)

    def rows(name):
        y0, hh = METAL_BANDS[name]
        return slice(y0, y0 + hh), hh

    def rivet(y, x):
        x %= W
        height[y, x] = 1.0
        tone[y, x] = 6
        tone[y + 1, x] = 1.5

    # Riveted plates with verdigris running down from the seams.
    r, hh = rows("plate")
    h = np.full((hh, W), 0.7) + 0.2 * (fbm(hh, W, 20, rng) - 0.5)
    seam_x = (xx % 32 == 0)[None, :] | (xx % 32 == 31)[None, :]
    h[:, xx % 32 == 0] = 0.1
    h[0] = 0.15
    h[hh - 1] = 0.3
    stain = fbm(hh, W, 10, rng)
    drip = vnoise(1, W, 5, rng)[0][None, :] * np.linspace(1.15, 0.2, hh)[:, None]
    green = (drip + 0.5 * stain) > 0.86
    light, _, _ = bevel_light(h)
    t = 3.4 + 3.0 * light + 2.0 * (stain - 0.5)
    t[:, xx % 32 == 0] = 0.8
    t[0] = 1.0
    tone[r], height[r] = t, h
    mat[r] = np.where(green, 1, 0)
    for x in range(0, W, 32):
        for y in (3, 12, 21, 28):
            rivet(r.start + y, x + 3)
            rivet(r.start + y, x + 28)
        for dx in (10, 16, 22):
            rivet(r.start + 3, x + dx)

    # Pipes: uniform round the circumference, a flange every 64 px.
    for name, material, period in (("pipe", 0, 64), ("pipe_v", 1, 32)):
        r, hh = rows(name)
        flange = (xx % period) < 5
        edge = ((xx % period) == 5) | ((xx % period) == period - 1)
        h = np.where(flange, 1.0, 0.55)[None, :] * np.ones((hh, 1)) + 0.1 * (fbm(hh, W, 12, rng, tile_y=True) - 0.5)
        light, _, _ = bevel_light(h, wrap_y=True)
        t = 3.6 + 3.0 * light + 1.6 * (fbm(hh, W, 9, rng, tile_y=True) - 0.5)
        t = np.where(flange[None, :], t + 1.4, t)
        t = np.where(edge[None, :], 1.0, t)
        tone[r], height[r] = t, h
        mat[r] = material
        mat[r][:, flange] = 2 if material == 1 else 0
        for x in range(2, W, period):
            for y in range(1, hh, 4):
                rivet(r.start + y, x)

    # Tank plating with one aetherite sight slit per 128 px.
    r, hh = rows("tank")
    h = np.full((hh, W), 0.7) + 0.2 * (fbm(hh, W, 24, rng) - 0.5)
    h[:, xx % 32 == 0] = 0.15
    h[0:3] = 1.0
    h[3] = 0.3
    h[hh - 3:] = 1.0
    h[hh - 4] = 0.3
    stain = fbm(hh, W, 12, rng)
    light, _, _ = bevel_light(h)
    t = 3.2 + 3.0 * light + 2.2 * (stain - 0.5)
    t[:, xx % 32 == 0] = 0.8
    t[0:3] += 1.0
    t[hh - 3:] += 1.0
    tone[r], height[r] = t, h
    mat[r] = np.where(stain > 0.62, 1, 0)
    mat[r][0:3] = 0
    mat[r][hh - 3:] = 0
    tone[r][1] = 6
    tone[r][hh - 2] = 5
    for x in range(0, W, 32):
        for y in range(6, hh - 5, 5):
            rivet(r.start + y, x + 3)
            rivet(r.start + y, x + 29)
    for x in (64, 192):
        ys = slice(r.start + 6, r.start + hh - 6)
        mat[ys, x - 2:x + 2] = 2
        tone[ys, x - 2:x + 2] = 4.0
        tone[ys, x - 2] = 5.5
        tone[ys, x + 1] = 2.5
        core = slice(r.start + 7, r.start + hh - 7)
        mat[core, x - 1:x + 1] = 4
        tone[core, x - 1] = 4
        tone[core, x] = 6
        height[ys, x - 2:x + 2] = 0.2

    # Railing / grating: an alpha cutout.
    r, hh = rows("grate")
    a = np.zeros((hh, W))
    a[0:2] = 255
    a[hh - 2:] = 255
    a[:, (xx % 8) < 2] = 255
    diag = (np.add.outer(np.arange(hh), xx) % 16) < 1
    a[diag] = 255
    alpha[r] = a
    mat[r] = 3
    tone[r] = 4.0 + 1.5 * (vnoise(hh, W, 4, rng) - 0.5)
    tone[r.start] = 5.5
    tone[r.start + 1] = 2.5
    tone[r.start + hh - 2] = 5.0
    height[r] = np.where(a > 0, 1.0, 0.0)

    # Chain (32 px per unit): face-on links alternating with edge-on ones.
    r, hh = rows("chain")
    a = np.zeros((hh, W))
    t = np.full((hh, W), 4.0)
    for x in range(0, W, 16):
        for yy, x0, x1 in ((1, x + 2, x + 8), (6, x + 2, x + 8)):
            a[yy, x0:x1] = 255
        a[2:6, x + 1] = 255
        a[2:6, x + 8] = 255
        t[1, x + 2:x + 8] = 6
        t[6, x + 2:x + 8] = 2.5
        t[2:6, x + 1] = 5
        t[2:6, x + 8] = 3
        a[3:5, x + 7:x + 18] = 255
        t[3, x + 7:x + 18] = 5.5
        t[4, x + 7:x + 18] = 2.5
    alpha[r], tone[r], mat[r] = a, t, 3
    height[r] = np.where(a > 0, 1.0, 0.0)

    # Rod / cable.
    r, hh = rows("rod")
    a = np.zeros((hh, W))
    a[2:6] = 255
    alpha[r], mat[r] = a, 3
    tone[r] = np.array([0, 0, 5.5, 4, 3, 2, 0, 0])[:, None] + 0.8 * (vnoise(hh, W, 6, rng) - 0.5)
    height[r] = np.where(a > 0, 1.0, 0.0)

    # Gear face: concentric bands (V runs outwards from the hub).
    r, hh = rows("gear")
    profile = np.concatenate([[1.0] * 3, [0.3], [0.6] * 9, [0.25], [0.7] * 10, [0.3], [1.0] * 6, [0.5]])
    h = np.repeat(profile[::-1][:, None], W, axis=1) + 0.12 * (fbm(hh, W, 10, rng) - 0.5)
    light, _, _ = bevel_light(h)
    t = 3.3 + 3.2 * light + 2.0 * (fbm(hh, W, 16, rng) - 0.5)
    t[profile[::-1] < 0.35] = 1.0
    tone[r], height[r] = t, h
    mat[r] = np.where(fbm(hh, W, 12, rng) > 0.64, 1, 0)
    mat[r][-3:] = 2
    for x in range(4, W, 16):
        rivet(r.start + 14, x)
        rivet(r.start + 26, x + 8)

    # Glass conduit tube full of aetherite, with a bronze collar every 64 px.
    r, hh = rows("tube")
    collar = (xx % 64) < 8
    flow = fbm(hh, W, 10, rng, tile_y=True)
    t = 4.2 + 3.0 * (flow - 0.5)
    t = np.where(flow > 0.62, 6.0, t)
    tone[r] = np.where(collar[None, :], 4.5, t)
    mat[r] = np.where(collar[None, :], 2, 4)
    tone[r][:, (xx % 64) == 0] = 1.5
    tone[r][:, (xx % 64) == 7] = 1.5
    tone[r][:, (xx % 64) == 1] = 6
    height[r] = np.where(collar[None, :], 1.0, 0.5)

    # Furnace grate: iron bars over fire.
    r, hh = rows("hot")
    bars = (xx % 6) < 2
    fire = fbm(hh, W, 6, rng, tile_y=True)
    t = 2.5 + 5.5 * fire * np.linspace(0.6, 1.25, hh)[:, None]
    tone[r] = np.where(bars[None, :], 2.0, t)
    tone[r][:, (xx % 6) == 0] = 3.5
    mat[r] = np.where(bars[None, :], 3, 5)
    tone[r][0:2] = 3
    mat[r][0:2] = 3
    tone[r][hh - 2:] = 2
    mat[r][hh - 2:] = 3
    height[r] = np.where(bars[None, :], 1.0, 0.2)

    # Dark iron louvres.
    r, hh = rows("iron")
    slat = np.arange(hh) % 8
    h = np.array([0.2, 0.5, 0.7, 0.85, 1.0, 1.0, 0.9, 0.1])[slat][:, None] * np.ones((1, W))
    h[:, xx % 64 < 2] = 1.0
    h += 0.1 * (fbm(hh, W, 12, rng) - 0.5)
    light, _, _ = bevel_light(h)
    t = 3.4 + 3.4 * light + 1.5 * (fbm(hh, W, 14, rng) - 0.5)
    t[slat == 7] = 0.8
    tone[r], height[r], mat[r] = t, h, 3
    for x in range(0, W, 64):
        for y in (3, 11, 19, 27):
            rivet(r.start + y, x + 1)

    # Gold inlay on dark bronze: two 8 px friezes (at 32 px per unit, each fits a quarter-unit step riser).
    r, hh = rows("gold")
    cell = np.zeros((8, 16), bool)
    cell[[0, 7]] = True
    for y, x in ((3, 3), (4, 3), (3, 4), (4, 4), (2, 10), (3, 9), (3, 11), (4, 9), (4, 11), (5, 10), (3, 10), (4, 10)):
        cell[y, x] = True
    other = np.zeros((8, 16), bool)
    other[[0, 7]] = True
    other[2, 1:7] = other[5, 1:7] = True
    other[2:6, 1] = other[2:6, 6] = True
    other[2, 9:15] = other[5, 9:15] = True
    other[2:6, 9] = other[2:6, 14] = True
    inlay = np.tile(np.vstack([cell, other]), (1, W // 16))
    tone[r] = np.where(inlay, 5.0 + 1.5 * (vnoise(hh, W, 5, rng) - 0.5), 2.6 + 1.6 * (fbm(hh, W, 10, rng) - 0.5))
    tone[r][[1, 6, 9, 14]] = 1.0
    mat[r] = np.where(inlay, 2, 0)
    height[r] = np.where(inlay, 1.0, 0.6)
    height[r][[1, 6, 9, 14]] = 0.2

    # Coals: dark lumps over a glowing bed (V runs outwards).
    r, hh = rows("coal")
    ident, dist = voronoi(hh, W, 60, rng)
    heat = fbm(hh, W, 8, rng, tile_y=True)
    lump = dist > 1.2
    t = np.where(lump, 1.0 + 2.0 * heat, 3.0 + 6.0 * heat)
    hot = (~lump) | (heat > 0.66)
    tone[r] = np.where(hot, np.maximum(t, 3.0 + 5.0 * heat), t)
    mat[r] = np.where(hot, 5, 3)
    height[r] = np.where(lump, 0.9, 0.3)

    ramps = np.stack([BRONZE, VERDIGRIS, GOLD, IRON, AETHER, FIRE])
    index = np.clip(np.rint(tone), 0, 7).astype(int)
    colour = ramps[mat, index]
    glow = mat >= 4
    emissive[glow] = colour[glow]
    emissive[(mat == 5) & (index < 3)] *= 0.35
    # Glowing parts keep a dark albedo so that their light is all emission.
    colour = np.where(glow[..., None], colour * 0.35, colour)
    return colour, alpha, normal_map(height, 2.2, wrap_x=True), emissive


def write_trims(folder, seed=7):
    rng = np.random.default_rng(seed)
    colour, normal = paint_stone(rng)
    save(f"{folder}/stone.png", colour)
    save(f"{folder}/stone_n.png", normal)
    colour, alpha, normal, emissive = paint_metal(rng)
    save(f"{folder}/metal.png", colour, alpha)
    save(f"{folder}/metal_n.png", normal)
    save(f"{folder}/metal_e.png", emissive)
