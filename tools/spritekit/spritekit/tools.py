"""Helpers for the scripts that generate sprite sheets, and the measurements
the checks are built on. numpy and pillow only.

    import sys; sys.path.insert(0, "tools/spritekit")
    from spritekit import tools as sk

    frames = [sk.harden_alpha(f) for f in frames]            # alpha is 0 or 255
    frames = [sk.remove_specks(f, 3) for f in frames]        # no stray islands
    frames = sk.quantize(frames, palette)                    # only the palette's colours
    frames = sk.align_baseline(frames)                       # the feet on one row
    atlas, rects = sk.pack(frames, cell=(64, 64), gutter=0)
    sk.write_anim_json("sprites/hero.anim.json", "hero.png", {
        "idle": sk.clip([rects[0:4]], 0.3),
        "walk": sk.clip([rects[4:10]], 0.1, events={0: "step", 3: "step"})}, 32, (0.5, 1), atlas=atlas)

A frame is a (height, width, 4) uint8 array, RGBA, straight alpha, row 0 at
the top: what `numpy.asarray(Image.open(...).convert("RGBA"))` gives. Every
function returns new arrays and leaves its input alone.
"""
import json
import math
import os

import numpy as np

OPAQUE_CUTOFF = 128   # an opaque sprite keeps alpha >= 0.5 (SpriteBatch.cpp OpaqueCutoff)
BLEND_CUTOFF = 3      # a blended sprite keeps alpha >= 0.01 (BlendCutoff)


# ------------------------------------------------------------------ measuring

def visible(frame, cutoff=1):
    """The mask of pixels the engine draws: alpha at or above the cutoff."""
    return np.asarray(frame)[..., 3] >= cutoff


def bounds(mask):
    """(top, bottom, left, right) of a mask, inclusive, or None when it is empty."""
    rows = np.flatnonzero(mask.any(axis=1))
    if not len(rows):
        return None
    columns = np.flatnonzero(mask.any(axis=0))
    return int(rows[0]), int(rows[-1]), int(columns[0]), int(columns[-1])


def baseline(mask, ignore=0):
    """The row the figure stands on: the lowest row of the mask, after
    leaving out the `ignore` lowest pixels (the tip of a scabbard or a cape
    hangs below the feet without being what the figure stands on). None for
    an empty mask."""
    per_row = mask.sum(axis=1)
    below = np.cumsum(per_row[::-1])[::-1]          # pixels at or below each row
    rows = np.flatnonzero((below > ignore) & (per_row > 0))
    if not len(rows):
        rows = np.flatnonzero(per_row > 0)
    return int(rows[-1]) if len(rows) else None


def centre_x(mask):
    """The horizontal centre of mass of a mask, in pixels from the left edge (None when empty)."""
    per_column = mask.sum(axis=0)
    total = per_column.sum()
    return float((per_column * (np.arange(mask.shape[1]) + 0.5)).sum() / total) if total else None


def components(mask, diagonal=True):
    """Labels the connected parts of a mask: returns (labels, sizes) where
    labels is 0 outside the mask and 1..n inside, and sizes[i] is the pixel
    count of label i + 1. `diagonal` joins pixels that touch by a corner
    (right for pixel-art figures; holes are counted without it)."""
    mask = np.asarray(mask, dtype=bool)
    height, width = mask.shape
    parent = [0]
    runs, previous = [], []
    padded = np.zeros((height, width + 2), dtype=np.int8)
    padded[:, 1:-1] = mask
    edges = np.diff(padded, axis=1)
    reach = 1 if diagonal else 0
    for y in range(height):
        starts, ends = np.flatnonzero(edges[y] == 1), np.flatnonzero(edges[y] == -1)
        current = []
        for start, end in zip(starts.tolist(), ends.tolist()):   # the run covers columns start..end-1
            label = len(parent)
            parent.append(label)
            for other_start, other_end, other in previous:
                if other_start < end + reach and other_end > start - reach:
                    a, b = _find(parent, label), _find(parent, other)
                    if a != b:
                        parent[max(a, b)] = min(a, b)
            current.append((start, end, label))
            runs.append((y, start, end, label))
        previous = current
    roots = {}
    labels = np.zeros((height, width), dtype=np.int32)
    for y, start, end, label in runs:
        root = _find(parent, label)
        labels[y, start:end] = roots.setdefault(root, len(roots) + 1)
    sizes = np.bincount(labels.ravel(), minlength=len(roots) + 1)[1:]
    return labels, sizes


