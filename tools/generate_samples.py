#!/usr/bin/env python3
"""Generate original placeholder art, maps and audio for the engine's games.

Pure standard library. Geometry is in tiles/metres, Y up; character sprites
use 32 pixels per tile. Everything here is procedurally drawn for f-engine;
no third-party game assets are used. Maps are written in format 2; the
placements, zones and props sections are preserved when a map is regenerated
so hand-authored content survives.
"""
import argparse
import json
import math
import random
import struct
import wave
import zlib
from pathlib import Path

REPO = Path(__file__).resolve().parents[1]


# --------------------------------------------------------------------------- files

def write_json(path, value):
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(value, indent=2) + "\n")


def png(path, width, height, pixels):
    def chunk(kind, data):
        return struct.pack(">I", len(data)) + kind + data + struct.pack(">I", zlib.crc32(kind + data))
    rows = b"".join(b"\0" + bytes(pixels[y * width * 4:(y + 1) * width * 4]) for y in range(height))
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 6, 0, 0, 0)) +
                     chunk(b"IDAT", zlib.compress(rows, 9)) + chunk(b"IEND", b""))


class Canvas:
    def __init__(self, width, height):
        self.width, self.height = width, height
        self.pixels = bytearray(width * height * 4)

    def rect(self, x, y, w, h, color):
        for yy in range(max(0, y), min(self.height, y + h)):
            for xx in range(max(0, x), min(self.width, x + w)):
                i = (yy * self.width + xx) * 4
                self.pixels[i:i + 4] = bytes((*color[:3], color[3] if len(color) > 3 else 255))

    def ellipse(self, cx, cy, rx, ry, color):
        for yy in range(int(cy - ry), int(cy + ry) + 1):
            for xx in range(int(cx - rx), int(cx + rx) + 1):
                if ((xx - cx) / max(rx, .5)) ** 2 + ((yy - cy) / max(ry, .5)) ** 2 <= 1:
                    self.rect(xx, yy, 1, 1, color)

    def save(self, path):
        png(path, self.width, self.height, self.pixels)


# --------------------------------------------------------------------------- geometry

class Diorama:
    """Collects coloured triangles and writes a binary glTF (GLB)."""

    def __init__(self):
        self.vertices = {"opaque": [], "blend": [], "water": []}
        self.indices = {"opaque": [], "blend": [], "water": []}
        self.extra_nodes = []

    def triangle(self, points, color, channel="opaque"):
        a, b, c = points
        u, v = [b[i] - a[i] for i in range(3)], [c[i] - a[i] for i in range(3)]
        n = [u[1] * v[2] - u[2] * v[1], u[2] * v[0] - u[0] * v[2], u[0] * v[1] - u[1] * v[0]]
        length = math.sqrt(sum(x * x for x in n)) or 1
        n = [x / length for x in n]
        for point, uv in zip(points, [(0, 0), (1, 0), (1, 1)]):
            self.indices[channel].append(len(self.vertices[channel]))
            linear = tuple(max(0, c) ** 2.2 for c in color[:3]) + (color[3] if len(color) > 3 else 1,)
            self.vertices[channel].append((*point, *n, *uv, *linear))

    def quad(self, points, color, channel="opaque"):
        self.triangle([points[0], points[1], points[2]], color, channel)
        self.triangle([points[0], points[2], points[3]], color, channel)

    def box(self, x, y, z, w, h, d, color, top=None):
        x0, x1, z0, z1 = x - w / 2, x + w / 2, z - d / 2, z + d / 2
        self.quad([(x0, y + h, z0), (x0, y + h, z1), (x1, y + h, z1), (x1, y + h, z0)], top or color)
        for points, shade in [([(x0, y, z1), (x1, y, z1), (x1, y + h, z1), (x0, y + h, z1)], .86),
                              ([(x1, y, z0), (x0, y, z0), (x0, y + h, z0), (x1, y + h, z0)], .66),
                              ([(x0, y, z0), (x0, y, z1), (x0, y + h, z1), (x0, y + h, z0)], .76),
                              ([(x1, y, z1), (x1, y, z0), (x1, y + h, z0), (x1, y + h, z1)], .94)]:
            self.quad(points, tuple(c * shade for c in color[:3]) + (1,))

    def ramp(self, x0, z0, x1, z1, y0, y1, axis, color):
        """A slope rising from y0 to y1 along +x (axis 'x') or +z (axis 'z')."""
        if axis == "x":
            pts = [(x0, y0, z0), (x0, y0, z1), (x1, y1, z1), (x1, y1, z0)]
        else:
            pts = [(x0, y0, z0), (x0, y1, z1), (x1, y1, z1), (x1, y0, z0)]
        self.quad(pts, color)

    def roof(self, x, y, z, w, h, d, color=(.42, .2, .18)):
        self.quad([(x - w / 2, y, z - d / 2), (x, y + h, z - d / 2), (x, y + h, z + d / 2), (x - w / 2, y, z + d / 2)], color)
        self.quad([(x, y + h, z - d / 2), (x + w / 2, y, z - d / 2), (x + w / 2, y, z + d / 2), (x, y + h, z + d / 2)],
                  tuple(min(1, c * 1.25) for c in color))
        for zz in (z - d / 2, z + d / 2):
            self.triangle([(x - w / 2, y, zz), (x + w / 2, y, zz), (x, y + h, zz)], (.62, .44, .31))

    def tree(self, x, z, height=1.6, color=(.2, .36, .22)):
        self.box(x, 0, z, .25, height * .7, .25, (.32, .22, .15))
        self.box(x, height * .55, z, 1.5, height * .7, 1.4, color)
        self.box(x, height * 1.1, z, .9, height * .45, .9, tuple(min(1, c * 1.15) for c in color))

    def write(self, path):
        binary = bytearray()
        views, accessors, primitives = [], [], []
        materials = {"opaque": 0, "blend": 1, "water": 2}
        for channel in ("opaque", "blend", "water"):
            verts, indices = self.vertices[channel], self.indices[channel]
            if not verts:
                continue
            offset = len(binary)
            for vertex in verts:
                binary.extend(struct.pack("<12f", *vertex))
            view_index = len(views)
            views.append({"buffer": 0, "byteOffset": offset, "byteLength": len(verts) * 48, "byteStride": 48, "target": 34962})
            attributes = {}
            for name, byte_offset, kind, count in [("POSITION", 0, "VEC3", 3), ("NORMAL", 12, "VEC3", 3),
                                                   ("TEXCOORD_0", 24, "VEC2", 2), ("COLOR_0", 32, "VEC4", 4)]:
                accessor = {"bufferView": view_index, "byteOffset": byte_offset, "componentType": 5126,
                            "count": len(verts), "type": kind}
                if name == "POSITION":
                    accessor["min"] = [min(v[i] for v in verts) for i in range(count)]
                    accessor["max"] = [max(v[i] for v in verts) for i in range(count)]
                attributes[name] = len(accessors)
                accessors.append(accessor)
            offset = len(binary)
            binary.extend(struct.pack(f"<{len(indices)}I", *indices))
            views.append({"buffer": 0, "byteOffset": offset, "byteLength": len(indices) * 4, "target": 34963})
            index = len(accessors)
            accessors.append({"bufferView": len(views) - 1, "componentType": 5125, "count": len(indices), "type": "SCALAR"})
            primitives.append({"attributes": attributes, "indices": index, "material": materials[channel]})
        nodes = [{"mesh": 0, "name": "scenery"}] + self.extra_nodes
        doc = {"asset": {"version": "2.0", "generator": "f-engine sample generator"}, "scene": 0,
               "scenes": [{"nodes": list(range(len(nodes)))}], "nodes": nodes, "meshes": [{"primitives": primitives}],
               "buffers": [{"byteLength": len(binary)}], "bufferViews": views, "accessors": accessors,
               "materials": [{"pbrMetallicRoughness": {"baseColorFactor": [1, 1, 1, 1]}, "doubleSided": True},
                             {"pbrMetallicRoughness": {"baseColorFactor": [1, 1, 1, 1]}, "alphaMode": "BLEND", "doubleSided": True},
                             {"pbrMetallicRoughness": {"baseColorFactor": [1, 1, 1, 1]}, "alphaMode": "BLEND",
                              "extras": {"fengine": {"material": "water"}}}]}
        metadata = json.dumps(doc, separators=(",", ":")).encode()
        metadata += b" " * (-len(metadata) % 4)
        binary += b"\0" * (-len(binary) % 4)
        total = 12 + 8 + len(metadata) + 8 + len(binary)
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(struct.pack("<III", 0x46546C67, 2, total) + struct.pack("<II", len(metadata), 0x4E4F534A) +
                         metadata + struct.pack("<II", len(binary), 0x004E4942) + binary)


# --------------------------------------------------------------------------- grids and maps

class GridBuilder:
    def __init__(self, width, depth, layers=1):
        self.width, self.depth = width, depth
        self.tags = []
        self.layers = [{"height": [[0.0] * width for _ in range(depth)], "walk": [[l == 0] * width for _ in range(depth)],
                        "ramp": [["."] * width for _ in range(depth)], "tags": [[0] * width for _ in range(depth)]}
                       for l in range(layers)]
        self.links = []
        self.water = []  # (x0, z0, x1, z1, surface height) from water_patch, drawn by terrain()

    def tag_bit(self, name):
        if name not in self.tags:
            self.tags.append(name)
        return 1 << self.tags.index(name)

    def fill(self, x0, z0, x1, z1, layer=0, height=None, walk=None, tag=None, ramp=None):
        for z in range(max(0, z0), min(self.depth, z1)):
            for x in range(max(0, x0), min(self.width, x1)):
                L = self.layers[layer]
                if height is not None:
                    L["height"][z][x] = height
                if walk is not None:
                    L["walk"][z][x] = walk
                if tag is not None:
                    L["tags"][z][x] |= self.tag_bit(tag)
                if ramp is not None:
                    L["ramp"][z][x] = ramp

    def json(self):
        layers = []
        for i, L in enumerate(self.layers):
            out = {"name": "ground" if i == 0 else f"layer{i}"}
            out["height"] = [" ".join(f"{h:g}" for h in row) for row in L["height"]]
            out["walk"] = ["".join("1" if w else "0" for w in row) for row in L["walk"]]
            if any(c != "." for row in L["ramp"] for c in row):
                out["ramp"] = ["".join(row) for row in L["ramp"]]
            if any(t for row in L["tags"] for t in row):
                out["tags"] = [" ".join(format(t, "x") for t in row) for row in L["tags"]]
            layers.append(out)
        result = {"width": self.width, "depth": self.depth, "layers": layers}
        if self.tags:
            result["tags"] = self.tags
        if self.links:
            result["links"] = self.links
        return result


