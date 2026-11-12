"""Read (and, for tests, write) glTF 2.0 files into numpy arrays.

Only what the checks need: the scene's nodes with their world matrices, each
mesh primitive's attributes and indices, materials, images (sizes only),
skins and animation names. No third-party glTF library, so the validator runs
anywhere Python and numpy do.
"""
import base64
import json
import os
import struct

import numpy as np

COMPONENT = {5120: np.int8, 5121: np.uint8, 5122: np.int16, 5123: np.uint16, 5125: np.uint32, 5126: np.float32}
WIDTH = {"SCALAR": 1, "VEC2": 2, "VEC3": 3, "VEC4": 4, "MAT2": 4, "MAT3": 9, "MAT4": 16}


class GltfError(Exception):
    """The file is not something a glTF loader can read."""


class Primitive:
    def __init__(self):
        self.mesh_name = ""
        self.node_name = ""
        self.node = -1
        self.material = -1
        self.mode = 4
        self.positions = None  # (n, 3) float, in mesh space
        self.world = np.eye(4)  # node's world matrix
        self.normals = None
        self.tangents = None
        self.uvs = None
        self.colors = None
        self.joints = None
        self.weights = None
        self.extra_uv_sets = 0
        self.extra_joint_sets = 0
        self.indices = None  # (m, 3) int
        self.indexed = True
        self.skinned = False
        self.targets = 0

    @property
    def world_positions(self):
        p = np.c_[self.positions, np.ones(len(self.positions))]
        return (p @ self.world.T)[:, :3]

    @property
    def label(self):
        return f"{self.node_name or self.mesh_name or 'mesh'}[{self.material_name}]"


class Model:
    def __init__(self, path):
        self.path = path
        self.json = {}
        self.primitives = []
        self.materials = []  # dicts from the file
        self.images = []  # {"name", "width", "height", "bytes", "mime", "uri"}
        self.textures = []
        self.skins = []
        self.animations = []  # {"name", "seconds", "channels"}
        self.nodes = []
        self.scene_nodes = set()
        self.problems = []  # structural problems found while reading: (severity, message)
        self.file_bytes = 0

    def material_name(self, index):
        if index is None or index < 0 or index >= len(self.materials):
            return "(no material)"
        return self.materials[index].get("name") or f"material{index}"


def _image_size(data):
    """(width, height, mime) of PNG or JPEG bytes; (0, 0, None) when unknown."""
    if data[:8] == b"\x89PNG\r\n\x1a\n" and len(data) >= 24:
        w, h = struct.unpack(">II", data[16:24])
        return w, h, "image/png"
    if data[:2] == b"\xff\xd8":
        i = 2
        while i + 9 < len(data):
            if data[i] != 0xFF:
                i += 1
                continue
            marker = data[i + 1]
            if marker in (0xC0, 0xC1, 0xC2):
                h, w = struct.unpack(">HH", data[i + 5:i + 9])
                return w, h, "image/jpeg"
            i += 2 + struct.unpack(">H", data[i + 2:i + 4])[0]
        return 0, 0, "image/jpeg"
    return 0, 0, None


