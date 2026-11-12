#!/usr/bin/env python3
"""Generates a small skinned, animated creature as a binary glTF.

The placeholder for a rigged character or monster exported from Blender: a
winged drake of coloured boxes on a skeleton of 14 joints, with a tail whose
vertices blend between neighbouring joints, and the clips a turn-based battle
plays: idle, attack, cast, hit and die. It exercises everything the engine
reads from a skin: joints, inverse bind matrices, JOINTS_0 and WEIGHTS_0, and
translation, rotation and scale channels on the joints.

    python3 tools/generate_creature.py <out.glb> [--tint r,g,b]

tools/generate_samples.py calls `write_creature` for the sample and for the
engine tests' fixture game.
"""
import argparse
import json
import math
import struct
from pathlib import Path

# name, parent, rest translation relative to the parent
JOINTS = [
    ("root", None, (0, 0, 0)),
    ("body", "root", (0, 0.9, 0)),
    ("neck", "body", (0, 0.45, 0.45)),
    ("head", "neck", (0, 0.35, 0.3)),
    ("wing_l", "body", (-0.4, 0.4, 0)),
    ("wingtip_l", "wing_l", (-0.8, 0.15, 0)),
    ("wing_r", "body", (0.4, 0.4, 0)),
    ("wingtip_r", "wing_r", (0.8, 0.15, 0)),
    ("tail1", "body", (0, -0.05, -0.5)),
    ("tail2", "tail1", (0, -0.05, -0.45)),
    ("tail3", "tail2", (0, -0.05, -0.45)),
    ("leg_l", "body", (-0.3, -0.35, 0.1)),
    ("leg_r", "body", (0.3, -0.35, 0.1)),
    ("jaw", "head", (0, -0.1, 0.2)),
]
INDEX = {name: i for i, (name, _, _) in enumerate(JOINTS)}


def rest_position(name):
    x = y = z = 0.0
    while name is not None:
        _, parent, (tx, ty, tz) = JOINTS[INDEX[name]]
        x, y, z = x + tx, y + ty, z + tz
        name = parent
    return (x, y, z)


def quat(axis, degrees):
    """Unit quaternion (x, y, z, w) turning `degrees` about `axis`."""
    half = math.radians(degrees) / 2
    length = math.sqrt(sum(a * a for a in axis)) or 1
    s = math.sin(half) / length
    return (axis[0] * s, axis[1] * s, axis[2] * s, math.cos(half))


def quat_mul(a, b):
    ax, ay, az, aw = a
    bx, by, bz, bw = b
    return (aw * bx + ax * bw + ay * bz - az * by, aw * by - ax * bz + ay * bw + az * bx,
            aw * bz + ax * by - ay * bx + az * bw, aw * bw - ax * bx - ay * by - az * bz)


class Mesh:
    def __init__(self):
        self.vertices = []  # (position, normal, uv, colour, joints, weights)
        self.indices = []

    def quad(self, points, colour, binding):
        a, b, c = points[0], points[1], points[2]
        u, v = [b[i] - a[i] for i in range(3)], [c[i] - a[i] for i in range(3)]
        n = [u[1] * v[2] - u[2] * v[1], u[2] * v[0] - u[0] * v[2], u[0] * v[1] - u[1] * v[0]]
        length = math.sqrt(sum(x * x for x in n)) or 1
        n = tuple(x / length for x in n)
        linear = tuple(max(0.0, x) ** 2.2 for x in colour) + (1.0,)
        base = len(self.vertices)
        for point, uv in zip(points, [(0, 0), (1, 0), (1, 1), (0, 1)]):
            joints, weights = binding(point)
            self.vertices.append((point, n, uv, linear, joints, weights))
        self.indices += [base, base + 1, base + 2, base, base + 2, base + 3]

    def box(self, centre, size, colour, joint, binding=None):
        """An axis-aligned box bound wholly to `joint`, or per vertex by `binding(point)`."""
        cx, cy, cz = centre
        hx, hy, hz = size[0] / 2, size[1] / 2, size[2] / 2
        x0, x1, y0, y1, z0, z1 = cx - hx, cx + hx, cy - hy, cy + hy, cz - hz, cz + hz
        bind = binding or (lambda point: ((INDEX[joint], 0, 0, 0), (1.0, 0.0, 0.0, 0.0)))
        faces = [([(x0, y1, z0), (x0, y1, z1), (x1, y1, z1), (x1, y1, z0)], 1.0),   # top
                 ([(x0, y0, z1), (x1, y0, z1), (x1, y1, z1), (x0, y1, z1)], 0.86),  # +z
                 ([(x1, y0, z0), (x0, y0, z0), (x0, y1, z0), (x1, y1, z0)], 0.66),  # -z
                 ([(x0, y0, z0), (x0, y0, z1), (x0, y1, z1), (x0, y1, z0)], 0.76),  # -x
                 ([(x1, y0, z1), (x1, y0, z0), (x1, y1, z0), (x1, y1, z1)], 0.94),  # +x
                 ([(x0, y0, z1), (x0, y0, z0), (x1, y0, z0), (x1, y0, z1)], 0.5)]   # bottom
        for points, shade in faces:
            self.quad(points, tuple(c * shade for c in colour), bind)