def write_map(folder, map_id, name, grid, scene, environment, points, lights=(), emitters=(), keep=True,
              placements=(), zones=(), props=None):
    target = folder / "maps" / map_id / "map.json"
    scene.write(folder / "maps" / map_id / "geometry.glb")
    authored = {"placements": list(placements), "zones": list(zones), "props": props or {}}
    if keep and target.exists():
        previous = json.loads(target.read_text())
        for key in authored:
            if key in previous:
                authored[key] = previous[key]
    document = {"format": 2, "id": map_id, "name": name, "geometry": "geometry.glb", "grid": grid.json(),
                "points": points, **authored, "lights": list(lights), "emitters": list(emitters),
                "colliders": [], "environment": environment}
    write_json(target, document)
    return document


# --------------------------------------------------------------------------- terrain helpers

PALETTE = {"grass": (.30, .46, .26), "path": (.58, .47, .32), "stone": (.46, .45, .44), "cave": (.24, .27, .33),
           "sand": (.74, .66, .48), "cliff": (.45, .39, .31), "snow": (.85, .88, .92), "wood": (.48, .33, .2),
           "silt": (.30, .29, .22)}


def terrain(grid, colour_of, rng):
    """Top surfaces (following heights and ramps), cliff walls between levels
    and the surfaces of the water basins carved by water_patch()."""
    scene = Diorama()
    L = grid.layers[0]
    submerged = {(x, z) for x0, z0, x1, z1, _ in grid.water for x in range(x0, x1) for z in range(z0, z1)}
    for z in range(grid.depth):
        for x in range(grid.width):
            h = L["height"][z][x]
            ramp = L["ramp"][z][x]
            base = PALETTE["silt"] if (x, z) in submerged else colour_of(x, z)
            colour = tuple(v * rng.uniform(0.93, 1.06) for v in base)
            corners = {"nw": h, "ne": h, "sw": h, "se": h}
            if ramp == "e":
                corners.update(ne=h + 1, se=h + 1)
            elif ramp == "w":
                corners.update(nw=h + 1, sw=h + 1)
            elif ramp == "n":
                corners.update(nw=h + 1, ne=h + 1)
            elif ramp == "s":
                corners.update(sw=h + 1, se=h + 1)
            scene.quad([(x, corners["nw"], z), (x, corners["sw"], z + 1), (x + 1, corners["se"], z + 1),
                        (x + 1, corners["ne"], z)], colour)
            for dx, dz in ((1, 0), (0, 1)):
                nx, nz = x + dx, z + dz
                if nx >= grid.width or nz >= grid.depth:
                    continue
                other = L["height"][nz][nx]
                if L["ramp"][nz][nx] != "." or ramp != ".":
                    continue
                if abs(other - h) > 0.01:
                    lo, hi = sorted((h, other))
                    wall = tuple(v * 0.85 for v in PALETTE["cliff"])
                    if dx:
                        scene.quad([(x + 1, lo, z), (x + 1, lo, z + 1), (x + 1, hi, z + 1), (x + 1, hi, z)], wall)
                    else:
                        scene.quad([(x, lo, z + 1), (x + 1, lo, z + 1), (x + 1, hi, z + 1), (x, hi, z + 1)], wall)
    for x0, z0, x1, z1, y in grid.water:
        scene.quad([(x0, y, z0), (x0, y, z1), (x1, y, z1), (x1, y, z0)], (.16, .45, .62, .8), "water")
    return scene


def house(scene, grid, x, z, w, d, wall=(.74, .62, .45), roof=(.45, .22, .2)):
    scene.box(x + w / 2, 0, z + d / 2, w, 2.2, d, wall)
    scene.roof(x + w / 2, 2.2, z + d / 2, w + 0.4, 1.4, d + 0.4, roof)
    scene.box(x + w / 2, 0.05, z + d + 0.02, 0.9, 1.4, 0.05, (.25, .17, .12))
    for wx in (x + 0.8, x + w - 0.8):
        scene.box(wx, 1.0, z + d + 0.03, 0.6, 0.6, 0.05, (1.3, .9, .4))
    grid.fill(x, z, x + w, z + d, walk=False)


def tree(scene, grid, x, z, colour=(.2, .38, .22)):
    scene.tree(x + 0.5, z + 0.5, 1.8, colour)
    grid.fill(x, z, x + 1, z + 1, walk=False)


def lamp(scene, x, z, lights, colour=(1, .62, .26)):
    scene.box(x, 0, z, 0.16, 1.9, 0.16, (.22, .2, .2))
    scene.box(x, 1.9, z, 0.32, 0.4, 0.32, (2.2, 1.5, .6))
    lights.append({"position": [x, 2.1, z], "color": list(colour), "radius": 5.5, "intensity": 2.6, "flicker": 0.08})


def water_patch(grid, x0, z0, x1, z1, depth=0.25, surface=-0.08):
    """Carve a basin `depth` below the ground (y = 0) and fill it with water up to
    `surface`, between the basin floor and the bank top, so it reads as recessed.
    Call before terrain(), which draws the basin and the water."""
    grid.fill(x0, z0, x1, z1, walk=False, height=-depth, tag="water")
    grid.water.append((x0, z0, x1, z1, surface))


