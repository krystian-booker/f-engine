#!/usr/bin/env python3
"""Writes the stage's floor (maps/stage/geometry.glb): one dark, matt 16 x 16
quad at height 0. Run it again only to change the floor; the result is
checked in. Standard library and numpy."""
import json
import os
import struct

import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))


def main():
    size = 16.0
    positions = np.array([[0, 0, 0], [size, 0, 0], [size, 0, size], [0, 0, size]], dtype=np.float32)
    normals = np.tile(np.array([0, 1, 0], dtype=np.float32), (4, 1))
    uvs = np.array([[0, 0], [1, 0], [1, 1], [0, 1]], dtype=np.float32)
    indices = np.array([0, 2, 1, 0, 3, 2], dtype=np.uint16)
    chunks = [positions.tobytes(), normals.tobytes(), uvs.tobytes(), indices.tobytes()]
    views, offset = [], 0
    for chunk in chunks:
        views.append({"buffer": 0, "byteOffset": offset, "byteLength": len(chunk)})
        offset += len(chunk)
    document = {
        "asset": {"version": "2.0", "generator": "spritekit stage"},
        "scene": 0, "scenes": [{"nodes": [0]}], "nodes": [{"mesh": 0, "name": "floor"}],
        "meshes": [{"name": "floor", "primitives": [{"attributes": {"POSITION": 0, "NORMAL": 1, "TEXCOORD_0": 2}, "indices": 3, "material": 0}]}],
        "materials": [{"name": "floor", "pbrMetallicRoughness": {"baseColorFactor": [0.16, 0.17, 0.21, 1], "metallicFactor": 0, "roughnessFactor": 1}}],
        "buffers": [{"byteLength": offset}], "bufferViews": views,
        "accessors": [
            {"bufferView": 0, "componentType": 5126, "count": 4, "type": "VEC3", "min": [0, 0, 0], "max": [size, 0, size]},
            {"bufferView": 1, "componentType": 5126, "count": 4, "type": "VEC3"},
            {"bufferView": 2, "componentType": 5126, "count": 4, "type": "VEC2"},
            {"bufferView": 3, "componentType": 5123, "count": 6, "type": "SCALAR"}]}
    text = json.dumps(document, separators=(",", ":")).encode()
    text += b" " * (-len(text) % 4)
    binary = b"".join(chunks)
    binary += b"\0" * (-len(binary) % 4)
    with open(os.path.join(HERE, "maps", "stage", "geometry.glb"), "wb") as out:
        out.write(struct.pack("<4sII", b"glTF", 2, 12 + 8 + len(text) + 8 + len(binary)))
        out.write(struct.pack("<I4s", len(text), b"JSON") + text)
        out.write(struct.pack("<I4s", len(binary), b"BIN\0") + binary)


if __name__ == "__main__":
    main()