def load(path):
    model = Model(path)
    with open(path, "rb") as file:
        raw = file.read()
    model.file_bytes = len(raw)
    folder = os.path.dirname(os.path.abspath(path))
    binary = None
    if raw[:4] == b"glTF":
        if len(raw) < 20:
            raise GltfError("truncated GLB header")
        version, length = struct.unpack("<II", raw[4:12])
        if version != 2:
            raise GltfError(f"GLB version {version}; the engine reads glTF 2.0")
        offset = 12
        document = None
        while offset + 8 <= len(raw):
            size, kind = struct.unpack("<II", raw[offset:offset + 8])
            chunk = raw[offset + 8:offset + 8 + size]
            if kind == 0x4E4F534A:
                document = json.loads(chunk.decode("utf-8"))
            elif kind == 0x004E4942 and binary is None:
                binary = chunk
            offset += 8 + size + (-size % 4)
        if document is None:
            raise GltfError("GLB without a JSON chunk")
    else:
        try:
            document = json.loads(raw.decode("utf-8"))
        except (UnicodeDecodeError, json.JSONDecodeError) as error:
            raise GltfError(f"not a glTF file: {error}") from error
    model.json = document
    if str(document.get("asset", {}).get("version", "")) != "2.0":
        raise GltfError("asset.version is not 2.0")

    buffers = []
    for index, buffer in enumerate(document.get("buffers", [])):
        uri = buffer.get("uri")
        if uri is None:
            if binary is None:
                raise GltfError(f"buffer {index} has no uri and the file has no binary chunk")
            data = binary
        elif uri.startswith("data:"):
            data = base64.b64decode(uri.split(",", 1)[1])
        else:
            target = os.path.join(folder, uri)
            if not os.path.isfile(target):
                raise GltfError(f"buffer file '{uri}' is missing")
            data = open(target, "rb").read()
        if len(data) < buffer.get("byteLength", 0):
            raise GltfError(f"buffer {index} is shorter than its byteLength")
        buffers.append(data)

    views = document.get("bufferViews", [])
    accessors = document.get("accessors", [])

    def view_bytes(index):
        view = views[index]
        data = buffers[view["buffer"]]
        start = view.get("byteOffset", 0)
        return data[start:start + view["byteLength"]], view.get("byteStride", 0)

    def read(index):
        accessor = accessors[index]
        if "sparse" in accessor:
            model.problems.append(("error", f"accessor {index} is sparse; export without sparse accessors"))
        dtype = np.dtype(COMPONENT[accessor["componentType"]]).newbyteorder("<")
        width = WIDTH[accessor["type"]]
        count = accessor["count"]
        if "bufferView" not in accessor:
            return np.zeros((count, width), dtype=dtype)
        data, stride = view_bytes(accessor["bufferView"])
        start = accessor.get("byteOffset", 0)
        item = dtype.itemsize * width
        if stride and stride != item:
            rows = np.lib.stride_tricks.as_strided(
                np.frombuffer(data, dtype=np.uint8, offset=start, count=(count - 1) * stride + item),
                shape=(count, item), strides=(stride, 1))
            values = np.frombuffer(rows.tobytes(), dtype=dtype).reshape(count, width)
        else:
            if start + count * item > len(data):
                raise GltfError(f"accessor {index} reads past its buffer view")
            values = np.frombuffer(data, dtype=dtype, offset=start, count=count * width).reshape(count, width)
        if accessor.get("normalized") and dtype.kind in "ui":
            limit = float(np.iinfo(dtype).max)
            values = np.maximum(values.astype(np.float64) / limit, -1.0)
        return values

    for index, image in enumerate(document.get("images", [])):
        entry = {"name": image.get("name") or image.get("uri") or f"image{index}", "width": 0, "height": 0,
                 "bytes": 0, "mime": image.get("mimeType"), "uri": image.get("uri"), "missing": False, "data": None}
        data = None
        if "bufferView" in image:
            data, _ = view_bytes(image["bufferView"])
        elif image.get("uri", "").startswith("data:"):
            data = base64.b64decode(image["uri"].split(",", 1)[1])
        elif image.get("uri"):
            target = os.path.join(folder, image["uri"])
            if os.path.isfile(target):
                data = open(target, "rb").read()
            else:
                entry["missing"] = True
        if data is not None:
            entry["data"] = data
            entry["bytes"] = len(data)
            entry["width"], entry["height"], mime = _image_size(data)
            entry["mime"] = mime or entry["mime"]
        model.images.append(entry)
    model.textures = document.get("textures", [])
    model.materials = document.get("materials", [])
    model.skins = document.get("skins", [])
    model.nodes = document.get("nodes", [])

    # World matrices of the nodes of the file's scene (the engine shows only those).
    def local(node):
        if "matrix" in node:
            return np.array(node["matrix"], dtype=np.float64).reshape(4, 4).T
        t = np.array(node.get("translation", [0, 0, 0]), dtype=np.float64)
        x, y, z, w = node.get("rotation", [0, 0, 0, 1])
        s = np.array(node.get("scale", [1, 1, 1]), dtype=np.float64)
        r = np.array([[1 - 2 * (y * y + z * z), 2 * (x * y - z * w), 2 * (x * z + y * w)],
                      [2 * (x * y + z * w), 1 - 2 * (x * x + z * z), 2 * (y * z - x * w)],
                      [2 * (x * z - y * w), 2 * (y * z + x * w), 1 - 2 * (x * x + y * y)]])
        m = np.eye(4)
        m[:3, :3] = r * s
        m[:3, 3] = t
        return m

    world = {}
    scenes = document.get("scenes", [])
    roots = scenes[document.get("scene", 0)].get("nodes", []) if scenes else list(range(len(model.nodes)))
    stack = [(root, np.eye(4)) for root in roots]
    while stack:
        index, parent = stack.pop()
        if index in world or index >= len(model.nodes):
            continue
        matrix = parent @ local(model.nodes[index])
        world[index] = matrix
        for child in model.nodes[index].get("children", []):
            stack.append((child, matrix))
    model.scene_nodes = set(world)
    model.world = world

    meshes = document.get("meshes", [])
    for node_index, node in enumerate(model.nodes):
        if "mesh" not in node or node_index not in world:
            continue
        mesh = meshes[node["mesh"]]
        for primitive in mesh.get("primitives", []):
            p = Primitive()
            p.mesh_name = mesh.get("name", "")
            p.node_name = node.get("name", "")
            p.node = node_index
            p.material = primitive.get("material", -1)
            p.material_name = model.material_name(p.material)
            p.mode = primitive.get("mode", 4)
            p.targets = len(primitive.get("targets", []))
            p.skinned = "skin" in node
            # A skinned mesh is placed by its joints, not by its node (glTF).
            p.world = np.eye(4) if p.skinned else world[node_index]
            attributes = primitive.get("attributes", {})
            if "extensions" in primitive and "KHR_draco_mesh_compression" in primitive["extensions"]:
                model.problems.append(("error", f"{p.label}: Draco compression; the engine cannot read it"))
                continue
            if "POSITION" not in attributes:
                model.problems.append(("error", f"{p.label}: no POSITION attribute"))
                continue
            p.positions = read(attributes["POSITION"]).astype(np.float64)
            p.normals = read(attributes["NORMAL"]).astype(np.float64) if "NORMAL" in attributes else None
            p.tangents = read(attributes["TANGENT"]).astype(np.float64) if "TANGENT" in attributes else None
            p.uvs = read(attributes["TEXCOORD_0"]).astype(np.float64) if "TEXCOORD_0" in attributes else None
            p.colors = read(attributes["COLOR_0"]).astype(np.float64) if "COLOR_0" in attributes else None
            p.joints = read(attributes["JOINTS_0"]).astype(np.int64) if "JOINTS_0" in attributes else None
            p.weights = read(attributes["WEIGHTS_0"]).astype(np.float64) if "WEIGHTS_0" in attributes else None
            p.extra_uv_sets = sum(1 for key in attributes if key.startswith("TEXCOORD_") and key != "TEXCOORD_0")
            p.extra_joint_sets = sum(1 for key in attributes if key.startswith("JOINTS_") and key != "JOINTS_0")
            if "indices" in primitive:
                flat = read(primitive["indices"]).astype(np.int64).reshape(-1)
            else:
                flat = np.arange(len(p.positions), dtype=np.int64)
                p.indexed = False
            if p.mode == 4:
                if len(flat) % 3:
                    model.problems.append(("error", f"{p.label}: index count {len(flat)} is not a multiple of 3"))
                    flat = flat[:len(flat) - len(flat) % 3]
                if len(flat) and flat.max() >= len(p.positions):
                    model.problems.append(("error", f"{p.label}: an index points past the vertex buffer"))
                    flat = np.minimum(flat, len(p.positions) - 1)
                p.indices = flat.reshape(-1, 3)
            else:
                p.indices = np.zeros((0, 3), dtype=np.int64)
            model.primitives.append(p)

    for index, animation in enumerate(document.get("animations", [])):
        seconds = 0.0
        for sampler in animation.get("samplers", []):
            times = accessors[sampler["input"]]
            if times.get("max"):
                seconds = max(seconds, float(times["max"][0]))
        model.animations.append({"name": animation.get("name") or "", "seconds": seconds,
                                 "channels": len(animation.get("channels", []))})
    return model


