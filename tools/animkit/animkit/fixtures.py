"""Small animated models for the self-test (and for trying a check by hand):
a figure of boxes on a skeleton, a prop of rigid nodes, clips baked from
Python functions of time, and a writer that puts them in a .glb.

    figure = fixtures.figure()
    clips = {"idle": fixtures.bake(figure, fixtures.idle, 2.0), "attack": fixtures.bake(figure, fixtures.attack, 1.0)}
    fixtures.write_glb("figure.glb", figure, clips)

A clip is a list of channels: {"joint", "path", "times", "values",
"interpolation"}; the tests break one by editing those arrays.
"""
import json
import math
import struct

import numpy as np

# name, parent, position at rest (model space). The figure stands on y = 0, faces +Z, arms out along X.
JOINTS = [
    ("root", None, (0, 0, 0)),
    ("hips", "root", (0, 1.0, 0)),
    ("chest", "hips", (0, 1.5, 0)),
    ("head", "chest", (0, 1.9, 0)),
    ("arm_l", "chest", (0.35, 1.7, 0)),
    ("hand_l", "arm_l", (0.9, 1.7, 0)),
    ("arm_r", "chest", (-0.35, 1.7, 0)),
    ("hand_r", "arm_r", (-0.9, 1.7, 0)),
    ("leg_l", "hips", (0.15, 1.0, 0)),
    ("foot_l", "leg_l", (0.15, 0.1, 0)),
    ("leg_r", "hips", (-0.15, 1.0, 0)),
    ("foot_r", "leg_r", (-0.15, 0.1, 0)),
]
# joint, centre, size: each box follows one joint rigidly
BOXES = [
    ("hips", (0, 1.1, 0), (0.5, 0.3, 0.3)), ("chest", (0, 1.6, 0), (0.6, 0.5, 0.35)), ("head", (0, 2.0, 0), (0.3, 0.3, 0.3)),
    ("arm_r", (-0.6, 1.7, 0), (0.5, 0.15, 0.15)), ("hand_r", (-1.05, 1.7, 0), (0.3, 0.18, 0.18)),
    ("hand_l", (1.05, 1.7, 0), (0.3, 0.18, 0.18)),
    ("leg_l", (0.15, 0.55, 0), (0.18, 0.9, 0.18)), ("foot_l", (0.15, 0.05, 0.05), (0.2, 0.1, 0.35)),
    ("leg_r", (-0.15, 0.55, 0), (0.18, 0.9, 0.18)), ("foot_r", (-0.15, 0.05, 0.05), (0.2, 0.1, 0.35)),
]


def quat(axis, degrees):
    """Unit quaternion (x, y, z, w) turning `degrees` about `axis`."""
    half = math.radians(degrees) / 2
    axis = np.asarray(axis, dtype=np.float64)
    axis = axis / (np.linalg.norm(axis) or 1.0)
    return np.r_[axis * math.sin(half), math.cos(half)]


def quat_mul(a, b):
    ax, ay, az, aw = a
    bx, by, bz, bw = b
    return np.array([aw * bx + ax * bw + ay * bz - az * by, aw * by - ax * bz + ay * bw + az * bx,
                     aw * bz + ax * by - ay * bx + az * bw, aw * bw - ax * bx - ay * by - az * bz])


def smooth(u):
    u = min(max(u, 0.0), 1.0)
    return u * u * (3 - 2 * u)