def tail_binding(point):
    """Blend along the tail by depth: each vertex follows its two nearest tail joints."""
    chain = ["body", "tail1", "tail2", "tail3"]
    stops = [rest_position(name)[2] for name in chain]  # decreasing z
    z = point[2]
    for i in range(len(chain) - 1):
        if z >= stops[i + 1] or i == len(chain) - 2:
            span = stops[i] - stops[i + 1]
            t = min(1.0, max(0.0, (stops[i] - z) / span))
            return (INDEX[chain[i]], INDEX[chain[i + 1]], 0, 0), (1.0 - t, t, 0.0, 0.0)
    return (INDEX["tail3"], 0, 0, 0), (1.0, 0.0, 0.0, 0.0)


def build_mesh(tint):
    hide = tint
    belly = tuple(min(1.0, c * 1.35 + 0.1) for c in tint)
    dark = tuple(c * 0.55 for c in tint)
    mesh = Mesh()
    body = rest_position("body")
    mesh.box((body[0], body[1], body[2]), (0.8, 0.7, 1.0), hide, "body")
    mesh.box((body[0], body[1] - 0.3, body[2] + 0.05), (0.6, 0.2, 0.8), belly, "body")
    neck = rest_position("neck")
    mesh.box((neck[0], neck[1] + 0.1, neck[2] + 0.1), (0.35, 0.5, 0.35), hide, "neck")
    head = rest_position("head")
    mesh.box((head[0], head[1] + 0.05, head[2] + 0.15), (0.45, 0.35, 0.6), hide, "head")
    mesh.box((head[0] - 0.15, head[1] + 0.3, head[2]), (0.08, 0.25, 0.08), (0.9, 0.85, 0.7), "head")  # horns
    mesh.box((head[0] + 0.15, head[1] + 0.3, head[2]), (0.08, 0.25, 0.08), (0.9, 0.85, 0.7), "head")
    mesh.box((head[0] - 0.14, head[1] + 0.1, head[2] + 0.4), (0.08, 0.08, 0.08), (1.0, 0.85, 0.2), "head")  # eyes
    mesh.box((head[0] + 0.14, head[1] + 0.1, head[2] + 0.4), (0.08, 0.08, 0.08), (1.0, 0.85, 0.2), "head")
    jaw = rest_position("jaw")
    mesh.box((jaw[0], jaw[1] - 0.05, jaw[2] + 0.05), (0.36, 0.1, 0.45), dark, "jaw")
    for side, name, tip in ((-1, "wing_l", "wingtip_l"), (1, "wing_r", "wingtip_r")):
        root, end = rest_position(name), rest_position(tip)
        mesh.box(((root[0] + end[0]) / 2, root[1] + 0.05, root[2] - 0.1), (0.8, 0.08, 0.7), dark, name)
        mesh.box((end[0] + side * 0.4, end[1], end[2] - 0.15), (0.8, 0.06, 0.9), hide, tip)
    # The tail: three segments whose vertices blend between joints.
    tail_start, tail_end = rest_position("tail1")[2] + 0.2, rest_position("tail3")[2] - 0.45
    segments = 6
    for i in range(segments):
        z0 = tail_start + (tail_end - tail_start) * i / segments
        z1 = tail_start + (tail_end - tail_start) * (i + 1) / segments
        width = 0.42 - 0.3 * i / segments
        y = rest_position("tail1")[1] - 0.04 * i
        mesh.box((0, y, (z0 + z1) / 2), (width, width, abs(z1 - z0)), hide if i % 2 == 0 else dark, "tail1",
                 binding=tail_binding)
    for name in ("leg_l", "leg_r"):
        leg = rest_position(name)
        mesh.box((leg[0], leg[1] - 0.25, leg[2]), (0.22, 0.55, 0.26), dark, name)
        mesh.box((leg[0], 0.05, leg[2] + 0.1), (0.3, 0.1, 0.45), hide, name)
    return mesh


