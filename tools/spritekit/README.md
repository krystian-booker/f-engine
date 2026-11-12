# spritekit: game-ready sprite sheets

This folder is the process for every sprite sheet that goes into a game on
this engine, written for the AI agents (and people) who generate them: how to
build a sheet, the helpers that keep it clean, and the gate it has to pass.
A sprite is an `.anim.json` with the atlas it names and, optionally, a normal
map and an emissive image: characters, props, effects played as sprites, and
the sprites of particles.

A sprite is **ready** when this exits 0:

```sh
python3 tools/spritekit/validate.py --manifest tools/<game>/sprites.json --engine build/fengine_headless
```

That one command runs six checks on every sprite the game ships:

| # | Check | What it answers |
| --- | --- | --- |
| 1 | Sheet integrity | Is the sheet sound? What the engine refuses or ignores in the JSON, missing or mis-sized images, frames outside the atlas, empty frames, cells no clip uses |
| 2 | Pixel hygiene | Is it clean pixel art? Soft alpha on a cutout, content against the frame's edge, specks, pinholes, colour count, near-duplicate and stray colours |
| 3 | Motion | Does it move right? Feet leaving the ground row, a clip standing higher than idle, a body that slides, a frame out of place, a loop that jumps, west unlike east |
| 4 | Clips and timing | Is it what the game plays? Required clips, `loop`, lengths and frame counts against the spec, frames shorter than a tick, events present and on time |
| 5 | Game readiness | Does it fit the game? Atlas budget, pixels per unit, pivot at the feet, size in world units, the normal map's convention, the emissive image, names, and whether the engine loads it |
| 6 | Visual consistency | Does it look right? A review of its sheets against the game's art direction, signed off against the files' hash |

Checks 1 to 5 are measured. Check 6 is judged: `review.py` draws the sprite,
someone looks, and the verdict is recorded. `validate.py` then refuses any
sprite whose review is missing, failed, or older than its files.

## Setup

Python 3.11+ with `numpy` and `pillow` (`pip install numpy pillow`); nothing
else, for the gate, the helpers and the self-test. The engine checks need a
build: `build/fengine_headless` to load sheets, `build/fengine` for engine
shots (on a machine without a display it runs under `xvfb-run` by itself).

`python3 tools/spritekit/selftest.py` proves the validator: it generates one
flawed sheet per check and expects each to be caught and nothing else to be
reported, clean sheets to come back clean, and every helper to fix what it
claims (`--engine build/fengine_headless` also holds this kit's reading of
the format against the engine's own loader).

## The process

Follow these steps in order. Do not skip to packing.

1. **Read the brief.** The game's art direction (for Aetherbound,
   `tools/aetherbound/SPEC.md`): the look, the palette, the frame size, the
   clips with their frame counts, durations and events, and the size of a
   hero. Pick the sprite's **profile** (below).