def _find(parent, x):
    while parent[x] != x:
        parent[x] = parent[parent[x]]
        x = parent[x]
    return x


def specks(mask, max_pixels=3):
    """The mask of stray islands: parts of at most `max_pixels` pixels that
    are not joined (even by a corner) to anything larger. A frame that is
    nothing but small parts (a spark, a mote) has no specks."""
    labels, sizes = components(mask)
    if not len(sizes) or sizes.max() <= max_pixels:
        return np.zeros(mask.shape, dtype=bool)
    small = np.flatnonzero(sizes <= max_pixels) + 1
    return np.isin(labels, small)


def pinholes(mask, max_pixels=1):
    """The mask of pinholes: transparent parts of at most `max_pixels` pixels
    closed in on all four sides by the figure."""
    labels, sizes = components(~np.asarray(mask, dtype=bool), diagonal=False)
    if not len(sizes):
        return np.zeros(mask.shape, dtype=bool)
    border = np.unique(np.concatenate([labels[0], labels[-1], labels[:, 0], labels[:, -1]]))
    small = [label for label in (np.flatnonzero(sizes <= max_pixels) + 1).tolist() if label not in set(border.tolist())]
    return np.isin(labels, small)


def _blurred(frame, radius):
    """The frame as premultiplied colour and alpha, 0..1, box-blurred: what the eye keeps of a pose."""
    image = np.asarray(frame).astype(np.float64) / 255.0
    image[..., :3] *= image[..., 3:4]
    size = 2 * radius + 1
    padded = np.pad(image, ((radius + 1, radius), (radius + 1, radius), (0, 0)))
    total = padded.cumsum(axis=0).cumsum(axis=1)
    return (total[size:, size:] - total[:-size, size:] - total[size:, :-size] + total[:-size, :-size]) / (size * size)


