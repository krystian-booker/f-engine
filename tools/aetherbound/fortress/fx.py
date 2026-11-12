"""Particle sprite sheets for the fortress (flames, embers, smoke, ash, heat
haze, glows, aether motes, mist) -> samples/aetherbound/fx/fortress/."""
import json
import math
import os
import numpy as np
from PIL import Image
from pix import value_noise, rng, hexrgb, FIRE, VIOLET


def write_sheet(folder, name, frames, durations, loop=False, ppu=32, pivot=(0.5, 0.5)):
    """frames: list of RGBA uint8 arrays of the same size, laid out in a row."""
    h, w = frames[0].shape[:2]
    sheet = np.zeros((h, w * len(frames), 4), dtype=np.uint8)
    for i, f in enumerate(frames):
        sheet[:, i * w:(i + 1) * w] = f
    Image.fromarray(sheet, "RGBA").save(os.path.join(folder, name + ".png"), optimize=True)
    clip = {"loop": loop, "directions": [[{"rect": [i * w, 0, w, h], "duration": durations[i]}
                                          for i in range(len(frames))]]}
    doc = {"atlas": name + ".png", "pixels_per_unit": ppu, "pivot": list(pivot), "clips": {"idle": clip}}
    with open(os.path.join(folder, name + ".anim.json"), "w") as f:
        json.dump(doc, f, indent=1)


def flame_frames(n=8, size=32, seed=3):
    r = rng(seed)
    frames = []
    noise = value_noise(size * 2, size * n, 5, seed, 2)
    for f in range(n):
        t = f / (n - 1)
        img = np.zeros((size, size, 4), dtype=np.uint8)
        H = size * (0.62 + 0.36 * math.sin(min(1.0, t * 1.4) * math.pi * 0.75))
        W = size * (0.36 - 0.2 * t)
        sway = r.uniform(-1.5, 1.5)
        for y in range(size):
            up = (size - 1 - y)
            if up > H:
                continue
            v = up / H
            # teardrop: widest at 25% of the height
            prof = math.sin(min(1.0, (v + 0.12) / 0.4) * math.pi / 2) * (1 - v ** 1.6)
            cx = size / 2 + math.sin(v * 3.2 + f * 1.3) * (1.5 + 3.5 * v * t) + sway * v
            for x in range(size):
                d = abs(x + 0.5 - cx) / max(0.5, W * prof)
                nz = noise[(y + f * size) % noise.shape[0], x + (f * 7) % size]
                edge = 1 - d - (nz - 0.4) * (0.5 + 1.3 * t) * (0.4 + v)
                if edge <= 0:
                    continue
                heat = edge * (1 - v * 0.85) * (1.15 - t * 0.85)
                k = 6 if heat > 0.62 else 5 if heat > 0.45 else 4 if heat > 0.3 else 3 if heat > 0.17 else \
                    2 if heat > 0.07 else 1
                img[y, x, :3] = FIRE[k]
                img[y, x, 3] = 255
        frames.append(img)
    return frames


def ember_frames(size=8):
    shapes = [
        ["........", "........", "...##...", "..####..", "..####..", "...##...", "........", "........"],
        ["........", "...#....", "...#....", ".######.", "...#....", "...#....", "........", "........"],
        ["........", "........", "...#....", "..###...", "...#....", "........", "........", "........"],
        ["........", "........", "........", "...#....", "........", "........", "........", "........"],
    ]
    frames = []
    for s in shapes:
        img = np.zeros((size, size, 4), dtype=np.uint8)
        for y, row in enumerate(s):
            for x, ch in enumerate(row):
                if ch == "#":
                    centre = abs(x - 3.5) < 1 and abs(y - 3.5) < 1
                    img[y, x, :3] = FIRE[6] if centre else FIRE[4]
                    img[y, x, 3] = 255
        frames.append(img)
    return frames


