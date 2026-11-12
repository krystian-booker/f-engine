"""Blender side: turns a kit.Mesh into objects (one per texture page), builds
the three materials, bakes ambient occlusion into vertex colours with Cycles
and exports geometry.glb with the textures embedded."""
import os
import numpy as np
import bpy

EMISSIVE = {"floor": 5.0, "metal": 5.0}


def reset():
    bpy.ops.wm.read_factory_settings(use_empty=True)
    scene = bpy.context.scene
    scene.render.engine = "CYCLES"
    scene.cycles.device = "CPU"
    scene.render.threads_mode = "FIXED"
    scene.render.threads = 4
    if scene.world is None:
        scene.world = bpy.data.worlds.new("World")
    return scene


def material(page, folder, emissive_strength):
    mat = bpy.data.materials.new(page)
    mat.use_nodes = True
    mat.use_backface_culling = page != "metal"   # chains and railings are seen from both sides
    nodes, links = mat.node_tree.nodes, mat.node_tree.links
    bsdf = next(n for n in nodes if n.type == "BSDF_PRINCIPLED")
    bsdf.inputs["Roughness"].default_value = 1.0
    bsdf.inputs["Metallic"].default_value = 0.0

    def image(name, colour=True):
        node = nodes.new("ShaderNodeTexImage")
        node.image = bpy.data.images.load(os.path.join(folder, name))
        node.interpolation = "Closest"
        if not colour:
            node.image.colorspace_settings.name = "Non-Color"
        return node

    base = image(f"{page}.png")
    links.new(base.outputs["Color"], bsdf.inputs["Base Color"])
    if page == "metal":  # chains and railings are alpha cutouts
        cut = nodes.new("ShaderNodeMath")
        cut.operation = "ROUND"
        links.new(base.outputs["Alpha"], cut.inputs[0])
        links.new(cut.outputs[0], bsdf.inputs["Alpha"])
    normal = nodes.new("ShaderNodeNormalMap")
    links.new(image(f"{page}_n.png", False).outputs["Color"], normal.inputs["Color"])
    links.new(normal.outputs["Normal"], bsdf.inputs["Normal"])
    if emissive_strength and os.path.exists(os.path.join(folder, f"{page}_e.png")):
        links.new(image(f"{page}_e.png").outputs["Color"], bsdf.inputs["Emission Color"])
        bsdf.inputs["Emission Strength"].default_value = emissive_strength
    return mat


def make_object(page, arrays, mat):
    P, N, UV, C, quads, tris = arrays
    me = bpy.data.meshes.new(page)
    me.vertices.add(len(P))
    me.vertices.foreach_set("co", np.stack([P[:, 0], -P[:, 2], P[:, 1]], axis=1).ravel())
    loops = np.concatenate([quads.ravel(), tris.ravel()])
    starts = np.concatenate([np.arange(len(quads)) * 4, len(quads) * 4 + np.arange(len(tris)) * 3])
    totals = np.concatenate([np.full(len(quads), 4), np.full(len(tris), 3)])
    me.loops.add(len(loops))
    me.loops.foreach_set("vertex_index", loops)
    me.polygons.add(len(starts))
    me.polygons.foreach_set("loop_start", starts)
    me.polygons.foreach_set("loop_total", totals)
    me.polygons.foreach_set("use_smooth", np.ones(len(starts), bool))
    me.update(calc_edges=True)
    uv = me.uv_layers.new(name="UVMap")
    uv.data.foreach_set("uv", np.stack([UV[loops, 0], 1.0 - UV[loops, 1]], axis=1).ravel())
    me.normals_split_custom_set_from_vertices(np.stack([N[:, 0], -N[:, 2], N[:, 1]], axis=1))
    colour = me.color_attributes.new("Col", "FLOAT_COLOR", "POINT")
    colour.data.foreach_set("color", np.ones(len(P) * 4))
    me.color_attributes.active_color = colour
    me.materials.append(mat)
    obj = bpy.data.objects.new(page, me)
    bpy.context.scene.collection.objects.link(obj)
    return obj


def bake_ao(objects, tints, samples=24, distance=3.0, strength=1.0, gamma=1.3, floor=0.12):
    scene = bpy.context.scene
    scene.cycles.samples = samples
    scene.world.light_settings.distance = distance
    scene.render.bake.target = "VERTEX_COLORS"
    for obj in objects:
        bpy.ops.object.select_all(action="DESELECT")
        obj.select_set(True)
        bpy.context.view_layer.objects.active = obj
        bpy.ops.object.bake(type="AO")
    for obj in objects:
        attribute = obj.data.color_attributes["Col"]
        data = np.zeros(len(obj.data.vertices) * 4)
        attribute.data.foreach_get("color", data)
        data = data.reshape(-1, 4)
        ao = np.clip(data[:, :3].mean(axis=1), 0, 1) ** gamma
        ao = floor + (1 - floor) * (1 - strength * (1 - ao))
        data[:, :3] = ao[:, None] * tints[obj.name]
        data[:, 3] = 1.0
        attribute.data.foreach_set("color", data.ravel())


def export(mesh, textures, out, ao=True, samples=24, emissive=None):
    reset()
    emissive = emissive or EMISSIVE
    objects, tints = [], {}
    for page in mesh.parts:
        arrays = mesh.arrays(page)
        obj = make_object(page, arrays, material(page, textures, emissive.get(page, 0.0)))
        objects.append(obj)
        tints[page] = arrays[3]
    if ao:
        bake_ao(objects, tints, samples=samples)
    else:
        for obj in objects:
            data = np.ones((len(obj.data.vertices), 4))
            data[:, :3] = tints[obj.name]
            obj.data.color_attributes["Col"].data.foreach_set("color", data.ravel())
    os.makedirs(os.path.dirname(out), exist_ok=True)
    bpy.ops.export_scene.gltf(filepath=out, export_format="GLB", export_yup=True, export_apply=False,
                              export_vertex_color="ACTIVE", export_all_vertex_colors=False,
                              export_active_vertex_color_when_no_material=True, export_materials="EXPORT",
                              export_normals=True, export_tangents=False, export_cameras=False, export_lights=False,
                              export_animations=False, export_extras=False)
    return out
