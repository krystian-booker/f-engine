#!/usr/bin/env python3
"""Proves the validator: every check is shown a small generated sprite sheet
with exactly the flaw it exists for and must name it (and nothing else), the
clean sheets must come back clean, and each helper must fix what it claims.

    python3 tools/spritekit/selftest.py                                  # numpy and pillow
    python3 tools/spritekit/selftest.py --engine build/fengine_headless  # also the engine's own loader
"""
import contextlib
import copy
import io
import json
import os
import shutil
import sys
import tempfile

import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
sys.dont_write_bytecode = True
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.dirname(HERE))

from kitcommon import kit as common  # noqa: E402
from spritekit import sheet as sheets, tools  # noqa: E402
from spritekit.kit import SpriteKit  # noqa: E402

WORK = tempfile.mkdtemp(prefix="spritekit-selftest-")
FAILURES = []
KIT = SpriteKit()
ENGINE = sys.argv[sys.argv.index("--engine") + 1] if "--engine" in sys.argv else None

WIDTH, HEIGHT, GROUND, TOP = 40, 52, 50, 4   # the cell, the row the soles stand on, and the rows left free above the head
OUTLINE, SKIN, HAIR, TUNIC, SHADE, BOOT = (26, 20, 36), (236, 190, 150), (150, 90, 40), (40, 90, 170), (28, 62, 124), (96, 60, 40)
SPEC = {"world_height": [1.0, 1.6], "normal": "required", "event_names": ["step", "hit"], "directions": [4], "clips": {
    "idle": {"stance": "upright", "loop": True, "in_place": True, "frames": 4, "seconds": 1.2},
    "walk": {"stance": "upright", "loop": True, "in_place": True, "frames": 6, "seconds": 0.6, "events": {"step": [0.0, 0.3]}},
    "attack": {"stance": "upright", "loop": False, "frames": 4, "seconds": 0.5, "events": {"hit": 0.2}}}}


# ------------------------------------------------------------------ a small generated character

def figure(direction=0, bob=0, step=0, lean=0, blade=0):
    """One frame of a chibi figure. direction: 0 south, 1 west, 2 north, 3
    east. bob lowers the body and head; step lifts the left (1) or right (2)
    foot; lean shifts the body sideways; blade draws a sword of that length."""
    image = np.zeros((HEIGHT, WIDTH, 4), dtype=np.uint8)
    side = direction in (1, 3)
    half = 6 if side else 8
    centre = WIDTH // 2 + lean

    def box(x0, y0, x1, y1, colour):
        image[y0:y1 + 1, x0:x1 + 1, :3] = colour
        image[y0:y1 + 1, x0:x1 + 1, 3] = 255

    for index, x in enumerate((centre - 5, centre + 2)):                     # the legs: a planted foot stays on the ground row
        lifted = 2 if step == index + 1 else 0
        box(x, 33 + TOP, x + 3, GROUND - lifted, BOOT)
    box(centre - half, 19 + TOP + bob, centre + half - 1, 34 + TOP, TUNIC)
    box(centre + half - 4, 19 + TOP + bob, centre + half - 1, 34 + TOP, SHADE)
    ys, xs = np.mgrid[0:HEIGHT, 0:WIDTH]
    head = (xs - centre + 0.5) ** 2 + (ys - (11 + TOP + bob)) ** 2 <= 8.2 ** 2
    image[head, :3], image[head, 3] = SKIN, 255
    hair = head & ((ys <= 9 + TOP + bob) if direction != 2 else True)
    image[hair, :3] = HAIR
    if blade:
        box(centre + half, 24 + TOP + bob, centre + half + blade, 25 + TOP + bob, (210, 216, 230))
    solid = image[..., 3] > 0
    inner = solid.copy()
    for dy, dx in ((1, 0), (-1, 0), (0, 1), (0, -1)):
        inner &= np.roll(solid, (dy, dx), axis=(0, 1))
    image[solid & ~inner, :3] = OUTLINE
    if direction in (0, 1, 3):                                               # eyes
        for x in ((centre - 3, centre + 2) if direction == 0 else (centre - 4,)):
            image[12 + TOP + bob:14 + TOP + bob, x, :3] = OUTLINE
    if direction == 3:
        image = image[:, ::-1].copy()
    return image


def relief(frame):
    """A height field for a frame: how deep inside the figure each pixel lies (a dome over the silhouette)."""
    solid = frame[..., 3] > 0
    depth = np.zeros(solid.shape)
    inside = solid.copy()
    for _ in range(7):
        depth += inside
        shrunk = inside.copy()
        for dy, dx in ((1, 0), (-1, 0), (0, 1), (0, -1)):
            shrunk &= np.roll(inside, (dy, dx), axis=(0, 1))
        inside = shrunk
    return np.sqrt(depth)


