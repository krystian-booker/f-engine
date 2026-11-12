"""The review sheet: one compact image per texture that shows what the
measurements cannot judge, and a group sheet that puts related textures side
by side at one scale.

A sheet is eight small tiles and the text: the whole texture, a detail
enlarged with nearest filtering, the texture on black and on white as the
game will sample it (halos show there) or tiled 3x3 or lit as a normal map,
its channels, its mips as the engine builds them, its values, and its palette
with the art direction's swatches beside it.
"""
import numpy as np
from PIL import Image, ImageDraw

from kitcommon import kit as common

from . import checks, tools
from . import image as ti

TILE = 256
GUTTER = 6
BACK = (20, 21, 28)
INK = (200, 204, 216)


# A colour grade is not a picture: it is judged by what it does to pictures.
LUT_QUESTIONS = [
    "1. Mood: does the grade push the picture where the art direction says this place is (warm, cold, faded, lurid)?",
    "2. Tones: is the grey ramp still smooth and rising, black near black, white near white, no bands?",
    "3. Colours: do the art direction's colours come through recognisable, none turned to mud or to neon?",
    "4. Neutrals: is the tint of the greys meant, and the same from dark to light (no colour crossing over mid-ramp)?",
]


def _checker(width, height, size=8):
    y, x = np.mgrid[0:height, 0:width]
    value = (((x // size) + (y // size)) % 2) * 28 + 58
    return np.dstack([value, value, value]).astype(np.uint8)


def _over(rgba, background):
    """Straight-alpha RGBA over a background colour or an (h, w, 3) image, as the engine blends it."""
    alpha = rgba[..., 3:4].astype(np.float64) / 255.0
    back = np.broadcast_to(np.asarray(background, dtype=np.float64), rgba[..., :3].shape)
    return (rgba[..., :3] * alpha + back * (1 - alpha) + 0.5).astype(np.uint8)


def _fit(rgba, box=TILE, smooth=False):
    """The image scaled to fit a box: enlarged by a whole factor (nearest, or bilinear on
    straight alpha when the game samples it so), or reduced as the engine's mips do it."""
    height, width = rgba.shape[:2]
    factor = min(box // width, box // height)
    if factor >= 1:
        size = (width * factor, height * factor)
        scaled = np.asarray(Image.fromarray(rgba).resize(size, Image.BILINEAR if smooth and factor > 1 else Image.NEAREST))
        return scaled, f"x{factor}"
    level, steps = rgba, 0
    while max(level.shape[:2]) > box:
        level = tools.halve(level, srgb=True)
        steps += 1
    return level, f"1/{2 ** steps}"


def _tile(content, caption, background=BACK):
    """A tile: the content centred in a TILE square under a caption bar, with a gutter on its
    right and below so neighbouring pictures cannot be read as one."""
    tile = Image.new("RGB", (TILE + GUTTER, TILE + 16 + GUTTER), background)
    picture = Image.fromarray(content[..., :3]) if isinstance(content, np.ndarray) else content
    tile.paste(picture.convert("RGB"), ((TILE - picture.width) // 2, 16 + (TILE - picture.height) // 2))
    ImageDraw.Draw(tile).rectangle([TILE, 0, TILE + GUTTER, TILE + 16 + GUTTER], fill=BACK)
    return common.label(tile, caption)


def _busiest(texture, size):
    """Top-left corner of the `size` square window with the most going on: alpha edges for a
    cutout, else colour changes."""
    height, width = texture.height, texture.width
    if width <= size and height <= size:
        return 0, 0
    values = texture.rgba.astype(np.int32)
    change = np.zeros((height, width))
    change[:, 1:] += np.abs(np.diff(values, axis=1)).max(axis=2)
    change[1:, :] += np.abs(np.diff(values, axis=0)).max(axis=2)
    if texture.alpha.min() < 255:
        edge = np.zeros((height, width))
        edge[:, 1:] += np.abs(np.diff(values[..., 3], axis=1))
        edge[1:, :] += np.abs(np.diff(values[..., 3], axis=0))
        change = change + 4 * edge
    step = max(1, size // 2)
    best, corner = -1.0, (0, 0)
    for top in range(0, max(1, height - size + 1), step):
        for left in range(0, max(1, width - size + 1), step):
            score = change[top:top + size, left:left + size].sum()
            if score > best:
                best, corner = score, (left, top)
    return corner


def _lit(texture, light, green_up):
    """A normal map lit by one directional light, as a grey surface: (h, w, 3) uint8."""
    vector = checks.normal_vectors(texture)
    if not green_up:
        vector = vector * [1, -1, 1]
    length = np.linalg.norm(vector, axis=2, keepdims=True)
    vector = vector / np.maximum(length, 1e-6)
    light = np.asarray(light, dtype=np.float64)
    light = light / np.linalg.norm(light)
    shade = np.clip(vector @ light, 0, 1) * 0.9 + 0.08
    grey = (np.clip(shade, 0, 1) ** (1 / 2.2) * 255).astype(np.uint8)
    return np.dstack([grey, grey, grey, np.full_like(grey, 255)])


def _channels(texture):
    half = TILE // 2
    out = Image.new("RGB", (TILE, TILE), BACK)
    for index, (name, colour) in enumerate((("R", (255, 110, 110)), ("G", (120, 230, 120)), ("B", (120, 150, 255)), ("A", (230, 230, 230)))):
        channel = texture.rgba[..., index]
        grey = np.dstack([channel, channel, channel, np.full_like(channel, 255)])
        scaled, _ = _fit(grey, half - 2)
        picture = Image.fromarray(scaled[..., :3])
        x, y = (index % 2) * half, (index // 2) * half
        out.paste(picture, (x + (half - picture.width) // 2, y + (half - picture.height) // 2))
        ImageDraw.Draw(out).text((x + 3, y + 2), name, fill=colour)
    return out


def _mips(texture):
    """The mip levels at 1:1, packed into a tile, from the first that fits."""
    levels = tools.mip_chain(texture.rgba, srgb=texture.role != ti.NORMAL)
    out = Image.fromarray(_checker(TILE, TILE))
    x = y = row = 0
    for level in levels[1:]:
        height, width = level.shape[:2]
        if width > TILE // 2 or height > TILE // 2 or max(width, height) < 4:
            continue
        if x + width > TILE:
            x, y, row = 0, y + row + 4, 0
        if y + height > TILE:
            break
        out.paste(Image.fromarray(_over(level, _checker(width, height))), (x, y))
        x += width + 4
        row = max(row, height)
    return out


def _far(texture):
    """The mip level about 32 texels across, enlarged: how the texture reads from far away."""
    level = texture.rgba
    srgb = texture.role != ti.NORMAL
    while max(level.shape[:2]) > 48:
        level = tools.halve(level, srgb)
    scaled, _ = _fit(level)
    return _over(scaled, _checker(scaled.shape[1], scaled.shape[0])), f"{level.shape[1]}x{level.shape[0]}"


def _values(texture):
    luminance = (texture.rgba[..., :3].astype(np.float64) @ checks.LUMA).astype(np.uint8)
    grey = np.dstack([luminance, luminance, luminance, texture.alpha])
    scaled, _ = _fit(grey)
    return _over(scaled, _checker(scaled.shape[1], scaled.shape[0]))


def _tiled(texture, across_u=True, across_v=True, rgba=None):
    """(the picture, its caption): 3x3 repeats of a texture that tiles both ways; one that
    repeats along one axis only is shown rolled by half, so its wrap runs through the middle."""
    rgba = texture.rgba if rgba is None else rgba
    if across_u and across_v:
        repeated, caption = np.tile(rgba, (3, 3, 1)), "tiled 3x3: seams, repeat"
    elif across_u:
        repeated, caption = np.roll(rgba, texture.width // 2, axis=1), "wrapped along U: the seam is the middle column"
    else:
        repeated, caption = np.roll(rgba, texture.height // 2, axis=0), "wrapped along V: the seam is the middle row"
    scaled, _ = _fit(repeated)
    return _over(scaled, _checker(scaled.shape[1], scaled.shape[0])), caption


def _palette_tile(texture, swatches):
    """The texture's own colours by share, the art direction's swatches, and a luminance histogram."""
    out = Image.new("RGB", (TILE, TILE), BACK)
    draw = ImageDraw.Draw(out)
    palette = tools.palette_of(texture.rgba, 24) if texture.rgba is not None else []
    for index, (colour, share) in enumerate(palette):
        x, y = 2 + (index % 6) * 42, 2 + (index // 6) * 24
        draw.rectangle([x, y, x + 39, y + 12], fill=colour)
        draw.text((x, y + 12), f"{share * 100:.0f}%" if share >= 0.005 else "<1%", fill=INK)
    y = 2 + 4 * 24 + 4
    if swatches:
        draw.text((2, y), "art direction:", fill=(255, 220, 140))
        x, y = 2, y + 12
        for name, colours in swatches.items():
            colours = [colours] if isinstance(colours, str) else colours
            width = 9 * len(colours)
            if x + width > TILE - 2:
                x, y = 2, y + 12
            for colour in colours:
                draw.rectangle([x, y, x + 8, y + 9], fill=tuple(int(colour.lstrip("#")[i:i + 2], 16) for i in (0, 2, 4)))
                x += 9
            x += 4
        y += 14
    if texture.rgba is not None and texture.alpha.max() > 0:
        luminance = (texture.rgba[..., :3].astype(np.float64) @ checks.LUMA)[texture.alpha > 0]
        histogram = np.bincount((luminance / 256 * 64).astype(int), minlength=64)[:64].astype(np.float64)
        histogram = np.sqrt(histogram / max(1.0, histogram.max()))
        base, top = TILE - 14, y + 2
        for index, value in enumerate(histogram):
            height = int(value * (base - top))
            draw.rectangle([index * 4, base - height, index * 4 + 2, base], fill=(150, 156, 176))
        draw.text((2, TILE - 12), "values: black to white (sqrt scale)", fill=INK)
    return out


def _lut_tiles(texture, swatches=None):
    """A LUT is judged on what it does: a hue and value chart and a grey ramp, before and after."""
    lut = texture.lut
    y, x = np.mgrid[0:TILE, 0:TILE] / (TILE - 1)
    hue, value = x * 6.0, 1.0 - y
    k = [(n + hue) % 6 for n in (5, 3, 1)]
    saturation = np.where(y < 0.5, 1.0, 0.45)
    level = np.where(y < 0.5, 1.0 - y * 1.6, 1.0 - (y - 0.5) * 1.6)
    chart = np.dstack([level * (1 - saturation * np.clip(np.minimum(np.minimum(c, 4 - c), 1), 0, 1)) for c in k])
    ramp = np.dstack([x * (value > -1)] * 3)
    ramp[:TILE // 2] = np.round(ramp[:TILE // 2] * 15) / 15       # stepped above, smooth below
    tiles = []
    for name, picture in (("hues at two saturations", chart), ("grey ramp: 16 steps, smooth", ramp)):
        graded = ti.lut_apply(lut, picture) if lut.table is not None else picture
        tiles.append(_tile((picture * 255).astype(np.uint8), name + ": before"))
        tiles.append(_tile((graded * 255).astype(np.uint8), name + ": after"))
    curve = Image.new("RGB", (TILE, TILE), BACK)
    draw = ImageDraw.Draw(curve)
    draw.line([0, TILE - 1, TILE - 1, 0], fill=(70, 72, 84))
    if lut.table is not None:
        grey = np.linspace(0, 1, TILE)
        out = ti.lut_apply(lut, np.stack([grey] * 3, axis=1))
        for channel, colour in enumerate(((255, 110, 110), (120, 230, 120), (120, 150, 255))):
            points = [(i, TILE - 1 - int(np.clip(out[i, channel], 0, 1) * (TILE - 1))) for i in range(TILE)]
            draw.line(points, fill=colour)
    tiles.append(_tile(curve, "what grey becomes: R, G, B against in = out"))
    if texture.rgba is not None:
        scaled, _ = _fit(texture.rgba)
        tiles.append(_tile(scaled, "the 16-cubed table the engine builds"))
    if swatches and lut.table is not None:
        # The art direction's colours, each before (left) and after (right) the grade.
        chart = Image.new("RGB", (TILE, TILE), BACK)
        draw = ImageDraw.Draw(chart)
        colours = [colour for group in swatches.values() for colour in ([group] if isinstance(group, str) else group)]
        size = max(8, min(28, (TILE - 4) // max(1, (len(colours) + 1) // 2)))
        for index, colour in enumerate(colours):
            before = np.array([int(colour.lstrip("#")[i:i + 2], 16) for i in (0, 2, 4)]) / 255.0
            after = np.clip(ti.lut_apply(lut, before[None, :])[0], 0, 1)
            x, y = 4 + (index % 2) * (TILE // 2), 4 + (index // 2) * size
            draw.rectangle([x, y, x + 50, y + size - 3], fill=tuple(int(v * 255) for v in before))
            draw.rectangle([x + 54, y, x + 104, y + size - 3], fill=tuple(int(v * 255) for v in after))
        tiles.append(_tile(chart, "art direction's colours: before, after"))
    return tiles


def draw(texture, profile, findings, lines, swatches=None, title=None):
    """The review sheet of one texture (a PIL image)."""
    lines = _lines(texture, profile, findings, lines)
    title = title or texture.label
    if texture.is_lut:
        return common.compose(_lut_tiles(texture, swatches), title, lines, columns=4)
    if texture.rgba is None:
        return common.compose([], title, lines)
    linear = checks.sampled_linear(texture, profile)
    alpha = texture.alpha.min() < 255
    tile_mode = profile.get("tile", "none")
    tiles = []
    whole, scale = _fit(texture.rgba)
    tiles.append(_tile(_over(whole, _checker(whole.shape[1], whole.shape[0])), f"whole, {texture.width}x{texture.height} at {scale}"))
    window = TILE // 4
    left, top = _busiest(texture, window)
    crop = texture.rgba[top:top + window, left:left + window]
    detail, factor = _fit(crop)
    tiles.append(_tile(_over(detail, _checker(detail.shape[1], detail.shape[0])), f"detail at ({left}, {top}), nearest {factor}"))
    if alpha:
        sampled, _ = _fit(crop, smooth=linear)
        how = "linear, as sampled" if linear else "nearest, as sampled"
        tiles.append(_tile(_over(sampled, (0, 0, 0)), f"detail on black ({how})"))
        tiles.append(_tile(_over(sampled, (255, 255, 255)), f"detail on white ({how})"))
    elif texture.role == ti.NORMAL:
        green_up = profile.get("normal_green", "up") == "up"
        reading = "green up" if green_up else "green down"
        for name, light in (("upper left", (-0.55, 0.55, 0.62)), ("lower right", (0.55, -0.55, 0.62))):
            lit, _ = _fit(_lit(texture, light, green_up))
            tiles.append(_tile(lit, f"lit from {name}, as the engine reads it"))
    else:
        if tile_mode != "none":
            tiles.append(_tile(*_tiled(texture, tile_mode in ("both", "u"), tile_mode in ("both", "v"))))
        else:
            tiles.append(_tile(_values(texture), "values (luminance)"))
        far, size = _far(texture)
        tiles.append(_tile(far, f"from far away: the {size} mip, enlarged"))
    tiles.append(_tile(_channels(texture), "channels"))
    tiles.append(_tile(_mips(texture), "mips at 1:1, as the engine builds them" if checks.has_mips(texture, profile) else
                       "at 1/2, 1/4, 1/8 (drawn without mips)"))
    if tile_mode != "none" and (alpha or texture.role == ti.NORMAL):
        # A normal map's seams show in how it lights, not in its colours.
        shown = _lit(texture, (-0.55, 0.55, 0.62), green_up) if texture.role == ti.NORMAL else None
        tiles.append(_tile(*_tiled(texture, tile_mode in ("both", "u"), tile_mode in ("both", "v"), shown)))
    elif alpha:
        whole_on, _ = _fit(texture.rgba, smooth=linear)
        tiles.append(_tile(_over(whole_on, (0, 0, 0)), "whole on black"))
    elif texture.role == ti.NORMAL:
        far, size = _far(texture)
        tiles.append(_tile(far, f"from far away: the {size} mip, enlarged"))
    else:
        tiles.append(_tile(_values(texture) if tile_mode != "none" else _over(_fit(texture.rgba)[0], (0, 0, 0)),
                           "values (luminance)" if tile_mode != "none" else "whole on black"))
    if texture.role == ti.NORMAL:
        # A palette says nothing about a normal map: show the detail lit instead, bumps against dents.
        lit = _lit(texture, (-0.55, 0.55, 0.62), green_up)[top:top + window, left:left + window]
        tiles.append(_tile(_fit(lit)[0], f"detail lit from upper left, {reading}"))
    else:
        tiles.append(_tile(_palette_tile(texture, swatches), "palette by share, art direction, values"))
    return common.compose(tiles, title, lines, columns=4)


def _lines(texture, profile, findings, lines):
    """The text beside the tiles: kitcommon's lines, with the facts wrapped instead of cut,
    how materials use the texture, and every finding in full."""
    groups, _ = common.verdict(findings, [group for group, _ in checks.GROUPS] + ["visual"])
    out = [f"profile: {profile['name']}"]
    out += common.wrap(checks.stats_line(texture, profile), 78, "  ")
    for line in checks.use_lines(texture)[:4]:
        out += common.wrap(line, 78, "  ")
    out.append("")
    for group, text in checks.GROUPS:
        out.append(f"[{groups[group].upper()}] {text}")
        for finding in findings:
            if finding.group != group or finding.severity == common.INFO:
                continue
            count = f"{finding.count:,} " if finding.count else ""
            where = f" in {finding.where}" if finding.where else ""
            wrapped = common.wrap(f"{'waived' if finding.waived else finding.severity}: {finding.check}: {count}{finding.message}{where}", 78, "     ")
            out += wrapped[:4]
    start = next((index for index, line in enumerate(lines) if line.startswith("Judge against")), None)
    asked = lines[start:] if start is not None else []
    if texture.is_lut and asked:
        asked = asked[:1]
        for question in LUT_QUESTIONS:
            asked += common.wrap(question)
    return out + [""] + asked


def draw_group(reviewed, keys, swatches=None):
    """Several textures on one sheet, each at the same number of screen pixels per texel
    where it can be (the label says the scale), with its palette strip and its verdict: for
    judging a folder or a map's textures as a set."""
    cell = 264
    largest = max(max(texture.width, texture.height) for _, _, _, texture in reviewed if texture.rgba is not None) if any(
        texture.rgba is not None for _, _, _, texture in reviewed) else cell
    shared = 1.0
    while largest * shared > cell:
        shared /= 2
    tiles, lines = [], [f"{len(reviewed)} textures, each at {('1/%d' % round(1 / shared)) if shared < 1 else '1:1'}", "unless its label says otherwise", ""]
    for (path, profile, findings, texture), key in zip(reviewed, keys):
        tile = Image.new("RGB", (cell + GUTTER, cell + 44), BACK)
        name = key.split("#")[-1] if "#" in key else key.split("/")[-1]
        scale = ""
        if texture.rgba is not None:
            level, steps = texture.rgba, 1.0
            while steps > shared:
                level = tools.halve(level, srgb=texture.role != ti.NORMAL)
                steps /= 2
            if max(level.shape[:2]) < 96:       # too small to read at the shared scale: enlarge it, and say so
                factor = max(1, 128 // max(level.shape[:2]))
                level = np.asarray(Image.fromarray(level).resize((level.shape[1] * factor, level.shape[0] * factor), Image.NEAREST))
                scale = f" (x{steps * factor:g})"
            picture = Image.fromarray(_over(level, _checker(level.shape[1], level.shape[0])))
            tile.paste(picture, ((cell - picture.width) // 2, 16 + (cell - picture.height) // 2))
            draw = ImageDraw.Draw(tile)
            for index, (colour, _) in enumerate(tools.palette_of(texture.rgba, 20)):
                draw.rectangle([index * 10, cell + 18, index * 10 + 8, cell + 28], fill=colour)
        draw = ImageDraw.Draw(tile)
        groups, ok = common.verdict(findings, [group for group, _ in checks.GROUPS])
        open_warnings = sum(1 for f in findings if f.severity != common.INFO and not f.waived)
        waived = sum(1 for f in findings if f.waived)
        state = "clean" if not open_warnings and not waived else f"{open_warnings} open, {waived} waived"
        draw.text((2, cell + 30), f"{texture.width}x{texture.height} {profile['name']}/{texture.role}: {state}"[:44],
                  fill=(255, 120, 120) if open_warnings else (255, 210, 120) if waived else (150, 220, 150))
        tiles.append(common.label(tile, (name + scale)[:42]))
        lines.append(f"{name}: {state}"[:60])
        for finding in findings:
            if finding.severity != common.INFO:
                lines.append(f"   {'waived' if finding.waived else finding.severity} {finding.check}"[:60])
    lines += ["", "Judge the set: one texel size, one palette, one hand?"]
    if swatches:
        strip = Image.new("RGB", (cell, cell + 44), BACK)
        draw = ImageDraw.Draw(strip)
        y = 20
        for name, colours in swatches.items():
            colours = [colours] if isinstance(colours, str) else colours
            for index, colour in enumerate(colours):
                draw.rectangle([2 + index * 14, y, 14 + index * 14, y + 10], fill=tuple(int(colour.lstrip("#")[i:i + 2], 16) for i in (0, 2, 4)))
            draw.text((6 + 14 * len(colours), y - 1), name[:24], fill=INK)
            y += 14
            if y > cell + 30:
                break
        tiles.append(common.label(strip, "art direction's colours"))
    columns = 5 if len(tiles) > 4 else max(1, len(tiles))
    return common.compose(tiles, "group: " + ", ".join(sorted({key.split("#")[0] for key in keys}))[:56], lines, columns=columns, side=380)