def difference(a, b, radius=None):
    """How unlike two frames are, 0 (the same) to 1 (nothing in common),
    compared after a small blur so that a figure moved by one pixel is a
    small change and a different pose a large one. Frames of different
    sizes are compared on a common canvas, bottom centres together."""
    a, b = np.asarray(a), np.asarray(b)
    if a.shape != b.shape:
        height, width = max(a.shape[0], b.shape[0]), max(a.shape[1], b.shape[1])
        a, b = _on_canvas(a, width, height), _on_canvas(b, width, height)
    if radius is None:
        radius = max(1, min(a.shape[0], a.shape[1]) // 32)
    x, y = _blurred(a, radius), _blurred(b, radius)
    most = np.maximum(x, y).sum()
    return float(np.abs(x - y).sum() / most) if most > 0 else 0.0


def loop_seam(frames):
    """How well a loop closes: (seam, largest, ratio). `seam` is the change
    from the last frame back to the first, `largest` the biggest change
    between two consecutive frames inside the clip, and `ratio` seam /
    largest: 1 or less for a loop that closes (going round is no bigger a
    step than the clip already takes), well above 1 for one that jumps back.
    None for fewer than three frames (a two-frame loop always closes)."""
    if len(frames) < 3:
        return None
    steps = [difference(frames[i], frames[i + 1]) for i in range(len(frames) - 1)]
    seam = difference(frames[-1], frames[0])
    largest = float(max(steps))
    return seam, largest, seam / max(largest, 1e-6)


def colours(frames, cutoff=1):
    """The colours of the visible pixels of some frames: (colours as an (n, 3)
    array, pixel count of each), most used first."""
    pixels = [np.asarray(f)[..., :3][np.asarray(f)[..., 3] >= cutoff] for f in frames]
    pixels = np.concatenate(pixels) if pixels else np.zeros((0, 3), dtype=np.uint8)
    if not len(pixels):
        return np.zeros((0, 3), dtype=np.uint8), np.zeros(0, dtype=np.int64)
    packed = (pixels[:, 0].astype(np.int64) << 16) | (pixels[:, 1].astype(np.int64) << 8) | pixels[:, 2]
    values, counts = np.unique(packed, return_counts=True)
    order = np.argsort(-counts, kind="stable")
    values, counts = values[order], counts[order]
    return np.stack([(values >> 16) & 255, (values >> 8) & 255, values & 255], axis=1).astype(np.uint8), counts


def colour_distance(a, b):
    """How far apart two colours look, roughly: a weighted distance in sRGB
    ('redmean'), 0 to about 765. Works on arrays of colours."""
    a, b = np.asarray(a, dtype=np.float64), np.asarray(b, dtype=np.float64)
    mean_red = (a[..., 0] + b[..., 0]) / 2
    delta = a - b
    return np.sqrt((2 + mean_red / 256) * delta[..., 0] ** 2 + 4 * delta[..., 1] ** 2 + (2 + (255 - mean_red) / 256) * delta[..., 2] ** 2)


def near_duplicates(palette, distance=12.0):
    """Pairs of colours a viewer cannot tell apart: [(i, j)] with i the more
    used (lower index) of the two, for colours closer than `distance`."""
    palette = np.asarray(palette, dtype=np.float64)
    if len(palette) < 2:
        return []
    apart = colour_distance(palette[:, None, :], palette[None, :, :])
    first, second = np.nonzero(np.triu(apart < distance, k=1))
    return list(zip(first.tolist(), second.tolist()))


# ------------------------------------------------------------------ cleaning frames

def harden_alpha(frame, threshold=OPAQUE_CUTOFF):
    """Alpha becomes 0 or 255: pixels at or above the threshold are kept solid,
    the rest are cleared (colour too). 128 is where the engine's opaque
    sprites cut, so the frame looks the same in the game as in the file."""
    out = np.array(frame, dtype=np.uint8)
    keep = out[..., 3] >= threshold
    out[..., 3] = np.where(keep, 255, 0)
    out[~keep, :3] = 0
    return out


def clear_hidden_colour(frame):
    """Colour under fully transparent pixels set to black: nothing the engine
    draws changes, the PNG gets smaller, and an editor that shows RGB alone
    shows no ghost of an old frame."""
    out = np.array(frame, dtype=np.uint8)
    out[out[..., 3] == 0, :3] = 0
    return out


def remove_specks(frame, min_pixels=4, cutoff=1):
    """Erases islands of fewer than `min_pixels` pixels that are detached from
    the figure."""
    out = np.array(frame, dtype=np.uint8)
    out[specks(visible(out, cutoff), min_pixels - 1)] = 0
    return out


def fill_pinholes(frame, max_pixels=1, cutoff=1):
    """Fills transparent holes of at most `max_pixels` pixels inside the
    figure with the most common colour among each hole pixel's neighbours."""
    out = np.array(frame, dtype=np.uint8)
    solid = visible(out, cutoff)
    holes = pinholes(solid, max_pixels)
    for y, x in zip(*np.nonzero(holes)):
        around = [out[y + dy, x + dx] for dy, dx in ((-1, 0), (1, 0), (0, -1), (0, 1))
                  if 0 <= y + dy < out.shape[0] and 0 <= x + dx < out.shape[1] and solid[y + dy, x + dx]]
        if around:
            values, counts = np.unique(np.array(around), axis=0, return_counts=True)
            out[y, x] = values[np.argmax(counts)]
            out[y, x, 3] = 255
    return out


def quantize(frames, palette_or_count, cutoff=1):
    """Maps every visible pixel of the frames to a palette. Give the palette
    (a list of (r, g, b) or '#rrggbb') and each pixel takes its nearest
    colour; give a number and the frames' own colours are reduced to that
    many, the most used first, each of the others folded into the kept
    colour nearest to it. All frames are treated together so a sheet shares
    one palette."""
    frames = [np.array(f, dtype=np.uint8) for f in frames]
    if isinstance(palette_or_count, int):
        used, _ = colours(frames, cutoff)
        palette = used[:palette_or_count].astype(np.float64)
    else:
        palette = np.array([_rgb(c) for c in palette_or_count], dtype=np.float64)
    if not len(palette):
        return frames
    for frame in frames:
        solid = frame[..., 3] >= cutoff
        pixels = frame[solid][:, :3]
        if not len(pixels):
            continue
        values, inverse = np.unique(pixels, axis=0, return_inverse=True)
        nearest = np.argmin(colour_distance(values[:, None, :].astype(np.float64), palette[None, :, :]), axis=1)
        frame[solid, :3] = palette[nearest][inverse.reshape(-1)].astype(np.uint8)
    return frames


def merge_near_duplicates(frames, distance=12.0, cutoff=1):
    """Folds every colour that has a more used colour within `distance` of it
    into that colour, so clouds of almost-equal tones become one tone."""
    frames = [np.array(f, dtype=np.uint8) for f in frames]
    used, _ = colours(frames, cutoff)
    keep = []
    for colour in used.astype(np.float64):
        if not keep or colour_distance(np.array(keep), colour[None, :]).min() >= distance:
            keep.append(colour)
    return quantize(frames, [tuple(int(v) for v in c) for c in keep], cutoff)


def _rgb(colour):
    if isinstance(colour, str):
        colour = colour.lstrip("#")
        return tuple(int(colour[i:i + 2], 16) for i in (0, 2, 4))
    return tuple(int(v) for v in colour[:3])


# ------------------------------------------------------------------ placing frames

def _on_canvas(frame, width, height, x=None, y=None):
    """The frame on a transparent canvas: by default bottom centre to bottom centre."""
    out = np.zeros((height, width, 4), dtype=np.uint8)
    h, w = frame.shape[:2]
    x = (width - w) // 2 if x is None else x
    y = height - h if y is None else y
    sx, sy = max(0, -x), max(0, -y)
    ex, ey = min(w, width - x), min(h, height - y)
    if ex > sx and ey > sy:
        out[y + sy:y + ey, x + sx:x + ex] = frame[sy:ey, sx:ex]
    return out


def shift(frame, dx=0, dy=0):
    """The frame's content moved by whole pixels (right and down are positive); what leaves the frame is lost."""
    frame = np.asarray(frame)
    return _on_canvas(frame, frame.shape[1], frame.shape[0], dx, dy)


def align_baseline(frames, row=None, ignore=0, cutoff=1):
    """Moves each frame's content up or down so every figure stands on the
    same row: `row` (counted from the top), or by default the lowest row any
    of them reaches. `ignore` is as in baseline(): how many of the lowest
    pixels do not count as feet. Empty frames are left alone."""
    found = [baseline(visible(f, cutoff), ignore) for f in frames]
    known = [b for b in found if b is not None]
    if not known:
        return [np.array(f, dtype=np.uint8) for f in frames]
    row = max(known) if row is None else row
    return [np.array(f, dtype=np.uint8) if b is None else shift(f, 0, row - b) for f, b in zip(frames, found)]


def trim_and_centre(frames, size=None, margin=1, cutoff=1):
    """Cuts the frames down to what they use and puts them on cells of one
    size: the figures keep their positions relative to each other (so the
    animation is untouched), the group is centred left to right and stands
    `margin` pixels above the cell's bottom edge, with at least `margin`
    transparent pixels on the other sides. `size` is (width, height) of the
    cell; by default the smallest that fits."""
    frames = [np.asarray(f) for f in frames]
    height, width = max(f.shape[0] for f in frames), max(f.shape[1] for f in frames)
    frames = [_on_canvas(f, width, height) for f in frames]
    union = np.zeros((height, width), dtype=bool)
    for frame in frames:
        union |= visible(frame, cutoff)
    box = bounds(union)
    if box is None:
        return frames
    top, bottom, left, right = box
    used_w, used_h = right - left + 1, bottom - top + 1
    cell_w, cell_h = size if size else (used_w + 2 * margin, used_h + 2 * margin)
    if used_w + 2 * margin > cell_w or used_h + 2 * margin > cell_h:
        raise ValueError(f"the frames use {used_w}x{used_h} pixels; with a margin of {margin} they do not fit {cell_w}x{cell_h}")
    x = (cell_w - used_w) // 2 - left
    y = cell_h - margin - 1 - bottom
    return [_on_canvas(f, cell_w, cell_h, x, y) for f in frames]


def mirror_direction(frames, normal=False):
    """The frames flipped left to right: a west row from an east row. With
    `normal=True` the frames are normal maps, whose red channel (the normal's
    x) is turned around as well. Only right for a figure that is the same on
    both sides: a sword in the right hand changes hands."""
    out = []
    for frame in frames:
        flipped = np.array(np.asarray(frame)[:, ::-1], dtype=np.uint8)
        if normal:
            flipped[..., 0] = 255 - flipped[..., 0]
        out.append(flipped)
    return out


def normal_from_height(height, strength=2.0, mask=None):
    """A normal map in the engine's convention (red = right, green = up, blue
    = towards the viewer, all three 0..255 around 128) from a height field: a
    (height, width) array where larger is nearer the viewer, in pixels.
    `strength` scales the relief. Outside `mask` (the figure's visible
    pixels) the normal is flat (128, 128, 255); slopes are taken inside the
    mask only, so the figure's edge does not tilt towards the background."""
    h = np.asarray(height, dtype=np.float64)
    inside = np.ones(h.shape, dtype=bool) if mask is None else np.asarray(mask, dtype=bool)

    def slope(axis):
        ahead, behind = np.roll(h, -1, axis=axis), np.roll(h, 1, axis=axis)
        ahead_in, behind_in = np.roll(inside, -1, axis=axis), np.roll(inside, 1, axis=axis)
        edge = [slice(None), slice(None)]
        for index in (0, -1):   # nothing wraps round the image
            edge[axis] = index
            (behind_in if index == 0 else ahead_in)[tuple(edge)] = False
        ahead = np.where(ahead_in & inside, ahead, h)
        behind = np.where(behind_in & inside, behind, h)
        steps = (ahead_in & inside).astype(np.float64) + (behind_in & inside)
        return np.where(steps > 0, (ahead - behind) / np.maximum(steps, 1), 0.0)

    dx, dy = slope(1), slope(0)                      # dy is along rows, which run down
    normal = np.stack([-dx * strength, dy * strength, np.ones_like(h)], axis=2)
    normal /= np.linalg.norm(normal, axis=2, keepdims=True)
    normal[~inside] = (0.0, 0.0, 1.0)
    out = np.empty(h.shape + (4,), dtype=np.uint8)
    out[..., :3] = np.clip(np.round(normal * 127.5 + 127.5), 0, 255).astype(np.uint8)
    out[..., 3] = 255
    return out


# ------------------------------------------------------------------ the sheet

def pack(frames, cell=None, gutter=0, columns=None, power_of_two=False):
    """Lays frames out on a grid atlas. Returns (atlas, rects): the RGBA atlas
    and one [x, y, width, height] per frame, in order. Every frame gets a
    cell of `cell` = (width, height) (by default the largest frame), a
    smaller frame standing at the cell's bottom centre, so one pivot serves
    them all. `gutter` is the number of transparent pixels kept between
    cells and around the atlas; the engine samples sprites without
    filtering, so 0 is safe as long as each figure keeps a transparent
    pixel inside its own frame (see the guide: outlines). `columns` fixes the
    cells per row (by default close to square); `power_of_two` rounds the
    atlas up to powers of two."""
    frames = [np.asarray(f) for f in frames]
    if not frames:
        raise ValueError("no frames to pack")
    cell_w, cell_h = cell if cell else (max(f.shape[1] for f in frames), max(f.shape[0] for f in frames))
    for index, frame in enumerate(frames):
        if frame.shape[1] > cell_w or frame.shape[0] > cell_h:
            raise ValueError(f"frame {index} is {frame.shape[1]}x{frame.shape[0]}: larger than the {cell_w}x{cell_h} cell")
    if columns is None:
        columns = max(1, math.ceil(math.sqrt(len(frames) * (cell_h + gutter) / (cell_w + gutter))))
    rows = math.ceil(len(frames) / columns)
    width, height = gutter + columns * (cell_w + gutter), gutter + rows * (cell_h + gutter)
    if power_of_two:
        width, height = 1 << (width - 1).bit_length(), 1 << (height - 1).bit_length()
    atlas = np.zeros((height, width, 4), dtype=np.uint8)
    rects = []
    for index, frame in enumerate(frames):
        x = gutter + (index % columns) * (cell_w + gutter)
        y = gutter + (index // columns) * (cell_h + gutter)
        atlas[y:y + cell_h, x:x + cell_w] = _on_canvas(frame, cell_w, cell_h)
        rects.append([x, y, cell_w, cell_h])
    return atlas, rects


def clip(directions, durations, loop=True, events=None):
    """One clip in the engine's form. `directions` is a list of rows (1, 4 or
    8: south first, then clockwise as the engine orders them), each a list
    of [x, y, w, h] rects; `durations` is seconds per frame, one number for
    all or a list; `events` maps a frame index to the event that frame
    fires, in every direction."""
    count = len(directions[0])
    seconds = [float(durations)] * count if isinstance(durations, (int, float)) else [float(d) for d in durations]
    if len(seconds) != count or any(len(row) != count for row in directions):
        raise ValueError("every direction needs the same number of frames, and a duration for each")
    rows = []
    for row in directions:
        frames = []
        for index, rect in enumerate(row):
            frame = {"rect": [int(v) for v in rect], "duration": seconds[index]}
            if events and index in events:
                frame["event"] = events[index]
            frames.append(frame)
        rows.append(frames)
    return {"loop": bool(loop), "directions": rows}


def write_anim_json(path, atlas_name, clips, pixels_per_unit, pivot, normal=None, emissive=None, atlas=None,
                    normal_image=None, emissive_image=None):
    """Writes an .anim.json exactly as the engine reads it, and with `atlas`
    (and `normal_image`, `emissive_image`) given as arrays, the PNGs next to
    it under the names given. `clips` is {name: clip(...)}. The result is
    read back the way the engine reads it; anything it would refuse raises
    ValueError here instead of failing in the game."""
    from PIL import Image
    from . import sheet as sheets
    folder = os.path.dirname(os.path.abspath(path))
    os.makedirs(folder, exist_ok=True)
    data = {"atlas": atlas_name}
    if normal:
        data["normal"] = normal
    if emissive:
        data["emissive"] = emissive
    data["pixels_per_unit"] = int(pixels_per_unit)
    data["pivot"] = [float(pivot[0]), float(pivot[1])]
    data["clips"] = clips
    for name, image in ((atlas_name, atlas), (normal, normal_image), (emissive, emissive_image)):
        if name and image is not None:
            Image.fromarray(np.asarray(image, dtype=np.uint8)).save(os.path.join(folder, name), optimize=True)
    with open(path, "w", encoding="utf-8") as out:
        json.dump(data, out, separators=(",", ":"))
        out.write("\n")
    loaded = sheets.load(path)
    refused = [f"{message}" + (f" ({where})" if where else "") for check, severity, message, where, _ in loaded.problems
               if severity == "error" and (check == "sheet.json" or atlas is not None)]
    if refused:
        raise ValueError(f"{path}: " + "; ".join(refused))
    return data
