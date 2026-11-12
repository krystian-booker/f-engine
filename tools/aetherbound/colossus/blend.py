"""Blender side shared by the colossus and crystal generators: turn a meshkit Builder into a
Blender mesh with materials and an armature, key actions on NLA tracks, export GLB."""
import json
import math
import os
import struct
import numpy as np
import bpy
from mathutils import Matrix, Quaternion, Vector
from PIL import Image


def reset_scene(fps=30):
    bpy.ops.wm.read_factory_settings(use_empty=True)
    bpy.context.preferences.edit.keyframe_new_interpolation_type = 'LINEAR'
    scene = bpy.context.scene
    scene.render.fps = fps
    return scene


def np_image(name, arr, srgb=True):
    """A packed Blender image from a float array [h, w, 3 or 4] in 0..1 (row 0 = top)."""
    h, w = arr.shape[:2]
    rgba = np.ones((h, w, 4), dtype=np.float32)
    rgba[..., :arr.shape[2]] = arr
    img = bpy.data.images.new(name, w, h, alpha=False)
    # Blender stores linear floats for 8-bit images in their colour space; write through a PNG so
    # the bytes in the file are exactly ours
    path = os.path.join(bpy.app.tempdir, name + ".png")
    Image.fromarray((np.clip(rgba[..., :3], 0, 1) * 255 + 0.5).astype(np.uint8), "RGB").save(path)
    bpy.data.images.remove(img)
    img = bpy.data.images.load(path)
    img.name = name
    img.colorspace_settings.name = 'sRGB' if srgb else 'Non-Color'
    img.pack()
    return img


def make_material(name, albedo=None, normal=None, emissive=None, emissive_color=(0, 0, 0), emissive_strength=1.0,
                  base_color=(1, 1, 1), nearest=True):
    mat = bpy.data.materials.new(name)
    mat.use_nodes = True
    nt = mat.node_tree
    bsdf = nt.nodes["Principled BSDF"]
    bsdf.inputs["Metallic"].default_value = 0.0
    bsdf.inputs["Roughness"].default_value = 0.8
    bsdf.inputs["Base Color"].default_value = (*base_color, 1)

    def tex(img):
        n = nt.nodes.new("ShaderNodeTexImage")
        n.image = img
        n.interpolation = 'Closest' if nearest else 'Linear'
        return n
    if albedo is not None:
        nt.links.new(tex(albedo).outputs["Color"], bsdf.inputs["Base Color"])
    if normal is not None:
        nm = nt.nodes.new("ShaderNodeNormalMap")
        nt.links.new(tex(normal).outputs["Color"], nm.inputs["Color"])
        nt.links.new(nm.outputs["Normal"], bsdf.inputs["Normal"])
    if emissive is not None:
        nt.links.new(tex(emissive).outputs["Color"], bsdf.inputs["Emission Color"])
        bsdf.inputs["Emission Strength"].default_value = emissive_strength
    elif any(emissive_color):
        bsdf.inputs["Emission Color"].default_value = (*emissive_color, 1)
        bsdf.inputs["Emission Strength"].default_value = emissive_strength
    return mat


def make_mesh(name, B, materials, scene):
    """materials: list of (atlas name, bpy material) in slot order."""
    pos, nrm, uv, tris = B.arrays()
    me = bpy.data.meshes.new(name)
    me.from_pydata(pos.tolist(), [], tris.tolist())
    slot = {a: i for i, (a, _) in enumerate(materials)}
    for _, m in materials:
        me.materials.append(m)
    me.polygons.foreach_set("material_index", [slot[a] for a in B.mat_of_tri])
    me.polygons.foreach_set("use_smooth", [True] * len(me.polygons))
    layer = me.uv_layers.new(name="UVMap")
    loops = np.zeros(len(me.loops), dtype=np.int32)
    me.loops.foreach_get("vertex_index", loops)
    layer.data.foreach_set("uv", uv[loops].reshape(-1).astype(np.float32))
    me.update()
    me.normals_split_custom_set_from_vertices(nrm.tolist())
    ob = bpy.data.objects.new(name, me)
    scene.collection.objects.link(ob)
    return ob


def make_armature(name, skel, scene):
    arm = bpy.data.armatures.new(name + "_rig")
    ob = bpy.data.objects.new(name, arm)
    scene.collection.objects.link(ob)
    bpy.context.view_layer.objects.active = ob
    bpy.ops.object.mode_set(mode='EDIT')
    for bname, (parent, head) in skel.items():
        b = arm.edit_bones.new(bname)
        b.head = head
        b.tail = (head[0], head[1] + 0.3, head[2])      # along +y with no roll: bone axes = armature axes
        b.roll = 0
    for bname, (parent, head) in skel.items():
        if parent:
            arm.edit_bones[bname].parent = arm.edit_bones[parent]
    bpy.ops.object.mode_set(mode='OBJECT')
    for pb in ob.pose.bones:
        pb.rotation_mode = 'QUATERNION'
    return ob


