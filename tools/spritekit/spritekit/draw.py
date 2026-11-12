"""The review sheets: what a reviewer looks at to judge a sprite sheet.

Page 1 is the overview: the sprite among the others of its profile (the
line-up), its silhouette, its size next to a hero-high bar, its palette, its
normal map lit from three sides, its emissive image, shots from the real
renderer when an engine is given, and every finding with the questions to
answer. The pages after it are the clips: each as a filmstrip per direction,
enlarged without filtering, with the frame's boundary, the pivot and the
ground row drawn in, an onion skin of all its frames and, for a loop, the
seam (the last frame beside the first).
"""
import json
import os
import shutil
import subprocess
import tempfile

import numpy as np
from PIL import Image, ImageDraw

from kitcommon import kit as common

from . import checks, sheet as sheets, tools

PAPER = (86, 88, 112)        # behind pixel art: dark outlines and light pixels both read on it
NIGHT = (16, 17, 24)         # behind soft, additive images: about what the game puts behind them
INK = (20, 21, 28)
GUIDE_FRAME, GUIDE_GROUND, GUIDE_PIVOT = (120, 124, 150), (90, 230, 230), (255, 90, 200)
MARK_EDGE, MARK_HOLE, MARK_SPECK = (255, 60, 60), (255, 0, 255), (255, 230, 0)
PAGE_WIDTH, PAGE_HEIGHT = 1900, 1830


class Pages:
    """A review that is several images. review.py saves 'the sheet' to one
    path; the first page goes there and the others beside it as _2, _3 ..."""

    def __init__(self, images):
        self.images = images

    def save(self, path):
        stem, extension = os.path.splitext(path)
        self.images[0].save(path)
        for index, image in enumerate(self.images[1:], 2):
            image.save(f"{stem}_{index}{extension}")
            print(f"wrote {stem}_{index}{extension}")
        index = len(self.images) + 1
        while os.path.isfile(f"{stem}_{index}{extension}"):   # pages of an older, longer review
            os.remove(f"{stem}_{index}{extension}")
            index += 1


# ------------------------------------------------------------------ small pieces

def _zoom(array, factor):
    """An RGB or RGBA array enlarged by a whole factor without filtering."""
    return np.repeat(np.repeat(array, factor, axis=0), factor, axis=1)


def _over(pixels, background):
    """RGBA pixels over a flat background, as an RGB array."""
    alpha = pixels[..., 3:4].astype(np.float32) / 255.0
    return (pixels[..., :3] * alpha + np.array(background, dtype=np.float32) * (1 - alpha)).astype(np.uint8)


