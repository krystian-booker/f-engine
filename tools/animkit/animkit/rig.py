"""A glTF file as the engine's animation runtime sees it: nodes with their
rest transforms, skins, skinned and rigid mesh parts, and animations whose
channels are sampled exactly as engine/assets/Model.cpp samples them.

    rig = load(path)                              # the file, through modelkit's glTF reader
    pose = sample(rig, clip, times)               # local transforms of every node at each time
    world = world_matrices(rig, pose)             # (times, nodes, 4, 4)
    points = skin(rig, world)                     # (times, vertices, 3): the mesh as the engine would draw it

What the engine does, and so what this does (Model::pose, sampleChannel,
skinVertices):
  - a channel holds its first value before its first key and its last value
    after its last key; a clip's length is the latest key of any channel, and
    the clock always starts at 0;
  - STEP holds the earlier key, LINEAR mixes, rotations are slerped the short
    way round and are NOT renormalised; CUBICSPLINE keys are read for their
    values only and played with linear interpolation;
  - a node an animation does not key keeps its rest transform; of two
    channels on one node and path the later one in the file wins;
  - world matrix = parent's world * T * R * S; a joint's skinning matrix is
    its world matrix times the skin's inverse bind matrix; a vertex is the
    weighted sum of up to four joints' matrices (weights normalised at load).
"""
import os
import sys

import numpy as np

sys.path.insert(0, os.path.join(os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__)))), "modelkit"))

from modelkit import gltf  # noqa: E402

PATHS = ("translation", "rotation", "scale")
WIDTH = {"translation": 3, "rotation": 4, "scale": 3}


class Channel:
    """One sampler bound to one node's translation, rotation or scale."""

    def __init__(self, node, path, interpolation, times, values):
        self.node, self.path, self.interpolation = node, path, interpolation
        self.times = times      # (keys,) seconds
        self.values = values    # (keys, 3 or 4): the key values (for CUBICSPLINE without the tangents)
        self.tangents = None    # CUBICSPLINE only: (in, out), each (keys, 3 or 4)

    @property
    def constant(self):
        return len(self.values) < 2 or bool(np.all(np.abs(self.values - self.values[0]) < 1e-7))

    @property
    def finite(self):
        """False when a key time or value is NaN or infinite: such a channel is reported and left out of the poses."""
        return bool(np.isfinite(self.times).all() and np.isfinite(self.values).all())


class Clip:
    """One animation: its channels in file order, and what could not be read."""

    def __init__(self, name, index):
        self.name, self.index = name, index
        self.channels = []      # the channels the engine plays
        self.problems = []      # (check id, severity, message, count): found while reading the samplers
        self.duration = 0.0     # the latest key of any channel: the length the engine reports
        self.start = 0.0        # the earliest key
        self.keys = 0           # keyframes over all channels
        self.bytes = 0          # bytes of sampler data (inputs counted once per accessor)


class Part:
    """A mesh primitive: skinned (placed by its joints) or rigid (placed by its node)."""

    def __init__(self, primitive, skin, offset):
        self.label, self.node, self.skin = primitive.label, primitive.node, skin
        self.positions, self.normals, self.indices = primitive.positions, primitive.normals, primitive.indices
        self.joints, self.weights = None, None
        self.offset = offset    # where its vertices start in the model's point list
        self.primitive = primitive


class Rig:
    def __init__(self, model):
        self.model = model
        self.names, self.parent, self.order = [], np.zeros(0, dtype=np.int64), []
        self.rest_t, self.rest_r, self.rest_s = (np.zeros((0, n)) for n in (3, 4, 3))
        self.skins = []         # {"joints": [node...], "inverse_bind": (joints, 4, 4)}
        self.parts = []
        self.clips = []
        self.vertices = 0
        self.faces = np.zeros((0, 3), dtype=np.int64)   # triangles over all parts' vertices

    def clip(self, name):
        for clip in self.clips:
            if clip.name == name:
                return clip
        return None

    def node_name(self, index):
        return self.names[index] if 0 <= index < len(self.names) else f"node{index}"


# ------------------------------------------------------------------ quaternions (x, y, z, w), arrays (..., 4)

def quat_normalise(q):
    length = np.linalg.norm(q, axis=-1, keepdims=True)
    return np.where(length > 1e-12, q / np.maximum(length, 1e-12), np.array([0.0, 0.0, 0.0, 1.0]))