def skin(mesh_ob, arm_ob, B):
    groups = {}
    for i, ws in enumerate(B.weights):
        tot = sum(w for _, w in ws)
        for bname, w in ws:
            if w <= 0:
                continue
            g = groups.get(bname)
            if g is None:
                g = groups[bname] = mesh_ob.vertex_groups.new(name=bname)
            g.add([i], w / tot, 'REPLACE')
    mesh_ob.parent = arm_ob
    mod = mesh_ob.modifiers.new("armature", 'ARMATURE')
    mod.object = arm_ob


def key_action(arm_ob, name, frames, skel):
    """frames: list (one per frame, at the scene's fps, starting at frame 0) of {bone: (loc, quat, scale)}
    in bone-local (pose basis) terms: loc (x, y, z), quat (w, x, y, z), scale float or 3-tuple.
    Creates the action with one key per frame and parks it on its own NLA track named `name`."""
    ad = arm_ob.animation_data or arm_ob.animation_data_create()
    act = bpy.data.actions.new(name)
    ad.action = act
    bones = list(frames[0].keys())
    last_q = {}
    for f, pose in enumerate(frames):
        for b in bones:
            loc, q, sc = pose[b]
            pb = arm_ob.pose.bones[b]
            q = np.asarray(q, dtype=float)
            if b in last_q and np.dot(last_q[b], q) < 0:
                q = -q
            last_q[b] = q
            pb.location = loc
            pb.rotation_quaternion = q
            pb.scale = (sc, sc, sc) if np.isscalar(sc) else sc
            pb.keyframe_insert("location", frame=f)
            pb.keyframe_insert("rotation_quaternion", frame=f)
            pb.keyframe_insert("scale", frame=f)
    track = ad.nla_tracks.new()
    track.name = name
    track.strips.new(name, 0, act)
    ad.action = None
    return act


def reset_pose(arm_ob):
    for pb in arm_ob.pose.bones:
        pb.location = (0, 0, 0)
        pb.rotation_quaternion = (1, 0, 0, 0)
        pb.scale = (1, 1, 1)


def export_glb(path, animations=True):
    for o in bpy.context.scene.objects:
        o.select_set(False)
    bpy.ops.export_scene.gltf(
        filepath=path, export_format='GLB', export_yup=True, export_apply=False,
        export_texcoords=True, export_normals=True, export_tangents=False,
        export_materials='EXPORT', export_image_format='AUTO', export_vertex_color='NONE',
        export_skins=True, export_all_influences=False, export_influence_nb=4, export_def_bones=False,
        export_animations=animations, export_animation_mode='NLA_TRACKS', export_force_sampling=True,
        export_optimize_animation_size=True, export_optimize_animation_keep_anim_armature=True,
        export_anim_slide_to_zero=True, export_morph=False, export_lights=False, export_cameras=False)


def read_glb(path):
    d = open(path, "rb").read()
    n = struct.unpack("<I", d[12:16])[0]
    return json.loads(d[20:20 + n]), d[20 + n + 8:]


def patch_glb(path, fn):
    """Rewrite the JSON chunk of a GLB through fn(json)."""
    d = open(path, "rb").read()
    n = struct.unpack("<I", d[12:16])[0]
    j = json.loads(d[20:20 + n])
    rest = d[20 + n:]
    fn(j)
    body = json.dumps(j, separators=(",", ":")).encode()
    body += b" " * ((4 - len(body) % 4) % 4)
    out = struct.pack("<4sII", b"glTF", 2, 12 + 8 + len(body) + len(rest)) + struct.pack("<I4s", len(body), b"JSON") + body + rest
    open(path, "wb").write(out)


def glb_report(path):
    j, _ = read_glb(path)
    verts = 0
    for m in j["meshes"]:
        for p in m["primitives"]:
            verts += j["accessors"][p["attributes"]["POSITION"]]["count"]
    anims = {}
    for a in j.get("animations", []):
        tmax = max(j["accessors"][s["input"]]["max"][0] for s in a["samplers"])
        anims[a["name"]] = round(tmax, 4)
    joints = len(j["skins"][0]["joints"]) if j.get("skins") else 0
    return dict(vertices=verts, joints=joints, animations=anims, materials=[m["name"] for m in j.get("materials", [])],
                images=len(j.get("images", [])), bytes=os.path.getsize(path))
