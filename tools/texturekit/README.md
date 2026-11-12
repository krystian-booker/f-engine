# texturekit: game-ready textures

This folder is the process for every texture that goes into a game on this
engine, written for the AI agents (and people) who generate them: how to
make a texture, the helpers that write and repair one, and the gate it has to
pass.

A texture here is an image file the game draws (a sky card, a panorama, UI
art, a plain effect image), a colour grade (`.cube`), or an image embedded in
a model, addressed as `models/colossus.glb#colossus_albedo`. Sprite sheets
(the images an `.anim.json` names as `atlas`, `normal` or `emissive`) belong
to `tools/spritekit`, the geometry and UVs that carry a texture to
`tools/modelkit`.

A texture is **ready** when this exits 0:

```sh
python3 tools/texturekit/validate.py --manifest tools/<game>/textures.json --engine build/fengine_headless
```

That one command runs six checks on every texture the game ships:

| # | Check | What it answers |
| --- | --- | --- |
| 1 | File and encoding | Is the file what the engine reads, and no more? Format, bit depth, interlacing, colour-management and text chunks, bloat, duplicates |
| 2 | Alpha and edges | Will its edges be clean? The alpha against how it is drawn, colour hidden under transparency, premultiplied data, content at the border |
| 3 | Colour and pixel discipline | Will it hold up as pixel art under the game's light? Tonal range, palette size, clouds of near-duplicate colours, upscaled or resampled texels |
| 4 | Tiling and layout | Does it repeat without showing it? Seams, a blotch that becomes a lattice, single-texel shimmer, an atlas that is mostly empty |
| 5 | Game readiness | Does it fit the game? Size, power of two, memory, names, samplers, normal and emissive maps, nine-slices, whether anything uses it, and whether the engine loads it |
| 6 | Visual consistency | Does it look right? A review of its sheet against the game's art direction, signed off against the file's hash |

Checks 1 to 5 are measured. Check 6 is judged: `review.py` draws the texture,
someone looks, and the verdict is recorded. `validate.py` then refuses any
texture whose review is missing, failed, or older than the file.

## Setup

Python 3.11+ with `numpy` and `pillow` (`pip install numpy pillow`), nothing
else. The engine check needs a build: `build/fengine_headless`.

`python3 tools/texturekit/selftest.py` proves the validator: it makes one
flawed texture per check and expects each to be caught, clean ones to come
back clean, and every helper to fix what it claims (with a build present it
also proves the engine check).

## The process

Follow these steps in order. Do not start by painting detail.

1. **Read the brief.** The game's art direction (for Aetherbound,
   `tools/aetherbound/SPEC.md`): the look, the palette, the texel density
   (there: 16 texels per world unit on walls and floors, 32 on small props).
   Pick the texture's **profile** (below): it says how the game will sample
   it, and that decides almost every rule that follows.
2. **Fix the size first**, from where it will be seen: texels per unit times
   the units it covers, rounded to a power of two. A texture is not made
   sharper by being larger; it is made sharper by matching its neighbours.