# ---- animations: per clip, per joint, lists of (time, value) keys
def clips():
    def rot(joint, keys):
        return (joint, "rotation", keys)

    def move(joint, keys):
        base = JOINTS[INDEX[joint]][2]
        return (joint, "translation", [(t, tuple(base[i] + d[i] for i in range(3))) for t, d in keys])

    x, y, z = (1, 0, 0), (0, 1, 0), (0, 0, 1)
    identity = (0, 0, 0, 1)
    idle = [
        move("body", [(0, (0, 0, 0)), (1, (0, 0.06, 0)), (2, (0, 0, 0))]),
        rot("neck", [(0, quat(x, -4)), (1, quat(x, 5)), (2, quat(x, -4))]),
        rot("head", [(0, quat(y, -6)), (1, quat(y, 6)), (2, quat(y, -6))]),
        rot("wing_l", [(0, quat(z, 8)), (0.5, quat(z, -14)), (1, quat(z, 8)), (1.5, quat(z, -14)), (2, quat(z, 8))]),
        rot("wing_r", [(0, quat(z, -8)), (0.5, quat(z, 14)), (1, quat(z, -8)), (1.5, quat(z, 14)), (2, quat(z, -8))]),
        rot("wingtip_l", [(0, quat(z, 10)), (0.5, quat(z, -18)), (1, quat(z, 10)), (1.5, quat(z, -18)), (2, quat(z, 10))]),
        rot("wingtip_r", [(0, quat(z, -10)), (0.5, quat(z, 18)), (1, quat(z, -10)), (1.5, quat(z, 18)), (2, quat(z, -10))]),
        rot("tail1", [(0, quat(y, -10)), (1, quat(y, 10)), (2, quat(y, -10))]),
        rot("tail2", [(0, quat(y, -14)), (1, quat(y, 14)), (2, quat(y, -14))]),
        rot("tail3", [(0, quat(y, -18)), (1, quat(y, 18)), (2, quat(y, -18))]),
    ]
    attack = [
        move("body", [(0, (0, 0, 0)), (0.25, (0, 0.1, -0.25)), (0.4, (0, -0.05, 0.7)), (0.8, (0, 0, 0))]),
        rot("body", [(0, identity), (0.25, quat(x, -12)), (0.4, quat(x, 16)), (0.8, identity)]),
        rot("neck", [(0, identity), (0.25, quat(x, -25)), (0.4, quat(x, 35)), (0.8, identity)]),
        rot("jaw", [(0, identity), (0.3, quat(x, 35)), (0.45, identity), (0.8, identity)]),
        rot("wing_l", [(0, identity), (0.25, quat(z, -40)), (0.45, quat(z, 20)), (0.8, identity)]),
        rot("wing_r", [(0, identity), (0.25, quat(z, 40)), (0.45, quat(z, -20)), (0.8, identity)]),
        rot("tail1", [(0, identity), (0.4, quat(x, -20)), (0.8, identity)]),
    ]
    cast = [
        move("body", [(0, (0, 0, 0)), (0.4, (0, 0.35, 0)), (0.8, (0, 0.35, 0)), (1.1, (0, 0, 0))]),
        rot("body", [(0, identity), (0.4, quat(x, -22)), (0.8, quat(x, -22)), (1.1, identity)]),
        rot("neck", [(0, identity), (0.4, quat(x, -18)), (0.8, quat(x, 10)), (1.1, identity)]),
        rot("jaw", [(0, identity), (0.5, quat(x, 30)), (0.9, quat(x, 30)), (1.1, identity)]),
        rot("wing_l", [(0, identity), (0.4, quat(z, -55)), (0.8, quat(z, -55)), (1.1, identity)]),
        rot("wing_r", [(0, identity), (0.4, quat(z, 55)), (0.8, quat(z, 55)), (1.1, identity)]),
        rot("wingtip_l", [(0, identity), (0.4, quat(z, -25)), (0.8, quat(z, -25)), (1.1, identity)]),
        rot("wingtip_r", [(0, identity), (0.4, quat(z, 25)), (0.8, quat(z, 25)), (1.1, identity)]),
    ]
    hit = [
        move("body", [(0, (0, 0, 0)), (0.1, (0, 0.05, -0.3)), (0.4, (0, 0, 0))]),
        rot("body", [(0, identity), (0.1, quat(x, -14)), (0.4, identity)]),
        rot("head", [(0, identity), (0.1, quat_mul(quat(y, 20), quat(x, -15))), (0.4, identity)]),
        ("root", "scale", [(0, (1, 1, 1)), (0.1, (1.06, 0.92, 1.06)), (0.4, (1, 1, 1))]),
    ]
    die = [
        move("body", [(0, (0, 0, 0)), (0.5, (0, -0.2, 0)), (1.2, (0, -0.55, 0))]),
        rot("body", [(0, identity), (0.5, quat(z, 12)), (1.2, quat(z, 70))]),
        rot("neck", [(0, identity), (1.2, quat(x, 50))]),
        rot("wing_l", [(0, identity), (1.2, quat(z, 60))]),
        rot("wing_r", [(0, identity), (1.2, quat(z, 30))]),
        rot("tail1", [(0, identity), (1.2, quat(y, 35))]),
        rot("leg_l", [(0, identity), (1.2, quat(x, -60))]),
        rot("leg_r", [(0, identity), (1.2, quat(x, -40))]),
    ]
    return {"idle": idle, "attack": attack, "cast": cast, "hit": hit, "die": die}


