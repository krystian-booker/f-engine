#!/usr/bin/env python3
"""Proves the validator: every check is shown a small texture with exactly the
flaw it exists for and must name it, clean textures must come back clean, and
each helper must fix what it says it fixes.

    python3 tools/texturekit/selftest.py                        # numpy and pillow
    python3 tools/texturekit/selftest.py --engine build/fengine_headless
                                                                # also ready.engine_load (default: that path, if it exists)
"""
import io
import json
import os
import shutil
import struct
import sys
import tempfile
import zlib

HERE = os.path.dirname(os.path.abspath(__file__))
sys.dont_write_bytecode = True
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.dirname(HERE))

import numpy as np  # noqa: E402
from PIL import Image  # noqa: E402

import fix  # noqa: E402
from kitcommon import kit as common  # noqa: E402
from texturekit import checks, sheet  # noqa: E402
from texturekit import image as ti  # noqa: E402
from texturekit import tools as tk  # noqa: E402
from texturekit.kit import TextureKit  # noqa: E402

gltf = ti.gltf
WORK = tempfile.mkdtemp(prefix="texturekit-selftest-")
FAILURES = []
KIT = TextureKit(HERE)

STONE = np.array([[27, 29, 46], [52, 54, 77], [74, 74, 96], [98, 96, 116], [122, 90, 69], [160, 120, 92]], dtype=np.uint8)


# ------------------------------------------------------------------ fixtures

