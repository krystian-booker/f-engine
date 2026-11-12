"""The automated checks: the file, its alpha, its colour, how it tiles and
whether it fits the game. Each returns Findings; kitcommon decides what they
mean for the verdict.

Everything is measured on the pixels the engine's decoder produces (8-bit
RGBA), in 8-bit levels (0 to 255), and judged by how the engine will sample
the texture: the profile says it (nearest or linear, tiling or not, mips),
and for a texture a glTF material uses, the material and its sampler do.
Thresholds are the profile's; the comments in profiles.json say what each is.
"""
import os
import re

import numpy as np

from kitcommon.kit import ERROR, INFO, WARN, Finding  # noqa: F401  (the kits share one Finding)

from . import image as ti
from . import tools

GROUPS = [
    ("file", "File and encoding"),
    ("alpha", "Alpha and edges"),
    ("colour", "Colour and pixel discipline"),
    ("tiling", "Tiling and layout"),
    ("ready", "Game readiness"),
]

LUMA = np.array([0.2126, 0.7152, 0.0722])
ALPHA_ORDER = {"none": 0, "mask": 1, "blend": 2}
NEAREST, LINEAR, REPEAT, CLAMP = 9728, 9729, 10497, 33071
FILTERS = {9728: "nearest", 9729: "linear", 9984: "nearest, nearest mip", 9985: "linear, nearest mip", 9986: "nearest, linear mips",
           9987: "linear, linear mips"}
# PNG chunks that are part of the picture; everything else is colour management or metadata.
PNG_PICTURE = {"IHDR", "PLTE", "IDAT", "IEND", "tRNS"}


def _percent(part, whole):
    return 100.0 * part / whole if whole else 0.0


def _kb(count):
    return f"{count / 1024:.0f} KB" if count >= 10240 else f"{count / 1024:.1f} KB" if count >= 1024 else f"{count} bytes"


def _hex(colour):
    return "#%02x%02x%02x" % tuple(int(v) for v in colour[:3])


def alpha_kind(texture):
    """'none' (opaque everywhere), 'mask' (only 0 and 255) or 'blend'."""
    alpha = texture.alpha
    if alpha.min() == 255:
        return "none"
    return "mask" if ((alpha == 0) | (alpha == 255)).all() else "blend"


def sampler_of(use):
    """What a glTF sampler means to the engine (readSampler in engine/assets/Model.cpp):
    {"nearest": magnified by point sampling, "mips": a mip chain is built and used,
     "clamp_u", "clamp_v", "text"}."""
    sampler = use["sampler"]
    minify = sampler.get("minFilter")
    return {"nearest": sampler.get("magFilter") == NEAREST, "mips": minify not in (NEAREST, LINEAR),
            "clamp_u": sampler.get("wrapS", REPEAT) == CLAMP, "clamp_v": sampler.get("wrapT", REPEAT) == CLAMP,
            "text": ("nearest" if sampler.get("magFilter") == NEAREST else "linear") + " / " +
                    FILTERS.get(minify, "linear, linear mips") + (", clamped" if CLAMP in (sampler.get("wrapS"), sampler.get("wrapT")) else "")}


def sampled_linear(texture, profile):
    """Whether the game magnifies this texture with linear filtering (then the colour under
    transparent texels and the neighbours of every edge matter)."""
    if texture.uses:
        return any(not sampler_of(use)["nearest"] for use in texture.uses)
    return profile.get("filter", "any") != "nearest"


def has_mips(texture, profile):
    if texture.uses:
        return any(sampler_of(use)["mips"] for use in texture.uses)
    return bool(profile.get("mips"))


def colour_table(texture):
    """(colours (n, 3), pixel counts) of the visible texels, most used first."""
    visible = texture.rgba[texture.alpha > 0][:, :3].astype(np.uint32)
    if not len(visible):
        return np.zeros((0, 3), dtype=np.int32), np.zeros(0, dtype=np.int64)
    packed = (visible[:, 0] << 16) | (visible[:, 1] << 8) | visible[:, 2]
    unique, counts = np.unique(packed, return_counts=True)
    order = np.argsort(-counts, kind="stable")
    unique, counts = unique[order], counts[order]
    return np.stack([(unique >> 16) & 255, (unique >> 8) & 255, unique & 255], axis=1).astype(np.int32), counts


# ------------------------------------------------------------------ 1: the file

