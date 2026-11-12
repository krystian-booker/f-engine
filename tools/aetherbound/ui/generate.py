#!/usr/bin/env python3
"""UI art and battle effect sprites for samples/aetherbound, drawn in code.

    python tools/aetherbound/ui/generate.py

Writes samples/aetherbound/ui/*.png (panels, cursor, button glyphs) and
samples/aetherbound/fx/battle/* (effect sheets with their .anim.json).
Needs numpy and pillow.
"""
import json
import math
import os

import numpy as np
from PIL import Image, ImageDraw, ImageFilter, ImageFont

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__)))))
GAME = os.path.join(ROOT, "samples", "aetherbound")
UI = os.path.join(GAME, "ui")
FX = os.path.join(GAME, "fx", "battle")
RNG = np.random.default_rng(11)


def save(array, path):
    """array: float RGBA 0..1, (h, w, 4)."""
    data = (np.clip(array, 0, 1) * 255 + 0.5).astype(np.uint8)
    Image.fromarray(data, "RGBA").save(path)


def grid(w, h):
    y, x = np.mgrid[0:h, 0:w].astype(np.float64)
    return (x + 0.5) / w * 2 - 1, (y + 0.5) / h * 2 - 1


def colourise(intensity, ramp):
    """intensity (h, w) 0..1 -> RGBA through a colour ramp [(t, (r, g, b, a)), ...]."""
    ts = np.array([t for t, _ in ramp])
    cs = np.array([c for _, c in ramp], dtype=np.float64)
    out = np.zeros(intensity.shape + (4,))
    for channel in range(4):
        out[..., channel] = np.interp(intensity, ts, cs[:, channel])
    return out


