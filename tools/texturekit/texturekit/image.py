"""Read a texture the way the engine does, and what the file says about itself.

A texture is addressed as a file (`ui/panel.png`, `maps/ruins/grade.cube`) or
as an image embedded in a glTF model (`models/colossus.glb#colossus_albedo`).
Either way the result is a `Texture`: the 8-bit RGBA pixels the engine's
decoder (stb_image, `engine/assets/Image.cpp`) hands to the GPU, the facts of
the encoding that decoder ignores (bit depth, interlacing, colour-management
and text chunks), and how the game uses it (the glTF materials that sample
it: base colour, normal or emissive, their alpha mode and sampler).

numpy and pillow only. The glTF reader is modelkit's.
"""
import io
import os
import struct
import sys

import numpy as np
from PIL import Image

sys.path.insert(0, os.path.join(os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__)))), "modelkit"))
from modelkit import gltf  # noqa: E402

Image.MAX_IMAGE_PIXELS = 16384 * 16384  # the engine's own limit; anything larger is refused there too

IMAGE_EXTENSIONS = (".png", ".jpg", ".jpeg", ".tga", ".bmp", ".gif", ".psd", ".hdr", ".pic", ".ppm", ".pgm", ".webp", ".tif", ".tiff",
                    ".dds", ".exr")
MODEL_EXTENSIONS = (".glb", ".gltf")
LUT_EXTENSION = ".cube"

# What stb_image decodes (each tried with fengine_headless and fe.app.image_size), by Pillow's name for the format.
ENGINE_DECODES = {"PNG", "JPEG", "TGA", "BMP", "GIF", "PSD", "HDR", "PPM"}
# What the engine's asset cache (engine/core/AssetCache.cpp) and asset catalog (engine/editor/AssetCatalog.cpp) take for an image.
ENGINE_EXTENSIONS = (".png", ".jpg", ".jpeg", ".tga")
FORMAT_EXTENSIONS = {"PNG": (".png",), "JPEG": (".jpg", ".jpeg"), "TGA": (".tga",)}

COLOUR, NORMAL, EMISSIVE, LUT = "colour", "normal", "emissive", "lut"
SLOTS = {COLOUR: "base colour", NORMAL: "normal map", EMISSIVE: "emissive"}


class Texture:
    """One texture, decoded. See the module docstring."""

    def __init__(self, path):
        self.path = path            # as it was addressed
        self.container = path       # the file on disk: itself, or the .glb that embeds it
        self.embedded = False
        self.name = os.path.splitext(os.path.basename(path))[0]
        self.named = True           # False: the glTF gave the image no name
        self.data = b""             # the encoded bytes
        self.format = None          # Pillow's name: PNG, JPEG, TGA...; CUBE for a LUT
        self.mode = ""              # Pillow's mode of the stored image: RGB, RGBA, P, L, I;16, CMYK...
        self.width = self.height = 0
        self.rgba = None            # (height, width, 4) uint8: what the engine uploads
        self.bit_depth = 8
        self.interlaced = False
        self.progressive = False
        self.stores_alpha = False   # the file carries an alpha channel (or a transparent palette entry)
        self.chunks = {}            # PNG: chunk type -> bytes of payload; JPEG: "EXIF", "ICC", "COM"
        self.gamma = None           # PNG gAMA as a display gamma (2.2 for the usual 45455), else None
        self.role = COLOUR
        self.role_from = "by default"
        self.uses = []              # how glTF materials sample it: dicts, see model_uses()
        self.partner = None         # for a normal or emissive map: (name, width, height) of the colour map of the same material
        self.lut = None             # for a .cube: Lut

    @property
    def is_lut(self):
        return self.lut is not None

    @property
    def alpha(self):
        return self.rgba[..., 3]

    @property
    def label(self):
        return self.path.replace(os.sep, "/")


class Lut:
    """A .cube file as the engine's readCube (engine/render/GpuAssets.cpp) reads it."""

    def __init__(self):
        self.size = 0
        self.table = None       # (size, size, size, 3) float, indexed [blue, green, red]
        self.rows = 0
        self.problems = []      # why the engine would refuse it (it then grades with the identity)
        self.domain = None      # (min, max) when the file declares one that is not 0..1; the engine ignores it
        self.one_dimensional = False


# ------------------------------------------------------------------ files

def split(path):
    """('models/colossus.glb', 'colossus_albedo') for an embedded texture; (path, None) for a file."""
    if "#" in path:
        container, name = path.split("#", 1)
        if container.lower().endswith(MODEL_EXTENSIONS):
            return container, name
    return path, None