def _box(centre, size):
    """24 vertices and 12 triangles with outward normals."""
    positions, normals, indices = [], [], []
    half = np.asarray(size, dtype=np.float64) / 2
    for normal, u, v in (((1, 0, 0), (0, 1, 0), (0, 0, 1)), ((-1, 0, 0), (0, 0, 1), (0, 1, 0)), ((0, 1, 0), (0, 0, 1), (1, 0, 0)),
                         ((0, -1, 0), (1, 0, 0), (0, 0, 1)), ((0, 0, 1), (1, 0, 0), (0, 1, 0)), ((0, 0, -1), (0, 1, 0), (1, 0, 0))):
        n, u, v = (np.array(a, dtype=np.float64) for a in (normal, u, v))
        base = len(positions)
        for a, b in ((-1, -1), (1, -1), (1, 1), (-1, 1)):
            positions.append((n + a * u + b * v) * half + centre)
            normals.append(n)
        indices += [[base, base + 1, base + 2], [base, base + 2, base + 3]]
    return np.array(positions), np.array(normals), np.array(indices)


def figure(tube=True):
    """The test figure: {"joints", "positions", "normals", "indices", "joint_index", "weights"}.
    With `tube`, its left arm is a tube whose rings blend from the shoulder joint to the hand joint."""
    names = [name for name, _, _ in JOINTS]
    positions, normals, indices, joints, weights = [], [], [], [], []
    offset = 0
    for joint, centre, size in BOXES:
        p, n, i = _box(centre, size)
        positions.append(p)
        normals.append(n)
        indices.append(i + offset)
        joints.append(np.tile([names.index(joint), 0, 0, 0], (len(p), 1)))
        weights.append(np.tile([1.0, 0, 0, 0], (len(p), 1)))
        offset += len(p)
    if tube:
        rings, around, radius = 7, 8, 0.09
        ring_positions, ring_normals, ring_joints, ring_weights = [], [], [], []
        for r in range(rings):
            along = r / (rings - 1)
            for a in range(around):
                angle = 2 * math.pi * a / around
                direction = np.array([0.0, math.cos(angle), math.sin(angle)])
                ring_positions.append(np.array([0.35 + 0.55 * along, 1.7, 0.0]) + direction * radius)
                ring_normals.append(direction)
                ring_joints.append([names.index("arm_l"), names.index("hand_l"), 0, 0])
                ring_weights.append([1 - along, along, 0, 0])
        faces = []
        for r in range(rings - 1):
            for a in range(around):
                a0, a1 = r * around + a, r * around + (a + 1) % around
                b0, b1 = a0 + around, a1 + around
                faces += [[a0, b0, a1], [a1, b0, b1]]
        positions.append(np.array(ring_positions))
        normals.append(np.array(ring_normals))
        indices.append(np.array(faces) + offset)
        joints.append(np.array(ring_joints))
        weights.append(np.array(ring_weights))
    return {"joints": list(JOINTS), "positions": np.concatenate(positions), "normals": np.concatenate(normals),
            "indices": np.concatenate(indices), "joint_index": np.concatenate(joints), "weights": np.concatenate(weights)}


def prop():
    """A rigid prop: a base and a wheel on its own node (no skin). {"joints", "boxes"}."""
    return {"joints": [("base", None, (0, 0, 0)), ("wheel", "base", (0, 1.0, 0))],
            "boxes": [("base", (0, 0.4, 0), (0.6, 0.8, 0.6)), ("wheel", (0, 1.0, 0.35), (1.2, 1.2, 0.1))]}


# ------------------------------------------------------------------ clips as functions of time

def idle(t, length=2.0):
    """A breathing sway that comes round exactly: {joint: {"rotation": q, "translation": offset}}."""
    s = math.sin(2 * math.pi * t / length)
    c = 0.5 - 0.5 * math.cos(2 * math.pi * t / length)
    return {"chest": {"rotation": quat((0, 0, 1), 3 * s)}, "head": {"rotation": quat((0, 1, 0), 6 * s)},
            "arm_l": {"rotation": quat((0, 0, 1), -8 - 5 * c)}, "arm_r": {"rotation": quat((0, 0, 1), 8 + 5 * c)},
            "hand_l": {"rotation": quat((0, 0, 1), -6 * c)}, "hand_r": {"rotation": quat((0, 0, 1), 6 * c)},
            "hips": {"translation": (0.0, 0.0, 0.0)}}


