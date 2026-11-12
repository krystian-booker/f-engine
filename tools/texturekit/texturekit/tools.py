"""Helpers for the scripts that generate textures: write a clean file in the
first place, and fix what the checks find. numpy and pillow only.

    import sys; sys.path.insert(0, "tools/texturekit")
    from texturekit import tools as tk

    tk.save_png("samples/<game>/maps/keep/textures/wall.png", pixels)     # 8-bit, no ancillary chunks, no unused alpha
    pixels = tk.quantize(pixels, 32)                                      # or a palette: tk.load_palette("palette.hex")
    normal = tk.normal_from_height(height, strength=2.0)                  # red right, green up, blue towards the viewer
    print(tk.seam(pixels))                                                # how hard the wrap is, against the interior

`pixels` is anything image-like: a path, a PIL image, or an array of shape
(h, w), (h, w, 3) or (h, w, 4), integers 0..255 or floats 0..1. Functions return uint8
arrays of the same channel count unless they say otherwise.
"""
import io
import json
import os

import numpy as np
from PIL import Image


def as_rgba(pixels):
    """Any image-like value as an (h, w, 4) uint8 array."""
    if isinstance(pixels, (str, os.PathLike)):
        pixels = Image.open(pixels)
    if isinstance(pixels, Image.Image):
        return np.asarray(pixels.convert("RGBA")).copy()
    array = np.asarray(pixels)
    if array.dtype.kind == "f":                     # floats are 0..1
        array = (np.clip(array.astype(np.float64), 0, 1) * 255 + 0.5).astype(np.uint8)
    elif array.dtype != np.uint8:                   # other integers are 0..255
        array = np.clip(array, 0, 255).astype(np.uint8)
    if array.ndim == 2:
        array = np.dstack([array, array, array])
    if array.shape[2] == 3:
        array = np.dstack([array, np.full(array.shape[:2], 255, dtype=np.uint8)])
    return np.ascontiguousarray(array[..., :4])


def _like(result, pixels):
    """`result` (RGBA) with the channel count the caller passed in."""
    if isinstance(pixels, np.ndarray) and pixels.ndim == 3 and pixels.shape[2] == 3:
        return result[..., :3]
    if isinstance(pixels, np.ndarray) and pixels.ndim == 2:
        return result[..., 0]
    return result


def _colours(rgba):
    """(unique RGBA colours as (n, 4), the index of each pixel's colour as (h, w), pixel count per colour)."""
    packed = rgba.astype(np.uint32)
    packed = (packed[..., 0] << 24) | (packed[..., 1] << 16) | (packed[..., 2] << 8) | packed[..., 3]
    unique, inverse, counts = np.unique(packed, return_inverse=True, return_counts=True)
    colours = np.stack([(unique >> 24) & 255, (unique >> 16) & 255, (unique >> 8) & 255, unique & 255], axis=1).astype(np.uint8)
    return colours, inverse.reshape(rgba.shape[:2]), counts


# ------------------------------------------------------------------ writing files

def encode_png(pixels):
    """The smallest plain PNG of these pixels: 8 bits, no ancillary chunks (no gamma, profile,
    text or time), not interlaced, alpha dropped when every pixel is opaque, and a palette when
    256 colours are enough (which is most pixel art). The decoded pixels are exactly the input's."""
    rgba = as_rgba(pixels)
    opaque = bool((rgba[..., 3] == 255).all())
    colours, index, _ = _colours(rgba)
    out = io.BytesIO()
    if len(colours) <= 256:
        image = Image.fromarray(index.astype(np.uint8), "P")
        image.putpalette(colours[:, :3].reshape(-1).tolist())
        options = {} if opaque else {"transparency": bytes(colours[:, 3].tolist())}
        image.save(out, "PNG", optimize=True, **options)
    else:
        Image.fromarray(rgba[..., :3] if opaque else rgba).save(out, "PNG", optimize=True)
    return out.getvalue()


def save_png(path, pixels):
    """Write `encode_png(pixels)` to `path`. Returns the number of bytes written."""
    data = encode_png(pixels)
    with open(path, "wb") as out:
        out.write(data)
    return len(data)


# ------------------------------------------------------------------ alpha

