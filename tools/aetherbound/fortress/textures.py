"""Procedurally painted pixel-art textures for the fortress map: tiling stone
(wall courses, flagstones, rock, dressed blocks), the prop atlas (timber,
charred timber with embers, iron, bronze, conduits, cloth, armour, windows)
with normal and emissive maps, mist, the sky backdrop cards and the LUT."""
import math
import os
import numpy as np
from PIL import Image
from pix import (Canvas, rng, value_noise, save, STONE, WARM, COLD, MOSS, WOOD, CHAR, IRON, BRONZE, VERDI, GOLD, RED,
                 BLACKC, FIRE, VIOLET, CREAM, hexrgb)

MORTAR = hexrgb("#1a1822")


def _block(c, r, x0, y0, w, h, pal, tone, mott, crack=False, chips=True, mortar=1):
    """One dressed stone: 1 px of mortar on its top and left, bevel light on
    the top/left edge, dark on the bottom/right, mottled face."""
    W, H = c.w, c.h
    ix0, iy0, iw, ih = x0 + mortar, y0 + mortar, w - mortar, h - mortar
    for yy in range(ih):
        for xx in range(iw):
            X, Y = (ix0 + xx) % W, (iy0 + yy) % H
            t = tone
            m = mott[Y, X]
            if m > 0.66:
                t += 1
            elif m < 0.3:
                t -= 1
            hgt = 1.0
            if yy == 0 or xx == 0:
                t += 1
                hgt = 0.62
            if yy == ih - 1 or xx == iw - 1:
                t -= 1
                hgt = 0.55
            if (yy == 0 and xx == iw - 1) or (yy == ih - 1 and xx == 0):
                t = tone
            c.rgb[Y, X] = pal[int(np.clip(t, 0, len(pal) - 1))]
            c.height[Y, X] = hgt + (m - 0.5) * 0.12
    if chips:
        # knocked-off corners
        for cx, cy, sx, sy in ((0, 0, 1, 1), (iw - 1, 0, -1, 1), (0, ih - 1, 1, -1), (iw - 1, ih - 1, -1, -1)):
            if r.random() < 0.45:
                n = int(r.integers(1, 4))
                for k in range(n):
                    for j in range(n - k):
                        X, Y = (ix0 + cx + sx * k) % W, (iy0 + cy + sy * j) % H
                        c.rgb[Y, X] = MORTAR if (k + j) < n - 1 else pal[max(0, tone - 2)]
                        c.height[Y, X] = 0.1 if (k + j) < n - 1 else 0.4
        # nicks along the edges
        for _ in range(int(r.integers(0, 3))):
            if r.random() < 0.5:
                xx = int(r.integers(1, max(2, iw - 1)))
                X, Y = (ix0 + xx) % W, (iy0 + (0 if r.random() < 0.5 else ih - 1)) % H
            else:
                yy = int(r.integers(1, max(2, ih - 1)))
                X, Y = (ix0 + (0 if r.random() < 0.5 else iw - 1)) % W, (iy0 + yy) % H
            c.rgb[Y, X] = pal[max(0, tone - 2)]
            c.height[Y, X] = 0.3
    if crack and iw > 6 and ih > 5:
        x = ix0 + int(r.integers(2, iw - 2))
        y = iy0
        dx = int(r.choice([-1, 1]))
        for _ in range(ih):
            X, Y = x % W, y % H
            c.rgb[Y, X] = pal[max(0, tone - 3)]
            c.height[Y, X] = 0.15
            if ix0 < x + 1 < ix0 + iw - 1:
                c.rgb[Y, (x + 1) % W] = pal[min(len(pal) - 1, tone + 1)]
            y += 1
            if r.random() < 0.55:
                x += dx
            if r.random() < 0.15:
                dx = -dx
            x = int(np.clip(x, ix0 + 1, ix0 + iw - 2))


def _moss(c, r, seed, amount=0.5, low_only=False):
    n = value_noise(c.w, c.h, 32, seed, 2)
    fine = r.random((c.h, c.w))
    dark = c.height < 0.5
    near = dark | np.roll(dark, 1, 0) | np.roll(dark, -1, 1) | np.roll(dark, 1, 1)
    mask = (n > 1 - amount * 0.55) & near & (fine < 0.8)
    spread = (n > 1 - amount * 0.38) & (fine < 0.35)
    for m, tones in ((mask, (1, 2, 3)), (spread, (2, 3))):
        ys, xs = np.nonzero(m)
        for y, x in zip(ys, xs):
            c.rgb[y, x] = MOSS[int(r.choice(tones))]


def _soot_streaks(c, r, count, pal_dark=0.72):
    for _ in range(count):
        x = int(r.integers(0, c.w))
        y = int(r.integers(0, c.h))
        length = int(r.integers(10, 46))
        width = int(r.integers(1, 4))
        for k in range(length):
            f = 1 - k / length
            for wx in range(width):
                if r.random() < 0.35 + 0.5 * f:
                    X, Y = (x + wx) % c.w, (y + k) % c.h
                    c.rgb[Y, X] *= pal_dark + 0.2 * (1 - f)


