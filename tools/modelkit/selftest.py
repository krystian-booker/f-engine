#!/usr/bin/env python3
"""Proves the validator: every check is shown a small model with exactly the
flaw it exists for and must name it, and a clean model must come back clean.

    python3 tools/modelkit/selftest.py            # numpy and pillow
    python3 tools/modelkit/selftest.py --blender  # also the bpy helpers (needs the bpy module)
"""
import io
import json
import os
import shutil
import sys
import tempfile

import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

import validate  # noqa: E402
from modelkit import gltf  # noqa: E402

WORK = tempfile.mkdtemp(prefix="modelkit-selftest-")
FAILURES = []


def png(width=64, height=64):
    from PIL import Image
    data = io.BytesIO()
    Image.new("RGB", (width, height), (120, 110, 100)).save(data, "PNG")
    return data.getvalue()


def box(size=(1, 1, 1), centre=(0, 0.5, 0), flip=False, smooth=False):
    """A box of 24 vertices: hard edges, outward normals, one UV square per face."""
    sx, sy, sz = (v / 2 for v in size)
    faces = [((1, 0, 0), (0, 1, 0), (0, 0, 1)), ((-1, 0, 0), (0, 1, 0), (0, 0, -1)), ((0, 1, 0), (0, 0, 1), (1, 0, 0)),
             ((0, -1, 0), (1, 0, 0), (0, 0, 1)), ((0, 0, 1), (1, 0, 0), (0, 1, 0)), ((0, 0, -1), (0, 1, 0), (1, 0, 0))]
    positions, normals, uvs, indices = [], [], [], []
    for normal, u, v in faces:
        n, u, v = np.array(normal, float), np.array(u, float), np.array(v, float)
        base = len(positions)
        for a, b in ((-1, -1), (1, -1), (1, 1), (-1, 1)):
            corner = (n + a * u + b * v) * [sx, sy, sz] + centre
            positions.append(corner)
            normals.append(-n if flip else n)
            uvs.append([(a + 1) / 2, (b + 1) / 2])
        quad = [[base, base + 1, base + 2], [base, base + 2, base + 3]]
        indices += [[t[0], t[2], t[1]] for t in quad] if flip else quad
    return {"positions": np.array(positions), "normals": np.array(normals), "uvs": np.array(uvs), "indices": np.array(indices)}


def grid(columns=3, rows=3, cell=1.0, uv_scale=1.0):
    """A flat, welded grid in the XZ plane facing up."""
    xs, zs = np.meshgrid(np.arange(columns + 1) * cell, np.arange(rows + 1) * cell)
    positions = np.stack([xs.ravel(), np.zeros(xs.size), zs.ravel()], axis=1)
    indices = []
    for r in range(rows):
        for c in range(columns):
            a = r * (columns + 1) + c
            b, d, e = a + 1, a + columns + 1, a + columns + 2
            indices += [[a, d, b], [b, d, e]]
    uvs = positions[:, [0, 2]] / (max(columns, rows) * cell) * uv_scale
    return {"positions": positions, "normals": np.tile([0.0, 1, 0], (len(positions), 1)), "uvs": uvs, "indices": np.array(indices)}


def merged(*parts):
    """Several primitives' data as one primitive."""
    out = {"positions": [], "normals": [], "uvs": [], "indices": []}
    offset = 0
    for part in parts:
        out["positions"].append(part["positions"])
        out["normals"].append(part["normals"])
        out["uvs"].append(part["uvs"])
        out["indices"].append(np.asarray(part["indices"]) + offset)
        offset += len(part["positions"])
    return {key: np.concatenate(value) for key, value in out.items()}


TEXTURED = [{"name": "stone", "pbrMetallicRoughness": {"baseColorTexture": {"index": 0}}}]
PLAIN = [{"name": "stone", "pbrMetallicRoughness": {"baseColorFactor": [0.5, 0.5, 0.5, 1]}}]


def case(name, primitives, expect=(), profile="default", overrides=None, materials=PLAIN, images=None, clean=False, extra=None,
         absent=()):
    """Write the model, validate it, and check that every id in `expect` was
    reported (and those in `absent` were not; `clean`: nothing at all)."""
    path = os.path.join(WORK, name.replace(" ", "_") + ".glb")
    for primitive in primitives:
        primitive.setdefault("material", 0)
    gltf.write_glb(path, primitives, materials=materials, images=images, extra=extra)
    settings = validate.profile_named(validate.load_profiles({profile: overrides or {}}), profile)
    findings, _ = validate.validate(path, settings)
    found = {f.check for f in findings if f.severity != "info"}
    problems = [f"missed {check}" for check in expect if check not in found]
    problems += [f"reported {check}" for check in absent if check in found]
    if clean and found:
        problems.append("reported " + ", ".join(sorted(found)))
    status = "ok" if not problems else "FAILED: " + "; ".join(problems)
    print(f"  {name:<58} {status}")
    if problems:
        FAILURES.append(name)
    return findings