def is_image_name(filename):
    return filename.lower().endswith(IMAGE_EXTENSIONS)


def png_chunks(data):
    """[(type, payload)] of a PNG's chunks, in file order (stops quietly where the file is cut short)."""
    out, offset = [], 8
    while offset + 8 <= len(data):
        length, = struct.unpack(">I", data[offset:offset + 4])
        kind = data[offset + 4:offset + 8].decode("latin-1")
        out.append((kind, data[offset + 8:offset + 8 + length]))
        offset += 12 + length
        if kind == "IEND":
            break
    return out


def decode(data, texture=None):
    """Fill a Texture from encoded image bytes. Raises ValueError when Pillow cannot read them."""
    texture = texture or Texture("")
    texture.data = data
    try:
        image = Image.open(io.BytesIO(data))
        image.load()
    except Exception as error:  # Pillow raises OSError, SyntaxError, ValueError, struct.error... for damaged files
        raise ValueError(f"not a readable image ({error})") from error
    texture.format = image.format
    texture.mode = image.mode
    texture.width, texture.height = image.size
    texture.stores_alpha = image.mode in ("RGBA", "LA", "PA", "RGBa", "La") or "transparency" in image.info
    if image.format == "PNG":
        for kind, payload in png_chunks(data):
            texture.chunks[kind] = texture.chunks.get(kind, 0) + len(payload)
            if kind == "IHDR" and len(payload) >= 13:
                texture.bit_depth = payload[8]
                texture.interlaced = payload[12] != 0
            elif kind == "gAMA" and len(payload) == 4:
                value = struct.unpack(">I", payload)[0]
                texture.gamma = 100000.0 / value if value else None
    elif image.format == "JPEG":
        texture.progressive = bool(image.info.get("progressive") or image.info.get("progression"))
        if image.info.get("exif"):
            texture.chunks["EXIF"] = len(image.info["exif"])
        if image.info.get("icc_profile"):
            texture.chunks["ICC"] = len(image.info["icc_profile"])
        if image.info.get("comment"):
            texture.chunks["COM"] = len(image.info["comment"])
    # The pixels as stb_image returns them: 8 bits, four channels; 16-bit samples keep their high byte.
    if image.mode in ("I;16", "I;16B", "I;16L", "I"):
        grey = (np.asarray(image).astype(np.uint32) >> 8).clip(0, 255).astype(np.uint8)
        texture.bit_depth = max(texture.bit_depth, 16)
        rgba = np.dstack([grey, grey, grey, np.full_like(grey, 255)])
    elif image.mode == "F":
        grey = (np.clip(np.asarray(image), 0, 1) * 255).astype(np.uint8)
        rgba = np.dstack([grey, grey, grey, np.full_like(grey, 255)])
    else:
        rgba = np.asarray(image.convert("RGBA"))
    texture.rgba = np.ascontiguousarray(rgba, dtype=np.uint8)
    return texture


def read_cube(text):
    """Parse a .cube exactly as the engine does: comments, TITLE and DOMAIN_* lines are skipped,
    LUT_3D_SIZE gives the size (2 to 64), every other line must start with a number and is a
    table row when it holds three; the table must be size cubed rows, red fastest."""
    lut = Lut()
    rows, low, high = [], None, None
    for number, line in enumerate(text.splitlines(), 1):
        tokens = line.split()
        if not tokens or tokens[0].startswith("#") or tokens[0] == "TITLE":
            continue
        if tokens[0] in ("DOMAIN_MIN", "DOMAIN_MAX"):
            try:
                values = tuple(float(v) for v in tokens[1:4])
            except ValueError:
                values = ()
            if tokens[0] == "DOMAIN_MIN":
                low = values
            else:
                high = values
            continue
        if tokens[0] == "LUT_3D_SIZE":
            try:
                lut.size = int(tokens[1])
            except (IndexError, ValueError):
                lut.problems.append(f"line {number}: LUT_3D_SIZE without a number")
            continue
        try:
            first = float(tokens[0])
        except ValueError:
            if tokens[0] == "LUT_1D_SIZE":
                lut.one_dimensional = True
            lut.problems.append(f"line {number}: '{tokens[0]}' is not something the engine's reader knows (it stops there)")
            break
        try:
            rows.append((first, float(tokens[1]), float(tokens[2])))
        except (IndexError, ValueError):
            continue  # the engine drops a row of fewer than three numbers; the count below then comes up short
    lut.rows = len(rows)
    if (low and any(abs(v) > 1e-6 for v in low)) or (high and any(abs(v - 1) > 1e-6 for v in high)):
        lut.domain = (low, high)
    if not lut.problems:
        if not 2 <= lut.size <= 64:
            lut.problems.append(f"LUT_3D_SIZE is {lut.size}; the engine reads 2 to 64")
        elif len(rows) != lut.size ** 3:
            lut.problems.append(f"{len(rows):,} rows of three numbers; a size of {lut.size} needs {lut.size ** 3:,}")
    if not lut.problems:
        lut.table = np.array(rows, dtype=np.float64).reshape(lut.size, lut.size, lut.size, 3)
    return lut