def check_file(texture, profile, context):
    out = []
    if texture.is_lut:
        return _check_cube(texture, profile, out)
    size = len(texture.data)
    extension = os.path.splitext(texture.container)[1].lower()
    if texture.format not in ti.ENGINE_DECODES:
        out.append(Finding("file.format", ERROR, f"a {texture.format} image, which the engine's decoder (stb_image) cannot read", None, None,
                           "Save it as PNG (JPEG only for photographic backdrops)."))
    elif texture.embedded:
        if texture.format not in ("PNG", "JPEG"):
            out.append(Finding("file.format", WARN, f"a {texture.format} image embedded in a glTF, which allows PNG and JPEG only", None, None,
                               "Embed it as PNG."))
    elif extension not in ti.ENGINE_EXTENSIONS:
        out.append(Finding("file.format", WARN, f"a '{extension}' file: it decodes, but the engine's asset cache and catalog know only "
                           ".png, .jpg, .jpeg and .tga as images (it is neither preloaded nor listed)", None, None, "Save it as PNG."))
    elif extension not in ti.FORMAT_EXTENSIONS.get(texture.format, (extension,)):
        out.append(Finding("file.format", WARN, f"a {texture.format} image named '{extension}' (the engine decodes by content, other tools by name)",
                           None, None, "Give it the extension of what it is, or save it as what its name says."))
    if texture.format == "JPEG" and (profile.get("lossless", True) or texture.role == ti.NORMAL):
        what = "a normal map" if texture.role == ti.NORMAL else f"the '{profile['name']}' profile"
        out.append(Finding("file.lossy", WARN, f"a JPEG, for {what}: block noise around every hard edge, colours that drift, no alpha", None, None,
                           "Save the source as PNG; never re-save a JPEG as PNG and call it clean."))
    if texture.mode == "CMYK":
        out.append(Finding("file.colour_mode", WARN, "a CMYK JPEG: the engine converts it without colour management, so its colours are not "
                           "the ones an image editor shows", None, None, "Convert to RGB before saving."))
    if texture.bit_depth > 8:
        out.append(Finding("file.bit_depth", WARN, f"{texture.bit_depth} bits per channel: the engine keeps the high byte of each sample and "
                           "the rest is file size", None, None, "Save 8 bits per channel (tools.save_png, or fix.py)."))
    if texture.interlaced:
        out.append(Finding("file.interlaced", WARN, "an interlaced (Adam7) PNG: a larger file and a slower decode, for a progressive display "
                           "nothing here uses", None, None, "Save without interlacing (tools.save_png, or fix.py)."))
    if "iCCP" in texture.chunks or "ICC" in texture.chunks:
        out.append(Finding("file.colour_profile", WARN, "an embedded ICC colour profile: editors and browsers convert the image through it, "
                           "the engine ignores it, so what was painted is not what the game shows",
                           texture.chunks.get("iCCP", texture.chunks.get("ICC")), None,
                           "Convert to sRGB and save without a profile (tools.save_png writes none; fix.py strips it without converting)."))
    elif texture.gamma is not None and abs(texture.gamma - 2.2) > 0.05:
        out.append(Finding("file.colour_profile", WARN, f"a gAMA chunk of {texture.gamma:.2f}: viewers that honour it show other tones than the "
                           "engine, which ignores it", None, None, "Save without a gamma chunk (tools.save_png, or fix.py)."))
    if texture.format == "PNG":
        extra = {kind: count for kind, count in texture.chunks.items() if kind not in PNG_PICTURE and kind != "iCCP"}
    else:
        extra = {kind: count for kind, count in texture.chunks.items() if kind in ("EXIF", "COM")}
    total = sum(count + (12 if texture.format == "PNG" else 0) for count in extra.values())
    if total > profile.get("max_metadata_bytes", 64):
        out.append(Finding("file.metadata", WARN, f"bytes of metadata the engine never reads ({', '.join(sorted(extra))}): text, time stamps, "
                           "EXIF and the like", total, None, "Save without metadata (tools.save_png, or fix.py)."))
    if texture.format == "PNG" and texture.rgba is not None and size > profile.get("bloat_min_bytes", 8192):
        best = len(tools.encode_png(texture.rgba))
        if size > profile.get("bloat_ratio", 1.6) * best and size - best > profile.get("bloat_min_bytes", 8192):
            colours = len(colour_table(texture)[1])
            how = "as a palette PNG" if colours <= 256 else "at full compression"
            out.append(Finding("file.bloat", WARN, f"bytes more than the pixels need: {_kb(size)} where {_kb(best)} holds exactly the same image "
                               f"{how}", size - best, None, "Write it with tools.save_png (generators), or run fix.py on it."))
    for other, relation, severity in (context.copies(texture) if context else []):
        out.append(Finding("file.duplicate", severity, f"pixel for pixel the same image as {other} ({relation})", 1, None,
                           "Ship one copy: delete the other, or point both users at one file." if severity != INFO else None))
    return out


def _check_cube(texture, profile, out):
    lut = texture.lut
    for problem in lut.problems:
        what = "a 1D LUT; the engine reads 3D ones" if lut.one_dimensional else problem
        out.append(Finding("file.cube", ERROR, f"the engine cannot read this LUT and will grade with none ({what})", None, None,
                           "Write TITLE, LUT_3D_SIZE n, then n*n*n lines of 'r g b' in 0..1, red changing fastest."))
    if lut.domain:
        out.append(Finding("file.cube_domain", WARN, f"DOMAIN_MIN/MAX of {lut.domain[0]} to {lut.domain[1]}: the engine ignores them and reads "
                           "the table as 0 to 1", None, None, "Bake the table for a 0..1 domain."))
    return out


# ------------------------------------------------------------------ 2: alpha

def fringe_texels(rgba, distance, strong):
    """Fully transparent texels that touch a clearly visible one (alpha at least `strong`)
    and hold a colour more than `distance` levels from the alpha-weighted mean of their
    visible neighbours. Returns (count, the worst hidden colour, the colour beside it)."""
    alpha = rgba[..., 3].astype(np.float64)
    hidden = alpha == 0
    if not hidden.any() or hidden.all():
        return 0, None, None
    weight = np.where(alpha >= strong, alpha, 0.0)
    colour = rgba[..., :3].astype(np.float64)
    height, width = alpha.shape
    padded_weight = np.pad(weight, 1)
    padded_colour = np.pad(colour * weight[..., None], ((1, 1), (1, 1), (0, 0)))
    total = np.zeros_like(colour)
    weights = np.zeros_like(alpha)
    for dy in (0, 1, 2):
        for dx in (0, 1, 2):
            total += padded_colour[dy:dy + height, dx:dx + width]
            weights += padded_weight[dy:dy + height, dx:dx + width]
    edge = hidden & (weights > 0)
    if not edge.any():
        return 0, None, None
    beside = total[edge] / weights[edge][:, None]
    difference = np.abs(colour[edge] - beside).max(axis=1)
    bad = difference > distance
    if not bad.any():
        return 0, None, None
    worst = int(np.argmax(difference))
    return int(bad.sum()), colour[edge][worst], beside[worst]


