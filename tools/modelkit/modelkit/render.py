"""Pictures of a model for a person (or an agent) to judge, drawn without the
engine: clay views that show the form and its shading, silhouettes at game
size, the textures as they land on the surface, texel density, and the UV
layout. Painter's algorithm with PIL: quick, and honest enough to review by.
"""
import io
import math

import numpy as np
from PIL import Image, ImageDraw, ImageFont

from . import checks

BACKGROUND = (38, 40, 52)
VIEWS = {  # name: (yaw, pitch) of the camera in degrees; yaw 0 looks at the model's front (+Z)
    "front": (0, 0), "right": (90, 0), "back": (180, 0), "left": (270, 0), "top": (0, 89),
    "three-quarter": (35, 20), "game camera": (0, 25),
}
MAP_CAMERA = (0, 40)  # a map is looked down on from its near (+Z) side


def _basis(yaw, pitch):
    y, p = math.radians(yaw), math.radians(pitch)
    forward = np.array([-math.sin(y) * math.cos(p), -math.sin(p), -math.cos(y) * math.cos(p)])  # camera looks along this
    right = np.array([math.cos(y), 0.0, -math.sin(y)])
    up = np.cross(right, forward)
    return right, up / np.linalg.norm(up), forward


class Scene:
    """The model as one triangle soup in world space, with what the views shade by."""

    def __init__(self, model):
        positions, normals, faces, colours, densities, two_sided = [], [], [], [], [], []
        offset = 0
        for p in model.primitives:
            if p.mode != 4 or not len(p.indices):
                continue
            world = p.world_positions
            linear = p.world[:3, :3]
            try:
                normal_matrix = np.linalg.inv(linear).T
            except np.linalg.LinAlgError:
                normal_matrix = linear
            n = (p.normals @ normal_matrix.T) if p.normals is not None else np.zeros_like(world)
            with np.errstate(invalid="ignore", divide="ignore"):
                n = np.nan_to_num(n / np.linalg.norm(n, axis=1, keepdims=True))
            positions.append(world)
            normals.append(n)
            faces.append(p.indices + offset)
            offset += len(world)
            colours.append(_albedo(model, p))
            material = model.materials[p.material] if 0 <= p.material < len(model.materials) else {}
            two_sided.append(np.full(len(p.indices), bool(material.get("doubleSided"))))
            image = checks._texture_of(model, p.material)
            if p.uvs is not None and image and image["width"]:
                density, _, _ = checks.texel_density(p, image)
            else:
                density = np.full(len(p.indices), np.nan)
            densities.append(density)
        self.positions = np.concatenate(positions) if positions else np.zeros((0, 3))
        self.normals = np.concatenate(normals) if normals else np.zeros((0, 3))
        self.faces = np.concatenate(faces) if faces else np.zeros((0, 3), dtype=np.int64)
        self.albedo = np.concatenate(colours) if colours else np.zeros((0, 3))
        self.density = np.concatenate(densities) if densities else np.zeros(0)
        self.two_sided = np.concatenate(two_sided) if two_sided else np.zeros(0, dtype=bool)
        corners = self.positions[self.faces] if len(self.faces) else np.zeros((0, 3, 3))
        cross = np.cross(corners[:, 1] - corners[:, 0], corners[:, 2] - corners[:, 0])
        with np.errstate(invalid="ignore", divide="ignore"):
            self.face_normals = np.nan_to_num(cross / np.linalg.norm(cross, axis=1, keepdims=True))
        self.shading_normals = np.nan_to_num(self.normals[self.faces].mean(axis=1)) if len(self.faces) else np.zeros((0, 3))
        low, high = (self.positions.min(axis=0), self.positions.max(axis=0)) if len(self.positions) else (np.zeros(3), np.ones(3))
        self.centre, self.extent = (low + high) / 2, float(np.linalg.norm(high - low)) or 1.0
        self.low, self.high = low, high