def lut_apply(lut, rgb):
    """Colours (..., 3) in 0..1 through the LUT as the engine applies it: the table resampled to 16
    cubed 8-bit entries (trilinear, truncated to bytes), then sampled trilinearly."""
    table = lut_16(lut).astype(np.float64) / 255.0
    return _trilinear(table, np.clip(rgb, 0, 1) * 15.0)


def lut_16(lut):
    """The 16x16x16 RGB8 texture the engine builds from the file, indexed [blue, green, red]."""
    axis = np.arange(16) / 15.0 * (lut.size - 1)
    b, g, r = np.meshgrid(axis, axis, axis, indexing="ij")
    sampled = _trilinear(lut.table, np.stack([r, g, b], axis=-1))
    return (np.clip(sampled, 0, 1) * 255).astype(np.uint8)


def _trilinear(table, position):
    """table[blue, green, red] sampled at (..., 3) positions given as (red, green, blue) lattice coordinates."""
    size = table.shape[0]
    low = np.clip(np.floor(position).astype(np.int64), 0, size - 1)
    high = np.minimum(low + 1, size - 1)
    f = np.clip(position - low, 0, 1)
    out = 0
    for dz, wz in ((low[..., 2], 1 - f[..., 2]), (high[..., 2], f[..., 2])):
        for dy, wy in ((low[..., 1], 1 - f[..., 1]), (high[..., 1], f[..., 1])):
            for dx, wx in ((low[..., 0], 1 - f[..., 0]), (high[..., 0], f[..., 0])):
                out = out + table[dz, dy, dx] * (wz * wy * wx)[..., None]
    return out


# ------------------------------------------------------------------ glTF

_MODELS = {}


def model(path):
    """modelkit's reading of a .glb or .gltf, cached while the file does not change."""
    key = os.path.abspath(path)
    stamp = (os.path.getmtime(key), os.path.getsize(key))
    cached = _MODELS.get(key)
    if cached is None or cached[0] != stamp:
        try:
            cached = (stamp, gltf.load(key))
        except gltf.GltfError as error:
            raise ValueError(f"{os.path.basename(path)}: {error}") from error
        _MODELS[key] = cached
    return cached[1]


def image_names(loaded):
    """The name each image of a model is addressed by after '#': its glTF name, made unique
    with its index where two share one (or have none)."""
    raw = loaded.json.get("images", [])
    names = [image.get("name") or f"image{index}" for index, image in enumerate(raw)]
    return [name if names.count(name) == 1 else f"{name}.{index}" for index, name in enumerate(names)]


def embedded(path):
    """[(name, index)] of the images a model carries inside itself (not the files it points at)."""
    loaded = model(path)
    names = image_names(loaded)
    out = []
    for index, image in enumerate(loaded.json.get("images", [])):
        if "bufferView" in image or str(image.get("uri", "")).startswith("data:"):
            out.append((names[index], index))
    return out


def model_uses(loaded):
    """{image index: [use]} for one model: which material samples the image as what. A use is
    {"model", "model_path", "material", "material_index", "role", "alpha_mode", "sampler", "drawn",
    "colour_image"}; `sampler` is
    the glTF sampler dict ({} when the texture names none: the engine then filters linearly, with
    mips, repeating); `drawn` is whether any mesh of the scene uses the material."""
    drawn = {primitive.material for primitive in loaded.primitives}
    samplers = loaded.json.get("samplers", [])
    uses = {}

    def source(reference):
        if not reference or reference.get("index") is None or reference["index"] >= len(loaded.textures):
            return None, {}
        texture = loaded.textures[reference["index"]]
        sampler = samplers[texture["sampler"]] if texture.get("sampler") is not None and texture["sampler"] < len(samplers) else {}
        return texture.get("source"), sampler

    for index, material in enumerate(loaded.materials):
        slots = [(COLOUR, material.get("pbrMetallicRoughness", {}).get("baseColorTexture")),
                 (NORMAL, material.get("normalTexture")), (EMISSIVE, material.get("emissiveTexture"))]
        colour_image, _ = source(slots[0][1])
        for role, reference in slots:
            image, sampler = source(reference)
            if image is None:
                continue
            uses.setdefault(image, []).append({
                "model": os.path.basename(loaded.path), "model_path": loaded.path, "material_index": index,
                "material": loaded.material_name(index), "role": role,
                "alpha_mode": material.get("alphaMode", "OPAQUE"), "sampler": sampler, "drawn": index in drawn,
                "colour_image": colour_image})
    return uses