def wall(seed=11, size=256):
    c = Canvas(size, size, MORTAR)
    r = rng(seed)
    mott = value_noise(size, size, 4, seed + 1, 2)
    big = value_noise(size, size, 64, seed + 2, 2)
    heights = []
    while sum(heights) < size:
        heights.append(int(r.choice([8, 16, 16, 16, 8, 16])))
    while sum(heights) > size:
        heights[-1] -= sum(heights) - size
        if heights[-1] <= 0:
            heights.pop()
    y = 0
    for h in heights:
        x = int(r.integers(0, 24))
        start = x
        while x < start + size:
            w = int(r.integers(14, 34)) if h >= 12 else int(r.integers(10, 24))
            if start + size - (x + w) < 10:
                w = start + size - x
            bx = x % size
            tone = 4 + int(np.round((big[y % size, bx] - 0.5) * 3)) + int(r.choice([-1, 0, 0, 0, 1]))
            pal = STONE
            u = r.random()
            if u < 0.10:
                pal = WARM
            elif u < 0.2:
                pal = COLD
            _block(c, r, x, y, w, h, pal, int(np.clip(tone, 2, 6)), mott, crack=r.random() < 0.14)
            x += w
        y += h
    _soot_streaks(c, r, 26)
    _moss(c, r, seed + 3, 0.5)
    return c


def floor(seed=23, size=256):
    """Flagstones on an 8 px lattice, worn and cracked."""
    c = Canvas(size, size, MORTAR)
    r = rng(seed)
    mott = value_noise(size, size, 4, seed + 1, 2)
    big = value_noise(size, size, 48, seed + 2, 2)
    n = size // 8
    used = np.zeros((n, n), dtype=bool)
    shapes = [(4, 3), (3, 3), (4, 2), (3, 2), (2, 3), (2, 2), (2, 2), (3, 4), (2, 1), (1, 2), (1, 1)]
    for gy in range(n):
        for gx in range(n):
            if used[gy, gx]:
                continue
            order = list(r.permutation(len(shapes) - 1)) + [len(shapes) - 1]
            for k in order:
                w, h = shapes[k]
                ys = [(gy + j) % n for j in range(h)]
                xs = [(gx + i) % n for i in range(w)]
                if gy + h > n or not used[np.ix_(ys, xs)].any():
                    if gy + h > n:
                        h = n - gy
                        ys = [(gy + j) % n for j in range(h)]
                        if used[np.ix_(ys, xs)].any():
                            continue
                    used[np.ix_(ys, xs)] = True
                    tone = 3 + int(np.round((big[(gy * 8) % size, (gx * 8) % size] - 0.5) * 3)) + int(
                        r.choice([-1, 0, 0, 1]))
                    u = r.random()
                    pal = WARM if u < 0.12 else COLD if u < 0.3 else STONE
                    _block(c, r, gx * 8, gy * 8, w * 8, h * 8, pal, int(np.clip(tone, 2, 5)), mott,
                           crack=r.random() < 0.22)
                    break
    # dirt in the joints, worn dark patches
    dirt = value_noise(size, size, 24, seed + 5, 2)
    joint = c.height < 0.2
    c.rgb[joint & (dirt > 0.55)] = hexrgb("#2a2420")
    wear = value_noise(size, size, 40, seed + 6, 3)
    c.rgb[wear < 0.34] *= 0.84
    _moss(c, r, seed + 3, 0.35)
    return c


def rock(seed=37, size=256, cells=230, stretch=2.7):
    """Fractured rock: stretched Voronoi cells with faceted shading."""
    r = rng(seed)
    pts = r.random((cells, 2)) * size
    tones = r.integers(2, 6, cells)
    yy, xx = np.mgrid[0:size, 0:size].astype(np.float32)
    best = np.full((size, size), 1e9, dtype=np.float32)
    second = np.full((size, size), 1e9, dtype=np.float32)
    idx = np.zeros((size, size), dtype=np.int32)
    for i, (px, py) in enumerate(pts):
        dx = np.abs(xx - px)
        dx = np.minimum(dx, size - dx) / stretch
        dy = np.abs(yy - py)
        dy = np.minimum(dy, size - dy)
        d = dx * dx + dy * dy
        closer = d < best
        second = np.where(closer, best, np.minimum(second, d))
        idx = np.where(closer, i, idx)
        best = np.where(closer, d, best)
    edge = (np.sqrt(second) - np.sqrt(best)) < 1.2
    c = Canvas(size, size)
    mott = value_noise(size, size, 4, seed + 1, 2)
    big = value_noise(size, size, 64, seed + 2, 2)
    # facet light: the upper-left part of each cell is lighter
    cx = pts[idx, 0]
    cy = pts[idx, 1]
    ddx = xx - cx
    ddx = np.where(ddx > size / 2, ddx - size, np.where(ddx < -size / 2, ddx + size, ddx))
    ddy = yy - cy
    ddy = np.where(ddy > size / 2, ddy - size, np.where(ddy < -size / 2, ddy + size, ddy))
    facet = -(ddx * 0.35 + ddy * 1.0)
    t = tones[idx] + np.where(facet > 2.2, 1, 0) + np.where(facet < -2.5, -1, 0) + np.where(mott > 0.7, 1, 0) - \
        np.where(mott < 0.28, 1, 0) + np.round((big - 0.5) * 2.5).astype(int)
    t = np.clip(t, 1, 7)
    pal_pick = (idx * 7919) % 10
    rgb = STONE[t]
    rgb = np.where((pal_pick < 2)[..., None], COLD[t], rgb)
    rgb = np.where((pal_pick == 9)[..., None], WARM[t], rgb)
    c.rgb = rgb.astype(np.float32)
    c.rgb[edge] = MORTAR
    c.height = np.clip(0.75 + facet * 0.02, 0.4, 1.0).astype(np.float32)
    c.height[edge] = 0.0
    _moss(c, r, seed + 3, 0.3)
    return c