def _albedo(model, p):
    """A colour per face: the base colour texture at the face's centre times the factor."""
    material = model.materials[p.material] if 0 <= p.material < len(model.materials) else {}
    factor = np.array(material.get("pbrMetallicRoughness", {}).get("baseColorFactor", [1, 1, 1, 1])[:3])
    colour = np.tile(factor, (len(p.indices), 1))
    image = checks._texture_of(model, p.material)
    if image and image.get("data") and p.uvs is not None:
        try:
            texture = np.asarray(Image.open(io.BytesIO(image["data"])).convert("RGB"), dtype=np.float64) / 255
        except OSError:
            return colour
        uv = p.uvs[p.indices].mean(axis=1)
        uv = uv - np.floor(uv)
        x = np.clip((uv[:, 0] * texture.shape[1]).astype(int), 0, texture.shape[1] - 1)
        y = np.clip((uv[:, 1] * texture.shape[0]).astype(int), 0, texture.shape[0] - 1)
        colour = colour * texture[y, x]
    emissive = np.array(material.get("emissiveFactor", [0, 0, 0]), dtype=np.float64)
    glow = checks._texture_of(model, p.material, "emissiveTexture")
    if emissive.any() and glow and glow.get("data") and p.uvs is not None:
        try:
            texture = np.asarray(Image.open(io.BytesIO(glow["data"])).convert("RGB"), dtype=np.float64) / 255
            uv = p.uvs[p.indices].mean(axis=1)
            uv = uv - np.floor(uv)
            x = np.clip((uv[:, 0] * texture.shape[1]).astype(int), 0, texture.shape[1] - 1)
            y = np.clip((uv[:, 1] * texture.shape[0]).astype(int), 0, texture.shape[0] - 1)
            colour = np.clip(colour + texture[y, x] * emissive, 0, 1)
        except OSError:
            pass
    elif emissive.any():
        colour = np.clip(colour + emissive, 0, 1)
    return colour


def render(scene, view, size=(512, 512), mode="clay", target_density=None, margin=0.06, frame=None):
    """mode: clay (shaded by the file's normals; back faces red), albedo, density, silhouette.
    `frame` is (low, high) in world space: the picture is fitted to that box
    (a part of a map) instead of to the whole model."""
    yaw, pitch = VIEWS[view] if isinstance(view, str) else view
    right, up, forward = _basis(yaw, pitch)
    relative = scene.positions - scene.centre
    x, y, depth = relative @ right, relative @ up, relative @ forward
    image = Image.new("RGB", size, (255, 255, 255) if mode == "silhouette" else BACKGROUND)
    if not len(scene.faces):
        return image
    chosen = np.ones(len(scene.faces), dtype=bool)
    fit_x, fit_y = x, y
    if frame is not None:
        low, high = np.asarray(frame[0], dtype=np.float64), np.asarray(frame[1], dtype=np.float64)
        box = np.array([[a, b, c] for a in (low[0], high[0]) for b in (low[1], high[1]) for c in (low[2], high[2])]) - scene.centre
        fit_x, fit_y = box @ right, box @ up
    span = max(float(np.ptp(fit_x)), float(np.ptp(fit_y)), 1e-6) * (1 + margin * 2)
    scale = min(size) / span
    px = (x - (fit_x.max() + fit_x.min()) / 2) * scale + size[0] / 2
    py = size[1] / 2 - (y - (fit_y.max() + fit_y.min()) / 2) * scale
    if frame is not None:
        # Everything is drawn, so nearer things hide what they hide in the game; only what is off the picture is skipped.
        fx, fy = px[scene.faces], py[scene.faces]
        chosen = (fx.max(axis=1) >= 0) & (fx.min(axis=1) <= size[0]) & (fy.max(axis=1) >= 0) & (fy.min(axis=1) <= size[1])
    draw = ImageDraw.Draw(image)
    facing = scene.face_normals @ -forward
    # Far to near; of two faces in one place (a sheet made two-sided by doubling it) the one facing the camera wins.
    order = np.argsort(-(depth[scene.faces].mean(axis=1) + np.where(facing < 0, scene.extent * 1e-4, 0.0)))
    order = order[chosen[order]]
    if mode == "silhouette":
        colours = np.zeros((len(scene.faces), 3))
    else:
        # Lit from over the camera's left shoulder, so every view is lit the same way.
        light = -forward * 0.6 + up * 0.65 - right * 0.4
        light /= np.linalg.norm(light)
        shading = np.where((scene.two_sided & (facing < 0))[:, None], -scene.shading_normals, scene.shading_normals)
        lit = np.clip(shading @ light, 0, 1) * 0.8 + np.clip(shading @ -forward, 0, 1) * 0.1 + 0.16
        if mode == "clay":
            colours = np.outer(lit, [0.82, 0.8, 0.78])
            # A one-sided face seen from behind is not drawn by the engine: a hole, or a flipped face.
            colours[(facing < -1e-6) & ~scene.two_sided] = [0.85, 0.15, 0.2]
        elif mode == "albedo":
            colours = scene.albedo * (0.55 + 0.6 * lit)[:, None]
        else:  # density: blue below the target, green at it, red above
            with np.errstate(divide="ignore", invalid="ignore"):
                ratio = np.log2(np.where(np.isfinite(scene.density), scene.density, np.nan)
                                / (target_density or np.nanmedian(scene.density) or 1))
            t = np.clip(np.nan_to_num(ratio, nan=0.0), -1.5, 1.5) / 1.5
            colours = np.stack([np.clip(t, 0, 1), 1 - np.abs(t), np.clip(-t, 0, 1)], axis=1) * (0.6 + 0.4 * lit)[:, None]
            colours[~np.isfinite(scene.density)] = [0.4, 0.4, 0.4]
    rgb = (np.clip(colours, 0, 1) * 255).astype(np.uint8)
    points = np.stack([px, py], axis=1)[scene.faces]
    for index in order.tolist():
        if mode == "albedo" and facing[index] < 0 and not scene.two_sided[index]:
            continue
        triangle = points[index]
        draw.polygon([(triangle[0, 0], triangle[0, 1]), (triangle[1, 0], triangle[1, 1]), (triangle[2, 0], triangle[2, 1])],
                     fill=tuple(int(v) for v in rgb[index]))
    return image