def quat_matrix(q):
    """Rotation matrices (..., 3, 3) as glm::mat3_cast gives them: a quaternion
    that is not unit length yields a matrix that also scales and skews."""
    x, y, z, w = q[..., 0], q[..., 1], q[..., 2], q[..., 3]
    out = np.empty(q.shape[:-1] + (3, 3))
    out[..., 0, 0] = 1 - 2 * (y * y + z * z)
    out[..., 0, 1] = 2 * (x * y - z * w)
    out[..., 0, 2] = 2 * (x * z + y * w)
    out[..., 1, 0] = 2 * (x * y + z * w)
    out[..., 1, 1] = 1 - 2 * (x * x + z * z)
    out[..., 1, 2] = 2 * (y * z - x * w)
    out[..., 2, 0] = 2 * (x * z - y * w)
    out[..., 2, 1] = 2 * (y * z + x * w)
    out[..., 2, 2] = 1 - 2 * (x * x + y * y)
    return out


def slerp(a, b, t):
    """glm::slerp: the short way round (b is negated when the two are more
    than 90 degrees apart as 4-vectors), linear when they nearly coincide."""
    cosine = np.sum(a * b, axis=-1, keepdims=True)
    b = np.where(cosine < 0, -b, b)
    cosine = np.abs(cosine)
    t = t[..., None] if np.ndim(t) == np.ndim(cosine) - 1 else t
    linear = a + (b - a) * t
    angle = np.arccos(np.clip(cosine, -1.0, 1.0))
    sine = np.sin(angle)
    with np.errstate(invalid="ignore", divide="ignore"):
        curved = (np.sin((1 - t) * angle) * a + np.sin(t * angle) * b) / sine
    return np.where((cosine > 1 - 1e-6) | (sine < 1e-9), linear, curved)


def quat_angle(a, b):
    """Degrees between two rotations (0 to 180), whatever the signs of the quaternions."""
    dot = np.abs(np.sum(quat_normalise(a) * quat_normalise(b), axis=-1))
    return np.degrees(2 * np.arccos(np.clip(dot, 0.0, 1.0)))


# ------------------------------------------------------------------ loading

def _decompose(matrix):
    """A node matrix as the engine stores it: translation, rotation, scale; a
    mirrored matrix keeps its mirroring as a negative x scale."""
    c = [matrix[:3, i] for i in range(3)]
    scale = np.array([np.linalg.norm(v) for v in c])
    if np.dot(c[0], np.cross(c[1], c[2])) < 0:
        scale[0] = -scale[0]
    with np.errstate(invalid="ignore", divide="ignore"):
        r = np.nan_to_num(np.stack([c[i] / scale[i] for i in range(3)], axis=1))
    trace = r[0, 0] + r[1, 1] + r[2, 2]
    if trace > 0:
        s = np.sqrt(trace + 1.0) * 2
        q = [(r[2, 1] - r[1, 2]) / s, (r[0, 2] - r[2, 0]) / s, (r[1, 0] - r[0, 1]) / s, 0.25 * s]
    elif r[0, 0] > r[1, 1] and r[0, 0] > r[2, 2]:
        s = np.sqrt(1.0 + r[0, 0] - r[1, 1] - r[2, 2]) * 2
        q = [0.25 * s, (r[0, 1] + r[1, 0]) / s, (r[0, 2] + r[2, 0]) / s, (r[2, 1] - r[1, 2]) / s]
    elif r[1, 1] > r[2, 2]:
        s = np.sqrt(1.0 + r[1, 1] - r[0, 0] - r[2, 2]) * 2
        q = [(r[0, 1] + r[1, 0]) / s, 0.25 * s, (r[1, 2] + r[2, 1]) / s, (r[0, 2] - r[2, 0]) / s]
    else:
        s = np.sqrt(1.0 + r[2, 2] - r[0, 0] - r[1, 1]) * 2
        q = [(r[0, 2] + r[2, 0]) / s, (r[1, 2] + r[2, 1]) / s, 0.25 * s, (r[1, 0] - r[0, 1]) / s]
    return matrix[:3, 3].copy(), np.array(q), scale


def load(path):
    """Read a .glb/.gltf into a Rig. Raises gltf.GltfError (or ValueError,
    KeyError, IndexError) when the file's structure cannot be read at all;
    what is wrong with one animation's samplers is kept on its Clip."""
    return from_model(gltf.load(path))