def block(seed=41, size=128):
    """Plain dressed stone for individual bevelled blocks: mottled, pitted,
    with faint tooling and hairline cracks."""
    r = rng(seed)
    c = Canvas(size, size)
    mott = value_noise(size, size, 5, seed + 1, 2)
    big = value_noise(size, size, 40, seed + 2, 2)
    t = 4 + np.where(mott > 0.68, 1, 0) - np.where(mott < 0.3, 1, 0) + np.round((big - 0.5) * 2.2).astype(int)
    c.rgb = STONE[np.clip(t, 2, 7)].astype(np.float32)
    c.height[:] = 0.8 + (mott - 0.5) * 0.25
    # tooling: short diagonal ticks
    for _ in range(170):
        x, y = int(r.integers(0, size)), int(r.integers(0, size))
        for k in range(int(r.integers(2, 5))):
            X, Y = (x + k) % size, (y + k // 2) % size
            c.rgb[Y, X] *= 0.86
            c.height[Y, X] -= 0.2
    # pits
    for _ in range(120):
        x, y = int(r.integers(0, size)), int(r.integers(0, size))
        c.rgb[y, x] = STONE[1]
        c.rgb[(y + 1) % size, x] = STONE[6]
        c.height[y, x] = 0.2
    # hairline cracks
    for _ in range(9):
        x, y = int(r.integers(0, size)), int(r.integers(0, size))
        dx = int(r.choice([-1, 1]))
        for k in range(int(r.integers(12, 40))):
            c.rgb[y % size, x % size] = STONE[1]
            c.height[y % size, x % size] = 0.2
            y += 1
            if r.random() < 0.5:
                x += dx
            if r.random() < 0.1:
                dx = -dx
    _moss(c, r, seed + 3, 0.18)
    return c


# ---------------------------------------------------------------------------- prop atlas
# Horizontal strips that tile in U across the full width (512 px = 16 units at
# 32 px per unit). name: (y0, height)
ATLAS = 512
STRIPS = {
    "wood": (0, 64), "char": (64, 64), "iron": (128, 32), "bronze": (160, 64), "conduit": (224, 32),
    "rope": (256, 16), "coals": (272, 48), "cloth": (320, 64), "cloth_dark": (384, 32), "armour": (416, 32),
    "window": (448, 32), "window_violet": (480, 32),
}


def props(seed=53):
    c = Canvas(ATLAS, ATLAS, (20, 18, 20))
    r = rng(seed)
    W = ATLAS
    mott = value_noise(W, W, 3, seed + 1, 2)
    streak = value_noise(W, W, 2, seed + 2, 1)
    long = np.repeat(value_noise(W // 8, W, 1, seed + 3, 1), 8, axis=1)  # grain: stretched along U

    def planks(y0, h, pal, charred=False):
        plank = 16
        for row in range(h // plank):
            py = y0 + row * plank
            base = int(r.integers(3, 6)) if not charred else int(r.integers(2, 5))
            # board ends
            ends = sorted(set(int(v) for v in r.integers(0, W, 3)))
            for y in range(plank):
                for x in range(W):
                    Y = py + y
                    g = long[Y, x]
                    t = base + (1 if g > 0.66 else -1 if g < 0.3 else 0)
                    hgt = 0.8 + (g - 0.5) * 0.2
                    if y == 0:
                        t += 1
                        hgt = 0.55
                    if y == plank - 1:
                        t = 0
                        hgt = 0.0
                    if y == plank - 2:
                        t -= 1
                    if x in ends:
                        t = 0
                        hgt = 0.0
                    c.rgb[Y, x] = pal[int(np.clip(t, 0, len(pal) - 1))]
                    c.height[Y, x] = hgt
            # knots
            for _ in range(5):
                kx, ky = int(r.integers(0, W)), py + int(r.integers(4, plank - 5))
                for dx, dy in ((0, 0), (1, 0), (0, 1), (1, 1)):
                    c.rgb[ky + dy, (kx + dx) % W] = pal[1]
                for dx, dy in ((-1, 0), (2, 0), (-1, 1), (2, 1), (0, -1), (1, -1), (0, 2), (1, 2)):
                    c.rgb[ky + dy, (kx + dx) % W] = pal[min(len(pal) - 1, base + 1)]
            # nail heads near board ends
            for e in ends:
                for off in (-3, 3):
                    c.rgb[py + 4, (e + off) % W] = IRON[6]
                    c.rgb[py + 11, (e + off) % W] = IRON[6]
                    c.rgb[py + 5, (e + off) % W] = IRON[1]
                    c.rgb[py + 12, (e + off) % W] = IRON[1]

    y0, h = STRIPS["wood"]
    planks(y0, h, WOOD)
    y0, h = STRIPS["char"]
    planks(y0, h, CHAR, charred=True)
    # alligatored char: glowing ember cracks
    ember = value_noise(W, h, 6, seed + 9, 2)
    cracks = value_noise(W, h, 3, seed + 10, 1)
    for y in range(h):
        for x in range(W):
            e = ember[y, x]
            if abs(cracks[y, x] - 0.5) < 0.035 and e > 0.42:
                heat = min(1.0, (e - 0.42) * 4)
                k = int(np.clip(1 + heat * 4.5, 1, 5))
                c.rgb[y0 + y, x] = FIRE[max(0, k - 1)]
                c.emit[y0 + y, x] = FIRE[k]
                c.height[y0 + y, x] = 0.2
            elif e > 0.7 and r.random() < 0.3:
                c.emit[y0 + y, x] = FIRE[1] * 0.6
                c.rgb[y0 + y, x] = FIRE[0]
            elif mott[y0 + y, x] > 0.75:
                c.rgb[y0 + y, x] = hexrgb("#5a5654")  # ash flecks

    # iron: band with rivets, plates
    y0, h = STRIPS["iron"]
    for y in range(h):
        for x in range(W):
            m = mott[y0 + y, x]
            t = 3 + (1 if m > 0.7 else -1 if m < 0.3 else 0)
            hgt = 0.8
            if y in (0, 16):
                t += 2
                hgt = 0.6
            if y in (15, h - 1):
                t = 0
                hgt = 0.0
            if x % 64 == 0:
                t = 0
                hgt = 0
            if x % 64 == 1:
                t += 1
            c.rgb[y0 + y, x] = IRON[int(np.clip(t, 0, 7))]
            c.height[y0 + y, x] = hgt
    for x in range(6, W, 16):
        for yy in (4, 10, 20, 26):
            c.rgb[y0 + yy, x] = IRON[6]
            c.rgb[y0 + yy, x + 1] = IRON[5]
            c.rgb[y0 + yy + 1, x] = IRON[4]
            c.rgb[y0 + yy + 1, x + 1] = IRON[0]
            c.height[y0 + yy:y0 + yy + 2, x:x + 2] = 1.0
    # rust blotches
    rust = value_noise(W, h, 10, seed + 12, 2)
    for y in range(h):
        for x in range(W):
            if rust[y, x] > 0.68 and c.height[y0 + y, x] > 0.5 and r.random() < 0.7:
                c.rgb[y0 + y, x] = hexrgb("#4a2c1e") if rust[y, x] < 0.78 else hexrgb("#6b3a22")

    # bronze plates with gold inlay and verdigris
    y0, h = STRIPS["bronze"]
    verd = value_noise(W, h, 12, seed + 14, 2)
    for y in range(h):
        for x in range(W):
            m = mott[y0 + y, x]
            t = 3 + (1 if m > 0.68 else -1 if m < 0.3 else 0)
            hgt = 0.8
            px, py = x % 64, y % 32
            if px == 0 or py == 0:
                t = 0
                hgt = 0
            elif px == 1 or py == 1:
                t += 2
                hgt = 0.6
            elif px == 63 or py == 31:
                t -= 2
                hgt = 0.5
            col = BRONZE[int(np.clip(t, 0, 6))]
            if hgt > 0.55 and verd[y, x] > 0.6 and r.random() < 0.85:
                col = VERDI[int(np.clip(1 + (verd[y, x] - 0.6) * 12, 0, 4))]
            # inlay: a diamond line per plate
            d = abs(px - 32) / 2 + abs(py - 16)
            if 9 <= d < 10 and hgt > 0.55:
                col = GOLD[2 if (x + y) % 5 else 3]
                hgt = 0.95
            if d < 2 and hgt > 0.55:
                col = GOLD[3]
            c.rgb[y0 + y, x] = col
            c.height[y0 + y, x] = hgt
    for x in range(4, W, 64):
        for dx in (0, 54):
            for yy in (4, 26, 36, 58):
                c.rgb[y0 + yy, x + dx] = GOLD[3]
                c.rgb[y0 + yy + 1, x + dx] = BRONZE[1]

    # conduit: bronze pipe, violet core seen through slits; emissive
    y0, h = STRIPS["conduit"]
    for y in range(h):
        for x in range(W):
            m = mott[y0 + y, x]
            # rounded pipe shading across V
            v = (y + 0.5) / h
            shade = math.sin(v * math.pi)
            t = int(np.clip(1 + shade * 4.5 + (0.6 if m > 0.7 else 0), 0, 6))
            col = BRONZE[t]
            hgt = 0.3 + shade * 0.7
            if x % 32 in (0, 1):  # collar
                col = BRONZE[min(6, t + 1)] if x % 32 == 1 else BRONZE[0]
                hgt = min(1.0, hgt + 0.2)
            core = abs(y - h / 2 + 0.5)
            if core < 5 and x % 32 > 4 and x % 32 < 29:
                k = 5 if core < 1.5 else 4 if core < 2.5 else 3 if core < 4 else 2
                pulse = 0.75 + 0.25 * math.sin(x / 32 * math.pi)
                col = VIOLET[k]
                c.emit[y0 + y, x] = VIOLET[min(k, 4)] * pulse * (0.75 if k < 5 else 1.0)
                hgt = 0.5
            c.rgb[y0 + y, x] = col
            c.height[y0 + y, x] = hgt

    # rope / chain
    y0, h = STRIPS["rope"]
    for y in range(h):
        for x in range(W):
            if y < 8:  # rope: twisted strands
                s = (x + y * 2) % 6
                t = 5 if s < 2 else 3 if s < 4 else 2
                if y in (0, 7):
                    t = 1
                c.rgb[y0 + y, x] = WOOD[t + 1] * np.array([1.0, 1.0, 0.85])
                c.height[y0 + y, x] = 0.4 + 0.5 * math.sin((y + 0.5) / 8 * math.pi)
            else:  # chain links
                yy = y - 8
                link = x % 12
                on = (2 <= yy <= 5 and link < 8 and (yy in (2, 5) or link in (0, 7))) or (yy in (3, 4) and link >= 6)
                c.rgb[y0 + y, x] = IRON[5 if (on and yy == 2) else 3 if on else 0]
                c.a[y0 + y, x] = 255
                c.height[y0 + y, x] = 0.9 if on else 0.0

    # coals: dark lumps with glowing gaps
    y0, h = STRIPS["coals"]
    heat = value_noise(W, h, 5, seed + 20, 2)
    lumps = value_noise(W, h, 3, seed + 21, 1)
    for y in range(h):
        for x in range(W):
            hv = heat[y, x]
            lv = lumps[y, x]
            if lv > 0.52:
                t = 1 if lv < 0.62 else 2 if lv < 0.75 else 3
                c.rgb[y0 + y, x] = CHAR[t + 1]
                c.height[y0 + y, x] = 0.6 + lv * 0.4
                if hv > 0.7:
                    c.emit[y0 + y, x] = FIRE[1]
                    c.rgb[y0 + y, x] = FIRE[0]
            else:
                k = int(np.clip(2 + hv * 4.5, 2, 6))
                c.rgb[y0 + y, x] = FIRE[k]
                c.emit[y0 + y, x] = FIRE[k]
                c.height[y0 + y, x] = 0.2

    # cloth: rust-red banner, black border, gold emblem every 128 px; weave
    y0, h = STRIPS["cloth"]
    fold = value_noise(W, h, 16, seed + 30, 2)
    for y in range(h):
        for x in range(W):
            t = 3 + (1 if fold[y, x] > 0.62 else -1 if fold[y, x] < 0.38 else 0)
            if (x + y) % 2 == 0 and mott[y0 + y, x] > 0.6:
                t -= 1
            col = RED[int(np.clip(t, 0, 6))]
            if y < 5 or y >= h - 5:
                col = BLACKC[2 if y in (4, h - 5) else 1]
            if y in (6, h - 7):
                col = GOLD[1]
            c.rgb[y0 + y, x] = col
            c.height[y0 + y, x] = 0.5 + (fold[y, x] - 0.5) * 0.8
    # emblem: a tower over a crystal diamond, drawn sideways (U runs down the banner)
    emblem = [
        "....#....",
        "...###...",
        "..##.##..",
        ".##...##.",
        "..##.##..",
        "...###...",
        "....#....",
        ".........",
        "#.#.#.#.#",
        "#########",
        ".#######.",
        ".##.#.##.",
        ".#######.",
        ".##.#.##.",
        ".#######.",
        "#########",
    ]
    for ex in range(48, W, 128):
        for j, rowp in enumerate(emblem):       # j runs along U (down the banner)
            for i, ch in enumerate(rowp):       # i runs across V
                if ch == "#":
                    for sx in range(2):
                        for sy in range(2):
                            X = ex + j * 2 + sx
                            Y = y0 + h // 2 - 9 + i * 2 + sy
                            c.rgb[Y, X % W] = GOLD[2 if (i + j) % 3 else 3]
    # scorch holes drawn dark (geometry does the tatters)
    y0d, hd = STRIPS["cloth_dark"]
    for y in range(hd):
        for x in range(W):
            t = 2 + (1 if fold[y, x] > 0.62 else -1 if fold[y, x] < 0.38 else 0)
            col = CREAM[int(np.clip(t - 1 + (1 if (x // 3 + y) % 9 == 0 else 0), 0, 4))] * 0.62
            if x % 48 == 0 or y in (0, hd - 1):
                col = CREAM[0] * 0.5
            c.rgb[y0d + y, x] = col
            c.height[y0d + y, x] = 0.5 + (fold[y, x] - 0.5) * 0.8

    # armour: rust-red plate | black plate | helm with visor slit | skin-less dark cloth
    y0, h = STRIPS["armour"]
    for y in range(h):
        for x in range(W):
            seg = x // 128
            m = mott[y0 + y, x]
            pal = (RED, BLACKC, IRON, BLACKC)[seg]
            base = (4, 2, 3, 1)[seg]
            t = base + (1 if m > 0.7 else -1 if m < 0.3 else 0)
            px = x % 32
            hgt = 0.8
            if px == 0 or y == 0:
                t -= 2
                hgt = 0.2
            if px == 1 or y == 1:
                t += 1
            if seg == 2 and 12 <= y <= 14 and 6 < px < 26:
                t = 0
                hgt = 0.1
            if seg == 0 and y in (h - 4, h - 3):
                c.rgb[y0 + y, x] = GOLD[1]
                continue
            c.rgb[y0 + y, x] = pal[int(np.clip(t, 0, len(pal) - 1))]
            c.height[y0 + y, x] = hgt

    # windows: stone surround, glowing slit (emissive), period 32
    for name, palette, core in (("window", FIRE, 4), ("window_violet", VIOLET, 4)):
        y0, h = STRIPS[name]
        for y in range(h):
            for x in range(W):
                px = x % 32
                m = mott[y0 + y, x]
                t = 3 + (1 if m > 0.7 else -1 if m < 0.3 else 0)
                col = STONE[t]
                hgt = 0.9
                inside = 11 <= px <= 20 and 3 <= y <= 28 and not (y < 6 and (px < 13 or px > 18)) and not (
                    y < 4 and (px < 14 or px > 17))
                frame = 9 <= px <= 22 and 1 <= y <= 30
                if frame and not inside:
                    col = STONE[6 if px == 9 or y == 1 else 2]
                    hgt = 1.0
                if inside:
                    g = 1 - abs(px - 15.5) / 6
                    k = int(np.clip(core - 2 + g * 3 + (0.5 - y / h) * 1.5, 1, 6))
                    if px in (15, 16) or y in (14, 15):  # mullions
                        col = hexrgb("#120c0c")
                        hgt = 0.6
                    else:
                        col = palette[k]
                        c.emit[y0 + y, x] = palette[min(k, 4)] * 0.9
                        hgt = 0.2
                c.rgb[y0 + y, x] = col
                c.height[y0 + y, x] = hgt
    return c


def mist(seed=61, size=256):
    """Tiling mist for the chasm's fog sheets: soft banks with clear gaps,
    alpha in a few flat bands so it reads as painted pixels."""
    n = value_noise(size, size, 64, seed, 4)
    n2 = value_noise(size, size, 128, seed + 1, 2)
    d = np.clip((n * 0.6 + n2 * 0.4 - 0.4) * 3.2, 0, 1)
    lvl = np.round(d * 5) / 5
    c = Canvas(size, size, (96, 106, 150))
    c.rgb[lvl > 0.5] = (116, 126, 172)
    c.rgb[lvl > 0.75] = (140, 150, 196)
    c.a = (lvl * 150).astype(np.float32)
    return c


# ---------------------------------------------------------------------------- sky
def _stamp(c, mask, color, x0=0, y0=0):
    h, w = mask.shape
    for y in range(h):
        for x in range(w):
            if mask[y, x]:
                X, Y = x0 + x, y0 + y
                if 0 <= X < c.w and 0 <= Y < c.h:
                    c.rgb[Y, X] = color
                    c.a[Y, X] = 255


def backdrop_ridge(seed=71, w=1024, h=192):
    """Far mountain ridges in three hazy layers, a dull red glow of distant
    fires in the valleys between them. Transparent above."""
    c = Canvas(w, h, (0, 0, 0), alpha=0)
    layers = ((hexrgb("#1d2148"), 70, 60, 110, 0.0), (hexrgb("#15183a"), 104, 46, 70, 0.55),
              (hexrgb("#0d0f26"), 138, 34, 46, 0.25))
    for layer, (col, base, amp, cell, glow) in enumerate(layers):
        n = value_noise(w, 1, cell, seed + layer, 3)[0]
        n2 = value_noise(w, 1, 7, seed + 10 + layer, 1)[0]
        g = value_noise(w, 1, 90, seed + 20 + layer, 2)[0]
        top = (base - n * amp - n2 * 4).astype(int)
        for x in range(w):
            t = max(0, top[x])
            d = np.arange(h - t)
            k = np.repeat(col[None, :], h - t, 0)
            rim = np.clip(1 - d / 3.0, 0, 1)[:, None]
            k = k * (1 + 0.12 * rim)
            if glow > 0:
                # ember light climbing the slope from the valley below
                f = np.clip((d - 8) / 40.0, 0, 1)[:, None] * glow * np.clip((g[x] - 0.35) * 2.2, 0, 1)
                f = np.round(f * 6) / 6
                k = k * (1 - f) + hexrgb("#8a2a14")[None, :] * f
            c.rgb[t:, x] = k
            c.a[t:, x] = 255
    return _fade_edges(c, 0.05)


def _fade_edges(c, margin=0.07):
    """Dithers the card's left and right ends away so its edges never show."""
    bayer = np.array([[0, 8, 2, 10], [12, 4, 14, 6], [3, 11, 1, 9], [15, 7, 13, 5]]) / 16.0
    m = int(c.w * margin)
    yy, xx = np.mgrid[0:c.h, 0:c.w]
    edge = np.minimum(xx, c.w - 1 - xx) / max(1, m)
    keep = edge > bayer[yy % 4, xx % 4]
    c.a = np.where(keep, c.a, 0).astype(np.float32)
    return c


def backdrop_skyline(seed=83, w=1536, h=368):
    """The burning outer ward beyond the walls: two layers of broken walls,
    towers and roofs in silhouette, small fires with dithered glows, lit
    windows, thin smoke. Transparent sky."""
    r = rng(seed)
    c = Canvas(w, h, (0, 0, 0), alpha=0)
    fires = []
    for layer, (dark, mid, base_y, scale) in enumerate(((hexrgb("#141634"), hexrgb("#1b1d40"), h - 166, 0.7),
                                                        (hexrgb("#090a18"), hexrgb("#10112a"), h - 124, 1.0))):
        gl = (base_y - value_noise(w, 1, 120, seed + layer, 2)[0] * 26).astype(int)
        for x in range(w):
            c.rgb[gl[x]:, x] = dark
            c.a[gl[x]:, x] = 255
        x = int(r.integers(0, 20))
        while x < w - 10:
            kind = r.choice(["tower", "wall", "house", "gap", "wall", "house", "spire", "house", "gap"])
            if kind == "gap":
                x += int(r.integers(8, 50))
                continue
            bw = {"tower": int(r.integers(14, 26)), "wall": int(r.integers(40, 110)), "house": int(r.integers(16, 34)),
                  "spire": int(r.integers(8, 13))}[kind]
            bh = {"tower": int(r.integers(44, 90)), "wall": int(r.integers(16, 28)), "house": int(r.integers(18, 38)),
                  "spire": int(r.integers(70, 120))}[kind]
            bw, bh = int(bw * scale), int(bh * scale)
            base = int(gl[min(w - 1, x + bw // 2)])
            top = base - bh
            broken = r.random() < 0.55
            slope = r.uniform(-0.5, 0.5)
            for xx in range(x, min(w, x + bw)):
                t = top
                if broken:
                    t += int(abs(math.sin(xx * 0.8 + seed)) * 4 + abs((xx - x) * slope))
                if kind in ("tower", "wall") and (xx - x) % 6 < 3:
                    t -= 3
                if kind == "house":
                    t = top + abs((xx - x) - bw // 2)
                if kind == "spire":
                    t = top + abs((xx - x) - bw // 2) * 9
                t = max(0, min(t, base))
                c.rgb[t:base + 6, xx] = mid if (xx - x) < bw * 0.3 else dark
                c.a[t:base + 6, xx] = 255
            if kind in ("tower", "house", "spire") and bh > 20:
                for _ in range(int(r.integers(1, 4))):
                    wx = x + int(r.integers(2, max(3, bw - 2)))
                    wy = top + int(r.integers(10, max(11, bh - 6)))
                    if 0 <= wx < w and 0 <= wy < h - 2 and c.a[wy, wx] > 0:
                        col = FIRE[int(r.integers(3, 6))] * (0.6 if layer == 0 else 1.0)
                        c.rgb[wy:wy + 2, wx] = col
            if r.random() < (0.3 if layer == 0 else 0.42):
                fires.append((x + bw // 2, top + int(r.integers(0, 6)), int(r.integers(3, 8)), layer))
            x += bw + int(r.integers(0, 14))
    bayer = np.array([[0, 8, 2, 10], [12, 4, 14, 6], [3, 11, 1, 9], [15, 7, 13, 5]]) / 16.0
    for fx, fy, rad, layer in fires:
        R = rad * 5
        for y in range(max(0, fy - R), min(h, fy + R)):
            for xx in range(max(0, fx - R), min(w, fx + R)):
                d = math.hypot(xx - fx, (y - fy) * 1.1) / R
                if d >= 1:
                    continue
                f = (1 - d) ** 2 * (0.8 if layer else 0.5)
                if c.a[y, xx] > 0:
                    # light on the masonry
                    c.rgb[y, xx] = c.rgb[y, xx] * (1 - f) + FIRE[2] * f
                elif f * 0.9 > bayer[y % 4, xx % 4]:
                    # dithered glow in the air
                    c.rgb[y, xx] = FIRE[1] * 0.9
                    c.a[y, xx] = 150
        for k in range(rad):
            tx = fx + int(r.integers(-rad // 2, rad // 2 + 1))
            th = int(r.integers(rad // 2 + 1, rad + 4))
            for j in range(th):
                X = tx + int(math.sin(j * 0.8 + k) * 1.2)
                Y = fy - j + rad // 2
                if 0 <= X < w and 0 <= Y < h:
                    f = 1 - j / th
                    c.rgb[Y, X] = FIRE[int(np.clip(1 + f * 5, 1, 5))]
                    c.a[Y, X] = 255
    return _fade_edges(c)


def backdrop_smoke(seed=97, w=512, h=512):
    """Smoke columns leaning east, lit from below; dithered alpha."""
    c = Canvas(w, h, (0, 0, 0), alpha=0)
    n = value_noise(w, h, 56, seed, 4)
    n2 = value_noise(w, h, 14, seed + 1, 2)
    yy, xx = np.mgrid[0:h, 0:w]
    dens = np.zeros((h, w), dtype=np.float32)
    up = (h - yy) / h
    for base_x, width, lean in ((90, 44, 0.5), (230, 62, 0.35), (370, 40, 0.6), (455, 34, 0.3)):
        cx = base_x + lean * (h - yy) * 0.35 + np.sin(yy * 0.03 + base_x) * 12
        wd = width * (0.4 + up * 1.7)
        d = np.abs(xx - cx) / wd
        dens += np.clip(1 - d, 0, 1) * np.clip(1.1 - up * 0.8, 0, 1)
    dens = dens * (0.5 + n * 1.0) - (n2 - 0.5) * 0.3
    lvl = np.clip(np.round(dens * 5), 0, 5).astype(int)
    cols = np.stack([hexrgb(v) for v in ("#000000", "#0f0e20", "#17142a", "#201a33", "#2b2038", "#38263c")])
    warm = np.stack([hexrgb(v) for v in ("#000000", "#1c1020", "#2c1622", "#421c1e", "#5c261a", "#783418")])
    f = np.clip(1 - up * 3.0, 0, 1)[..., None]
    c.rgb = (cols[lvl] * (1 - f) + warm[lvl] * f).astype(np.float32)
    bayer = np.array([[0, 8, 2, 10], [12, 4, 14, 6], [3, 11, 1, 9], [15, 7, 13, 5]]) / 16.0
    thr = bayer[yy % 4, xx % 4]
    a = np.clip(dens * 1.3, 0, 1) * np.clip(1.3 - up * 1.1, 0, 1)
    c.a = np.where((lvl > 0) & (a > thr * 0.9), 235, 0).astype(np.float32)
    return c


def backdrop_glow(w=256, h=256):
    """The crystal's violet halo with faint rays; alpha, drawn behind the tower."""
    c = Canvas(w, h, (0, 0, 0), alpha=0)
    yy, xx = np.mgrid[0:h, 0:w].astype(np.float32)
    dx, dy = (xx - w / 2) / (w / 2), (yy - h * 0.5) / (h / 2)
    d = np.sqrt(dx * dx + dy * dy)
    ang = np.arctan2(dy, dx)
    rays = 0.5 + 0.5 * np.sin(ang * 11) * np.sin(ang * 7 + 1.3)
    g = np.clip(1 - d, 0, 1)
    v = g ** 2.4 * (0.82 + 0.18 * rays) + np.clip(1 - d * 3.4, 0, 1) ** 2 * 0.8
    v = np.clip(v, 0, 1)
    col = VIOLET[2][None, None, :] * (1 - v[..., None]) + VIOLET[4][None, None, :] * v[..., None]
    c.rgb = col.astype(np.float32)
    c.a = (np.clip(v * 1.1, 0, 1) * 255).astype(np.float32)
    return c


def lut(path, size=16):
    """Night-battle grade: cool, slightly crushed shadows pushed to indigo,
    warm highlights kept hot, a little extra saturation, soft toe."""
    lines = ["TITLE \"aetherbound fortress night\"", f"LUT_3D_SIZE {size}"]
    for b in range(size):
        for g in range(size):
            for r_ in range(size):
                c = np.array([r_, g, b], dtype=np.float64) / (size - 1)
                luma = float(c @ np.array([0.2126, 0.7152, 0.0722]))
                # split tone: shadows to indigo, highlights to amber
                shadow = (1 - luma) ** 2
                high = luma ** 2
                c = c + shadow * np.array([-0.004, 0.0, 0.012]) + high * np.array([0.03, 0.012, -0.03])
                # saturation
                c = luma + (c - luma) * 1.05
                # gentle S curve
                c = np.clip(c, 0, 1)
                c = c * c * (3 - 2 * c) * 0.35 + c * 0.65
                # lift blacks a touch into blue so nothing is dead black
                c = c * 0.99 + np.array([0.004, 0.005, 0.009])
                c = np.clip(c, 0, 1)
                lines.append(f"{c[0]:.5f} {c[1]:.5f} {c[2]:.5f}")
    with open(path, "w") as f:
        f.write("\n".join(lines) + "\n")


def build(out):
    os.makedirs(out, exist_ok=True)
    made = {}
    for name, fn, strength in (("wall", wall, 2.2), ("floor", floor, 2.0), ("rock", rock, 2.4), ("block", block, 1.6)):
        c = fn()
        save(c.image().convert("RGB"), os.path.join(out, f"{name}.png"), 48)
        save(c.normal_image(strength), os.path.join(out, f"{name}_n.png"))
        made[name] = c
    c = props()
    save(c.image().convert("RGB"), os.path.join(out, "props.png"), 128)
    save(c.normal_image(2.0), os.path.join(out, "props_n.png"))
    save(c.emissive_image(), os.path.join(out, "props_e.png"), 48)
    save(mist().image(), os.path.join(out, "mist.png"))
    save(backdrop_ridge().image(), os.path.join(out, "sky_ridge.png"))
    save(backdrop_skyline().image(), os.path.join(out, "sky_skyline.png"))
    save(backdrop_smoke().image(), os.path.join(out, "sky_smoke.png"))
    save(backdrop_glow().image(), os.path.join(out, "sky_glow.png"))
    lut(os.path.join(out, "grade.cube"))
    return made


if __name__ == "__main__":
    import sys
    build(sys.argv[1] if len(sys.argv) > 1 else "/tmp/fortress_tex")
