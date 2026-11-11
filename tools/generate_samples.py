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
        self.vertices = {"opaque": [], "blend": [], "water": [], "collision": []}
        self.indices = {"opaque": [], "blend": [], "water": [], "collision": []}
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

    def collision_box(self, x, y, z, w, h, d):
        """A hidden box the walkability bake reads instead of the visible mesh."""
        x0, x1, z0, z1 = x - w / 2, x + w / 2, z - d / 2, z + d / 2
        self.quad([(x0, y + h, z0), (x0, y + h, z1), (x1, y + h, z1), (x1, y + h, z0)], (1, 1, 1), "collision")
        for points in [[(x0, y, z1), (x1, y, z1), (x1, y + h, z1), (x0, y + h, z1)],
                       [(x1, y, z0), (x0, y, z0), (x0, y + h, z0), (x1, y + h, z0)],
                       [(x0, y, z0), (x0, y, z1), (x0, y + h, z1), (x0, y + h, z0)],
                       [(x1, y, z1), (x1, y, z0), (x1, y + h, z0), (x1, y + h, z1)]]:
            self.quad(points, (1, 1, 1), "collision")

    def write(self, path):
        binary = bytearray()
        views, accessors, primitives, collision = [], [], [], []
        materials = {"opaque": 0, "blend": 1, "water": 2, "collision": 0}
        for channel in ("opaque", "blend", "water", "collision"):
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
            (collision if channel == "collision" else primitives).append(
                {"attributes": attributes, "indices": index, "material": materials[channel]})
        nodes = [{"mesh": 0, "name": "scenery"}] + self.extra_nodes
        meshes = [{"primitives": primitives}]
        if collision:
            # A hidden collider mesh: the walkability bake reads it instead of the visible one.
            nodes.append({"mesh": len(meshes), "name": "collision",
                          "extras": {"fengine": {"collision": True, "visible": False}}})
            meshes.append({"primitives": collision})
        doc = {"asset": {"version": "2.0", "generator": "f-engine sample generator"}, "scene": 0,
               "scenes": [{"nodes": list(range(len(nodes)))}], "nodes": nodes, "meshes": meshes,
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


KEEP_AUTHORED = True  # --fresh: rewrite placements, zones and props from the generator's lists


def write_map(folder, map_id, name, grid, scene, environment, points, lights=(), emitters=(), keep=True,
              placements=(), zones=(), props=None, overrides=()):
    """Writes the map's geometry and map.json. The grid rows are a starting
    point: `fengine_headless --bake <game>` derives walkability from the
    geometry and the placed models afterwards (generate_samples.py --headless
    runs it), keeping `overrides` (cells the bake would get wrong, such as the
    map's doors on the blocked edge ring)."""
    target = folder / "maps" / map_id / "map.json"
    scene.write(folder / "maps" / map_id / "geometry.glb")
    authored = {"placements": list(placements), "zones": list(zones), "props": props or {}}
    if keep and KEEP_AUTHORED and target.exists():
        previous = json.loads(target.read_text())
        for key in authored:
            if key in previous:
                authored[key] = previous[key]
    document = {"format": 2, "id": map_id, "name": name, "geometry": "geometry.glb", "grid": grid.json(),
                "points": points, **authored, "lights": list(lights), "emitters": list(emitters),
                "colliders": [], "bake": {"overrides": list(overrides)}, "environment": environment}
    write_json(target, document)
    return document


def walkable_override(x, z, w=1, d=1):
    return {"x": x, "z": z, "layer": 0, "w": w, "d": d, "walkable": True}


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


def prop_tree(path, height=1.8, colour=(.2, .38, .22)):
    """A tree whose trunk alone blocks (a hidden collision box), so bodies walk under the crown."""
    scene = Diorama()
    scene.tree(0, 0, height, colour)
    scene.collision_box(0, 0, 0, 0.3, height * .7, 0.3)
    scene.write(path)


def prop_lamp(path, colour=(1, .62, .26)):
    scene = Diorama()
    scene.box(0, 0, 0, 0.16, 1.9, 0.16, (.22, .2, .2))
    scene.box(0, 1.9, 0, 0.32, 0.4, 0.32, tuple(c * 2.2 for c in colour[:3]))
    scene.collision_box(0, 0, 0, 0.25, 1.9, 0.25)
    scene.write(path)


def prop_fence(path, length=2):
    """A fence `length` cells long along x, a quarter cell thick: it stands on a
    cell boundary and blocks both sides' edge quarters."""
    scene = Diorama()
    for x in (-length / 2 + 0.1, length / 2 - 0.1):
        scene.box(x, 0, 0, 0.12, 1.0, 0.12, (.42, .3, .18))
    for y in (0.35, 0.8):
        scene.box(0, y, 0, length, 0.08, 0.06, (.52, .38, .22))
    scene.collision_box(0, 0, 0, length, 1.0, 0.25)
    scene.write(path)


def kit_floor(path, cells, colour=(.52, .5, .46)):
    """A floor slab `cells` x `cells`: the bake reads its top as ground."""
    scene = Diorama()
    scene.box(0, 0, 0, cells, 0.1, cells, colour, top=tuple(min(1, c * 1.1) for c in colour))
    scene.write(path)


def kit_cliff(path, cells, height=1.0, colour=PALETTE["cliff"]):
    """A raised block whose top is a floor `height` up; its sides are cliff faces."""
    scene = Diorama()
    scene.box(0, 0, 0, cells, height, cells, colour, top=PALETTE["grass"])
    scene.write(path)


def kit_stairs(path, height=1.0, length=2):
    """Stairs rising `height` over `length` cells along +x (origin at the centre of the base)."""
    scene = Diorama()
    steps = 8
    for i in range(steps):
        x0 = -length / 2 + i * length / steps
        scene.box(x0 + length / steps / 2, 0, 0, length / steps, height * (i + 1) / steps, 1, PALETTE["stone"])
    # The bake reads the slope from the collision ramp, not the steps.
    for z0, z1 in ((-0.5, 0.5),):
        scene.quad([(-length / 2, 0, z0), (-length / 2, 0, z1), (length / 2, height, z1), (length / 2, height, z0)],
                   (1, 1, 1), "collision")
        scene.quad([(length / 2, height, z0), (length / 2, height, z1), (length / 2, 0, z1), (length / 2, 0, z0)],
                   (1, 1, 1), "collision")
    scene.write(path)


def sample_props(folder):
    """Models for the Assets panel: props with their origin at the centre of
    the base and footprints of whole cells, and a kit of ground pieces."""
    prop_house(folder / "fx" / "house.glb", 4, 3)
    prop_house(folder / "fx" / "house_small.glb", 3, 3, wall=(.66, .58, .52), roof=(.28, .3, .44))
    prop_tree(folder / "fx" / "tree.glb")
    prop_lamp(folder / "fx" / "lamp.glb")
    prop_fence(folder / "fx" / "fence.glb")
    kit = folder / "fx" / "kit"
    kit_floor(kit / "floor_1x1.glb", 1)
    kit_floor(kit / "floor_2x2.glb", 2)
    kit_cliff(kit / "cliff_1x1.glb", 1)
    kit_cliff(kit / "cliff_2x2.glb", 2)
    kit_stairs(kit / "stairs_1x2.glb")


def prop_house(path, w, d, wall=(.74, .62, .45), roof=(.45, .22, .2)):
    """A house model for the editor's Assets panel: origin at the centre of its
    base, footprint w x d cells, with a hidden collision box over the walls (the
    roof's overhang does not block)."""
    scene = Diorama()
    scene.box(0, 0, 0, w, 2.2, d, wall)
    scene.roof(0, 2.2, 0, w + 0.4, 1.4, d + 0.4, roof)
    scene.box(0, 0.05, d / 2 + 0.02, 0.9, 1.4, 0.05, (.25, .17, .12))
    for wx in (-w / 2 + 0.8, w / 2 - 0.8):
        scene.box(wx, 1.0, d / 2 + 0.03, 0.6, 0.6, 0.05, (1.3, .9, .4))
    scene.collision_box(0, 0, 0, w, 2.2, d)
    scene.write(path)


def tree(scene, grid, x, z, colour=(.2, .38, .22)):
    scene.tree(x + 0.5, z + 0.5, 1.8, colour)
    grid.fill(x, z, x + 1, z + 1, walk=False)


def lamp(scene, x, z, lights, colour=(1, .62, .26)):
    scene.box(x, 0, z, 0.16, 1.9, 0.16, (.22, .2, .2))
    scene.box(x, 1.9, z, 0.32, 0.4, 0.32, (2.2, 1.5, .6))
    lamp_light(x, z, lights, colour)


def lamp_light(x, z, lights, colour=(1, .62, .26)):
    lights.append({"position": [x, 2.1, z], "color": list(colour), "radius": 5.5, "intensity": 2.6, "flicker": 0.08})


def prop(id_, model, x, z, yaw=0):
    """A placed model: the walkability bake makes its collision from the model."""
    placement = {"id": id_, "type": "prop", "position": [x, 0, z], "props": {"model": model}}
    if yaw:
        placement["yaw"] = yaw
    return placement


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


# --------------------------------------------------------------------------- Starfall (the sample game)

def vehicle_sprite(folder, name, draw):
    """A one-clip, four-direction sprite of two frames (the strider, the skiff)."""
    c = Canvas(64, 48 * 4)
    directions = []
    for direction in range(4):
        entries = []
        for frame in range(2):
            ox, oy = frame * 32, direction * 48
            draw(c, ox, oy, direction, frame)
            entries.append({"rect": [ox, oy, 32, 48], "duration": 0.3})
        directions.append(entries)
    c.save(folder / "sprites" / f"{name}.png")
    write_json(folder / "sprites" / f"{name}.anim.json", {
        "atlas": f"{name}.png", "pixels_per_unit": 32, "pivot": [0.5, 1],
        "clips": {"idle": {"loop": True, "directions": [[d[0]] for d in directions]},
                  "walk": {"loop": True, "directions": directions}}})


def draw_strider(c, ox, oy, direction, frame):
    """A long-legged riding bird."""
    body, dark, beak = (236, 196, 72), (176, 132, 40), (232, 120, 48)
    bob = frame
    side = direction in (1, 3)
    flip = -1 if direction == 1 else 1
    c.rect(ox + 12, oy + 34 + bob, 2, 12 - bob, dark)
    c.rect(ox + 18, oy + 34 - bob + 1, 2, 12 + bob - 1, dark)
    c.ellipse(ox + 16, oy + 28 - bob, 10, 8, OUTLINE)
    c.ellipse(ox + 16, oy + 28 - bob, 9, 7, body)
    if side:
        c.rect(ox + 16 + flip * 7 - 2, oy + 12 - bob, 5, 14, body)
        c.ellipse(ox + 16 + flip * 8, oy + 11 - bob, 5, 4, body)
        c.rect(ox + 16 + flip * 12 - 1, oy + 11 - bob, 4, 2, beak)
        c.rect(ox + 16 + flip * 9, oy + 9 - bob, 2, 2, OUTLINE)
        c.rect(ox + 16 - flip * 10 - 1, oy + 22 - bob, 4, 6, dark)
    else:
        c.rect(ox + 14, oy + 12 - bob, 5, 12, body)
        c.ellipse(ox + 16, oy + 11 - bob, 5, 4, body)
        if direction == 0:
            c.rect(ox + 15, oy + 12 - bob, 3, 2, beak)
            c.rect(ox + 13, oy + 9 - bob, 2, 2, OUTLINE); c.rect(ox + 18, oy + 9 - bob, 2, 2, OUTLINE)


def draw_skiff(c, ox, oy, direction, frame):
    """A small airship: a hull under a balloon, with a spinning propeller."""
    hull, trim, bag = (132, 86, 52), (214, 178, 96), (206, 84, 70)
    c.ellipse(ox + 16, oy + 14, 13, 9, OUTLINE)
    c.ellipse(ox + 16, oy + 14, 12, 8, bag)
    c.rect(ox + 6, oy + 13, 20, 2, trim)
    c.rect(ox + 9, oy + 22, 1, 8, OUTLINE); c.rect(ox + 22, oy + 22, 1, 8, OUTLINE)
    c.rect(ox + 6, oy + 30, 20, 9, OUTLINE)
    c.rect(ox + 7, oy + 31, 18, 7, hull)
    c.rect(ox + 7, oy + 31, 18, 2, trim)
    blade = 6 if frame == 0 else 2
    if direction in (1, 3):
        x = ox + (3 if direction == 3 else 27)
        c.rect(x, oy + 34 - blade // 2, 2, blade + 1, (230, 230, 240))
    else:
        c.rect(ox + 16 - blade // 2, oy + (39 if direction == 0 else 29), blade + 1, 2, (230, 230, 240))


def overworld_icons(folder):
    """Small landmark models for the overworld: a town, a cave mouth, a shrine."""
    town = Diorama()
    for x, z, w in ((-.22, -.1, .4), (.24, .05, .34), (-.05, .28, .3)):
        town.box(x, 0, z, w, .28, w, (.74, .62, .45))
        town.roof(x, .28, z, w + .1, .2, w + .1)
    town.write(folder / "fx" / "ow_town.glb")
    cave = Diorama()
    cave.box(0, 0, -.1, .9, .7, .6, (.36, .34, .36))
    cave.box(0, 0, .22, .36, .42, .1, (.05, .05, .08))
    cave.box(-.3, .7, -.1, .25, .25, .3, (.3, .29, .31))
    cave.write(folder / "fx" / "ow_cave.glb")
    shrine = Diorama()
    shrine.box(0, 0, 0, .9, .12, .9, (.8, .8, .84))
    for x, z in ((-.3, -.3), (.3, -.3), (-.3, .3), (.3, .3)):
        shrine.box(x, .12, z, .12, .6, .12, (.86, .86, .9))
    shrine.box(0, .72, 0, .9, .1, .9, (.7, .74, .9))
    shrine.box(0, .12, 0, .2, .4, .2, (.5, 1.4, 2.4))
    shrine.write(folder / "fx" / "ow_shrine.glb")


def overworld(folder):
    """The walkable world map: a continent cut by a river only a strider fords,
    mountains, forests with their own encounters, and an islet only the skiff
    reaches. The grid wraps east-west and north-south."""
    rng = random.Random(41)
    W, D = 40, 28
    grid = GridBuilder(W, D)
    land = [[False] * W for _ in range(D)]

    def blob(cx, cz, rx, rz):
        for z in range(D):
            for x in range(W):
                if ((x + .5 - cx) / rx) ** 2 + ((z + .5 - cz) / rz) ** 2 <= 1:
                    land[z][x] = True
    blob(12, 14, 8.5, 8)     # the western half: the town's plains
    blob(25, 13, 7.5, 8.5)   # the eastern half: forest and the grotto's hills
    blob(18.5, 14, 4, 6)     # joined across the river
    blob(35, 12, 2.6, 2.6)   # the shrine's islet
    river = {(x, z) for z in range(D) for x in (18, 19) if land[z][x]}
    mountains = {(x, z) for x, z in [(22, 7), (23, 7), (24, 7), (27, 8), (28, 9), (28, 10), (28, 11), (22, 8),
                                     (21, 9), (27, 12), (26, 13), (9, 8), (10, 8), (11, 7), (6, 17), (7, 18)]
                 if land[z][x]}
    forest = {(x, z) for z in range(D) for x in range(W)
              if land[z][x] and x >= 20 and (x, z) not in mountains and not (24 <= x <= 26 and 9 <= z <= 12)
              and x < 32}
    for z in range(D):
        for x in range(W):
            if not land[z][x]:
                continue
            if (x, z) in river:
                grid.fill(x, z, x + 1, z + 1, walk=False, height=-0.12, tag="shallows")
            elif (x, z) in mountains:
                grid.fill(x, z, x + 1, z + 1, walk=False, height=1.3, tag="mountain")
            elif (x, z) in forest:
                grid.fill(x, z, x + 1, z + 1, tag="encounter:forest")
                grid.fill(x, z, x + 1, z + 1, tag="forest")
            elif x < 32 and not (9 <= x <= 13 and 12 <= z <= 16):
                grid.fill(x, z, x + 1, z + 1, tag="encounter:plains")
    # The sea: every cell that is not land, as rectangles of water one row at a time.
    for z in range(D):
        x = 0
        while x < W:
            if land[z][x]:
                x += 1
                continue
            end = x
            while end < W and not land[z][end]:
                end += 1
            grid.fill(x, z, end, z + 1, walk=False, height=-0.3, tag="water")
            grid.water.append((x, z, end, z + 1, -0.1))
            x = end
    for x, z in sorted(river):
        grid.water.append((x, z, x + 1, z + 1, -0.04))

    def colour(x, z):
        if (x, z) in mountains:
            return PALETTE["cliff"]
        if (x, z) in forest:
            return (.2, .36, .22)
        if (x, z) in river:
            return PALETTE["sand"]
        edge = any(not land[(z + dz) % D][(x + dx) % W] for dx, dz in ((1, 0), (-1, 0), (0, 1), (0, -1)))
        return PALETTE["sand"] if edge else PALETTE["grass"]
    scene = terrain(grid, colour, rng)
    # What the walkability bake reads instead of the visible mesh: flat ground
    # under every land cell (the forest's trees are scenery, not obstacles),
    # nothing under the river and the sea, and the mountains as blocks.
    for z in range(D):
        for x in range(W):
            if land[z][x] and (x, z) not in river and (x, z) not in mountains:
                scene.quad([(x, 0, z), (x, 0, z + 1), (x + 1, 0, z + 1), (x + 1, 0, z)], (1, 1, 1), "collision")
    for x, z in sorted(mountains):
        scene.collision_box(x + .5, 0, z + .5, 1, 1.3, 1)
        scene.box(x + .5, 1.3, z + .5, .6, rng.uniform(.4, .9), .6, PALETTE["snow"])
    for x, z in sorted(forest):
        if rng.random() < 0.55:
            scene.box(x + rng.uniform(.3, .7), 0, z + rng.uniform(.3, .7), .12, .3, .12, (.32, .22, .15))
            scene.box(x + .5, .25, z + .5, .7, .45, .7, tuple(v * rng.uniform(.85, 1.1) for v in (.16, .34, .2)))
    placements = [
        {"id": "amber_vale", "type": "door", "position": [11.5, 0, 14.5],
         "props": {"map": "town", "point": "entry", "name": "Amber Vale", "model": "fx/ow_town.glb", "solid": False}},
        {"id": "crystal_grotto", "type": "door", "position": [25.5, 0, 10.5],
         "props": {"map": "grotto", "point": "entry", "name": "Crystal Grotto", "model": "fx/ow_cave.glb",
                   "solid": False}},
        {"id": "starfall_shrine", "type": "npc", "position": [35.5, 0, 12.5],
         "props": {"name": "Starfall Shrine", "model": "fx/ow_shrine.glb", "script": "events.shrine",
                   "solid": False}},
        {"id": "strider", "type": "vehicle", "position": [13.5, 0, 15.5],
         "props": {"kind": "strider", "sprite": "sprites/strider.anim.json"}},
        {"id": "skiff", "type": "vehicle", "position": [9.5, 0, 16.5],
         "props": {"kind": "skiff", "sprite": "sprites/skiff.anim.json"}},
    ]
    doc = write_map(folder, "overworld", "The Starfall Coast", grid, scene, {
        "ambient": [.5, .5, .48], "sun_color": [1, .92, .78], "sun_direction": [-.5, -1, -.35],
        "fog_color": [.66, .76, .86], "fog_density": .006, "shadow_distance": 40,
        "sky": {"mode": "gradient", "top": [.24, .44, .8], "horizon": [.8, .86, .92], "cloud_cover": .35},
        "water": {"color": [.1, .36, .56], "deep_color": [.04, .14, .3]},
        "post": {"bloom_strength": .3, "focus": .55, "focus_range": .3, "lut": "ui/amber.cube"}},
        {"start": {"position": [11.5, 0, 15.5], "facing": [0, 1]},
         "town": {"position": [11.5, 0, 15.5], "facing": [0, 1]},
         "grotto": {"position": [25.5, 0, 11.5], "facing": [0, 1]},
         "shrine": {"position": [35.5, 0, 13.5], "facing": [0, 1]}},
        placements=placements,
        props={"music": "overworld", "battle_stage": "battle_grass", "overworld": True,
               "camera": {"pitch": 52, "distance": 21, "fov": 30}})
    # The map wraps, and its bake keeps every landmass: the far bank and the
    # islet are reached by strider and skiff, not on foot, so they are not
    # stray islands to prune. The river and the peaks stay closed to walkers
    # whatever the bake makes of their surfaces.
    target = folder / "maps" / "overworld" / "map.json"
    doc["grid"]["wrap"] = True
    doc["bake"]["options"] = {"prune_islands": False, "block_edges": False}
    doc["bake"]["overrides"] = [{"x": x, "z": z, "layer": 0, "w": 1, "d": 1, "walkable": False}
                                for x, z in sorted(river | mountains)]
    write_json(target, doc)


def starfall():
    folder = REPO / "samples" / "starfall"
    looks = {
        "aster": {"coat": (178, 61, 66), "hair": (83, 53, 40), "weapon": (200, 210, 230)},
        "mira": {"coat": (49, 126, 142), "hair": (235, 197, 118), "trim": (240, 240, 250)},
        "bram": {"coat": (96, 108, 76), "hair": (48, 44, 40), "weapon": (150, 120, 70), "hat": (70, 76, 52)},
        "sel": {"coat": (142, 92, 168), "hair": (236, 236, 244), "trim": (255, 214, 120)},
        "elder": {"coat": (118, 93, 157), "hair": (204, 203, 197)},
        "merchant": {"coat": (89, 142, 86), "hair": (104, 64, 40), "hat": (120, 80, 40)},
        "innkeeper": {"coat": (74, 109, 163), "hair": (124, 76, 44)},
        "villager": {"coat": (160, 120, 80), "hair": (60, 40, 30)},
        "stablehand": {"coat": (196, 150, 70), "hair": (150, 70, 40), "hat": (220, 200, 150)},
        "warden": {"coat": (90, 170, 220), "hair": (220, 240, 255), "trim": (255, 255, 255)},
    }
    for name, look in looks.items():
        sprite_sheet(folder, name, look)
    for name in ("aster", "mira", "bram", "sel", "elder"):
        portrait(folder, name, looks[name])
    sprite_sheet(folder, "slime", {"kind": "slime", "coat": (86, 177, 132)}, monster=True)
    sprite_sheet(folder, "wisp", {"kind": "wisp", "coat": (120, 170, 255)}, monster=True)
    sprite_sheet(folder, "golem", {"kind": "golem", "coat": (106, 112, 138)}, monster=True)
    sprite_sheet(folder, "wolf", {"kind": "wolf", "coat": (128, 110, 96)}, monster=True)
    sprite_sheet(folder, "ember", {"kind": "wisp", "coat": (255, 140, 70)}, monster=True)
    vehicle_sprite(folder, "strider", draw_strider)
    vehicle_sprite(folder, "skiff", draw_skiff)
    render_showcase(folder)
    chest_sprite(folder)
    crystal_sprite(folder, (90, 190, 255))
    standard_audio(folder, {"vale": (262, "major", 96), "grotto": (220, "minor", 80), "battle": (220, "minor", 150),
                            "boss": (196, "minor", 132), "title": (294, "major", 72),
                            "overworld": (330, "major", 112)})
    lut(folder / "ui" / "amber.cube", (1.04, 1.0, 0.94), 1.05)
    lut(folder / "ui" / "grotto.cube", (0.95, 1.0, 1.05), 1.1)
    backdrop(folder / "ui" / "hills.png", (84, 110, 96), 3, 4)

    sample_props(folder)
    overworld_icons(folder)
    # The grotto's guardian: a rigged, animated 3D creature among the sprites.
    from generate_creature import write_creature
    write_creature(folder / "fx" / "drake.glb", (0.36, 0.5, 0.62))

    # Town: the terrain is ground, paths and the pond; houses, trees, lamps and
    # a fence are placed models, and the walkability bake derives the grid.
    rng = random.Random(11)
    grid = GridBuilder(20, 16)
    paths = lambda x, z: PALETTE["path"] if (8 <= x <= 11 or 7 <= z <= 8) else PALETTE["grass"]
    lights = []
    water_patch(grid, 14, 10, 18, 13)
    scene = terrain(grid, paths, rng)
    for x, z in [(7.5, 6.5), (12.5, 9.5), (8.5, 12.5)]:
        lamp_light(x, z, lights)
    # A Blender-style marker node: the editor's "Import markers" turns it into a point.
    scene.extra_nodes.append({"name": "well", "translation": [6.5, 0, 8.5], "extras": {"fengine": {"spawn": "well"}}})
    placements = [
        prop("house_inn", "fx/house.glb", 4, 3.5),                 # cells 2..5 x 2..4
        prop("house_merchant", "fx/house_small.glb", 14.5, 3.5),   # cells 13..15 x 2..4
        prop("house_south", "fx/house.glb", 4, 11.5),              # cells 2..5 x 10..12
        prop("tree_west", "fx/tree.glb", 1.5, 6.5),
        prop("tree_east", "fx/tree.glb", 18.5, 6.5),
        prop("tree_south", "fx/tree.glb", 6.5, 13.5),
        prop("tree_gate", "fx/tree.glb", 12.5, 14.5),
        prop("tree_corner", "fx/tree.glb", 1.5, 13.5),
        prop("lamp_plaza", "fx/lamp.glb", 7.5, 6.5),
        prop("lamp_east", "fx/lamp.glb", 12.5, 9.5),
        prop("lamp_south", "fx/lamp.glb", 8.5, 12.5),
        prop("fence_pond", "fx/fence.glb", 14, 11, yaw=90),         # along the pond's west bank
        npc("elder", 5, 6, "elder", {"name": "Elder Rowan", "script": "events.elder"}),
        npc("merchant", 14, 6, "merchant", {"name": "Merchant", "script": "events.merchant"}),
        npc("innkeeper", 3, 6, "innkeeper", {"name": "Innkeeper", "script": "events.inn"}, facing=(1, 0)),
        npc("villager", 11, 11, "villager", {"name": "Villager", "wander": True, "script": "events.villager"}),
        npc("stablehand", 12, 13, "stablehand", {"name": "Stablehand", "script": "events.stablehand"},
            facing=(-1, 0)),
        {"id": "town_chest", "type": "chest", "position": [16.5, 0, 8.5], "props": {"item": "potion", "count": 2,
                                                                                     "sprite": "sprites/chest.anim.json"}},
        {"id": "sign", "type": "sign", "position": [11.5, 0, 13.5],
         "props": {"text": "South gate: the Starfall Coast. The Crystal Grotto lies east, across the river."}},
        {"id": "save", "type": "save_point", "position": [9.5, 0, 9.5], "props": {"sprite": "sprites/crystal.anim.json"}},
        door("to_coast", 9, 15, "overworld", "town"),
        door("to_coast2", 10, 15, "overworld", "town"),
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
        {"entry": {"position": [9.5, 0, 13.6], "facing": [0, -1]}},
        lights=lights, placements=placements, zones=zones,
        props={"music": "vale", "camera": {"pitch": 42, "distance": 18, "fov": 30, "min": [6, 5], "max": [14, 11]}},
        overrides=[walkable_override(9, 15, 2, 1)])  # the gate on the edge ring

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
        {"id": "guardian", "type": "boss", "position": [8, 0.5, 5.3], "facing": [0, 1],
         "props": {"name": "Grotto Drake", "model": "fx/drake.glb", "script": "events.guardian", "solid": False,
                   "hide_flag": "guardian_defeated"}},
        {"id": "vale_crystal", "type": "npc", "position": [8, 0.5, 4.2],
         "props": {"name": "Vale Crystal", "sprite": "sprites/crystal.anim.json", "script": "events.crystal"}},
        {"id": "grotto_chest", "type": "chest", "position": [2.5, 0, 6.5], "props": {"item": "ether", "count": 1,
                                                                                      "sprite": "sprites/chest.anim.json"}},
        door("exit", 7, 15, "overworld", "grotto"), door("exit2", 8, 15, "overworld", "grotto"),
        {"id": "drip", "type": "sound", "position": [12, 1, 12], "props": {"sound": "audio/chime.wav", "radius": 6, "volume": .2}},
        {"id": "wisp_symbol", "type": "symbol", "position": [4.5, 0, 9.5], "props": {"troop": "wisps",
                                                                                      "sprite": "sprites/wisp.anim.json"}},
    ]
    # A polygon zone across the cave before the dais: walking into it runs a script, once.
    zones = [{"id": "drake_warning", "type": "event", "shape": "polygon", "min": [3, 8], "max": [13, 10.5],
              "points": [[4, 10.5], [12, 10.5], [13, 9], [10, 8], [6, 8], [3, 9]],
              "props": {"script": "events.warning", "once": True}}]
    write_map(folder, "grotto", "Crystal Grotto", grid, scene, {
        "ambient": [.16, .2, .32], "sun_color": [.3, .4, .6], "sun_direction": [-.3, -1, -.2], "fog_color": [.06, .09, .16],
        "fog_density": .03, "sky": {"mode": "color", "horizon": [.04, .05, .1]},
        "weather": {"kind": "dust", "intensity": .8, "color": [.6, .8, 1, .6]},
        "post": {"bloom_strength": .55, "focus": .5, "focus_range": .2, "lut": "ui/grotto.cube"}},
        {"entry": {"position": [8, 0, 14.4], "facing": [0, -1]}}, lights=lights, placements=placements, zones=zones,
        props={"music": "grotto", "battle_stage": "battle_cave", "camera": {"pitch": 46, "distance": 17, "fov": 30}},
        overrides=[walkable_override(7, 15, 2, 1)])  # the exit on the edge ring

    overworld(folder)

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


# --------------------------------------------------------------------------- test fixture

def fixture():
    """A small arena used by the engine's own C++ and Lua tests."""
    root = REPO / "tests" / "game"
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
    """Default UI art shipped in builtin/assets/ui (window skin, cursor, emotes)."""
    root = REPO / "builtin" / "assets" / "ui"
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
    parser.add_argument("targets", nargs="*", default=["fixture"], help="engine, fixture, starfall, all")
    parser.add_argument("--headless", help="fengine_headless executable: bakes each sample's walkability afterwards")
    parser.add_argument("--fresh", action="store_true",
                        help="rewrite placements, zones and map props from the generator (editor edits are lost)")
    args = parser.parse_args()
    global KEEP_AUTHORED
    KEEP_AUTHORED = not args.fresh
    targets = set(args.targets)
    if "all" in targets:
        targets = {"engine", "fixture", "starfall"}
    for name in sorted(targets):
        globals()[name]()
        print(f"generated {name}")
    if args.headless:
        import subprocess
        for name in sorted(targets & {"starfall"}):
            subprocess.run([args.headless, "--bake", str(REPO / "samples" / name)], check=True)


if __name__ == "__main__":
    main()
