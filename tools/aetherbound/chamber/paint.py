"""Pixel-art painting helpers: tileable noise, palette ramps, bevel light from
a height field, normal maps. Everything is numpy; images are written as PNG.
"""
import math
import numpy as np
from PIL import Image


def rgb(hex_colour):
    h = hex_colour.lstrip("#")
    return np.array([int(h[i:i + 2], 16) for i in (0, 2, 4)], np.float64)


def ramp(*colours):
    return np.stack([rgb(c) for c in colours])


# Palette of the night (SPEC.md), as tone ramps from dark to light.
STONE = ramp("#16141f", "#25232f", "#383544", "#4d4959", "#66616f", "#817b87", "#9c96a0", "#b8b2b9")
SLATE = ramp("#131620", "#202533", "#313849", "#454d61", "#5b6479", "#747d92", "#8f97aa", "#abb2c1")
WARM = ramp("#19141a", "#2a2228", "#40353a", "#584a4d", "#736263", "#8f7c7a", "#aa9792", "#c4b3ab")
DIRT = ramp("#0d0c12", "#17151b", "#221f25", "#2f2a2f", "#3e373a", "#4f4747", "#625a57", "#777069")
BRONZE = ramp("#130e0a", "#211812", "#33261b", "#4a3b2c", "#65513a", "#83694a", "#a3875c", "#c4a873")
VERDIGRIS = ramp("#0e1f1c", "#16302b", "#22443b", "#32594d", "#3f6b5e", "#578a77", "#78a892", "#9cc4ad")
GOLD = ramp("#3d2c10", "#5c431a", "#7d5f25", "#a07f33", "#c9a24a", "#e3c268", "#f5dd8e", "#fff2c0")
IRON = ramp("#08090d", "#0f1016", "#181a23", "#232632", "#303443", "#414657", "#565c6e", "#6f7587")
AETHER = ramp("#1f0e3a", "#3a1a66", "#5a2bb0", "#7a44e0", "#9b5cff", "#bd92ff", "#e2ccff", "#ffffff")
FIRE = ramp("#3d0c02", "#7a1e05", "#b8380a", "#e85d12", "#ff7a1e", "#ffa53d", "#ffd27a", "#fff3c4")
ASH = ramp("#101119", "#1a1c27", "#262936", "#353949", "#484d5f", "#5f6577", "#7a8092", "#989eae")


def srgb_to_linear(c):
    c = np.asarray(c, np.float64) / 255.0
    return np.where(c <= 0.04045, c / 12.92, ((c + 0.055) / 1.055) ** 2.4)


def linear(hex_colour, scale=1.0):
    return [round(float(v) * scale, 4) for v in srgb_to_linear(rgb(hex_colour))]


def vnoise(h, w, cell, rng, tile_x=True, tile_y=True):
    """Smooth value noise in 0..1 with features `cell` pixels across."""
    gw, gh = max(1, int(round(w / cell))), max(1, int(round(h / cell)))
    grid = rng.random((gh + (0 if tile_y else 1), gw + (0 if tile_x else 1)))
    ys = (np.arange(h) + 0.5) / h * gh
    xs = (np.arange(w) + 0.5) / w * gw
    y0, x0 = np.floor(ys).astype(int), np.floor(xs).astype(int)
    fy, fx = ys - y0, xs - x0
    fy, fx = fy * fy * (3 - 2 * fy), fx * fx * (3 - 2 * fx)
    ya, yb = (y0 % grid.shape[0], (y0 + 1) % grid.shape[0]) if tile_y else (y0, np.minimum(y0 + 1, grid.shape[0] - 1))
    xa, xb = (x0 % grid.shape[1], (x0 + 1) % grid.shape[1]) if tile_x else (x0, np.minimum(x0 + 1, grid.shape[1] - 1))
    top = grid[np.ix_(ya, xa)] * (1 - fx) + grid[np.ix_(ya, xb)] * fx
    bottom = grid[np.ix_(yb, xa)] * (1 - fx) + grid[np.ix_(yb, xb)] * fx
    return top * (1 - fy[:, None]) + bottom * fy[:, None]