def uv_layout(model, size=384):
    """One tile per material: its texture with the UV triangles drawn over it."""
    tiles = []
    for index, material in enumerate(model.materials):
        parts = [p for p in model.primitives if p.material == index and p.uvs is not None and len(p.indices)]
        image = checks._texture_of(model, index)
        if not parts or not image:
            continue
        if image and image.get("data"):
            try:
                tile = Image.open(io.BytesIO(image["data"])).convert("RGB").resize((size, size), Image.NEAREST)
                tile = Image.blend(tile, Image.new("RGB", (size, size), BACKGROUND), 0.35)
            except OSError:
                tile = Image.new("RGB", (size, size), BACKGROUND)
        else:
            tile = Image.new("RGB", (size, size), BACKGROUND)
        draw = ImageDraw.Draw(tile)
        for p in parts:
            uv = p.uvs[p.indices]
            uv = uv - np.floor(uv.min(axis=1, keepdims=True))  # each face drawn in the tile it starts in
            for triangle in (uv * size).tolist()[:20000]:
                draw.line([tuple(triangle[0]), tuple(triangle[1]), tuple(triangle[2]), tuple(triangle[0])], fill=(120, 230, 255))
        label = material.get("name") or f"material{index}"
        if image and image["width"]:
            label += f"  {image['width']}x{image['height']}"
        draw.rectangle([0, 0, size, 16], fill=(0, 0, 0))
        draw.text((4, 2), label, fill=(255, 255, 255))
        tiles.append(tile)
    return tiles


def _label(image, text):
    draw = ImageDraw.Draw(image)
    draw.rectangle([0, 0, image.width, 15], fill=(0, 0, 0))
    draw.text((4, 2), text, fill=(255, 255, 255))
    return image


def built_region(scene):
    """The box that holds the dense part of a map: where 98% of its vertices
    are, so a backdrop plane that runs to the horizon does not shrink the rest."""
    if not len(scene.positions):
        return scene.low, scene.high
    low, high = np.percentile(scene.positions, 1, axis=0), np.percentile(scene.positions, 99, axis=0)
    pad = (high - low) * 0.03
    low, high = low - pad, high + pad
    low[1], high[1] = scene.low[1], scene.high[1]
    return np.maximum(low, scene.low), np.minimum(high, scene.high)