def write_glb(path, primitives, materials=None, images=None, nodes=None, extra=None):
    """Write a small GLB (the self-test's fixtures). `primitives` is a list of dicts with
    `positions`, `indices` and optionally `normals`, `uvs`, `material`, `translation`, `scale`."""
    blob = bytearray()
    views, accessors, meshes, node_list = [], [], [], []

    def add(array, dtype, kind, target=None, bounds=False):
        data = np.ascontiguousarray(array, dtype=dtype)
        while len(blob) % 4:
            blob.append(0)
        view = {"buffer": 0, "byteOffset": len(blob), "byteLength": data.nbytes}
        if target:
            view["target"] = target
        blob.extend(data.tobytes())
        views.append(view)
        component = {np.dtype(np.float32): 5126, np.dtype(np.uint32): 5125, np.dtype(np.uint16): 5123}[np.dtype(dtype)]
        accessor = {"bufferView": len(views) - 1, "componentType": component, "count": int(len(data)), "type": kind}
        if bounds:
            accessor["min"] = [float(v) for v in data.min(axis=0)]
            accessor["max"] = [float(v) for v in data.max(axis=0)]
        accessors.append(accessor)
        return len(accessors) - 1

    for index, primitive in enumerate(primitives):
        attributes = {"POSITION": add(primitive["positions"], np.float32, "VEC3", 34962, bounds=True)}
        if primitive.get("normals") is not None:
            attributes["NORMAL"] = add(primitive["normals"], np.float32, "VEC3", 34962)
        if primitive.get("uvs") is not None:
            attributes["TEXCOORD_0"] = add(primitive["uvs"], np.float32, "VEC2", 34962)
        entry = {"attributes": attributes,
                 "indices": add(np.asarray(primitive["indices"]).reshape(-1), np.uint32, "SCALAR", 34963)}
        if primitive.get("material") is not None:
            entry["material"] = primitive["material"]
        meshes.append({"name": primitive.get("name", f"mesh{index}"), "primitives": [entry]})
        node = {"name": primitive.get("name", f"node{index}"), "mesh": index}
        for key in ("translation", "scale", "rotation"):
            if primitive.get(key) is not None:
                node[key] = list(primitive[key])
        node_list.append(node)
    document = {"asset": {"version": "2.0", "generator": "modelkit"}, "scene": 0,
                "scenes": [{"nodes": list(range(len(node_list)))}], "nodes": nodes or node_list, "meshes": meshes,
                "materials": materials or [], "accessors": accessors, "bufferViews": views}
    if images:
        document["images"], document["textures"] = [], []
        for index, data in enumerate(images):
            while len(blob) % 4:
                blob.append(0)
            views.append({"buffer": 0, "byteOffset": len(blob), "byteLength": len(data)})
            blob.extend(data)
            document["images"].append({"bufferView": len(views) - 1, "mimeType": "image/png", "name": f"image{index}"})
            document["textures"].append({"source": index})
    while len(blob) % 4:
        blob.append(0)
    document.update(extra or {})
    document["buffers"] = [{"byteLength": len(blob)}]
    text = json.dumps(document).encode("utf-8")
    text += b" " * (-len(text) % 4)
    with open(path, "wb") as out:
        out.write(b"glTF" + struct.pack("<II", 2, 12 + 8 + len(text) + 8 + len(blob)))
        out.write(struct.pack("<II", len(text), 0x4E4F534A) + text)
        out.write(struct.pack("<II", len(blob), 0x004E4942) + bytes(blob))