def main():
    print("mesh integrity")
    case("a clean box is clean", [box()], clean=True)
    case("two boxes touching along an edge are not a defect", [merged(box(), box(centre=(1, 1.5, 0)))], clean=True)
    b = box()
    b["indices"] = np.vstack([b["indices"], [[0, 0, 1]]])
    case("a collapsed triangle", [b], ["integrity.degenerate_faces"])
    b = box()
    b["indices"] = np.vstack([b["indices"], b["indices"][:1]])
    case("the same face twice", [b], ["integrity.duplicate_faces"])
    fin = {"positions": np.array([[0.5, 0, 0.5], [0.5, 1, 0.5], [1.5, 0.5, 1.5]]), "normals": np.tile([0.7, 0, -0.7], (3, 1)),
           "uvs": np.zeros((3, 2)), "indices": np.array([[0, 1, 2]])}
    case("a fin on a box's edge", [merged(box(), fin)], ["integrity.non_manifold_edges"])
    b = box()
    b["positions"] = np.vstack([b["positions"], [[5, 5, 5]]])
    b["normals"] = np.vstack([b["normals"], [[0, 1, 0]]])
    b["uvs"] = np.vstack([b["uvs"], [[0, 0]]])
    case("a vertex nothing uses", [b], ["integrity.loose_vertices"])
    scrap = {"positions": np.array([[4, 0, 4], [4.1, 0, 4], [4, 0, 4.1]]), "normals": np.tile([0.0, 1, 0], (3, 1)),
             "uvs": np.zeros((3, 2)), "indices": np.array([[0, 2, 1]])}
    case("a lone triangle", [merged(box(), scrap)], ["integrity.loose_parts"])
    g = grid(6, 6)
    soup = {key: g[key][g["indices"].reshape(-1)] for key in ("positions", "normals", "uvs")}
    soup["indices"] = np.arange(len(soup["positions"])).reshape(-1, 3)
    case("a mesh exported unwelded", [soup], ["integrity.duplicate_vertices"])

    print("normals and shading")
    b = box()
    b["normals"] = -b["normals"]
    case("normals pointing inwards", [b], ["normals.flipped"])
    case("a closed shell inside out", [box(flip=True)], ["normals.inside_out"])
    g = grid()
    g["indices"][4] = g["indices"][4][[0, 2, 1]]
    case("one face wound the other way", [g], ["normals.inconsistent_winding"])
    g = grid()
    g["normals"] = np.tile([0.996, 0.087, 0], (len(g["positions"]), 1))
    case("normals almost in the plane of their faces", [g], ["normals.shading"])
    g = grid()
    g["normals"][3] = [0, 0, 0]
    case("a zero-length normal", [g], ["normals.degenerate"])
    g = grid()
    g["normals"] = None
    case("no normals at all", [g], ["normals.missing"])

    print("topology")
    strip = grid(40, 1)
    strip["positions"][:, 2] *= 0.0005
    strip["positions"][:, 0] *= 0.05
    strip["positions"] = strip["positions"] * [1, 1, 1]
    strip["positions"][:, 0] *= 20  # each cell 1 long, 0.0005 wide
    case("a strip of slivers", [strip], ["topology.slivers", "topology.needles"])
    big = {"positions": np.array([[0, 0, 0], [2, 0, 0], [2, 0, 2], [0, 0, 2]], float), "indices": np.array([[0, 2, 1], [0, 3, 2]])}
    small = {"positions": np.array([[2, 0, 0], [3, 0, 0], [3, 0, 1], [2, 0, 1], [3, 0, 2], [2, 0, 2]], float),
             "indices": np.array([[0, 3, 1], [1, 3, 2], [3, 5, 2], [2, 5, 4]])}
    for part in (big, small):
        part["normals"] = np.tile([0.0, 1, 0], (len(part["positions"]), 1))
        part["uvs"] = part["positions"][:, [0, 2]] / 3
    case("a vertex on the middle of a neighbour's edge", [merged(big, small)], ["topology.t_junctions"])
    angles = np.linspace(0, 2 * np.pi, 24, endpoint=False)
    ring = np.stack([np.cos(angles), np.zeros(24), np.sin(angles)], axis=1)
    fan = {"positions": ring, "normals": np.tile([0.0, 1, 0], (24, 1)), "uvs": ring[:, [0, 2]] / 2 + 0.5,
           "indices": np.array([[0, i + 1, i] for i in range(1, 23)])}
    case("a 24-sided polygon triangulated as a fan", [fan], ["topology.ngons"])
    case("a round disc of proper quads is not an n-gon", [grid(6, 6)], absent=["topology.ngons"])
    over = grid(1, 1)
    under = grid(1, 1)
    under["positions"] = under["positions"] * 0.5 + [0.25, 0, 0.25]
    case("one face lying on another", [merged(over, under)], ["topology.coplanar_overlap"])
    lid = box()
    floor = grid(3, 3)
    floor["positions"] -= [1.5, 0, 1.5]
    case("a box standing on a floor is not z-fighting", [merged(floor, lid)], absent=["topology.coplanar_overlap"])

    print("UV mapping")
    g = grid()
    g["uvs"] = None
    case("no UVs under a texture", [g], ["uv.missing"], materials=TEXTURED, images=[png()])
    g = grid()
    g["uvs"] = np.zeros_like(g["uvs"])
    case("every UV on one point", [g], ["uv.collapsed"], materials=TEXTURED, images=[png()])
    a, b = grid(2, 2), grid(2, 2, uv_scale=8.0)
    b["positions"] += [3, 0, 0]
    case("two parts at very different texel densities", [merged(a, b)], ["uv.texel_density_spread"], materials=TEXTURED,
         images=[png()])
    case("the wrong texel density for its profile", [grid(4, 4, uv_scale=1.0)], ["uv.texel_density"],
         overrides={"texel_density": 128}, materials=TEXTURED, images=[png()])
    a, b = grid(2, 2), grid(2, 2)
    b["positions"] += [3, 0, 0]
    case("two islands on the same texels", [merged(a, b)], ["uv.overlap"], overrides={"uv": "unique"}, materials=TEXTURED,
         images=[png()])
    g = grid()
    g["uvs"] = g["uvs"] * [1, 0.05]
    case("a texture stretched twenty to one", [g], ["uv.stretch"], materials=TEXTURED, images=[png()])
    tiles = []
    for index in range(12):
        tile = grid(1, 1)
        tile["positions"] += [index * 1.5, 0, 0]
        tile["uvs"] = tile["uvs"] * 0.01 + [index * 0.05, 0.5]
        tiles.append(tile)
    case("islands smaller than a few texels", [merged(*tiles)], ["uv.tiny_islands"], materials=TEXTURED, images=[png()])

    print("game readiness")
    case("over its triangle budget", [box()], ["ready.max_triangles"], overrides={"max_triangles": 4})
    case("the wrong size for its profile", [box(size=(10, 10, 10), centre=(0, 5, 0))], ["ready.scale"], overrides={"height": [1, 2]})
    case("its origin is not under it", [box(centre=(0, 0, 0))], ["ready.origin"], overrides={"origin": "base_centre"})
    b = box()
    b["scale"] = [2, 2, 2]
    case("a scale that was never applied", [b], ["ready.transforms"])
    case("a material without a name", [box()], ["ready.material_names"], materials=[{}])
    case("a material nothing uses", [box()], ["ready.unused_material"], materials=PLAIN + [{"name": "spare"}])
    case("a texture that is not a power of two", [box()], ["ready.texture_pot"], materials=TEXTURED, images=[png(100, 60)])
    case("a texture larger than the profile allows", [box()], ["ready.texture_size"], overrides={"max_texture_size": 32},
         materials=TEXTURED, images=[png()])
    case("an extension the engine cannot read", [box()], ["ready.extensions"],
         extra={"extensionsUsed": ["KHR_draco_mesh_compression"], "extensionsRequired": ["KHR_draco_mesh_compression"]})
    broken = os.path.join(WORK, "broken.glb")
    with open(broken, "wb") as out:
        out.write(b"glTF\x02\x00\x00\x00")
    findings, _ = validate.validate(broken, validate.profile_named(validate.load_profiles(), "default"))
    ok = any(f.check == "ready.structure" for f in findings)
    print(f"  {'a file that is not a glTF':<58} {'ok' if ok else 'FAILED'}")
    if not ok:
        FAILURES.append("broken file")

    print("visual consistency (the sign-off)")
    path = os.path.join(WORK, "signed.glb")
    gltf.write_glb(path, [dict(box(), material=0)], materials=PLAIN)
    profile = validate.profile_named(validate.load_profiles(), "default")
    for name, review, expected in (
            ("a model nobody has looked at", None, True),
            ("a review of an older version of the file", {"verdict": "pass", "sha256": "0" * 64}, True),
            ("a review that failed it", {"verdict": "fail", "sha256": validate.sha256(path), "notes": "reads as a blob"}, True),
            ("a review that passed this exact file", {"verdict": "pass", "sha256": validate.sha256(path)}, False)):
        findings, _ = validate.validate(path, profile, review=review, require_review=True)
        blocked = any(f.check == "visual.review" for f in findings)
        print(f"  {name:<58} {'ok' if blocked == expected else 'FAILED'}")
        if blocked != expected:
            FAILURES.append(name)

    print("waivers")
    path = os.path.join(WORK, "waived.glb")
    flawed = box()
    flawed["indices"] = np.concatenate([flawed["indices"], [[0, 0, 1], [1, 1, 2]]])  # two collapsed faces
    gltf.write_glb(path, [dict(flawed, material=0)], materials=PLAIN)

    def left(waivers, check="integrity.degenerate_faces", **more):
        findings, _ = validate.validate(path, profile, waivers, **more)
        return any(f.check == check and not f.waived for f in findings)

    for name, got, expected in (
            ("a flaw with no waiver fails", left({}), True),
            ("a waiver with a reason lets it through", left({"integrity.degenerate_faces": "known, fix in the generator"}), False),
            ("a capped waiver holds while the count is under the cap",
             left({"integrity.degenerate_faces": {"reason": "known", "max": 2}}), False),
            ("a capped waiver stops holding when the flaw grows",
             left({"integrity.degenerate_faces": {"reason": "known", "max": 1}}), True),
            ("a waiver with no reason is not a waiver", left({"integrity.degenerate_faces": {"max": 9}}), True),
            ("the visual review cannot be waived",
             left({"visual.review": "trust me"}, check="visual.review", require_review=True), True)):
        print(f"  {name:<58} {'ok' if got == expected else 'FAILED'}")
        if got != expected:
            FAILURES.append(name)

    if "--blender" in sys.argv:
        blender_helpers()
    shutil.rmtree(WORK, ignore_errors=True)
    print(f"\n{'all checks proved' if not FAILURES else str(len(FAILURES)) + ' FAILED: ' + ', '.join(FAILURES)}")
    return 1 if FAILURES else 0