def attack(t, length=1.0):
    """From the idle pose: the left arm is drawn back, swung forward to a dead stop at 0.5 s, held, and returned."""
    start = idle(0.0)
    swing = min(max((t - 0.3) / 0.2, 0.0), 1.0) ** 2                      # accelerates into the blow: an impact, not a pop
    settle = smooth((t - 0.6) / 0.4)
    angle = 40 * smooth(t / 0.3) if t < 0.3 else 40 - 130 * swing if t < 0.6 else -90 * (1 - settle)
    pose = dict(start)
    pose["arm_l"] = {"rotation": quat_mul(start["arm_l"]["rotation"], quat((0, 1, 0), angle))}
    pose["chest"] = {"rotation": quat((0, 1, 0), angle * 0.15)}
    return pose


def bake(model, function, seconds, fps=30, length=None):
    """Sample a pose function at `fps`: a clip (list of channels) with one LINEAR key per frame on every joint it names."""
    count = int(round(seconds * fps))
    times = np.arange(count + 1) / fps
    rest = {name: np.array(position, dtype=np.float64) for name, _, position in model["joints"]}
    parent = {name: parent for name, parent, _ in model["joints"]}
    tracks = {}
    for t in times:
        pose = function(float(t)) if length is None else function(float(t), length)
        for joint, values in pose.items():
            for path, value in values.items():
                if path == "translation":   # an offset from the joint's rest translation
                    value = np.asarray(value, dtype=np.float64) + rest[joint] - (rest[parent[joint]] if parent[joint] else 0)
                tracks.setdefault((joint, path), []).append(np.asarray(value, dtype=np.float64))
    return [{"joint": joint, "path": path, "times": times.copy(), "values": np.array(values), "interpolation": "LINEAR"}
            for (joint, path), values in tracks.items()]


def channel(clip, joint, path):
    for entry in clip:
        if entry["joint"] == joint and entry["path"] == path:
            return entry
    raise KeyError(f"{joint}.{path}")


# ------------------------------------------------------------------ writing

