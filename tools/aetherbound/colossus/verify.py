#!/usr/bin/env python3
"""Checks colossus.glb against the brief: limits, animation names and lengths, that the clips
join up (dormant -> awaken -> idle, stagger -> overload_enter -> overload -> sunder), that the
eyes are dark in `dormant` and lit 0.6 s into `awaken`, and that the looping clips loop.

    python3 tools/aetherbound/colossus/verify.py
"""
import json
import os
import struct
import sys
import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(os.path.dirname(HERE)))
PATH = os.path.join(ROOT, "samples", "aetherbound", "models", "colossus.glb")
EXPECT = {"dormant": 4, "awaken": 5, "idle": 3, "slam": 1.8, "sweep": 2.0, "pulse": 2.6, "hit": 0.6, "stagger": 2.5,
          "overload_enter": 3, "overload": 2, "sunder": 6}


def load(path):
    d = open(path, "rb").read()
    n = struct.unpack("<I", d[12:16])[0]
    return json.loads(d[20:20 + n]), d[20 + n + 8:]


def accessor(j, b, i):
    a = j["accessors"][i]
    v = j["bufferViews"][a["bufferView"]]
    comp = {"SCALAR": 1, "VEC2": 2, "VEC3": 3, "VEC4": 4, "MAT4": 16}[a["type"]]
    dt = {5126: np.float32, 5123: np.uint16, 5121: np.uint8, 5125: np.uint32}[a["componentType"]]
    off = v.get("byteOffset", 0) + a.get("byteOffset", 0)
    return np.frombuffer(b, dtype=dt, count=a["count"] * comp, offset=off).reshape(a["count"], comp)


def main():
    j, b = load(PATH)
    ok = True

    def check(cond, msg):
        nonlocal ok
        print(("ok   " if cond else "FAIL ") + msg)
        ok = ok and cond
    verts = sum(j["accessors"][p["attributes"]["POSITION"]]["count"] for m in j["meshes"] for p in m["primitives"])
    check(verts <= 15000, f"{verts} vertices (limit 15000)")
    check(len(j["skins"]) == 1, f"{len(j['skins'])} skin, {len(j['skins'][0]['joints'])} joints")
    check(len(j["materials"]) <= 5, f"{len(j['materials'])} materials: {[m['name'] for m in j['materials']]}")
    for p in j["meshes"][0]["primitives"]:
        w = accessor(j, b, p["attributes"]["WEIGHTS_0"])
        check("JOINTS_1" not in p["attributes"] and abs(w.sum(axis=1) - 1).max() < 1e-3, "at most 4 joints per vertex, weights sum to 1")
    names = [n.get("name") for n in j["nodes"]]
    anims = {}
    for a in j["animations"]:
        ch = {}
        for c in a["channels"]:
            s = a["samplers"][c["sampler"]]
            ch[(names[c["target"]["node"]], c["target"]["path"])] = (accessor(j, b, s["input"])[:, 0], accessor(j, b, s["output"]))
        anims[a["name"]] = ch
    for name, length in EXPECT.items():
        if name not in anims:
            check(False, f"animation {name} missing")
            continue
        tmax = max(t[-1] for t, _ in anims[name].values())
        check(abs(tmax - length) < 1e-3, f"{name}: {tmax:.3f} s (expected {length})")
    check(set(anims) == set(EXPECT), f"animations: {sorted(anims)}")

    rest = {}
    for n in j["nodes"]:
        rest[(n.get("name"), "translation")] = np.array(n.get("translation", [0, 0, 0]), dtype=float)
        rest[(n.get("name"), "rotation")] = np.array(n.get("rotation", [0, 0, 0, 1]), dtype=float)
        rest[(n.get("name"), "scale")] = np.array(n.get("scale", [1, 1, 1]), dtype=float)

    def pose(name, t):
        out = {}
        for key in rest:
            if key in anims[name]:
                ts, vs = anims[name][key]
                i = int(np.argmin(np.abs(ts - t)))
                out[key] = vs[i].astype(float)
            else:
                out[key] = rest[key]
        return out

    def diff(a, b_):
        worst = 0.0
        for key in a:
            x, y = a[key], b_[key]
            if key[1] == "rotation" and np.dot(x, y) < 0:
                y = -y
            worst = max(worst, float(np.abs(x - y).max()))
        return worst
    for a, ta, b_, tb in (("dormant", 0, "awaken", 0), ("awaken", 5.0, "idle", 0), ("stagger", 2.5, "overload_enter", 0),
                          ("overload_enter", 3.0, "overload", 0), ("overload", 0, "sunder", 0), ("idle", 0, "idle", 3.0),
                          ("dormant", 0, "dormant", 4.0), ("overload", 0, "overload", 2.0), ("idle", 0, "slam", 0), ("slam", 1.8, "idle", 0),
                          ("idle", 0, "sweep", 0), ("sweep", 2.0, "idle", 0), ("idle", 0, "pulse", 0), ("pulse", 2.6, "idle", 0),
                          ("idle", 0, "hit", 0), ("hit", 0.6, "idle", 0), ("idle", 0, "stagger", 0)):
        d = diff(pose(a, ta), pose(b_, tb))
        check(d < 0.02, f"{a} at {ta} s matches {b_} at {tb} s (largest difference {d:.4f})")
    for eye in ("eye_L", "eye_R"):
        s = anims["dormant"].get((eye, "scale"))
        dark = max(v.max() for v in [s[1]]) if s else 1.0
        check(dark < 0.01, f"{eye} dark in dormant (scale {dark:.4f})")
        ts, vs = anims["awaken"][(eye, "scale")]
        before = vs[ts <= 0.5 + 1e-4].max()
        at = vs[int(np.argmin(np.abs(ts - 0.6)))].max()
        check(before < 0.01 and at >= 1.0, f"{eye} in awaken: {before:.4f} up to 0.5 s, {at:.2f} at 0.6 s")
    print("all good" if ok else "PROBLEMS")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