def blob_frames(n, size, seed, tones, dissolve=True, soft=False):
    """Puffy cloud blobs with banded shading; later frames break up."""
    r = rng(seed)
    frames = []
    for f in range(n):
        t = f / max(1, n - 1)
        img = np.zeros((size, size, 4), dtype=np.uint8)
        blobs = [(size / 2 + r.uniform(-0.18, 0.18) * size, size / 2 + r.uniform(-0.18, 0.18) * size,
                  size * r.uniform(0.16, 0.3)) for _ in range(6)]
        noise = value_noise(size, size, max(3, size // 8), seed + f * 3, 2)
        yy, xx = np.mgrid[0:size, 0:size].astype(np.float32) + 0.5
        dens = np.zeros((size, size), dtype=np.float32)
        for (bx, by, br) in blobs:
            d = np.sqrt((xx - bx) ** 2 + (yy - by) ** 2) / br
            dens = np.maximum(dens, 1 - d)
        dens = dens + (noise - 0.5) * (0.5 + (0.9 * t if dissolve else 0))
        win = 1 - np.sqrt((xx - size / 2) ** 2 + (yy - size / 2) ** 2) / (size / 2)
        dens = np.minimum(dens, win * 3.0 - 0.25)
        inside = dens > (0.05 + (0.22 * t if dissolve else 0))
        # light from the upper left
        shade = dens + ((size / 2 - xx) + (size / 2 - yy)) / size * 0.55
        lvl = np.clip(np.floor(shade * len(tones) * 0.9), 0, len(tones) - 1).astype(int)
        cols = np.stack([hexrgb(c) for c in tones])
        img[..., :3] = cols[lvl]
        if soft:
            a = np.clip(dens * 2.2, 0, 1)
            a = np.round(a * 6) / 6
            img[..., 3] = (a * 255 * inside).astype(np.uint8)
        else:
            img[..., 3] = np.where(inside, 255, 0)
        frames.append(img)
    return frames


def ash_frames(size=8):
    shapes = [
        ["........", "........", "..##....", "..###...", "...##...", "........", "........", "........"],
        ["........", "........", "...#....", "..###...", "...#....", "........", "........", "........"],
        ["........", "........", "....##..", "..###...", "..##....", "........", "........", "........"],
        ["........", "........", "........", "..####..", "........", "........", "........", "........"],
    ]
    frames = []
    for s in shapes:
        img = np.zeros((size, size, 4), dtype=np.uint8)
        for y, row in enumerate(s):
            for x, ch in enumerate(row):
                if ch == "#":
                    img[y, x] = (200, 196, 205, 255) if (x + y) % 2 else (150, 146, 160, 255)
        frames.append(img)
    return frames


def radial(size, power=2.0, color=(255, 255, 255), bands=0):
    yy, xx = np.mgrid[0:size, 0:size].astype(np.float32) + 0.5
    d = np.sqrt((xx - size / 2) ** 2 + (yy - size / 2) ** 2) / (size / 2)
    a = np.clip(1 - d, 0, 1) ** power
    if bands:
        a = np.round(a * bands) / bands
    img = np.zeros((size, size, 4), dtype=np.uint8)
    img[..., :3] = color
    img[..., 3] = (a * 255).astype(np.uint8)
    return img


def mote_frames(size=8):
    shapes = [
        ["........", "...#....", "..###...", ".#####..", "..###...", "...#....", "........", "........"],
        ["........", "........", "...#....", "..###...", "...#....", "........", "........", "........"],
        ["...#....", "...#....", "..###...", "#######.", "..###...", "...#....", "...#....", "........"],
        ["........", "........", "...#....", "..###...", "...#....", "........", "........", "........"],
    ]
    frames = []
    for s in shapes:
        img = np.zeros((size, size, 4), dtype=np.uint8)
        for y, row in enumerate(s):
            for x, ch in enumerate(row):
                if ch == "#":
                    centre = abs(x - 3) < 1 and abs(y - 3) < 1
                    img[y, x, :3] = VIOLET[6] if centre else VIOLET[4]
                    img[y, x, 3] = 255
        frames.append(img)
    return frames


def figure_frames():
    """Four 1.5-unit stand-ins for the heroes, seen from behind, for judging
    scale in test shots (64 x 64, feet at the bottom centre)."""
    looks = [("#24305e", "#3a57c8", "#c8a86a"), ("#2a2a30", "#a01c24", "#16161a"), ("#d8cfb8", "#2f8f8a", "#8a3a22"),
             ("#7c828c", "#b88a30", "#8a8a8a")]
    frames = []
    for coat, cape, hair in looks:
        img = np.zeros((64, 64, 4), dtype=np.uint8)
        o = (16, 15, 24)

        def px(x, y, c):
            if 0 <= x < 64 and 0 <= y < 64:
                img[y, x] = (*[int(v) for v in hexrgb(c)], 255) if isinstance(c, str) else (*c, 255)

        for y in range(16, 64):
            for x in range(20, 44):
                yy = y - 16
                cx = abs(x - 31.5)
                if yy < 18:  # head
                    if cx * cx + (yy - 9) ** 2 < 81:
                        px(x, y, hair)
                elif yy < 36:  # torso + cape
                    w = 8 + (yy - 18) * 0.25
                    if cx < w:
                        px(x, y, cape if cx < w - 2 and yy > 20 else coat)
                elif yy < 46:  # legs
                    if 1 < cx < 7:
                        px(x, y, coat)
                else:
                    if 1 < cx < 8:
                        px(x, y, "#1c1a20")
        # outline
        a = img[..., 3] > 0
        edge = a & ~(np.roll(a, 1, 0) & np.roll(a, -1, 0) & np.roll(a, 1, 1) & np.roll(a, -1, 1))
        img[edge] = (*o, 255)
        frames.append(img)
    return frames


def build(folder, with_figures=True):
    os.makedirs(folder, exist_ok=True)
    write_sheet(folder, "flame", flame_frames(), [0.1] * 8)
    write_sheet(folder, "ember", ember_frames(), [0.5, 0.5, 0.9, 3.0])
    write_sheet(folder, "smoke", blob_frames(6, 48, 5, ["#0e0d14", "#1a1822", "#2a2632", "#3d3742", "#544c54"]),
                [0.9, 0.9, 1.0, 1.1, 1.3, 9.0])
    write_sheet(folder, "ash", ash_frames(), [0.4, 0.4, 0.4, 0.4], loop=True)
    write_sheet(folder, "haze", [radial(32, 1.5)], [1.0])
    write_sheet(folder, "glow", [radial(64, 2.2)], [1.0])
    write_sheet(folder, "mote", mote_frames(), [0.3, 0.3, 0.3, 0.3], loop=True)
    write_sheet(folder, "mist", blob_frames(4, 64, 9, ["#5a6494", "#6f7aac", "#8792c2", "#a0aad6"], dissolve=False,
                                            soft=True), [4.0, 4.0, 4.0, 30.0])
    if with_figures:
        for i, f in enumerate(figure_frames()):
            write_sheet(folder, f"scale_{i + 1}", [f], [1.0], ppu=32, pivot=(0.5, 1.0))


if __name__ == "__main__":
    import sys
    build(sys.argv[1])
