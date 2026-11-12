"""Blender side of the fortress build: one mesh object per material, ambient
occlusion baked with Cycles into vertex colours, glTF export with embedded
textures. World (x, y, z) is Blender (x, -z, y)."""
import json
import os
import struct
import time
import numpy as np
import bpy
from mathutils import Vector
from mathutils.bvhtree import BVHTree

# material -> (albedo, normal, emissive, emissive strength, blend)
MATERIALS = {
    "wall": ("wall.png", "wall_n.png", None, 0, False),
    "floor": ("floor.png", "floor_n.png", None, 0, False),
    "rock": ("rock.png", "rock_n.png", None, 0, False),
    "block": ("block.png", "block_n.png", None, 0, False),
    "props": ("props.png", "props_n.png", "props_e.png", 3.6, False),
    "mist": ("mist.png", None, None, 0, True),
}
ORDER = ["floor", "wall", "block", "rock", "props", "mist"]


def reset():
    bpy.ops.wm.read_factory_settings(use_empty=True)
    scene = bpy.context.scene
    scene.render.engine = "CYCLES"
    scene.cycles.device = "CPU"
    scene.render.threads_mode = "FIXED"
    scene.render.threads = 4
    return scene


def material(name, texdir):
    albedo, normal, emissive, strength, blend = MATERIALS[name]
    m = bpy.data.materials.new(name)
    m.use_nodes = True
    m.use_backface_culling = True
    nt = m.node_tree
    bsdf = nt.nodes["Principled BSDF"]
    bsdf.inputs["Roughness"].default_value = 1.0
    bsdf.inputs["Metallic"].default_value = 0.0

    def tex(file, space):
        node = nt.nodes.new("ShaderNodeTexImage")
        node.image = bpy.data.images.load(os.path.join(texdir, file))
        node.image.colorspace_settings.name = space
        node.interpolation = "Closest"
        return node

    a = tex(albedo, "sRGB")
    nt.links.new(a.outputs["Color"], bsdf.inputs["Base Color"])
    if blend:
        nt.links.new(a.outputs["Alpha"], bsdf.inputs["Alpha"])
        m.surface_render_method = "BLENDED"
        m.use_backface_culling = False
    if normal:
        n = tex(normal, "Non-Color")
        nm = nt.nodes.new("ShaderNodeNormalMap")
        nt.links.new(n.outputs["Color"], nm.inputs["Color"])
        nt.links.new(nm.outputs["Normal"], bsdf.inputs["Normal"])
    if emissive:
        e = tex(emissive, "sRGB")
        nt.links.new(e.outputs["Color"], bsdf.inputs["Emission Color"])
        bsdf.inputs["Emission Strength"].default_value = strength
    return m


def to_blender(P):
    return np.stack([P[..., 0], -P[..., 2], P[..., 1]], -1)


def make_object(name, data, mat):
    P = to_blender(data["P"]).reshape(-1, 3)
    # weld positions so the exporter can share vertices
    key = np.round(P * 2048).astype(np.int64)
    uniq, inverse = np.unique(key, axis=0, return_inverse=True)
    inverse = inverse.reshape(-1)
    # welding can collapse slivers: drop triangles that lost a corner
    tri = inverse.reshape(-1, 3)
    good = (tri[:, 0] != tri[:, 1]) & (tri[:, 1] != tri[:, 2]) & (tri[:, 0] != tri[:, 2])
    if not good.all():
        for k_ in ("P", "UV", "C", "N"):
            data[k_] = data[k_][good]
        P = to_blender(data["P"]).reshape(-1, 3)
        inverse = tri[good].reshape(-1)
    N = to_blender(data["N"]).reshape(-1, 3)
    n = len(P) // 3
    co = np.zeros((len(uniq), 3), dtype=np.float32)
    co[inverse] = P
    me = bpy.data.meshes.new(name)
    me.vertices.add(len(co))
    me.vertices.foreach_set("co", co.ravel())
    me.loops.add(len(P))
    me.loops.foreach_set("vertex_index", inverse.astype(np.int32))
    me.polygons.add(n)
    me.polygons.foreach_set("loop_start", np.arange(0, len(P), 3, dtype=np.int32))
    me.polygons.foreach_set("loop_total", np.full(n, 3, dtype=np.int32))
    me.update(calc_edges=True)
    uv = me.uv_layers.new(name="UVMap")
    UV = data["UV"].reshape(-1, 2).copy()
    UV[:, 1] = 1.0 - UV[:, 1]
    uv.data.foreach_set("uv", UV.astype(np.float32).ravel())
    attr = me.color_attributes.new("Color", "FLOAT_COLOR", "CORNER")
    me.color_attributes.active_color = attr
    me.color_attributes.render_color_index = 0
    me.materials.append(mat)
    ob = bpy.data.objects.new(name, me)
    bpy.context.scene.collection.objects.link(ob)
    # corners that share position, normal and uv get one colour, so they export as one vertex
    group_key = np.concatenate([inverse[:, None].astype(np.int64), np.round(N * 64).astype(np.int64),
                                np.round(data["UV"].reshape(-1, 2) * 4096).astype(np.int64)], 1)
    _, groups = np.unique(group_key, axis=0, return_inverse=True)
    ob["_groups"] = 0
    GROUPS[name] = groups.reshape(-1)
    return ob