def bleed_edges(pixels, distance=2):
    """Carry the colour of visible texels into the fully transparent ones next to them, `distance`
    texels outwards (None: until the whole image is filled), leaving alpha as it is. A linearly
    filtered image blends each edge texel with its transparent neighbour; if that neighbour holds
    black (or white), the edge gets a dark (or bright) halo. Two texels are enough for the engine
    (its mips weight colour by alpha, so only magnification reaches under the edge). Lossless:
    nothing visible changes."""
    rgba = as_rgba(pixels)
    colour = rgba[..., :3].astype(np.float64)
    known = rgba[..., 3] > 0
    if not known.any() or known.all():
        return rgba
    steps = distance if distance is not None else max(rgba.shape[:2])
    for _ in range(steps):
        if known.all():
            break
        total = np.zeros_like(colour)
        weight = np.zeros(known.shape)
        padded_colour = np.pad(colour * known[..., None], ((1, 1), (1, 1), (0, 0)))
        padded_known = np.pad(known, 1).astype(np.float64)
        height, width = known.shape
        for dy in (0, 1, 2):
            for dx in (0, 1, 2):
                if dy == 1 and dx == 1:
                    continue
                total += padded_colour[dy:dy + height, dx:dx + width]
                weight += padded_known[dy:dy + height, dx:dx + width]
        grow = ~known & (weight > 0)
        if not grow.any():
            break
        colour[grow] = total[grow] / weight[grow][:, None]
        known = known | grow
    out = rgba.copy()
    hidden = rgba[..., 3] == 0
    out[..., :3][hidden] = np.clip(colour[hidden] + 0.5, 0, 255).astype(np.uint8)
    return out


def premultiply(pixels):
    """Straight alpha to premultiplied (colour times alpha), for the `premultiplied` blend mode."""
    rgba = as_rgba(pixels).astype(np.float64)
    rgba[..., :3] = rgba[..., :3] * rgba[..., 3:4] / 255.0
    return (rgba + 0.5).astype(np.uint8)


def unpremultiply(pixels):
    """Premultiplied alpha back to straight, which is what the engine's `alpha` blend and its
    canvas expect. Colour under alpha 0 is lost in a premultiplied image: bleed_edges() refills it."""
    rgba = as_rgba(pixels).astype(np.float64)
    alpha = rgba[..., 3:4]
    rgba[..., :3] = np.where(alpha > 0, np.minimum(rgba[..., :3] * 255.0 / np.maximum(alpha, 1), 255), 0)
    return bleed_edges((rgba + 0.5).astype(np.uint8), None)


# ------------------------------------------------------------------ tiling

