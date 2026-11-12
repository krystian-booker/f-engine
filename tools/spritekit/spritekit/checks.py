"""The automated checks on a sprite sheet: the sheet itself, pixel hygiene,
motion, timing against the spec, and game readiness. Each returns Findings;
kitcommon decides what they mean for the verdict.

Everything is measured on the files the game ships: the .anim.json as the
engine reads it and the pixels of the atlas inside each frame's rectangle.
A sprite's profile says what kind of thing it is (a character standing on
the ground with hard pixel edges, a soft additive effect, a particle) and
what the game expects of each clip; thresholds come from there.
"""
import os
import re

import numpy as np

from kitcommon.kit import ERROR, INFO, WARN, Finding  # noqa: F401  (the kits share one Finding)

from . import tools

GROUPS = [
    ("sheet", "Sheet integrity"),
    ("pixels", "Pixel hygiene"),
    ("motion", "Motion"),
    ("timing", "Clips and timing"),
    ("ready", "Game readiness"),
]

# What no reason excuses: a file the engine refuses, and the look nobody judged.
UNWAIVABLE = {"sheet.json", "sheet.missing_image"}

FLAT_NORMAL = np.array([128, 128, 255])


class Measured:
    """What the checks need to know of one frame's pixels."""

    def __init__(self, pixels, cutoff, ignore):
        self.alpha = pixels[..., 3]
        self.mask = self.alpha >= cutoff
        self.area = int(self.mask.sum())
        self.bounds = tools.bounds(self.mask)
        self.empty = self.bounds is None
        self.height = 0 if self.empty else self.bounds[1] - self.bounds[0] + 1
        self.lowest = None if self.empty else self.bounds[1]
        self.baseline = tools.baseline(self.mask, ignore)
        self.centre = tools.centre_x(self.mask)


def measure(sheet, profile):
    """A Measured per distinct frame rectangle."""
    cutoff = tools.OPAQUE_CUTOFF if profile.get("alpha") == "hard" else tools.BLEND_CUTOFF
    ignore = profile.get("baseline_ignore_pixels", 0) or 0
    return {rect: Measured(sheet.pixels(frame), cutoff, ignore) for rect, frame in sheet.rects().items()}


def clip_spec(profile, name):
    """What the profile expects of a clip: its defaults, then the conventions
    for a clip of that name, then what the game's manifest says about it."""
    spec = dict(profile.get("clip_defaults") or {})
    spec.update((profile.get("clip_kinds") or {}).get(name) or {})
    spec.update((profile.get("clips") or {}).get(name) or {})
    return spec


def _rows(sheet, clip, measured):
    """Per direction of a clip: the frames that lie inside the atlas with their measurements."""
    for row, frames in enumerate(clip.directions):
        inside = [(frame, measured[frame.rect]) for frame in frames if frame.inside and frame.rect in measured]
        yield row, clip.direction_name(row), inside


def _where(clip, direction=None, frames=None):
    text = clip.name if not direction else f"{clip.name} {direction}"
    if frames:
        shown = ", ".join(str(i) for i in frames[:6]) + (" ..." if len(frames) > 6 else "")
        text += f" frame{'s' if len(frames) > 1 else ''} {shown}"
    return text


def _list(names, limit=5):
    names = list(names)
    return ", ".join(names[:limit]) + (f" and {len(names) - limit} more" if len(names) > limit else "")


# ------------------------------------------------------------------ 1. the sheet