def fbm(h, w, cell, rng, octaves=3, **kw):
    total, amplitude, norm = np.zeros((h, w)), 1.0, 0.0
    for _ in range(octaves):
        total += amplitude * vnoise(h, w, max(1.5, cell), rng, **kw)
        norm += amplitude
        amplitude *= 0.5
        cell /= 2
    return total / norm


def bevel_light(height, wrap_x=True, wrap_y=False):
    """Light from the top-left across a height field: positive on faces turned to it."""
    def shift(a, dy, dx):
        out = np.roll(a, (dy, dx), axis=(0, 1))
        if not wrap_y:
            if dy > 0:
                out[:dy] = a[:1]
            elif dy < 0:
                out[dy:] = a[-1:]
        if not wrap_x:
            if dx > 0:
                out[:, :dx] = a[:, :1]
            elif dx < 0:
                out[:, dx:] = a[:, -1:]
        return out
    dx = shift(height, 0, -1) - shift(height, 0, 1)
    dy = shift(height, -1, 0) - shift(height, 1, 0)
    return (dx + dy) * 0.5, dx * 0.5, dy * 0.5


def normal_map(height, strength=2.0, wrap_x=True, wrap_y=False):
    _, dx, dy = bevel_light(height, wrap_x, wrap_y)
    n = np.stack([-dx * strength, dy * strength, np.ones_like(height)], axis=-1)
    n /= np.linalg.norm(n, axis=-1, keepdims=True)
    return np.clip((n * 0.5 + 0.5) * 255 + 0.5, 0, 255).astype(np.uint8)


def tones(index, colours):
    """Maps a float tone field onto a ramp (hard clusters, no blending)."""
    i = np.clip(np.rint(index), 0, len(colours) - 1).astype(int)
    return colours[i]


def save(path, array, alpha=None):
    array = np.clip(np.rint(array), 0, 255).astype(np.uint8)
    if alpha is not None:
        array = np.dstack([array, np.clip(np.rint(alpha), 0, 255).astype(np.uint8)])
    Image.fromarray(array).save(path)


def voronoi(h, w, count, rng, tile_x=True, tile_y=True):
    """Cells of irregular stones: returns (cell id, distance to the cell's edge in pixels)."""
    pts = rng.random((count, 2)) * [h, w]
    yy, xx = np.mgrid[0:h, 0:w] + 0.5
    best = np.full((h, w), 1e9)
    second = np.full((h, w), 1e9)
    ident = np.zeros((h, w), int)
    for i, (py, px) in enumerate(pts):
        dy = np.abs(yy - py)
        dx = np.abs(xx - px)
        if tile_y:
            dy = np.minimum(dy, h - dy)
        if tile_x:
            dx = np.minimum(dx, w - dx)
        d = np.hypot(dy, dx * 0.85)
        closer = d < best
        second = np.where(closer, best, np.minimum(second, d))
        ident = np.where(closer, i, ident)
        best = np.where(closer, d, best)
    return ident, (second - best) * 0.5


def crack(mask, rng, y, x, length, wander=0.6, direction=None, wrap_x=True):
    """Draws a wandering one-pixel crack into a boolean mask; returns its points."""
    h, w = mask.shape
    a = rng.uniform(0, 2 * math.pi) if direction is None else direction
    points = []
    for _ in range(int(length)):
        xi, yi = int(round(x)), int(round(y))
        if wrap_x:
            xi %= w
        if not (0 <= yi < h and 0 <= xi < w):
            break
        mask[yi, xi] = True
        points.append((yi, xi))
        a += rng.normal(0, wander) * 0.5
        if direction is not None:
            a += (direction - a) * 0.15
        x += math.cos(a)
        y += math.sin(a)
    return points


def write_cube(path, fn, size=17, title="grade"):
    """Writes a .cube 3D LUT; fn maps an (n, 3) array of sRGB 0..1 to graded sRGB 0..1."""
    axis = np.linspace(0, 1, size)
    b, g, r = np.meshgrid(axis, axis, axis, indexing="ij")
    table = np.clip(fn(np.stack([r.ravel(), g.ravel(), b.ravel()], axis=1)), 0, 1)
    with open(path, "w") as f:
        f.write(f"TITLE \"{title}\"\nLUT_3D_SIZE {size}\n")
        for row in table:
            f.write("%.5f %.5f %.5f\n" % tuple(row))