def from_model(model):
    rig = Rig(model)
    document, nodes = model.json, model.nodes
    count = len(nodes)
    rig.names = [node.get("name") or f"node{index}" for index, node in enumerate(nodes)]
    rig.parent = np.full(count, -1, dtype=np.int64)
    for index, node in enumerate(nodes):
        for child in node.get("children", []):
            if 0 <= child < count:
                rig.parent[child] = index
    rig.rest_t, rig.rest_r, rig.rest_s = np.zeros((count, 3)), np.tile([0.0, 0, 0, 1], (count, 1)), np.ones((count, 3))
    for index, node in enumerate(nodes):
        if "matrix" in node:
            rig.rest_t[index], rig.rest_r[index], rig.rest_s[index] = _decompose(
                np.array(node["matrix"], dtype=np.float64).reshape(4, 4).T)
        else:
            rig.rest_t[index] = node.get("translation", [0, 0, 0])
            rig.rest_r[index] = node.get("rotation", [0, 0, 0, 1])
            rig.rest_s[index] = node.get("scale", [1, 1, 1])
    # Parents before children, over the nodes of the file's scene (the engine composes only those).
    scenes = document.get("scenes", [])
    roots = scenes[document.get("scene", 0)].get("nodes", []) if scenes else [i for i in range(count) if rig.parent[i] < 0]
    stack, seen = list(reversed(roots)), set()
    while stack:
        index = stack.pop()
        if index in seen or not 0 <= index < count:
            continue
        seen.add(index)
        rig.order.append(index)
        stack.extend(reversed(nodes[index].get("children", [])))

    for skin in model.skins:
        joints = [int(j) for j in skin.get("joints", [])]
        if "inverseBindMatrices" in skin:
            inverse = model.accessor(skin["inverseBindMatrices"]).astype(np.float64).reshape(-1, 4, 4).transpose(0, 2, 1)
            if len(inverse) < len(joints):
                raise ValueError("a skin has fewer inverse bind matrices than joints")
            inverse = inverse[:len(joints)]
        else:
            inverse = np.tile(np.eye(4), (len(joints), 1, 1))
        rig.skins.append({"joints": joints, "inverse_bind": inverse, "name": skin.get("name") or ""})

    offset, faces = 0, []
    for primitive in model.primitives:
        if primitive.positions is None:
            continue
        skin = nodes[primitive.node].get("skin", -1) if primitive.skinned else -1
        part = Part(primitive, -1, offset)
        if 0 <= skin < len(rig.skins) and primitive.joints is not None and primitive.weights is not None:
            weights = np.where(np.isfinite(primitive.weights), np.maximum(primitive.weights, 0.0), 0.0)
            total = weights.sum(axis=1, keepdims=True)
            # As the engine: weights sum to 1; a vertex bound to nothing follows its first joint.
            part.weights = np.where(total > 1e-6, weights / np.maximum(total, 1e-6), np.array([1.0, 0, 0, 0]))
            part.joints = np.clip(primitive.joints, 0, len(rig.skins[skin]["joints"]) - 1)
            part.skin = skin
        rig.parts.append(part)
        faces.append(primitive.indices + offset)
        offset += len(primitive.positions)
    rig.vertices = offset
    rig.faces = np.concatenate(faces) if faces else np.zeros((0, 3), dtype=np.int64)

    accessors = document.get("accessors", [])
    for index, animation in enumerate(document.get("animations", [])):
        rig.clips.append(_read_clip(model, accessors, count, index, animation))
    return rig


