#!/usr/bin/env python3
"""Generates the character sprites of Aetherbound: Prologue.

    /tmp/claude-0/bl/venv/bin/python tools/aetherbound/sprites/generate.py [name ...] [--sheets DIR]

Names: cael rowan lyra darius guard lancer swords (default: all; rowan also
writes rowan_bound.anim.json and rowan_glow.png). Output goes to
samples/aetherbound/sprites/. `--sheets DIR` also writes enlarged contact
sheets of every clip there, for looking at.

Each character is a small 3D figure made of ellipsoids and tapered capsules
(chars.py) on one posable rig (rig.py), posed per frame (clips.py), ray-cast
orthographically from four sides (render.py) and reduced to pixel art
(pixel.py, framer.py): a few flat tones per material from a fixed palette,
hard alpha, contact lines and a dark outline, with the face drawn by hand
on whole pixels. Needs numpy and pillow only.
"""
import json
import os
import sys
from multiprocessing import Pool

import numpy as np
from PIL import Image

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
ROOT = os.path.dirname(os.path.dirname(os.path.dirname(HERE)))
OUT = os.path.join(ROOT, "samples", "aetherbound", "sprites")

import render  # noqa: E402
import pixel  # noqa: E402
import framer  # noqa: E402
import extras  # noqa: E402
from chars import CHARS  # noqa: E402
from clips import CLIPS, frames_of  # noqa: E402

FRAME = render.SIZE
COLUMNS = 20
DIRECTIONS = ("south", "west", "north", "east")


def layout():
    """Where each clip's block of frames sits: {clip: (column, band)}."""
    place, col, band = {}, 0, 0
    for name, durations, _, _ in CLIPS:
        n = len(durations)
        if col + n > COLUMNS:
            col, band = 0, band + 1
        place[name] = (col, band)
        col += n
    return place, band + 1


def render_clip(job):
    name, clip = job
    c = CHARS[name]
    poses = frames_of(clip, c["weapon"])
    out = []
    for d in range(4):
        row = []
        for p in poses:
            rgba, px, marks, view = framer.render_frame(name, p, d)
            normal = pixel.normal_image(px)
            glow = extras.veins(px, marks, view) if name == "rowan" else None
            if px["clipped"]:
                print(f"  note: {name} {clip} {DIRECTIONS[d]} frame {len(row)} is cut by the frame edge: {px['clipped']}")
            row.append((rgba, normal, glow))
        out.append(row)
    return name, clip, out


def build(names, sheets=None):
    place, bands = layout()
    width, height = COLUMNS * FRAME, bands * 4 * FRAME
    jobs = [(n, clip[0]) for n in names for clip in CLIPS]
    with Pool(min(4, os.cpu_count() or 1)) as pool:
        results = pool.map(render_clip, jobs, chunksize=2)
    by = {}
    for name, clip, out in results:
        by.setdefault(name, {})[clip] = out
    os.makedirs(OUT, exist_ok=True)
    for name in names:
        atlas = np.zeros((height, width, 4), np.uint8)
        normal = np.zeros((height, width, 4), np.uint8)
        normal[..., :] = (128, 128, 255, 255)
        glow = np.zeros((height, width, 4), np.uint8)
        clips = {}
        for clip, durations, loop, events in CLIPS:
            col, band = place[clip]
            directions = []
            for d in range(4):
                frames = []
                for i, duration in enumerate(durations):
                    rgba, nrm, gl = by[name][clip][d][i]
                    x, y = (col + i) * FRAME, (band * 4 + d) * FRAME
                    atlas[y:y + FRAME, x:x + FRAME] = rgba
                    normal[y:y + FRAME, x:x + FRAME] = nrm
                    if gl is not None:
                        glow[y:y + FRAME, x:x + FRAME] = gl
                    f = {"rect": [x, y, FRAME, FRAME], "duration": duration}
                    if i in events:
                        f["event"] = events[i]
                    frames.append(f)
                directions.append(frames)
            clips[clip] = {"loop": loop, "directions": directions}
        Image.fromarray(atlas).save(os.path.join(OUT, name + ".png"), optimize=True)
        Image.fromarray(normal[..., :3]).save(os.path.join(OUT, name + "_n.png"), optimize=True)
        anim = {"atlas": name + ".png", "normal": name + "_n.png", "pixels_per_unit": 32, "pivot": [0.5, 1],
                "clips": clips}
        write_json(os.path.join(OUT, name + ".anim.json"), anim)
        if name == "rowan":
            Image.fromarray(glow).save(os.path.join(OUT, "rowan_glow.png"), optimize=True)
            bound = dict(anim)
            bound["emissive"] = "rowan_glow.png"
            write_json(os.path.join(OUT, "rowan_bound.anim.json"), bound)
        print("wrote", name, f"{width}x{height}")
        if sheets:
            write_sheets(name, by[name], sheets)


def write_json(path, data):
    with open(path, "w") as f:
        json.dump(data, f, separators=(",", ":"))
        f.write("\n")


def write_sheets(name, clips, folder, zoom=4, per_sheet=6):
    """Contact sheets: clips side by side (frames across, directions down)."""
    os.makedirs(folder, exist_ok=True)
    groups, cur, count = [], [], 0
    for clip, durations, _, _ in CLIPS:
        n = len(durations)
        if count + n > per_sheet and cur:
            groups.append(cur)
            cur, count = [], 0
        cur.append(clip)
        count += n
    if cur:
        groups.append(cur)
    cell = FRAME * zoom
    for gi, group in enumerate(groups):
        total = sum(len(clips[c][0]) for c in group)
        sheet = Image.new("RGBA", (total * cell, 4 * cell), (58, 60, 82, 255))
        x = 0
        for clip in group:
            for i in range(len(clips[clip][0])):
                for d in range(4):
                    im = Image.fromarray(clips[clip][d][i][0]).resize((cell, cell), Image.NEAREST)
                    if (x // cell + d) % 2:
                        tint = Image.new("RGBA", (cell, cell), (66, 68, 92, 255))
                        sheet.paste(tint, (x, d * cell))
                    sheet.paste(im, (x, d * cell), im)
                x += cell
        sheet.save(os.path.join(folder, f"{name}_{gi:02d}_{'_'.join(group)}.png"))


def main(argv):
    sheets = None
    if "--sheets" in argv:
        i = argv.index("--sheets")
        sheets = argv[i + 1]
        argv = argv[:i] + argv[i + 2:]
    known = list(CHARS) + ["swords"]
    names = argv or known
    for n in names:
        if n not in known:
            sys.exit(f"unknown name {n}; choose from {' '.join(known)}")
    chars = [n for n in names if n in CHARS]
    if chars:
        build(chars, sheets)
    if "swords" in names:
        extras.swords(OUT)
        print("wrote sword_broken, sword_half")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