def blender_helpers():
    """The bpy helpers: a mesh with real defects is reported, cleaned, exported, and passes."""
    print("Blender helpers")
    sys.path.insert(0, os.path.join(HERE, "blender"))
    import bpy
    import modelkit_bpy as mk
    bpy.ops.wm.read_factory_settings(use_empty=True)
    # A crate with a 12-sided lid, a doubled vertex, a loose vertex and an unapplied scale.
    angles = np.linspace(0, 2 * np.pi, 12, endpoint=False)
    vertices = [(float(np.cos(a)) * 0.5, float(np.sin(a)) * 0.5, 1.0) for a in angles]
    vertices += [(float(np.cos(a)) * 0.5, float(np.sin(a)) * 0.5, 0.0) for a in angles]
    faces = [tuple(range(12)), tuple(range(23, 11, -1))] + [(i, 12 + i, 12 + (i + 1) % 12, (i + 1) % 12) for i in range(12)]
    vertices += [(0.5, 0.0, 1.0), (3.0, 3.0, 3.0)]  # a double of vertex 0, and a stray
    mesh = bpy.data.meshes.new("crate")
    mesh.from_pydata(vertices, [], faces)
    obj = bpy.data.objects.new("crate", mesh)
    bpy.context.scene.collection.objects.link(obj)
    obj.scale = (1.0, 1.0, 1.5)
    bpy.context.view_layer.update()
    material = bpy.data.materials.new("wood")
    mesh.materials.append(material)
    before = mk.inspect(obj)
    text = " | ".join(message for _, message in before)
    for needle in ("n-gons", "loose", "coincide", "scale"):
        ok = needle in text
        print(f"  {'inspect reports ' + needle:<58} {'ok' if ok else 'FAILED'}")
        if not ok:
            FAILURES.append("inspect " + needle)
    ready = mk.prepare([obj], sharp_angle=40, origin="base_centre", report=lambda line: None)
    mk.box_unwrap(obj, pixels_per_unit=32, texture_size=64)
    density = mk.texel_density(obj, 64)
    out = os.path.join(WORK, "crate.glb")
    mk.export_glb(out, [obj], animations=False)
    profile = validate.profile_named(validate.load_profiles({"prop": {"texel_density": None}}), "prop")
    findings, model = validate.validate(out, profile)
    bad = sorted({f.check for f in findings if f.severity != "info"})
    for name, ok in (("prepare leaves nothing export should not see", ready), ("box_unwrap gives the density asked for", abs(density - 32) < 4),
                     ("the cleaned, exported crate validates", not bad)):
        print(f"  {name:<58} {'ok' if ok else 'FAILED ' + ', '.join(bad)}")
        if not ok:
            FAILURES.append(name)


if __name__ == "__main__":
    sys.exit(main())