def _read_clip(model, accessors, node_count, index, animation):
    clip = Clip(animation.get("name") or "", index)
    samplers = animation.get("samplers", [])
    counted = set()
    seen = {}

    def problem(check, severity, message, count=None):
        clip.problems.append((check, severity, message, count))

    for number, channel in enumerate(animation.get("channels", [])):
        target = channel.get("target", {})
        node, path = target.get("node"), target.get("path")
        where = f"channel {number}"
        if node is None or not 0 <= node < node_count:
            problem("keys.target", "warn", f"{where} targets no node of the file; the engine skips it")
            continue
        where = f"{model.nodes[node].get('name') or 'node' + str(node)}.{path}"
        if path == "weights":
            problem("keys.target", "warn", f"{where} animates morph weights, which the engine does not play")
            continue
        if path not in PATHS:
            problem("keys.target", "error", f"{where}: '{path}' is not translation, rotation or scale")
            continue
        if not 0 <= channel.get("sampler", -1) < len(samplers):
            problem("keys.sampler", "error", f"{where} names a sampler the animation does not have")
            continue
        sampler = samplers[channel["sampler"]]
        interpolation = sampler.get("interpolation", "LINEAR")
        try:
            source_in, source_out = accessors[sampler["input"]], accessors[sampler["output"]]
            times = model.accessor(sampler["input"]).astype(np.float64).reshape(-1)
            output = model.accessor(sampler["output"]).astype(np.float64)
        except (KeyError, IndexError, ValueError, TypeError, gltf.GltfError) as error:
            problem("keys.sampler", "error", f"{where}: its sampler cannot be read ({error})")
            continue
        for accessor_index, accessor in ((sampler["input"], source_in), (sampler["output"], source_out)):
            if accessor_index not in counted:
                counted.add(accessor_index)
                clip.bytes += accessor["count"] * gltf.WIDTH[accessor["type"]] * np.dtype(gltf.COMPONENT[accessor["componentType"]]).itemsize
        if source_in.get("type") != "SCALAR" or output.shape[1] != WIDTH[path]:
            problem("keys.sampler", "error",
                    f"{where}: its sampler is {source_in.get('type')} times with {source_out.get('type')} values; "
                    f"{path} needs SCALAR times and VEC{WIDTH[path]} values")
            continue
        per_key = 3 if interpolation == "CUBICSPLINE" else 1
        if len(output) < len(times) * per_key:
            problem("keys.sampler", "error", f"{where}: {len(times)} key times but {len(output)} values"
                                             + (" (cubic splines need three per key)" if per_key == 3 else ""), len(times))
            continue
        if len(output) > len(times) * per_key:
            problem("keys.sampler", "warn", f"{where}: {len(output) - len(times) * per_key} more values than key times", len(output))
            output = output[:len(times) * per_key]
        if not len(times):
            problem("keys.sampler", "warn", f"{where} has no keys; the engine skips it")
            continue
        if interpolation not in ("LINEAR", "STEP", "CUBICSPLINE"):
            problem("keys.interpolation", "warn", f"{where}: unknown interpolation '{interpolation}'; the engine plays it as LINEAR")
        if interpolation == "CUBICSPLINE":
            out = Channel(node, path, interpolation, times, output[1::3].copy())
            out.tangents = (output[0::3].copy(), output[2::3].copy())
        else:
            out = Channel(node, path, interpolation, times, output)
        if (node, path) in seen:
            problem("keys.duplicate_channels", "error",
                    f"{where} is keyed by two channels; the engine plays only the later one")
        seen[(node, path)] = True
        clip.channels.append(out)
        clip.keys += len(times)
    finite = [channel for channel in clip.channels if len(channel.times) and channel.finite]
    if finite:
        clip.duration = float(max(channel.times[-1] for channel in finite))
        clip.start = float(min(channel.times[0] for channel in finite))
    return clip


# ------------------------------------------------------------------ evaluation

def sample_channel(channel, times, spline=False):
    """The channel's value at each time, as the engine's sampleChannel gives
    it. `spline` evaluates a CUBICSPLINE channel as authored instead (what the
    engine does not do), to measure the difference."""
    keys, values = channel.times, channel.values
    times = np.asarray(times, dtype=np.float64)
    if len(keys) == 1:
        return np.repeat(values[:1], len(times), axis=0)
    upper = np.clip(np.searchsorted(keys, times, side="right"), 1, len(keys) - 1)
    a, b = values[upper - 1], values[upper]
    span = keys[upper] - keys[upper - 1]
    t = np.clip((times - keys[upper - 1]) / np.maximum(1e-6, span), 0.0, 1.0)[:, None]
    if spline and channel.tangents is not None:
        d = span[:, None]
        out_tangent, in_tangent = channel.tangents[1][upper - 1], channel.tangents[0][upper]
        t2, t3 = t * t, t * t * t
        value = (2 * t3 - 3 * t2 + 1) * a + (t3 - 2 * t2 + t) * d * out_tangent + (-2 * t3 + 3 * t2) * b + (t3 - t2) * d * in_tangent
        if channel.path == "rotation":
            value = quat_normalise(value)
    elif channel.interpolation == "STEP":
        value = a.copy()
    elif channel.path == "rotation":
        value = slerp(a, b, t)
    else:
        value = a + (b - a) * t
    value[times <= keys[0]] = values[0]
    value[times >= keys[-1]] = values[-1]
    return value