def use_has_tangents(use):
    """Whether the meshes that draw a use's material carry a TANGENT attribute (True), leave
    the engine to derive tangents from the UVs (False), or no mesh draws it (None)."""
    parts = [primitive for primitive in model(use["model_path"]).primitives if primitive.material == use["material_index"]]
    if not parts:
        return None
    return all(primitive.tangents is not None for primitive in parts)


def _from_model(texture, loaded, index):
    """Role, uses and partner of image `index` of a model."""
    names = image_names(loaded)
    texture.uses = texture.uses + model_uses(loaded).get(index, [])
    roles = {use["role"] for use in texture.uses}
    if roles:
        # A texture used as a normal map anywhere is a normal map; emissive wins over colour only when it is never the colour.
        texture.role = NORMAL if NORMAL in roles else COLOUR if COLOUR in roles else EMISSIVE
        texture.role_from = f"material '{texture.uses[0]['material']}' uses it as its {SLOTS[texture.role]}"
    for use in texture.uses:
        partner = use["colour_image"]
        if use["role"] != COLOUR and partner is not None and partner != index and partner < len(loaded.images):
            other = loaded.images[partner]
            if other["width"]:
                texture.partner = (names[partner], other["width"], other["height"])
                break


def _role_from_name(texture, profile):
    for role, suffixes in (profile.get("role_suffixes") or {}).items():
        if any(texture.name.lower().endswith(suffix) for suffix in suffixes):
            texture.role, texture.role_from = role, f"its name ends like a {SLOTS.get(role, role)} ('{texture.name}')"
            return


def load(path, profile=None, models=()):
    """The Texture at `path` (a file, or `model.glb#image`). `models` are glTF files that may
    reference a loose file (the game's; by default those in the file's own folder), so its role
    and sampler are known. Raises ValueError/OSError when it cannot be read."""
    profile = profile or {}
    container, inner = split(path)
    texture = Texture(path)
    texture.container = container
    if inner is not None:
        loaded = model(container)
        names = image_names(loaded)
        if inner not in names:
            raise ValueError(f"{os.path.basename(container)} has no image named '{inner}' (it has: {', '.join(names) or 'none'})")
        index = names.index(inner)
        entry = loaded.images[index]
        if entry["data"] is None:
            raise ValueError(f"image '{inner}' of {os.path.basename(container)} points at a file that is missing ({entry['uri']})")
        texture.embedded = True
        texture.name = inner
        texture.named = bool(loaded.json["images"][index].get("name"))
        decode(entry["data"], texture)
        _from_model(texture, loaded, index)
    elif container.lower().endswith(LUT_EXTENSION):
        with open(container, "rb") as file:
            texture.data = file.read()
        texture.format, texture.role, texture.role_from = "CUBE", LUT, "a .cube file"
        texture.lut = read_cube(texture.data.decode("utf-8", errors="replace"))
        if texture.lut.table is not None:
            # Laid out for looking at: the blue slices side by side, red to the right, green down.
            strip = np.concatenate(list(lut_16(texture.lut)), axis=1)
            texture.rgba = np.dstack([strip, np.full(strip.shape[:2], 255, dtype=np.uint8)])
            texture.height, texture.width = texture.rgba.shape[:2]
    else:
        with open(container, "rb") as file:
            decode(file.read(), texture)
        _role_from_name(texture, profile)
        target = os.path.abspath(container)
        folder = os.path.dirname(target)
        candidates = list(models) or [os.path.join(folder, name) for name in sorted(os.listdir(folder))
                                      if name.lower().endswith(MODEL_EXTENSIONS)]
        for candidate in candidates:
            try:
                loaded = model(candidate)
            except (ValueError, OSError):
                continue
            base = os.path.dirname(os.path.abspath(candidate))
            for index, image in enumerate(loaded.json.get("images", [])):
                uri = image.get("uri")
                if uri and not uri.startswith("data:") and os.path.abspath(os.path.join(base, uri)) == target:
                    _from_model(texture, loaded, index)
    if profile.get("role"):
        texture.role, texture.role_from = profile["role"], f"the profile '{profile.get('name', '')}' says so"
    return texture