def bricks(size=64, seed=1, rows=8):
    """A clean pixel-art brick wall: six colours, hard edges, drawn on wrapped coordinates, its
    light and dark bricks spread evenly so no part of it stands out when it repeats."""
    rng = np.random.default_rng(seed)
    out = np.zeros((size, size, 3), dtype=np.uint8)
    course, length = size // rows, size // 4
    for y in range(size):
        row = y // course
        for x in range(size):
            shifted = (x + (length // 2 if row % 2 else 0)) % size
            inside_x, inside_y = shifted % length, y % course
            if inside_y == 0 or inside_x == 0:
                out[y, x] = STONE[0]                                    # mortar
            elif inside_y == 1 and inside_x < length - 1:
                out[y, x] = STONE[5] if inside_x == 2 else STONE[4]     # a lit top edge with a glint
            else:
                out[y, x] = STONE[2 + (row + shifted // length) % 2]
    speck = rng.random((size, size)) < 0.06
    face = (out == STONE[2]).all(axis=2) | (out == STONE[3]).all(axis=2)
    out[speck & face] = STONE[1]
    return out


def height_field(size=64):
    """A tileable relief: rounded studs on a plate, with diagonal slopes."""
    y, x = np.mgrid[0:size, 0:size]
    return (np.sin(x / size * 4 * np.pi) * np.sin(y / size * 4 * np.pi) + 1) / 2 + 0.2 * np.cos((x + y) / size * 2 * np.pi)


def glow(size=64, colour=(155, 92, 255), radius=0.42):
    """A soft straight-alpha glow with the colour carried under the transparency."""
    y, x = np.mgrid[0:size, 0:size]
    distance = np.hypot(x - size / 2 + 0.5, y - size / 2 + 0.5) / size
    alpha = np.clip(1 - distance / radius, 0, 1) ** 2
    out = np.zeros((size, size, 4), dtype=np.uint8)
    out[..., :3] = colour
    out[..., 3] = (alpha * 255).astype(np.uint8)
    return out


def icon(size=32, colour=(232, 196, 110), hidden=None):
    """A hard-edged arrow with a clear margin; `hidden` is the colour under alpha 0 (default:
    the edge colours carried outwards, as a clean file has them)."""
    if hidden is None:
        return tk.bleed_edges(icon(size, colour, (0, 0, 0)), None)
    out = np.zeros((size, size, 4), dtype=np.uint8)
    out[..., :3] = hidden
    for y in range(4, size - 4):
        half = min(y - 4, size - 5 - y)
        out[y, 4:6 + half, :3] = colour
        out[y, 4:6 + half, 3] = 255
        out[y, 4, :3] = (60, 44, 20)
    return out


def panel(size=48, border=12, scanlines=False):
    """A nine-slice panel: dark glass with a gold line; the stretched parts are constant along the stretch."""
    out = np.zeros((size, size, 4), dtype=np.uint8)
    y = np.arange(size)[:, None] * np.ones((1, size))
    out[..., 0], out[..., 1], out[..., 2], out[..., 3] = 18, 20, 44 - y / size * 20, 230
    out[3, 3:-3], out[-4, 3:-3], out[3:-3, 3], out[3:-3, -4] = (201, 162, 74, 255), (201, 162, 74, 255), (201, 162, 74, 255), (201, 162, 74, 255)
    if scanlines:
        out[border:-border:3, border:-border, :3] = (90, 60, 160)
    return out


def png_bytes(pixels, **options):
    data = io.BytesIO()
    image = pixels if isinstance(pixels, Image.Image) else Image.fromarray(np.asarray(pixels))
    image.save(data, "PNG", **options)
    return data.getvalue()


def with_chunk(data, kind, payload):
    """A PNG with one more chunk after IHDR."""
    chunk = struct.pack(">I", len(payload)) + kind + payload + struct.pack(">I", zlib.crc32(kind + payload) & 0xFFFFFFFF)
    return data[:33] + chunk + data[33:]


def write(name, data):
    path = os.path.join(WORK, name)
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "wb") as out:
        out.write(data if isinstance(data, (bytes, bytearray)) else tk.encode_png(data))
    return path


QUAD = {"positions": np.array([[0, 0, 0], [1, 0, 0], [1, 1, 0], [0, 1, 0]], float), "normals": np.tile([0.0, 0, 1], (4, 1)),
        "uvs": np.array([[0, 1], [1, 1], [1, 0], [0, 0]], float), "indices": np.array([[0, 1, 2], [0, 2, 3]]), "material": 0}
PIXEL_SAMPLER = {"magFilter": 9728, "minFilter": 9984}


def glb(name, images, material=None, sampler=PIXEL_SAMPLER, names=True, tangents=False, drawn=True):
    """A one-quad model with these images [(name, pixels)] embedded: the first is the base colour
    unless `material` says otherwise ({"normal": 1, "emissive": 2, "alpha": "MASK"})."""
    material = material or {}
    entry = {"name": "surface", "pbrMetallicRoughness": {"baseColorTexture": {"index": material.get("colour", 0)}}}
    if "normal" in material:
        entry["normalTexture"] = {"index": material["normal"]}
    if "emissive" in material:
        entry["emissiveTexture"], entry["emissiveFactor"] = {"index": material["emissive"]}, [1, 1, 1]
    if material.get("alpha"):
        entry["alphaMode"] = material["alpha"]
    path = os.path.join(WORK, name)
    os.makedirs(os.path.dirname(path), exist_ok=True)
    quad = dict(QUAD)
    if not drawn:
        quad["material"] = 1
    encoded = [data if isinstance(data, bytes) else tk.encode_png(data) for _, data in images]
    gltf.write_glb(path, [quad], materials=[entry, {"name": "other"}], images=encoded)
    # Names, samplers and tangents are not write_glb's business: patch the JSON chunk.
    with open(path, "rb") as file:
        raw = file.read()
    length, = struct.unpack("<I", raw[12:16])
    document = json.loads(raw[20:20 + length])
    rest = raw[20 + length:]
    for index, (image_name, _) in enumerate(images):
        if names:
            document["images"][index]["name"] = image_name
        else:
            document["images"][index].pop("name", None)
        if sampler is not None:
            document["textures"][index]["sampler"] = 0
    if sampler is not None:
        document["samplers"] = [sampler]
    if tangents:
        size, = struct.unpack("<I", rest[:4])
        blob = bytearray(rest[8:8 + size])
        document["bufferViews"].append({"buffer": 0, "byteOffset": len(blob), "byteLength": 64})
        blob.extend(np.tile(np.array([1, 0, 0, 1], np.float32), (4, 1)).tobytes())
        document["accessors"].append({"bufferView": len(document["bufferViews"]) - 1, "componentType": 5126, "count": 4, "type": "VEC4"})
        document["meshes"][0]["primitives"][0]["attributes"]["TANGENT"] = len(document["accessors"]) - 1
        document["buffers"] = [{"byteLength": len(blob)}]
        rest = struct.pack("<II", len(blob), 0x004E4942) + bytes(blob)
    text = json.dumps(document).encode("utf-8")
    text += b" " * (-len(text) % 4)
    with open(path, "wb") as out:
        out.write(b"glTF" + struct.pack("<II", 2, 12 + 8 + len(text) + len(rest)) + struct.pack("<II", len(text), 0x4E4F534A) + text + rest)
    return path


CUBE_HEAD = 'TITLE "test"\nLUT_3D_SIZE {size}\n'


def cube(name, size=16, grade=lambda rgb: rgb ** 0.9 * [1.0, 0.97, 0.92], head=CUBE_HEAD, rows=None):
    axis = np.arange(size) / (size - 1)
    b, g, r = np.meshgrid(axis, axis, axis, indexing="ij")
    table = grade(np.stack([r, g, b], axis=-1)).reshape(-1, 3)
    lines = [f"{row[0]:.5f} {row[1]:.5f} {row[2]:.5f}" for row in table[:rows]]
    return write(name, (head.format(size=size) + "\n".join(lines) + "\n").encode("utf-8"))


# ------------------------------------------------------------------ the harness

def report(name, problems):
    print(f"  {name:<66} {'ok' if not problems else 'FAILED: ' + '; '.join(problems)}")
    if problems:
        FAILURES.append(name)


def case(name, path, profile="default", expect=(), absent=(), clean=False, overrides=None, engine=None, kit=KIT, info=()):
    """Validate `path` and check that every id in `expect` was reported (and those in
    `absent` were not; `clean`: nothing at all; `info`: ids reported as observations)."""
    settings = common.profile_named(kit.profiles(), profile, overrides)
    findings, _, subject = kit.validate(path, settings, engine=engine)
    found = {f.check for f in findings if f.severity != "info"}
    noted = {f.check for f in findings if f.severity == "info"}
    problems = [f"missed {check}" for check in expect if check not in found]
    problems += [f"did not note {check}" for check in info if check not in noted]
    problems += [f"reported {check}" for check in absent if check in found]
    if clean and found:
        problems.append("reported " + ", ".join(sorted(found)))
    report(name, problems)
    return findings, subject


def check(name, condition, detail=""):
    report(name, [] if condition else [detail or "not so"])


def ids(pixels, profile, overrides=None, name="probe.png"):
    """The check ids (not info) of some pixels under a profile: for the helper proofs."""
    settings = common.profile_named(KIT.profiles(), profile, overrides)
    findings, _, _ = KIT.validate(write("probe/" + name, pixels), settings)
    return {f.check for f in findings if f.severity != "info"}


def main():
    engine = None
    if "--engine" in sys.argv:
        engine = sys.argv[sys.argv.index("--engine") + 1]
    elif os.path.isfile(os.path.join(HERE, "..", "..", "build", "fengine_headless")):
        engine = os.path.normpath(os.path.join(HERE, "..", "..", "build", "fengine_headless"))
    rng = np.random.default_rng(7)
    wall = bricks()

    print("clean textures are clean")
    case("a tiling pixel-art wall", write("wall.png", wall), "tile", clean=True)
    case("the same wall embedded in a model, sampled nearest with mips",
         glb("wall.glb", [("wall", wall)]) + "#wall", "tile", clean=True)
    normal = tk.normal_from_height(height_field(), 3.0)
    flat_emissive = np.zeros((64, 64, 3), dtype=np.uint8)
    flat_emissive[20:24, 8:56] = (155, 92, 255)
    model = glb("set.glb", [("wall", wall), ("wall_n", tk.flip_green(normal)), ("wall_e", flat_emissive)], {"normal": 1, "emissive": 2})
    case("its normal map (green down: the mesh has no tangents)", model + "#wall_n", "tile", clean=True)
    case("its emissive map", model + "#wall_e", "tile", clean=True)
    tangent_model = glb("tangents.glb", [("wall", wall), ("wall_n", normal)], {"normal": 1}, tangents=True)
    case("a green-up normal map on a mesh with tangents", tangent_model + "#wall_n", "tile", clean=True)
    trim = np.concatenate([wall[:32], np.roll(wall[:16], 5, axis=1), wall[:16, ::-1][:, ::1]], axis=0)
    trim[40:44] = STONE[4]
    case("a trim sheet (bands that repeat along U only)", write("trim.png", trim), "trim", clean=True)
    atlas = np.zeros((64, 64, 3), dtype=np.uint8) + STONE[1]
    atlas[2:40, 2:62] = wall[:38, :60]
    atlas[42:62, 2:30] = STONE[4]
    atlas[44:60, 4:28:2] = STONE[5]
    case("an atlas", write("atlas.png", atlas), "atlas", clean=True)
    case("a soft glow, colour carried under the transparency", write("fx_glow.png", glow()), "effect", clean=True)
    case("a UI icon with a clear margin", write("cursor.png", icon()), "ui", clean=True)
    case("a nine-slice panel", write("panel.png", panel()), "ui", overrides={"nine_slice": 12}, clean=True)
    sheet_cells = np.zeros((64, 128, 4), dtype=np.uint8)
    for cell in range(4):
        sheet_cells[:32, cell * 32:(cell + 1) * 32] = icon()
        sheet_cells[32:, cell * 32:(cell + 1) * 32] = icon(colour=(120, 200, 220))
    case("a sheet of glyphs in 32 px cells", write("glyphs.png", sheet_cells), "ui", overrides={"cell": [32, 32]}, clean=True)
    ridge = np.zeros((48, 192, 4), dtype=np.uint8)
    ridge[..., :3] = (39, 48, 90)
    for x in range(192):
        ridge[int(20 + 8 * np.sin(x / 9.0) + 4 * np.sin(x / 3.1)):, x, 3] = 255
    ridge[36:, :, :3] = (12, 16, 36)
    case("a sky card: a mask with the colour bled under it", write("sky_ridge.png", ridge), "backdrop", clean=True)
    case("a colour grade", cube("grade.cube"), "lut", clean=True)

    print("false alarms that stay fixed")
    case("a backdrop named sky_glow is not an emissive map", write("sky_glow.png", glow(colour=(120, 80, 220))), "backdrop", clean=True)
    case("a brick wall whose wrap falls on a joint is not a seam", write("joint.png", np.roll(wall, 1, axis=1)), "tile", clean=True)
    smoke = glow(colour=(34, 30, 40), radius=0.45)
    case("dark smoke on straight alpha is not premultiplied", write("smoke.png", smoke), "effect", clean=True)
    shadow = glow(colour=(0, 0, 0))
    case("a black blob shadow is neither premultiplied nor a fringe", write("shadow.png", shadow), "effect",
         absent=["alpha.premultiplied", "alpha.fringe"])
    soft_e = np.zeros((64, 64, 3), dtype=np.uint8)
    soft_e[...] = (glow()[..., 3:4] / 255.0 * [155, 92, 255]).astype(np.uint8)
    soft_model = glb("soft.glb", [("wall", wall), ("wall_e", soft_e)], {"emissive": 1})
    case("an emissive map may hold a glow's falloff", soft_model + "#wall_e", "tile", clean=True)
    small = np.zeros((32, 64, 3), dtype=np.uint8)
    small[2:12, 2:20] = wall[:10, :18]
    case("a small atlas that is mostly empty wastes nothing worth a warning", write("shard.png", small), "atlas", absent=["tiling.empty"])
    case("a palette PNG is not a colour-mode problem",
         write("pal.png", png_bytes(Image.fromarray(wall).quantize(colors=8, dither=Image.Dither.NONE))), "tile",
         absent=["file.colour_mode", "file.bit_depth", "file.format"])
    grey = png_bytes(Image.fromarray((wall @ [0.3, 0.6, 0.1]).astype(np.uint8)))
    case("a greyscale PNG decodes like any other", write("grey.png", grey), "default", absent=["file.colour_mode", "file.format"])
    bevel = np.zeros((32, 32, 3), dtype=np.uint8) + [128, 128, 255]
    bevel[:, ::8] = [70, 128, 220]
    bevel[::8, :] = [128, 190, 220]
    bevel_model = glb("bevel.glb", [("wall", wall[:32, :32]), ("wall_n", bevel)], {"normal": 1})
    case("a normal map of straight bevels says nothing about green: no verdict", bevel_model + "#wall_n", "tile",
         absent=["ready.normal_green"])

    print("1. file and encoding")
    webp = io.BytesIO()
    Image.fromarray(wall).save(webp, "WEBP", lossless=True)
    case("a WebP image", write("wall.webp", webp.getvalue()), "tile", ["file.format"])
    bmp = io.BytesIO()
    Image.fromarray(wall).save(bmp, "BMP")
    case("a BMP (decodes, but the engine does not preload it)", write("wall.bmp", bmp.getvalue()), "tile", ["file.format"])
    jpeg = io.BytesIO()
    Image.fromarray(wall).save(jpeg, "JPEG", quality=95)
    case("a JPEG named .png", write("liar.png", jpeg.getvalue()), "backdrop", ["file.format"], overrides={"lossless": False})
    case("pixel art saved as JPEG", write("wall.jpg", jpeg.getvalue()), "tile", ["file.lossy"])
    case("a photographic panorama may be JPEG", write("sky.jpg", jpeg.getvalue()), "backdrop", absent=["file.lossy"],
         overrides={"lossless": False, "pixel_art": False})
    cmyk = io.BytesIO()
    Image.fromarray(wall).convert("CMYK").save(cmyk, "JPEG")
    case("a CMYK JPEG", write("cmyk.jpg", cmyk.getvalue()), "backdrop", ["file.colour_mode"], overrides={"lossless": False})
    deep = png_bytes(Image.fromarray((wall[..., 0].astype(np.uint16) << 8) + 77))
    case("a 16-bit PNG", write("deep.png", deep), "default", ["file.bit_depth"])
    case("an interlaced PNG", write("interlaced.png", with_interlace(wall)), "tile", ["file.interlaced"])
    case("a PNG with an ICC profile", write("icc.png", with_chunk(tk.encode_png(wall), b"iCCP", b"x\x00\x00" + zlib.compress(b"p" * 300))),
         "tile", ["file.colour_profile"])
    case("a PNG with a gamma of 1.0", write("gamma.png", with_chunk(tk.encode_png(wall), b"gAMA", struct.pack(">I", 100000))), "tile",
         ["file.colour_profile"])
    case("the usual gamma chunk (1/2.2) changes nothing", write("gamma22.png", with_chunk(tk.encode_png(wall), b"gAMA", struct.pack(">I", 45455))),
         "tile", clean=True)
    case("a PNG carrying text and a time stamp",
         write("text.png", with_chunk(tk.encode_png(wall), b"tEXt", b"Comment\x00" + b"made with love and a very long prompt " * 4)), "tile",
         ["file.metadata"])
    noisy = np.zeros((128, 128, 3), dtype=np.uint8) + STONE[rng.integers(0, 6, size=(128, 128))]
    bloated = png_bytes(Image.fromarray(noisy), compress_level=0)
    case("a PNG stored without compression", write("bloat.png", bloated), "default", ["file.bloat"])
    case("the same pixels written by save_png", write("lean.png", noisy), "default", absent=["file.bloat"])
    case("a .cube the engine cannot read (rows missing)", cube("short.cube", rows=4000), "lut", ["file.cube"])
    case("a 1D LUT", cube("one.cube", head='TITLE "x"\nLUT_1D_SIZE {size}\n'), "lut", ["file.cube"])
    case("a .cube with a domain the engine ignores", cube("domain.cube", head=CUBE_HEAD + "DOMAIN_MIN 0 0 0\nDOMAIN_MAX 2 2 2\n"), "lut",
         ["file.cube_domain"])

    print("2. alpha and edges")
    nothing = np.zeros((32, 32, 4), dtype=np.uint8)
    nothing[..., :3] = 200
    case("an image that is transparent everywhere", write("nothing.png", png_bytes(nothing)), "ui", ["alpha.empty"])
    opaque = np.dstack([wall, np.full(wall.shape[:2], 255, dtype=np.uint8)])
    case("an alpha channel that is 255 everywhere is noted, not a flaw (the engine uploads RGBA8 anyway)",
         write("opaque.png", png_bytes(opaque)), "tile", clean=True, info=["alpha.unused"])
    holes = opaque.copy()
    holes[10:20, 10:20, 3] = 0
    case("a cutout in a profile that expects none", write("holes.png", holes), "tile", ["alpha.kind"])
    soft = opaque.copy()
    soft[10:20, 10:20, 3] = 128
    case("soft alpha where the profile allows a mask", write("soft.png", soft), "trim", ["alpha.kind"])
    case("a cutout in an OPAQUE material", glb("opaque.glb", [("wall", holes)]) + "#wall", "trim", ["alpha.kind"])
    case("soft alpha in a MASK material", glb("mask.glb", [("wall", soft)], {"alpha": "MASK"}) + "#wall", "overlay", ["alpha.kind"])
    case("a MASK material with nothing cut out", glb("mask2.glb", [("wall", wall)], {"alpha": "MASK"}) + "#wall", "trim", ["alpha.kind"])
    case("a cutout in a MASK material is what MASK is for", glb("mask3.glb", [("wall", holes)], {"alpha": "MASK"}) + "#wall", "trim", clean=True)
    alpha_normal = np.dstack([normal, np.full(normal.shape[:2], 255, dtype=np.uint8)])
    alpha_normal[:8, :8, 3] = 0
    case("a normal map with transparency", glb("an.glb", [("wall", wall), ("wall_n", tk.flip_green(alpha_normal))], {"normal": 1}) + "#wall_n",
         "tile", ["alpha.kind"])
    case("an icon with black under its transparency, filtered linearly", write("fringe.png", png_bytes(icon(hidden=(0, 0, 0)))), "ui",
         ["alpha.fringe"])
    case("the same icon in a pixel-art UI (nearest): no halo to fear", write("fringe_px.png", png_bytes(icon(hidden=(0, 0, 0)))), "ui",
         absent=["alpha.fringe"], overrides={"filter": "nearest"})
    case("a glow stored premultiplied", write("premult.png", png_bytes(tk.premultiply(glow(colour=(255, 240, 220))))), "effect",
         ["alpha.premultiplied"])
    case("a straight glow where the profile says premultiplied", write("straight.png", glow(colour=(255, 240, 220))), "effect",
         ["alpha.premultiplied"], overrides={"alpha_data": "premultiplied"})
    case("a premultiplied glow where the profile says so", write("premult_ok.png", png_bytes(tk.premultiply(glow(colour=(255, 240, 220))))),
         "effect", absent=["alpha.premultiplied"], overrides={"alpha_data": "premultiplied"})
    tight = icon()
    tight[:, 0] = (232, 196, 110, 255)
    case("an icon that touches its edge", write("tight.png", tight), "ui", ["alpha.border"])
    crowded = sheet_cells.copy()
    crowded[8:24, 30:34] = (232, 196, 110, 255)
    case("a glyph that runs into its neighbour's cell", write("crowded.png", crowded), "ui", ["alpha.border"], overrides={"cell": [32, 32]})

    print("3. colour and pixel discipline")
    case("one flat colour", write("flat.png", np.zeros((32, 32, 3), dtype=np.uint8) + STONE[2]), "tile", ["colour.flat"])
    case("a wall crushed to black", write("crushed.png", (wall * 0.12).astype(np.uint8)), "tile", ["colour.crushed"])
    case("a wall blown to white", write("blown.png", (236 + wall * 0.07).astype(np.uint8)), "tile", ["colour.blown"])
    faint = np.zeros((64, 64, 3), dtype=np.uint8) + 120
    faint[::8] = 123
    faint[:, ::16] = 122
    case("almost no contrast", write("faint.png", faint), "tile", ["colour.contrast"])
    many = (rng.integers(0, 6, size=(64, 64, 1)) * 40 + rng.integers(0, 3, size=(64, 64, 3)) * 13).astype(np.uint8)
    case("pixel art with hundreds of colours", write("many.png", many), "tile", ["colour.count"], overrides={"max_colours": 16})
    blurred = np.asarray(Image.fromarray(np.tile(wall, (3, 3, 1))).resize((204, 204), Image.BILINEAR).resize((192, 192), Image.BILINEAR))[64:128, 64:128]
    case("pixel art that went through a resize with smoothing", write("blurred.png", blurred), "tile", ["colour.near_duplicates"])
    case("pixel art stored at 4x", write("big.png", np.repeat(np.repeat(wall, 4, axis=0), 4, axis=1)), "tile", ["colour.upscaled"],
         overrides={"repeat_contrast": 255})
    mixed = np.asarray(Image.fromarray(wall).resize((160, 160), Image.NEAREST))
    case("pixel art resampled by 2.5 (mixels)", write("mixels.png", mixed), "tile", ["colour.mixels"], absent=["colour.upscaled"],
         overrides={"pot": False})
    case("pixel art resampled by 1.5 (texels one and two pixels wide)",
         write("mixels15.png", np.asarray(Image.fromarray(wall).resize((96, 96), Image.NEAREST))), "tile", ["colour.mixels"], overrides={"pot": False})
    game = make_game()
    case("a colour that is not in the game's palette", os.path.join(game, "maps", "keep", "offpalette.png"), "tile", ["colour.palette"],
         kit=game_kit(game))
    case("a texture on the game's palette", os.path.join(game, "maps", "keep", "wall.png"), "tile", absent=["colour.palette"], kit=game_kit(game))
    case("a LUT that is the identity", cube("identity.cube", grade=lambda rgb: rgb), "lut", ["colour.lut_identity"])
    case("a LUT that clips", cube("clip.cube", grade=lambda rgb: rgb * 1.6 - 0.2), "lut", ["colour.lut_clipped"])
    case("a LUT whose tones turn back", cube("wave.cube", grade=lambda rgb: rgb + 0.2 * np.sin(rgb * 14)), "lut", ["colour.lut_tone"])
    case("a LUT that throws the range away", cube("dull.cube", grade=lambda rgb: 0.4 + rgb * 0.2), "lut", ["colour.lut_tone"])

    print("4. tiling and layout")
    y, x = np.mgrid[0:64, 0:64]
    weave = np.where((((x + y) % 12) < 3)[..., None], STONE[1], STONE[3]).astype(np.uint8)
    case("a diagonal weave whose lines miss each other at the wrap", write("weave.png", weave), "tile", ["tiling.seam"])
    case("the same weave with a period that divides the size", write("weave8.png", np.where((((x + y) % 8) < 2)[..., None], STONE[1], STONE[3])
                                                                                    .astype(np.uint8)), "tile", absent=["tiling.seam"])
    ramp = np.zeros((64, 64, 3), dtype=np.uint8) + (40 + np.arange(64) * 1.5).astype(np.uint8)[None, :, None]
    ramp[rng.random((64, 64)) < 0.3] += 6
    case("a texture that is darker on the left than on the right", write("ramp.png", ramp), "tile", ["tiling.seam"])
    case("a trim sheet need not wrap top to bottom", write("trim2.png", ramp.transpose(1, 0, 2)), "trim", absent=["tiling.seam"])
    blotch = wall.copy()
    blotch[16:32, 16:32] = (blotch[16:32, 16:32] * 0.35).astype(np.uint8)
    case("one dark blotch in a tile", write("blotch.png", blotch), "tile", ["tiling.repeat"])
    dither = np.where(((x + y) % 2 == 0)[..., None], STONE[1], STONE[5]).astype(np.uint8)
    dither[::16] = STONE[0]
    case("a one-texel checker", write("dither.png", dither), "tile", ["tiling.shimmer"])
    empty = np.zeros((256, 256, 3), dtype=np.uint8)
    empty[:64, :64] = wall
    case("an atlas that is mostly unused", write("empty.png", empty), "atlas", ["tiling.empty"])

    print("5. game readiness")
    case("a file that is not an image", write("broken.png", b"\x89PNG\r\n\x1a\nnot really"), "tile", ["ready.structure"])
    case("an image the model does not have", model + "#nope", "tile", ["ready.structure"])
    case("a name with capitals and spaces", write("Stone Wall FINAL.png", wall), "tile", ["ready.name"])
    case("an embedded image without a name", glb("unnamed.glb", [("x", wall)], names=False) + "#image0", "tile", ["ready.name"])
    case("a normal map whose name does not say so", glb("badname.glb", [("wall", wall), ("bumps", tk.flip_green(normal))], {"normal": 1}) + "#bumps",
         "tile", ["ready.name"])
    case("a colour map named like a normal map", glb("badname2.glb", [("wall_n", wall)]) + "#wall_n", "tile", ["ready.name"])
    case("a texture over the profile's size", write("huge.png", np.tile(wall, (16, 16, 1))), "tile", ["ready.max_size"],
         overrides={"repeat_contrast": 255})
    case("a texture under the profile's size", write("tiny.png", wall[:8, :8]), "tile", ["ready.min_size"])
    case("a tile that is not a power of two", write("npot.png", bricks(60, rows=6)), "tile", ["ready.pot"])
    case("a sky card need not be a power of two", write("sky_card.png", np.concatenate([ridge, ridge[:, :60]], axis=1)), "backdrop", clean=True)
    case("a strip of 16 to 1", write("strip.png", np.tile(wall[:16], (1, 4, 1))), "trim", ["ready.aspect"])
    case("a texture over the memory budget", write("heavy.png", np.tile(wall, (8, 8, 1))), "tile", ["ready.memory"],
         overrides={"max_megabytes": 1.0, "repeat_contrast": 255})
    case("a sheet that is not a whole number of cells", write("cells.png", sheet_cells[:, :120]), "ui", ["ready.cell"], overrides={"cell": [32, 32]})
    case("pixel art sampled linearly", glb("linear.glb", [("wall", wall)], sampler={"magFilter": 9729, "minFilter": 9987}) + "#wall", "tile",
         ["ready.sampler"])
    case("pixel art with no sampler at all (the engine then filters linearly)", glb("none.glb", [("wall", wall)], sampler=None) + "#wall", "tile",
         ["ready.sampler"])
    case("a world texture sampled without mips", glb("nomips.glb", [("wall", wall)], sampler={"magFilter": 9728, "minFilter": 9728}) + "#wall",
         "tile", ["ready.sampler"])
    case("a tiling texture whose sampler clamps",
         glb("clamp.glb", [("wall", wall)], sampler={"magFilter": 9728, "minFilter": 9984, "wrapS": 33071}) + "#wall", "tile", ["ready.sampler"])
    case("an image no mesh's material uses", glb("orphan.glb", [("wall", wall)], drawn=False) + "#wall", "tile", ["ready.unused"])
    flat_normal = np.zeros((32, 32, 3), dtype=np.uint8) + [128, 128, 255]
    case("a normal map that is flat", glb("flat.glb", [("wall", wall[:32, :32]), ("wall_n", flat_normal)], {"normal": 1}) + "#wall_n", "tile",
         ["ready.normal_flat"])
    gamma = (255 * (tk.flip_green(normal) / 255.0) ** (1 / 2.2)).astype(np.uint8)
    case("a normal map saved through an sRGB conversion", glb("gamma.glb", [("wall", wall), ("wall_n", gamma)], {"normal": 1}) + "#wall_n", "tile",
         ["ready.normal_encoding"])
    painted = tk.flip_green(normal).copy()
    painted[16:48, 16:48] = (painted[16:48, 16:48].astype(int) * 0.6 + 40).astype(np.uint8)
    case("a normal map that was painted on", glb("painted.glb", [("wall", wall), ("wall_n", painted)], {"normal": 1}) + "#wall_n", "tile",
         ["ready.normal_length"])
    swapped = tk.flip_green(normal)[..., [2, 1, 0]]
    case("a normal map with red and blue swapped", glb("swapped.glb", [("wall", wall), ("wall_n", swapped)], {"normal": 1}) + "#wall_n", "tile",
         ["ready.normal_blue"], absent=["ready.normal_encoding"], overrides={"normal_centre_tolerance": 200})
    case("a green-up normal map on a mesh without tangents", glb("up.glb", [("wall", wall), ("wall_n", normal)], {"normal": 1}) + "#wall_n", "tile",
         ["ready.normal_green"])
    case("a green-down normal map on a mesh with tangents",
         glb("down.glb", [("wall", wall), ("wall_n", tk.flip_green(normal))], {"normal": 1}, tangents=True) + "#wall_n", "tile", ["ready.normal_green"])
    case("an emissive map with nothing lit",
         glb("dark.glb", [("wall", wall), ("wall_e", np.zeros((64, 64, 3), dtype=np.uint8))], {"emissive": 1}) + "#wall_e", "tile",
         ["ready.emissive_black"])
    case("an emissive map that is lit all over", glb("bright.glb", [("wall", wall), ("wall_e", wall)], {"emissive": 1}) + "#wall_e", "tile",
         ["ready.emissive_coverage"])
    case("a normal map half the size of its colour map",
         glb("half.glb", [("wall", wall), ("wall_n", tk.flip_green(tk.normal_from_height(height_field(32), 3.0)))], {"normal": 1}) + "#wall_n", "tile",
         ["ready.map_size"])
    case("a nine-slice panel with scanlines in its middle", write("system.png", panel(scanlines=True)), "ui", ["ready.nine_slice"],
         overrides={"nine_slice": 12})
    case("a nine-slice whose borders leave no middle", write("small_panel.png", panel(24, 12)), "ui", ["ready.nine_slice"],
         overrides={"nine_slice": 12})
    case("a LUT of 64 cubed", cube("large.cube", 64), "lut", ["ready.lut_size"])
    if engine:
        case("the engine decodes a clean PNG", write("engine_ok.png", wall), "tile", clean=True, engine=engine)
        case("the engine loads a model's embedded textures", model + "#wall", "tile", clean=True, engine=engine)
        case("the engine refuses a WebP named .png", write("engine_webp.png", webp.getvalue()), "tile", ["ready.engine_load"], engine=engine)
        truncated = tk.encode_png(noisy)
        case("the engine refuses a PNG cut short", write("engine_cut.png", truncated[:len(truncated) // 2]), "default",
             ["ready.structure"], engine=engine)
    else:
        print("  (no engine: ready.engine_load not proved; pass --engine build/fengine_headless)")

    print("the game around a texture (manifest)")
    manifest_cases(game)

    print("helpers")
    helper_cases(wall, normal, rng)

    print("review sheets")
    for name, path, profile, overrides in (("a tile", os.path.join(WORK, "wall.png"), "tile", None), ("a normal map", model + "#wall_n", "tile", None),
                                           ("a glow", os.path.join(WORK, "fx_glow.png"), "effect", None),
                                           ("a LUT", os.path.join(WORK, "grade.cube"), "lut", None)):
        settings = common.profile_named(KIT.profiles(), profile, overrides)
        findings, _, subject = KIT.validate(path, settings)
        drawn = sheet.draw(subject, settings, findings, ["profile", "", "Judge against the art direction:"] + list(KIT.questions), {"stone": ["#1b1d2e"]})
        check(f"the sheet of {name} draws, and stays compact", 600 < drawn.width <= 1600 and drawn.height <= 900, f"{drawn.size}")
    group = sheet.draw_group([(p, common.profile_named(KIT.profiles(), "tile"), [], ti.load(p)) for p in
                              (os.path.join(WORK, "wall.png"), os.path.join(WORK, "blotch.png"), os.path.join(WORK, "huge.png"))],
                             ["wall.png", "blotch.png", "huge.png"], {})
    check("a group sheet draws", group.width <= 1800, f"{group.size}")

    shutil.rmtree(WORK, ignore_errors=True)
    print()
    if FAILURES:
        print(f"{len(FAILURES)} case(s) FAILED: " + "; ".join(FAILURES))
        return 1
    print("all checks proved")
    return 0


def with_interlace(pixels):
    """An Adam7 PNG. Pillow does not write one, so the scanlines are packed here."""
    pixels = np.asarray(pixels)[..., :3]
    height, width = pixels.shape[:2]
    raw = bytearray()
    for start_y, start_x, step_y, step_x in ((0, 0, 8, 8), (0, 4, 8, 8), (4, 0, 8, 4), (0, 2, 4, 4), (2, 0, 4, 2), (0, 1, 2, 2), (1, 0, 2, 1)):
        part = pixels[start_y::step_y, start_x::step_x]
        if part.size:
            for row in part:
                raw += b"\x00" + row.tobytes()

    def chunk(kind, payload):
        return struct.pack(">I", len(payload)) + kind + payload + struct.pack(">I", zlib.crc32(kind + payload) & 0xFFFFFFFF)

    return (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 2, 0, 0, 1)) +
            chunk(b"IDAT", zlib.compress(bytes(raw))) + chunk(b"IEND", b""))


def make_game():
    """A small game folder with a manifest: one map with a model, loose textures, a UI folder,
    a sprite (which is not a texture), a palette, and files nobody uses."""
    game = os.path.join(WORK, "game")
    wall = bricks()
    off = wall.copy()
    off[(off == STONE[5]).all(axis=2)] = (40, 200, 90)
    files = {
        "maps/keep/wall.png": wall, "maps/keep/offpalette.png": off, "maps/keep/unused.png": bricks(seed=5),
        "maps/keep/copy.png": wall, "ui/cursor.png": icon(), "ui/panel.png": panel(), "ui/plate.png": panel(48, 12)[::-1],
        "sprites/hero.png": icon(64), "sprites/hero_n.png": np.zeros((64, 64, 3), dtype=np.uint8) + [128, 128, 255],
        "replays/reference/shot.png": wall,
    }
    for name, pixels in files.items():
        os.makedirs(os.path.dirname(os.path.join(game, name)), exist_ok=True)
        tk.save_png(os.path.join(game, name), pixels)
    shutil.copy(glb("keepmodel.glb", [("wall", wall), ("wall_copy", wall)]), os.path.join(game, "maps", "keep", "geometry.glb"))
    with open(os.path.join(game, "sprites", "hero.anim.json"), "w", encoding="utf-8") as out:
        json.dump({"atlas": "hero.png", "normal": "hero_n.png", "clips": {}}, out)
    with open(os.path.join(game, "maps", "keep", "map.json"), "w", encoding="utf-8") as out:
        json.dump({"environment": {"sky": {"backdrops": [{"image": "maps/keep/wall.png"}, {"image": "maps/keep/offpalette.png"}]}}}, out)
    with open(os.path.join(game, "main.lua"), "w", encoding="utf-8") as out:
        out.write('local function panel(kind) return "ui/" .. (kind or "panel") .. ".png" end\n'
                  'fe.canvas.image("ui/cursor.png", 0, 0)\npanel("plate")\n')
    with open(os.path.join(WORK, "palette.hex"), "w", encoding="utf-8") as out:
        out.write("\n".join("%02x%02x%02x" % tuple(colour) for colour in STONE) + "\n")
    entries = {name: {"profile": "tile"} for name in ("maps/keep/wall.png", "maps/keep/offpalette.png", "maps/keep/unused.png", "maps/keep/copy.png")}
    entries["ui/cursor.png"] = {"profile": "ui"}
    entries.update({name: {"profile": "ui", "overrides": {"nine_slice": 12}} for name in ("ui/panel.png", "ui/plate.png")})
    entries["maps/keep/geometry.glb#wall"] = {"profile": "tile"}
    manifest = {"root": "game", "reviews": "texture_reviews", "folders": ["."], "ignore": ["replays/*"], "palette": "palette.hex",
                "textures": entries}
    with open(os.path.join(WORK, "textures.json"), "w", encoding="utf-8") as out:
        json.dump(manifest, out, indent=1)
    return game


def game_kit(game):
    """A kit that knows the test game (as validate.py does once it has read the manifest)."""
    kit = TextureKit(HERE)
    kit._entries(os.path.join(WORK, "textures.json"))
    return kit


def run(function, argv):
    """A command line's exit code and output."""
    import contextlib
    out = io.StringIO()
    with contextlib.redirect_stdout(out):
        try:
            code = function(argv)
        except SystemExit as stop:
            code, text = (stop.code if isinstance(stop.code, int) else 1), str(stop.code)
            out.write(text)
    return code, out.getvalue()


def manifest_cases(game):
    manifest = os.path.join(WORK, "textures.json")
    kit = game_kit(game)
    path = lambda name: os.path.join(game, *name.split("/"))  # noqa: E731
    case("a file nothing in the game names", path("maps/keep/unused.png"), "tile", ["ready.unused"], kit=kit)
    case("a file a map names", path("maps/keep/wall.png"), "tile", absent=["ready.unused"], kit=kit)
    case("a file whose name Lua builds from parts", path("ui/plate.png"), "ui", absent=["ready.unused"], kit=kit, overrides={"nine_slice": 12})
    case("a loose file with the pixels of another", path("maps/keep/copy.png"), "tile", ["file.duplicate"], kit=kit)
    code, text = run(TextureKit(HERE).validate_main, ["--manifest", manifest, "--no-review"])
    check("an embedded image the manifest does not list fails the run", code == 1 and "geometry.glb#wall_copy" in text and "not listed" in text)
    check("a sprite's atlas and normal map are not textures", "hero.png" not in text and "hero_n.png" not in text)
    check("ignored folders are not searched", "shot.png" not in text)
    with open(manifest, encoding="utf-8") as file:
        document = json.load(file)
    document["textures"]["maps/keep/geometry.glb#wall_copy"] = {"profile": "tile"}
    with open(manifest, "w", encoding="utf-8") as out:
        json.dump(document, out)
    kit = game_kit(game)
    case("the same image twice in one model", path("maps/keep/geometry.glb") + "#wall_copy", "tile", ["file.duplicate"], kit=kit)
    tk.save_png(path("maps/keep/stray.png"), bricks(seed=9))
    code, text = run(TextureKit(HERE).validate_main, ["--manifest", manifest, "--no-review"])
    check("a loose image the manifest does not list fails the run", code == 1 and "maps/keep/stray.png" in text)
    os.remove(path("maps/keep/stray.png"))
    code, text = run(TextureKit(HERE).validate_main, ["--manifest", manifest, "ui", "--no-review"])
    check("a folder names its textures: 'ui' validates three", "3 of 3 texture(s) ready" in text, text[-200:])
    code, text = run(TextureKit(HERE).validate_main, ["--manifest", manifest, "maps/keep/geometry.glb", "--no-review", "--lenient"])
    check("a model names the textures inside it", "of 2 texture(s) ready" in text, text[-200:])
    code, text = run(TextureKit(HERE).validate_main, ["--manifest", manifest, "ui"])
    check("without a signed review a texture is not ready", code == 1 and "no visual review on record" in text)
    code, text = run(TextureKit(HERE).review_main, ["--manifest", manifest, "ui"])
    sheets = os.path.join(WORK, "texture_reviews")
    check("review.py writes a sheet per texture and one for the group",
          all(os.path.isfile(os.path.join(sheets, name)) for name in ("ui_cursor.png", "ui_panel.png", "ui_plate.png", "group_ui.png")), text[-300:])
    code, text = run(TextureKit(HERE).review_main, ["--manifest", manifest, "maps/keep/geometry.glb"])
    check("each texture of a model gets its own sheet",
          all(os.path.isfile(os.path.join(sheets, name)) for name in ("maps_keep_geometry-wall.png", "maps_keep_geometry-wall_copy.png")), text[-300:])
    code, text = run(TextureKit(HERE).review_main, ["--manifest", manifest, "ui", "--sign", "pass", "--by", "selftest",
                                                    "--notes", "three UI images judged together on the group sheet"])
    code, text = run(TextureKit(HERE).validate_main, ["--manifest", manifest, "ui"])
    check("signing a group records every texture in it", code == 0 and "3 of 3 texture(s) ready" in text, text[-300:])
    run(TextureKit(HERE).review_main, ["--manifest", manifest, "maps/keep/geometry.glb", "--sign", "pass", "--by", "selftest",
                                       "--notes", "both embedded textures judged from their sheets"])
    shutil.copy(glb("keepmodel2.glb", [("wall", bricks(seed=3)), ("wall_copy", bricks())]), path("maps/keep/geometry.glb"))
    code, text = run(TextureKit(HERE).validate_main, ["--manifest", manifest, "maps/keep/geometry.glb", "--lenient"])
    check("changing the model voids the reviews of its textures", code == 1 and "changed after its visual review" in text, text[-300:])


def helper_cases(wall, normal, rng):
    # save_png / encode_png
    rgba = np.dstack([wall, np.full(wall.shape[:2], 255, dtype=np.uint8)])
    dirty = with_chunk(png_bytes(rgba), b"tEXt", b"Software\x00" + b"x" * 200)
    before = ids(dirty, "tile")
    path = os.path.join(WORK, "probe", "saved.png")
    tk.save_png(path, ti.decode(dirty).rgba)
    saved = ti.load(path)
    check("save_png: no metadata, no unused alpha, a palette, the same pixels",
          "file.metadata" in before and ti.decode(dirty).stores_alpha and not saved.stores_alpha
          and not ids(open(path, "rb").read(), "tile") and saved.mode == "P"
          and np.array_equal(saved.rgba[..., :3], wall), f"before {sorted(before)}")
    translucent = glow()
    check("save_png keeps alpha that is used, exactly", np.array_equal(ti.decode(tk.encode_png(translucent)).rgba, translucent))
    # fix.py
    interlaced = os.path.join(WORK, "probe", "fixme.png")
    with open(interlaced, "wb") as out:
        out.write(with_chunk(with_interlace(wall), b"gAMA", struct.pack(">I", 100000)))
    steps, data = fix.plan(interlaced)
    check("fix.py plans to strip the gamma and the interlacing, and writes nothing by itself",
          data is not None and any("gAMA" in step for step in steps) and any("interlacing" in step for step in steps)
          and ti.load(interlaced).interlaced, str(steps))
    run(fix.main, [interlaced, "--write"])
    check("fix.py --write leaves a clean file with the same pixels", not ids(open(interlaced, "rb").read(), "tile")
          and np.array_equal(ti.load(interlaced).rgba[..., :3], wall))
    fringed = os.path.join(WORK, "probe", "fringed.png")
    with open(fringed, "wb") as out:
        out.write(png_bytes(icon(hidden=(0, 0, 0))))
    run(fix.main, [fringed, "--bleed", "--write"])
    after = ti.load(fringed)
    check("fix.py --bleed removes the fringe and changes no visible texel",
          "alpha.fringe" not in ids(open(fringed, "rb").read(), "ui") and np.array_equal(after.alpha, icon()[..., 3])
          and np.array_equal(after.rgba[after.alpha > 0], icon()[icon()[..., 3] > 0]))
    # bleed_edges
    black_under = icon(hidden=(0, 0, 0))
    bled = tk.bleed_edges(black_under)
    check("bleed_edges fixes alpha.fringe and keeps alpha",
          "alpha.fringe" in ids(png_bytes(black_under), "ui") and "alpha.fringe" not in ids(png_bytes(bled), "ui")
          and np.array_equal(bled[..., 3], black_under[..., 3]))
    # premultiply / unpremultiply
    straight = glow(colour=(255, 240, 220))
    check("unpremultiply fixes alpha.premultiplied",
          "alpha.premultiplied" in ids(png_bytes(tk.premultiply(straight)), "effect")
          and "alpha.premultiplied" not in ids(png_bytes(tk.unpremultiply(tk.premultiply(straight))), "effect"))
    # seam / make_tileable / mirror_tile
    y, x = np.mgrid[0:64, 0:64]
    clouds = np.zeros((64, 64, 3), dtype=np.uint8)
    clouds[...] = (70 + 40 * np.sin(x / 13.0) * np.cos(y / 17.0) + 30 * (x / 64.0))[..., None] + rng.integers(0, 4, size=(64, 64, 1))
    measured = tk.seam(clouds)
    check("seam() measures a wrap that does not meet", measured["u"]["ratio"] > 1.5 and tk.seam(bricks())["u"]["ratio"] < 1.5,
          f"{measured['u']['ratio']:.2f}, {tk.seam(bricks())['u']['ratio']:.2f}")
    check("make_tileable fixes tiling.seam", "tiling.seam" in ids(clouds, "tile", {"pixel_art": False})
          and "tiling.seam" not in ids(tk.make_tileable(clouds), "tile", {"pixel_art": False}))
    cracked = bricks()
    cracked[:, 40:] = np.roll(cracked[:, 40:], 3, axis=0)
    cracked[:, :3] = STONE[4]
    hard = tk.make_tileable(cracked, hard=True)
    check("make_tileable(hard=True) adds no colour", len(tk.palette_of(hard)) <= len(tk.palette_of(cracked)))
    mirrored = tk.mirror_tile(clouds)
    check("mirror_tile always wraps", mirrored.shape == (128, 128, 3) and "tiling.seam" not in ids(mirrored, "tile", {"pixel_art": False}))
    # quantize / palette_of / load_palette
    blurred = np.asarray(Image.fromarray(np.tile(wall, (3, 3, 1))).resize((204, 204), Image.BILINEAR).resize((192, 192), Image.BILINEAR))[64:128, 64:128]
    cleaned = tk.quantize(blurred, STONE)
    check("quantize to a palette fixes colour.near_duplicates", "colour.near_duplicates" in ids(blurred, "tile", {"seam_factor": 99, "repeat_contrast": 255})
          and not {"colour.near_duplicates", "colour.count"} & ids(cleaned, "tile", {"seam_factor": 99, "repeat_contrast": 255})
          and len(tk.palette_of(cleaned)) <= len(STONE))
    reduced = tk.quantize(blurred, 8)
    check("quantize to a number of colours", len(tk.palette_of(reduced)) <= 8)
    dithered = tk.quantize(np.zeros((16, 16, 3), dtype=np.uint8) + ((STONE[2].astype(int) + STONE[3]) // 2).astype(np.uint8), STONE[2:4], dither=True)
    shares = sorted(share for _, share in tk.palette_of(dithered))
    check("quantize(dither=True) mixes the two nearest colours", len(shares) == 2 and 0.3 < shares[0] <= 0.5, str(shares))
    top = tk.palette_of(wall)
    check("palette_of lists colours by share", abs(sum(share for _, share in top) - 1) < 1e-6 and top[0][1] >= top[-1][1] and len(top) == 6)
    check("load_palette reads a .hex file, a list and an image", len(tk.load_palette(os.path.join(WORK, "palette.hex"))) == 6
          and len(tk.load_palette(["#1b1d2e", (1, 2, 3)])) == 2 and len(tk.load_palette(wall)) == 6)
    # normal maps
    model = glb("helper_n.glb", [("wall", wall), ("wall_n", normal)], {"normal": 1}, tangents=True)
    settings = common.profile_named(KIT.profiles(), "tile")
    findings, _, texture = KIT.validate(model + "#wall_n", settings)
    direction, ratio = checks.normal_green(texture, True)
    check("normal_from_height: unit vectors, green up, tileable, clean", not [f for f in findings if f.severity != "info"]
          and direction == "up" and ratio > 1.5, f"{direction} x{ratio:.1f}")
    bump = np.zeros((32, 32))
    bump[12:20, 12:20] = 1.0
    relief = tk.normal_from_height(bump, 2.0, wrap=False)
    check("normal_from_height: a raised block's top edge leans up (green over 128), its left edge left (red under 128)",
          relief[12, 16, 1] > 140 and relief[19, 16, 1] < 116 and relief[16, 12, 0] < 116 and relief[16, 19, 0] > 140,
          f"{relief[12, 16].tolist()} {relief[16, 12].tolist()}")
    texture.rgba = np.dstack([tk.flip_green(normal), np.full(normal.shape[:2], 255, dtype=np.uint8)])
    check("flip_green turns it green-down", checks.normal_green(texture, True)[0] == "down")
    painted = normal.copy()
    painted[16:48, 16:48] = (painted[16:48, 16:48].astype(int) * 0.6 + 40).astype(np.uint8)
    fixed = tk.renormalize(painted)
    lengths = np.linalg.norm(fixed.astype(np.float64) / 127.5 - 1, axis=2)
    check("renormalize makes every vector unit length", np.abs(lengths - 1).max() < 0.03, f"{lengths.min():.3f}..{lengths.max():.3f}")
    # sizes and mips
    odd = bricks(60, rows=6)
    padded, scale = tk.pad_pow2(odd)
    check("pad_pow2 pads to the next power of two and says how to scale the UVs",
          padded.shape[:2] == (64, 64) and np.array_equal(padded[:60, :60], odd) and scale == (60 / 64, 60 / 64))
    chain = tk.mip_chain(wall)
    check("mip_chain goes down to 1x1", [level.shape[0] for level in chain] == [64, 32, 16, 8, 4, 2, 1])
    edge = np.zeros((2, 2, 4), dtype=np.uint8)
    edge[0, 0] = (200, 40, 40, 255)          # one red texel, three transparent black ones
    half = tk.halve(edge)
    check("mips weight colour by alpha, as the engine's do", half[0, 0].tolist() == [200, 40, 40, 64], str(half[0, 0].tolist()))
    grey = np.zeros((2, 2, 4), dtype=np.uint8)
    grey[..., 3] = 255
    grey[0, :, :3] = 255
    check("mips average colour in linear light", 185 <= int(tk.halve(grey)[0, 0, 0]) <= 189 and int(tk.halve(grey, srgb=False)[0, 0, 0]) == 128,
          f"{tk.halve(grey)[0, 0, 0]}")


if __name__ == "__main__":
    sys.exit(main())