def noise(w, h, scale, seed):
    """Smooth value noise 0..1."""
    rng = np.random.default_rng(seed)
    small = rng.random((max(2, h // scale + 2), max(2, w // scale + 2)))
    image = Image.fromarray((small * 255).astype(np.uint8)).resize((w, h), Image.BICUBIC)
    return np.asarray(image, dtype=np.float64) / 255


def write_sheet(name, frames, durations, ppu=32, loop=False, pivot=(0.5, 0.5)):
    """frames: list of RGBA float arrays of one size; one row."""
    h, w = frames[0].shape[:2]
    sheet = np.concatenate(frames, axis=1)
    save(sheet, os.path.join(FX, name + ".png"))
    if isinstance(durations, (int, float)):
        durations = [durations] * len(frames)
    clip = [{"rect": [i * w, 0, w, h], "duration": durations[i]} for i in range(len(frames))]
    json.dump({"atlas": name + ".png", "pixels_per_unit": ppu, "pivot": list(pivot),
               "clips": {"idle": {"loop": loop, "directions": [clip]}}},
              open(os.path.join(FX, name + ".anim.json"), "w"), indent=1)


# ----------------------------------------------------------------- effects

WHITE_BLUE = [(0, (0.3, 0.5, 1, 0)), (0.35, (0.5, 0.7, 1, 0.5)), (0.7, (0.85, 0.95, 1, 0.95)), (1, (1, 1, 1, 1))]
FIRE = [(0, (0.6, 0.05, 0, 0)), (0.3, (0.9, 0.25, 0.02, 0.6)), (0.6, (1, 0.6, 0.1, 0.95)), (0.85, (1, 0.9, 0.5, 1)),
        (1, (1, 1, 0.9, 1))]
ICE = [(0, (0.2, 0.5, 1, 0)), (0.4, (0.4, 0.75, 1, 0.6)), (0.75, (0.75, 0.95, 1, 0.95)), (1, (1, 1, 1, 1))]
VIOLET = [(0, (0.3, 0.1, 0.8, 0)), (0.35, (0.55, 0.25, 1, 0.55)), (0.7, (0.8, 0.6, 1, 0.95)), (1, (1, 0.95, 1, 1))]
GREEN = [(0, (0.1, 0.7, 0.3, 0)), (0.4, (0.4, 0.95, 0.5, 0.6)), (0.75, (0.8, 1, 0.7, 0.95)), (1, (1, 1, 0.9, 1))]


def slash():
    frames = []
    w = h = 96
    x, y = grid(w, h)
    r = np.sqrt((x + 0.1) ** 2 + (y - 0.05) ** 2)
    angle = np.arctan2(y - 0.05, x + 0.1)
    for f in range(6):
        t = f / 5
        head = -2.4 + t * 3.2
        tail = head - (1.9 - t * 0.6)
        arc = np.exp(-((r - 0.68) / (0.07 + 0.1 * (1 - t))) ** 2)
        window = np.clip((angle - tail) / 0.5, 0, 1) * np.clip((head - angle) / 0.12, 0, 1)
        taper = np.clip((angle - tail) / (head - tail + 1e-6), 0, 1) ** 1.5
        intensity = arc * window * taper * (1 - t * 0.55)
        frames.append(colourise(np.clip(intensity * 1.3, 0, 1), WHITE_BLUE))
    write_sheet("slash", frames, [0.03, 0.04, 0.04, 0.05, 0.06, 0.08])


def impact():
    frames = []
    w = h = 128
    x, y = grid(w, h)
    r = np.sqrt(x * x + y * y)
    angle = np.arctan2(y, x)
    rays = np.zeros_like(r)
    for k in range(14):
        a = RNG.random() * math.tau
        width = 0.05 + RNG.random() * 0.08
        d = np.abs(np.angle(np.exp(1j * (angle - a))))
        rays += np.exp(-(d / width) ** 2) * (0.5 + RNG.random())
    for f in range(6):
        t = f / 5
        reach = 0.25 + t * 0.75
        body = np.clip(1 - r / reach, 0, 1) ** 0.7 * np.clip(rays, 0, 1.4) * (1 - t) ** 0.8
        core = np.exp(-(r / (0.18 * (1 - t) + 0.02)) ** 2) * (1 - t)
        ring = np.exp(-((r - reach * 0.9) / 0.04) ** 2) * (1 - t) * 0.7
        frames.append(colourise(np.clip(body + core + ring, 0, 1),
                                [(0, (1, 0.5, 0.2, 0)), (0.4, (1, 0.7, 0.4, 0.7)), (0.8, (1, 0.95, 0.8, 1)), (1, (1, 1, 1, 1))]))
    write_sheet("impact", frames, 0.05)


def ember():
    frames = []
    w, h = 96, 96
    x, y = grid(w, h)
    for f in range(7):
        t = f / 6
        n = noise(w, h, 10, 100 + f) * 0.6 + noise(w, h, 5, 200 + f) * 0.4
        cy = 0.35 - t * 0.5
        rx, ry = 0.25 + 0.35 * math.sin(min(1, t * 1.6) * math.pi / 2), 0.35 + 0.45 * t
        d = np.sqrt((x / rx) ** 2 + ((y - cy) / ry) ** 2)
        intensity = np.clip(1.25 - d - (n - 0.5) * (0.8 + t), 0, 1) * (1 - t ** 2.2)
        frames.append(colourise(np.clip(intensity * 1.4, 0, 1), FIRE))
    write_sheet("ember", frames, 0.06)


def frost():
    frames = []
    w = h = 96
    x, y = grid(w, h)
    r = np.sqrt(x * x + y * y)
    angle = np.arctan2(y, x)
    for f in range(6):
        t = f / 5
        grow = min(1, t * 2.2)
        spikes = np.zeros_like(r)
        for k in range(6):
            a = k * math.tau / 6 + 0.26
            d = np.abs(np.angle(np.exp(1j * (angle - a))))
            length = 0.9 * grow
            width = 0.16 * (1 - r / max(length, 1e-3))
            spikes = np.maximum(spikes, np.clip(1 - d / np.clip(width, 1e-3, None), 0, 1) * (r < length))
            a2 = a + math.tau / 12
            d2 = np.abs(np.angle(np.exp(1j * (angle - a2))))
            length2 = 0.5 * grow
            width2 = 0.12 * (1 - r / max(length2, 1e-3))
            spikes = np.maximum(spikes, 0.8 * np.clip(1 - d2 / np.clip(width2, 1e-3, None), 0, 1) * (r < length2))
        facets = 0.75 + 0.25 * np.sign(np.sin(angle * 12 + r * 9))
        glow = np.exp(-(r / 0.5) ** 2) * 0.5
        fade = 1 if t < 0.6 else 1 - (t - 0.6) / 0.4
        shatter = 1 if t < 0.6 else (noise(w, h, 4, 300 + f) > (t - 0.6) * 1.8)
        frames.append(colourise(np.clip((spikes * facets + glow) * fade * shatter, 0, 1), ICE))
    write_sheet("frost", frames, [0.05, 0.05, 0.08, 0.1, 0.07, 0.07])


def surge():
    frames = []
    w, h = 96, 128
    x, y = grid(w, h)
    for f in range(7):
        t = f / 6
        width = 0.12 + 0.3 * math.sin(min(1, t * 1.4) * math.pi)
        n = noise(w, h, 8, 400 + f)
        column = np.exp(-(x / width) ** 2) * np.clip(1.1 - (y * -0.5 + 0.5) * (0.3 + t), 0, 1)
        column *= 0.7 + 0.6 * n
        rings = np.zeros_like(x)
        for k in range(3):
            ry = 0.9 - ((t * 1.6 + k / 3) % 1) * 1.8
            rings += np.exp(-((y - ry) / 0.03) ** 2) * np.exp(-(x / (width * 2.4)) ** 2)
        base = np.exp(-((y - 0.9) / 0.12) ** 2) * np.exp(-(x / 0.7) ** 2)
        frames.append(colourise(np.clip((column + rings * 0.8 + base) * (1 - t ** 2), 0, 1), VIOLET))
    write_sheet("surge", frames, 0.06, pivot=(0.5, 0.75))


def mend():
    frames = []
    w, h = 96, 128
    x, y = grid(w, h)
    sparks = [(RNG.random() * 1.4 - 0.7, RNG.random(), 0.5 + RNG.random()) for _ in range(22)]
    for f in range(8):
        t = f / 7
        image = np.zeros_like(x)
        for sx, phase, speed in sparks:
            sy = 0.9 - ((t * speed + phase) % 1) * 1.8
            life = 1 - abs(((t * speed + phase) % 1) - 0.5) * 2
            d = np.sqrt((x - sx * (1 - t * 0.3)) ** 2 + ((y - sy) * 0.75) ** 2)
            image += np.exp(-(d / 0.035) ** 2) * life
            image += 0.5 * np.exp(-((x - sx * (1 - t * 0.3)) / 0.012) ** 2) * np.exp(-((y - sy) / 0.14) ** 2) * life
        column = np.exp(-(x / 0.45) ** 2) * 0.28 * np.clip(y * 0.5 + 0.8, 0, 1)
        ring = np.exp(-((np.sqrt(x ** 2 + ((y - 0.85) * 4) ** 2) - 0.6) / 0.06) ** 2) * 0.8
        frames.append(colourise(np.clip((image + column + ring) * math.sin(min(1, t + 0.15) * math.pi) ** 0.6, 0, 1), GREEN))
    write_sheet("mend", frames, 0.07, pivot=(0.5, 0.75))


def ward():
    frames = []
    w = h = 96
    x, y = grid(w, h)
    r = np.sqrt(x * x + y * y)
    # A hexagonal lattice clipped to a dome.
    q = x * 9
    s = y * 9
    a = np.abs(np.sin(q * 1.0) * np.sin((q * 0.5 + s * 0.866)) * np.sin((q * 0.5 - s * 0.866)))
    lattice = np.clip(1 - a * 6, 0, 1)
    for f in range(6):
        t = f / 5
        radius = 0.55 + 0.3 * min(1, t * 2)
        shell = np.exp(-((r - radius) / 0.03) ** 2)
        inside = (r < radius) * lattice * (0.35 + 0.65 * (r / radius) ** 2)
        sweep = np.exp(-((y + 1 - t * 2.2) / 0.18) ** 2) * (r < radius)
        fade = 1 if t < 0.5 else 1 - (t - 0.5) / 0.5
        frames.append(colourise(np.clip((shell + inside * 0.7 + sweep * 0.5) * fade, 0, 1), VIOLET))
    write_sheet("ward", frames, 0.08)


def resonance():
    frames = []
    w = h = 160
    x, y = grid(w, h)
    r = np.sqrt(x * x + y * y)
    angle = np.arctan2(y, x)
    shards = np.zeros_like(r)
    for k in range(18):
        a = RNG.random() * math.tau
        width = 0.03 + RNG.random() * 0.05
        d = np.abs(np.angle(np.exp(1j * (angle - a))))
        shards = np.maximum(shards, np.clip(1 - d / width, 0, 1) * (0.5 + RNG.random() * 0.5))
    for f in range(8):
        t = f / 7
        image = np.zeros_like(r)
        for tilt in (0.7, -0.7):  # two crossing cuts
            u = x * math.cos(tilt) + y * math.sin(tilt)
            v = -x * math.sin(tilt) + y * math.cos(tilt)
            reach = min(1, t * 3)
            line = np.exp(-(v / (0.035 + 0.05 * (1 - t))) ** 2) * (np.abs(u) < reach) * np.clip(1 - np.abs(u), 0, 1) ** 0.3
            image += line * (1 - t) ** 0.6
        reach = 0.2 + t * 0.85
        burst = shards * np.clip(1 - np.abs(r - reach * 0.7) / (0.35 * (1 - t) + 0.05), 0, 1) * (1 - t) ** 0.5
        core = np.exp(-(r / (0.3 * (1 - t) + 0.03)) ** 2) * (1 - t)
        ring = np.exp(-((r - reach) / 0.025) ** 2) * (1 - t)
        frames.append(colourise(np.clip(image + burst + core + ring * 0.8, 0, 1), VIOLET))
    write_sheet("resonance", frames, [0.03, 0.04, 0.05, 0.06, 0.07, 0.08, 0.08, 0.09])


def simple():
    # ring: a thin circle (also the shape a shock wave bends the scene by)
    x, y = grid(128, 128)
    r = np.sqrt(x * x + y * y)
    ring = np.exp(-((r - 0.82) / 0.05) ** 2)
    image = np.ones((128, 128, 4))
    image[..., 3] = ring
    write_sheet("ring", [image], 1, ppu=64)

    # mote: a soft dot; glow: a wide soft light
    for name, size, power, ppu in (("mote", 16, 1.6, 16), ("glow", 64, 2.2, 32)):
        x, y = grid(size, size)
        r = np.sqrt(x * x + y * y)
        image = np.ones((size, size, 4))
        image[..., 3] = np.clip(1 - r, 0, 1) ** power
        write_sheet(name, [image], 1, ppu=ppu)

    # shard: a small crystal splinter
    x, y = grid(16, 32)
    body = np.clip(1 - np.abs(x) / np.clip(0.9 - np.abs(y) * 0.9, 1e-3, None), 0, 1) > 0
    facet = np.where(x < 0, 0.75, 1.0)
    image = np.zeros((32, 16, 4))
    image[..., 0], image[..., 1], image[..., 2] = 0.85 * facet, 0.7 * facet, 1.0
    image[..., 3] = body * 1.0
    write_sheet("shard", [image], 1, ppu=32)

    # rubble: a chunk of masonry
    x, y = grid(16, 16)
    n = noise(16, 16, 4, 77)
    body = (np.sqrt(x * x + y * y) + (n - 0.5) * 0.7) < 0.75
    shade = 0.55 + 0.35 * np.clip(-y * 0.6 - x * 0.4 + 0.5, 0, 1)
    image = np.zeros((16, 16, 4))
    image[..., 0], image[..., 1], image[..., 2] = shade * 0.8, shade * 0.76, shade * 0.86
    image[..., 3] = body * 1.0
    write_sheet("rubble", [image], 1, ppu=32)

    # dust: puffs of smoke
    frames = []
    for f in range(4):
        x, y = grid(64, 64)
        n = noise(64, 64, 8, 500 + f) * 0.6 + noise(64, 64, 4, 600 + f) * 0.4
        r = np.sqrt(x * x + y * y)
        alpha = np.clip(1.0 - r - (n - 0.5) * 0.9, 0, 1) ** 1.6 * 0.8
        image = np.ones((64, 64, 4))
        image[..., :3] = (0.75 + 0.25 * n)[..., None]
        image[..., 3] = alpha
        frames.append(image)
    write_sheet("dust", frames, 0.3, ppu=32, loop=True)

    # shadow: the soft blob under a character (drawn multiplied)
    x, y = grid(48, 24)
    r = np.sqrt(x * x + y * y)
    darkness = np.clip(1 - r, 0, 1) ** 0.8 * 0.55
    image = np.ones((24, 48, 4))
    image[..., :3] = (1 - darkness)[..., None]
    image[..., 3] = 1
    write_sheet("shadow", [image], 1, ppu=40)

    # beam: a column of light, bright core, soft falloff, fading at the top
    x, y = grid(64, 256)
    core = np.exp(-(x / 0.16) ** 2) + 0.5 * np.exp(-(x / 0.5) ** 2)
    streaks = 0.8 + 0.2 * noise(64, 256, 3, 900)[:, :]
    falloff = np.clip((y + 1) / 0.5, 0, 1)
    image = np.ones((256, 64, 4))
    image[..., 0], image[..., 1] = 0.92, 0.88
    image[..., 3] = np.clip(core * streaks * falloff, 0, 1)
    write_sheet("beam", [image], 1, ppu=8, pivot=(0.5, 1))

    # star: a four-pointed flare
    x, y = grid(96, 96)
    r = np.sqrt(x * x + y * y)
    flare = np.exp(-(np.abs(y) / 0.03) ** 1) * np.exp(-(np.abs(x) / 0.55) ** 1.3)
    flare += np.exp(-(np.abs(x) / 0.03) ** 1) * np.exp(-(np.abs(y) / 0.4) ** 1.3)
    halo = np.exp(-(r / 0.18) ** 2) + 0.25 * np.exp(-(r / 0.5) ** 2)
    image = np.ones((96, 96, 4))
    image[..., 0], image[..., 1] = 0.85, 0.75
    image[..., 3] = np.clip(flare + halo, 0, 1)
    write_sheet("star", [image], 1, ppu=32)

    # veil: the wall of Aether (a lattice that shimmers)
    x, y = grid(128, 128)
    q, s = x * 7, y * 7
    a = np.abs(np.sin(q) * np.sin(q * 0.5 + s * 0.866) * np.sin(q * 0.5 - s * 0.866))
    lattice = np.clip(1 - a * 5, 0, 1)
    r = np.sqrt(x * x + y * y)
    image = np.ones((128, 128, 4))
    image[..., 0], image[..., 1] = 0.75, 0.55
    image[..., 3] = np.clip(lattice * 0.9 + 0.12, 0, 1) * np.clip(1.15 - r, 0, 1) ** 0.8
    write_sheet("veil", [image], 1, ppu=32)


def circle():
    size = 256
    image = Image.new("L", (size, size), 0)
    draw = ImageDraw.Draw(image)
    c = size / 2

    def ring(radius, width):
        draw.ellipse([c - radius, c - radius, c + radius, c + radius], outline=255, width=width)

    ring(122, 4)
    ring(110, 2)
    ring(70, 3)
    ring(30, 2)
    for k in range(6):
        a0 = k * math.tau / 6 - math.pi / 2
        a1 = a0 + math.tau / 3
        draw.line([c + 110 * math.cos(a0), c + 110 * math.sin(a0), c + 110 * math.cos(a1), c + 110 * math.sin(a1)],
                  fill=255, width=3)
    for k in range(24):
        a = k * math.tau / 24
        inner, outer = (86, 104) if k % 2 else (92, 104)
        draw.line([c + inner * math.cos(a), c + inner * math.sin(a), c + outer * math.cos(a), c + outer * math.sin(a)],
                  fill=255, width=3)
    for k in range(6):
        a = k * math.tau / 6
        px, py = c + 50 * math.cos(a), c + 50 * math.sin(a)
        draw.polygon([(px, py - 9), (px + 6, py), (px, py + 9), (px - 6, py)], fill=255)
    soft = np.asarray(image.filter(ImageFilter.GaussianBlur(5)), dtype=np.float64) / 255
    sharp = np.asarray(image.filter(ImageFilter.GaussianBlur(0.8)), dtype=np.float64) / 255
    alpha = np.clip(sharp + soft * 0.9, 0, 1)
    out = np.ones((size, size, 4))
    out[..., 3] = alpha
    write_sheet("circle", [out], 1, ppu=64)


def silhouette():
    """The shape behind Rowan for one frame: something vast, winged, crowned."""
    w, h = 384, 512
    image = Image.new("L", (w, h), 0)
    draw = ImageDraw.Draw(image)
    cx = w / 2
    # Halo: a great ring with rays.
    for radius, width in ((150, 5), (164, 2), (118, 2)):
        draw.ellipse([cx - radius, 150 - radius, cx + radius, 150 + radius], outline=230, width=width)
    for k in range(36):
        a = k * math.tau / 36
        inner, outer = 168, (196 if k % 3 else 228)
        draw.line([cx + inner * math.cos(a), 150 + inner * math.sin(a), cx + outer * math.cos(a), 150 + outer * math.sin(a)],
                  fill=200, width=2)
    # Six wings: fans of long feathers, upper, middle and lower pairs.
    for side in (-1, 1):
        for base_angle, length, count, spread, by in ((-0.55, 190, 9, 0.5, 190), (0.0, 170, 8, 0.4, 235), (0.5, 150, 7, 0.38, 285)):
            for k in range(count):
                a = base_angle + (k / (count - 1) - 0.5) * spread
                reach = length * (0.6 + 0.4 * math.sin((k + 0.5) / count * math.pi))
                x0, y0 = cx + side * 18, by
                x1, y1 = cx + side * (18 + reach * math.cos(a)), by + reach * math.sin(a)
                nx, ny = -(y1 - y0), (x1 - x0)
                norm = math.hypot(nx, ny) or 1
                nx, ny = nx / norm * 9, ny / norm * 9
                mx, my = (x0 + x1) / 2, (y0 + y1) / 2
                draw.polygon([(x0, y0), (mx + nx, my + ny), (x1, y1), (mx - nx, my - ny)], fill=255)
    # The figure: a tall narrow body with a crowned head and trailing robes.
    draw.polygon([(cx - 26, 200), (cx + 26, 200), (cx + 44, 360), (cx + 70, 500), (cx, 470), (cx - 70, 500), (cx - 44, 360)],
                 fill=255)
    draw.ellipse([cx - 22, 150, cx + 22, 204], fill=255)
    for k in (-2, -1, 0, 1, 2):  # crown
        draw.polygon([(cx + k * 9 - 5, 156), (cx + k * 9, 120 - (3 - abs(k)) * 9), (cx + k * 9 + 5, 156)], fill=255)
    # Arms, open.
    for side in (-1, 1):
        draw.polygon([(cx + side * 24, 214), (cx + side * 96, 268), (cx + side * 104, 300), (cx + side * 84, 286),
                      (cx + side * 22, 244)], fill=255)
    sharp = np.asarray(image.filter(ImageFilter.GaussianBlur(1.2)), dtype=np.float64) / 255
    glow = np.asarray(image.filter(ImageFilter.GaussianBlur(14)), dtype=np.float64) / 255
    alpha = np.clip(sharp * 0.95 + glow * 0.8, 0, 1)
    # Light pours out from behind it: brightest at the edges, the body itself burnt out.
    out = np.ones((h, w, 4))
    out[..., 0] = 0.92
    out[..., 1] = 0.86
    out[..., 3] = alpha
    write_sheet("silhouette", [out], 1, ppu=16, pivot=(0.5, 1))


# ---------------------------------------------------------------------- UI

def rounded(size, radius, fill, outline=None, width=1):
    image = Image.new("RGBA", size, (0, 0, 0, 0))
    ImageDraw.Draw(image).rounded_rectangle([0, 0, size[0] - 1, size[1] - 1], radius, fill=fill, outline=outline,
                                            width=width)
    return image


def panels():
    scale = 4  # drawn large and reduced, for clean edges

    def make(name, fill_top, fill_bottom, line, glow=None, brackets=False, scan=False):
        s = 96 * scale
        image = Image.new("RGBA", (s, s), (0, 0, 0, 0))
        body = Image.new("RGBA", (s, s), (0, 0, 0, 0))
        gradient = np.zeros((s, s, 4), dtype=np.uint8)
        for row in range(s):
            t = row / (s - 1)
            gradient[row, :, :] = [int(fill_top[i] + (fill_bottom[i] - fill_top[i]) * t) for i in range(4)]
        mask = Image.new("L", (s, s), 0)
        ImageDraw.Draw(mask).rounded_rectangle([2 * scale, 2 * scale, s - 2 * scale - 1, s - 2 * scale - 1], 7 * scale,
                                               fill=255)
        body = Image.fromarray(gradient, "RGBA")
        image.paste(body, (0, 0), mask)
        draw = ImageDraw.Draw(image)
        if scan:
            for row in range(0, s, 3 * scale):
                draw.line([3 * scale, row, s - 3 * scale, row], fill=(120, 80, 200, 26), width=scale)
        draw.rounded_rectangle([2 * scale, 2 * scale, s - 2 * scale - 1, s - 2 * scale - 1], 7 * scale,
                               outline=(0, 0, 0, 220), width=scale)
        inset = 6 * scale
        draw.rounded_rectangle([inset, inset, s - inset - 1, s - inset - 1], 4 * scale, outline=line, width=scale)
        if brackets:
            for cx, cy, dx, dy in ((inset, inset, 1, 1), (s - inset, inset, -1, 1), (inset, s - inset, 1, -1),
                                   (s - inset, s - inset, -1, -1)):
                draw.line([cx, cy, cx + dx * 12 * scale, cy], fill=glow, width=2 * scale)
                draw.line([cx, cy, cx, cy + dy * 12 * scale], fill=glow, width=2 * scale)
        else:
            for cx, cy in ((inset, inset), (s - inset, inset), (inset, s - inset), (s - inset, s - inset)):
                d = 3 * scale
                draw.polygon([(cx, cy - d), (cx + d, cy), (cx, cy + d), (cx - d, cy)], fill=line)
        image = image.resize((96, 96), Image.LANCZOS)
        image.save(os.path.join(UI, name + ".png"))

    make("panel", (16, 20, 44, 232), (6, 8, 20, 240), (201, 162, 74, 255))
    make("plate", (10, 12, 28, 214), (4, 5, 14, 224), (150, 132, 96, 200))
    make("system", (18, 8, 40, 236), (4, 2, 12, 244), (150, 100, 255, 255), glow=(214, 180, 255, 255), brackets=True,
         scan=True)


def cursor():
    scale = 4
    image = Image.new("RGBA", (64 * scale, 40 * scale), (0, 0, 0, 0))
    draw = ImageDraw.Draw(image)
    points = [(6, 6), (56, 20), (6, 34), (18, 20)]
    draw.polygon([(x * scale, y * scale) for x, y in points], fill=(0, 0, 0, 200))
    inner = [(11, 11), (48, 20), (11, 29), (20, 20)]
    draw.polygon([(x * scale, y * scale) for x, y in inner], fill=(244, 214, 130, 255))
    highlight = [(13, 13), (40, 19.5), (20, 19.5)]
    draw.polygon([(x * scale, y * scale) for x, y in highlight], fill=(255, 246, 214, 255))
    image.resize((64, 40), Image.LANCZOS).save(os.path.join(UI, "cursor.png"))

    image = Image.new("RGBA", (32 * scale, 32 * scale), (0, 0, 0, 0))
    draw = ImageDraw.Draw(image)
    draw.polygon([(4 * scale, 8 * scale), (28 * scale, 8 * scale), (16 * scale, 26 * scale)], fill=(0, 0, 0, 180))
    draw.polygon([(8 * scale, 10 * scale), (24 * scale, 10 * scale), (16 * scale, 22 * scale)], fill=(244, 214, 130, 255))
    image.resize((32, 32), Image.LANCZOS).save(os.path.join(UI, "advance.png"))


def glyphs():
    cell, scale = 64, 4
    s = cell * scale
    sheet = Image.new("RGBA", (cell * 8, cell * 8), (0, 0, 0, 0))
    mono = ImageFont.truetype(os.path.join(UI, "fonts", "ShareTechMono_400Regular.ttf"), 30 * scale)
    small = ImageFont.truetype(os.path.join(UI, "fonts", "ShareTechMono_400Regular.ttf"), 17 * scale)

    def text(draw, label, font, fill, dy=0):
        box = draw.textbbox((0, 0), label, font=font)
        draw.text(((s - (box[2] - box[0])) / 2 - box[0], (s - (box[3] - box[1])) / 2 - box[1] + dy), label, font=font,
                  fill=fill)

    def arrow(draw, direction, fill):
        c, r = s / 2, 14 * scale
        angle = {"Up": -90, "Down": 90, "Left": 180, "Right": 0}[direction]
        points = []
        for a in (0, 130, 230):
            rad = math.radians(angle + a)
            points.append((c + r * math.cos(rad), c + r * math.sin(rad)))
        draw.polygon(points, fill=fill)

    def keycap(label):
        image = Image.new("RGBA", (s, s), (0, 0, 0, 0))
        draw = ImageDraw.Draw(image)
        draw.rounded_rectangle([5 * scale, 9 * scale, s - 5 * scale, s - 5 * scale], 10 * scale, fill=(20, 22, 34, 255))
        draw.rounded_rectangle([5 * scale, 6 * scale, s - 5 * scale, s - 10 * scale], 10 * scale, fill=(226, 222, 210, 255),
                               outline=(40, 42, 58, 255), width=2 * scale)
        ink = (30, 32, 48, 255)
        if label in ("Up", "Down", "Left", "Right"):
            arrow(draw, label, ink)
        else:
            short = {"Return": "ENT", "Escape": "ESC", "Space": "SPC", "Left Shift": "SHF", "Tab": "TAB",
                     "Backspace": "BSP"}.get(label, label)
            text(draw, short, small if len(short) > 1 else mono, ink, dy=-2 * scale)
        return image.resize((cell, cell), Image.LANCZOS)

    def button(label, face, ink, shape=None):
        image = Image.new("RGBA", (s, s), (0, 0, 0, 0))
        draw = ImageDraw.Draw(image)
        draw.ellipse([6 * scale, 8 * scale, s - 6 * scale, s - 4 * scale], fill=(10, 10, 16, 255))
        draw.ellipse([6 * scale, 5 * scale, s - 6 * scale, s - 7 * scale], fill=face, outline=(10, 10, 16, 255),
                     width=2 * scale)
        c, r = s / 2, 13 * scale
        cy = c - scale
        if shape == "cross":
            draw.line([c - r, cy - r, c + r, cy + r], fill=ink, width=4 * scale)
            draw.line([c - r, cy + r, c + r, cy - r], fill=ink, width=4 * scale)
        elif shape == "circle":
            draw.ellipse([c - r, cy - r, c + r, cy + r], outline=ink, width=4 * scale)
        elif shape == "square":
            draw.rectangle([c - r, cy - r, c + r, cy + r], outline=ink, width=4 * scale)
        elif shape == "triangle":
            draw.polygon([(c, cy - r - 2 * scale), (c + r + scale, cy + r), (c - r - scale, cy + r)], outline=ink,
                         width=4 * scale)
        elif shape in ("Up", "Down", "Left", "Right"):
            arrow(draw, shape, ink)
        elif shape == "start":
            for k in (-1, 0, 1):
                draw.line([c - r, cy + k * 8 * scale, c + r, cy + k * 8 * scale], fill=ink, width=3 * scale)
        else:
            text(draw, label, mono, ink, dy=-3 * scale)
        return image.resize((cell, cell), Image.LANCZOS)

    keys = ["Return", "Escape", "Space", "Z", "X", "Left Shift", "Tab", "Backspace", "Up", "Down", "Left", "Right", "W",
            "A", "S", "D"]
    for i, key in enumerate(keys):
        sheet.paste(keycap(key), ((i % 8) * cell, (i // 8) * cell))
    dark, grey = (34, 36, 50, 255), (70, 72, 88, 255)
    pads = {
        2: [("A", dark, (120, 220, 120, 255), None), ("B", dark, (240, 100, 100, 255), None),
            ("X", dark, (110, 170, 255, 255), None), ("Y", dark, (245, 215, 100, 255), None)],
        4: [("", dark, (130, 170, 255, 255), "cross"), ("", dark, (255, 120, 120, 255), "circle"),
            ("", dark, (240, 150, 220, 255), "square"), ("", dark, (120, 225, 190, 255), "triangle")],
        6: [("B", grey, (240, 240, 240, 255), None), ("A", grey, (240, 240, 240, 255), None),
            ("Y", grey, (240, 240, 240, 255), None), ("X", grey, (240, 240, 240, 255), None)],
    }
    for row, faces in pads.items():
        entries = faces + [("", grey, (236, 236, 236, 255), "start")] + [
            ("", grey, (236, 236, 236, 255), d) for d in ("Up", "Down", "Left", "Right")]
        for i, (label, face, ink, shape) in enumerate(entries):
            sheet.paste(button(label, face, ink, shape), ((i % 8) * cell, (row + i // 8) * cell))
    sheet.save(os.path.join(UI, "glyphs.png"))


def main():
    os.makedirs(UI, exist_ok=True)
    os.makedirs(FX, exist_ok=True)
    slash()
    impact()
    ember()
    frost()
    surge()
    mend()
    ward()
    resonance()
    simple()
    circle()
    silhouette()
    panels()
    cursor()
    glyphs()
    print("wrote", len(os.listdir(FX)), "effect files and", len([f for f in os.listdir(UI) if f.endswith(".png")]), "UI images")


if __name__ == "__main__":
    main()
