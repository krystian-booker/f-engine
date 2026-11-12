"""Blender-side helpers for model generators (run inside Blender, or with the
`bpy` module): inspect a mesh while it still has its real topology, clean it,
unwrap it at a fixed texel density, and export it the way the engine reads.

    import sys; sys.path.insert(0, "tools/modelkit/blender")
    import modelkit_bpy as mk

    problems = mk.inspect(obj)            # n-gons, non-manifold edges, loose parts, ...
    mk.clean(obj)                         # weld, drop degenerate and loose geometry, fix normals, triangulate
    mk.box_unwrap(obj, pixels_per_unit=32, texture_size=512)
    mk.export_glb("models/crate.glb", [obj])

The exported file is then judged by tools/modelkit/validate.py; these are the
steps that make it pass. `inspect` sees things the exported triangles no
longer show: real n-gons, modifiers that were never applied, transforms that
were never applied.
"""
import math

import bmesh
import bpy
from mathutils import Matrix, Vector


def inspect(obj, ngon_sides=4):
    """What is wrong with a mesh object before export, as a list of
    (severity, message). Empty when it is clean."""
    out = []
    if obj.type != "MESH":
        return [("error", f"{obj.name} is a {obj.type}, not a mesh")]
    for modifier in obj.modifiers:
        if modifier.type == "BOOLEAN":
            out.append(("error", f"{obj.name}: boolean modifier '{modifier.name}' is not applied; apply it and clean the cut"))
        elif modifier.type not in ("ARMATURE",):
            out.append(("warn", f"{obj.name}: modifier '{modifier.name}' ({modifier.type}) will be applied by the export; check its result"))
    scale = obj.matrix_world.to_scale()
    if max(abs(scale.x - 1), abs(scale.y - 1), abs(scale.z - 1)) > 1e-4:
        out.append(("warn", f"{obj.name}: scale {tuple(round(v, 3) for v in scale)} is not applied"))
    bm = bmesh.new()
    bm.from_mesh(obj.data)
    bm.verts.ensure_lookup_table()
    bm.edges.ensure_lookup_table()
    bm.faces.ensure_lookup_table()
    ngons = [f for f in bm.faces if len(f.verts) > ngon_sides]
    if ngons:
        out.append(("warn", f"{obj.name}: {len(ngons)} n-gons (the largest has {max(len(f.verts) for f in ngons)} sides); "
                            "cut them into quads so the triangulation is deliberate"))
    concave = [f for f in ngons if any(loop.is_convex is False for loop in f.loops)]
    if concave:
        out.append(("warn", f"{obj.name}: {len(concave)} of them are concave, which triangulate badly"))
    non_manifold = [e for e in bm.edges if len(e.link_faces) > 2]
    if non_manifold:
        out.append(("error", f"{obj.name}: {len(non_manifold)} edges shared by three or more faces (internal faces or fused parts)"))
    wire = [e for e in bm.edges if not e.link_faces]
    loose = [v for v in bm.verts if not v.link_faces]
    if wire or loose:
        out.append(("error", f"{obj.name}: {len(loose)} loose vertices and {len(wire)} loose edges"))
    flat = [f for f in bm.faces if f.calc_area() < 1e-10]
    if flat:
        out.append(("error", f"{obj.name}: {len(flat)} zero-area faces"))
    doubles = bmesh.ops.find_doubles(bm, verts=bm.verts, dist=1e-5)["targetmap"]
    if doubles:
        out.append(("warn", f"{obj.name}: {len(doubles)} vertices coincide with another; merge by distance"))
    if not obj.data.uv_layers:
        out.append(("warn", f"{obj.name}: no UV map"))
    if not obj.data.materials or any(m is None for m in obj.data.materials):
        out.append(("warn", f"{obj.name}: an empty material slot"))
    bm.free()
    return out


def clean(obj, merge_distance=1e-5, sharp_angle=None, triangulate=True, recalculate_normals=True):
    """Make a mesh exportable: weld coincident vertices, dissolve degenerate
    geometry, delete what no face uses, face the normals outwards, and
    triangulate (quads along their shorter diagonal, n-gons by 'beauty') so the engine
    gets the triangles this function chose. `sharp_angle` (degrees) marks
    edges sharper than it, so smooth shading stops there."""
    bm = bmesh.new()
    bm.from_mesh(obj.data)
    bmesh.ops.remove_doubles(bm, verts=bm.verts, dist=merge_distance)
    bmesh.ops.dissolve_degenerate(bm, edges=bm.edges, dist=merge_distance)
    loose_edges = [e for e in bm.edges if not e.link_faces]
    if loose_edges:
        bmesh.ops.delete(bm, geom=loose_edges, context="EDGES")
    loose_verts = [v for v in bm.verts if not v.link_faces]
    if loose_verts:
        bmesh.ops.delete(bm, geom=loose_verts, context="VERTS")
    flat = [f for f in bm.faces if f.calc_area() < 1e-12]
    if flat:
        bmesh.ops.delete(bm, geom=flat, context="FACES")
    if recalculate_normals:
        bmesh.ops.recalc_face_normals(bm, faces=bm.faces)
    if sharp_angle is not None:
        limit = math.radians(sharp_angle)
        for edge in bm.edges:
            if len(edge.link_faces) == 2:
                edge.smooth = edge.calc_face_angle(0.0) < limit
            else:
                edge.smooth = False
        for face in bm.faces:
            face.smooth = True
    if triangulate:
        bmesh.ops.triangulate(bm, faces=bm.faces, quad_method="SHORT_EDGE", ngon_method="BEAUTY")
    bm.to_mesh(obj.data)
    bm.free()
    obj.data.update()
    return obj