def sample(rig, clip, times, spline=False):
    """Local transforms of every node at each time: (t, r, s) with shapes
    (times, nodes, 3), (times, nodes, 4), (times, nodes, 3). `clip` None gives the rest pose."""
    times = np.asarray(times, dtype=np.float64).reshape(-1)
    t = np.repeat(rig.rest_t[None], len(times), axis=0)
    r = np.repeat(rig.rest_r[None], len(times), axis=0)
    s = np.repeat(rig.rest_s[None], len(times), axis=0)
    for channel in (clip.channels if clip is not None else []):
        if not channel.finite:
            continue
        value = sample_channel(channel, times, spline)
        (t if channel.path == "translation" else r if channel.path == "rotation" else s)[:, channel.node] = value
    return t, r, s


def blend(pose_a, pose_b, weight):
    """Model::blendPoses: move pose_a towards pose_b by `weight`; rotations the short way and renormalised."""
    (ta, ra, sa), (tb, rb, sb) = pose_a, pose_b
    weight = np.clip(weight, 0.0, 1.0)
    return ta + (tb - ta) * weight, quat_normalise(slerp(ra, rb, np.full(ra.shape[:-1], weight))), sa + (sb - sa) * weight


def local_matrices(pose):
    t, r, s = pose
    out = np.zeros(t.shape[:-1] + (4, 4))
    out[..., :3, :3] = quat_matrix(r) * s[..., None, :]
    out[..., :3, 3] = t
    out[..., 3, 3] = 1.0
    return out


def world_matrices(rig, pose):
    """World (model space) matrices of every node at each time: (times, nodes, 4, 4)."""
    world = local_matrices(pose)
    for index in rig.order:
        parent = rig.parent[index]
        if parent >= 0:
            world[:, index] = world[:, parent] @ world[:, index]
    return world


def skin(rig, world, normals=False, chunk=24):
    """Every vertex of the model in model space at each time: (times, vertices, 3)
    in the order of rig.parts (rig.faces indexes it). With `normals`, also the
    vertex normals the engine would shade with."""
    count = len(world)
    points = np.zeros((count, rig.vertices, 3))
    shading = np.zeros((count, rig.vertices, 3)) if normals else None
    for part in rig.parts:
        size = len(part.positions)
        span = slice(part.offset, part.offset + size)
        rest = np.c_[part.positions, np.ones(size)]
        if part.skin < 0:
            matrix = world[:, part.node]
            points[:, span] = np.einsum("tij,vj->tvi", matrix[:, :3, :], rest)
            if normals and part.normals is not None:
                shading[:, span] = np.einsum("tij,vj->tvi", matrix[:, :3, :3], part.normals)
            continue
        entry = rig.skins[part.skin]
        for start in range(0, count, chunk):
            joints = world[start:start + chunk, entry["joints"]] @ entry["inverse_bind"]      # (chunk, joints, 4, 4)
            for k in range(4):
                weight = part.weights[:, k]
                used = weight > 0 if k else np.ones(size, dtype=bool)   # the engine always adds the first influence
                if not used.any():
                    continue
                matrices = joints[:, part.joints[used, k]]                                   # (chunk, used, 4, 4)
                points[start:start + chunk, part.offset + np.nonzero(used)[0]] += \
                    np.einsum("tvij,vj->tvi", matrices[:, :, :3, :], rest[used]) * weight[used][None, :, None]
                if normals and part.normals is not None:
                    shading[start:start + chunk, part.offset + np.nonzero(used)[0]] += \
                        np.einsum("tvij,vj->tvi", matrices[:, :, :3, :3], part.normals[used]) * weight[used][None, :, None]
    if normals:
        with np.errstate(invalid="ignore", divide="ignore"):
            shading = np.nan_to_num(shading / np.linalg.norm(shading, axis=2, keepdims=True))
        return points, shading
    return points


def frame_times(clip, fps):
    """The times the engine shows when it plays the clip at `fps` ticks a second: 0, 1/fps, ... and the last key."""
    count = max(1, int(round(clip.duration * fps)))
    times = np.arange(count + 1) / fps
    times[-1] = clip.duration
    return times if times[-1] > times[-2] or count == 1 else times[:-1]


def joint_dominance(rig):
    """For each node: the indices (into the model's point list) of the vertices it carries most of."""
    owner = np.full(rig.vertices, -1, dtype=np.int64)
    for part in rig.parts:
        span = slice(part.offset, part.offset + len(part.positions))
        if part.skin < 0:
            owner[span] = part.node
        else:
            joints = np.asarray(rig.skins[part.skin]["joints"])
            owner[span] = joints[part.joints[np.arange(len(part.weights)), part.weights.argmax(axis=1)]]
    return owner