def check_sheet(sheet, profile, measured):
    out = []
    grouped = {}
    for check, severity, message, where, fix in sheet.problems:
        grouped.setdefault((check, severity, message, fix), []).append(where)
    for (check, severity, message, fix), places in grouped.items():
        places = [p for p in places if p]
        out.append(Finding(check, severity, message, len(places) if len(places) > 1 else None, _list(places, 3) or None, fix))
    if sheet.atlas is None or not sheet.loadable:
        return out
    height, width = sheet.atlas.shape[:2]
    for key in ("normal", "emissive"):
        image = getattr(sheet, key)
        if image is not None and image.shape[:2] != (height, width):
            out.append(Finding("sheet.image_size", ERROR,
                               f"the {key} image is {image.shape[1]}x{image.shape[0]} and the atlas {width}x{height}: the engine logs an "
                               f"error and draws the sprite without it", None, getattr(sheet, key + "_name"),
                               "Write it at the atlas's size, laid out exactly like the atlas."))
    outside = [frame for frame in sheet.frames() if not frame.inside]
    if outside:
        out.append(Finding("sheet.frame_outside", ERROR, f"frames whose rectangle leaves the {width}x{height} atlas (the engine smears the edge pixels)",
                           len(outside), _list((f.where for f in outside), 3),
                           "Fix the rectangles, or the atlas was written at another size than the .anim.json assumes."))
    allowed = profile.get("directions")
    for clip in sheet.clips.values():
        counts = [len(row) for row in clip.directions]
        if len(set(counts)) > 1:
            out.append(Finding("sheet.direction_frames", WARN, f"its directions have {'/'.join(map(str, counts))} frames: turning mid-clip jumps the animation",
                               len(counts), clip.name, "Give every direction of a clip the same number of frames."))
        if allowed and len(clip.directions) in (1, 4, 8) and len(clip.directions) not in allowed:
            out.append(Finding("sheet.directions", WARN,
                               f"{len(clip.directions)} direction{'s' if len(clip.directions) != 1 else ''}; the '{profile['name']}' profile expects "
                               f"{' or '.join(map(str, allowed))}", None, clip.name,
                               "Draw the missing directions (tools.mirror_direction makes a west row from an east row of a symmetric figure)."))
        for row, direction, frames in _rows(sheet, clip, measured):
            sizes = {frame.rect[2:] for frame, _ in frames}
            if len(sizes) > 1:
                out.append(Finding("sheet.frame_size", WARN, "its frames differ in size (" + ", ".join(f"{w}x{h}" for w, h in sorted(sizes))
                                   + "): the pivot is a fraction of the frame, so the sprite shifts as the clip plays", len(sizes),
                                   _where(clip, direction), "Put every frame of a clip in a cell of one size (tools.pack does)."))
            empty = [frame.index for frame, m in frames if m.empty]
            if not empty:
                continue
            if len(empty) == len(frames):
                out.append(Finding("sheet.empty_frame", ERROR, "every frame is empty: the clip draws nothing", len(empty), _where(clip, direction),
                                   "The generator wrote no pixels into these cells, or the rectangles point at the wrong part of the atlas."))
            else:
                trailing = 0
                while not clip.loop and trailing < len(empty) and empty[-1 - trailing] == len(frames) - 1 - trailing:
                    trailing += 1
                middle = empty[:len(empty) - trailing]
                if middle:
                    out.append(Finding("sheet.empty_frame", WARN, "frames with nothing in them: the sprite blinks out while the clip plays", len(middle),
                                       _where(clip, direction, middle), "Draw them, or drop them from the clip. Only the last frame of a clip that "
                                                                       "does not loop may be empty (so an effect ends on nothing)."))
                if trailing > 1:
                    out.append(Finding("sheet.empty_frame", WARN, f"ends on {trailing} empty frames: the first already ends the effect, the others "
                                       "are dead time and wasted cells (or frames the generator failed to draw)", trailing - 1,
                                       _where(clip, direction, empty[-trailing:]), "Keep one empty frame at the end, or draw the others."))
                elif trailing == 1:
                    out.append(Finding("sheet.empty_frame", INFO, "ends on an empty frame (the effect vanishes when the clip ends)", 1, _where(clip, direction)))
    # Rectangles that cut across each other show part of another frame.
    rects = sorted(sheet.rects())
    if len(rects) <= 4096:
        r = np.array(rects, dtype=np.int64)
        x0, y0, x1, y1 = r[:, 0], r[:, 1], r[:, 0] + r[:, 2], r[:, 1] + r[:, 3]
        overlap = (x0[:, None] < x1[None, :]) & (x0[None, :] < x1[:, None]) & (y0[:, None] < y1[None, :]) & (y0[None, :] < y1[:, None])
        pairs = int(np.triu(overlap, k=1).sum())
        if pairs:
            first = np.argwhere(np.triu(overlap, k=1))[0]
            out.append(Finding("sheet.frame_overlap", WARN, "pairs of frames whose rectangles overlap without being the same: each shows part of the other",
                               pairs, f"{list(rects[first[0]])} and {list(rects[first[1]])}", "Give every frame its own cell, or reuse a cell exactly."))
    # Pixels no frame uses: a forgotten clip, or leftovers of an older layout.
    used = np.zeros((height, width), dtype=bool)
    for x, y, w, h in rects:
        used[y:y + h, x:x + w] = True
    stray = (sheet.atlas[..., 3] > 0) & ~used
    if stray.any():
        cell = _grid(rects)
        if cell:
            ys, xs = np.nonzero(stray)
            count = len(set(zip((ys // cell[1]).tolist(), (xs // cell[0]).tolist())))
            what = f"cells of the {cell[0]}x{cell[1]} grid that hold pixels but belong to no clip"
        else:
            count, what = int(tools.components(stray)[1].size), "patches of pixels in the atlas that belong to no frame"
        out.append(Finding("sheet.unused_cells", WARN, what + " (a clip that was forgotten, or leftovers)", count, None,
                           "Add the clip that uses them to the .anim.json, or clear them from the atlas."))
    return out


def _grid(rects):
    """(width, height) of the cells when every frame is one size and on a grid of that size."""
    sizes = {(w, h) for _, _, w, h in rects}
    if len(sizes) != 1:
        return None
    w, h = next(iter(sizes))
    return (w, h) if all(x % w == 0 and y % h == 0 for x, y, _, _ in rects) else None


# ------------------------------------------------------------------ 2. pixels

def check_pixels(sheet, profile, measured):
    out = []
    if sheet.atlas is None:
        return out
    hard = profile.get("alpha") == "hard"
    height, width = sheet.atlas.shape[:2]
    by_rect = sheet.rects()

    # Soft pixels where the sprite is drawn as a cutout.
    if hard:
        soft, lost, places = 0, 0, []
        for rect, frame in by_rect.items():
            alpha = measured[rect].alpha
            between = (alpha > 0) & (alpha < 255)
            if between.any():
                soft += int(between.sum())
                lost += int((between & (alpha < tools.OPAQUE_CUTOFF)).sum())
                places.append(frame.where)
        if soft:
            out.append(Finding("pixels.soft_alpha", WARN,
                               f"half-transparent pixels in a sprite drawn as a cutout: the engine cuts at alpha 0.5, so {lost:,} of them vanish "
                               f"and {soft - lost:,} turn solid (anti-aliasing or a resize got into the pixel art)", soft,
                               _list(places, 3), "Keep alpha at 0 or 255 (tools.harden_alpha), and never scale pixel art with a smooth filter."))

    # The figure against the edge of its frame, and a neighbour against it from outside.
    margin = profile.get("frame_margin")
    edge_alpha = tools.OPAQUE_CUTOFF if hard else profile.get("edge_alpha", 24)
    if margin:
        touching, bleeding = [], []
        for rect, frame in by_rect.items():
            x, y, w, h = rect
            alpha = measured[rect].alpha
            if min(w, h) <= 2 * margin:
                continue
            inner = alpha[margin:h - margin, margin:w - margin]
            ring = alpha.astype(np.int32).sum() - inner.astype(np.int32).sum()
            if ring and max(alpha[:margin].max(), alpha[h - margin:].max(), alpha[:, :margin].max(), alpha[:, w - margin:].max()) >= edge_alpha:
                touching.append(frame.where)
            # One pixel outside the rectangle: what an outline or a distortion reads when it looks past the frame's edge.
            outer = sheet.atlas[max(0, y - 1):min(height, y + h + 1), max(0, x - 1):min(width, x + w + 1), 3]
            total = int((outer >= tools.OPAQUE_CUTOFF).sum()) - int((alpha >= tools.OPAQUE_CUTOFF).sum())
            if total > 0:
                bleeding.append(frame.where)
        if touching:
            what = ("the figure is cut off there, and the engine's outline (which is drawn in the transparent pixels around the figure, "
                    "inside the frame) is missing on that side") if hard else "the image stops at a visible straight edge"
            out.append(Finding("pixels.edge", WARN, f"frames whose content reaches within {margin} px of the frame's edge: {what}", len(touching),
                               _list(touching, 4), f"Keep {margin} transparent px inside every frame: use a larger cell or a smaller figure "
                                                   "(tools.trim_and_centre lays frames out with a margin), or fade the image out before the edge."))
        if bleeding:
            out.append(Finding("pixels.bleed", WARN, "frames with another frame's solid pixels right against their edge in the atlas: an outline "
                               "(or a distort effect) reads one texel past the frame and draws them", len(bleeding), _list(bleeding, 4),
                               "Keep a transparent pixel inside the edge of every frame, or pack with a gutter (tools.pack(..., gutter=1))."))

    # Specks and pinholes.
    speck_size, hole_size = profile.get("speck_pixels"), profile.get("pinhole_pixels")
    if speck_size or hole_size:
        specks, holes, speck_places, hole_places = 0, 0, [], []
        for rect, frame in by_rect.items():
            mask = measured[rect].mask
            if speck_size:
                found = int(tools.specks(mask, speck_size).sum())
                if found:
                    specks += found
                    speck_places.append(frame.where)
            if hole_size:
                found = int(tools.pinholes(mask, hole_size).sum())
                if found:
                    holes += found
                    hole_places.append(frame.where)
        if specks:
            out.append(Finding("pixels.specks", WARN, f"stray pixels: islands of {speck_size} px or fewer detached from the figure, in "
                               f"{len(speck_places)} frames", specks, _list(speck_places, 4),
                               "Erase them (tools.remove_specks), or join them to the figure if they are meant (a spark is a clip of its own)."))
        if holes:
            out.append(Finding("pixels.pinholes", WARN, f"pinholes: transparent holes of {hole_size} px inside the figure, in {len(hole_places)} "
                               "frames; the background sparkles through as the clip plays", holes, _list(hole_places, 4),
                               "Fill them (tools.fill_pinholes); a real gap between an arm and the body is at least two pixels wide."))

    # The palette.
    cutoff = tools.OPAQUE_CUTOFF if hard else tools.BLEND_CUTOFF
    palette, counts = tools.colours([sheet.pixels(frame) for frame in by_rect.values()], cutoff)
    sheet.palette = (palette, counts)
    limit = profile.get("max_colors")
    if limit and len(palette) > limit:
        out.append(Finding("pixels.colors", WARN, f"{len(palette):,} colours; the '{profile['name']}' profile allows {limit} (a limited palette is "
                           "what makes a cast look like one set)", len(palette) - limit, None,
                           "Draw from the game's palette (tools.quantize(frames, palette) maps everything onto it)."))
    distance = profile.get("near_duplicate_distance")
    if distance and 1 < len(palette) <= 1024:
        pairs = tools.near_duplicates(palette, distance)
        redundant = sorted({j for _, j in pairs})
        if len(redundant) > (profile.get("max_near_duplicates") or 0):
            shown = ", ".join("#%02x%02x%02x~#%02x%02x%02x" % (*palette[i], *palette[j]) for i, j in pairs[:3])
            out.append(Finding("pixels.near_duplicates", WARN, "colours that cannot be told from another colour of the sheet (the same tone "
                               f"mixed twice): {shown}", len(redundant), None,
                               "Use one of each pair (tools.merge_near_duplicates), or take colours from one palette table instead of computing them."))
    rare = profile.get("stray_color_pixels")
    if rare and len(palette) > 4 and len(by_rect) >= profile.get("stray_color_frames", 8):
        strays = [i for i, count in enumerate(counts.tolist()) if count < rare]
        if strays:
            shown = ", ".join("#%02x%02x%02x (%d px)" % (*palette[i], counts[i]) for i in strays[:4])
            out.append(Finding("pixels.stray_colors", WARN, f"colours used by fewer than {rare} pixels in the whole sheet: {shown}", len(strays), None,
                               "A colour that appears once is an accident (a blend, a leftover): repaint those pixels in a colour of the palette."))
    # Colour under transparent pixels: the engine never samples it (no filtering, no mips, cut before blending).
    hidden = int(((sheet.atlas[..., 3] == 0) & (sheet.atlas[..., :3].max(axis=2) > 0)).sum())
    if hidden:
        out.append(Finding("pixels.hidden_color", INFO, "fully transparent pixels that still carry a colour (harmless in the engine, which samples "
                           "sprites unfiltered; tools.clear_hidden_colour makes the PNG smaller)", hidden))
    return out


# ------------------------------------------------------------------ 3. motion

def check_motion(sheet, profile, measured):
    out = []
    if sheet.atlas is None:
        return out
    grounded = bool(profile.get("grounded"))
    jitter_limit = profile.get("baseline_jitter", 2)
    pop_limit = profile.get("baseline_pop", 2)
    drift_limit = profile.get("centre_drift", 3)
    scale_limit = profile.get("scale_ratio", 1.35)
    pop_pixels = profile.get("pop_pixels", 3)
    seam_limit = profile.get("loop_seam_ratio", 1.5)
    idle = sheet.clips.get("idle")

    def reference(row):
        """The idle clip's baseline and size in a direction: what the other clips are held to."""
        if idle is None or not idle.directions:
            return None
        frames = [m for frame in idle.directions[min(row, len(idle.directions) - 1)]
                  if frame.inside and not (m := measured[frame.rect]).empty]
        if not frames:
            return None
        return float(np.median([m.baseline for m in frames])), float(np.median([m.area for m in frames]))

    for clip in sheet.clips.values():
        spec = clip_spec(profile, clip.name)
        stance = spec.get("stance") if grounded else None
        for row, direction, frames in _rows(sheet, clip, measured):
            solid = [(frame, m) for frame, m in frames if not m.empty]
            if len(solid) < 1:
                continue
            pixels = [sheet.pixels(frame) for frame, _ in frames]
            same = [np.array_equal(pixels[i], pixels[i + 1]) for i in range(len(pixels) - 1)]
            if len(frames) > 1 and all(same):
                out.append(Finding("motion.static", WARN, f"a clip of {len(frames)} frames in which nothing moves", len(frames), _where(clip, direction),
                                   "Make it one frame with the whole duration, or animate it."))
            elif any(same):
                held = [frames[i + 1][0].index for i, equal in enumerate(same) if equal]
                out.append(Finding("motion.hold", INFO, "frames identical to the one before (a hold; one frame with a longer duration saves the cell "
                                   "unless they reuse it)", len(held), _where(clip, direction, held)))

            if stance in ("upright", "low"):
                base = [m.baseline for _, m in solid]
                limit = spec.get("baseline_jitter", jitter_limit)
                if limit is not None and max(base) - min(base) > limit:
                    worst = [frame.index for frame, m in solid if m.baseline in (max(base), min(base))]
                    out.append(Finding("motion.baseline_jitter", WARN,
                                       f"the feet move up and down by {max(base) - min(base)} px while the clip plays (rows {min(base)} to {max(base)}; "
                                       f"{limit} px is allowed): the figure hops on the spot", max(base) - min(base), _where(clip, direction, worst),
                                       "Keep the planted foot on one row in every frame (tools.align_baseline), and let the body bob, not the feet."))
            ref = reference(row) if stance in ("upright", "low") and clip is not idle else None
            if ref:
                base = float(np.median([m.baseline for _, m in solid]))
                if pop_limit is not None and abs(base - ref[0]) > pop_limit:
                    out.append(Finding("motion.baseline_pop", WARN,
                                       f"touches the ground {abs(base - ref[0]):.0f} px {'lower' if base > ref[0] else 'higher'} than 'idle' (row {base:.0f} "
                                       f"against {ref[0]:.0f}; {pop_limit} px is allowed): the figure jumps when the clip changes", int(abs(base - ref[0])),
                                       _where(clip, direction), "Draw every clip on the idle clip's ground row (tools.align_baseline(frames, row))."))
                area = float(np.median([m.area for _, m in solid]))
                ratio = area / ref[1] if ref[1] else 1.0
                if stance == "upright" and scale_limit and (ratio > scale_limit or ratio < 1 / scale_limit):
                    out.append(Finding("motion.scale_pop", WARN,
                                       f"the figure covers {ratio:.2f} times the pixels it does in 'idle': drawn at another scale, so it swells or "
                                       "shrinks when the clip changes", None, _where(clip, direction),
                                       "Draw every clip at one scale; a pose changes the outline, not the amount of figure."))
            if stance == "upright" and spec.get("in_place") and len(solid) > 1 and drift_limit is not None:
                centres = [m.centre for _, m in solid]
                if max(centres) - min(centres) > drift_limit:
                    out.append(Finding("motion.drift", WARN,
                                       f"the body slides {max(centres) - min(centres):.1f} px sideways in a clip that plays on the spot ({drift_limit} px is "
                                       "allowed): the engine moves the sprite, the clip must not", int(round(max(centres) - min(centres))),
                                       _where(clip, direction), "Centre every frame on the pivot column (tools.trim_and_centre keeps a clip's frames together)."))
            # One frame out of place: away from both its neighbours, which agree with each other.
            if stance in ("upright", "low") and len(solid) >= 4:
                order = solid + solid[:2] if clip.loop else solid
                popped = []
                for (_, before), (frame, here), (_, after) in zip(order, order[1:], order[2:]):
                    for a, b, c in ((before.baseline, here.baseline, after.baseline), (before.centre, here.centre, after.centre)):
                        if abs(a - c) <= 1 and abs(b - a) >= pop_pixels and abs(b - c) >= pop_pixels and (b - a) * (b - c) > 0:
                            popped.append(frame.index)
                            break
                if popped:
                    out.append(Finding("motion.pop", WARN, f"frames that sit {pop_pixels} px or more away from both their neighbours, which agree with each "
                                       "other: a misplaced frame", len(popped), _where(clip, direction, sorted(set(popped))),
                                       "Check the frame's cell and its offset in the cell; onion-skin it against its neighbours."))
            if clip.loop and seam_limit and len(frames) >= 3 and not all(same):
                seam, _, ratio = tools.loop_seam(pixels)
                if ratio > seam_limit and seam > profile.get("loop_seam_floor", 0.04):
                    out.append(Finding("motion.loop_seam", WARN,
                                       f"the loop does not close: going from the last frame back to the first is a change {ratio:.1f} times as large as "
                                       "the largest between two frames inside the clip, so it jumps every time round", None, _where(clip, direction),
                                       "Make the last frame lead into the first (the frame after the last IS the first), or do not loop the clip."))
        # West against east: the same figure seen from two sides.
        if grounded and clip_spec(profile, clip.name).get("stance") == "upright" and len(clip.directions) in (4, 8):
            west, east = (1, 3) if len(clip.directions) == 4 else (2, 6)
            pairs = [(measured[a.rect], measured[b.rect]) for a, b in zip(clip.directions[west], clip.directions[east])
                     if a.inside and b.inside and not measured[a.rect].empty and not measured[b.rect].empty]
            if pairs:
                base = float(np.median([abs(a.baseline - b.baseline) for a, b in pairs]))
                area = float(np.median([a.area / b.area for a, b in pairs]))
                if (pop_limit is not None and base > pop_limit) or (scale_limit and (area > scale_limit or area < 1 / scale_limit)):
                    what = f"stand {base:.0f} px apart" if pop_limit is not None and base > pop_limit else f"differ {max(area, 1 / area):.2f} times in size"
                    out.append(Finding("motion.mirror", WARN, f"the west and east rows {what}: the figure changes when it turns round", None, clip.name,
                                       "Draw both sides on one ground row at one scale (tools.mirror_direction makes one from the other when the "
                                       "figure is symmetric)."))
    return out


# ------------------------------------------------------------------ 4. timing

def check_timing(sheet, profile, measured):
    out = []
    specs = profile.get("clips") or {}
    required = list(profile.get("required_clips") or []) + [name for name, spec in specs.items() if not (spec or {}).get("optional")]
    missing = [name for name in dict.fromkeys(required) if name not in sheet.clips]
    if sheet.clips and "idle" not in sheet.clips:
        first = sorted(sheet.clips)[0]
        out.append(Finding("timing.no_idle", WARN, f"no 'idle' clip: a clip name the sheet lacks, and a particle, then plays '{first}' (the first by "
                           "name)", None, None, "Name the resting clip 'idle'."))
        missing = [name for name in missing if name != "idle"]
    if missing:
        out.append(Finding("timing.missing_clip", ERROR, "clips the game plays that the sheet lacks (the engine shows 'idle' instead, with idle's "
                           f"events and length): {_list(missing, 8)}", len(missing), None, "Add them, or fix the names: they are matched exactly."))
    known_events = profile.get("event_names")
    shortest, longest = profile.get("min_frame_seconds"), profile.get("max_frame_seconds")
    tolerance = profile.get("event_tolerance", 0.035)
    for clip in sheet.clips.values():
        if not clip.directions or not all(clip.directions):
            continue
        spec = clip_spec(profile, clip.name)
        if spec.get("loop") is not None and bool(spec["loop"]) != clip.loop:
            consequence = ("it never ends, so 'anim_finished' never comes and whatever waits for it waits for ever" if clip.loop
                           else "it plays once and freezes on its last frame")
            given = "" if clip.loop_given else " (no 'loop' in the file means true)"
            out.append(Finding("timing.loop", ERROR, f"'loop' is {str(clip.loop).lower()}{given} and the game expects {str(bool(spec['loop'])).lower()}: "
                               f"{consequence}", None, clip.name, f"Set \"loop\": {str(bool(spec['loop'])).lower()} on the clip."))
        lengths = [clip.length(row) for row in range(len(clip.directions))]
        if max(lengths) - min(lengths) > 1e-6:
            out.append(Finding("timing.direction_length", WARN, "its directions last " + "/".join(f"{v:.2f}" for v in lengths) + " s: the clip ends at "
                               "a different moment depending on which way the sprite faces", None, clip.name,
                               "Give every direction the same durations."))
        wanted = spec.get("seconds")
        if isinstance(wanted, (int, float)):
            wanted = [wanted - 0.011, wanted + 0.011]
        if wanted and not (wanted[0] - 1e-6 <= lengths[0] <= wanted[1] + 1e-6):
            out.append(Finding("timing.length", WARN, f"lasts {lengths[0]:.2f} s; a '{clip.name}' clip of the '{profile['name']}' profile takes "
                               f"{wanted[0]:.2f} to {wanted[1]:.2f} s", None, clip.name, "Change the frame durations (or the number of frames)."))
        count = len(clip.directions[0])
        wanted = spec.get("frames")
        if wanted is not None:
            low, high = (wanted, wanted) if isinstance(wanted, int) else wanted
            if not low <= count <= high:
                expected = str(low) if low == high else f"{low} to {high}"
                out.append(Finding("timing.frames", WARN, f"{count} frame{'s' if count != 1 else ''}; the spec asks for {expected}", None, clip.name,
                                   "Draw the frames the spec lists, or change the spec in the manifest if the design changed."))
        short, long_ = [], []
        for row, frames in enumerate(clip.directions):
            for frame in frames:
                last_hold = not clip.loop and frame.index == len(frames) - 1
                if shortest and frame.duration < shortest - 1e-9:
                    short.append(frame)
                if longest and frame.duration > longest + 1e-9 and not last_hold:
                    long_.append(frame)
        if short:
            out.append(Finding("timing.frame_duration", WARN, f"frames shorter than {shortest * 1000:.0f} ms (the shortest is {min(f.duration for f in short) * 1000:.0f} "
                               "ms): the game ticks 60 times a second, so such a frame is skipped on some passes", len(short), _list((f.where for f in short), 3),
                               "Make no frame shorter than one tick (1/60 s); two ticks if it must be seen."))
        if long_:
            out.append(Finding("timing.frame_duration", WARN, f"frames held longer than {longest:g} s (the longest is {max(f.duration for f in long_):g} s) "
                               "in the middle of a clip", len(long_), _list((f.where for f in long_), 3),
                               "A long hold belongs on the last frame of a clip that does not loop; otherwise it is a stall."))
        # Events.
        times = []   # per direction: [(name, seconds)]
        for row, frames in enumerate(clip.directions):
            starts = clip.starts(row)
            times.append([(frame.event, starts[frame.index]) for frame in frames if frame.event])
        expected = spec.get("events")
        if expected is not None:
            names = list(expected) if not isinstance(expected, dict) else list(expected)
            want = {}
            for name in names:
                at = expected[name] if isinstance(expected, dict) else None
                want[name] = want.get(name, 0) + (len(at) if isinstance(at, list) else 1)
            for row, events in enumerate(times):
                have = {}
                for name, _ in events:
                    have[name] = have.get(name, 0) + 1
                lacking = [f"'{name}'" + (f" x{count}" if count > 1 else "") for name, count in want.items() if have.get(name, 0) < count]
                if lacking:
                    out.append(Finding("timing.events_missing", ERROR, f"lacks the event {_list(lacking)} the game listens for (it has: "
                                       f"{_list(sorted(have)) or 'none'})", len(lacking), _where(clip, clip.direction_name(row)),
                                       "Put \"event\" on the frame where it happens, in every direction."))
                if isinstance(expected, dict):
                    for name, at in expected.items():
                        moments = at if isinstance(at, list) else [at]
                        found = sorted(time for event, time in events if event == name)
                        if len(found) != len(moments):
                            continue      # an event too few or too many is reported above; pairing the rest would blame the wrong one
                        for wanted_time, real in zip(sorted(m for m in moments if m is not None), found):
                            if abs(real - wanted_time) > tolerance:
                                out.append(Finding("timing.event_time", WARN, f"'{name}' fires at {real:.2f} s; the game expects it at {wanted_time:.2f} s "
                                                   "(its own timers and effects are set to that moment)", None, _where(clip, clip.direction_name(row)),
                                                   "Move the event to the frame that starts then, or change the durations before it."))
        if known_events is not None:
            unknown = sorted({name for events in times for name, _ in events if name not in known_events})
            if unknown:
                out.append(Finding("timing.event_unknown", WARN, f"events the game does not know: {_list(unknown)} (it listens for {_list(known_events, 8)})",
                                   len(unknown), clip.name, "Fix the spelling: an event name is matched exactly."))
        if len(times) > 1 and any(sorted(events) != sorted(times[0]) for events in times[1:]) and max(lengths) - min(lengths) <= 1e-6:
            if any(len(events) == len(times[0]) for events in times[1:]) or not expected:
                out.append(Finding("timing.event_directions", WARN, "its events fire at different moments (or not at all) depending on the direction",
                                   None, clip.name, "Put each event on the same frame in every direction."))
    return out


# ------------------------------------------------------------------ 5. readiness

def check_ready(sheet, profile, measured):
    out = []
    # Names.
    pattern = profile.get("naming")
    if pattern:
        names = [os.path.basename(sheet.path)[:-len(".anim.json")] if sheet.path.endswith(".anim.json") else os.path.basename(sheet.path)]
        names += [os.path.splitext(name)[0] for name in (sheet.atlas_name, sheet.normal_name, sheet.emissive_name) if name]
        bad = [name for name in names if not re.fullmatch(pattern, os.path.basename(name))]
        if bad:
            out.append(Finding("ready.naming", WARN, f"file names outside the convention ({pattern}): {_list(bad)}", len(bad), None,
                               "Lower case, digits and underscores: paths are matched exactly, and a packed game is case sensitive everywhere."))
        bad = [name for name in sheet.clips if not re.fullmatch(pattern, name)]
        if bad:
            out.append(Finding("ready.naming", WARN, f"clip names outside the convention ({pattern}): {_list(bad)}", len(bad), None,
                               "Scripts play clips by name: lower case, digits and underscores."))
    wanted = profile.get("pixels_per_unit")
    if wanted and sheet.pixels_per_unit != wanted:
        given = "" if sheet.ppu_given else " (the file gives none; the engine then assumes 32, not game.json's value)"
        out.append(Finding("ready.pixels_per_unit", WARN, f"{sheet.pixels_per_unit} pixels per unit{given}; the '{profile['name']}' profile expects {wanted}: "
                           "its pixels are another size on screen than the rest of the cast's", None, None,
                           f"Draw it at {wanted} px per unit and say so in the file."))
    elif not sheet.ppu_given and sheet.loadable:
        out.append(Finding("ready.pixels_per_unit", INFO, "no 'pixels_per_unit' in the file: the engine assumes 32 (game.json's value is not used for a sheet)"))
    pivot = profile.get("pivot")
    tolerance = profile.get("pivot_tolerance", 0.02)
    if pivot and (abs(sheet.pivot[0] - pivot[0]) > tolerance or abs(sheet.pivot[1] - pivot[1]) > tolerance):
        given = "" if sheet.pivot_given else " (the file gives none: the engine's default)"
        out.append(Finding("ready.pivot", WARN, f"the pivot is [{sheet.pivot[0]:g}, {sheet.pivot[1]:g}]{given}; the '{profile['name']}' profile puts it at "
                           f"[{pivot[0]:g}, {pivot[1]:g}] (x from the left, y from the top of the frame: where the entity's position is)", None, None,
                           "Set \"pivot\" in the .anim.json."))
    if sheet.atlas is None:
        return out
    height, width = sheet.atlas.shape[:2]
    limit = profile.get("max_atlas_size")
    if limit and max(width, height) > limit:
        out.append(Finding("ready.atlas_size", ERROR, f"the atlas is {width}x{height}; the '{profile['name']}' budget is {limit} on a side", None, None,
                           "Fewer or smaller frames, or split the sheet: the atlas, its normal map and its emissive image are each held uncompressed."))
    if profile.get("power_of_two") and (width & (width - 1) or height & (height - 1)):
        out.append(Finding("ready.atlas_pot", WARN, f"the atlas is {width}x{height}: not a power of two, which the profile asks for", None, None,
                           "tools.pack(..., power_of_two=True)."))
    rects = list(sheet.rects())
    used = np.zeros((height, width), dtype=bool)
    for x, y, w, h in rects:
        used[y:y + h, x:x + w] = True
    unused = 100.0 * (1 - used.mean())
    sheet.unused_percent = unused
    limit = profile.get("max_unused_percent")
    if limit is not None and unused > limit and width * height >= profile.get("unused_min_pixels", 128 * 128):
        out.append(Finding("ready.atlas_unused", WARN, f"{unused:.0f}% of the atlas belongs to no frame ({limit}% is allowed): memory that shows nothing",
                           None, None, "Pack the frames closer (tools.pack), or crop the atlas."))

    # The figure's size and where it stands, from the idle clip.
    idle = sheet.clips.get("idle") or (sheet.clips[sorted(sheet.clips)[0]] if sheet.clips else None)
    frames = [measured[f.rect] for f in idle.directions[0] if f.inside and not measured[f.rect].empty] if idle and idle.directions else []
    if frames:
        tall = float(np.median([m.height for m in frames]))
        sheet.figure = (tall, tall / sheet.pixels_per_unit)
        world = profile.get("world_height")
        if world and not (world[0] <= tall / sheet.pixels_per_unit <= world[1]):
            out.append(Finding("ready.world_height", WARN,
                               f"the figure is {tall:.0f} px tall in 'idle': {tall / sheet.pixels_per_unit:.2f} units at {sheet.pixels_per_unit} px per unit; "
                               f"the '{profile['name']}' profile expects {world[0]:g} to {world[1]:g} (a hero is about 1.5)", None, None,
                               "Draw it at the size the game's scale gives it; do not fix size with the sprite's 'scale'."))
        gap = profile.get("pivot_gap")
        if gap and profile.get("grounded"):
            frame_height = idle.directions[0][0].rect[3]
            feet = float(np.median([m.lowest for m in frames])) + 1          # the row just under the soles
            lift = sheet.pivot[1] * frame_height - feet
            if not (gap[0] <= lift <= gap[1]):
                where = (f"float {lift:.0f} px ({lift / sheet.pixels_per_unit:.2f} units) above" if lift > 0 else f"sink {-lift:.0f} px below")
                out.append(Finding("ready.pivot_feet", WARN, f"the feet {where} the pivot row (the entity's position on the ground): row {feet:.0f} "
                                   f"against {sheet.pivot[1] * frame_height:.0f} of {frame_height}; {gap[0]} to {gap[1]} px is allowed", int(abs(lift)), "idle",
                                   "Move the figure in its frame, or set the pivot's y to (row under the soles + 1) / frame height."))

    out += _check_normal(sheet, profile, measured)
    out += _check_emissive(sheet, profile, measured)
    return out


def _check_normal(sheet, profile, measured):
    out = []
    need = profile.get("normal", "optional")
    if not sheet.normal_name:
        if need == "required":
            out.append(Finding("ready.normal_missing", WARN, f"no normal map; the '{profile['name']}' profile asks for one (without it the sprite is lit "
                               "as one flat card and point lights do not move across it)", None, None,
                               "Write one laid out like the atlas (tools.normal_from_height) and name it with \"normal\"."))
        return out
    if sheet.normal is None or sheet.atlas is None or sheet.normal.shape != sheet.atlas.shape:
        return out
    visible = np.zeros(sheet.atlas.shape[:2], dtype=bool)
    for rect in sheet.rects():
        x, y, w, h = rect
        visible[y:y + h, x:x + w] = measured[rect].mask
    total = int(visible.sum())
    if not total:
        return out
    vectors = sheet.normal[..., :3].astype(np.float64) / 127.5 - 1.0
    length = np.linalg.norm(vectors, axis=2)
    bad = visible & ((length < 0.85) | (length > 1.15))
    if bad.sum() > max(0, profile.get("normal_bad_pixels", 0)):
        out.append(Finding("ready.normal_length", WARN, "pixels of the figure whose normal is not unit length (the shader normalises, so a short one "
                           "is noise and a zero one is black)", int(bad.sum()), sheet.normal_name,
                           "Normalise before encoding: rgb = round(n * 127.5 + 127.5) with |n| = 1 (tools.normal_from_height does)."))
    away = visible & (vectors[..., 2] <= 0.0) & ~bad
    if away.sum() > max(0, profile.get("normal_bad_pixels", 0)):
        out.append(Finding("ready.normal_facing", WARN, "pixels of the figure whose normal points away from the viewer (blue under 128): they are lit "
                           "from behind", int(away.sum()), sheet.normal_name, "Blue is the normal's z towards the viewer: 128 to 255."))
    flat = np.abs(sheet.normal[..., :3].astype(np.int32) - FLAT_NORMAL).max(axis=2) <= 2
    outside = ~visible & ~flat
    if outside.sum() > profile.get("normal_outside_percent", 2.0) / 100 * total:
        out.append(Finding("ready.normal_coverage", WARN, f"the normal map has relief on {int(outside.sum()):,} pixels where the atlas is transparent "
                           f"({100 * outside.sum() / total:.0f}% of the figure's area): it is laid out differently from the atlas, or was baked from "
                           "another version of the frames", int(outside.sum()), sheet.normal_name,
                           "Bake the normal map from the very frames that are in the atlas, in the same cells, flat (128, 128, 255) outside the figure."))
        return out
    if (visible & ~flat).sum() < 0.02 * total:
        out.append(Finding("ready.normal_flat", WARN, "the normal map is flat (128, 128, 255) under the whole figure: it costs a texture and changes "
                           "nothing", None, sheet.normal_name, "Derive it from the figure's form (tools.normal_from_height), or drop it."))
        return out
    # The convention: on a rounded figure, normals lean left on its left side and up on its upper side.
    lean_x, lean_y, weight = 0.0, 0.0, 0
    for rect in sheet.rects():
        x, y, w, h = rect
        m = measured[rect]
        if m.empty or m.area < 24:
            continue
        ys, xs = np.nonzero(m.mask & ~flat[y:y + h, x:x + w])
        if len(xs) < 12:
            continue
        nx, ny = vectors[y + ys, x + xs, 0], vectors[y + ys, x + xs, 1]
        dx, dy = xs - xs.mean(), -(ys - ys.mean())           # right and up from the figure's middle
        if dx.std() > 0 and nx.std() > 0:
            lean_x += float(np.corrcoef(dx, nx)[0, 1]) * len(xs)
        if dy.std() > 0 and ny.std() > 0:
            lean_y += float(np.corrcoef(dy, ny)[0, 1]) * len(xs)
        weight += len(xs)
    if weight:
        sheet.normal_lean = (lean_x / weight, lean_y / weight)
        limit = profile.get("normal_convention_limit", -0.15)
        flipped = [name for name, value in (("red (x: it must point right)", lean_x / weight), ("green (y: it must point up)", lean_y / weight))
                   if value < limit]
        if flipped:
            out.append(Finding("ready.normal_convention", WARN, f"the normal map looks flipped in {' and '.join(flipped)}: the figure's left side "
                               "leans right or its top leans down, so it is lit from the wrong side", len(flipped), sheet.normal_name,
                               "The engine reads red = right, green = up, blue = towards the viewer (the OpenGL convention); invert the channel "
                               "(255 - value) if the map came from a tool that writes green down."))
    return out


def _check_emissive(sheet, profile, measured):
    out = []
    if not sheet.emissive_name or sheet.emissive is None or sheet.atlas is None or sheet.emissive.shape != sheet.atlas.shape:
        return out
    glow = sheet.emissive[..., :3].astype(np.float64).max(axis=2) * sheet.emissive[..., 3] / 255.0   # what the shader adds: colour times alpha
    lit = glow >= profile.get("emissive_floor", 8)
    visible = np.zeros(sheet.atlas.shape[:2], dtype=bool)
    for rect in sheet.rects():
        x, y, w, h = rect
        visible[y:y + h, x:x + w] = measured[rect].mask
    if not (lit & visible).any():
        out.append(Finding("ready.emissive_dark", WARN, "the emissive image adds no light anywhere on the figure (black, or fully transparent: the "
                           "shader adds colour times alpha)", None, sheet.emissive_name, "Paint what glows in colour with alpha 255, or drop the image."))
        return out
    outside = lit & ~visible
    if outside.sum() > profile.get("emissive_outside_pixels", 0):
        out.append(Finding("ready.emissive_outside", WARN, "glowing pixels of the emissive image that lie outside the figure: never drawn on an opaque "
                           "sprite, a ghost on a blended one", int(outside.sum()), sheet.emissive_name,
                           "Mask the emissive image with the atlas's own alpha; if it glows beyond the figure the engine's bloom does that."))
    return out


CHECKS = {"sheet": check_sheet, "pixels": check_pixels, "motion": check_motion, "timing": check_timing, "ready": check_ready}


def run(sheet, profile):
    """Every finding of the five measured groups, and the per-frame measurements."""
    measured = measure(sheet, profile) if sheet.atlas is not None else {}
    findings = []
    for group, _ in GROUPS:
        # A file the engine refuses is fixed first: what is left of it would only be measured for consequences.
        if sheet.loadable or group == "sheet":
            findings.extend(CHECKS[group](sheet, profile, measured))
    return findings, measured


def stats(sheet):
    """One line about the sheet for the report."""
    if sheet.atlas is None:
        return f"{len(sheet.clips)} clips; no atlas"
    height, width = sheet.atlas.shape[:2]
    rects = list(sheet.rects())
    sizes = sorted({(w, h) for _, _, w, h in rects})
    size = f"{sizes[0][0]}x{sizes[0][1]}" if len(sizes) == 1 else f"{len(sizes)} sizes"
    rows = sorted({len(clip.directions) for clip in sheet.clips.values()})
    palette = getattr(sheet, "palette", None)
    parts = [f"{width}x{height} atlas", f"{len(sheet.clips)} clip{'s' if len(sheet.clips) != 1 else ''}", f"{len(rects)} frames of {size}",
             f"{'/'.join(map(str, rows))} direction{'s' if rows != [1] else ''}", f"{sheet.pixels_per_unit} px per unit"]
    if palette is not None:
        parts.append(f"{len(palette[0]):,} colours")
    figure = getattr(sheet, "figure", None)
    if figure:
        parts.append(f"{figure[0]:.0f} px tall = {figure[1]:.2f} units")
    if sheet.normal is not None:
        parts.append("normal map")
    if sheet.emissive is not None:
        parts.append("emissive")
    return ", ".join(parts)