3. **Write it as a generator script**, not by hand: a Python file under
   `tools/<game>/` that builds the image from nothing (numpy arrays, a seed),
   so it can be regenerated and fixed. Follow [the rules of the
   craft](#rules-of-the-craft). A tile is drawn on wrapped coordinates from
   the start; it is not made to tile afterwards.
4. **Values, then palette, then detail.** Block the light-to-dark structure
   in three or four tones, map them onto the palette's ramps
   (`tk.quantize`), then add the detail the texel grid can carry. Derive the
   normal map from a height field (`tk.normal_from_height`), paint the
   emissive map black first.
5. **Save it clean** with `tk.save_png` (8 bits, no ancillary chunks, no
   unused alpha, a palette when 256 colours do), after `tk.bleed_edges` if it
   has transparency and is filtered linearly. A texture that goes into a
   `.glb` is saved the same way *before* Blender loads it: the exporter keeps
   the file's bytes.
6. **Validate** while iterating:
   `python3 tools/texturekit/validate.py out/wall.png --profile tile --engine build/fengine_headless`
   (a model: `validate.py out/keep.glb` checks every texture in it). Every
   finding says where and how to fix it. Fix the generator, not the file.
7. **Review**: `python3 tools/texturekit/review.py --manifest ... <texture or group>`
   writes the review sheets. **Open the images and look at them** against
   the [review questions](#6-visual-consistency), a related group at a time.
   Then record the verdict with `--sign pass|fail --by ... --notes ...`.
8. **Register** the texture in the game's manifest with its profile, run the
   manifest validation, and commit the texture, its generator and the
   manifest together.

A texture that fails is not shipped with a waiver to make the run green.
Waivers are for flaws that are deliberate, or known and scheduled; see
[the manifest](#the-manifest). A new texture gets no waivers.

## What the engine expects

Every line here was read in the engine's source or tried against the built
engine; `docs/authoring.md` and `docs/architecture.md` ("Colour") are the
engine's own account.

| | |
| --- | --- |
| Decoding | `stb_image` (`engine/assets/Image.cpp`), always to 8-bit RGBA, at most 16384 texels a side. PNG of any kind (palette, greyscale, 1 to 16 bits, interlaced), JPEG (baseline and progressive), TGA, BMP, GIF, PSD, HDR and PNM decode; WebP, TIFF and DDS do not. The asset cache and catalog know only `.png`, `.jpg`, `.jpeg` and `.tga` as images. Ship PNG |
| What is ignored | Everything in a PNG but the pixels: gamma (`gAMA`), ICC profiles, text, time. A 16-bit file keeps its high bytes. So an image editor that honours a profile shows other colours than the game |
| On the GPU | RGBA8, four bytes a texel whatever the file stores (an opaque alpha channel costs nothing there); a third more with mips. `fengine_assetc cook` turns PNGs that a glTF references *by file*, and backdrops and panoramas, into BC7 `.ktx`; images embedded in a `.glb`, sprites and UI stay as they are |
| Model textures | Embedded in the `.glb` (PNG or JPEG) or a file next to it. Base colour and emissive are decoded from sRGB, normal maps are read as plain numbers. The glTF sampler decides the rest: `magFilter` NEAREST or linear; a mip-mapped `minFilter` (the Blender default; `NEAREST_MIPMAP_NEAREST` for pixel art) makes the engine build a mip chain; wrap is repeat, clamp or mirror. A texture with no sampler is filtered linearly, with mips, repeating |
| Mips | Built by the engine at load, a 2x2 box per level, colour averaged in linear light and weighted by alpha, so the colour under transparent texels does not darken mips. Magnification with linear filtering still reaches under the edge |
| Alpha on meshes | The material's `alphaMode`: OPAQUE ignores the texture's alpha, MASK cuts at the cutoff, BLEND blends straight (not premultiplied) alpha and is sorted. Emissive and normal maps: only RGB is read |
| Normal maps | Tangent space, unit vectors, red to the right, blue towards the viewer. **Green depends on the mesh.** With a `TANGENT` attribute in the file the engine follows glTF: green points up the image. Without one (every model of Aetherbound) the engine derives tangents from the UVs, and those put green *down* the image: a green-up map is then lit upside down. Verified by rendering one dome both ways (`engine/assets/Model.cpp`, `generateTangents`). Until that changes, a map for a mesh without tangents is authored green-down (`tk.flip_green`); profile key `derived_tangents_green` holds this fact in one place. Sprite normal maps are green-up (spritekit) |
| Emissive | `emissiveFactor` times `KHR_materials_emissive_strength` times the map's RGB, added unlit; above 1 it blooms |
| Canvas images | `fe.canvas.image`, `fe.canvas.nine_slice`, `fe.text.icon`: drawn as stored (no sRGB decode; the canvas composes in sRGB), straight alpha, clamped, no mips, point sampled unless the call passes `linear = true`. Any size |
| Sky | `environment.sky.backdrops[].image`: a card far away, decoded from sRGB, with mips, linear, clamped, straight alpha times a tint. `sky.mode = "panorama"`: sRGB, no mips, linear, repeating along U and clamped along V |
| Colour grades | `environment.post.lut`: a `.cube` 3D LUT. The reader (`readCube` in `engine/render/GpuAssets.cpp`) skips comments, `TITLE` and `DOMAIN_*` lines (a domain other than 0..1 is ignored, not applied), wants `LUT_3D_SIZE` from 2 to 64 and exactly size cubed rows, red changing fastest, and resamples every table to 16 cubed bytes. A file it cannot read is logged and the scene is graded with none |
| Particles, sprites | Emitters and weather draw sprite sheets (`.anim.json`), not loose images: spritekit |

## Profiles

`profiles.json` holds the conventions and budgets per kind of texture, and
every threshold the checks use (each with a comment). A game's manifest may
override a value or add a profile under `"profiles"`; one texture may
override values under `"overrides"`.

| Profile | For | Sampled | Repeats | Alpha | Size | Pixel art |
| --- | --- | --- | --- | --- | --- | --- |
| `tile` | wall, floor, rock: a tiling world texture | nearest, mips | both ways | none | power of two, 16 to 512 | 64 colours |
| `trim` | trim sheet: bands of mouldings, plating, banners | nearest, mips | along U | mask | power of two, 16 to 512 | 96 colours |
| `atlas` | one model's unique UVs: character, boss, prop, a floor medallion | nearest, mips | no | mask | power of two, 16 to 1024 | 256 colours |
| `overlay` | see-through layer on geometry: mist, foam, cobwebs | nearest, mips | both ways | blend | power of two, 16 to 512 | 64 colours |
| `normal_map`, `emissive` | such a map that stands alone as a file | nearest, mips | set `"tile"` | none | power of two, 16 to 1024 | emissive: yes |
| `effect` | a soft glow, shaft or puff that is not a sprite sheet | linear | no | blend, clear margin | 8 to 512 | no |
| `ui` | panels, cursors, icons, button glyphs on the canvas | linear (`"filter": "nearest"` for a pixel UI) | no | blend, clear margin | 8 to 1024 | no |
| `backdrop` | sky card or panorama | linear, mips | no (`"tile": "u"` for a panorama) | blend | 16 to 2048 | no |
| `lut` | a `.cube` colour grade | | | | up to 33 cubed (16 is all the engine keeps) | |
| `default` | anything else | not assumed | no | blend | 4 to 2048 | no |

What a texture is *used as* is not the profile's business. A texture a glTF
material samples is a base colour, a normal map or an emissive map because
the material says so, and its sampler and alpha mode are read from the model;
a loose file is recognised by its name (`_n`, `_normal`, `_e`, `_glow`,
`_emissive`) unless the profile fixes the role (`ui`, `backdrop`, `effect`
are always colour). So `maps/keep/geometry.glb#wall_n` takes the profile of
its colour map, `tile`, and is still checked as a normal map.

Overrides worth knowing: `"nine_slice": 24` (the border the game passes to
`fe.canvas.nine_slice`), `"cell": [64, 64]` (a sheet of glyphs),
`"tile": "u"`, `"lossless": false` (a photographic panorama may be JPEG),
`"alpha_data": "premultiplied"` (an image drawn with that blend).

## Rules of the craft

These are what a professional would do without being asked, and what the
checks then find nothing to say about.

**Size and density**
- One texel size across a scene. Choose the texture's size from the texels
  per unit of the brief and the area it covers; never enlarge a texture to
  "add detail" or shrink one to save a budget it is not near.
- Powers of two for everything on world geometry: mips halve cleanly and a
  repeat lands on whole texels. UI and sky cards need not be.
- Never store pixel art enlarged, and never resize it with smoothing or by a
  factor that is not a whole number. One texel in the file is one texel of
  the art.

**Pixel art**
- A small palette on purpose: ramps of 3 to 5 tones per material, taken from
  the art direction's colours, hue shifting along the ramp (shadows cooler,
  lights warmer) rather than one colour made darker.
- Hard edges. No anti-aliasing against transparency, no soft brush, no blur,
  no gradient that is not a dither. After any filter, occlusion bake or
  blend, quantize back to the palette (`tk.quantize`).
- Values first. Squint, or look at the sheet's "from far away" tile: the big
  shapes must read in three tones. The scene supplies light and shadow; the
  texture must not arrive already dark, already bright or already lit from
  one side.
- The smallest feature is two texels wide where it matters. A 1 px checker
  or hairline is dither at 1:1 and flicker at any distance.

**Tiling**
- Draw on wrapped coordinates (`x % width`), with noise that is periodic in
  the tile, so the wrap is like any other place. Cross-fading a seam away
  afterwards (`tk.make_tileable`) costs sharpness or cuts shapes.
- Even it out: no part of a tile much lighter, darker or more saturated than
  the rest. One distinctive stone is a lattice of that stone on a wall.
  Variation belongs in many small differences, and in the scene (decals,
  props, light).
- A trim sheet repeats along U only: keep each band's features on a period
  that divides the width.

**Alpha**
- Decide the kind and say it in the material: none, a cutout (0 or 255 only,
  alpha mode MASK) or soft (BLEND, sorted and never batched). Pixel art has
  no half-transparent edge.
- The engine blends straight alpha. Do not premultiply (unless the image is
  drawn with `blend = "premultiplied"` and the profile says so).
- Under every transparent texel there is still a colour. If the image is
  filtered linearly (UI with `linear = true`, sky cards, effects), carry the
  edge colour outwards (`tk.bleed_edges`); black there is a dark halo.
- Leave a clear margin of a texel around icons, effects and each cell of a
  sheet, so nothing is cut at a quad's edge or bleeds into its neighbour.

**Normal maps**
- Derive them from a height field (`tk.normal_from_height`), at the size of
  the colour map, so relief and painted pixels line up. Never paint on one,
  scale one with a picture filter, or save one through colour management;
  `tk.renormalize` after any arithmetic.
- Red right, blue out, unit length, flat is (128, 128, 255). Green: up the
  image for a mesh exported with tangents, down for one without, as "What
  the engine expects" explains. Look at the sheet's lit views: it lights the
  map the way the engine will.
- Relief that carries: joints and bevels two texels wide, not noise.

**Emissive maps**
- Black first; then only what is lit from within. Colours from the palette's
  light sources; the strength (HDR) belongs to the material, not the map.
- A glow's falloff may be smooth here: bloom finishes it. Its *shape* is
  still pixel art.

**UI**
- A nine-slice stretches its edges along their length and its middle both
  ways: keep those parts constant along the stretch (a gradient is fine) and
  put detail in the corners, inside the border the game passes.
- Draw at the canvas's resolution; a sheet of glyphs is a grid of equal
  cells, each glyph centred with a margin.

**Sky and grades**
- A sky card is a silhouette and a few values, far darker than it feels
  right in an editor: fog, tint and bloom are added to it.
- A grade (`.cube`) moves mood, not exposure: keep black black and white
  white, keep the grey ramp rising, write it at 16 or 17 cubed.

**Files**
- PNG, 8 bits, written by `tk.save_png`. No JPEG for anything with hard
  edges, alpha or numbers in it (normal maps).
- Lower case names with underscores that say what the map is: `wall`,
  `wall_n`, `wall_e`.
- One copy of each image in the game folder. Working files live outside it.

## The helpers

`texturekit/tools.py`, used inside a generator (numpy and pillow only):

```python
import sys; sys.path.insert(0, "tools/texturekit")
from texturekit import tools as tk

pixels = tk.quantize(painted, tk.load_palette("tools/<game>/palette.hex"))   # or a number of colours
print(tk.seam(pixels))                                                       # how the wrap compares with the interior
normal = tk.normal_from_height(height, strength=2.0)                         # green up; tk.flip_green for a mesh without tangents
tk.save_png("samples/<game>/maps/keep/textures/wall.png", pixels)
tk.save_png("samples/<game>/ui/cursor.png", tk.bleed_edges(cursor))
```

| Function | Does |
| --- | --- |
| `save_png(path, pixels)`, `encode_png(pixels)` | The smallest plain PNG of exactly these pixels: 8 bits, no ancillary chunks, not interlaced, alpha dropped when unused, a palette when 256 colours are enough |
| `bleed_edges(rgba, distance=2)` | Carries the colour of visible texels into the fully transparent ones beside them; alpha and everything visible stay as they were. Fixes `alpha.fringe` |
| `premultiply(rgba)`, `unpremultiply(rgba)` | Between straight and premultiplied alpha. Fixes `alpha.premultiplied` |
| `seam(pixels)` | Measures the wrap against the interior, per axis; what `tiling.seam` uses |
| `make_tileable(pixels, width=0.25, hard=False, axes="both")` | Cross-fades the image with itself shifted by half. It softens the faded band and, on pixel art, makes new in-between colours; `hard=True` switches texel by texel with a dither instead (palette kept, shapes may be cut). A repair, not a method |
| `mirror_tile(pixels)` | The image and its mirror images, twice the size: always seamless, never a new colour, but the symmetry shows on anything with a direction |
| `quantize(pixels, palette_or_n, dither=False)` | Every texel to its nearest palette colour (or to the image's best `n`); `dither=True` adds an ordered dither between the two nearest. Fixes `colour.count`, `colour.near_duplicates`, `colour.palette` |
| `palette_of(pixels, limit=None)`, `load_palette(source)` | The image's colours by share; a palette from a `.hex`, `.gpl` or `.json` file, a list, or an image |
| `normal_from_height(height, strength=1.0, wrap=True)` | A unit-length, green-up normal map from a height field; `wrap` keeps a tileable height tileable |
| `flip_green(pixels)`, `renormalize(pixels)` | Between green-up and green-down; unit length again after painting or blending |
| `pad_pow2(pixels, mode)` | Pads an atlas to powers of two and returns the factor to scale its UVs by. Not for tiles |
| `halve(pixels, srgb=True)`, `mip_chain(pixels, srgb=True)` | The engine's own mip levels (2x2 box, linear light, alpha-weighted), to look at what the texture is at a distance |

`fix.py` applies the repairs that change no visible pixel to files that
already exist: strips chunks, 16 to 8 bits, removes interlacing, drops an
unused alpha channel, recompresses, and with `--bleed` fills the colour
under alpha 0. It says what it would do and writes only with `--write`:

```sh
python3 tools/texturekit/fix.py samples/<game>/ui/*.png --bleed            # what would change
python3 tools/texturekit/fix.py samples/<game>/ui/*.png --bleed --write
```

It does not touch textures inside a `.glb` (fix those where the generator
writes the image), and it cannot fix what needs a decision: a seam, a
palette, a size, a normal map's convention. A rewritten file needs its
review signed again.

## The checks

Every finding has an id (`group.name`), a severity, a count where there is
something to count (texels, colours, bytes), where it is, and a fix.
**Errors** always fail. **Warnings** fail too, unless the manifest waives
them (or `--lenient` while iterating). `--info` also prints observations
that are not flaws (an alpha channel that is opaque everywhere, a twin in
another model, a name that only code builds). Levels are 8-bit (0 to 255);
the numbers below are the defaults of `profiles.json`.

### 1. File and encoding

| Id | Means | Fix |
| --- | --- | --- |
| `file.format` | An image the engine cannot decode (WebP, TIFF...: an error); or one it decodes but does not know as an image by its extension; or a file whose extension is not what it is | Save it as PNG |
| `file.lossy` | A JPEG where the profile needs exact pixels (pixel art, anything with alpha, any normal map) | Save the source as PNG |
| `file.colour_mode` | A CMYK JPEG: converted without colour management | Convert to RGB |
| `file.bit_depth` | 16 bits per channel: the engine keeps the high byte | `tk.save_png`, `fix.py` |
| `file.interlaced` | An Adam7 PNG: larger, slower, for nothing | `tk.save_png`, `fix.py` |
| `file.colour_profile` | An ICC profile, or a gamma chunk that is not 1/2.2: other tools show other colours than the engine | Convert to sRGB, save without (`fix.py` strips, it does not convert) |
| `file.metadata` | Over 64 bytes of text, time stamps, EXIF and the like | `tk.save_png`, `fix.py` |
| `file.bloat` | Over 1.6 times (and 8 KB more than) the smallest plain PNG of the same pixels | `tk.save_png` before the exporter, `fix.py` |
| `file.duplicate` | Pixel for pixel another texture of the manifest: a loose file twice, a file and its embedded twin, or twice in one model (a twin in another model is only noted) | Ship one copy |
| `file.cube` | A `.cube` the engine's reader refuses: wrong row count, a 1D LUT, a size outside 2 to 64 (an error: the scene is graded with none) | Write `LUT_3D_SIZE n` and n cubed rows |
| `file.cube_domain` | `DOMAIN_MIN/MAX` other than 0..1, which the engine ignores | Bake the table for 0..1 |

### 2. Alpha and edges

| Id | Means | Fix |
| --- | --- | --- |
| `alpha.empty` | Every texel fully transparent (an error) | Write the alpha that was meant |
| `alpha.kind` | More transparency than what draws it allows: a cutout or soft alpha in an OPAQUE material (ignored), soft alpha under MASK (cut ragged), MASK with nothing cut out, alpha in a normal or emissive map, or more than the profile expects where no material says | Fix the material's alpha mode, or the alpha |
| `alpha.fringe` | Where the texture is magnified linearly: more than 8 fully transparent texels that touch a visible one and hold a colour over 32 levels from it. Filtering mixes that colour into the edge: a dark or bright halo. Not raised for nearest-sampled textures, where it cannot show | `tk.bleed_edges`, `fix.py --bleed` |
| `alpha.premultiplied` | Colour that fades to black exactly as alpha does, where the engine blends straight alpha (grey edges); or the reverse where the profile says premultiplied | `tk.unpremultiply` / `tk.premultiply` |
| `alpha.border` | Visible texels in the margin the profile keeps clear (per cell, with `cell`): the image is cut at its edge or runs into its neighbour | Leave a transparent margin |

### 3. Colour and pixel discipline

Not run on normal maps. The palette checks are for colour maps in pixel-art
profiles; an emissive map may hold a glow's falloff.

| Id | Means | Fix |
| --- | --- | --- |
| `colour.flat` | One colour over the whole image | A material factor, or a 1x1 |
| `colour.crushed`, `colour.blown`, `colour.contrast` | Nothing brighter than 32; nothing darker than 232; under 6 levels between the 5th and 95th percentile. Skipped for soft-alpha images, whose values are in their alpha | Give it a value structure in the middle of the range |
| `colour.count` | More colours than the profile's pixel art keeps | `tk.quantize` |
| `colour.near_duplicates` | More than a fifth of the colours (and more than 8) lie within 3 levels of a more used one: what blur, resampling, anti-aliasing and occlusion bakes leave | `tk.quantize` after any filter |
| `colour.upscaled` | Every texel is an N x N block | Store it at 1x |
| `colour.mixels` | Texels of mixed sizes: blocks on no common grid, or a quarter of the columns and rows being exact copies of their neighbour in pairs (a resample by a factor that is not whole) | Go back to the 1x original |
| `colour.palette` | With `"palette"` in the manifest: colours more than 2 levels from every palette colour | `tk.quantize(pixels, palette)` |
| `colour.lut_identity`, `colour.lut_clipped`, `colour.lut_tone` | A grade that does nothing; one with over 1% of its entries outside 0..1; a grey ramp that turns back, or that keeps under half the range | Remove it; scale it into range; keep the tone curve rising |

### 4. Tiling and layout

| Id | Means | Fix |
| --- | --- | --- |
| `tiling.seam` | On an axis the profile repeats: the step across the wrap is over 1.5 times the largest step between neighbouring columns (rows) inside, texel to texel, or between eighths of the image (a drift in brightness). Measured against the texture itself, so a busy texture whose wrap looks like its own joints passes and a calm one with a faint line does not | Generate on wrapped coordinates; `tk.make_tileable` as a repair |
| `tiling.repeat` | One sixteenth of a tile (one strip of a trim sheet) stands more than 14 levels from the typical one: a blotch, band or gradient that becomes a lattice | Even out the large shapes |
| `tiling.shimmer` | Over three quarters of the contrast is single-texel detail that the first mip erases | Two-texel features, calmer dither |
| `tiling.empty` | Over 40% of an atlas (and over 16,384 texels) is one flat colour | Pack it smaller, or use the space |

### 5. Game readiness

| Id | Means |
| --- | --- |
| `ready.structure`, `ready.engine_load` | Not a readable image, or not an image of that model; or the engine itself does not load it (with its message). Never waivable |
| `ready.max_size`, `ready.memory` | Over the profile's size, or its GPU budget (errors) |
| `ready.min_size`, `ready.pot`, `ready.aspect`, `ready.cell` | Smaller than the profile expects; not a power of two where the profile needs one; a strip over 8 to 1 (4 to 1 for tiles); a sheet that is not a whole number of cells |
| `ready.name` | Not lower case with underscores; an embedded image with no name; a normal or emissive map whose name does not say so, or a colour map named like one |
| `ready.sampler` | The glTF sampler against the profile: pixel art magnified linearly (or no sampler at all, which means linear), smooth art point-sampled, a world texture without mips, a tiling texture that clamps |
| `ready.unused` | An embedded image no drawn material samples; a file no map, data file, Lua script or model names (a name Lua builds from `"folder/" .. name .. ".png"` counts as named, and is noted) |
| `ready.normal_flat`, `ready.normal_encoding`, `ready.normal_length`, `ready.normal_blue` | A normal map that is flat everywhere; whose red and green do not centre on 128 (saved through a gamma or colour conversion, or channels swapped); with over 2% of its vectors off unit length (painted on, scaled, blended); with vectors pointing into the surface |
| `ready.normal_green` | A map whose green points the other way from how the engine will read it on the mesh that uses it (see "What the engine expects"). The direction is read from the map itself: only one reading of green makes its slopes fit together into a surface. Relief of straight horizontal and vertical bevels alone cannot tell, and then nothing is said |
| `ready.map_size` | A normal or emissive map that is not the size of the colour map of its material |
| `ready.emissive_black`, `ready.emissive_coverage` | An emissive map with nothing lit; or with more than half of it lit |
| `ready.nine_slice` | With `"nine_slice"`: detail across the direction the panel stretches (in its edges or middle); or borders that leave no middle (an error) |
| `ready.lut_size` | A table over 33 cubed: the engine resamples to 16 cubed |

`ready.engine_load` decodes a file with `fe.app.image_size` and compares the
size, and loads a model (which decodes its embedded images) with
`fe.app.model_animations`. No Lua binding reads a `.cube` without a
renderer, so a LUT is not loaded by the engine here: `file.cube` is this
kit's own copy of the reader's rules.

### 6. Visual consistency

`review.py` writes one compact image per texture, the review sheet:

- the **whole** texture and a **detail** enlarged with nearest filtering
  (the busiest part of it; for a cutout, its edges);
- for a texture with alpha, that detail **on black and on white, filtered
  as the game will** (halos and cut edges show here); for a normal map, the
  map **lit from two sides the way the engine will read it**, and its detail
  lit; otherwise the texture **tiled 3x3** (a trim sheet: rolled so the wrap
  runs down the middle) and **from far away** (its 32 px mip enlarged);
- its **channels**, its **mips** as the engine builds them, its **values**
  (luminance), and its **palette** by share with the art direction's
  colours beside it (`"swatches"` in the manifest) and a histogram;
- how materials sample it, and every finding of checks 1 to 5.

A `.cube` is shown by what it does: a hue chart and a grey ramp before and
after, the tone curves, and the art direction's colours through the grade.

Naming several textures (a folder, a model, a glob) also writes a **group
sheet**: the set side by side at one scale, each with its palette strip and
its state.

Open the images. Judge each texture against the game's art direction
document, answering each question; any "no" is a `fail`, unless it is
exactly a flaw the manifest already waives as known debt, which the notes
then name:

1. **Scale**: at the size the game shows it (1:1 and the small mips), does it still read as what it is?
2. **Seams and repeat**: tiled 3x3, is there a line where it wraps, or one blotch that turns into a lattice?
3. **Palette and value**: do its colours and its light-to-dark range sit in the art direction's palette, neither crushed nor washed out?
4. **Pixel discipline**: hard edges, a small deliberate palette, no blur, no stray anti-aliasing, no stretched or half-size texels; the same texel size as the textures next to it?
5. **Edges**: on black and on white, is there a halo or a cut-off edge? Does a normal map light as bumps, not dents, from both sides?
6. **Authorship**: does everything in it look meant (no noise that reads as static, no generated-by-accident pattern), and made by the same hand as its neighbours?

For a grade the questions are its own (the sheet prints them): the mood the
art direction asks for, a smooth rising grey ramp, colours that survive,
neutrals tinted on purpose.

Then record it, for one texture or for the group that was judged together:

```sh
python3 tools/texturekit/review.py --manifest tools/<game>/textures.json maps/keep/geometry.glb \
    --sign pass --by "who" --notes "what was looked at and what was judged, texture by texture, including any flaw accepted"
```

**Reviewing many textures well.** A game has dozens; reviewing each alone
invites a rubber stamp, and reviewing them together is how an art lead
actually works, because most of what can be wrong is a difference *between*
textures.

- Review a related set at a time: one model's textures, one map's, one UI
  folder (a dozen at most). Start with the group sheet: one palette, one
  texel size, one hand? Then open each sheet; with the questions in mind a
  sheet takes a minute.
- A colour map, its normal map and its emissive map are one decision: look
  at them in that order.
- One `--sign` may cover the set, and its notes then name each texture or
  kind and what was judged in it. A texture that fails gets its own
  `--sign fail` with its own notes: sign the others by name.
- A texture inside a model is bound to the model's hash, so exporting the
  model again voids the reviews of all its textures. Review the group
  again; if nothing in the textures changed, the group sheet shows it.

Rules for the reviewer, agent or person:

- You must have looked at the sheet for *this* file. The verdict is stored
  with the file's SHA-256 (for an embedded texture: the model's); any change
  voids it.
- The notes say what you saw, not that you looked. Name the flaws you
  accepted and why. "Looks good" is not a review.
- When in doubt, `fail` it, say why in the notes, and fix the generator. A
  failed review costs a regeneration; a wrong pass ships.
- The review cannot be waived.

## The manifest

One JSON file per game lists every texture it ships. Aetherbound's is
`tools/aetherbound/textures.json`.

```json
{
  "root": "../../samples/aetherbound",
  "art_direction": "tools/aetherbound/SPEC.md",
  "reviews": "texture_reviews",
  "folders": ["."],
  "ignore": ["replays/*"],
  "swatches": { "aetherite": ["#3a1a66", "#9b5cff", "#e2ccff"] },
  "profiles": { "tile": { "max_colours": 48 } },
  "textures": {
    "maps/keep/geometry.glb#wall": { "profile": "tile" },
    "ui/panel.png": {
      "profile": "ui",
      "overrides": { "nine_slice": 24 },
      "waive": {
        "alpha.fringe": { "reason": "Known debt from before texturekit: ... Fix in tools/<game>/ui/generate.py: ...", "max": 12 }
      },
      "review": { "verdict": "pass", "sha256": "...", "by": "...", "notes": "...", "date": "..." }
    }
  }
}
```

- `root`: where the texture paths start, relative to the manifest.
- `folders`, `ignore`: where the game's files are searched, and globs to
  leave out (reference screenshots). Every image file and `.cube` there that
  no `.anim.json` names, and every image embedded in a model there, must be
  listed: one that is not fails the run, so nothing ships unvalidated.
- A key is a file, or `model.glb#image` for an embedded texture (the image's
  glTF name; `name.index` where two share one).
- `profiles`, `overrides`: the game's own conventions over `profiles.json`,
  and one texture's own.
- `swatches`: the art direction's colours by name, drawn on every sheet.
  `palette`: a palette file (relative to the manifest) that pixel-art
  textures must keep to; leave it out when the art direction gives ranges
  rather than a fixed palette, as Aetherbound's does.
- `waive`: check id to a reason, or to `{"reason", "max"}`. With `max` the
  waiver holds only while the count of that finding stays at or under it, so
  a known flaw cannot quietly grow. Write the reason for the next reader:
  either why it is right for this texture ("By design: ..."), or what is
  wrong, where to fix it, and that it is debt ("Known debt ..."). Remove the
  waiver when the flaw is fixed.
- `review`: written by `review.py --sign`.

Waive a flaw only when it is deliberate, or when fixing it is out of the
change at hand and it is recorded as debt with its fix. Never waive to get a
new texture through: a new texture starts with no waivers.

## Commands

```sh
# one texture, while iterating (a model: every texture in it)
python3 tools/texturekit/validate.py out/wall.png --profile tile --engine build/fengine_headless
python3 tools/texturekit/validate.py out/keep.glb --profile tile
python3 tools/texturekit/validate.py "out/keep.glb#wall_n" --profile tile

# everything a game ships (the gate)
python3 tools/texturekit/validate.py --manifest tools/<game>/textures.json --engine build/fengine_headless

# part of it: a folder, a model, a glob of the manifest's keys
python3 tools/texturekit/validate.py --manifest tools/<game>/textures.json ui "maps/keep/sky_*" maps/keep/geometry.glb

# options: --lenient (warnings pass), --info, --json report.json, --no-review (skip check 6 while iterating)

# the review sheets (and the group sheet), then the verdict
python3 tools/texturekit/review.py --manifest tools/<game>/textures.json maps/keep/geometry.glb
python3 tools/texturekit/review.py --manifest tools/<game>/textures.json --unreviewed
python3 tools/texturekit/review.py --manifest tools/<game>/textures.json maps/keep/geometry.glb --sign pass --by "..." --notes "..."

# lossless repairs to files
python3 tools/texturekit/fix.py samples/<game>/ui/*.png [--bleed] [--write]

# prove the validator itself
python3 tools/texturekit/selftest.py
```

## What this does not see

- The checks read the image. They cannot know intent: a dark texture may be
  a night sky. That is what profiles, the review sheet and the reviewer are
  for.
- How a texture sits on its model: texel density, stretch, island padding
  and overlaps are modelkit's (`uv.*`). Here an atlas is only what the image
  alone shows.
- The scene. The sheet shows a texture flat and unlit; fog, lights, bloom
  and the grade change everything, and they hide some flaws (a repeat) and
  reveal others. Judge a texture in an engine shot before calling its scene
  done (`tools/aetherbound/shot.py` renders any map).
- Seams that look like the texture's own joints: a brick wall cut at a
  course line passes `tiling.seam` by design, even if bricks end up half
  length. The 3x3 tile on the sheet is where that shows.
- Bumps against dents. `ready.normal_green` knows which way green points
  relative to red, not whether the height was inverted; the lit views on the
  sheet are for that.
- A resample that left no exact copies (smoothing, then quantizing), or
  JPEG damage re-saved as PNG, is caught only as far as
  `colour.near_duplicates` catches it.
- Premultiplied data in a dark image looks like straight alpha; the check
  says nothing rather than guess.
- `ready.unused` is a search of the game's text. A file whose name code
  assembles in some other way than `"folder/" .. name .. ".png"` is reported
  unused: waive it with the line of code that builds the name.
- Colour grades are read with this kit's copy of the engine's parser, not by
  the engine; cooked `.ktx` files are not looked at; sprite sheets are
  spritekit's. A Radiance `.hdr` decodes in the engine but not in Pillow, so
  the kit calls it unreadable (the engine would flatten it to 8 bits anyway).