def check_alpha(texture, profile, context):
    out = []
    if texture.is_lut or texture.rgba is None:
        return out
    alpha = texture.alpha
    rgb = texture.rgba[..., :3]
    kind = alpha_kind(texture)
    if alpha.max() == 0:
        return [Finding("alpha.empty", ERROR, "every texel is fully transparent: nothing will draw", alpha.size, None,
                        "The generator wrote alpha 0 everywhere; write the alpha it meant.")]
    if texture.stores_alpha and kind == "none":
        # Not a flaw in this engine: every texture is uploaded as RGBA8 whatever the file holds. An observation.
        out.append(Finding("alpha.unused", INFO, "an alpha channel that is 255 everywhere (the engine uploads RGBA8 either way; "
                           "tools.save_png and fix.py drop it)"))
    semi = int(((alpha > 0) & (alpha < 255)).sum())
    if texture.role != ti.COLOUR:
        if kind != "none":
            out.append(Finding("alpha.kind", WARN, f"transparency in a{'n emissive' if texture.role == ti.EMISSIVE else ' normal'} map: the "
                               "engine reads only its RGB there", int((alpha < 255).sum()), None, "Save it without alpha."))
        return out

    # What the alpha is against what will be done with it: the material's mode, else the profile.
    judged = False
    for use in texture.uses:
        if use["role"] != ti.COLOUR:
            continue
        mode, where = use["alpha_mode"], f"material '{use['material']}'"
        if mode == "OPAQUE" and kind != "none":
            judged = True
            out.append(Finding("alpha.kind", WARN, "texels with transparency in an OPAQUE material: the engine ignores alpha there, so the "
                               "cutout that was painted will not show", int((alpha < 255).sum()), where,
                               "Set the material to alpha mask (or blend), or fill the texture."))
        elif mode == "MASK" and kind == "blend":
            judged = True
            out.append(Finding("alpha.kind", WARN, "half-transparent texels in a MASK material: each is cut at the cutoff, so the soft edge "
                               "becomes a ragged hard one", semi, where, "Paint alpha as 0 or 255 only (threshold it), or use blend."))
        elif mode == "MASK" and kind == "none":
            judged = True
            out.append(Finding("alpha.kind", WARN, "a MASK material on a texture with nothing cut out: the cutout test runs for nothing", None,
                               where, "Make the material opaque."))
    allowed = profile.get("alpha", "blend")
    if not judged and ALPHA_ORDER[kind] > ALPHA_ORDER.get(allowed, 2):
        count = semi if kind == "blend" and allowed == "mask" else int((alpha < 255).sum())
        need = "no transparency" if allowed == "none" else "alpha of 0 or 255 only (a cutout)"
        out.append(Finding("alpha.kind", WARN, f"texels with {'soft ' if kind == 'blend' else ''}transparency; the '{profile['name']}' "
                           f"profile expects {need}", count, None,
                           "Threshold the alpha (pixel art has no half-transparent edge)." if allowed == "mask" else
                           "Fill the image, or give it a profile that is meant to be see-through."))
    if kind == "none":
        return out

    # Colour hidden under alpha 0 next to visible texels: under linear filtering it is mixed into the edge.
    if sampled_linear(texture, profile):
        count, hidden, beside = fringe_texels(texture.rgba, profile.get("fringe_distance", 32), profile.get("fringe_alpha", 96))
        if count > profile.get("fringe_min_texels", 8):
            out.append(Finding("alpha.fringe", WARN, f"fully transparent texels beside visible ones hold another colour (such as {_hex(hidden)} "
                               f"next to {_hex(beside)}): linear filtering mixes it into the edge as a "
                               f"{'dark' if hidden.sum() < beside.sum() else 'bright'} halo", count, None,
                               "Carry the edge colour outwards under the transparency: tools.bleed_edges (fix.py --bleed does it, changing nothing visible)."))

    # Premultiplied data where the engine blends straight alpha, or the reverse.
    soft = (alpha >= 8) & (alpha <= 247)
    if soft.sum() >= profile.get("premultiplied_min_texels", 64):
        peak = rgb.max(axis=2).astype(np.float64)
        within = peak[soft] <= alpha[soft] + 2.0
        if profile.get("alpha_data", "straight") == "premultiplied":
            over = int((~within).sum())
            if over > 0.01 * soft.sum():
                out.append(Finding("alpha.premultiplied", WARN, "half-transparent texels are brighter than their alpha: straight-alpha data "
                                   "where the profile says premultiplied (it will glow at the edges)", over, None,
                                   "Multiply colour by alpha (tools.premultiply), or draw it with the `alpha` blend."))
        else:
            thin, solid = soft & (alpha < 96), alpha >= 200
            # Premultiplied: colour never exceeds alpha, and it fades with it. A dark image is not evidence either way.
            if within.mean() >= 0.995 and thin.sum() >= 16 and solid.sum() >= 16 and peak[solid].mean() >= 64 \
                    and (peak[thin] / alpha[thin]).mean() >= 0.5 and peak[thin].mean() < 0.6 * peak[solid].mean():
                out.append(Finding("alpha.premultiplied", WARN, "the colour fades to black exactly as the alpha does: premultiplied data, which "
                                   "the engine's straight-alpha blend darkens a second time (grey edges)", int(soft.sum()), None,
                                   "Store straight alpha (tools.unpremultiply), or draw it with blend = 'premultiplied'."))

    # Content in the margin the profile wants clear (per cell of a sheet).
    margin = profile.get("margin", 0)
    if margin and not profile.get("nine_slice"):
        cell = profile.get("cell") or [texture.width, texture.height]
        solid = alpha > profile.get("margin_alpha", 16)
        touching, cells = 0, []
        for top in range(0, texture.height - cell[1] + 1, cell[1]):
            for left in range(0, texture.width - cell[0] + 1, cell[0]):
                block = solid[top:top + cell[1], left:left + cell[0]]
                inner = block[margin:-margin, margin:-margin] if 2 * margin < min(block.shape) else block[:0]
                count = int(block.sum() - inner.sum())
                if count:
                    touching += count
                    cells.append((left // cell[0], top // cell[1]))
        if touching:
            where = None
            if profile.get("cell"):
                where = "cell" + ("s " if len(cells) > 1 else " ") + ", ".join(f"({x}, {y})" for x, y in cells[:6]) + (" ..." if len(cells) > 6 else "")
            out.append(Finding("alpha.border", WARN, f"visible texels in the outer {margin} px that the '{profile['name']}' profile keeps clear: "
                               "the image is cut hard at its edge, and under linear filtering it picks up its neighbour in a sheet",
                               touching, where, "Leave a transparent margin around the content (or around each cell of a sheet)."))
    return out


# ------------------------------------------------------------------ 3: colour

def near_duplicates(colours, distance, limit=4096):
    """How many of the colours (most used first) lie within `distance` levels on every channel
    of a colour that is used more: the cloud around each real palette entry that blurring,
    resampling and anti-aliasing leave behind."""
    colours = colours[:limit]
    count = 0
    for start in range(0, len(colours), 512):
        block = colours[start:start + 512]
        close = np.abs(block[:, None, :] - colours[None, :, :]).max(axis=2) <= distance
        earlier = np.arange(len(colours))[None, :] < (start + np.arange(len(block)))[:, None]
        count += int((close & earlier).any(axis=1).sum())
    return count


def texel_grid(rgba):
    """The grid the image's content sits on: for each axis, the smallest gap between two
    places where neighbouring columns (rows) differ, the largest size every such place is a
    multiple of, and how many such places there are."""
    values = rgba.astype(np.int16)
    out = []
    for axis in (1, 0):
        changes = np.nonzero(np.abs(np.diff(values, axis=axis)).max(axis=(1 - axis, 2)) > 0)[0] + 1
        size = values.shape[axis]
        if not len(changes):
            out.append((size, size, 0))
            continue
        edges = np.concatenate([[0], changes, [size]])
        out.append((int(np.diff(edges).min()), int(np.gcd.reduce(edges[1:])), len(changes)))
    return out


def doubled_lines(rgba):
    """For each axis: (the share of columns (rows) that sit in a pair of exact copies, the share
    that sit in a run of three or more). Nearest-neighbour scaling by a factor between 1 and 2
    turns some source columns into two and leaves the rest alone, so it shows as many pairs
    and hardly any longer run; drawn pixel art has few exact pairs, and flat bands have long runs."""
    out = []
    for axis in (1, 0):
        same = (np.diff(rgba.astype(np.int16), axis=axis) == 0).all(axis=(1 - axis, 2))
        runs, run = [], 1
        for value in same:
            if value:
                run += 1
            else:
                runs.append(run)
                run = 1
        runs.append(run)
        total = max(1, len(same) + 1)
        out.append((sum(r for r in runs if r == 2) / total, sum(r for r in runs if r > 2) / total))
    return out


def check_colour(texture, profile, context):
    out = []
    if texture.is_lut:
        return _check_lut_colour(texture, profile, out)
    if texture.rgba is None or texture.role == ti.NORMAL:
        return out
    colours, counts = colour_table(texture)
    if not len(colours):
        return out
    visible = int(counts.sum())
    soft = alpha_kind(texture) == "blend"   # a glow, smoke, a shadow: its shape and values are in its alpha
    if len(colours) == 1 and texture.width * texture.height > 16 and texture.alpha.min() == texture.alpha.max():
        if texture.role == ti.EMISSIVE and colours[0].max() == 0:
            return out  # ready.emissive_black says it
        return [Finding("colour.flat", WARN, f"one single colour ({_hex(colours[0])}) on {texture.width}x{texture.height} texels", visible, None,
                        "Use the material's colour factor (or a 1x1 texture): a flat texture costs memory and a sampler for nothing.")]

    if texture.role == ti.COLOUR and not soft:
        luminance = colours @ LUMA
        order = np.argsort(luminance)
        cumulative = np.cumsum(counts[order]) / visible

        def level(q):
            return float(luminance[order][min(int(np.searchsorted(cumulative, q)), len(order) - 1)])

        if level(0.99) < profile.get("min_peak", 32):
            out.append(Finding("colour.crushed", WARN, f"nothing in it is brighter than {level(0.99):.0f} of 255: crushed to black, it will not "
                               "hold up under the game's lighting and exposure", None, None,
                               "Lift the values: the lit parts of a surface belong in the middle of the range; light and shadow are the scene's job."))
        elif level(0.01) > profile.get("max_floor", 232):
            out.append(Finding("colour.blown", WARN, f"nothing in it is darker than {level(0.01):.0f} of 255: blown out, with no room for light "
                               "to add anything before it clips and blooms", None, None, "Bring the values down and put contrast back."))
        elif level(0.95) - level(0.05) < profile.get("min_contrast", 6):
            out.append(Finding("colour.contrast", WARN, f"its values span only {level(0.95) - level(0.05):.0f} levels between the 5th and 95th "
                               "percentile: it will read as a flat colour", None, None,
                               "Give it a value structure (darker joints, lighter faces), or use a flat colour and no texture."))

    # Palette discipline is for the colour map: an emissive map may hold a glow's falloff, which bloom finishes.
    if profile.get("pixel_art") and texture.role == ti.COLOUR:
        limit = profile.get("max_colours")
        if limit and len(colours) > limit:
            out.append(Finding("colour.count", WARN, f"distinct colours; the '{profile['name']}' profile keeps pixel art to {limit}", len(colours),
                               None, "Reduce it to a palette (tools.quantize); 3 to 5 tones per material is the usual discipline."))
        near = near_duplicates(colours, profile.get("near_colour_distance", 3))
        if near > max(profile.get("near_colour_min", 8), profile.get("near_colour_share", 0.2) * len(colours)):
            out.append(Finding("colour.near_duplicates", WARN, f"of its {len(colours):,} colours are within {profile.get('near_colour_distance', 3)} "
                               "levels of a more common one: the cloud that blur, resampling, anti-aliasing or a soft brush leaves in pixel art",
                               near, None, "Quantize to the palette (tools.quantize) after any filtering, and never scale pixel art with smoothing."))
        # (An emissive map takes its size from its colour map, however coarse what it draws.)
        (gap_x, step_x, places_x), (gap_y, step_y, places_y) = texel_grid(texture.rgba)
        block = int(np.gcd(step_x, step_y))
        enough = profile.get("grid_min_places", 8)
        if block >= 2 and places_x >= 1 and places_y >= 1:
            out.append(Finding("colour.upscaled", WARN, f"every texel is a {block}x{block} block: the image is stored {block} times too large "
                               f"({texture.width // block}x{texture.height // block} holds the same picture under nearest filtering)", block, None,
                               "Store pixel art at 1x and let the sampler enlarge it."))
        elif gap_x >= 2 and gap_y >= 2 and places_x >= enough and places_y >= enough:
            out.append(Finding("colour.mixels", WARN, f"its texels are blocks of mixed sizes (never narrower than {min(gap_x, gap_y)} px, on no "
                               "common grid): pixel art resampled by a factor that is not a whole number", None, None,
                               "Go back to the 1x original; scale only by whole factors with nearest filtering."))
        else:
            (pairs_x, long_x), (pairs_y, long_y) = doubled_lines(texture.rgba)
            share = profile.get("mixel_pair_share", 0.25)
            if pairs_x >= share and pairs_y >= share and long_x <= 0.2 * pairs_x and long_y <= 0.2 * pairs_y:
                out.append(Finding("colour.mixels", WARN, f"{pairs_x * 100:.0f}% of its columns and {pairs_y * 100:.0f}% of its rows are exact "
                                   "copies of their neighbour, hardly ever three in a row: pixel art enlarged by a factor between 1 and 2, so "
                                   "some texels are one pixel wide and some two", None, None,
                                   "Go back to the 1x original; scale only by whole factors with nearest filtering."))

    palette = context.palette() if context else None
    if palette is not None and profile.get("pixel_art") and len(palette):
        _, distance = tools.nearest_in_palette(colours, palette)
        off = distance > profile.get("palette_tolerance", 2)
        if off.any():
            worst = colours[off][:3]
            out.append(Finding("colour.palette", WARN, f"colours that are not in the game's palette (such as {', '.join(_hex(c) for c in worst)}), "
                               f"on {_percent(counts[off].sum(), visible):.1f}% of its texels", int(off.sum()), None,
                               "Quantize to the palette: tools.quantize(pixels, tools.load_palette(path))."))
    return out


def _check_lut_colour(texture, profile, out):
    lut = texture.lut
    if lut.table is None:
        return out
    table = lut.table
    size = lut.size
    axis = np.arange(size) / (size - 1)
    identity = np.stack(np.meshgrid(axis, axis, axis, indexing="ij")[::-1], axis=-1)
    if np.abs(table - identity).max() < 1.0 / 255:
        out.append(Finding("colour.lut_identity", WARN, "the table is the identity: it grades nothing", None, None,
                           "Remove the `lut` from the map's post settings, or write the grade."))
        return out
    outside = int(((table < -1e-4) | (table > 1 + 1e-4)).any(axis=3).sum())
    if outside > profile.get("lut_max_clipped", 0.01) * size ** 3:
        out.append(Finding("colour.lut_clipped", WARN, f"of its {size ** 3:,} entries are outside 0..1; the engine clamps them, flattening "
                           "whatever was there", outside, None, "Scale the grade into range instead of letting it clip."))
    grey = np.clip(table[np.arange(size), np.arange(size), np.arange(size)], 0, 1) @ LUMA
    drops = int((np.diff(grey) < -0.5 / 255).sum())
    if drops:
        out.append(Finding("colour.lut_tone", WARN, "steps of the grey ramp come out darker than the step before: tones invert, which shows "
                           "as bands in every gradient", drops, None, "Keep the tone curve rising."))
    elif grey[-1] - grey[0] < profile.get("lut_min_range", 0.5):
        out.append(Finding("colour.lut_tone", WARN, f"black comes out at {grey[0] * 255:.0f} and white at {grey[-1] * 255:.0f} of 255: the grade "
                           "throws away most of the range", None, None, "Grade with exposure and the tone curve first; keep black near black and white near white."))
    return out


# ------------------------------------------------------------------ 4: tiling and layout

def repeat_contrast(texture, across_u=True, across_v=True, grid=4):
    """How far the most unlike part of the image (it cut into a 4x4 grid, or strips for a
    texture that repeats along one axis only) is from the typical one, in levels: the blob,
    stain or gradient that turns into a visible lattice when the texture repeats."""
    rgba = texture.rgba.astype(np.float64)
    shown = np.dstack([rgba[..., :3] * rgba[..., 3:4] / 255.0, rgba[..., 3:4]])
    height, width = shown.shape[:2]
    gy = grid if across_v else 1
    gx = grid if across_u else 1
    if height < gy * 2 or width < gx * 2:
        return 0.0
    blocks = shown[:height // gy * gy, :width // gx * gx].reshape(gy, height // gy, gx, width // gx, 4).mean(axis=(1, 3)).reshape(-1, 4)
    return float(np.abs(blocks - np.median(blocks, axis=0)).max())


def nyquist_share(texture):
    """(levels of luminance detail that the first mip level erases, that as a share of the
    image's whole contrast): near 1 means the picture *is* single-texel detail (a 1 px
    checker, dither, hairlines), which flickers as soon as it is drawn smaller than 1:1."""
    luminance = texture.rgba[..., :3].astype(np.float64) @ LUMA * (texture.alpha / 255.0)
    height, width = luminance.shape[0] // 2 * 2, luminance.shape[1] // 2 * 2
    if height < 2 or width < 2:
        return 0.0, 0.0
    luminance = luminance[:height, :width]
    half = luminance.reshape(height // 2, 2, width // 2, 2).mean(axis=(1, 3))
    lost = float(np.abs(luminance - np.repeat(np.repeat(half, 2, axis=0), 2, axis=1)).mean())
    contrast = float(np.abs(luminance - luminance.mean()).mean())
    return lost, lost / contrast if contrast > 1e-6 else 0.0


def check_tiling(texture, profile, context):
    out = []
    if texture.is_lut or texture.rgba is None:
        return out
    tile = profile.get("tile", "none")
    axes = [axis for axis in ("u", "v") if tile in ("both", axis)]
    if axes and min(texture.width, texture.height) >= 8:
        measured = tools.seam(texture.rgba)
        for axis in axes:
            m = measured[axis]
            factor, floor = profile.get("seam_factor", 1.5), profile.get("seam_min", 4)
            hard = m["step"] > factor * m["interior"] and m["step"] > floor
            drift = m["drift"] > factor * m["interior_drift"] and m["drift"] > floor
            if hard or drift:
                edges = "left and right edges" if axis == "u" else "top and bottom edges"
                what = (f"a step of {m['step']:.0f} levels where the largest between neighbours inside is {m['interior']:.0f}" if hard else
                        f"its two sides differ by {m['drift']:.0f} levels where neighbouring eighths inside differ by at most {m['interior_drift']:.0f}")
                out.append(Finding("tiling.seam", WARN, f"the {edges} do not meet: {what}; it will show as a line every repeat",
                                   texture.height if axis == "u" else texture.width, edges,
                                   "Generate the pattern on wrapped coordinates; as a repair, tools.make_tileable (see its notes for pixel art)."))
        if texture.role == ti.COLOUR:
            contrast = repeat_contrast(texture, "u" in axes, "v" in axes)
            if contrast > profile.get("repeat_contrast", 14):
                out.append(Finding("tiling.repeat", WARN, f"one part of it stands {contrast:.0f} levels apart from the rest (a blotch, a band or a "
                                   "gradient): repeated across a wall or a floor it reads as a lattice", int(round(contrast)), None,
                                   "Even out the large shapes (keep variation small and frequent), or break the repeat in the scene with decals and props."))
    if texture.role != ti.NORMAL and has_mips(texture, profile) and min(texture.width, texture.height) >= 8:
        lost, share = nyquist_share(texture)
        if share > profile.get("shimmer_share", 0.75) and lost > profile.get("shimmer_min", 6):
            out.append(Finding("tiling.shimmer", WARN, f"{share * 100:.0f}% of its contrast is single-texel detail (a 1 px checker, dither or "
                               "hairlines) that the first mip level erases: it flickers whenever it is drawn smaller than 1:1", None, None,
                               "Make the smallest feature two texels wide, or lower the dither's contrast."))
    if profile.get("atlas") and texture.role == ti.COLOUR and tile == "none":
        colours, counts = colour_table(texture)
        covered = float(counts.sum()) / (texture.width * texture.height)
        empty = max(float(counts[0]) / (texture.width * texture.height) if len(counts) else 0.0, 1.0 - covered)
        wasted = int(empty * texture.width * texture.height)
        if empty > profile.get("atlas_max_empty", 0.4) and wasted > profile.get("atlas_min_waste", 16384):
            out.append(Finding("tiling.empty", WARN, f"texels, {empty * 100:.0f}% of the atlas, are one flat colour (unused space): the texture is "
                               "larger than what is painted on it", wasted, None,
                               "Pack the islands into a smaller texture, or scale them up to use it (the UV side is modelkit's)."))
    return out


# ------------------------------------------------------------------ 5: game readiness

def normal_vectors(texture):
    return texture.rgba[..., :3].astype(np.float64) / 127.5 - 1.0


def normal_green(texture, wrap):
    """Which way green points, from the map alone: a normal map is the slope of a surface, and
    only one reading of its green channel gives slopes that fit together into one (a field
    with no curl). Returns ("up" | "down" | None, how many times better that reading fits)."""
    vector = normal_vectors(texture)
    depth = np.clip(vector[..., 2], 0.2, None)
    slope_x, slope_y = -vector[..., 0] / depth, vector[..., 1] / depth     # the height's gradient, if green points up the image

    def curl(gx, gy):
        down = np.roll(gx, -1, axis=0) - gx
        right = np.roll(gy, -1, axis=1) - gy
        down = (down + np.roll(down, -1, axis=1)) / 2
        right = (right + np.roll(right, -1, axis=0)) / 2
        difference = np.abs(down - right)
        return float((difference if wrap else difference[1:-2, 1:-2]).mean()) if min(difference.shape) > 4 else 0.0

    up, down = curl(slope_x, slope_y), curl(slope_x, -slope_y)
    if max(up, down) < 0.004:
        return None, 1.0
    ratio = max(up, down) / max(min(up, down), 1e-9)
    return ("up" if up < down else "down"), ratio


def check_ready(texture, profile, context):
    out = []
    name = texture.name
    # The name: what the game's files and the next person read.
    pattern = profile.get("name_pattern", "^[a-z][a-z0-9_]*$")
    if not texture.named:
        out.append(Finding("ready.name", WARN, "an embedded image with no name (it can only be addressed by its index)", None, None,
                           "Name the image in the generator (in Blender: the image datablock's name)."))
    elif pattern and not re.match(pattern, name):
        out.append(Finding("ready.name", WARN, f"the name '{name}' does not follow the convention (lower case, digits and underscores: {pattern})",
                           None, None, "Rename it, and the references to it."))
    else:
        suffixes = profile.get("role_suffixes") or {}
        mine = [suffix for suffix in suffixes.get(texture.role, []) if name.lower().endswith(suffix)]
        other = [(role, suffix) for role, group in suffixes.items() if role != texture.role for suffix in group if name.lower().endswith(suffix)]
        if texture.role in suffixes and not mine and texture.role_from != "by default":
            out.append(Finding("ready.name", WARN, f"used as a{'n' if texture.role == ti.EMISSIVE else ''} {ti.SLOTS[texture.role]} but named "
                               f"'{name}': the convention ends such a map in {' or '.join(suffixes[texture.role])}", None, None,
                               "Rename it so the name says what the map is."))
        elif other and not mine and texture.uses:
            out.append(Finding("ready.name", WARN, f"named like a{'n' if other[0][0] == ti.EMISSIVE else ''} {ti.SLOTS.get(other[0][0], other[0][0])} "
                               f"('{name}') but used as a {ti.SLOTS[texture.role]}", None, None, "Rename it, or fix the material."))
    if texture.is_lut:
        return out + _check_lut_ready(texture, profile, context)
    if texture.rgba is None:
        return out

    width, height = texture.width, texture.height
    largest, smallest = profile.get("max_size"), profile.get("min_size")
    if largest and max(width, height) > largest:
        out.append(Finding("ready.max_size", ERROR, f"{width}x{height}; the '{profile['name']}' profile allows {largest}", None, None,
                           "Reduce it: texel density on the surface, not texture size, is what the player sees."))
    if smallest and min(width, height) < smallest:
        out.append(Finding("ready.min_size", WARN, f"{width}x{height}; the '{profile['name']}' profile expects at least {smallest} on each side",
                           None, None, "A texture this small is usually a colour: use a material factor, or check it is the right file."))
    if profile.get("pot") and any(side & (side - 1) for side in (width, height)):
        out.append(Finding("ready.pot", WARN, f"{width}x{height} is not a power of two: its mip levels drop rows and columns and stop lining up "
                           "with the texel grid, and a repeat no longer lands on whole texels", None, None,
                           "Use 16, 32, 64, 128, 256, 512 or 1024 on each side (tools.pad_pow2 for an atlas)."))
    aspect = max(width, height) / max(1, min(width, height))
    if profile.get("max_aspect") and aspect > profile["max_aspect"]:
        out.append(Finding("ready.aspect", WARN, f"{width}x{height} is {aspect:.0f} to 1; the '{profile['name']}' profile expects at most "
                           f"{profile['max_aspect']} to 1", None, None, "Split a strip into rows of one texture, or check the size is what was meant."))
    megabytes = width * height * 4 * (4 / 3 if has_mips(texture, profile) else 1) / 1e6
    if profile.get("max_megabytes") and megabytes > profile["max_megabytes"]:
        out.append(Finding("ready.memory", ERROR, f"{megabytes:.1f} MB on the GPU (RGBA8{' with mips' if has_mips(texture, profile) else ''}); "
                           f"the '{profile['name']}' budget is {profile['max_megabytes']} MB", None, None, "Halve the size."))
    cell = profile.get("cell")
    if cell and (width % cell[0] or height % cell[1]):
        out.append(Finding("ready.cell", WARN, f"{width}x{height} is not a whole number of {cell[0]}x{cell[1]} cells", None, None,
                           "Size the sheet to its grid."))

    # How materials sample it.
    seen = set()
    for use in texture.uses:
        sampler = sampler_of(use)
        key = (use["model"], sampler["text"])
        if key in seen:
            continue
        seen.add(key)
        where = f"material '{use['material']}' of {use['model']}"
        if profile.get("filter") == "nearest" and not sampler["nearest"]:
            out.append(Finding("ready.sampler", WARN, "sampled with linear magnification: pixel art turns to blur", None, where,
                               "Set the texture's interpolation to Closest (glTF magFilter NEAREST)."))
        elif profile.get("filter") == "linear" and sampler["nearest"]:
            out.append(Finding("ready.sampler", WARN, "sampled with nearest magnification: smooth art turns to stair-steps", None, where,
                               "Set the texture's interpolation to Linear."))
        if profile.get("mips") and not sampler["mips"]:
            out.append(Finding("ready.sampler", WARN, "sampled without mips: it sparkles as soon as it is seen from a distance", None, where,
                               "Use a mip-mapped minification filter (the Blender exporter's default; NEAREST_MIPMAP_NEAREST for pixel art)."))
        tile = profile.get("tile", "none")
        if (tile in ("both", "u") and sampler["clamp_u"]) or (tile in ("both", "v") and sampler["clamp_v"]):
            out.append(Finding("ready.sampler", WARN, "a tiling texture whose sampler clamps: past its first repeat it smears its edge texel",
                               None, where, "Set the texture's extension to Repeat."))

    if texture.embedded and not any(use["drawn"] for use in texture.uses):
        out.append(Finding("ready.unused", WARN, "no material of a mesh in the model's scene samples this image: it ships inside the .glb for nothing",
                           len(texture.data), None, "Remove unused images before export (orphan data in Blender)."))
    elif not texture.embedded and context:
        used, how = context.referenced(texture)
        if not used:
            out.append(Finding("ready.unused", WARN, "nothing in the game names this file (its maps, data, Lua and glTF were searched): it "
                               "ships for nothing", len(texture.data), None,
                               "Delete it, or keep working files outside the game folder; if code builds its name, say so with a waiver."))
        elif how:
            out.append(Finding("ready.referenced", INFO, how))

    if texture.role == ti.NORMAL:
        out += _check_normal(texture, profile, context)
    elif texture.role == ti.EMISSIVE:
        lit = texture.rgba[..., :3].max(axis=2) > profile.get("emissive_dark", 16)
        share = float(lit.mean())
        if not lit.any():
            out.append(Finding("ready.emissive_black", WARN, "an emissive map with nothing lit in it: a texture and a sampler for no glow", None,
                               None, "Remove the emissive texture from the material, or paint what glows."))
        elif share > profile.get("emissive_max_lit", 0.5):
            out.append(Finding("ready.emissive_coverage", WARN, f"{share * 100:.0f}% of the emissive map is lit: emission is for what is lit from "
                               "within (windows, runes, embers), and with an HDR strength this much of it washes the surface out in bloom",
                               int(lit.sum()), None, "Keep it mostly black; make a surface brighter with its colour and the scene's lights."))
    if texture.partner and texture.role in (ti.NORMAL, ti.EMISSIVE) and (texture.partner[1], texture.partner[2]) != (width, height):
        out.append(Finding("ready.map_size", WARN, f"{width}x{height}, but the colour map it belongs to ('{texture.partner[0]}') is "
                           f"{texture.partner[1]}x{texture.partner[2]}: its texels do not line up with the pixel art they shade", None, None,
                           "Make the maps of one material the same size."))

    border = profile.get("nine_slice")
    if border:
        out += _check_nine_slice(texture, profile, border)
    return out


def _check_normal(texture, profile, context):
    out = []
    vector = normal_vectors(texture)
    rgb = texture.rgba[..., :3].astype(np.int32)
    total = texture.width * texture.height
    if (np.abs(rgb - [128, 128, 255]).max(axis=2) <= profile.get("normal_flat_tolerance", 3)).all():
        return [Finding("ready.normal_flat", WARN, "a normal map that is flat everywhere: it changes nothing and costs a texture", None, None,
                        "Remove it from the material (the engine uses a flat normal when there is none), or bake the relief.")]
    median = np.median(rgb[..., :2].reshape(-1, 2), axis=0)
    if np.abs(median - 128).max() > profile.get("normal_centre_tolerance", 24):
        guess = (" (about 188 is what 128 becomes when a linear image is saved through an sRGB conversion)" if median.min() > 170 else
                 " (about 55 is what 128 becomes when an sRGB image is converted to linear on save)" if median.max() < 80 else "")
        out.append(Finding("ready.normal_encoding", WARN, f"its red and green centre on {median[0]:.0f} and {median[1]:.0f}, not 128: the "
                           f"vectors lean one way everywhere{guess}", None, None,
                           "Save normal maps as plain data: no colour management, no gamma, 'Non-Color' in Blender."))
        return out
    length = np.linalg.norm(vector, axis=2)
    off = np.abs(length - 1.0) > profile.get("normal_length_tolerance", 0.15)
    if off.sum() > profile.get("normal_bad_share", 0.02) * total:
        out.append(Finding("ready.normal_length", WARN, f"vectors that are not unit length ({_percent(off.sum(), total):.0f}% of the map; lengths "
                           f"from {length.min():.2f} to {length.max():.2f}): it was painted on, scaled or blended as if it were a picture",
                           int(off.sum()), None, "Renormalise it (tools.renormalize), or derive it again from the height (tools.normal_from_height)."))
    back = vector[..., 2] <= 0
    if back.sum() > profile.get("normal_back_share", 0.005) * total:
        out.append(Finding("ready.normal_blue", WARN, "vectors that point into the surface (blue under 128): they are lit from behind", int(back.sum()),
                           None, "Blue is the part towards the viewer and is never negative: check the channel order and the encoding."))
    # Which way green points, against what the engine will do with it on this mesh.
    direction, ratio = normal_green(texture, profile.get("tile", "none") == "both")
    if direction and ratio >= profile.get("normal_green_confidence", 1.5):
        for use in texture.uses:
            if use["role"] != ti.NORMAL:
                continue
            tangents = ti.use_has_tangents(use)
            if tangents is None:
                continue
            expected = "up" if tangents else profile.get("derived_tangents_green", "down")
            if direction != expected:
                why = ("its mesh carries tangents, so the engine reads green as up (glTF)" if tangents else
                       "its mesh has no tangents, and the tangents the engine derives read green as pointing down the image")
                out.append(Finding("ready.normal_green", WARN, f"a green-{direction} normal map where the engine will light it as green-{expected}: "
                                   f"{why}; every bump is lit from the wrong side top to bottom", None,
                                   f"material '{use['material']}' of {use['model']}",
                                   "Export the mesh with tangents and keep maps green-up (the documented convention), or flip green "
                                   "(tools.flip_green) for a mesh without tangents; see the guide, 'What the engine expects'."))
                break
        else:
            if not any(use["role"] == ti.NORMAL for use in texture.uses) and direction != profile.get("normal_green", "up"):
                out.append(Finding("ready.normal_green_note", INFO, f"a green-{direction} normal map (the documented convention is green-"
                                   f"{profile.get('normal_green', 'up')}); no mesh uses it, so nothing says how it will be lit"))
    return out


def _check_nine_slice(texture, profile, border):
    """A nine-slice stretches its edges along their length and its middle both ways, so those
    parts must not hold detail across the direction they are stretched in."""
    out = []
    sides = border if isinstance(border, dict) else {key: border for key in ("l", "t", "r", "b")}
    left, top, right, bottom = (int(sides.get(key, 8)) for key in ("l", "t", "r", "b"))
    width, height = texture.width, texture.height
    if left + right >= width or top + bottom >= height:
        return [Finding("ready.nine_slice", ERROR, f"{width}x{height} with borders of {left}+{right} and {top}+{bottom}: there is no middle "
                        "left to stretch", None, None, "Make the image larger than its two borders, or the borders smaller.")]
    values = texture.rgba.astype(np.int32)
    step = profile.get("nine_slice_step", 12)
    along_x = np.abs(np.diff(values[:, left:width - right], axis=1)).max(axis=2) > step       # stretched horizontally
    along_y = np.abs(np.diff(values[top:height - bottom, :], axis=0)).max(axis=2) > step      # stretched vertically
    count = int(along_x.sum() + along_y.sum())
    if count > profile.get("nine_slice_min_texels", 4):
        parts = []
        if along_x[:top].any() or along_x[height - bottom:].any():
            parts.append("top or bottom edge")
        if along_y[:, :left].any() or along_y[:, width - right:].any():
            parts.append("left or right edge")
        if along_x[top:height - bottom].any() or along_y[:, left:width - right].any():
            parts.append("middle")
        out.append(Finding("ready.nine_slice", WARN, f"texels of detail across the direction the panel stretches (border {left}/{top}/{right}/"
                           f"{bottom}): lines and patterns there are smeared or thickened by however much each panel is enlarged", count,
                           ", ".join(parts), "Keep the stretched parts constant along the stretch (gradients are fine); put detail in the corners."))
    return out


def _check_lut_ready(texture, profile, context):
    out = []
    lut = texture.lut
    if lut.table is not None and lut.size > profile.get("lut_max_size", 33):
        out.append(Finding("ready.lut_size", WARN, f"a table of {lut.size} cubed entries: the engine resamples every LUT to 16 cubed, so the "
                           "rest is file size and load time", lut.size ** 3, None, "Write it at 16 or 17."))
    if context:
        used, how = context.referenced(texture)
        if not used:
            out.append(Finding("ready.unused", WARN, "no map or script names this LUT: it ships for nothing", len(texture.data), None,
                               "Delete it, or name it in a map's `environment.post.lut`."))
    return out


CHECKS = {"file": check_file, "alpha": check_alpha, "colour": check_colour, "tiling": check_tiling, "ready": check_ready}


def run(texture, profile, context=None):
    findings = []
    for group, _ in GROUPS:
        findings.extend(CHECKS[group](texture, profile, context))
    return findings


def stats_line(texture, profile):
    """One line of facts for the report and the review sheet."""
    if texture.is_lut:
        lut = texture.lut
        return f"a .cube LUT of {lut.size} cubed ({lut.rows:,} rows), {_kb(len(texture.data))}; the engine resamples it to 16 cubed"
    colours = len(colour_table(texture)[1])
    megabytes = texture.width * texture.height * 4 * (4 / 3 if has_mips(texture, profile) else 1) / 1e6
    parts = [f"{texture.width}x{texture.height} {texture.format} ({texture.mode}), {_kb(len(texture.data))}",
             f"{colours:,} colours, alpha {alpha_kind(texture)}",
             f"{megabytes:.2f} MB on the GPU{' with mips' if has_mips(texture, profile) else ''}",
             f"{ti.SLOTS.get(texture.role, texture.role)} ({texture.role_from})"]
    return "; ".join(parts)


def use_lines(texture):
    """How glTF materials sample it, for the review sheet."""
    lines = []
    for use in texture.uses:
        lines.append(f"{use['model']}: material '{use['material']}' {ti.SLOTS[use['role']]}, {use['alpha_mode'].lower()}, "
                     f"{sampler_of(use)['text']}" + ("" if use["drawn"] else " (not drawn)"))
    return lines