def write_creature(path, tint=(0.36, 0.5, 0.62)):
    mesh = build_mesh(tint)
    binary = bytearray()
    views, accessors = [], []

    def view(data, target=None, stride=None):
        while len(binary) % 4:
            binary.append(0)
        entry = {"buffer": 0, "byteOffset": len(binary), "byteLength": len(data)}
        if target:
            entry["target"] = target
        if stride:
            entry["byteStride"] = stride
        binary.extend(data)
        views.append(entry)
        return len(views) - 1

    def accessor(view_index, component, count, kind, offset=0, bounds=None):
        entry = {"bufferView": view_index, "byteOffset": offset, "componentType": component, "count": count, "type": kind}
        if bounds:
            entry["min"], entry["max"] = bounds
        accessors.append(entry)
        return len(accessors) - 1

    # Vertices: position, normal, uv, colour (12 floats), joints (4 x u16), weights (4 floats) = 72 bytes.
    data = bytearray()
    for position, normal, uv, colour, joints, weights in mesh.vertices:
        data += struct.pack("<12f", *position, *normal, *uv, *colour)
        data += struct.pack("<4H", *joints)
        data += struct.pack("<4f", *weights)
    count = len(mesh.vertices)
    vertex_view = view(data, 34962, 72)
    positions = [v[0] for v in mesh.vertices]
    bounds = ([min(p[i] for p in positions) for i in range(3)], [max(p[i] for p in positions) for i in range(3)])
    attributes = {
        "POSITION": accessor(vertex_view, 5126, count, "VEC3", 0, bounds),
        "NORMAL": accessor(vertex_view, 5126, count, "VEC3", 12),
        "TEXCOORD_0": accessor(vertex_view, 5126, count, "VEC2", 24),
        "COLOR_0": accessor(vertex_view, 5126, count, "VEC4", 32),
        "JOINTS_0": accessor(vertex_view, 5123, count, "VEC4", 48),
        "WEIGHTS_0": accessor(vertex_view, 5126, count, "VEC4", 56),
    }
    index_view = view(struct.pack(f"<{len(mesh.indices)}H", *mesh.indices), 34963)
    indices = accessor(index_view, 5123, len(mesh.indices), "SCALAR")

    # Inverse bind matrices: the rest pose only translates, so each is a translation back.
    inverse = bytearray()
    for name, _, _ in JOINTS:
        x, y, z = rest_position(name)
        inverse += struct.pack("<16f", 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, -x, -y, -z, 1)
    inverse_accessor = accessor(view(inverse), 5126, len(JOINTS), "MAT4")

    # Node 0 is the skinned mesh; joints follow in order.
    nodes = [{"name": "creature", "mesh": 0, "skin": 0}]
    for name, parent, translation in JOINTS:
        nodes.append({"name": name, "translation": list(translation)})
    for i, (name, parent, _) in enumerate(JOINTS):
        if parent is not None:
            nodes[INDEX[parent] + 1].setdefault("children", []).append(i + 1)

    animations = []
    for clip_name, channels in clips().items():
        samplers, targets = [], []
        for joint, path_name, keys in channels:
            times = struct.pack(f"<{len(keys)}f", *[k[0] for k in keys])
            width = 4 if path_name == "rotation" else 3
            values = b"".join(struct.pack(f"<{width}f", *k[1]) for k in keys)
            input_accessor = accessor(view(times), 5126, len(keys), "SCALAR",
                                      bounds=([keys[0][0]], [keys[-1][0]]))
            output_accessor = accessor(view(values), 5126, len(keys), "VEC4" if width == 4 else "VEC3")
            samplers.append({"input": input_accessor, "output": output_accessor, "interpolation": "LINEAR"})
            targets.append({"sampler": len(samplers) - 1, "target": {"node": INDEX[joint] + 1, "path": path_name}})
        animations.append({"name": clip_name, "samplers": samplers, "channels": targets})

    doc = {
        "asset": {"version": "2.0", "generator": "f-engine creature generator"},
        "scene": 0,
        "scenes": [{"nodes": [0, 1]}],
        "nodes": nodes,
        "meshes": [{"primitives": [{"attributes": attributes, "indices": indices, "material": 0}]}],
        "skins": [{"joints": [i + 1 for i in range(len(JOINTS))], "inverseBindMatrices": inverse_accessor,
                   "skeleton": 1}],
        "animations": animations,
        "materials": [{"pbrMetallicRoughness": {"baseColorFactor": [1, 1, 1, 1], "metallicFactor": 0}}],
        "buffers": [{"byteLength": 0}],
        "bufferViews": views,
        "accessors": accessors,
    }
    while len(binary) % 4:
        binary.append(0)
    doc["buffers"][0]["byteLength"] = len(binary)
    metadata = json.dumps(doc, separators=(",", ":")).encode()
    metadata += b" " * (-len(metadata) % 4)
    total = 12 + 8 + len(metadata) + 8 + len(binary)
    path = Path(path)
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(struct.pack("<III", 0x46546C67, 2, total) + struct.pack("<II", len(metadata), 0x4E4F534A) +
                     metadata + struct.pack("<II", len(binary), 0x004E4942) + bytes(binary))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("output", type=Path)
    parser.add_argument("--tint", default="0.36,0.5,0.62", help="hide colour as r,g,b in 0..1")
    args = parser.parse_args()
    write_creature(args.output, tuple(float(c) for c in args.tint.split(",")))
    print(f"wrote {args.output}")


if __name__ == "__main__":
    main()