def stage_points(cx=8.0, cz=7.5):
    points = {"centre": {"position": [cx, 0, cz]}}
    for i in range(4):
        points[f"party{i + 1}"] = {"position": [cx + 3.4 + i * 0.4, 0, cz - 1.8 + i * 1.25], "facing": [-1, 0]}
        points[f"enemy{i + 1}"] = {"position": [cx - 3.5 - (i // 2) * 1.3, 0, cz - 1.6 + (i % 2) * 2.4 + (i // 2) * 0.6],
                                   "facing": [1, 0]}
    return points


def battle_stage(folder, map_id, name, ground, decoration, environment):
    grid = GridBuilder(16, 15)
    rng = random.Random(map_id)
    scene = terrain(grid, lambda x, z: PALETTE[ground], rng)
    decoration(scene, rng)
    write_map(folder, map_id, name, grid, scene, environment, stage_points(), keep=False)


def npc(id_, x, z, sprite, props=None, facing=(0, 1)):
    p = {"id": id_, "type": "npc", "position": [x + 0.5, 0, z + 0.5], "facing": list(facing),
         "props": {"sprite": f"sprites/{sprite}.anim.json", **(props or {})}}
    return p


def door(id_, x, z, map_id, point, trigger="touch"):
    return {"id": id_, "type": "door", "position": [x + 0.5, 0, z + 0.5], "props": {"map": map_id, "point": point, "trigger": trigger}}


def zone(id_, kind, x0, z0, x1, z1, props):
    return {"id": id_, "type": kind, "min": [x0, z0], "max": [x1, z1], "props": props}


# --------------------------------------------------------------------------- Vale (classic ATB side-view)

def vale():
    folder = REPO / "samples" / "vale"
    looks = {
        "aster": {"coat": (178, 61, 66), "hair": (83, 53, 40), "weapon": (200, 210, 230)},
        "mira": {"coat": (49, 126, 142), "hair": (235, 197, 118), "trim": (240, 240, 250)},
        "elder": {"coat": (118, 93, 157), "hair": (204, 203, 197)},
        "merchant": {"coat": (89, 142, 86), "hair": (104, 64, 40), "hat": (120, 80, 40)},
        "innkeeper": {"coat": (74, 109, 163), "hair": (124, 76, 44)},
        "villager": {"coat": (160, 120, 80), "hair": (60, 40, 30)},
    }
    for name, look in looks.items():
        sprite_sheet(folder, name, look)
    for name in ("aster", "mira", "elder"):
        portrait(folder, name, looks[name])
    sprite_sheet(folder, "slime", {"kind": "slime", "coat": (86, 177, 132)}, monster=True)
    sprite_sheet(folder, "wisp", {"kind": "wisp", "coat": (120, 170, 255)}, monster=True)
    sprite_sheet(folder, "golem", {"kind": "golem", "coat": (106, 112, 138)}, monster=True)
    chest_sprite(folder)
    crystal_sprite(folder, (90, 190, 255))
    standard_audio(folder, {"vale": (262, "major", 96), "grotto": (220, "minor", 80), "battle": (220, "minor", 150),
                            "boss": (196, "minor", 132), "title": (294, "major", 72)})
    lut(folder / "ui" / "amber.cube", (1.04, 1.0, 0.94), 1.05)
    lut(folder / "ui" / "grotto.cube", (0.95, 1.0, 1.05), 1.1)
    backdrop(folder / "ui" / "hills.png", (84, 110, 96), 3, 4)

    # Town: houses, pond, lamps, an exit north to the grotto.
    rng = random.Random(11)
    grid = GridBuilder(20, 16)
    paths = lambda x, z: PALETTE["path"] if (8 <= x <= 11 or 7 <= z <= 8) else PALETTE["grass"]
    grid.fill(0, 0, 20, 1, walk=False); grid.fill(0, 15, 20, 16, walk=False)
    grid.fill(0, 0, 1, 16, walk=False); grid.fill(19, 0, 20, 16, walk=False)
    grid.fill(9, 0, 11, 1, walk=True)
    grid.fill(9, 15, 11, 16, walk=True)
    lights = []
    water_patch(grid, 14, 10, 18, 13)
    scene = terrain(grid, paths, rng)
    house(scene, grid, 2, 2, 4, 3)
    house(scene, grid, 13, 2, 5, 3, wall=(.66, .58, .52), roof=(.28, .3, .44))
    house(scene, grid, 2, 10, 4, 3, wall=(.7, .6, .5), roof=(.4, .3, .18))
    for x, z in [(1, 6), (18, 6), (6, 13), (12, 14), (1, 13)]:
        tree(scene, grid, x, z)
    for x, z in [(7.5, 6.5), (12.5, 9.5), (8.5, 12.5)]:
        lamp(scene, x, z, lights)
    # A Blender-style marker node: the editor's "Import markers" turns it into a point.
    scene.extra_nodes.append({"name": "well", "translation": [6.5, 0, 8.5], "extras": {"fengine": {"spawn": "well"}}})
    placements = [
        npc("elder", 5, 6, "elder", {"name": "Elder Rowan", "pages": [
            {"script": "events.elder", "trigger": "action"}]}),
        npc("merchant", 14, 6, "merchant", {"name": "Merchant", "script": "events.merchant"}),
        npc("innkeeper", 3, 6, "innkeeper", {"name": "Innkeeper", "script": "events.inn"}, facing=(1, 0)),
        npc("villager", 11, 11, "villager", {"name": "Villager", "routine": "wander", "script": "events.villager"}),
        {"id": "town_chest", "type": "chest", "position": [16.5, 0, 8.5], "props": {"item": "potion", "count": 2,
                                                                                     "sprite": "sprites/chest.anim.json"}},
        {"id": "sign", "type": "sign", "position": [11.5, 0, 13.5], "props": {"text": "North: the Crystal Grotto."}},
        {"id": "save", "type": "save_point", "position": [9.5, 0, 9.5], "props": {"sprite": "sprites/crystal.anim.json"}},
        door("to_grotto", 9, 0, "grotto", "entry"),
        door("to_grotto2", 10, 0, "grotto", "entry"),
        {"id": "fountain", "type": "emitter", "position": [16, 0.2, 11.5],
         "props": {"rate": 14, "lifetime": 1.2, "velocity": [0, 1.4, 0], "velocity_jitter": [0.3, 0.2, 0.3],
                   "gravity": [0, -2.4, 0], "size": 0.06, "color": [0.7, 0.85, 1, 0.8], "color_end": [0.7, 0.85, 1, 0]}},
    ]
    zones = [zone("plaza_cam", "camera", 6, 5, 14, 11, {"distance": 15, "pitch": 38})]
    write_map(folder, "town", "Amber Vale", grid, scene, {
        "ambient": [.42, .42, .4], "sun_color": [1, .86, .66], "sun_direction": [-.6, -1, -.4],
        "fog_color": [.62, .7, .78], "fog_density": .012,
        "sky": {"mode": "gradient", "top": [.28, .46, .78], "horizon": [.82, .84, .86], "cloud_cover": .45,
                "backdrops": [{"image": "ui/hills.png", "distance": 45, "height": 14, "width": 90, "yaw": 0,
                               "elevation": -2, "parallax": 0.3}]},
        "weather": {"kind": "leaves", "intensity": .6},
        "post": {"bloom_strength": .35, "focus": .55, "focus_range": .22, "lut": "ui/amber.cube"}},
        {"entry": {"position": [9.5, 0, 14.5], "facing": [0, -1]}, "return": {"position": [9.5, 0, 1.8], "facing": [0, 1]}},
        lights=lights, placements=placements, zones=zones,
        props={"music": "vale", "battle_stage": "battle_grass", "camera": {"pitch": 42, "distance": 18, "fov": 30,
                                                                          "min": [6, 5], "max": [14, 11]}})

    # Grotto: cave walls, glowing crystals, random encounters, the guardian.
    rng = random.Random(12)
    grid = GridBuilder(16, 16)
    grid.fill(0, 0, 16, 1, walk=False); grid.fill(0, 15, 16, 16, walk=False)
    grid.fill(0, 0, 1, 16, walk=False); grid.fill(15, 0, 16, 16, walk=False)
    grid.fill(7, 15, 9, 16, walk=True)
    grid.fill(5, 4, 11, 7, height=0.5)
    grid.fill(5, 7, 11, 8, ramp="n")
    grid.fill(1, 8, 15, 15, tag="encounter:grotto")
    lights = []
    scene = terrain(grid, lambda x, z: PALETTE["cave"], rng)
    for x in range(16):
        for z in (0, 15):
            if not (z == 15 and 7 <= x <= 8):
                scene.box(x + .5, 0, z + .5, 1, rng.uniform(1.4, 3), 1, (.22, .25, .32))
    for z in range(1, 15):
        for x in (0, 15):
            scene.box(x + .5, 0, z + .5, 1, rng.uniform(1.4, 3), 1, (.22, .25, .32))
    for x, z in [(3, 3), (12, 3), (3, 11), (12, 11)]:
        scene.box(x + .5, 0, z + .5, 1.2, 2.2, 1.2, (.24, .28, .36))
        scene.box(x + .5, 2.2, z + .5, .35, .7, .35, (.3, 1.2, 2.2))
        grid.fill(x, z, x + 1, z + 1, walk=False)
        lights.append({"position": [x + .5, 2.8, z + .5], "color": [.35, .75, 1], "radius": 7, "intensity": 3, "flicker": .15})
    placements = [
        {"id": "guardian", "type": "npc", "position": [8, 0.5, 5], "facing": [0, 1],
         "props": {"name": "Guardian", "sprite": "sprites/golem.anim.json", "pages": [
             {"script": "events.guardian", "trigger": "action"},
             {"when": {"flag": "guardian_defeated"}, "hidden": True}]}},
        {"id": "vale_crystal", "type": "npc", "position": [8, 0.5, 4.2],
         "props": {"name": "Vale Crystal", "sprite": "sprites/crystal.anim.json", "script": "events.crystal", "solid": True}},
        {"id": "grotto_chest", "type": "chest", "position": [2.5, 0, 6.5], "props": {"item": "ether", "count": 1,
                                                                                      "sprite": "sprites/chest.anim.json"}},
        door("exit", 7, 15, "town", "return"), door("exit2", 8, 15, "town", "return"),
        {"id": "drip", "type": "sound", "position": [12, 1, 12], "props": {"sound": "audio/chime.wav", "radius": 6, "volume": .2}},
        {"id": "wisp_symbol", "type": "symbol", "position": [4.5, 0, 9.5], "props": {"troop": "wisps",
                                                                                      "sprite": "sprites/wisp.anim.json"}},
    ]
    write_map(folder, "grotto", "Crystal Grotto", grid, scene, {
        "ambient": [.16, .2, .32], "sun_color": [.3, .4, .6], "sun_direction": [-.3, -1, -.2], "fog_color": [.06, .09, .16],
        "fog_density": .03, "sky": {"mode": "color", "horizon": [.04, .05, .1]},
        "weather": {"kind": "dust", "intensity": .8, "color": [.6, .8, 1, .6]},
        "post": {"bloom_strength": .55, "focus": .5, "focus_range": .2, "lut": "ui/grotto.cube"}},
        {"entry": {"position": [8, 0, 14.4], "facing": [0, -1]}}, lights=lights, placements=placements,
        props={"music": "grotto", "battle_stage": "battle_cave", "camera": {"pitch": 46, "distance": 17, "fov": 30}})

    def grass_decor(scene, rng):
        for x, z in [(2, 2), (13, 2), (1, 12), (14, 12)]:
            scene.tree(x + .5, z + .5, 2.2)
    def cave_decor(scene, rng):
        for x, z in [(2, 2), (13, 2), (2, 12), (13, 12)]:
            scene.box(x + .5, 0, z + .5, 1.4, 2.6, 1.4, (.24, .28, .36))
            scene.box(x + .5, 2.6, z + .5, .3, .6, .3, (.3, 1.2, 2.2))
    battle_stage(folder, "battle_grass", "Meadow", "grass", grass_decor, {
        "ambient": [.45, .45, .42], "sun_color": [1, .9, .7], "fog_color": [.6, .7, .8], "fog_density": .01,
        "sky": {"mode": "gradient", "cloud_cover": .5}, "post": {"focus": .5, "focus_range": .3, "lut": "ui/amber.cube"}})
    battle_stage(folder, "battle_cave", "Cavern", "cave", cave_decor, {
        "ambient": [.2, .24, .36], "sun_color": [.4, .5, .7], "fog_color": [.05, .08, .14], "fog_density": .03,
        "sky": {"mode": "color", "horizon": [.04, .05, .1]}, "post": {"bloom_strength": .5, "lut": "ui/grotto.cube"}})


# --------------------------------------------------------------------------- Wayfarers (round-order, break and boost)

def wayfarers():
    folder = REPO / "samples" / "wayfarers"
    looks = {
        "rook": {"coat": (70, 90, 60), "hair": (40, 30, 26), "weapon": (210, 210, 220), "hat": (60, 50, 40)},
        "selene": {"coat": (120, 70, 140), "hair": (230, 225, 240), "trim": (220, 200, 120)},
        "ferryman": {"coat": (80, 110, 150), "hair": (150, 150, 150)},
        "scholar": {"coat": (140, 120, 90), "hair": (90, 60, 40), "hat": (60, 60, 90)},
        "barkeep": {"coat": (150, 60, 50), "hair": (40, 30, 30)},
        "child": {"coat": (200, 160, 60), "hair": (120, 70, 30)},
    }
    for name, look in looks.items():
        sprite_sheet(folder, name, look)
    for name in ("rook", "selene"):
        portrait(folder, name, looks[name])
    sprite_sheet(folder, "wolf", {"kind": "wolf", "coat": (120, 120, 130)}, monster=True)
    sprite_sheet(folder, "bandit", {"coat": (90, 60, 50), "hair": (30, 20, 20), "hat": (40, 40, 40), "weapon": (180, 180, 190)})
    sprite_sheet(folder, "wisp", {"kind": "wisp", "coat": (255, 170, 90)}, monster=True)
    render_showcase(folder)
    chest_sprite(folder)
    crystal_sprite(folder, (255, 200, 90))
    standard_audio(folder, {"riverford": (294, "major", 100), "route": (262, "major", 116), "cliffhaven": (247, "minor", 90),
                            "battle": (233, "minor", 156), "title": (262, "minor", 70)})
    lut(folder / "ui" / "warm.cube", (1.06, 1.0, 0.92), 1.08)
    lut(folder / "ui" / "dusk.cube", (1.05, 0.9, 0.95), 1.1)
    backdrop(folder / "ui" / "ridge.png", (96, 104, 120), 5, 7)
    world = Canvas(240, 135)
    world.rect(0, 0, 240, 135, (40, 80, 120))
    world.ellipse(90, 70, 70, 45, (90, 140, 80))
    world.ellipse(170, 55, 50, 30, (120, 110, 90))
    world.rect(60, 64, 120, 3, (200, 180, 120))
    world.save(folder / "ui" / "worldmap.png")

    # Riverford: a river crossed by a bridge on grid layer 1 (walk under or over it).
    rng = random.Random(21)
    grid = GridBuilder(22, 16, layers=2)
    grid.fill(0, 0, 22, 1, walk=False); grid.fill(0, 15, 22, 16, walk=False)
    grid.fill(0, 0, 1, 16, walk=False); grid.fill(21, 0, 22, 16, walk=False)
    grid.fill(0, 7, 1, 9, walk=True); grid.fill(21, 7, 22, 9, walk=True)
    lights = []
    colour = lambda x, z: PALETTE["path"] if 7 <= z <= 8 else PALETTE["grass"]
    water_patch(grid, 10, 1, 13, 15)
    scene = terrain(grid, colour, rng)
    # Bridge deck on layer 1, ramps at both ends link from the ground.
    grid.fill(9, 7, 10, 9, layer=1, walk=True, height=0.0, ramp="e")
    grid.fill(10, 7, 13, 9, layer=1, walk=True, height=1.0)
    grid.fill(13, 7, 14, 9, layer=1, walk=True, height=0.0, ramp="w")
    for z in (7, 8):
        grid.links.append({"x": 9, "z": z, "from": 0, "to": 1, "dir": "e"})
        grid.links.append({"x": 13, "z": z, "from": 0, "to": 1, "dir": "w"})
    scene.ramp(9, 7, 10, 9, 0, 1, "x", PALETTE["wood"])
    scene.box(11.5, 0.8, 8, 3, 0.2, 2, PALETTE["wood"])
    scene.quad([(14, 0, 7), (14, 0, 9), (13, 1, 9), (13, 1, 7)], PALETTE["wood"])
    house(scene, grid, 2, 2, 5, 3)
    house(scene, grid, 15, 2, 5, 3, wall=(.6, .55, .5), roof=(.25, .35, .5))
    house(scene, grid, 3, 11, 4, 3, wall=(.72, .5, .4))
    for x, z in [(1, 5), (8, 13), (19, 12), (16, 10)]:
        tree(scene, grid, x, z)
    for x, z in [(8.5, 6.5), (14.5, 6.5), (5.5, 9.5)]:
        lamp(scene, x, z, lights)
    placements = [
        npc("ferryman", 8, 9, "ferryman", {"name": "Ferryman", "verbs": ["inquire", "purchase"], "level": 3,
                                           "info": "The cliffs north hide a shrine.", "reveals": "found_cliffhaven",
                                           "wares": ["tonic", "ember_charm"], "script": "events.ferryman"}),
        npc("scholar", 17, 6, "scholar", {"name": "Selene", "pages": [
            {"script": "events.selene_join", "trigger": "action"},
            {"when": {"flag": "selene_joined"}, "hidden": True}]}),
        npc("barkeep", 4, 6, "barkeep", {"name": "Barkeep", "script": "events.tavern", "pages": [
            {"script": "events.tavern", "trigger": "action", "when": {"hours": [6, 22]}},
            {"script": "events.tavern_night", "trigger": "action", "when": {"hours": [22, 6]}}]}),
        npc("child", 15, 11, "child", {"name": "Child", "routine": "wander", "verbs": ["inquire"], "level": 1,
                                       "info": "Wolves hate fire!", "script": "events.child"}),
        {"id": "river_chest", "type": "chest", "position": [11.5, 1.0, 8.5], "layer": 1,
         "props": {"item": "tonic", "count": 2, "sprite": "sprites/chest.anim.json"}},
        {"id": "save", "type": "save_point", "position": [6.5, 0, 8.5], "props": {"sprite": "sprites/crystal.anim.json"}},
        {"id": "east_road", "type": "npc", "position": [20.5, 0, 7.5],
         "props": {"name": "Signpost", "script": "events.signpost", "solid": False}},
        door("to_route", 0, 7, "route", "east"), door("to_route2", 0, 8, "route", "east"),
    ]
    write_map(folder, "riverford", "Riverford", grid, scene, {
        "ambient": [.42, .42, .4], "sun_color": [1, .88, .7], "fog_color": [.7, .74, .8], "fog_density": .01,
        "sky": {"mode": "gradient", "cloud_cover": .5,
                "backdrops": [{"image": "ui/ridge.png", "distance": 50, "height": 16, "width": 110, "yaw": 0, "elevation": -2}]},
        "post": {"lut": "ui/warm.cube", "lut_blend_target": "ui/dusk.cube", "lut_blend": 0}},
        {"west": {"position": [1.5, 0, 7.8], "facing": [1, 0]}, "start": {"position": [6.5, 0, 9.5]}}, lights=lights,
        placements=placements, props={"music": "riverford", "daynight": True, "battle_stage": "stage_forest",
                                      "camera": {"pitch": 40, "distance": 19, "fov": 30, "min": [6, 5], "max": [16, 11]}})

    # The meadow route: encounters, visible enemies and chests.
    rng = random.Random(22)
    grid = GridBuilder(26, 12)
    grid.fill(0, 0, 26, 1, walk=False); grid.fill(0, 11, 26, 12, walk=False)
    grid.fill(1, 1, 25, 11, tag="encounter:meadow")
    scene = terrain(grid, lambda x, z: PALETTE["path"] if 5 <= z <= 6 else PALETTE["grass"], rng)
    for x in range(2, 24, 3):
        tree(scene, grid, x, 2 if x % 2 else 9)
    placements = [
        {"id": "wolf_pack", "type": "symbol", "position": [14.5, 0, 8.5], "props": {"troop": "wolves",
                                                                                   "sprite": "sprites/wolf.anim.json"}},
        {"id": "route_chest", "type": "chest", "position": [20.5, 0, 3.5], "props": {"item": "ember_charm",
                                                                                     "sprite": "sprites/chest.anim.json"}},
        door("to_riverford", 25, 5, "riverford", "west"), door("to_riverford2", 25, 6, "riverford", "west"),
        door("to_cliffhaven", 0, 5, "cliffhaven", "south"), door("to_cliffhaven2", 0, 6, "cliffhaven", "south"),
    ]
    write_map(folder, "route", "Meadow Road", grid, scene, {
        "ambient": [.44, .44, .42], "sun_color": [1, .9, .75], "fog_color": [.7, .78, .85], "fog_density": .008,
        "sky": {"mode": "gradient", "cloud_cover": .6}, "weather": {"kind": "leaves", "intensity": .5},
        "post": {"lut": "ui/warm.cube"}},
        {"east": {"position": [24.2, 0, 5.8], "facing": [-1, 0]}, "west": {"position": [1.6, 0, 5.8], "facing": [1, 0]}},
        placements=placements, props={"music": "route", "battle_stage": "stage_plains", "daynight": True})

    # Cliffhaven: terraces joined by ramps (stairs).
    rng = random.Random(23)
    grid = GridBuilder(18, 16)
    grid.fill(0, 0, 18, 1, walk=False); grid.fill(0, 15, 18, 16, walk=False)
    grid.fill(0, 0, 1, 16, walk=False); grid.fill(17, 0, 18, 16, walk=False)
    grid.fill(1, 1, 17, 6, height=2.0)
    grid.fill(1, 6, 17, 10, height=1.0)
    grid.fill(8, 5, 10, 6, height=1.0, ramp="n")
    grid.fill(8, 9, 10, 10, height=0.0, ramp="n")
    scene = terrain(grid, lambda x, z: PALETTE["stone"] if z < 10 else PALETTE["sand"], rng)
    lights = []
    house(scene, grid, 3, 1, 4, 3, wall=(.7, .68, .64), roof=(.3, .3, .35))
    for z in range(1, 4):
        for x in range(3, 7):
            grid.layers[0]["height"][z][x] = 2.0
    scene.box(13.5, 2.0, 3.5, 1, 2.4, 1, (.8, .75, .6))
    lights.append({"position": [13.5, 4.8, 3.5], "color": [1, .7, .4], "radius": 7, "intensity": 3, "type": "spot",
                   "direction": [0, -1, 0], "inner_cone": 25, "outer_cone": 40})
    placements = [
        npc("shrine_keeper", 12, 4, "scholar", {"name": "Shrine Keeper", "script": "events.shrine"}),
        door("to_route", 8, 14, "route", "west"), door("to_route2", 9, 14, "route", "west"),
    ]
    write_map(folder, "cliffhaven", "Cliffhaven", grid, scene, {
        "ambient": [.38, .38, .42], "sun_color": [.95, .85, .75], "fog_color": [.75, .78, .85], "fog_density": .012,
        "sky": {"mode": "gradient", "cloud_cover": .3}, "post": {"lut": "ui/warm.cube"}},
        {"south": {"position": [8.8, 0, 13.6], "facing": [0, -1]}}, lights=lights, placements=placements,
        props={"music": "cliffhaven", "battle_stage": "stage_plains",
               "camera": {"pitch": 45, "distance": 18, "fov": 30}})

    def forest(scene, rng):
        for x, z in [(2, 2), (13, 2), (1, 12), (14, 12), (7, 1)]:
            scene.tree(x + .5, z + .5, 2.4, (.18, .32, .2))
    def plains(scene, rng):
        for x, z in [(3, 2), (12, 12)]:
            scene.box(x + .5, 0, z + .5, 1.2, .8, 1, (.5, .48, .45))
    battle_stage(folder, "stage_forest", "Forest", "grass", forest, {
        "ambient": [.4, .42, .4], "sun_color": [1, .9, .7], "fog_color": [.5, .6, .55], "fog_density": .015,
        "sky": {"mode": "gradient"}, "post": {"lut": "ui/warm.cube"}})
    battle_stage(folder, "stage_plains", "Plains", "grass", plains, {
        "ambient": [.45, .45, .42], "sun_color": [1, .92, .78], "fog_color": [.7, .78, .85], "fog_density": .008,
        "sky": {"mode": "gradient", "cloud_cover": .5}, "post": {"lut": "ui/warm.cube"}})


# --------------------------------------------------------------------------- Ridgeline (grid tactics)

def ridgeline():
    folder = REPO / "samples" / "ridgeline"
    looks = {
        "cora": {"coat": (60, 80, 150), "hair": (200, 160, 80), "weapon": (220, 220, 230)},
        "bram": {"coat": (90, 110, 60), "hair": (60, 40, 30), "hat": (70, 60, 40)},
        "lio": {"coat": (220, 220, 230), "hair": (90, 60, 40), "trim": (80, 160, 220)},
        "wren": {"coat": (130, 60, 60), "hair": (30, 30, 40)},
        "raider": {"coat": (110, 50, 40), "hair": (40, 30, 30), "hat": (70, 60, 60), "weapon": (180, 180, 180)},
        "captain": {"coat": (60, 30, 30), "hair": (20, 20, 20), "hat": (160, 130, 40), "weapon": (230, 210, 120)},
    }
    for name, look in looks.items():
        sprite_sheet(folder, name, look)
    for name in ("cora", "bram", "lio", "wren"):
        portrait(folder, name, looks[name])
    standard_audio(folder, {"camp": (220, "major", 84), "council": (196, "minor", 70), "battle": (220, "minor", 140),
                            "title": (247, "minor", 66)})
    lut(folder / "ui" / "ridge.cube", (1.02, 0.98, 0.95), 1.12)
    backdrop(folder / "ui" / "peaks.png", (110, 116, 132), 6, 9)

    # Camp: tents around a fire.
    rng = random.Random(31)
    grid = GridBuilder(16, 12)
    grid.fill(0, 0, 16, 1, walk=False); grid.fill(0, 11, 16, 12, walk=False)
    grid.fill(0, 0, 1, 12, walk=False); grid.fill(15, 0, 16, 12, walk=False)
    grid.fill(7, 0, 9, 1, walk=True)
    scene = terrain(grid, lambda x, z: PALETTE["grass"] if (x + z) % 5 else PALETTE["path"], rng)
    lights = [{"position": [8, 1, 6], "color": [1, .55, .25], "radius": 8, "intensity": 3.5, "flicker": .25}]
    for x, z, colour in [(2, 2, (.6, .5, .35)), (12, 2, (.45, .5, .6)), (2, 8, (.55, .45, .4)), (12, 8, (.5, .55, .45))]:
        scene.box(x + 1, 0, z + .75, 2, .2, 1.5, colour)
        scene.roof(x + 1, .2, z + .75, 2.2, 1.3, 1.6, colour)
        grid.fill(x, z, x + 2, z + 2, walk=False)
    scene.box(8, 0, 6, .8, .2, .8, (.3, .25, .2))
    grid.fill(7, 5, 9, 7, walk=False)
    placements = [
        npc("bram", 5, 5, "bram", {"name": "Bram", "script": "events.talk_bram"}, facing=(1, 0)),
        npc("lio", 10, 5, "lio", {"name": "Lio", "script": "events.talk_lio"}, facing=(-1, 0)),
        npc("wren", 8, 8, "wren", {"name": "Wren", "script": "events.talk_wren"}, facing=(0, -1)),
        {"id": "campfire", "type": "emitter", "position": [8, 0.2, 6],
         "props": {"rate": 26, "lifetime": 1.0, "velocity": [0, 1.2, 0], "velocity_jitter": [0.2, 0.3, 0.2],
                   "size": 0.12, "size_end": 0.02, "color": [2.4, 1.1, 0.3, 1], "color_end": [1.2, 0.3, 0.1, 0]}},
        {"id": "fire_sound", "type": "sound", "position": [8, 0.5, 6], "props": {"sound": "audio/chime.wav", "radius": 4, "volume": 0.1}},
        {"id": "to_ridge", "type": "npc", "position": [8, 0, 1.4],
         "props": {"name": "Ridge path", "script": "events.ridge_path", "solid": False}},
    ]
    write_map(folder, "camp", "Ridge Camp", grid, scene, {
        "ambient": [.22, .22, .32], "sun_color": [.45, .5, .75], "sun_direction": [.4, -1, .3],
        "fog_color": [.1, .12, .2], "fog_density": .02, "sky": {"mode": "gradient", "top": [.03, .04, .12],
                                                                 "horizon": [.2, .2, .35], "cloud_cover": .2},
        "weather": {"kind": "embers", "intensity": .4}, "post": {"bloom_strength": .5, "lut": "ui/ridge.cube"}},
        {"start": {"position": [8, 0, 9.5], "facing": [0, -1]}}, lights=lights, placements=placements,
        props={"music": "camp", "camera": {"pitch": 44, "distance": 16, "fov": 30}})

    # The ridge: terraces, ramps, grass (burns), a stream (freezes, conducts).
    rng = random.Random(32)
    grid = GridBuilder(18, 15)
    heights = [
        "000000000000000000", "001111000002222000", "001111000002222000", "001111110002222200",
        "000011110000222200", "000000000000000000", "000000000000000000", "000000000000000000",
        "000000000000000000", "000001100000110000", "000001100000110000", "000000000000000000",
        "000000000000000000", "000000000000000000", "000000000000000000"]
    for z, row in enumerate(heights):
        for x, h in enumerate(row):
            grid.layers[0]["height"][z][x] = float(h)
    grid.fill(4, 5, 6, 6, height=0.0, ramp="n")
    grid.fill(12, 5, 14, 6, height=1.0, ramp="n")
    grid.fill(12, 6, 14, 7, height=0.0, ramp="n")
    grid.fill(1, 6, 8, 9, tag="grass")
    grid.fill(10, 7, 17, 9, tag="grass")
    for z in range(15):
        x = 8 + (1 if z % 4 < 2 else 0)
        grid.fill(x, z, x + 1, z + 1, tag="water")
        grid.layers[0]["height"][z][x] = -0.2
    grid.fill(3, 12, 15, 14, tag="deploy")
    colour = lambda x, z: (PALETTE["cliff"] if grid.layers[0]["height"][z][x] >= 1 else
                           (.2, .4, .6) if grid.layers[0]["height"][z][x] < 0 else PALETTE["grass"])
    scene = terrain(grid, colour, rng)
    for x, z in [(1, 13), (16, 12), (16, 2), (0, 9)]:
        tree(scene, grid, x, z)
    write_map(folder, "ridge", "Hollow Ridge", grid, scene, {
        "ambient": [.4, .4, .42], "sun_color": [1, .9, .78], "sun_direction": [-.5, -1, -.6],
        "fog_color": [.7, .74, .82], "fog_density": .008,
        "sky": {"mode": "gradient", "cloud_cover": .5,
                "backdrops": [{"image": "ui/peaks.png", "distance": 55, "height": 20, "width": 120, "yaw": 0, "elevation": -4}]},
        "post": {"focus": .5, "focus_range": .3, "lut": "ui/ridge.cube"}},
        {"start": {"position": [8, 0, 12.5]}}, keep=True,
        props={"music": "battle", "camera": {"pitch": 50, "distance": 20, "fov": 32}})


# --------------------------------------------------------------------------- test fixture

def fixture():
    """A small arena used by the engine's own C++ and Lua tests."""
    root = REPO / "lualib" / "tests" / "game"
    grid = GridBuilder(12, 10, layers=2)
    scene = Diorama()
    grid.fill(0, 0, 12, 1, walk=False)
    grid.fill(0, 9, 12, 10, walk=False)
    grid.fill(0, 0, 1, 10, walk=False)
    grid.fill(11, 0, 12, 10, walk=False)
    grid.fill(5, 1, 6, 6, walk=False)            # wall with a gap at z 6..8
    grid.fill(8, 2, 10, 4, height=1.0)           # raised platform
    grid.fill(7, 2, 8, 4, ramp="e")              # ramp up to it from the west
    grid.fill(2, 7, 4, 9, tag="water")
    grid.fill(1, 1, 3, 3, tag="deploy")
    grid.fill(6, 7, 7, 8, layer=1, height=0.0, walk=True, ramp="e")  # bridge ramp
    grid.fill(7, 7, 9, 8, layer=1, height=1.0, walk=True)            # bridge deck
    grid.links.append({"x": 6, "z": 7, "from": 0, "to": 1, "dir": "e"})
    for z in range(10):
        for x in range(12):
            h = grid.layers[0]["height"][z][x]
            scene.quad([(x, h, z), (x, h, z + 1), (x + 1, h, z + 1), (x + 1, h, z)], (.3, .4, .3))
    write_map(root, "arena", "Test Arena", grid, scene, {"sky": {"mode": "gradient"}},
              {"start": {"position": [2.5, 0, 5.5], "facing": [0, 1], "layer": 0},
               "east": {"position": [9.5, 1, 3.5], "layer": 0}},
              lights=[{"position": [6, 2, 5], "radius": 6, "intensity": 2}])
    write_json(root / "game.json", {"title": "fengine tests", "virtual_resolution": {"width": 320, "height": 180}})


# --------------------------------------------------------------------------- characters

CLIPS = [("idle", 2, True, 0.45), ("walk", 4, True, 0.14), ("attack", 3, False, 0.11), ("cast", 3, False, 0.16),
         ("item", 2, False, 0.2), ("hurt", 1, False, 0.3), ("ko", 1, True, 1.0), ("victory", 2, True, 0.3)]
OUTLINE = (31, 29, 44)
SKIN = (247, 201, 155)


def shade(color, factor):
    return tuple(max(0, min(255, int(c * factor))) for c in color[:3])


def draw_person(c, ox, oy, direction, clip, frame, look):
    """One 32x48 frame of a humanoid. `look` = {coat, hair, trim, hat, weapon}."""
    coat, hair, trim = look["coat"], look["hair"], look.get("trim", (235, 190, 105))
    bob = 1 if (clip in ("walk", "idle") and frame % 2 == 1) else 0
    lift = -3 if clip == "victory" and frame == 1 else 0
    if clip == "ko":
        c.rect(ox + 4, oy + 36, 24, 9, OUTLINE)
        c.rect(ox + 6, oy + 37, 14, 7, coat)
        c.rect(ox + 20, oy + 37, 7, 7, SKIN)
        c.rect(ox + 21, oy + 37, 6, 3, hair)
        return
    y = oy + bob + lift
    step = (frame % 2) if clip == "walk" else 0
    # legs
    c.rect(ox + 9, oy + 38 + lift, 6, 7 - step, OUTLINE)
    c.rect(ox + 17, oy + 38 + lift, 6, 6 + step, OUTLINE)
    # body
    c.rect(ox + 8, y + 19, 16, 21, OUTLINE)
    c.rect(ox + 10, y + 21, 12, 17, coat)
    c.rect(ox + 9, y + 29, 14, 3, trim)
    # arms by pose
    arm = shade(coat, 0.7)
    if clip == "attack":
        reach = [2, 8, 4][frame]
        side = -1 if direction == 1 else 1
        hx = ox + 16 + side * (6 + reach)
        c.rect(min(ox + 16, hx), y + 24, abs(hx - (ox + 16)) + 4, 4, arm)
        c.rect(hx, y + 22, 3, 3, SKIN)
        if look.get("weapon"):
            c.rect(hx + side * 2, y + 12, 2, 12, look["weapon"])
    elif clip in ("cast", "item") or clip == "victory":
        up = [6, 12, 10][frame % 3] if clip == "cast" else 10
        c.rect(ox + 5, y + 22 - up // 2, 4, 10, arm)
        c.rect(ox + 23, y + 22 - up // 2, 4, 10, arm)
        c.rect(ox + 5, y + 19 - up // 2, 4, 3, SKIN)
        c.rect(ox + 23, y + 19 - up // 2, 4, 3, SKIN)
        if clip == "cast" and frame >= 1:
            c.ellipse(ox + 16, y + 6 - up // 3, 3, 3, (180, 230, 255))
    else:
        c.rect(ox + 6, y + 22, 4, 13, arm)
        c.rect(ox + 22, y + 22, 4, 13, arm)
        c.rect(ox + 6, y + 34, 4, 3, SKIN)
        c.rect(ox + 22, y + 34, 4, 3, SKIN)
    if clip == "hurt":
        c.rect(ox + 10, y + 21, 12, 3, (255, 255, 255))
    # head
    c.rect(ox + 8, y + 4, 16, 17, OUTLINE)
    c.rect(ox + 10, y + 7, 12, 12, SKIN)
    c.rect(ox + 9, y + 3, 14, 6, hair)
    if look.get("hat"):
        c.rect(ox + 7, y + 1, 18, 4, look["hat"])
        c.rect(ox + 10, y - 2, 12, 4, look["hat"])
    if direction == 2:  # north: back of the head
        c.rect(ox + 9, y + 7, 14, 12, hair)
    elif direction == 1:  # west
        c.rect(ox + 10, y + 12, 2, 3, OUTLINE)
        c.rect(ox + 19, y + 7, 5, 10, hair)
    elif direction == 3:  # east
        c.rect(ox + 21, y + 12, 2, 3, OUTLINE)
        c.rect(ox + 8, y + 7, 5, 10, hair)
    else:
        c.rect(ox + 11, y + 12, 2, 3, OUTLINE)
        c.rect(ox + 19, y + 12, 2, 3, OUTLINE)
        c.rect(ox + 14, y + 17, 4, 1, (160, 88, 76))


def draw_monster(c, ox, oy, direction, clip, frame, look):
    kind, body = look["kind"], look["coat"]
    dark = shade(body, 0.6)
    bob = 1 if frame % 2 else 0
    if clip == "ko":
        c.ellipse(ox + 16, oy + 42, 12, 4, dark)
        return
    lunge = (-3 if direction == 1 else 3) if clip == "attack" and frame == 1 else 0
    if kind == "slime":
        c.ellipse(ox + 16 + lunge, oy + 34 - bob, 13 + bob, 11 - bob, OUTLINE)
        c.ellipse(ox + 16 + lunge, oy + 34 - bob, 12 + bob, 10 - bob, body)
        c.rect(ox + 10 + lunge, oy + 30, 3, 4, OUTLINE)
        c.rect(ox + 19 + lunge, oy + 30, 3, 4, OUTLINE)
        c.rect(ox + 9 + lunge, oy + 26, 4, 2, (230, 255, 240))
    elif kind == "wolf":
        c.rect(ox + 4 + lunge, oy + 26 - bob, 22, 12, OUTLINE)
        c.rect(ox + 5 + lunge, oy + 27 - bob, 20, 10, body)
        c.rect(ox + (2 if direction == 1 else 22) + lunge, oy + 22 - bob, 8, 9, body)
        c.rect(ox + 6, oy + 38, 3, 7, dark); c.rect(ox + 21, oy + 38, 3, 7, dark)
        c.rect(ox + (4 if direction == 1 else 26) + lunge, oy + 24 - bob, 2, 2, (255, 230, 120))
    elif kind == "golem":
        c.rect(ox + 5, oy + 9 + bob, 22, 32, OUTLINE)
        c.rect(ox + 8, oy + 11 + bob, 16, 27, body)
        c.rect(ox + 1 + lunge, oy + 22 + bob, 7, 17, dark); c.rect(ox + 24 + lunge, oy + 22 + bob, 7, 17, dark)
        c.rect(ox + 7, oy + 37, 8, 7, dark); c.rect(ox + 18, oy + 37, 8, 7, dark)
        c.rect(ox + 11, oy + 17 + bob, 3, 3, (97, 245, 255)); c.rect(ox + 20, oy + 17 + bob, 3, 3, (97, 245, 255))
    elif kind == "wisp":
        c.ellipse(ox + 16, oy + 22 - bob * 2, 9, 9, shade(body, 1.3))
        c.ellipse(ox + 16, oy + 22 - bob * 2, 6, 6, (255, 255, 255))
        c.ellipse(ox + 16, oy + 36, 6, 2, dark)


def sprite_sheet(folder, name, look, monster=False):
    c, clips = draw_sheet(look, monster)
    c.save(folder / "sprites" / f"{name}.png")
    write_json(folder / "sprites" / f"{name}.anim.json", {"atlas": f"{name}.png", "pixels_per_unit": 32,
                                                          "pivot": [0.5, 1], "clips": clips})


def draw_sheet(look, monster=False):
    rows = len(CLIPS) * 4
    c = Canvas(128, 48 * rows)
    clips = {}
    row = 0
    for clip, frames, loop, duration in CLIPS:
        directions = []
        for direction in range(4):
            entries = []
            for frame in range(frames):
                ox, oy = frame * 32, row * 48
                (draw_monster if monster else draw_person)(c, ox, oy, direction, clip, frame, look)
                entry = {"rect": [ox, oy, 32, 48], "duration": duration}
                if clip == "walk" and frame in (1, 3) and not monster:
                    entry["event"] = "footstep"
                entries.append(entry)
            directions.append(entries)
            row += 1
        clips[clip] = {"loop": loop, "directions": directions}
    return c, clips


def prop_sprite(folder, name, draw, clips):
    """Small static or two-state sprites (chests, signs, crystals)."""
    c = Canvas(64, 48)
    draw(c)
    c.save(folder / "sprites" / f"{name}.png")
    out = {}
    for clip, column in clips.items():
        out[clip] = {"directions": [[{"rect": [column * 32, 0, 32, 48], "duration": 1}]]}
    write_json(folder / "sprites" / f"{name}.anim.json", {"atlas": f"{name}.png", "pixels_per_unit": 32,
                                                          "pivot": [0.5, 1], "clips": out})


def chest_sprite(folder):
    def draw(c):
        for column, opened in ((0, False), (1, True)):
            ox = column * 32
            c.rect(ox + 4, 24, 24, 20, OUTLINE)
            c.rect(ox + 6, 26, 20, 16, (137, 71, 40))
            c.rect(ox + 5, 28, 22, 3, (238, 189, 94))
            if opened:
                c.rect(ox + 6, 18, 20, 7, (110, 55, 30))
                c.rect(ox + 9, 27, 14, 6, (255, 230, 140))
            else:
                c.rect(ox + 14, 29, 4, 7, (255, 220, 122))
    prop_sprite(folder, "chest", draw, {"closed": 0, "open": 1, "idle": 0})


def crystal_sprite(folder, color):
    def draw(c):
        for column, bright in ((0, 1.0), (1, 1.4)):
            ox = column * 32
            for i in range(14):
                width = 14 - abs(7 - i) * 2
                c.rect(ox + 16 - width // 2, 18 + i * 2, width, 2, shade(color, bright * (0.7 + i / 30)))
    prop_sprite(folder, "crystal", draw, {"idle": 0, "glow": 1})


def bevel_normals(c, depth=3):
    """Tangent-space normal map (x right, y up) that rounds a sprite's edges:
    height grows with the distance to the nearest transparent pixel."""
    w, h = c.width, c.height
    far = depth + 1
    height = [0 if c.pixels[i * 4 + 3] < 128 else far for i in range(w * h)]
    for _ in range(depth):
        nxt = height[:]
        for y in range(h):
            for x in range(w):
                i = y * w + x
                if height[i]:
                    around = [height[j] for j in (i - 1, i + 1, i - w, i + w)
                              if 0 <= j < w * h and abs(j % w - x) <= 1]
                    nxt[i] = min(height[i], min(around, default=0) + 1)
        height = nxt
    out = Canvas(w, h)
    for y in range(h):
        for x in range(w):
            i = y * w + x
            if not height[i]:
                out.rect(x, y, 1, 1, (128, 128, 255, 0))
                continue
            at = lambda xx, yy: height[yy * w + xx] if 0 <= xx < w and 0 <= yy < h else 0
            dx = (at(x + 1, y) - at(x - 1, y)) / (2 * depth)
            dy = (at(x, y + 1) - at(x, y - 1)) / (2 * depth)
            n = (-dx, dy, 1.0)
            length = math.sqrt(sum(v * v for v in n))
            out.rect(x, y, 1, 1, tuple(int((v / length * 0.5 + 0.5) * 255) for v in n) + (255,))
    return out


def gltf_box(path, size, height, textures):
    """A textured box (no tangents: the engine generates them) for material tests."""
    sx, sz = size / 2, size / 2
    faces = [((0, 1, 0), [(-sx, height, sz), (sx, height, sz), (sx, height, -sz), (-sx, height, -sz)]),
             ((0, 0, 1), [(-sx, 0, sz), (sx, 0, sz), (sx, height, sz), (-sx, height, sz)]),
             ((0, 0, -1), [(sx, 0, -sz), (-sx, 0, -sz), (-sx, height, -sz), (sx, height, -sz)]),
             ((1, 0, 0), [(sx, 0, sz), (sx, 0, -sz), (sx, height, -sz), (sx, height, sz)]),
             ((-1, 0, 0), [(-sx, 0, -sz), (-sx, 0, sz), (-sx, height, sz), (-sx, height, -sz)])]
    positions, normals, uvs, indices = [], [], [], []
    for normal, corners in faces:
        base = len(positions)
        positions += corners
        normals += [normal] * 4
        uvs += [(0, 1), (1, 1), (1, 0), (0, 0)]
        indices += [base, base + 1, base + 2, base, base + 2, base + 3]
    blob = b"".join(struct.pack("<3f", *v) for v in positions)
    normal_offset = len(blob)
    blob += b"".join(struct.pack("<3f", *v) for v in normals)
    uv_offset = len(blob)
    blob += b"".join(struct.pack("<2f", *v) for v in uvs)
    index_offset = len(blob)
    blob += b"".join(struct.pack("<H", i) for i in indices)
    path.parent.mkdir(parents=True, exist_ok=True)
    path.with_suffix(".bin").write_bytes(blob)
    views = [(0, normal_offset), (normal_offset, uv_offset - normal_offset), (uv_offset, index_offset - uv_offset),
             (index_offset, len(blob) - index_offset)]
    write_json(path, {
        "asset": {"version": "2.0", "generator": "f-engine generate_samples.py"},
        "extensionsUsed": ["KHR_materials_emissive_strength"],
        "scene": 0, "scenes": [{"nodes": [0]}], "nodes": [{"name": path.stem, "mesh": 0}],
        "meshes": [{"primitives": [{"attributes": {"POSITION": 0, "NORMAL": 1, "TEXCOORD_0": 2}, "indices": 3,
                                    "material": 0}]}],
        "materials": [{"pbrMetallicRoughness": {"baseColorTexture": {"index": 0}},
                       "normalTexture": {"index": 1}, "emissiveTexture": {"index": 2},
                       "emissiveFactor": [1, 1, 1],
                       "extensions": {"KHR_materials_emissive_strength": {"emissiveStrength": 3}}}],
        "textures": [{"source": i, "sampler": 0} for i in range(3)],
        "samplers": [{"magFilter": 9728, "minFilter": 9987}],
        "images": [{"uri": name} for name in textures],
        "buffers": [{"uri": path.with_suffix(".bin").name, "byteLength": len(blob)}],
        "bufferViews": [{"buffer": 0, "byteOffset": o, "byteLength": n} for o, n in views],
        "accessors": [
            {"bufferView": 0, "componentType": 5126, "count": len(positions), "type": "VEC3",
             "min": [min(v[k] for v in positions) for k in range(3)],
             "max": [max(v[k] for v in positions) for k in range(3)]},
            {"bufferView": 1, "componentType": 5126, "count": len(normals), "type": "VEC3"},
            {"bufferView": 2, "componentType": 5126, "count": len(uvs), "type": "VEC2"},
            {"bufferView": 3, "componentType": 5123, "count": len(indices), "type": "SCALAR"}]})


def gltf_water(path, width, depth):
    """A flat `material: water` surface centred on the origin."""
    w, d = width / 2, depth / 2
    positions = [(-w, 0, d), (w, 0, d), (w, 0, -d), (-w, 0, -d)]
    blob = b"".join(struct.pack("<3f", *v) for v in positions)
    normal_offset = len(blob)
    blob += struct.pack("<3f", 0, 1, 0) * 4
    index_offset = len(blob)
    blob += struct.pack("<6H", 0, 1, 2, 0, 2, 3)
    path.parent.mkdir(parents=True, exist_ok=True)
    path.with_suffix(".bin").write_bytes(blob)
    write_json(path, {
        "asset": {"version": "2.0", "generator": "f-engine generate_samples.py"},
        "scene": 0, "scenes": [{"nodes": [0]}], "nodes": [{"name": path.stem, "mesh": 0}],
        "meshes": [{"primitives": [{"attributes": {"POSITION": 0, "NORMAL": 1}, "indices": 2, "material": 0}]}],
        "materials": [{"pbrMetallicRoughness": {"baseColorFactor": [.16, .45, .62, .8]}, "alphaMode": "BLEND",
                       "extras": {"fengine": {"material": "water"}}}],
        "buffers": [{"uri": path.with_suffix(".bin").name, "byteLength": len(blob)}],
        "bufferViews": [{"buffer": 0, "byteOffset": 0, "byteLength": normal_offset},
                        {"buffer": 0, "byteOffset": normal_offset, "byteLength": index_offset - normal_offset},
                        {"buffer": 0, "byteOffset": index_offset, "byteLength": len(blob) - index_offset}],
        "accessors": [
            {"bufferView": 0, "componentType": 5126, "count": 4, "type": "VEC3", "min": [-w, 0, -d], "max": [w, 0, d]},
            {"bufferView": 1, "componentType": 5126, "count": 4, "type": "VEC3"},
            {"bufferView": 2, "componentType": 5123, "count": 6, "type": "SCALAR"}]})


def render_showcase(folder):
    """fx/: assets for the rendering replay (materials, lit sprites, decals)."""
    fx = folder / "fx"
    plank, seam = (150, 104, 62), (70, 44, 28)
    albedo, bumps, glow = Canvas(64, 64), Canvas(64, 64), Canvas(64, 64)
    albedo.rect(0, 0, 64, 64, plank)
    bumps.rect(0, 0, 64, 64, (128, 128, 255))
    glow.rect(0, 0, 64, 64, (0, 0, 0))
    for y in range(64):
        for x in range(64):
            grain = int(14 * math.sin(x * 0.7 + math.sin(y * 0.3) * 2))
            albedo.rect(x, y, 1, 1, shade(plank, 1 + grain / 255))
    for edge in (0, 15, 16, 31, 32, 47, 48, 63):
        albedo.rect(0, edge, 64, 1, seam)
        # Grooves between planks: normals lean away from each seam.
        lean = 70 if edge % 16 == 15 else 186
        bumps.rect(0, edge, 64, 1, (128, lean, 230))
    for x in (0, 1, 62, 63):
        albedo.rect(x, 0, 1, 64, seam)
        bumps.rect(x, 0, 1, 64, (60 if x < 32 else 196, 128, 230))
    for k in range(20):
        glow.rect(22 + k, 30 + int(3 * math.sin(k * 0.8)), 2, 3, (255, 150, 60))
    albedo.save(fx / "crate.png")
    bumps.save(fx / "crate_normal.png")
    glow.save(fx / "crate_glow.png")
    gltf_box(fx / "crate.gltf", 0.8, 0.8, ["crate.png", "crate_normal.png", "crate_glow.png"])
    gltf_water(fx / "pond.gltf", 3, 2)

    sheet, clips = draw_sheet({"coat": (70, 90, 60), "hair": (40, 30, 26), "weapon": (210, 210, 220),
                               "hat": (60, 50, 40)})
    sheet.save(fx / "rook_lit.png")
    bevel_normals(sheet).save(fx / "rook_lit_normal.png")
    mask = Canvas(sheet.width, sheet.height)
    for oy in range(0, sheet.height, 48):
        for ox in range(0, sheet.width, 32):
            mask.rect(ox + 15, oy + 30, 2, 2, (120, 220, 255, 255))
    mask.save(fx / "rook_lit_glow.png")
    write_json(fx / "rook_lit.anim.json", {"atlas": "rook_lit.png", "normal": "rook_lit_normal.png",
                                           "emissive": "rook_lit_glow.png", "pixels_per_unit": 32,
                                           "pivot": [0.5, 1], "clips": clips})

    circle = Canvas(64, 64)
    for y in range(64):
        for x in range(64):
            r = math.hypot(x - 31.5, y - 31.5)
            angle = math.atan2(y - 31.5, x - 31.5)
            ring = r > 27 and r < 30 or r > 18 and r < 20 or (r > 20 and r < 27 and math.sin(angle * 6) > 0.92)
            if ring:
                circle.rect(x, y, 1, 1, (255, 255, 255, 255))
    circle.save(fx / "circle.png")
    write_json(fx / "circle.anim.json", {"atlas": "circle.png", "pixels_per_unit": 32, "pivot": [0.5, 0.5],
                                         "clips": {"idle": {"directions": [[{"rect": [0, 0, 64, 64],
                                                                              "duration": 1}]]}}})


def portrait(folder, name, look):
    c = Canvas(48, 48)
    c.rect(0, 0, 48, 48, shade(look["coat"], 0.35))
    c.rect(8, 30, 32, 18, look["coat"])
    c.rect(14, 8, 20, 24, SKIN)
    c.rect(12, 4, 24, 9, look["hair"])
    c.rect(12, 10, 4, 14, look["hair"])
    c.rect(18, 18, 3, 3, OUTLINE)
    c.rect(27, 18, 3, 3, OUTLINE)
    c.rect(21, 26, 6, 2, (160, 88, 76))
    c.save(folder / "portraits" / f"{name}.png")


# --------------------------------------------------------------------------- audio

def write_wave(path, samples, rate=22050):
    path.parent.mkdir(parents=True, exist_ok=True)
    with wave.open(str(path), "wb") as out:
        out.setnchannels(1)
        out.setsampwidth(2)
        out.setframerate(rate)
        out.writeframes(struct.pack(f"<{len(samples)}h", *[max(-32767, min(32767, int(v))) for v in samples]))


def music(path, root, mode, tempo, bars=8, rate=22050):
    """A short looping tune: bass on chord roots, melody from a scale."""
    scale = [0, 2, 4, 5, 7, 9, 11] if mode == "major" else [0, 2, 3, 5, 7, 8, 10]
    progression = [0, 5, 3, 4] if mode == "major" else [0, 5, 2, 6]
    rng = random.Random(zlib.crc32(path.name.encode()))
    beat = 60 / tempo
    samples = []
    for bar in range(bars):
        chord = progression[bar % len(progression)]
        for step in range(4):
            note = scale[(chord + rng.choice([0, 2, 4, 7])) % 7] + (12 if rng.random() < 0.2 else 0)
            freq = root * 2 ** (note / 12)
            bass = root / 2 * 2 ** (scale[chord % 7] / 12)
            for i in range(int(rate * beat)):
                t = i / rate
                env = min(1, t * 30) * math.exp(-t * 2.5)
                v = (math.sin(t * freq * math.tau) * 0.18 + math.sin(t * freq * 2 * math.tau) * 0.04 +
                     math.sin(t * bass * math.tau) * 0.12 * min(1, t * 10))
                samples.append(v * env * 14000)
    write_wave(path, samples, rate)


def sfx(path, kind, rate=22050):
    noise = random.Random(len(kind))
    specs = {"cursor": (0.04, 1200, 0.0), "confirm": (0.09, 900, 0.0), "cancel": (0.09, 400, 0.0),
             "buzzer": (0.18, 120, 0.3), "hit": (0.12, 180, 0.6), "chest": (0.25, 600, 0.1), "step": (0.05, 110, 0.7),
             "heal": (0.35, 1400, 0.0), "save": (0.4, 700, 0.0), "buy": (0.15, 1600, 0.0), "equip": (0.12, 500, 0.2),
             "defeat_enemy": (0.35, 220, 0.5), "victory": (1.2, 523, 0.0), "chime": (0.3, 880, 0.0)}
    duration, freq, grit = specs[kind]
    samples = []
    for i in range(int(rate * duration)):
        t = i / rate
        env = (1 - t / duration) ** 2
        sweep = freq * (1 + (0.5 if kind in ("heal", "save", "victory") else -0.3) * t / duration)
        v = math.sin(t * sweep * math.tau) * (1 - grit) + noise.uniform(-1, 1) * grit
        samples.append(v * env * 12000)
    write_wave(path, samples, rate)


def standard_audio(folder, tunes):
    for name, (root, mode, tempo) in tunes.items():
        music(folder / "audio" / f"{name}.wav", root, mode, tempo)
    for kind in ("cursor", "confirm", "cancel", "buzzer", "hit", "chest", "step", "heal", "save", "buy", "equip",
                 "defeat_enemy", "chime"):
        sfx(folder / "audio" / f"{kind}.wav", kind)
    music(folder / "audio" / "victory.wav", 392, "major", 160, bars=2)
    entries = {"music": {name: {"file": f"audio/{name}.wav"} for name in list(tunes) + ["victory"]},
               "sfx": {kind: f"audio/{kind}.wav" for kind in ("cursor", "confirm", "cancel", "buzzer", "hit", "chest",
                                                              "step", "heal", "save", "buy", "equip", "defeat_enemy",
                                                              "chime")}}
    entries["music"]["victory"]["loop"] = False
    write_json(folder / "data" / "audio.json", entries)


# --------------------------------------------------------------------------- grading and backdrops

def lut(path, tint, contrast=1.0):
    lines = ['TITLE "f-engine grade"', "LUT_3D_SIZE 16"]
    for b in range(16):
        for g in range(16):
            for r in range(16):
                values = []
                for i, v in enumerate((r, g, b)):
                    x = v / 15
                    x = 0.5 + (x - 0.5) * contrast
                    values.append(f"{max(0, min(1, x * tint[i])):.5f}")
                lines.append(" ".join(values))
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text("\n".join(lines) + "\n")


def backdrop(path, color, peaks, seed):
    rng = random.Random(seed)
    w, h = 256, 96
    c = Canvas(w, h)
    heights = []
    level = h * 0.5
    for x in range(w):
        level += rng.uniform(-2.5, 2.5)
        level = max(h * 0.15, min(h * 0.85, level))
        heights.append(level)
    for x in range(w):
        for y in range(int(heights[x]), h):
            c.rect(x, y, 1, 1, (*color, 255))
    for _ in range(peaks):
        px = rng.randrange(w)
        for x in range(max(0, px - 20), min(w, px + 20)):
            top = heights[x] - (20 - abs(x - px)) * 1.2
            for y in range(int(top), int(heights[x])):
                c.rect(x, y, 1, 1, (*shade(color, 1.15), 255))
    c.save(path)


# --------------------------------------------------------------------------- engine UI art

def engine():
    """Default UI art shipped in lualib/fe/assets/ui (window skin, cursor, emotes)."""
    root = REPO / "lualib" / "fe" / "assets" / "ui"
    skin = Canvas(24, 24)
    for y in range(24):
        for x in range(24):
            d = min(x, y, 23 - x, 23 - y)
            if d == 0:
                color = (196, 208, 232, 255)
            elif d == 1:
                color = (60, 80, 120, 255)
            elif d == 2:
                color = (30, 44, 78, 250)
            else:
                shade = int(14 + 10 * (y / 23))
                color = (shade, shade + 14, shade + 40, 238)
            skin.rect(x, y, 1, 1, color)
    skin.save(root / "window.png")
    cursor = Canvas(8, 8)
    for i in range(4):
        cursor.rect(1 + i, 1 + i, 1, 7 - 2 * i, (255, 236, 160))
    cursor.rect(0, 0, 1, 8, (60, 40, 20))
    cursor.save(root / "cursor.png")
    bubble = Canvas(24, 24)
    for y in range(24):
        for x in range(24):
            d = min(x, y, 23 - x, 23 - y)
            if d == 0 and (x, y) not in ((0, 0), (23, 0), (0, 23), (23, 23)):
                bubble.rect(x, y, 1, 1, (40, 40, 52, 255))
            elif d > 0:
                bubble.rect(x, y, 1, 1, (250, 248, 240, 245))
    bubble.save(root / "bubble.png")
    # Emote balloons: 16x16 frames in one row (exclaim, question, music, sweat, dots, anger, heart, idea)
    emotes = Canvas(16 * 8, 16)
    ink = (40, 32, 48)
    for index in range(8):
        ox = index * 16
        emotes.ellipse(ox + 7.5, 6.5, 7, 6, (40, 32, 48))
        emotes.ellipse(ox + 7.5, 6.5, 6, 5, (252, 250, 244))
        emotes.rect(ox + 6, 12, 3, 2, (252, 250, 244))
        emotes.rect(ox + 6, 14, 1, 1, ink)
        if index == 0:
            emotes.rect(ox + 7, 2, 2, 6, (220, 40, 40)); emotes.rect(ox + 7, 9, 2, 2, (220, 40, 40))
        elif index == 1:
            emotes.rect(ox + 6, 2, 4, 1, ink); emotes.rect(ox + 9, 3, 1, 2, ink); emotes.rect(ox + 7, 5, 2, 1, ink)
            emotes.rect(ox + 7, 6, 1, 2, ink); emotes.rect(ox + 7, 9, 1, 2, ink)
        elif index == 2:
            emotes.rect(ox + 9, 2, 1, 6, ink); emotes.rect(ox + 10, 2, 2, 1, ink); emotes.rect(ox + 6, 7, 3, 2, ink)
        elif index == 3:
            emotes.rect(ox + 10, 3, 2, 3, (80, 160, 240)); emotes.rect(ox + 11, 6, 1, 1, (80, 160, 240))
        elif index == 4:
            for dx in (4, 7, 10):
                emotes.rect(ox + dx, 6, 2, 2, ink)
        elif index == 5:
            emotes.rect(ox + 5, 3, 2, 2, (220, 40, 40)); emotes.rect(ox + 9, 3, 2, 2, (220, 40, 40))
            emotes.rect(ox + 5, 8, 2, 2, (220, 40, 40)); emotes.rect(ox + 9, 8, 2, 2, (220, 40, 40))
        elif index == 6:
            emotes.ellipse(ox + 6, 5, 2, 2, (230, 70, 110)); emotes.ellipse(ox + 9, 5, 2, 2, (230, 70, 110))
            for r in range(4):
                emotes.rect(ox + 4 + r, 6 + r, 8 - 2 * r, 1, (230, 70, 110))
        else:
            emotes.ellipse(ox + 7.5, 5, 3, 3, (250, 210, 60)); emotes.rect(ox + 6, 8, 3, 2, (120, 110, 90))
    emotes.save(root / "emotes.png")
    names = ["exclaim", "question", "music", "sweat", "dots", "anger", "heart", "idea"]
    clips = {}
    for index, name in enumerate(names):
        clips[name] = {"loop": False, "directions": [[{"rect": [index * 16, 0, 16, 16], "duration": 1.2}]]}
    write_json(root / "emotes.anim.json", {"atlas": "emotes.png", "pixels_per_unit": 32, "pivot": [0.5, 1], "clips": clips})
    spark = Canvas(16, 16)
    for y in range(16):
        for x in range(16):
            r = math.sqrt((x - 7.5) ** 2 + (y - 7.5) ** 2) / 7.5
            a = int(max(0, 1 - r) ** 1.5 * 255)
            if a:
                spark.rect(x, y, 1, 1, (255, 255, 255, a))
    spark.save(root / "spark.png")
    write_json(root / "spark.anim.json", {"atlas": "spark.png", "pixels_per_unit": 32, "pivot": [0.5, 0.5],
                                          "clips": {"idle": {"directions": [[{"rect": [0, 0, 16, 16], "duration": 1}]]}}})


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("targets", nargs="*", default=["fixture"], help="engine, fixture, vale, wayfarers, ridgeline, all")
    args = parser.parse_args()
    targets = set(args.targets)
    if "all" in targets:
        targets = {"engine", "fixture", "vale", "wayfarers", "ridgeline"}
    for name in sorted(targets):
        globals()[name]()
        print(f"generated {name}")


if __name__ == "__main__":
    main()
