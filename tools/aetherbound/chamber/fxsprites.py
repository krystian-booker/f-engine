"""Particle sprites of the chamber and the ruins (samples/aetherbound/fx/chamber):
flame puffs, embers, Aetherite motes, dust, smoke, a soft glow."""
import json
import math
import os
import numpy as np
from paint import FIRE, AETHER, ASH, vnoise, fbm, save


def write_sheet(folder, name, frames, durations, loop=False, ppu=32):
    """frames: list of (h, w, 4) arrays, laid out in one row."""
    h, w = frames[0].shape[:2]
    sheet = np.zeros((h, w * len(frames), 4))
    for i, frame in enumerate(frames):
        sheet[:, i * w:(i + 1) * w] = frame
    save(os.path.join(folder, f"{name}.png"), sheet[..., :3], sheet[..., 3])
    clip = [{"rect": [i * w, 0, w, h], "duration": durations[i]} for i in range(len(frames))]
    document = {"atlas": f"{name}.png", "pixels_per_unit": ppu, "pivot": [0.5, 0.5],
                "clips": {"idle": {"loop": loop, "directions": [clip]}}}
    with open(os.path.join(folder, f"{name}.anim.json"), "w") as f:
        json.dump(document, f, indent=1)


def flame_frames(rng, size=16, count=6):
    frames = []
    yy, xx = np.mgrid[0:size, 0:size] + 0.5
    noise = fbm(size * count, size, 5, rng, 2, tile_x=False, tile_y=False)
    for k in range(count):
        age = k / (count - 1)
        cx, cy = size / 2 + 0.6 * math.sin(k * 1.9), size * (0.62 - 0.2 * age)
        rx, ry = size * (0.36 - 0.12 * age), size * (0.34 + 0.1 * age - 0.2 * age * age)
        d = np.hypot((xx - cx) / rx, (yy - cy) / (ry * np.where(yy < cy, 1.5, 0.8)))
        heat = 1.05 - d + 0.55 * (noise[k * size:(k + 1) * size] - 0.5) - 0.35 * age
        index = np.clip(np.rint(heat * 7.5 - 0.5 * age * 4), 0, 7).astype(int)
        frame = np.zeros((size, size, 4))
        frame[..., :3] = FIRE[index]
        frame[..., 3] = np.where(heat > 0.16, 255, 0)
        frames.append(frame)
    return frames


def dot_frames(colours, sizes, canvas=8):
    """Small diamond sparks: each frame a core colour and a size (1..)."""
    frames = []
    for colour, halo, radius in sizes:
        frame = np.zeros((canvas, canvas, 4))
        # Centred on a pixel, not on the corner between four: there the nearest pixel is a
        # whole unit away and a spark of radius under 1 covered nothing (spritekit: sheet.empty_frame).
        c = canvas // 2 - 1
        for y in range(canvas):
            for x in range(canvas):
                d = abs(x - c) + abs(y - c)
                if d <= radius:
                    frame[y, x, :3] = colours[colour]
                    frame[y, x, 3] = 255
                elif d <= radius + 1 and halo is not None:
                    frame[y, x, :3] = colours[halo]
                    frame[y, x, 3] = 255
        frames.append(frame)
    return frames


def mote_frame(radius, brightness, size=16):
    yy, xx = np.mgrid[0:size, 0:size] + 0.5
    d = np.hypot(xx - size / 2, yy - size / 2)
    frame = np.zeros((size, size, 4))
    core = d <= max(1.0, radius * 0.32)
    level = np.clip(1.0 - d / radius, 0, 1)
    steps = np.ceil(level * 3) / 3                    # three flat rings of light
    frame[..., :3] = np.where(core[..., None], AETHER[7], np.where((steps > 0.6)[..., None], AETHER[6], AETHER[4]))
    frame[..., 3] = np.where(core, 255, steps ** 1.5 * 150) * brightness
    return frame


def smoke_frames(rng, size=32, count=4):
    frames = []
    yy, xx = np.mgrid[0:size, 0:size] + 0.5
    for k in range(count):
        noise = fbm(size, size, 9, rng, 3, tile_x=False, tile_y=False)
        d = np.hypot(xx - size / 2, yy - size / 2) / (size * (0.3 + 0.045 * k))
        density = 1.0 - d + 0.9 * (noise - 0.5)
        level = np.clip(np.floor(density * 4), 0, 3)      # three flat alpha steps
        tone = np.clip(np.rint(3.0 + 3.0 * (noise - 0.3) - 0.8 * (yy / size)), 1, 6).astype(int)
        frame = np.zeros((size, size, 4))
        frame[..., :3] = ASH[tone]
        frame[..., 3] = level * 62
        frames.append(frame)
    return frames


def glow_frame(size=64, power=2.2):
    yy, xx = np.mgrid[0:size, 0:size] + 0.5
    d = np.hypot(xx - size / 2, yy - size / 2) / (size / 2)
    value = np.clip(1 - d, 0, 1) ** power
    frame = np.zeros((size, size, 4))
    frame[..., :3] = 255
    frame[..., 3] = value * 255
    return frame


def write_fx(folder, seed=5):
    os.makedirs(folder, exist_ok=True)
    rng = np.random.default_rng(seed)
    write_sheet(folder, "flame", flame_frames(rng), [0.12, 0.12, 0.14, 0.16, 0.18, 0.4])
    write_sheet(folder, "ember", dot_frames(FIRE, [(7, 5, 0.6), (6, 4, 0.6), (5, None, 0.6), (4, None, 0.6)], 4),
                [0.5, 0.7, 0.9, 3.0])
    # A mote of Aetherite: swells out of nothing, twinkles, then the emitter's colour fades it. Soft-edged, so
    # that one drifting past the lens is a blur of light and not a block.
    write_sheet(folder, "mote", [mote_frame(r, b) for r, b in ((1.5, 0.5), (2.6, 0.8), (4.2, 1.0), (3.4, 0.85), (4.6, 1.0),
                                                              (3.6, 0.9), (4.4, 1.0), (3.8, 0.9))],
                [0.5, 0.6, 1.4, 0.9, 1.2, 1.1, 1.5, 60.0])
    write_sheet(folder, "dust", dot_frames(ASH, [(5, None, 0.6), (7, None, 0.6), (6, None, 0.6)], 4), [0.8, 2.0, 60.0])
    write_sheet(folder, "smoke", smoke_frames(rng), [1.2, 1.6, 2.2, 60.0])
    write_sheet(folder, "glow", [glow_frame()], [60.0])
    write_sheet(folder, "haze", [glow_frame(32, 1.2)], [60.0])
    # A veil of mist that fades in and out over twelve seconds.
    veil = []
    for strength in (0.15, 0.5, 0.85, 1.0, 0.85, 0.5, 0.15, 0.0):
        frame = glow_frame(32, 1.6)
        frame[..., 3] *= strength
        veil.append(frame)
    write_sheet(folder, "veil", veil, [1.5, 1.5, 1.5, 3.0, 1.5, 1.5, 1.5, 60.0])