def _fit(pixels, box):
    """The largest whole zoom that keeps a frame inside `box` pixels (at least 1), or a fraction for frames larger than it."""
    height, width = pixels.shape[:2]
    if max(width, height) <= box:
        return max(1, box // max(width, height))
    return box / max(width, height)


def _scaled(rgb, factor):
    if factor >= 1:
        return Image.fromarray(_zoom(rgb, int(factor)))
    image = Image.fromarray(rgb)
    return image.resize((max(1, round(image.width * factor)), max(1, round(image.height * factor))), Image.BOX)


def _silhouette(pixels, cutoff):
    out = np.full(pixels.shape[:2] + (3,), 232, dtype=np.uint8)
    out[pixels[..., 3] >= cutoff] = (0, 0, 0)
    return out


def _first_frame(sheet, row=0):
    clip = sheet.clips.get("idle") or (sheet.clips[sorted(sheet.clips)[0]] if sheet.clips else None)
    if clip is None or sheet.atlas is None:
        return None
    frames = [f for f in clip.directions[min(row, len(clip.directions) - 1)] if f.inside]
    # The fullest frame: an effect's first frame is often a spark.
    return max(frames, key=lambda f: int((sheet.pixels(f)[..., 3] > 8).sum())) if frames else None


def _tile(image, caption):
    out = Image.new("RGB", (max(image.width, 8 + 6 * len(caption)), image.height + 16), INK)
    out.paste(image, (0, 16))
    return common.label(out, caption)


def _row(images, gap=6, background=INK):
    width = sum(i.width for i in images) + gap * (len(images) + 1)
    height = max(i.height for i in images) + 2 * gap
    out = Image.new("RGB", (width, height), background)
    x = gap
    for image in images:
        out.paste(image, (x, height - gap - image.height))
        x += image.width + gap
    return out


# ------------------------------------------------------------------ overview tiles

def lineup(sheet, profile, others, cutoff, background):
    """This sprite's resting frame among the others of its profile, at one scale, in colour and in black."""
    grounded = bool(profile.get("grounded"))
    entries = [("this", sheet)]
    for key, path in others[:11]:
        try:
            entries.append((os.path.basename(key)[:-len(".anim.json")], sheets.load(path)))
        except OSError:
            continue
    colour, black, names = [], [], []
    for name, other in entries:
        frame = _first_frame(other)
        if frame is None:
            continue
        pixels = other.pixels(frame)
        # Grounded figures at one world scale (3 screen px per pixel at 32 px per unit); anything else fitted to a box.
        factor = max(1, round(3 * 32 / other.pixels_per_unit)) if grounded and max(pixels.shape[:2]) * 3 * 32 / other.pixels_per_unit <= 420 \
            else _fit(pixels, 132)
        colour.append(_scaled(_over(pixels, background), factor))
        black.append(_scaled(_silhouette(pixels, cutoff), factor))
        names.append(name)
    if not colour:
        return None
    strip = Image.new("RGB", (sum(i.width for i in colour) + 4 * (len(colour) + 1), max(i.height for i in colour) * 2 + 30), INK)
    draw = ImageDraw.Draw(strip)
    x, tall = 4, max(i.height for i in colour)
    for index, (image, shadow, name) in enumerate(zip(colour, black, names)):
        strip.paste(image, (x, tall - image.height))
        strip.paste(shadow, (x, 2 * tall + 4 - shadow.height))
        if index == 0:
            draw.rectangle([x - 2, 0, x + image.width + 1, 2 * tall + 5], outline=(255, 220, 140))
        draw.text((x + 2, 2 * tall + 12), name[:max(4, image.width // 6)], fill=(255, 220, 140) if index == 0 else (200, 204, 216))
        x += image.width + 4
    kind = "one world scale" if grounded else "each fitted to a box"
    return _tile(strip, f"line-up: the '{profile['name']}' sprites of the manifest, resting frame, {kind}; this one framed")


def game_size(sheet, profile, cutoff, background):
    """The sprite at 1x and 2x, in colour and black, beside a bar the height of a hero (1.5 units)."""
    frame = _first_frame(sheet)
    if frame is None:
        return None
    pixels = sheet.pixels(frame)
    if max(pixels.shape[:2]) > 300:
        return None
    bar = max(1, round(1.5 * sheet.pixels_per_unit))
    parts = []
    for factor in (1, 2):
        for image in (_over(pixels, background), _silhouette(pixels, cutoff)):
            parts.append(Image.fromarray(_zoom(image, factor)))
        ruler = Image.new("RGB", (6, bar * factor), (230, 60, 60))
        parts.append(ruler)
    return _tile(_row(parts, 8), f"game size 1x and 2x; the red bar is a hero (1.5 units = {bar} px)")


def silhouettes(sheet, cutoff):
    clip = sheet.clips.get("idle") or (sheet.clips[sorted(sheet.clips)[0]] if sheet.clips else None)
    if clip is None:
        return None
    parts = []
    for row in range(len(clip.directions)):
        frame = clip.directions[row][0]
        if frame.inside:
            pixels = sheet.pixels(frame)
            parts.append(_scaled(_silhouette(pixels, cutoff), _fit(pixels, 200) if max(pixels.shape[:2]) > 66 else 3))
    return _tile(_row(parts[:8]), "silhouette of the resting frame, each direction") if parts else None


def palette_tile(sheet, profile):
    palette = getattr(sheet, "palette", None)
    if palette is None or not len(palette[0]):
        return None
    colours, counts = palette
    total = counts.sum()
    shown = min(len(colours), 64)
    columns = 8
    rows = (shown + columns - 1) // columns
    image = Image.new("RGB", (columns * 74 + 4, rows * 30 + 4), INK)
    draw = ImageDraw.Draw(image)
    twins = set()
    if profile.get("near_duplicate_distance") and len(colours) <= 1024:
        for i, j in tools.near_duplicates(colours, profile["near_duplicate_distance"]):
            twins.update((i, j))
    for index in range(shown):
        x, y = 4 + (index % columns) * 74, 4 + (index // columns) * 30
        draw.rectangle([x, y, x + 26, y + 24], fill=tuple(int(v) for v in colours[index]),
                       outline=MARK_EDGE if index in twins else (60, 62, 80))
        share = 100.0 * counts[index] / total
        draw.text((x + 30, y + 6), f"{share:.1f}%" if share >= 0.1 else f"{counts[index]}px", fill=(200, 204, 216))
    more = f", the {shown} most used" if len(colours) > shown else ""
    return _tile(image, f"palette: {len(colours):,} colours{more}, by share of pixels; red box = has a near twin")


def lit_normals(sheet, cutoff):
    """The resting frame of up to two directions shaded by the normal map under a light from the left, the right and above."""
    if sheet.normal is None or sheet.atlas is None or sheet.normal.shape != sheet.atlas.shape:
        return None
    clip = sheet.clips.get("idle") or sheet.clips[sorted(sheet.clips)[0]]
    parts = []
    for row in sorted({0, min(1, len(clip.directions) - 1)}):
        frame = clip.directions[row][0]
        if not frame.inside:
            continue
        pixels, encoded = sheet.pixels(frame), sheet.pixels(frame, sheet.normal)
        normal = encoded[..., :3].astype(np.float32) / 127.5 - 1.0
        normal[..., 1] *= -1                                    # green is up; rows run down
        length = np.linalg.norm(normal, axis=2, keepdims=True)
        normal = normal / np.maximum(length, 1e-6)
        factor = _fit(pixels, 200) if max(pixels.shape[:2]) > 66 else 3
        for light in ((-0.85, 0.0, 0.5), (0.85, 0.0, 0.5), (0.0, -0.85, 0.5)):   # x right, y down, z to the viewer
            direction = np.array(light, dtype=np.float32) / np.linalg.norm(light)
            shade = np.clip((normal * direction).sum(axis=2), 0, 1)[..., None]
            lit = np.clip(pixels[..., :3].astype(np.float32) * (0.22 + 1.1 * shade), 0, 255).astype(np.uint8)
            lit = np.concatenate([lit, pixels[..., 3:4]], axis=2)
            parts.append(_scaled(_over(lit, (30, 31, 42)), factor))
        raw = np.concatenate([encoded[..., :3], np.where(pixels[..., 3:4] >= cutoff, 255, 70).astype(np.uint8)], axis=2)
        parts.append(_scaled(_over(raw, (30, 31, 42)), factor))
    return _tile(_row(parts), "normal map: lit from the left, the right, above; then the map itself (dim outside the figure)") if parts else None


def emissive_tile(sheet):
    if sheet.emissive is None or sheet.atlas is None or sheet.emissive.shape != sheet.atlas.shape:
        return None
    frame = _first_frame(sheet)
    clip = sheet.clips.get("idle") or sheet.clips[sorted(sheet.clips)[0]]
    parts = []
    for row in range(min(4, len(clip.directions))):
        frame = clip.directions[row][0]
        if not frame.inside:
            continue
        pixels, glow = sheet.pixels(frame), sheet.pixels(frame, sheet.emissive)
        base = _over(pixels, (12, 12, 18)).astype(np.float32) * 0.35
        added = glow[..., :3].astype(np.float32) * (glow[..., 3:4].astype(np.float32) / 255.0)
        parts.append(_scaled(np.clip(base + added, 0, 255).astype(np.uint8), _fit(pixels, 200) if max(pixels.shape[:2]) > 66 else 3))
    return _tile(_row(parts), "emissive image added to the dimmed figure, resting frame per direction") if parts else None


# ------------------------------------------------------------------ the clips

def _marks(rgb, sheet, frame, profile, measured, factor):
    """Colours the flaws the checks measured onto an enlarged frame: edge contact, pinholes, specks."""
    m = measured.get(frame.rect)
    if m is None or factor < 2:
        return rgb
    height, width = m.mask.shape
    marks = []
    if profile.get("pinhole_pixels"):
        marks.append((tools.pinholes(m.mask, profile["pinhole_pixels"]), MARK_HOLE))
    if profile.get("speck_pixels"):
        marks.append((tools.specks(m.mask, profile["speck_pixels"]), MARK_SPECK))
    margin = profile.get("frame_margin")
    if margin and min(width, height) > 2 * margin:
        level = tools.OPAQUE_CUTOFF if profile.get("alpha") == "hard" else profile.get("edge_alpha", 24)
        ring = np.ones((height, width), dtype=bool)
        ring[margin:height - margin, margin:width - margin] = False
        marks.append((ring & (m.alpha >= level), MARK_EDGE))
    for mask, colour in marks:
        if mask.any():
            rgb[_zoom(mask, factor)] = colour
    return rgb


def clip_block(sheet, clip, profile, measured, background, cell):
    """One clip: a row per direction of its frames, then the onion skin and, for a loop, the seam."""
    hard = profile.get("alpha") == "hard"
    grounded = bool(profile.get("grounded"))
    idle = sheet.clips.get("idle")
    frames0 = clip.directions[0]
    width, height = frames0[0].rect[2], frames0[0].rect[3]
    factor = _fit(np.zeros((height, width)), cell)
    step_x, step_y = round(width * factor) + 4, round(height * factor) + 16
    extras = 1 + (2 if clip.loop and len(frames0) >= 3 else 0) if len(frames0) > 1 else 0
    columns = len(frames0) + extras
    block = Image.new("RGB", (columns * step_x + 6, len(clip.directions) * step_y + 18), INK)
    draw = ImageDraw.Draw(block)
    spec = checks.clip_spec(profile, clip.name)
    head = f"{clip.name}: {len(frames0)} fr, {clip.length(0):.2f} s, {'loop' if clip.loop else 'once'}"
    if spec.get("stance"):
        head += f", {spec['stance']}"
    draw.text((4, 2), head[:max(12, (block.width - 6) // 6)], fill=(255, 220, 140))
    for row, frames in enumerate(clip.directions):
        y = 18 + row * step_y
        ground = None
        if grounded and idle is not None and spec.get("stance") in ("upright", "low"):
            reference = [measured[f.rect].lowest for f in idle.directions[min(row, len(idle.directions) - 1)]
                         if f.rect in measured and not measured[f.rect].empty]
            ground = int(np.median(reference)) + 1 if reference else None
        stack = []
        for index, frame in enumerate(frames):
            x = 3 + index * step_x
            if not frame.inside:
                draw.rectangle([x, y, x + step_x - 5, y + step_y - 17], outline=MARK_EDGE)
                continue
            pixels = sheet.pixels(frame)
            stack.append(pixels)
            rgb = _over(pixels, background)
            if factor >= 1:
                rgb = _marks(_zoom(rgb, int(factor)), sheet, frame, profile, measured, int(factor))
                tile = Image.fromarray(rgb)
            else:
                tile = _scaled(rgb, factor)
            block.paste(tile, (x, y))
            draw.rectangle([x - 1, y - 1, x + tile.width, y + tile.height], outline=GUIDE_FRAME)
            if ground is not None and factor >= 1:
                draw.line([x, y + ground * factor, x + tile.width - 1, y + ground * factor], fill=GUIDE_GROUND)
            px, py = x + sheet.pivot[0] * width * factor, y + sheet.pivot[1] * height * factor
            draw.line([px, py - 5, px, py + 2], fill=GUIDE_PIVOT)
            draw.line([px - 4, min(py, y + tile.height), px + 4, min(py, y + tile.height)], fill=GUIDE_PIVOT)
            note = f"{frame.duration * 1000:.0f}ms" + (f" !{frame.event}" if frame.event else "")
            draw.text((x + 1, y + tile.height + 2), note[:max(4, step_x // 6)], fill=(255, 150, 90) if frame.event else (150, 154, 170))
        if extras and stack:
            x = 3 + len(frames) * step_x
            onion = np.mean([_over(p, background).astype(np.float32) for p in stack], axis=0).astype(np.uint8)
            tile = _scaled(onion, factor)
            block.paste(tile, (x, y))
            draw.rectangle([x - 1, y - 1, x + tile.width, y + tile.height], outline=(200, 170, 90))
            if ground is not None and factor >= 1:
                draw.line([x, y + ground * factor, x + tile.width - 1, y + ground * factor], fill=GUIDE_GROUND)
            draw.text((x + 1, y + tile.height + 2), "onion", fill=(200, 170, 90))
            if extras == 3 and len(stack) == len(frames):
                seam = tools.loop_seam(stack)
                for offset, pixels in enumerate((stack[-1], stack[0])):
                    sx = x + (1 + offset) * step_x - (4 if offset else 0)
                    block.paste(_scaled(_over(pixels, background), factor), (sx, y))
                bad = seam and seam[2] > profile.get("loop_seam_ratio", 1.5)
                draw.rectangle([x + step_x - 1, y - 1, x + 3 * step_x - 8, y + tile.height], outline=MARK_EDGE if bad else (120, 200, 120))
                draw.text((x + step_x + 1, y + tile.height + 2), f"seam last>first x{seam[2]:.1f}" if seam else "seam",
                          fill=MARK_EDGE if bad else (120, 200, 120))
        if len(clip.directions) > 1:
            draw.text((block.width - 6 * len(clip.direction_name(row)) - 4, y + 1), clip.direction_name(row), fill=(120, 124, 150))
    return block


def clip_pages(sheet, profile, measured, title, background):
    """The clips flowed into pages: blocks side by side in bands, bands down the page."""
    blocks = []
    sizes = [max(f.rect[2], f.rect[3]) for f in sheet.frames()]
    largest = max(sizes) if sizes else 64
    cell = 128 if largest > 48 else 96
    widest = max(len(c.directions[0]) for c in sheet.clips.values()) + 3
    if widest * (cell + 4) > PAGE_WIDTH:
        cell = max(24, PAGE_WIDTH // widest - 4)
    for clip in sheet.clips.values():
        blocks.append(clip_block(sheet, clip, profile, measured, background, cell))
    # Each block goes into the first band that still has room for it, so short clips fill the space beside long ones.
    pages, bands = [], []
    for block in blocks:
        for band in bands:
            if sum(b.width for b in band) + block.width <= PAGE_WIDTH:
                band.append(block)
                break
        else:
            bands.append([block])
    current, used = [], 18
    for band in bands:
        tall = max(b.height for b in band)
        if current and used + tall > PAGE_HEIGHT:
            pages.append(current)
            current, used = [], 18
        current.append(band)
        used += tall
    if current:
        pages.append(current)
    legend = ("grey box: the frame   cyan line: idle's ground row   pink: the pivot   red pixels: content against the frame edge   "
              "magenta: pinholes   yellow: specks   orange: an event")
    images = []
    for number, page in enumerate(pages, 1):
        width = max(sum(b.width for b in band) for band in page)
        height = 34 + sum(max(b.height for b in band) for band in page)
        image = Image.new("RGB", (max(width, 6 * len(legend) + 8), height), INK)
        draw = ImageDraw.Draw(image)
        draw.text((4, 3), f"{title}   clips, page {number} of {len(pages)}", fill=(255, 220, 140))
        draw.text((4, 18), legend, fill=(150, 154, 170))
        y = 34
        for band in page:
            x = 0
            for block in band:
                image.paste(block, (x, y))
                x += block.width
            y += max(b.height for b in band)
        images.append(image)
    return images


# ------------------------------------------------------------------ the engine

def engine_shots(sheet, profile, engine, here):
    """Shots of the sprite in the real renderer on this kit's stage: lit by a
    point light from the left and from the right, with the engine's outline,
    each direction, and at the game's camera distance. Returns [(image, caption)]."""
    work = tempfile.mkdtemp(prefix="spritekit-shots-")
    game = os.path.join(work, "game")
    shutil.copytree(os.path.join(here, "stage"), game, ignore=shutil.ignore_patterns("*.py", "__pycache__"))
    os.makedirs(os.path.join(game, "sprites"))
    name = os.path.basename(sheet.path)
    for source in sheet.files():
        if os.path.isfile(source):
            shutil.copy(source, os.path.join(game, "sprites", os.path.basename(source)))
    character = bool(profile.get("grounded"))
    hard = profile.get("alpha") == "hard"
    frame = _first_frame(sheet)
    tall = (frame.rect[3] if frame else 64) / sheet.pixels_per_unit
    wide = (frame.rect[2] if frame else 64) / sheet.pixels_per_unit
    figure = getattr(sheet, "figure", None)
    size = min(max(tall, wide), figure[1] * 1.6) if figure else max(tall, wide)
    close = max(2.0, size * 2.4)
    clips = list(sheet.clips)
    moving = next((c for c in ("walk", "run", "attack") if c in sheet.clips), clips[0])
    middle = sheet.clips[moving].length(0) * 0.5
    sprite = {"mode": "standing" if character else ("upright" if hard else "facing"), "blend": "opaque" if hard else "additive",
              "unlit": not hard}
    sprite.update(profile.get("stage_sprite") or {})     # how the game really draws it, when that is not the default (a multiply decal)
    directions = max(len(c.directions) for c in sheet.clips.values())
    first = sheet.clips[clips[0]] if "idle" not in sheet.clips else sheet.clips["idle"]
    third = first.length(0) / 3 if len(first.directions[0]) > 1 else 0
    if hard:
        shots = [{"caption": "engine: resting clip, point light on the left", "clip": "idle", "facing": [0, 1], "light": [-1.5, 1.0, 0.4],
                  "distance": close},
                 {"caption": "engine: the same, point light on the right", "clip": "idle", "facing": [0, 1], "light": [1.5, 1.0, 0.4],
                  "distance": close},
                 {"caption": f"engine: '{moving}' halfway, with the engine's outline", "clip": moving, "at": middle,
                  "facing": [-1, 0] if directions > 1 else [0, 1], "light": [-1.5, 1.0, 0.4], "distance": close, "outline": True}]
    else:
        shots = [{"caption": f"engine: drawn {sprite['blend']}, a third into the clip", "clip": "idle", "at": third, "facing": [0, 1],
                  "light": [0, 2.5, 2.0], "distance": close}]
    if directions > 1:
        shots.append({"caption": "engine: every direction at the game camera (distance 13), hero-high bar", "clip": "idle", "turn": True,
                      "light": [0, 2.5, 2.0], "distance": 13, "bar": True})
    else:
        shots.append({"caption": "engine: at the game camera (distance 13), hero-high bar", "clip": "idle", "at": third, "facing": [0, 1],
                      "light": [0, 2.5, 2.0], "distance": 13, "bar": True})
    for index, shot in enumerate(shots):
        shot["tick"] = 40 + index * 48      # far apart: a slow frame must not show the next shot's set-up
    with open(os.path.join(game, "shots.json"), "w", encoding="utf-8") as out:
        # A sprite whose pivot is not on its bottom edge is lifted so that the whole frame stands above the floor.
        lift = 0 if character else (1 - sheet.pivot[1]) * tall
        json.dump({"sprite": "sprites/" + name, "settings": sprite, "height": tall, "lift": lift, "directions": directions, "shots": shots}, out)
    user = os.path.join(work, "user")
    try:
        subprocess.run(common.windowed([engine, game, "--mute", "--user-data", user]), capture_output=True, text=True, timeout=300)
    except (OSError, subprocess.TimeoutExpired) as error:
        print(f"  (no engine shots: {error})")
        shutil.rmtree(work, ignore_errors=True)
        return []
    images = []
    for index, shot in enumerate(shots, 1):
        file = os.path.join(user, "screenshots", f"shot_{index}.png")
        if os.path.isfile(file):
            image = Image.open(file).convert("RGB")
            w, h = image.size
            side = min(w, h)
            crop = image.crop(((w - side) // 2, 0, (w + side) // 2, side))
            images.append((crop.resize((440, 440), Image.LANCZOS), shot["caption"]))
    if not images:
        print("  (the engine wrote no shots)")
    shutil.rmtree(work, ignore_errors=True)
    return images


# ------------------------------------------------------------------ the pages

def pages(sheet, profile, findings, title, lines, others, engine, here):
    hard = profile.get("alpha") == "hard"
    cutoff = tools.OPAQUE_CUTOFF if hard else 24
    background = PAPER if hard else NIGHT
    measured = getattr(sheet, "measured", {})
    tiles = []
    if sheet.atlas is not None and sheet.clips:
        for tile in (lineup(sheet, profile, others, cutoff, background), game_size(sheet, profile, cutoff, background),
                     silhouettes(sheet, cutoff), palette_tile(sheet, profile) if hard else None, lit_normals(sheet, cutoff),
                     emissive_tile(sheet)):
            if tile is not None:
                tiles.append(tile)
        if engine:
            shots = engine_shots(sheet, profile, engine, here)
            if shots:
                strip = Image.new("RGB", (len(shots) * 444 + 4, 460), INK)
                for index, (image, caption) in enumerate(shots):
                    strip.paste(common.label(_tile(image, caption).crop((0, 0, 440, 456)), caption[:72]), (4 + index * 444, 2))
                tiles.append(strip)
    if not tiles:
        tiles = [Image.new("RGB", (400, 120), INK)]
    strips = clip_pages(sheet, profile, measured, title, background) if sheet.atlas is not None and sheet.clips else []
    if len(strips) == 1 and strips[0].height <= 640:
        # A small sheet is one page: its clips first, then the rest.
        return [common.compose([strips[0]] + tiles, title, lines, columns=1, side=540)]
    return [common.compose(tiles, title + "   (page 1: overview; the clips follow)", lines, columns=1, side=540)] + strips