def character():
    """The clean sheet as data: {clip: {"frames": [per direction [frames]], "durations", "loop", "events"}}."""
    bobs = (0, 1, 0, 1)
    walk = ((0, 1), (1, 1), (0, 0), (0, 2), (1, 2), (0, 0))                    # (bob, lifted foot): contact, pass, contact
    attack = ((0, -1, 0), (1, -2, 0), (0, 2, 6), (0, 1, 3))                  # (bob, lean, blade)
    return {
        "idle": {"frames": [[figure(d, bob=b) for b in bobs] for d in range(4)], "durations": 0.3, "loop": True, "events": {}},
        "walk": {"frames": [[figure(d, bob=b, step=s) for b, s in walk] for d in range(4)], "durations": 0.1, "loop": True,
                 "events": {0: "step", 3: "step"}},
        "attack": {"frames": [[figure(d, bob=b, lean=l, blade=k) for b, l, k in attack] for d in range(4)],
                   "durations": [0.1, 0.1, 0.1, 0.2], "loop": False, "events": {2: "hit"}}}


def write(name, clips, ppu=32, pivot=(0.5, 1.0), normal=True, emissive=None, gutter=0, edit_json=None, edit_atlas=None, edit_normal=None,
          cell=None):
    """Packs the clips into <name>.png (+ _n.png), writes <name>.anim.json and returns its path."""
    frames, slots = [], {}
    for clip, body in clips.items():
        for d, row in enumerate(body["frames"]):
            for i, frame in enumerate(row):
                slots[(clip, d, i)] = len(frames)
                frames.append(frame)
    atlas, rects = tools.pack(frames, cell=cell, gutter=gutter, columns=min(8, len(frames)))
    document = {clip: tools.clip([[rects[slots[(clip, d, i)]] for i in range(len(row))] for d, row in enumerate(body["frames"])],
                                 body["durations"], body["loop"], body["events"]) for clip, body in clips.items()}
    normal_image = None
    if normal:
        normals = [tools.normal_from_height(relief(f), 1.5, f[..., 3] > 0) for f in frames]
        normal_image, _ = tools.pack(normals, cell=cell, gutter=gutter, columns=min(8, len(frames)))
        flat = np.zeros_like(normal_image)
        flat[...] = (128, 128, 255, 255)
        normal_image = np.where(normal_image[..., 3:4] > 0, normal_image, flat)
        if edit_normal:
            normal_image = edit_normal(normal_image, atlas)
    if edit_atlas:
        atlas = edit_atlas(atlas)
    path = os.path.join(WORK, name + ".anim.json")
    tools.write_anim_json(path, name + ".png", document, ppu, pivot, normal=name + "_n.png" if normal else None,
                          emissive=name + "_glow.png" if emissive is not None else None, atlas=atlas, normal_image=normal_image,
                          emissive_image=emissive(atlas) if emissive is not None else None)
    if edit_json:
        with open(path, encoding="utf-8") as file:
            data = json.load(file)
        text = edit_json(data)
        data = text if isinstance(text, str) else data
        with open(path, "w", encoding="utf-8") as out:
            out.write(data if isinstance(data, str) else json.dumps(data))
    return path


def profile_of(name, overrides=None):
    return common.profile_named(KIT.profiles(), name, overrides)


def found_in(path, profile):
    findings, _, _ = KIT.validate(path, profile, engine=ENGINE)
    return {f.check for f in findings if f.severity != "info"}, findings


def report(name, problems):
    print(f"  {name:<66} {'ok' if not problems else 'FAILED: ' + '; '.join(problems)}")
    if problems:
        FAILURES.append(name)


def case(name, path, expect=(), profile="character", overrides=SPEC, also=()):
    """Validate `path` and require exactly the ids in `expect` (those in `also` may come with them)."""
    found, _ = found_in(path, profile_of(profile, overrides))
    problems = [f"missed {check}" for check in expect if check not in found]
    extra = sorted(found - set(expect) - set(also))
    if extra:
        problems.append("also reported " + ", ".join(extra))
    report(name, problems)


def changed(base, clip, edit):
    """A copy of the clean sheet with one clip's data edited: edit(body) changes it in place."""
    clips = copy.deepcopy(base)
    edit(clips[clip])
    return clips


def each_frame(body, function, directions=range(4), indices=None):
    for d in directions:
        for i in range(len(body["frames"][d])):
            if indices is None or i in indices:
                body["frames"][d][i] = function(body["frames"][d][i])


# ------------------------------------------------------------------ the cases