def _wrap_step(values):
    """How the wrap between the last and the first column compares with the interior: the mean
    step across the wrap, the largest mean step between two neighbouring interior columns, and
    the same for the image cut into eight bands (a slow drift shows there, not texel to texel)."""
    height, width = values.shape[:2]
    interior = np.abs(np.diff(values, axis=1)).mean(axis=(0, 2))
    wrap = float(np.abs(values[:, 0] - values[:, -1]).mean())
    bands = min(8, width // 2)
    size = width // bands
    means = values[:, :bands * size].reshape(height, bands, size, -1).mean(axis=(0, 2))
    drift = np.abs(np.diff(means, axis=0)).max(axis=1)
    band_wrap = float(np.abs(means[0] - means[-1]).max())
    return wrap, float(interior.max()) if len(interior) else 0.0, band_wrap, float(drift.max()) if len(drift) else 0.0


def seam(pixels):
    """How the texture meets itself when it repeats, in 8-bit levels: {"u": {...}, "v": {...}}
    with, for the left/right wrap ("u") and the top/bottom wrap ("v"): `step` (mean difference
    across the wrap), `interior` (the largest mean difference between two neighbouring columns
    or rows inside), `drift` and `interior_drift` (the same on eighths of the image) and
    `ratio`, the worse of step/interior and drift/interior_drift. A ratio near or under 1 means
    the wrap looks like any other place in the texture; the `tiling.seam` check warns from
    about 1.5. Colour is weighted by alpha, so a transparent edge is not a seam."""
    rgba = as_rgba(pixels).astype(np.float64)
    values = np.dstack([rgba[..., :3] * rgba[..., 3:4] / 255.0, rgba[..., 3:4]])
    out = {}
    for name, array in (("u", values), ("v", values.transpose(1, 0, 2))):
        step, interior, drift, interior_drift = _wrap_step(array)
        ratio = max(step / max(interior, 1e-9) if step > 0 else 0.0, drift / max(interior_drift, 1e-9) if drift > 0 else 0.0)
        out[name] = {"step": step, "interior": interior, "drift": drift, "interior_drift": interior_drift, "ratio": ratio}
    return out


def make_tileable(pixels, width=0.25, hard=False, axes="both"):
    """Make a texture repeat without a seam by cross-fading it with itself shifted half its
    size: the middle keeps the original, and towards each edge it turns into the shifted copy,
    whose edges are the original's (continuous) middle. `width` is the share of the size the
    fade takes on each side.

    Honest notes. The fade mixes two unrelated places, so it softens detail there, and on pixel
    art it makes new in-between colours (`colour.near_duplicates` will say so): pass `hard=True`
    to switch between the two copies texel by texel with an ordered dither instead of mixing
    them, which keeps the palette but can cut a brick or a plank in two; quantize() afterwards
    is the other way out. Neither replaces drawing the pattern to wrap in the first place
    (generate with coordinates taken modulo the size). Not for normal maps: derive those from a
    tileable height with normal_from_height()."""
    rgba = as_rgba(pixels)
    height, width_px = rgba.shape[:2]
    weight = np.ones((height, width_px))

    def ramp(size):
        position = (np.arange(size) + 0.5) / size
        edge = np.minimum(position, 1 - position)            # 0 at the border, 0.5 in the middle
        return np.clip(edge / max(width, 1e-6), 0, 1)

    shifted = rgba
    if axes in ("both", "u"):
        weight = weight * ramp(width_px)[None, :]
        shifted = np.roll(shifted, width_px // 2, axis=1)
    if axes in ("both", "v"):
        weight = weight * ramp(height)[:, None]
        shifted = np.roll(shifted, height // 2, axis=0)
    if hard:
        bayer = np.array([[0, 8, 2, 10], [12, 4, 14, 6], [3, 11, 1, 9], [15, 7, 13, 5]]) / 16.0 + 1 / 32.0
        threshold = np.tile(bayer, (height // 4 + 1, width_px // 4 + 1))[:height, :width_px]
        result = np.where((weight > threshold)[..., None], rgba, shifted)
    else:
        result = (rgba * weight[..., None] + shifted * (1 - weight[..., None]) + 0.5).astype(np.uint8)
    return _like(result, pixels)


def mirror_tile(pixels, axes="both"):
    """A texture twice the size made of the image and its mirror images: it always repeats
    without a seam and never changes a colour, but the symmetry shows on anything with a
    direction (lettering, light from one side, a diagonal crack), and it doubles the size."""
    rgba = as_rgba(pixels)
    if axes in ("both", "u"):
        rgba = np.concatenate([rgba, rgba[:, ::-1]], axis=1)
    if axes in ("both", "v"):
        rgba = np.concatenate([rgba, rgba[::-1]], axis=0)
    return _like(rgba, pixels)


# ------------------------------------------------------------------ colour

def palette_of(pixels, limit=None):
    """[((r, g, b), share)] of the visible texels, most used first (`limit`: only the first so many)."""
    rgba = as_rgba(pixels)
    visible = rgba[rgba[..., 3] > 0][:, :3]
    if not len(visible):
        return []
    packed = (visible[:, 0].astype(np.uint32) << 16) | (visible[:, 1].astype(np.uint32) << 8) | visible[:, 2]
    unique, counts = np.unique(packed, return_counts=True)
    order = np.argsort(-counts, kind="stable")[:limit]
    return [((int(unique[i] >> 16) & 255, int(unique[i] >> 8) & 255, int(unique[i]) & 255), float(counts[i]) / len(visible)) for i in order]


def load_palette(source):
    """A palette as an (n, 3) uint8 array, from a list of colours ("#rrggbb" or (r, g, b)), a
    .hex or .txt file (one rrggbb per line), a GIMP .gpl, a .json list of colours, an (n, 3) array, or an
    image (a path, a PIL image or an (h, w, channels) array: its distinct colours)."""
    def one(value):
        if isinstance(value, str):
            text = value.strip().lstrip("#")
            return [int(text[i:i + 2], 16) for i in (0, 2, 4)]
        return [int(v) for v in value[:3]]

    if isinstance(source, np.ndarray) and source.ndim == 2:
        return source[:, :3].astype(np.uint8)
    if isinstance(source, (list, tuple)):
        return np.array([one(value) for value in source], dtype=np.uint8).reshape(-1, 3)
    extension = os.path.splitext(str(source))[1].lower()
    if extension in (".hex", ".txt", ".gpl", ".json"):
        with open(source, encoding="utf-8") as file:
            text = file.read()
        if extension == ".json":
            return load_palette(json.loads(text))
        colours = []
        for line in text.splitlines():
            line = line.split(";")[0].strip()
            if not line or line.startswith("#!") or line.startswith("GIMP") or line.split(":")[0] in ("Name", "Columns"):
                continue
            parts = line.split()
            if extension == ".gpl":
                if len(parts) >= 3 and all(part.isdigit() for part in parts[:3]):
                    colours.append([int(part) for part in parts[:3]])
            elif len(parts[0].lstrip("#")) == 6:
                colours.append(one(parts[0]))
        return np.array(colours, dtype=np.uint8).reshape(-1, 3)
    return np.array([colour for colour, _ in palette_of(source)], dtype=np.uint8).reshape(-1, 3)


def nearest_in_palette(colours, palette):
    """For each (n, 3) colour: (index of the nearest palette colour, the largest channel difference to it)."""
    colours = np.asarray(colours, dtype=np.int32).reshape(-1, 3)
    palette = np.asarray(palette, dtype=np.int32).reshape(-1, 3)
    index = np.zeros(len(colours), dtype=np.int64)
    distance = np.zeros(len(colours), dtype=np.int64)
    for start in range(0, len(colours), 4096):
        block = colours[start:start + 4096]
        difference = block[:, None, :] - palette[None, :, :]
        # Nearest by a luminance-weighted distance, so a hue is not traded for a closer grey.
        cost = (difference.astype(np.float64) ** 2 * [0.30, 0.59, 0.11]).sum(axis=2)
        best = cost.argmin(axis=1)
        index[start:start + 4096] = best
        distance[start:start + 4096] = np.abs(difference[np.arange(len(block)), best]).max(axis=1)
    return index, distance


def quantize(pixels, palette, dither=False):
    """Reduce the image to a palette: a number of colours (the image's own best `n`, by median
    cut over its visible texels) or a palette (anything load_palette() takes), each texel to
    its nearest colour. `dither=True` adds an ordered 4x4 dither between the two nearest
    colours, which is the pixel-art way to fake a gradient; without it areas go flat. Alpha is
    kept as it is. This is how blur, anti-aliasing and resampling are cleaned out of pixel art."""
    rgba = as_rgba(pixels)
    visible = rgba[..., 3] > 0
    if isinstance(palette, (int, np.integer)):
        source = Image.fromarray(rgba[visible][:, :3].reshape(1, -1, 3) if visible.any() else rgba[..., :3])
        reduced = source.quantize(colors=int(palette), method=Image.Quantize.MEDIANCUT, dither=Image.Dither.NONE)
        used = sorted(set(np.asarray(reduced).reshape(-1).tolist()))
        table = np.array(reduced.getpalette()[:768], dtype=np.uint8).reshape(-1, 3)[used]
    else:
        table = load_palette(palette)
    colours, index, _ = _colours(np.dstack([rgba[..., :3], np.full(rgba.shape[:2], 255, dtype=np.uint8)]))
    nearest, _ = nearest_in_palette(colours[:, :3], table)
    result = rgba.copy()
    if dither and len(table) > 1:
        # The second nearest, and how far the colour lies from the first towards it.
        work = colours[:, :3].astype(np.float64)
        first = table[nearest].astype(np.float64)
        cost = ((work[:, None, :] - table[None, :, :].astype(np.float64)) ** 2 * [0.30, 0.59, 0.11]).sum(axis=2)
        cost[np.arange(len(work)), nearest] = np.inf
        second_index = cost.argmin(axis=1)
        second = table[second_index].astype(np.float64)
        span = ((second - first) ** 2).sum(axis=1)
        along = np.where(span > 0, ((work - first) * (second - first)).sum(axis=1) / np.maximum(span, 1e-9), 0.0)
        bayer = np.array([[0, 8, 2, 10], [12, 4, 14, 6], [3, 11, 1, 9], [15, 7, 13, 5]]) / 16.0 + 1 / 32.0
        height, width = rgba.shape[:2]
        threshold = np.tile(bayer, (height // 4 + 1, width // 4 + 1))[:height, :width]
        choose_second = np.clip(along, 0, 1)[index] > threshold      # at the midpoint, every other texel
        result[..., :3] = np.where(choose_second[..., None], table[second_index][index], table[nearest][index])
    else:
        result[..., :3] = table[nearest][index]
    return _like(result, pixels)


# ------------------------------------------------------------------ normal maps

def normal_from_height(height, strength=1.0, wrap=True):
    """A tangent-space normal map from a height field (2D array, white high; or any image, by
    its luminance), in the convention the engine's guide and glTF use: red is the slope to the
    right, green the slope *up the image*, blue points at the viewer, every vector of unit
    length, flat is (128, 128, 255). `strength` scales the relief: 1 treats a full black-to-white
    step between neighbouring texels as a 45 degree slope. `wrap=True` takes differences around
    the edges, so a tileable height gives a tileable normal map; pass False for atlases.
    Returns (h, w, 3) uint8. Store it as it is: never gamma-correct or resize it carelessly."""
    array = np.asarray(height) if not isinstance(height, (str, os.PathLike, Image.Image)) else as_rgba(height)
    if array.ndim == 3:
        rgb = array[..., :3].astype(np.float64)
        array = rgb @ [0.2126, 0.7152, 0.0722]
    field = array.astype(np.float64)
    if np.asarray(array).dtype == np.uint8 or field.max() > 1.5:
        field = field / 255.0
    if wrap:
        right = (np.roll(field, -1, axis=1) - np.roll(field, 1, axis=1)) / 2
        down = (np.roll(field, -1, axis=0) - np.roll(field, 1, axis=0)) / 2
    else:
        down, right = np.gradient(field)
    x = -right * strength          # a slope rising to the right leans the normal to the left
    y = down * strength            # a slope rising down the image leans the normal up the image
    z = np.ones_like(field)
    length = np.sqrt(x * x + y * y + z * z)
    normal = np.dstack([x / length, y / length, z / length])
    return np.clip(normal * 127.5 + 127.5 + 0.5, 0, 255).astype(np.uint8)


def flip_green(pixels):
    """Turn a normal map between "green up" (glTF, and what the engine reads on models and
    sprites alike) and "green down" (DirectX style), for a map that came from a tool of the
    other convention."""
    rgba = as_rgba(pixels)
    rgba[..., 1] = 255 - rgba[..., 1]
    return _like(rgba, pixels)


def renormalize(pixels):
    """Make every vector of a normal map unit length again (after painting on it, scaling it or
    blending two maps). Vectors pointing away from the viewer are turned to face it."""
    rgba = as_rgba(pixels)
    vector = rgba[..., :3].astype(np.float64) / 127.5 - 1.0
    vector[..., 2] = np.abs(vector[..., 2])
    length = np.linalg.norm(vector, axis=2, keepdims=True)
    vector = np.where(length > 1e-6, vector / np.maximum(length, 1e-6), [0.0, 0.0, 1.0])
    rgba[..., :3] = np.clip(vector * 127.5 + 127.5 + 0.5, 0, 255).astype(np.uint8)
    return _like(rgba, pixels)


# ------------------------------------------------------------------ sizes and mips

def pad_pow2(pixels, mode="edge"):
    """Pad the right and bottom of an image up to the next powers of two: `mode` "edge" repeats
    the last texel (for an atlas: UVs must then be scaled by old size / new size), "transparent"
    adds clear texels with the edge colour bled under them, "wrap" repeats the image. Returns
    (padded, (u_scale, v_scale)): multiply the old UVs by the scale. Never use this on a tiling
    texture: a tile must be a power of two by construction."""
    rgba = as_rgba(pixels)
    height, width = rgba.shape[:2]
    new_width, new_height = 1 << max(0, (width - 1).bit_length()), 1 << max(0, (height - 1).bit_length())
    pad = ((0, new_height - height), (0, new_width - width), (0, 0))
    if mode == "wrap":
        result = np.pad(rgba, pad, mode="wrap")
    elif mode == "transparent":
        result = np.pad(rgba, pad, mode="edge")
        result[height:, :, 3] = 0
        result[:, width:, 3] = 0
    else:
        result = np.pad(rgba, pad, mode="edge")
    return _like(result, pixels), (width / new_width, height / new_height)


def _srgb_to_linear(values):
    c = values / 255.0
    return np.where(c <= 0.04045, c / 12.92, ((c + 0.055) / 1.055) ** 2.4)


def _linear_to_srgb(values):
    c = np.clip(values, 0, 1)
    return np.clip(np.where(c <= 0.0031308, c * 12.92, 1.055 * c ** (1 / 2.4) - 0.055) * 255 + 0.5, 0, 255).astype(np.uint8)


def halve(pixels, srgb=True):
    """The next mip level as the engine makes it (Image::halved): a 2x2 box, colour averaged in
    linear light when `srgb` (colour and emissive maps; False for normal maps) and weighted by
    alpha, so the colour under transparent texels does not darken the edges; alpha a plain mean."""
    rgba = as_rgba(pixels)
    height, width = rgba.shape[:2]
    out_height, out_width = max(1, height // 2), max(1, width // 2)
    ys = np.minimum(np.arange(out_height)[:, None] * 2 + np.array([0, 1])[None, :], height - 1)   # (oh, 2)
    xs = np.minimum(np.arange(out_width)[:, None] * 2 + np.array([0, 1])[None, :], width - 1)     # (ow, 2)
    block = rgba[ys[:, None, :, None], xs[None, :, None, :]].reshape(out_height, out_width, 4, 4).astype(np.float64)
    alpha = block[..., 3] / 255.0
    total = alpha.sum(axis=2)
    colour = _srgb_to_linear(block[..., :3]) if srgb else block[..., :3] / 255.0
    weighted = (colour * alpha[..., None]).sum(axis=2) / np.maximum(total, 1e-9)[..., None]
    plain = colour.mean(axis=2)
    mean = np.where((total > 0)[..., None], weighted, plain)
    out = np.zeros((out_height, out_width, 4), dtype=np.uint8)
    out[..., :3] = _linear_to_srgb(mean) if srgb else np.clip(mean * 255 + 0.5, 0, 255).astype(np.uint8)
    out[..., 3] = np.clip(total * 0.25 * 255 + 0.5, 0, 255).astype(np.uint8)
    return out


def mip_chain(pixels, srgb=True):
    """Every level from the image itself down to 1x1, as the engine builds them for textures
    that are sampled with mips (world textures and backdrops). Look at the small ones: they are
    what the texture is at a distance."""
    levels = [as_rgba(pixels)]
    while levels[-1].shape[0] > 1 or levels[-1].shape[1] > 1:
        levels.append(halve(levels[-1], srgb))
    return levels


def replace_glb_image(path, name, pixels):
    """Write `pixels` (or ready PNG/JPEG bytes) over the image called `name` inside a .glb,
    in place: for repairing an embedded texture without rebuilding the model. Every other
    byte of the model's data is kept; buffer views are laid out again one after another."""
    import json
    import struct
    data = pixels if isinstance(pixels, (bytes, bytearray)) else encode_png(pixels)
    with open(path, "rb") as file:
        blob = file.read()
    if blob[:4] != b"glTF":
        raise ValueError(f"{path} is not a .glb")
    length, kind = struct.unpack_from("<II", blob, 12)
    if kind != 0x4E4F534A:
        raise ValueError(f"{path}: the first chunk is not JSON")
    document = json.loads(blob[20:20 + length])
    start = 20 + length
    binary_length, binary_kind = struct.unpack_from("<II", blob, start)
    if binary_kind != 0x004E4942:
        raise ValueError(f"{path}: no binary chunk")
    binary = blob[start + 8:start + 8 + binary_length]
    images = [image for image in document.get("images", []) if image.get("name") == name and "bufferView" in image]
    if len(images) != 1:
        raise ValueError(f"{path}: {len(images)} embedded images are called '{name}'")
    target = images[0]["bufferView"]
    views = document["bufferViews"]
    if any(view.get("buffer", 0) != 0 for view in views):
        raise ValueError(f"{path}: more than one buffer")
    out = bytearray()
    for index in sorted(range(len(views)), key=lambda i: views[i].get("byteOffset", 0)):
        view = views[index]
        offset = view.get("byteOffset", 0)
        content = data if index == target else binary[offset:offset + view["byteLength"]]
        out += b"\0" * (-len(out) % 4)
        view["byteOffset"], view["byteLength"] = len(out), len(content)
        out += content
    out += b"\0" * (-len(out) % 4)
    document["buffers"][0]["byteLength"] = len(out)
    if data[:4] == b"\x89PNG":
        images[0]["mimeType"] = "image/png"
    text = json.dumps(document, separators=(",", ":")).encode("utf-8")
    text += b" " * (-len(text) % 4)
    with open(path, "wb") as file:
        file.write(b"glTF" + struct.pack("<II", 2, 12 + 8 + len(text) + 8 + len(out)))
        file.write(struct.pack("<II", len(text), 0x4E4F534A) + text)
        file.write(struct.pack("<II", len(out), 0x004E4942) + bytes(out))