2. **State the intent first.** Put the sprite in the game's manifest with its
   profile, and what the game expects of its clips under the profile's
   `clips` (or the sprite's own `overrides`): which loop, which stand on the
   ground, which events fire when. The checks hold the sheet to that; see
   [The spec of a sprite](#the-spec-of-a-sprite).
3. **Design the figure at game size.** A hero is 48 px in a 64 px frame; what
   does not read as a black shape at that size will not be saved by detail.
   Fix the palette (a table of colours, a few tones per material) before the
   first frame.
4. **Build it as a generator script**, not by hand: a Python file under
   `tools/<game>/` that draws every frame from nothing, so it can be
   regenerated and fixed. Follow [the rules of the craft](#rules-of-the-craft).
5. **Clean the frames** with the helpers, in this order: `harden_alpha`,
   `remove_specks`, `fill_pinholes`, `quantize` (onto the palette), then
   `align_baseline` per clip and direction.
6. **Pack and write** with `pack` and `write_anim_json`: one cell size per
   sheet, a clear pixel inside every frame, the same layout for the normal
   map and the emissive image. `write_anim_json` refuses what the engine
   would refuse.
7. **Validate** while iterating:
   `python3 tools/spritekit/validate.py out/hero.anim.json --profile character --engine build/fengine_headless`
   (or `--manifest ... sprites/hero.anim.json --no-review` to hold it to the
   game's own spec). Every finding names the clip, direction and frames and
   how to fix it. Fix the generator, not the PNG.
8. **Review**: `python3 tools/spritekit/review.py --manifest ... sprites/hero.anim.json --engine build/fengine`
   writes the review sheets. **Open every page and look** against the
   [review questions](#6-visual-consistency). Then record the verdict with
   `--sign pass|fail --by ... --notes ...`.
9. **Register** it: run the manifest validation, and commit the sheet, its
   images, its generator and the manifest together.

A sprite that fails is not shipped with a waiver to make the run green.
Waivers are for flaws that are deliberate, or known and scheduled; see
[the manifest](#the-manifest). A new sprite starts with no waivers.

## What the engine expects

Verified in `engine/assets/SpriteAnimation.cpp`, `engine/render/GpuAssets.cpp`,
`engine/render/SpriteGeometry.cpp`, `engine/world/Animation.cpp` and
`shaders/fs_sprite.sc`; `docs/authoring.md` ("Sprites") is the engine's own
account.

| | |
| --- | --- |
| File | `<name>.anim.json`: strict JSON, `{"atlas", "normal"?, "emissive"?, "pixels_per_unit"?, "pivot"?, "clips"}`. Images are named relative to the `.anim.json` and decoded to 8-bit RGBA; use PNG. Any other key is ignored without a word |
| Clip | `{"loop"?: true, "directions": [...]}` with 1, 4 or 8 direction rows. A row is a list of frames `{"rect": [x, y, w, h], "duration"?: 0.15, "event"?: "name"}`: whole pixels, `w` and `h` at least 1, duration above 0. An empty row, another number of rows or no clips at all and the engine refuses the whole sheet |
| Directions | Rows run clockwise seen from the camera, starting with the one that faces the viewer: 4 rows are south, west, north, east; 8 add the diagonals between them. A fixed `sprite.direction` beyond a clip's rows uses the nearest row |
| Sampling | **Nearest, no mip maps, clamped** (sprite atlases are uploaded with point filtering). A frame's neighbours never bleed in through sampling, so a sheet needs no gutter and no power-of-two size. The engine's **outline** and **distort** effects, however, read `outline_width` texels (1 by default) to each side of a pixel and do not stop at the frame's rectangle: a figure that touches its frame's edge loses its outline there, and its pixels show up as stray outline dots on the frame beside it in the atlas. Keep one transparent pixel inside every frame |
| Alpha | Straight alpha. A sprite drawn `opaque` (the default; characters and props) is a cutout: pixels under alpha 0.5 are dropped and the rest drawn solid, so anything between 0 and 255 is wrong there. A blended sprite (`alpha`, `additive`, `multiply`, `premultiplied`) keeps every pixel from alpha 0.01 up. Colour under fully transparent pixels is never read |
| Size | A frame is drawn `w / pixels_per_unit` by `h / pixels_per_unit` world units, times the sprite's `scale`. `pixels_per_unit` is 1 to 4096 and **defaults to 32 when the file gives none**; `game.json`'s `pixels_per_unit` is not used for a sheet that loads |
| Pivot | `[x, y]` as fractions of the frame, x from the left and y from the top; the default is `[0.5, 1]`. That point of the frame sits on the entity's position (the feet, on the ground). Frames of different sizes in one clip therefore shift as it plays |
| Playing | A clip name the sheet lacks plays `idle`, and without an `idle` the first clip by name: frames, events and the end all follow that clip. A clip that does not loop holds its last frame and emits `anim_finished` once when its time is up; a looping clip never does |
| Events | A frame's `event` is emitted as an `anim_event` when the frame begins (on the first frame: as the clip starts, and again each time a loop comes round). The game ticks at 60 Hz: a frame shorter than 1/60 s is skipped on some passes, though its event still fires |
| Particles | An emitter's sprite plays its `idle` clip (or the first by name), direction row 0, over each particle's **age in seconds**; the clip is not stretched to the particle's life. A clip that does not loop, with a long last frame, is the usual form |
| Normal map | `"normal"`: an image the size of the atlas, laid out like it, read as plain numbers (not sRGB): red = right, green = **up**, blue = towards the viewer, each `value / 255 * 2 - 1`. With it the sprite is lit per pixel. Another size and the engine logs an error and draws the sprite without it; a missing file is ignored silently |
| Emissive | `"emissive"`: the same layout; its colour times its alpha is added unlit (eyes, runes, veins) |
| `fengine_assetc` | `sheet sheet.json out.anim.json` describes a uniform grid sheet (`frame_width`, `frame_height`, `directions`, clips of column indices): it writes no `pivot`, `normal` or events, so add them. `atlas manifest.json outdir` packs loose frames with padding. The helpers below do both and write everything |

## Profiles

`profiles.json` holds the conventions per kind of sprite. A game's manifest
may override a value or add a profile under `"profiles"`, and one sprite may
override values under its own `"overrides"`.

| Profile | For | Alpha | Directions | Pivot, size | Checked on top of the common checks |
| --- | --- | --- | --- | --- | --- |
| `character` | a figure that stands and turns: heroes, enemies, townsfolk | hard | 4 or 8 | `[0.5, 1]`, 32 px per unit, 1.0 to 2.2 units tall | frame margin, specks, pinholes, 64 colours, near-duplicate and stray colours, the ground row, drift, scale, west against east, `idle` required |
| `prop` | pixel art that does not walk: a chest, a sign, a dropped sword | hard | 1, 4 or 8 | any | frame margin, specks, pinholes, 48 colours, near duplicates |
| `effect` | a flipbook where something happens: a slash, an impact, a spell | soft | 1 | any | frame margin (content with alpha 24 or more at the edge), 0.1 to 3 s, no frame over 0.5 s unless it is the last of a clip that ends |
| `image` | one still picture: a blob shadow, a magic circle, a glow, a light shaft | soft | 1 | any | frame margin, exactly one frame, atlas mostly used |
| `particle` | the sprite of an emitter's particles | soft | 1 | any | nothing about edges (a particle fills its cell) or long frames (it holds its last) |
| `default` | anything else | soft | any | any | only what is wrong for every sprite: use a real profile |

Every profile also checks what holds for any sheet: the JSON, the images,
rectangles, empty frames, unused cells, loops that jump, frames shorter than
a tick, names, the atlas within 2048 px a side and not mostly unused, the
normal map and the emissive image when there are any.

### The spec of a sprite

State what the game expects once, and the checks hold every sheet of that
profile to it. Aetherbound's `sprites.json` gives its `character` profile the
clip table of its `SPEC.md`:

```json
"profiles": {"character": {
  "directions": [4], "world_height": [1.35, 1.65], "normal": "required", "event_names": ["step", "hit"],
  "clips": {
    "idle":   {"stance": "upright", "loop": true,  "in_place": true, "frames": 4, "seconds": 1.2},
    "walk":   {"stance": "upright", "loop": true,  "in_place": true, "frames": 6, "seconds": 0.6, "events": {"step": [0.0, 0.3]}},
    "attack": {"stance": "upright", "loop": false, "frames": 6, "seconds": 0.6, "events": {"hit": 0.24}},
    "kneel":  {"stance": "low",     "loop": false, "frames": 1},
    "thrown": {"stance": "air",     "loop": false, "frames": 1}
  }}}
```

| Key of a clip | Means |
| --- | --- |
| *(the clip is listed)* | The game plays it: the sheet must have it (`"optional": true` if not) |
| `loop` | What `loop` must be. The game waits for `anim_finished` on a clip that ends; a walk must come round |
| `stance` | `upright`: stands on the idle clip's ground row, steady, the same amount of figure as idle, west like east. `low` (kneeling, lying): on the ground row and steady, any shape. `air` (thrown, jumping): not held to the ground. No stance: not held to anything |
| `in_place` | The body's centre must not slide sideways (the engine moves the sprite; idle, walk and run play on the spot) |
| `frames` | The number of frames per direction: a number, or `[least, most]` |
| `seconds` | The clip's length: a number (within 0.01 s), or `[least, most]` |
| `events` | The events the game listens for: a list of names (one entry per time it must fire), or `{name: seconds}` (a list of seconds for several) when the game's own timers depend on the moment. Aetherbound's battle waits 0.24 s after starting `attack` before it lands the blow, so `hit` must fire then |
| `baseline_jitter` | How many pixels the feet may move in this clip (a run has a flight phase: 3) |

The profile's `event_names` lists every event the game knows, so a misspelt
one is found; `required_clips` adds clips that must exist without saying
more about them. `profiles.json` gives `idle`, `walk`, `run`, `attack`,
`hurt` and `ko` sensible conventions (`clip_kinds`) for a game that states
nothing; what the manifest says about a clip wins.

## Rules of the craft

These are what a professional would do without being asked, and what the
checks then find nothing to say about.

**Form**
- Silhouette before detail. At 48 px a figure has room for a head, a torso,
  limbs and one prop; exaggerate what tells it from the rest of the cast
  (hair, a cape, a weapon) and check it in black.
- One scale for the whole cast, and for every clip of a figure: a pose
  changes the outline, not the amount of figure.
- One light direction in every frame and every direction (upper left is the
  convention of the Aetherbound cast). The normal map does the moving light;
  the painted shading must not fight it.

**Pixels**
- Alpha is 0 or 255 on anything drawn as a cutout. Never scale, rotate or
  blur pixel art with a smooth filter; redraw it, or scale by whole factors
  with nearest filtering.
- A limited palette from one table: three or four tones per material, one
  outline tone per ramp. Do not compute colours per pixel; two tones a few
  steps apart are one tone mixed twice.
- No orphan pixels and no one-pixel holes. A real gap (between an arm and
  the body) is at least two pixels wide.
- One clear pixel between the figure and each edge of its frame, weapon tips
  and capes included. If a pose does not fit, use a larger cell for the
  whole sheet.
- Soft effects fade to nothing before the frame's edge. Drawn additively, a
  glow that still has alpha at the edge shows its frame as a box.

**Motion**
- The planted foot stays on one row through a clip, and that row is the
  same in every grounded clip and direction. The body bobs; the ground does
  not. Kneeling and lying poses touch the same row.
- Clips that play on the spot keep the body over the pivot. The engine
  moves the sprite.
- A loop's last frame leads into its first: draw the cycle on a wheel, not
  on a line. Do not repeat the first frame at the end.
- An attack has anticipation, one frame that is the hit (the `hit` event is
  on it) and a recovery. A walk has contact, pass, contact; the `step`
  events are on the contact frames.
- East and west are the same figure. Mirror one from the other only when
  nothing about it is handed (a sword changes hands when mirrored).

**The sheet**
- One cell size per sheet; every direction of a clip has the same number of
  frames with the same durations and the same events.
- Every cell that has pixels belongs to a clip, and every frame of a clip
  has pixels, except the last frame of an effect that ends on nothing.
- An effect that must vanish ends, without `loop`, on **one** empty frame.
- The normal map and the emissive image come from the very frames in the
  atlas, in the same cells: flat `(128, 128, 255)` and black outside the
  figure.
- Names are lower case with underscores: `hero.anim.json`, `hero.png`,
  `hero_n.png`, clip `look_up`. Paths and clip names are matched exactly.

## The helpers

`spritekit/tools.py`, used inside a generator (numpy and pillow only). A
frame is a `(height, width, 4)` uint8 RGBA array; every function returns new
arrays.

```python
import sys; sys.path.insert(0, "tools/spritekit")
from spritekit import tools as sk

frames = [sk.harden_alpha(f) for f in frames]                 # alpha 0 or 255
frames = [sk.fill_pinholes(sk.remove_specks(f, 4), 1) for f in frames]
frames = sk.quantize(frames, PALETTE)                         # only the palette's colours
walk = sk.align_baseline(frames[4:10])                        # the feet on one row
atlas, rects = sk.pack(frames, cell=(64, 64))
sk.write_anim_json("samples/<game>/sprites/hero.anim.json", "hero.png", {
    "idle": sk.clip([rects[0:4]], 0.3),
    "walk": sk.clip([rects[4:10]], 0.1, events={0: "step", 3: "step"})},
    pixels_per_unit=32, pivot=(0.5, 1), atlas=atlas)
```

| Function | Does |
| --- | --- |
| `pack(frames, cell, gutter, columns, power_of_two)` | Lays frames on a grid atlas, each in a cell of one size standing at its bottom centre; returns the atlas and a rect per frame |
| `clip(directions, durations, loop, events)` | One clip in the engine's form from rows of rects |
| `write_anim_json(path, atlas_name, clips, pixels_per_unit, pivot, normal, emissive, atlas=, normal_image=, emissive_image=)` | Writes the `.anim.json` (and the PNGs given as arrays), reads it back the way the engine does and raises on anything it would refuse |
| `harden_alpha(frame, threshold=128)` | Alpha becomes 0 or 255, cut where the engine cuts |
| `remove_specks(frame, min_pixels)` | Erases islands smaller than `min_pixels` that are detached from the figure |
| `fill_pinholes(frame, max_pixels)` | Fills enclosed transparent holes with the colour around them |
| `quantize(frames, palette_or_count)` | Maps every pixel to its nearest colour of a palette, or reduces the frames to their most used colours |
| `merge_near_duplicates(frames, distance)` | Folds colours that cannot be told apart into the more used of each pair |
| `clear_hidden_colour(frame)` | Blanks the colour under transparent pixels (a smaller PNG; the engine never reads it) |
| `align_baseline(frames, row, ignore)` | Moves each frame up or down so every figure stands on one row |
| `trim_and_centre(frames, size, margin)` | Cuts frames to what they use and puts them on cells of one size with a margin, keeping their positions relative to each other |
| `shift(frame, dx, dy)` | Moves a frame's content by whole pixels |
| `mirror_direction(frames, normal=False)` | A west row from an east row; with `normal=True` the red channel of a normal map is turned with it |
| `normal_from_height(height, strength, mask)` | A normal map in the engine's convention from a height field, flat outside the figure |
| `loop_seam(frames)` | `(seam, largest, ratio)`: how big the step from the last frame to the first is against the largest step inside the clip; the measure `motion.loop_seam` uses |
| `difference`, `baseline`, `centre_x`, `bounds`, `components`, `specks`, `pinholes`, `colours`, `near_duplicates` | The measurements the checks are built on, for a generator that wants to test itself |

The helpers cannot fix design: they will not make a walk read as a walk,
and `align_baseline` on a run flattens its flight phase. Kneeling frames
that float are fixed in the pose, not by sliding the frame.

## The checks

Every finding has an id (`group.name`), a severity, the clip, direction and
frames it is in, and a fix. **Errors** always fail. **Warnings** fail too,
unless the manifest waives them (or `--lenient` while iterating). `--info`
also prints observations that are not flaws (a hold, an effect's empty last
frame, colour under transparent pixels). A file the engine refuses is
reported as that and nothing else: fix it first.

### 1. Sheet integrity

| Id | Means | Fix |
| --- | --- | --- |
| `sheet.json` | The engine refuses the file: not JSON, no `atlas` or `clips`, a clip without 1, 4 or 8 direction rows, an empty row, a `rect` that is not four numbers or has no size, a duration of 0 or less, a wrong type. Never waivable | Write it with `write_anim_json` |
| `sheet.unknown_key` | A key the engine does not read (`loops`, `durations`, `events`...): it is ignored and the default applies | Fix the spelling |
| `sheet.json_types` | Fractional `pixels_per_unit` or `rect`, a pivot of three numbers: the engine cuts them | Whole numbers |
| `sheet.missing_image` | The atlas, normal or emissive image is missing, outside the folder or not decodable: the sprite is invisible, or silently unlit. Never waivable | Write the PNG next to the `.anim.json` |
| `sheet.image_size` | The normal or emissive image is not the atlas's size: the engine drops it | Same size, same layout |
| `sheet.frame_outside` | A frame's rectangle leaves the atlas | Fix the rect or the atlas size |
| `sheet.direction_frames`, `sheet.directions` | Directions of a clip with different frame counts; a direction count the profile does not expect | Draw every direction |
| `sheet.frame_size` | Frames of one clip differ in size: the pivot is a fraction, so the sprite shifts | One cell size (`pack`) |
| `sheet.empty_frame` | Error: every frame of a clip is empty (it draws nothing). Warning: an empty frame in the middle of a clip (a blink), or more than one at the end. One empty last frame on a clip that ends is how an effect vanishes (info) | Draw them or drop them |
| `sheet.frame_overlap` | Two rectangles that cut across each other without being the same | A cell per frame |
| `sheet.unused_cells` | Cells that hold pixels and belong to no clip | Add the forgotten clip, or clear them |

### 2. Pixel hygiene

| Id | Means | Fix |
| --- | --- | --- |
| `pixels.soft_alpha` | Half-transparent pixels on a cutout: the engine drops those under 0.5 and makes the rest solid | `harden_alpha`; no smooth scaling |
| `pixels.edge` | Content within the profile's `frame_margin` (1 px) of the frame's edge: cut off, and the engine's outline is missing there. On soft sprites: alpha 24 or more at the edge, a visible straight cut | A larger cell (`trim_and_centre`), or fade before the edge |
| `pixels.bleed` | Another frame's solid pixels right against this frame's edge in the atlas: an outline or distort effect draws them | Fix `pixels.edge` on the neighbour, or `pack(..., gutter=1)` |
| `pixels.specks` | Islands of 3 px or fewer detached from the figure | `remove_specks` |
| `pixels.pinholes` | One-pixel transparent holes enclosed by the figure: the background sparkles through | `fill_pinholes` |
| `pixels.colors` | More colours than the profile allows | `quantize` onto the palette |
| `pixels.near_duplicates` | Colours within a hair of another colour of the sheet (distance under 12 of 765) | `merge_near_duplicates`, or one palette table |
| `pixels.stray_colors` | Colours used by fewer than 8 pixels in a sheet of 8 frames or more | Repaint them from the palette |
| `pixels.hidden_color` (info) | Colour under fully transparent pixels: harmless here, since the engine samples sprites unfiltered | `clear_hidden_colour` if the PNG's size matters |

### 3. Motion

Measured per clip and direction on the visible pixels. The ground row of a
frame is its lowest row after leaving out the 5 lowest pixels, so the tip of
a scabbard or a cape below the feet is not taken for the ground.

| Id | Means | Fix |
| --- | --- | --- |
| `motion.baseline_jitter` | The feet move more than 2 px up and down within an `upright` or `low` clip | Keep the planted foot on one row (`align_baseline`) |
| `motion.baseline_pop` | A clip touches the ground more than 2 px above or below `idle` in the same direction: the figure jumps when the clip changes | `align_baseline(frames, row)`; pose kneeling figures on the ground |
| `motion.scale_pop` | An `upright` clip covers more than 1.35 times (or less than 1/1.35 of) the pixels idle does: drawn at another scale | One scale |
| `motion.drift` | The body's centre slides more than 3 px in an `in_place` clip | Centre on the pivot |
| `motion.pop` | In a clip of four frames or more, one frame sits 3 px or more away from both its neighbours while they agree: a misplaced frame | Check its cell and offset |
| `motion.loop_seam` | Going from a loop's last frame to its first is a change over 1.5 times the largest between two frames inside the clip | Close the cycle, or do not loop |
| `motion.static` | A clip of several frames in which nothing moves | One frame, or animate it |
| `motion.mirror` | West and east rows of an `upright` clip stand apart or differ in size | Draw both on one row at one scale (`mirror_direction`) |
| `motion.hold` (info) | Frames identical to the one before | A longer duration saves the cell |

### 4. Clips and timing

| Id | Means | Fix |
| --- | --- | --- |
| `timing.missing_clip` | A clip the game plays is not in the sheet: the engine shows `idle` instead, with idle's events and length | Add it; names are matched exactly |
| `timing.no_idle` | No `idle`: a missing clip, and a particle, then plays the first clip by name | Name the resting clip `idle` |
| `timing.loop` | `loop` is not what the game expects: a clip it waits on never ends, or a walk freezes on its last frame | Set `loop` (absent means true) |
| `timing.length`, `timing.frames` | The clip's length or frame count is outside the spec | Change the durations or the frames, or the spec |
| `timing.frame_duration` | A frame shorter than a tick (1/60 s), or held over the profile's limit in the middle of a clip | No frame under a tick; long holds go last |
| `timing.direction_length` | Directions of a clip last different times | The same durations in each |
| `timing.events_missing` | An event the game listens for is absent in a direction | Put it on its frame in every direction |
| `timing.event_time` | An event fires more than 0.035 s (two ticks) from the moment the spec gives | Move it, or fix the durations before it |
| `timing.event_unknown` | An event that is not in the profile's `event_names` | Fix the spelling |
| `timing.event_directions` | An event at different moments in different directions | The same frame in each |

### 5. Game readiness

| Id | Means |
| --- | --- |
| `ready.structure`, `ready.engine_load` | The `.anim.json` cannot be read at all; or, with `--engine`, the engine's own loader disagrees with what this kit read (clips, loop, directions, lengths through `fe.app.sprite_clips`; the size of each image through `fe.app.image_size`). Never waivable |
| `ready.atlas_size`, `ready.atlas_unused`, `ready.atlas_pot` | The atlas is over the profile's size; more than half of it (a tenth for an `image`) belongs to no frame; not a power of two on a profile that asks for one (none does: the engine does not need it for sprites) |
| `ready.pixels_per_unit` | Not the profile's pixels per unit: its pixels are another size on screen than the rest of the cast's |
| `ready.pivot`, `ready.pivot_feet` | The pivot is not where the profile puts it; on a grounded profile, the soles in `idle` are more than 2 px above the pivot row (it floats) or below it (it sinks) |
| `ready.world_height` | The figure's height in `idle`, in pixels over `pixels_per_unit`, is outside the profile's range in world units |
| `ready.normal_missing`, `ready.normal_flat` | No normal map where the profile requires one; one that is flat under the whole figure |
| `ready.normal_length`, `ready.normal_facing` | Normals under the figure that are not unit length, or point away from the viewer (blue under 128) |
| `ready.normal_coverage` | Relief where the atlas is transparent (over 2% of the figure's area): the map is laid out differently, or baked from other frames |
| `ready.normal_convention` | The figure's left side leans right or its top leans down across the sheet: red or green is flipped (a map from a tool that writes green down) |
| `ready.emissive_dark`, `ready.emissive_outside` | An emissive image that adds no light on the figure; one that glows outside it |
| `ready.naming` | File or clip names that are not lower case, digits and underscores |

### 6. Visual consistency

`review.py` writes the review sheets into the manifest's reviews folder
(`tools/<game>/sprite_reviews/`, not committed). A small sprite is one page;
a character is three (`<name>.png`, `<name>_2.png`, `<name>_3.png`):

- the **line-up**: the resting frame of every sprite of the same profile in
  the manifest, at one world scale, in colour and in black, this one framed;
- **game size**: the sprite at 1x and 2x beside a bar the height of a hero;
- the **silhouette** of each direction;
- the **palette** as swatches with each colour's share, near twins boxed;
- the **normal map** lit from the left, the right and above, and the map;
- the **emissive** image added to the dimmed figure;
- with `--engine`, **shots from the real renderer** on this kit's stage
  (`stage/`): a point light on the left, then on the right, a moving clip
  with the engine's outline on, and every direction at the game camera's
  distance beside the hero bar;
- every **clip as a filmstrip** per direction, enlarged without filtering,
  with the frame's box, the pivot, idle's ground row, each frame's duration
  and event, content against the edge in red, pinholes in magenta and
  specks in yellow; then an **onion skin** of the clip and, for a loop, the
  **seam** (the last frame beside the first, with its ratio);
- every finding of checks 1 to 5.

Open every page. Judge it against the game's art direction document,
answering each question; any "no" is a `fail`. For a figure:

1. **Silhouette**: recognisable in black at game size, and distinct from the rest of the cast?
2. **Proportions and palette**: the same head-to-body ratio, outline weight, number of tones and colour family as the others of its profile?
3. **Pixels**: clean clusters and one consistent outline; no orphan pixels, banding, smooth gradients or blur?
4. **Ground**: the feet on the ground row through every grounded clip, and the body on the pivot in clips on the spot?
5. **Motion**: the walk reads as a walk, and each clip reads as what its name says at a glance?
6. **Attack**: anticipation, one clear hit frame where the `hit` event is marked, and a recovery?
7. **Loops**: the last frame of each looping clip leads into the first, with no pop?
8. **Light**: the same shading direction in every frame and direction, and a normal map that lights the side the light is on?
9. **Spec**: the clips, frame counts, directions and colours the art direction asks for?

For a prop, an effect, a still image or a particle the sheet asks instead:
does it read at game size; does it stay inside its frames with no straight
cut or box when drawn in the engine; does a flipbook build, peak and die
away (or loop without a seam); are the colours the art direction's for that
thing; do its edges and level of detail match its family; does it meet the
spec.

Then record it:

```sh
python3 tools/spritekit/review.py --manifest tools/<game>/sprites.json sprites/hero.anim.json \
    --sign pass --by "who" --notes "what was looked at and what was judged, including any flaw accepted"
```

Rules for the reviewer, agent or person:

- You must have looked at the sheets for *these* files. The verdict is
  stored with the SHA-256 of the `.anim.json`, the atlas, the normal map and
  the emissive image together; a change to any of them voids it.
- The notes say what you saw, not that you looked. Name the flaws you
  accepted and why. "Looks good" is not a review.
- When in doubt, `fail` it, say why in the notes, and fix the generator. A
  failed review costs a rebuild; a wrong pass ships.
- The review cannot be waived.

## The manifest

One JSON file per game lists every sprite it ships. Aetherbound's is
`tools/aetherbound/sprites.json`.

```json
{
  "root": "../../samples/aetherbound",
  "art_direction": "tools/aetherbound/SPEC.md",
  "reviews": "sprite_reviews",
  "folders": ["sprites", "fx", "ui"],
  "profiles": { "character": { "directions": [4], "clips": { "idle": { "stance": "upright", "loop": true } } } },
  "sprites": {
    "sprites/hero.anim.json": {
      "profile": "character",
      "overrides": { "world_height": [1.7, 2.0] },
      "waive": {
        "pixels.edge": { "reason": "By design: the banner he carries is meant to leave the frame.", "max": 4 }
      },
      "review": { "verdict": "pass", "sha256": "...", "by": "...", "notes": "...", "date": "..." }
    }
  }
}
```

- `root`: where the sprite paths start, relative to the manifest.
- `folders`: searched for `.anim.json` files; one the manifest does not list
  fails the run, so nothing ships unvalidated (`"ignore"` takes globs for
  what really is not the game's).
- `profiles`: the game's own conventions and clip spec, over `profiles.json`.
- `overrides`: values for this one sprite, over its profile. `stage_sprite`
  there (`{"mode": "decal", "blend": "multiply"}`) tells the review's engine
  shots how the game draws a sprite that is not drawn the usual way.
- `waive`: check id to a reason, or to `{"reason", "max"}`. With `max` the
  waiver holds only while the count of that finding stays at or under it, so
  a known flaw cannot quietly grow. Write the reason for the next reader:
  either why it is right for this sprite ("By design: ..."), or what is
  wrong, where to fix it, and that it is debt ("Known debt: ..."). Remove
  the waiver when the flaw is fixed.
- `review`: written by `review.py --sign`.

Waive a flaw only when it is deliberate, or when fixing it is out of the
change at hand and it is recorded as debt with its fix. Never waive to get a
new sprite through: a new sprite starts with no waivers.

## Commands

```sh
# one sprite, while iterating
python3 tools/spritekit/validate.py out/hero.anim.json --profile character --engine build/fengine_headless

# everything a game ships (the gate)
python3 tools/spritekit/validate.py --manifest tools/<game>/sprites.json --engine build/fengine_headless

# options: --lenient (warnings pass), --info, --json report.json, --no-review (skip check 6 while iterating)

# the review sheets, then the verdict
python3 tools/spritekit/review.py --manifest tools/<game>/sprites.json sprites/hero.anim.json --engine build/fengine
python3 tools/spritekit/review.py --manifest tools/<game>/sprites.json --unreviewed --engine build/fengine
python3 tools/spritekit/review.py --manifest tools/<game>/sprites.json sprites/hero.anim.json --sign pass --by "..." --notes "..."

# prove the validator itself
python3 tools/spritekit/selftest.py [--engine build/fengine_headless]
```

## What this does not see

- The checks measure pixels, not drawing. Whether a walk reads as a walk, a
  face has a look, or a pose has weight is what the filmstrips and the
  reviewer are for.
- A still sheet is not motion. The filmstrip, the onion skin and the seam
  show where the frames are, not how the clip feels at speed: watch the
  sprite in the game, or step through a replay.
- `motion.loop_seam` compares blurred frames, which saturates: a loop that
  drops one frame of a fast cycle (a walk) is not told from one that closes,
  and an effect that grows from nothing and snaps back is caught only when
  it also moves. It finds loops that jump back a long way; the seam strip
  is for the rest.
- The ground row is the lowest substantial row of the figure. A kneeling
  figure whose knee, not its foot, is lowest is still held to that row;
  a pose that truly belongs higher gets `"stance": "air"` or a waiver.
- `ready.normal_convention` reads the sheet as a whole and assumes rounded
  forms. It finds a flipped channel, not a subtly wrong bake.
- The engine check loads the `.anim.json` and decodes its images headless.
  It does not draw: how a sprite looks lit, outlined and bloomed is in the
  engine shots, on a neutral stage, not in the game's scene.
- The colour checks know nothing of the game's palette unless the manifest
  lowers `max_colors`; whether the colours are the art direction's is judged
  in the review.