def main():
    base = character()
    slug = iter(range(10000))
    new = lambda: f"s{next(slug):03d}"  # noqa: E731

    print("clean sheets")
    case("a clean character is clean", write(new(), base))
    case("the same packed with a gutter is clean", write(new(), base, gutter=2))
    still = {"idle": {"frames": [[figure(0)]], "durations": 1.0, "loop": True, "events": {}}}
    case("a one-frame prop is clean", write(new(), still, normal=False), profile="prop", overrides=None)
    ys, xs = np.mgrid[0:32, 0:32]
    glow = lambda radius: np.dstack([np.full((32, 32, 3), 255), np.clip(255 * (1 - np.hypot(xs - 15.5, ys - 15.5) / radius), 0, 255)]).astype(np.uint8)  # noqa: E731
    burst = {"idle": {"frames": [[glow(r) for r in (4, 8, 12, 14)] + [np.zeros((32, 32, 4), np.uint8)]], "durations": 0.06, "loop": False, "events": {}}}
    case("a soft effect that ends on an empty frame is clean", write(new(), burst, normal=False), profile="effect", overrides=None)
    case("a one-frame glow is clean", write(new(), {"idle": dict(burst["idle"], frames=[[glow(14)]])}, normal=False), profile="image", overrides=None)
    mote = {"idle": {"frames": [[glow(15)[8:24, 8:24], glow(9)[8:24, 8:24]]], "durations": [0.5, 60.0], "loop": False, "events": {}}}
    case("a particle that fills its cell and holds its last frame is clean", write(new(), mote, normal=False), profile="particle", overrides=None)
    alternating = changed(base, "idle", lambda b: b.update(frames=[[figure(d, bob=k) for k in (0, 0, 2, 2)] for d in range(4)]))
    case("a loop whose steps alternate small and large still closes", write(new(), alternating), also=["motion.hold"])
    raised = {clip: dict(body, frames=[[tools.shift(f, 0, -2) for f in row] for row in body["frames"]]) for clip, body in base.items()}
    sword = changed(raised, "idle", lambda b: each_frame(b, lambda f: _with(f, (slice(GROUND - 1, GROUND + 1), slice(23, 25)), (210, 216, 230))))
    case("a scabbard tip hanging below the feet is not the ground", write(new(), sword), overrides=dict(SPEC, pivot_gap=[0, 4], baseline_pop=1))

    print("sheet integrity")
    case("a misspelt key", write(new(), base, edit_json=lambda d: d["clips"]["idle"].update(loops=True)), ["sheet.unknown_key"])
    case("a rectangle of half pixels", write(new(), base, edit_json=lambda d: d["clips"]["idle"]["directions"][0][0].update(
        rect=[0.5, 0, WIDTH, HEIGHT])), ["sheet.json_types"])
    case("three directions (the engine refuses the file)", write(new(), base, edit_json=lambda d: d["clips"]["idle"]["directions"].pop()),
         ["sheet.json"])
    case("a duration of zero (the engine refuses the file)", write(new(), base, edit_json=lambda d: d["clips"]["walk"]["directions"][1][2].update(
        duration=0)), ["sheet.json"])
    case("not JSON at all", write(new(), base, edit_json=lambda d: "{atlas: nope"), ["sheet.json"])
    case("an atlas that is not there", write(new(), base, edit_json=lambda d: d.update(atlas="gone.png")), ["sheet.missing_image"])
    case("a normal map of another size", write(new(), base, edit_normal=lambda n, a: n[:, :-8]), ["sheet.image_size"])
    case("a frame outside the atlas", write(new(), base, edit_json=lambda d: d["clips"]["idle"]["directions"][0][0].update(
        rect=[8 * WIDTH - 8, 0, WIDTH, HEIGHT])), ["sheet.frame_outside"], also=["sheet.unused_cells"])
    path = write(new(), base, edit_json=lambda d: d["clips"]["walk"]["directions"][1].pop())
    case("a direction with fewer frames than the others", path, ["sheet.direction_frames", "timing.direction_length"], also=["sheet.unused_cells"])
    one = changed(base, "attack", lambda b: b.update(frames=b["frames"][:1]))
    case("a clip with one direction on a four-direction character", write(new(), one), ["sheet.directions"])
    case("frames of two sizes in one clip", write(new(), base, edit_json=lambda d: d["clips"]["idle"]["directions"][0][1].update(
        rect=d["clips"]["idle"]["directions"][0][1]["rect"][:2] + [WIDTH - 4, HEIGHT])), ["sheet.frame_size"])
    blink = changed(base, "walk", lambda b: each_frame(b, lambda f: np.zeros_like(f), indices=(2,)))
    case("an empty frame in the middle of a clip", write(new(), blink), ["sheet.empty_frame"], also=["motion.baseline_jitter"])
    nothing = {"idle": {"frames": [[np.zeros((8, 8, 4), np.uint8)] * 3], "durations": [0.5, 1.0, 60.0], "loop": False, "events": {}}}
    case("a sprite whose every frame is empty", write(new(), nothing, normal=False), ["sheet.empty_frame"], profile="particle", overrides=None)
    tail = {"idle": dict(burst["idle"], frames=[burst["idle"]["frames"][0] + [np.zeros((32, 32, 4), np.uint8)]])}
    case("an effect that ends on two empty frames", write(new(), tail, normal=False), ["sheet.empty_frame"], profile="effect", overrides=None)
    case("two rectangles that cut across each other", write(new(), base, gutter=2, edit_json=lambda d: d["clips"]["idle"]["directions"][0][1].update(
        rect=[d["clips"]["idle"]["directions"][0][1]["rect"][0] - 6] + d["clips"]["idle"]["directions"][0][1]["rect"][1:])),
         ["sheet.frame_overlap"], also=["sheet.unused_cells", "motion.drift", "motion.pop", "pixels.edge", "pixels.bleed"])
    case("a clip left out of the .anim.json", write(new(), base, edit_json=lambda d: d["clips"].pop("attack")),
         ["sheet.unused_cells", "timing.missing_clip"], also=["ready.normal_coverage"])

    print("pixel hygiene")
    soft = changed(base, "idle", lambda b: each_frame(b, lambda f: _soften(f)))
    case("anti-aliased edges on a cutout", write(new(), soft), ["pixels.soft_alpha"],
         also=["pixels.colors", "pixels.near_duplicates", "pixels.stray_colors", "ready.normal_coverage"])
    hard = changed(soft, "idle", lambda b: each_frame(b, tools.harden_alpha))
    case("  ... tools.harden_alpha fixes it", write(new(), hard), also=["pixels.colors", "pixels.near_duplicates", "pixels.stray_colors"])
    tall = changed(base, "idle", lambda b: each_frame(b, lambda f: tools.shift(f, 0, 1), directions=(1,)))
    case("a figure standing on the frame's bottom edge", write(new(), tall), ["pixels.edge", "pixels.bleed"])
    case("  ... a gutter stops the bleed, not the cut", write(new(), tall, gutter=1), ["pixels.edge"])
    roomy = {clip: dict(body, frames=[tools.trim_and_centre(row, (WIDTH + 2, HEIGHT + 2)) for row in body["frames"]]) for clip, body in tall.items()}
    case("  ... tools.trim_and_centre into a larger cell fixes it", write(new(), roomy))
    dusty = changed(base, "walk", lambda b: each_frame(b, lambda f: _with(f, (slice(5, 6), slice(3, 5)), TUNIC), indices=(1, 4)))
    case("stray pixels beside the figure", write(new(), dusty), ["pixels.specks"])
    swept = changed(dusty, "walk", lambda b: each_frame(b, lambda f: tools.remove_specks(f, 4)))
    case("  ... tools.remove_specks fixes it", write(new(), swept))
    holed = changed(base, "walk", lambda b: each_frame(b, lambda f: _with(f, (slice(26, 27), slice(18, 19)), None), indices=(0, 3)))
    case("pinholes in the figure", write(new(), holed), ["pixels.pinholes"])
    filled = changed(holed, "walk", lambda b: each_frame(b, lambda f: tools.fill_pinholes(f, 1)))
    case("  ... tools.fill_pinholes fixes it", write(new(), filled))
    case("more colours than the profile allows", write(new(), base), ["pixels.colors"], overrides=dict(SPEC, max_colors=4))
    twin = changed(base, "idle", lambda b: each_frame(b, lambda f: _recolour(f, TUNIC, (42, 91, 168), rows=slice(26, 30))))
    case("the same blue mixed twice", write(new(), twin), ["pixels.near_duplicates"])
    merged = {clip: dict(body, frames=_together(body["frames"], lambda fs: tools.merge_near_duplicates(fs, 12))) for clip, body in twin.items()}
    case("  ... tools.merge_near_duplicates fixes it", write(new(), merged))
    odd = changed(base, "idle", lambda b: b["frames"][0].__setitem__(0, _with(b["frames"][0][0], (slice(24, 25), slice(16, 19)), (250, 40, 200))))
    case("a colour that appears three times in the whole sheet", write(new(), odd), ["pixels.stray_colors"])
    palette = [OUTLINE, SKIN, HAIR, TUNIC, SHADE, BOOT, (210, 216, 230)]
    mapped = {clip: dict(body, frames=_together(body["frames"], lambda fs: tools.quantize(fs, palette))) for clip, body in odd.items()}
    case("  ... tools.quantize onto the palette fixes it", write(new(), mapped))

    print("motion")
    frozen = changed(base, "idle", lambda b: b.update(frames=[[row[0]] * 4 for row in b["frames"]]))
    case("a four-frame clip in which nothing moves", write(new(), frozen), ["motion.static"])
    hop = changed(base, "walk", lambda b: each_frame(b, lambda f: tools.shift(f, 0, -3), indices=(1, 4)))
    case("feet that leave the ground in a walk", write(new(), hop), ["motion.baseline_jitter"], also=["motion.pop"])
    planted = changed(hop, "walk", lambda b: b.update(frames=[tools.align_baseline(row) for row in b["frames"]]))
    case("  ... tools.align_baseline fixes it", write(new(), planted))
    high = changed(base, "attack", lambda b: each_frame(b, lambda f: tools.shift(f, 0, -4)))
    case("a clip drawn four pixels above the idle ground", write(new(), high), ["motion.baseline_pop"])
    level = changed(high, "attack", lambda b: b.update(frames=[tools.align_baseline(row, GROUND) for row in b["frames"]]))
    case("  ... tools.align_baseline(frames, row) fixes it", write(new(), level))
    big = changed(base, "attack", lambda b: each_frame(b, lambda f: _scale(f, 1.25)))
    case("a clip drawn a quarter larger", write(new(), big), ["motion.scale_pop"], also=["pixels.edge", "pixels.bleed", "pixels.near_duplicates"])
    slide = changed(base, "idle", lambda b: b.update(frames=[[tools.shift(f, 2 * i, 0) for i, f in enumerate(row)] for row in b["frames"]]))
    case("a body that slides sideways in an idle (and jumps back)", write(new(), slide), ["motion.drift", "motion.loop_seam"])
    wrong = changed(base, "walk", lambda b: each_frame(b, lambda f: tools.shift(f, 5, 0), indices=(2,), directions=(0,)))
    case("one frame five pixels out of place", write(new(), wrong), ["motion.pop"], also=["motion.drift"])
    drifting = {"idle": {"frames": [[tools.shift(glow(8), dx, 0) for dx in (-6, -3, 0, 3, 6)]], "durations": 0.1, "loop": True, "events": {}}}
    case("a looping effect that drifts across and snaps back", write(new(), drifting, normal=False), ["motion.loop_seam"], profile="effect",
         overrides=None)
    lopsided = changed(base, "idle", lambda b: each_frame(b, lambda f: tools.shift(f, 0, -4), directions=(3,)))
    case("an east row four pixels above the west row", write(new(), lopsided), ["motion.mirror", "motion.baseline_pop"])
    mirrored = changed(lopsided, "idle", lambda b: b["frames"].__setitem__(3, tools.mirror_direction(b["frames"][1])))
    case("  ... tools.mirror_direction of the west row fixes it", write(new(), mirrored), also=["motion.baseline_pop"])

    print("clips and timing")
    case("no idle clip", write(new(), {"stand": base["idle"]}), ["timing.no_idle"], overrides=dict(SPEC, clips={}))
    case("a clip the game plays is missing", write(new(), {k: v for k, v in base.items() if k != "attack"}), ["timing.missing_clip"])
    case("a clip the game waits on is marked loop", write(new(), changed(base, "attack", lambda b: b.update(loop=True))), ["timing.loop"])
    case("a walk that does not loop", write(new(), changed(base, "walk", lambda b: b.update(loop=False))), ["timing.loop"])
    case("one direction slower than the others", write(new(), base, edit_json=lambda d: d["clips"]["idle"]["directions"][2][0].update(duration=0.4)),
         ["timing.direction_length"])
    case("a walk cycle that takes two seconds", write(new(), changed(base, "walk", lambda b: b.update(durations=0.34))),
         ["timing.length", "timing.event_time"])
    case("fewer frames than the spec lists", write(new(), changed(base, "idle", lambda b: b.update(
        frames=[row[:2] for row in b["frames"]], durations=0.6))), ["timing.frames"])
    case("a frame shorter than a tick", write(new(), changed(base, "attack", lambda b: b.update(durations=[0.1, 0.09, 0.01, 0.3]))),
         ["timing.frame_duration"])
    case("a three-second hold in the middle of a clip", write(new(), changed(base, "idle", lambda b: b.update(durations=[0.3, 3.0, 0.3, 0.3]))),
         ["timing.frame_duration", "timing.length"])
    case("an attack with no hit event", write(new(), changed(base, "attack", lambda b: b.update(events={}))), ["timing.events_missing"])
    case("the hit one frame late", write(new(), changed(base, "attack", lambda b: b.update(events={3: "hit"}))), ["timing.event_time"])
    case("a misspelt event", write(new(), changed(base, "walk", lambda b: b.update(events={0: "step", 3: "setp"}))),
         ["timing.event_unknown", "timing.events_missing"])
    case("an event on another frame in one direction", write(new(), base, edit_json=_move_event), ["timing.event_directions", "timing.event_time"])

    print("game readiness")
    case("a file name with capitals", write("Hero_Sheet", base), ["ready.naming"])
    case("the wrong pixels per unit", write(new(), base, ppu=16), ["ready.pixels_per_unit", "ready.world_height"])
    case("a pivot in the middle of a character", write(new(), base, pivot=(0.5, 0.5)), ["ready.pivot", "ready.pivot_feet"])
    case("an atlas over the budget", write(new(), base), ["ready.atlas_size"], overrides=dict(SPEC, max_atlas_size=256))
    case("an atlas that is not a power of two", write(new(), base), ["ready.atlas_pot"], overrides=dict(SPEC, power_of_two=True))
    case("an atlas that is mostly unused", write(new(), base, normal=False, edit_atlas=lambda a: np.pad(a, ((0, 400), (0, 400), (0, 0)))),
         ["ready.atlas_unused"], overrides=dict(SPEC, normal="optional"))
    tiny = {clip: dict(body, frames=[[_scale(f, 0.5) for f in row] for row in body["frames"]]) for clip, body in base.items()}
    case("a figure half the height of a hero", write(new(), tiny), ["ready.world_height"], also=["pixels.near_duplicates"])
    floating = {clip: dict(body, frames=[[tools.shift(f, 0, -5) for f in row] for row in body["frames"]]) for clip, body in base.items()}
    case("feet five pixels above the pivot", write(new(), floating), ["ready.pivot_feet"])
    case("no normal map where the profile asks for one", write(new(), base, normal=False), ["ready.normal_missing"])
    case("a normal map with zero-length normals", write(new(), base, edit_normal=lambda n, a: np.where(
        (a[..., 3:4] > 0) & (np.arange(n.shape[1])[None, :, None] % 7 == 0), np.array([128, 128, 128, 255], np.uint8), n)), ["ready.normal_length"])
    case("a normal map that faces away", write(new(), base, edit_normal=lambda n, a: np.where(
        a[..., 3:4] > 0, np.dstack([n[..., :2], 255 - n[..., 2], n[..., 3]]), n)), ["ready.normal_facing"])
    case("a normal map that is flat", write(new(), base, edit_normal=lambda n, a: np.full_like(n, (128, 128, 255, 255))), ["ready.normal_flat"])
    case("a normal map laid out one cell off", write(new(), base, edit_normal=lambda n, a: np.roll(n, WIDTH // 2, axis=1)),
         ["ready.normal_coverage"])
    case("a normal map with green pointing down", write(new(), base, edit_normal=lambda n, a: np.dstack([n[..., 0], 255 - n[..., 1], n[..., 2:]])),
         ["ready.normal_convention"])
    flipped = {clip: dict(body, frames=[tools.mirror_direction(row) for row in body["frames"]]) for clip, body in base.items()}
    case("a normal map mirrored with its sheet, the red channel left as it was", write(new(), flipped, edit_normal=lambda n, a: _mirror_cells(
        _mirror_cells(n), red=True)), ["ready.normal_convention"])
    case("  ... tools.mirror_direction(normal=True) turns the red channel too", write(new(), flipped))
    case("an emissive image that is black", write(new(), base, emissive=lambda a: np.zeros_like(a)), ["ready.emissive_dark"])
    case("an emissive image that glows outside the figure", write(new(), base, emissive=lambda a: np.full_like(a, (155, 92, 255, 255))),
         ["ready.emissive_outside"])
    case("an emissive image inside the figure is clean", write(new(), base, emissive=lambda a: np.where(
        (a[..., :3] == np.array(SKIN)).all(axis=2, keepdims=True), np.array([155, 92, 255, 255], np.uint8), 0).astype(np.uint8)))
    broken = os.path.join(WORK, "unreadable.anim.json")
    with open(broken, "wb") as out:
        out.write(b"\xff\xfe\x00{")
    case("a file that cannot be read as text", broken, ["ready.structure"])

    helpers()
    reviews_and_waivers(write(new(), dusty))
    if ENGINE:
        engine(base)
    shutil.rmtree(WORK, ignore_errors=True)
    print(f"\n{'all checks proved' if not FAILURES else str(len(FAILURES)) + ' FAILED: ' + ', '.join(FAILURES)}")
    return 1 if FAILURES else 0


# ------------------------------------------------------------------ fixtures' small tools

def _with(frame, where, colour):
    """A copy of the frame with a region painted (or cleared when colour is None)."""
    out = frame.copy()
    out[where] = 0 if colour is None else (*colour, 255)
    return out


def _soften(frame):
    """The frame as a smooth resize would leave it: half-transparent pixels along its edges."""
    from PIL import Image
    small = Image.fromarray(frame).resize((frame.shape[1] * 2, frame.shape[0] * 2), Image.BILINEAR).resize((frame.shape[1], frame.shape[0]), Image.BILINEAR)
    return np.asarray(small).copy()


def _scale(frame, factor):
    """The figure redrawn at another scale, standing on the same spot."""
    from PIL import Image
    size = (round(frame.shape[1] * factor), round(frame.shape[0] * factor))
    scaled = np.asarray(Image.fromarray(frame).resize(size, Image.NEAREST))
    shift_down = round((HEIGHT - 1 - GROUND) * (factor - 1))
    return tools.shift(tools._on_canvas(scaled, frame.shape[1], frame.shape[0]), 0, shift_down)


def _recolour(frame, old, new, rows):
    out = frame.copy()
    region = out[rows]
    region[(region[..., :3] == np.array(old)).all(axis=2), :3] = new
    return out


def _together(rows, function):
    """Applies a helper that takes a list of frames to every frame of a clip at once."""
    flat = function([f for row in rows for f in row])
    out, index = [], 0
    for row in rows:
        out.append(flat[index:index + len(row)])
        index += len(row)
    return out


def _move_event(data):
    frames = data["clips"]["attack"]["directions"][1]
    frames[2].pop("event")
    frames[1]["event"] = "hit"


def _mirror_cells(normal, red=False):
    """Every cell of a packed normal map flipped left to right; with `red`, its red channel turned as well. Flipping the
    pixels and the channel is what mirroring a sprite does to its normals; a generator that forgets the channel (flip, then
    flip back with it) leaves a map whose every normal leans the wrong way sideways."""
    out = normal.copy()
    for x in range(0, normal.shape[1] - WIDTH + 1, WIDTH):
        out[:, x:x + WIDTH] = tools.mirror_direction([normal[:, x:x + WIDTH]], normal=red)[0]
    return out


# ------------------------------------------------------------------ helpers, reviews, waivers, the engine

def helpers():
    print("helpers")
    frames = [figure(0, bob=b) for b in (0, 1, 0, 1)]
    atlas, rects = tools.pack(frames, gutter=2, columns=2)
    ok = atlas.shape[:2] == (2 + 2 * (HEIGHT + 2), 2 + 2 * (WIDTH + 2)) and all(
        np.array_equal(atlas[y:y + h, x:x + w], f) for (x, y, w, h), f in zip(rects, frames)) and not atlas[:2].any() and not atlas[:, :2].any()
    report("pack lays frames on a grid with a transparent gutter", [] if ok else ["the atlas or the rectangles are wrong"])
    atlas, rects = tools.pack(frames, power_of_two=True)
    report("pack(power_of_two=True) rounds the atlas up", [] if atlas.shape[:2] == (128, 128) else [str(atlas.shape)])
    path = os.path.join(WORK, "helper.anim.json")
    tools.write_anim_json(path, "helper.png", {"idle": tools.clip([rects], 0.25, events={1: "blink"})}, 32, (0.5, 1), atlas=atlas)
    loaded = sheets.load(path)
    ok = not loaded.problems and loaded.clips["idle"].directions[0][1].event == "blink" and abs(loaded.clips["idle"].length() - 1.0) < 1e-9 \
        and np.array_equal(loaded.atlas, atlas)
    report("write_anim_json writes what the loader reads back", [] if ok else [str(loaded.problems)])
    try:
        tools.write_anim_json(path, "helper.png", {"idle": {"loop": True, "directions": [[], []]}}, 32, (0.5, 1))
        report("write_anim_json refuses what the engine would refuse", ["it wrote a clip of two empty directions"])
    except ValueError:
        report("write_anim_json refuses what the engine would refuse", [])
    dome = np.zeros((33, 33))
    ys, xs = np.mgrid[0:33, 0:33]
    inside = np.hypot(xs - 16, ys - 16) <= 14
    dome[inside] = np.sqrt(14 ** 2 - (xs[inside] - 16) ** 2 - (ys[inside] - 16) ** 2)
    normal = tools.normal_from_height(dome, 1.0, inside)
    ok = normal[16, 6, 0] < 110 and normal[16, 26, 0] > 146 and normal[6, 16, 1] > 146 and normal[26, 16, 1] < 110 and normal[16, 16, 2] > 250 \
        and tuple(normal[0, 0, :3]) == (128, 128, 255)
    report("normal_from_height: red right, green up, blue to the viewer, flat outside", [] if ok else ["wrong channel directions"])
    lengths = np.linalg.norm(normal[..., :3].astype(float) / 127.5 - 1, axis=2)
    report("normal_from_height writes unit normals", [] if abs(lengths - 1).max() < 0.03 else [f"lengths up to {lengths.max():.2f}"])
    flipped = tools.mirror_direction([normal], normal=True)[0]
    ok = abs(int(flipped[16, 6, 0]) - int(normal[16, 6, 0])) <= 2 and tuple(flipped[0, 0, :3]) in ((127, 128, 255), (128, 128, 255))
    report("mirror_direction(normal=True) turns the red channel with the pixels", [] if ok else ["a mirrored dome is not the dome"])
    closing = [np.roll(frames[0], dx, axis=1) for dx in (0, 1, 2, 1)]
    jumping = [np.roll(frames[0], dx, axis=1) for dx in (0, 2, 4, 6)]
    ok = tools.loop_seam(closing)[2] < 1.2 and tools.loop_seam(jumping)[2] > 1.8 and tools.loop_seam(frames[:2]) is None
    report("loop_seam: about 1 for a loop that closes, well above for one that jumps", [] if ok else [
        f"{tools.loop_seam(closing)[2]:.2f} and {tools.loop_seam(jumping)[2]:.2f}"])
    centred = tools.trim_and_centre([tools.shift(f, 5, -3) for f in frames], margin=2)
    box = tools.bounds(np.any([tools.visible(f) for f in centred], axis=0))
    ok = box[0] == 2 and box[2] == 2 and centred[0].shape[0] - 1 - box[1] == 2 and centred[0].shape[1] - 1 - box[3] == 2
    report("trim_and_centre leaves exactly the margin asked for", [] if ok else [str(box)])
    cleared = tools.clear_hidden_colour(_with(frames[0], (slice(0, 2), slice(0, 2)), None) | np.array([9, 9, 9, 0], np.uint8))
    report("clear_hidden_colour blanks the colour under transparent pixels", [] if not cleared[cleared[..., 3] == 0, :3].any() else ["colour left"])
    reduced = tools.quantize(frames, 3)
    report("quantize(frames, 3) leaves three colours", [] if len(tools.colours(reduced)[0]) == 3 else [str(len(tools.colours(reduced)[0]))])


def reviews_and_waivers(path):
    print("visual consistency (the sign-off)")
    profile = profile_of("character", SPEC)
    digest = KIT.digest(path)
    for name, review, expected in (
            ("a sprite nobody has looked at", None, True),
            ("a review of an older version of the files", {"verdict": "pass", "sha256": "0" * 64}, True),
            ("a review that failed it", {"verdict": "fail", "sha256": digest, "notes": "the walk limps"}, True),
            ("a review that passed these exact files", {"verdict": "pass", "sha256": digest}, False)):
        findings, _, _ = KIT.validate(path, profile, review=review, require_review=True)
        blocked = any(f.check == "visual.review" for f in findings)
        report(name, [] if blocked == expected else ["blocked" if blocked else "let through"])
    with open(path[:-len(".anim.json")] + "_n.png", "ab") as out:      # the normal map changes: the review is of older files
        out.write(b" ")
    findings, _, _ = KIT.validate(path, profile, review={"verdict": "pass", "sha256": digest}, require_review=True)
    report("changing the normal map voids the review", [] if any(f.check == "visual.review" for f in findings) else ["still passes"])
    from spritekit import draw
    findings, _, subject = KIT.validate(path, profile)
    pages = draw.pages(subject, profile, findings, "selftest", ["profile: character"], [("other.anim.json", path)], None, HERE)
    report("the review of a small sheet is one page: its clips, then the overview", [] if len(pages) == 1 and pages[0].height > 900 else [
        f"{len(pages)} pages"])
    many = {f"{clip}_{n}" if n else clip: body for n in range(5) for clip, body in character().items()}
    findings, _, subject = KIT.validate(write("many", many), profile)
    pages = draw.pages(subject, profile, findings, "selftest", ["profile: character"], [], None, HERE)
    folder = os.path.join(WORK, "pages")
    os.makedirs(folder)
    with contextlib.redirect_stdout(io.StringIO()):
        draw.Pages(pages).save(os.path.join(folder, "many.png"))
    written = sorted(os.listdir(folder))
    report("the review of a large sheet is an overview and pages of clips", [] if len(pages) >= 3 and written[:2] == ["many.png", "many_2.png"] else [
        ", ".join(written)])

    print("waivers")

    def left(waivers, check="pixels.specks", **more):
        findings, _, _ = KIT.validate(path, profile, waivers, **more)
        return any(f.check == check and not f.waived for f in findings)

    for name, got, expected in (
            ("a flaw with no waiver fails", left({}), True),
            ("a waiver with a reason lets it through", left({"pixels.specks": "By design: sparks"}), False),
            ("a capped waiver holds while the count is under the cap", left({"pixels.specks": {"reason": "known", "max": 16}}), False),
            ("a capped waiver stops holding when the flaw grows", left({"pixels.specks": {"reason": "known", "max": 3}}), True),
            ("a waiver with no reason is not a waiver", left({"pixels.specks": {"max": 99}}), True),
            ("the visual review cannot be waived", left({"visual.review": "trust me"}, check="visual.review", require_review=True), True)):
        report(name, [] if got == expected else ["waived" if expected else "not waived"])
    bad = write("refused", character(), edit_json=lambda d: d["clips"]["idle"]["directions"].pop())
    findings, _, _ = KIT.validate(bad, profile, {"sheet.json": "trust me"})
    report("a sheet the engine refuses cannot be waived", [] if any(f.check == "sheet.json" and not f.waived for f in findings) else ["waived"])


def engine(base):
    """With --engine: the engine's loader agrees with this kit's about what is a sheet."""
    print("the engine's loader")
    profile = profile_of("character", SPEC)
    for name, path, refuses in (
            ("the clean character loads in the engine", write("engine_ok", base), False),
            ("the engine refuses three directions too", write("engine_three", base, edit_json=lambda d: d["clips"]["idle"]["directions"].pop()), True),
            ("the engine refuses a negative rectangle too", write("engine_rect", base, edit_json=lambda d: d["clips"]["idle"]["directions"][0][0].update(
                rect=[-1, 0, 8, 8])), True),
            ("the engine refuses a string for a duration too", write("engine_dur", base, edit_json=lambda d: d["clips"]["idle"]["directions"][0][0].update(
                duration="fast")), True)):
        findings, _, _ = KIT.validate(path, profile, engine=ENGINE)
        disagree = [f.message for f in findings if f.check == "ready.engine_load"]
        ours = any(f.check == "sheet.json" for f in findings)
        report(name, ([] if ours == refuses else ["this kit " + ("refuses" if ours else "accepts") + " it"]) + disagree)


if __name__ == "__main__":
    sys.exit(main())
