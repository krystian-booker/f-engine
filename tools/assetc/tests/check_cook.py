#!/usr/bin/env python3
"""Checks `fengine_assetc cook` output: the crate's albedo, normal and
emissive textures became BC7 KTX files with full mip chains (normal map
linear, colour sRGB); sprite images were left alone."""
import struct
import sys
from pathlib import Path

BC7, BC7_SRGB = 0x8E8C, 0x8E8D
folder = Path(sys.argv[1])


def header(path):
    data = path.read_bytes()
    assert data[:12] == b"\xabKTX 11\xbb\r\n\x1a\n", f"{path} is not KTX 1"
    words = struct.unpack("<13I", data[12:64])
    return {"format": words[4], "width": words[6], "height": words[7], "mips": words[11]}


expected = {"crate.ktx": BC7_SRGB, "crate_glow.ktx": BC7_SRGB, "crate_normal.ktx": BC7}
for name, internal in expected.items():
    info = header(folder / name)
    assert info["format"] == internal, f"{name}: format {info['format']:#x}, expected {internal:#x}"
    assert (info["width"], info["height"]) == (64, 64), f"{name}: size {info['width']}x{info['height']}"
    assert info["mips"] == 7, f"{name}: {info['mips']} mips, expected 7"
for sprite in ("rook_lit.ktx", "rook_lit_normal.ktx", "circle.ktx"):
    assert not (folder / sprite).exists(), f"{sprite}: sprite images stay PNG"
print("cooked textures OK")