def apply_transforms(obj, location=False):
    """Bake the object's rotation and scale (and location, if asked) into its
    mesh, so sizes in the file are sizes in the game."""
    matrix = obj.matrix_world.copy()
    if not location:
        translation = matrix.to_translation()
        matrix.translation = Vector((0, 0, 0))
    obj.data.transform(matrix)
    obj.matrix_world.identity()
    if not location:
        obj.matrix_world.translation = translation
    if matrix.determinant() < 0:
        obj.data.flip_normals()
    obj.data.update()


def box_unwrap(obj, pixels_per_unit, texture_size, uv_name="UVMap"):
    """Project every face along its dominant axis at a fixed texel density:
    `pixels_per_unit` texels of a `texture_size` texture per world unit. For
    tiling pixel-art textures; UVs run outside 0..1 on purpose."""
    mesh = obj.data
    layer = mesh.uv_layers.get(uv_name) or mesh.uv_layers.new(name=uv_name)
    scale = pixels_per_unit / float(texture_size)
    world = obj.matrix_world
    for polygon in mesh.polygons:
        normal = (world.to_3x3() @ polygon.normal).normalized()
        axis = max(range(3), key=lambda i: abs(normal[i]))
        u_axis, v_axis = ((1, 2), (0, 2), (0, 1))[axis]
        for loop_index in polygon.loop_indices:
            position = world @ mesh.vertices[mesh.loops[loop_index].vertex_index].co
            layer.data[loop_index].uv = (position[u_axis] * scale, position[v_axis] * scale)
    return layer


def texel_density(obj, texture_size, uv_name=None):
    """Median pixels per world unit of an object's UVs for a texture of
    `texture_size` (a number, or (width, height))."""
    mesh = obj.data
    layer = mesh.uv_layers.get(uv_name) if uv_name else mesh.uv_layers.active
    if layer is None:
        return 0.0
    width, height = (texture_size, texture_size) if isinstance(texture_size, (int, float)) else texture_size
    world = obj.matrix_world
    values = []
    for polygon in mesh.polygons:
        loops = list(polygon.loop_indices)
        points = [world @ mesh.vertices[mesh.loops[i].vertex_index].co for i in loops]
        uvs = [layer.data[i].uv for i in loops]
        area = sum(((points[i] - points[0]).cross(points[i + 1] - points[0])).length for i in range(1, len(points) - 1)) / 2
        uv_area = abs(sum((uvs[i].x - uvs[0].x) * (uvs[i + 1].y - uvs[0].y) - (uvs[i].y - uvs[0].y) * (uvs[i + 1].x - uvs[0].x)
                          for i in range(1, len(uvs) - 1))) / 2
        if area > 1e-12 and uv_area > 0:
            values.append((math.sqrt(uv_area * width * height / area), area))
    if not values:
        return 0.0
    values.sort()
    half, running = sum(area for _, area in values) / 2, 0.0
    for density, area in values:
        running += area
        if running >= half:
            return density
    return values[-1][0]


def set_origin(obj, where="base_centre"):
    """Move the mesh so the object's origin is at the centre of its base
    (props, characters), its centre (pickups), or left alone ("keep")."""
    if where == "keep":
        return
    points = [vertex.co for vertex in obj.data.vertices]
    if not points:
        return
    low = Vector((min(p.x for p in points), min(p.y for p in points), min(p.z for p in points)))
    high = Vector((max(p.x for p in points), max(p.y for p in points), max(p.z for p in points)))
    centre = (low + high) / 2
    # Blender is Z up: the base is the lowest Z.
    offset = Vector((centre.x, centre.y, low.z if where == "base_centre" else centre.z))
    obj.data.transform(Matrix.Translation(-offset))
    obj.data.update()


def export_glb(path, objects=None, animations=True, apply_modifiers=True):
    """Export the given objects (or the whole scene) as a .glb the engine
    loads: Y up, triangles with normals and UVs, materials with embedded PNG
    textures, skins limited to four influences, every action as a named
    animation; no Draco, no morph targets, no cameras or lights."""
    if objects is not None:
        bpy.ops.object.select_all(action="DESELECT")
        for obj in objects:
            obj.select_set(True)
        bpy.context.view_layer.objects.active = objects[0]
    options = dict(
        filepath=path, export_format="GLB", export_yup=True, use_selection=objects is not None,
        export_apply=apply_modifiers, export_normals=True, export_texcoords=True, export_tangents=False,
        export_materials="EXPORT", export_image_format="AUTO",
        export_draco_mesh_compression_enable=False, export_morph=False,
        export_cameras=False, export_lights=False, export_extras=True,
        export_skins=True, export_influence_nb=4, export_all_influences=False,
        export_animations=animations, export_animation_mode="NLA_TRACKS", export_force_sampling=True,
    )
    # Blender renames export options between versions: pass only the ones this one knows.
    known = bpy.ops.export_scene.gltf.get_rna_type().properties.keys()
    bpy.ops.export_scene.gltf(**{key: value for key, value in options.items() if key in known})
    return path


def prepare(objects, sharp_angle=40, origin="keep", report=print):
    """inspect, clean and apply transforms on each object; reports what
    inspect found before cleaning and what is still wrong after it. Returns
    True when nothing is left that export should not see."""
    ok = True
    for obj in objects:
        before = inspect(obj)
        apply_transforms(obj)
        clean(obj, sharp_angle=sharp_angle)
        set_origin(obj, origin)
        after = [entry for entry in inspect(obj) if "n-gons" not in entry[1]]
        for severity, message in before:
            if (severity, message) not in after:
                report(f"  cleaned: {message}")
        for severity, message in after:
            report(f"  {severity}: {message}")
            ok = ok and severity != "error"
    return ok