def _map_tiles(scene, cell, target_density):
    """A map is judged from where the game looks at it: the whole built region,
    then that region cut into parts, each as form and as textures."""
    low, high = built_region(scene)
    frame = (low, high)
    tiles = [
        _label(render(scene, "top", (cell, cell), "clay", frame=frame), "form: top  (red = one-sided face seen from behind)"),
        _label(render(scene, MAP_CAMERA, (cell, cell), "clay", frame=frame), "form: game camera"),
        _label(render(scene, MAP_CAMERA, (cell, cell), "albedo", frame=frame), "textures: game camera"),
        _label(render(scene, "top", (cell, cell), "density", target_density, frame=frame), "texel density (green = on target)"),
    ]
    width, depth = float(high[0] - low[0]), float(high[2] - low[2])
    columns, rows = (3, 2) if width >= depth else (2, 3)
    for row in range(rows):
        for column in range(columns):
            part_low = np.array([low[0] + width * column / columns, low[1], low[2] + depth * row / rows])
            part_high = np.array([low[0] + width * (column + 1) / columns, high[1], low[2] + depth * (row + 1) / rows])
            # Only as tall as what stands in this part, so towers elsewhere do not shrink it.
            inside = ((scene.positions >= part_low) & (scene.positions <= part_high)).all(axis=1)
            if not inside.any():
                continue
            part_low[1], part_high[1] = np.percentile(scene.positions[inside, 1], 10), scene.positions[inside, 1].max()
            name = f"part {row * columns + column + 1} (x {part_low[0]:.0f}..{part_high[0]:.0f}, z {part_low[2]:.0f}..{part_high[2]:.0f})"
            part = (part_low, part_high)
            tiles.append(_label(render(scene, MAP_CAMERA, (cell, cell), "clay", frame=part), f"form: {name}"))
            tiles.append(_label(render(scene, MAP_CAMERA, (cell, cell), "albedo", frame=part), f"textures: {name}"))
    return tiles


def sheet(model, title, lines, target_density=None, hero_height=1.5, extra=(), layout="object"):
    """The review sheet: one image to look at. `lines` are printed down the side
    (the model's numbers and the findings); `extra` are more images (engine shots).
    `layout` "map" shows a map's geometry from the game's side, part by part."""
    scene = Scene(model)
    cell = 384
    if layout == "map":
        cell = 512
        tiles = _map_tiles(scene, cell, target_density)  # no UV layout: a map's textures tile
        return _compose(tiles, cell, title, lines, extra)
    tiles = []
    for view in ("front", "right", "back", "three-quarter", "top", "game camera"):
        tiles.append(_label(render(scene, view, (cell, cell), "clay"), f"form: {view}  (red = one-sided face seen from behind)"))
    for view in ("three-quarter", "game camera"):
        tiles.append(_label(render(scene, view, (cell, cell), "albedo"), f"textures: {view}"))
    tiles.append(_label(render(scene, "three-quarter", (cell, cell), "density", target_density), "texel density (green = on target)"))
    # Silhouettes: large, and at the size it has on screen next to a hero.
    silhouette = Image.new("RGB", (cell, cell), (255, 255, 255))
    silhouette.paste(render(scene, "game camera", (cell - 120, cell - 120), "silhouette"), (0, 60))
    height = max(float(scene.high[1] - scene.low[1]), 1e-6)
    for row, pixels_per_unit in enumerate((32, 16)):  # a close and a wide game camera
        small = int(max(8, min(110, height * pixels_per_unit)))
        thumb = render(scene, "game camera", (small, small), "silhouette", margin=0)
        silhouette.paste(thumb, (cell - 115, 30 + row * 130))
        hero = int(hero_height * pixels_per_unit)
        ImageDraw.Draw(silhouette).rectangle([cell - 115 + small + 4, 30 + row * 130 + small - hero,
                                              cell - 115 + small + 4 + max(3, hero // 3), 30 + row * 130 + small], fill=(200, 60, 60))
    tiles.append(_label(silhouette, "silhouette, and at game size (red bar = a hero)"))
    tiles.extend(uv_layout(model, cell))
    return _compose(tiles, cell, title, lines, extra)


def _compose(tiles, cell, title, lines, extra):
    for image, caption in extra:
        tiles.append(_label(image.convert("RGB").resize((cell, int(cell * image.height / image.width))).crop((0, 0, cell, cell)), caption))
    columns = 4
    rows = (len(tiles) + columns - 1) // columns
    side = 520
    out = Image.new("RGB", (columns * cell + side, max(rows * cell, 40 + 15 * (len(lines) + 2))), (20, 21, 28))
    for index, tile in enumerate(tiles):
        out.paste(tile, ((index % columns) * cell, (index // columns) * cell))
    draw = ImageDraw.Draw(out)
    draw.text((columns * cell + 12, 10), title, fill=(255, 220, 140))
    for row, line in enumerate(lines):
        colour = (255, 120, 120) if line.startswith("error") else (255, 210, 120) if line.startswith("warn") else (200, 204, 216)
        draw.text((columns * cell + 12, 34 + row * 15), line[:84], fill=colour)
    return out