GROUPS = {}


def average_groups(name, C):
    g = GROUPS[name]
    count = np.bincount(g).astype(np.float32)
    out = np.zeros_like(C)
    for ch in range(C.shape[1]):
        out[:, ch] = (np.bincount(g, weights=C[:, ch]) / count)[g]
    return out


def bake_ao(objects, distance=2.5, samples=48):
    scene = bpy.context.scene
    scene.cycles.samples = samples
    if scene.world is None:
        scene.world = bpy.data.worlds.new("world")
    scene.world.light_settings.distance = distance
    scene.render.bake.target = "VERTEX_COLORS"
    out = {}
    for ob in objects:
        for o in bpy.context.scene.objects:
            o.select_set(False)
        ob.select_set(True)
        bpy.context.view_layer.objects.active = ob
        t = time.time()
        bpy.ops.object.bake(type="AO")
        me = ob.data
        col = np.zeros(len(me.loops) * 4, dtype=np.float32)
        me.color_attributes["Color"].data.foreach_get("color", col)
        out[ob.name] = col.reshape(-1, 4)[:, 0].copy()
        print(f"  baked AO {ob.name}: {len(me.polygons)} tris in {time.time() - t:.1f}s")
    return out


def build(arrays, texdir, out_glb, ao=True, ao_floor=0.34, ao_gamma=0.85):
    reset()
    objects = {}
    for name in ORDER:
        if name in arrays and len(arrays[name]["P"]):
            objects[name] = make_object(name, arrays[name], material(name, texdir))
    solid = [objects[n] for n in objects if not MATERIALS[n][4]]
    for n in objects:
        if MATERIALS[n][4]:
            objects[n].hide_render = True
    occlusion = bake_ao(solid) if ao else {}
    stats = {}
    for name, ob in objects.items():
        C = arrays[name]["C"].reshape(-1, 3).astype(np.float32)
        if name in occlusion:
            a = occlusion[name]
            a = ao_floor + (1 - ao_floor) * np.power(np.clip(a, 0, 1), ao_gamma)
            C = C * a[:, None]
        C = average_groups(name, C)
        rgba = np.concatenate([np.clip(C, 0, 1), np.ones((len(C), 1), dtype=np.float32)], 1)
        ob.data.color_attributes["Color"].data.foreach_set("color", rgba.ravel())
        ob.hide_render = False
        stats[name] = len(ob.data.polygons)
    for o in bpy.context.scene.objects:
        o.select_set(True)
    bpy.ops.export_scene.gltf(filepath=out_glb, export_format="GLB", export_yup=True, export_apply=True,
                              export_normals=True, export_tangents=False, export_vertex_color="ACTIVE",
                              export_all_vertex_colors=False, export_image_format="AUTO",
                              export_materials="EXPORT", export_animations=False, export_extras=False,
                              export_cameras=False, export_lights=False)
    return stats


def bvh_of(arrays, skip=("mist",)):
    verts, polys = [], []
    for name, data in arrays.items():
        if name in skip:
            continue
        P = data["P"].reshape(-1, 3)
        base = len(verts)
        verts.extend(map(tuple, P))
        polys.extend((base + i, base + i + 1, base + i + 2) for i in range(0, len(P), 3))
    return BVHTree.FromPolygons(verts, polys)


def glb_info(path):
    d = open(path, "rb").read()
    n = struct.unpack("<I", d[12:16])[0]
    j = json.loads(d[20:20 + n])
    info = {"bytes": len(d), "materials": [m["name"] for m in j.get("materials", [])],
            "primitives": sum(len(m["primitives"]) for m in j["meshes"])}
    tris = 0
    for m in j["meshes"]:
        for p in m["primitives"]:
            tris += j["accessors"][p["indices"]]["count"] // 3
    info["triangles"] = tris
    info["alpha"] = {m["name"]: m.get("alphaMode", "OPAQUE") for m in j["materials"]}
    info["double"] = {m["name"]: m.get("doubleSided", False) for m in j["materials"]}
    return info