def write_glb(path, model, clips, extra=None, skinned_parts=1, mutate=None):
    """Write the model and its clips ({name: channels}, or a list of (name, channels) to allow duplicate
    names) as a .glb. `mutate(document)` may edit the glTF JSON before it is written."""
    blob = bytearray()
    views, accessors = [], []

    def add(array, dtype, kind, bounds=False):
        data = np.ascontiguousarray(array, dtype=dtype)
        while len(blob) % 4:
            blob.append(0)
        views.append({"buffer": 0, "byteOffset": len(blob), "byteLength": data.nbytes})
        blob.extend(data.tobytes())
        component = {np.dtype(np.float32): 5126, np.dtype(np.uint32): 5125, np.dtype(np.uint16): 5123}[np.dtype(dtype)]
        accessor = {"bufferView": len(views) - 1, "componentType": component, "count": int(len(data)), "type": kind}
        if bounds:
            flat = data.reshape(len(data), -1) if len(data) else np.zeros((1, 1))
            accessor["min"] = [float(v) for v in np.nan_to_num(flat).min(axis=0)]
            accessor["max"] = [float(v) for v in np.nan_to_num(flat).max(axis=0)]
        accessors.append(accessor)
        return len(accessors) - 1

    joints = model["joints"]
    names = [name for name, _, _ in joints]
    rest = {name: np.array(position, dtype=np.float64) for name, _, position in joints}
    nodes = []
    for name, parent, position in joints:
        local = rest[name] - (rest[parent] if parent else 0)
        nodes.append({"name": name, "translation": [float(v) for v in local]})
    for index, (name, parent, _) in enumerate(joints):
        if parent:
            nodes[names.index(parent)].setdefault("children", []).append(index)
    roots = [index for index, (_, parent, _) in enumerate(joints) if not parent]
    document = {"asset": {"version": "2.0", "generator": "animkit"}, "scene": 0, "materials": [
        {"name": "clay", "pbrMetallicRoughness": {"baseColorFactor": [0.6, 0.62, 0.7, 1]}}]}
    meshes, skins = [], []
    if "boxes" in model:        # a rigid prop: one mesh per node
        for joint, centre, size in model["boxes"]:
            p, n, i = _box(np.array(centre) - rest[joint], size)
            meshes.append({"name": joint, "primitives": [{"attributes": {"POSITION": add(p, np.float32, "VEC3", True),
                                                                         "NORMAL": add(n, np.float32, "VEC3")},
                                                          "indices": add(i.reshape(-1), np.uint32, "SCALAR"), "material": 0}]})
            nodes[names.index(joint)]["mesh"] = len(meshes) - 1
    else:
        inverse = np.tile(np.eye(4, dtype=np.float32), (len(joints), 1, 1))
        for index, name in enumerate(names):
            inverse[index, 3, :3] = -rest[name]     # column-major: the translation is the fourth column
        skins.append({"name": "skeleton", "joints": list(range(len(joints))),
                      "inverseBindMatrices": add(inverse.reshape(-1, 16), np.float32, "MAT4")})
        faces = model["indices"]
        chunks = np.array_split(np.arange(len(faces)), skinned_parts)
        primitives = []
        for chunk in chunks:    # several primitives sharing nothing, to count as that many skinned parts
            used = np.unique(faces[chunk])
            remap = np.full(len(model["positions"]), -1)
            remap[used] = np.arange(len(used))
            primitives.append({"attributes": {
                "POSITION": add(model["positions"][used], np.float32, "VEC3", True),
                "NORMAL": add(model["normals"][used], np.float32, "VEC3"),
                "JOINTS_0": add(model["joint_index"][used], np.uint16, "VEC4"),
                "WEIGHTS_0": add(model["weights"][used], np.float32, "VEC4")},
                "indices": add(remap[faces[chunk]].reshape(-1), np.uint32, "SCALAR"), "material": 0})
        meshes.append({"name": "figure", "primitives": primitives})
        nodes.append({"name": "figure_mesh", "mesh": 0, "skin": 0})
        roots.append(len(nodes) - 1)
    document.update({"scenes": [{"nodes": roots}], "nodes": nodes, "meshes": meshes})
    if skins:
        document["skins"] = skins
    animations = []
    for name, channels in (clips.items() if isinstance(clips, dict) else clips):
        samplers, targets = [], []
        for entry in channels:
            values = np.asarray(entry["values"], dtype=np.float64)
            kind = entry.get("type") or ("VEC4" if values.shape[-1] == 4 else "VEC3")
            samplers.append({"input": add(np.asarray(entry["times"], dtype=np.float64), np.float32, "SCALAR", True),
                             "output": add(values, np.float32, kind), "interpolation": entry.get("interpolation", "LINEAR")})
            target = {"path": entry["path"]}
            if entry.get("joint") is not None:
                target["node"] = names.index(entry["joint"]) if isinstance(entry["joint"], str) else int(entry["joint"])
            targets.append({"sampler": len(samplers) - 1, "target": target})
        animation = {"samplers": samplers, "channels": targets}
        if name:
            animation["name"] = name
        animations.append(animation)
    if animations:
        document["animations"] = animations
    document["accessors"], document["bufferViews"] = accessors, views
    while len(blob) % 4:
        blob.append(0)
    document["buffers"] = [{"byteLength": len(blob)}]
    document.update(extra or {})
    if mutate:
        mutate(document)
    text = json.dumps(document).encode("utf-8")
    text += b" " * (-len(text) % 4)
    with open(path, "wb") as out:
        out.write(b"glTF" + struct.pack("<II", 2, 12 + 8 + len(text) + 8 + len(blob)))
        out.write(struct.pack("<II", len(text), 0x4E4F534A) + text)
        out.write(struct.pack("<II", len(blob), 0x004E4942) + bytes(blob))
    return path
