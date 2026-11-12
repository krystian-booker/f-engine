"""The Blender half of the self-test (python3 tools/animkit/selftest.py
--blender): a small rig is given actions with the faults the helpers exist
for; each helper must repair its fault, and the export must come out of
animkit's validator clean, with every action under its own name."""
import math
import os

import numpy as np


def _q(degrees, axis=(0, 1, 0)):
    half = math.radians(degrees) / 2
    return (math.cos(half), axis[0] * math.sin(half), axis[1] * math.sin(half), axis[2] * math.sin(half))


def _rig(bpy):
    """An armature (root, arm, hand) with a box on each bone, standing on the ground, in a fresh scene."""
    import bmesh
    from mathutils import Matrix
    bpy.ops.wm.read_factory_settings(use_empty=True)
    scene = bpy.context.scene
    scene.render.fps = 30
    armature = bpy.data.armatures.new("golem")
    rig = bpy.data.objects.new("golem", armature)
    scene.collection.objects.link(rig)
    bpy.context.view_layer.objects.active = rig
    bpy.ops.object.mode_set(mode="EDIT")
    bones = {}
    for name, parent, head, tail in (("root", None, (0, 0, 0), (0, 0, 1.0)), ("arm", "root", (0.3, 0, 1.2), (1.0, 0, 1.2)),
                                     ("hand", "arm", (1.0, 0, 1.2), (1.4, 0, 1.2))):
        bone = armature.edit_bones.new(name)
        bone.head, bone.tail = head, tail
        if parent:
            bone.parent = bones[parent]
        bones[name] = bone
    bpy.ops.object.mode_set(mode="OBJECT")
    mesh = bpy.data.meshes.new("golem_mesh")
    bm = bmesh.new()
    owners = []
    for bone, centre, size in (("root", (0, 0, 0.75), (0.6, 0.4, 1.5)), ("arm", (0.65, 0, 1.2), (0.7, 0.2, 0.2)),
                               ("hand", (1.2, 0, 1.2), (0.4, 0.3, 0.3))):
        made = bmesh.ops.create_cube(bm, size=1.0, matrix=Matrix.Translation(centre) @ Matrix.Diagonal((*size, 1.0)))
        owners += [(vertex.index, bone) for vertex in made["verts"]]
    bm.verts.index_update()
    owners = [(vertex.index, bone) for vertex, (_, bone) in zip(bm.verts, owners)]
    bmesh.ops.triangulate(bm, faces=bm.faces)
    bm.to_mesh(mesh)
    bm.free()
    body = bpy.data.objects.new("golem_mesh", mesh)
    scene.collection.objects.link(body)
    material = bpy.data.materials.new("clay")
    mesh.materials.append(material)
    for bone in ("root", "arm", "hand"):
        group = body.vertex_groups.new(name=bone)
        group.add([index for index, owner in owners if owner == bone], 1.0, "REPLACE")
    body.parent = rig
    body.modifiers.new("armature", "ARMATURE").object = rig
    for bone in rig.pose.bones:
        bone.rotation_mode = "QUATERNION"
    return rig, body


def _keyed(bpy, ak, rig, name, keys, interpolation="BEZIER"):
    """An action on the arm's rotation with the given (frame, quaternion) keys."""
    action = ak.new_action(rig, name)
    bone = rig.pose.bones["arm"]
    for frame, value in keys:
        bone.rotation_quaternion = value
        bone.keyframe_insert("rotation_quaternion", frame=frame)
    for curve in ak.fcurves(action):
        for point in curve.keyframe_points:
            point.interpolation = interpolation
            point.handle_left_type = point.handle_right_type = "AUTO_CLAMPED"
        curve.update()
    return action


def run(kit, work, report, profile):
    print("Blender helpers")
    import bpy
    import animkit_bpy as ak
    spec = {"idle": {"loop": True, "seconds": 2, "grounded": True, "in_place": True},
            "wave": {"loop": False, "seconds": 1, "from": "idle", "to": "idle", "grounded": True, "in_place": True}}

    def said(messages, word, severities=("error", "warn")):
        return any(word in message for severity, message in messages if severity in severities)

    def faulty():
        rig, body = _rig(bpy)
        # idle: ends 15 degrees from where it starts, and its middle key is on the far side of the sphere.
        idle = _keyed(bpy, ak, rig, "idle", [(0, _q(0)), (30, tuple(-v for v in _q(25))), (60, _q(15))])
        # wave: three Bezier keys, starting at frame 10.
        wave = _keyed(bpy, ak, rig, "wave", [(10, _q(0)), (22, _q(-70)), (40, _q(0))])
        return rig, body, idle, wave

    def validate(path, **overrides):
        findings, _, subject = kit.validate(path, profile("character", spec, **overrides))
        return sorted({f.check for f in findings if f.severity != "info"}), subject

    # 1. What the faults do when they are exported as they are.
    rig, body, idle, wave = faulty()
    before = ak.check_action(idle, spec["idle"])
    report("check_action reports a loop that does not close", [] if said(before, "does not end where it starts") else ["not reported"])
    report("check_action reports quaternions on opposite sides", [] if said(before, "long way round") else ["not reported"])
    report("check_action reports an action that starts late",
           [] if said(ak.check_action(wave, spec["wave"]), "first key is at frame") else ["not reported"])
    ak.push_to_nla(rig)
    raw = os.path.join(work, "golem_raw.glb")
    ak.export_glb(raw, [rig, body])
    bad, subject = validate(raw)
    report("the unrepaired loop is caught by the validator", [] if "loops.seam_pose" in bad else [f"found {bad}"])
    names = sorted(take.name for take in subject.takes)
    report("push_to_nla and export_glb give every action its name", [] if names == ["idle", "wave"] else [f"names {names}"])
    report("the export starts every animation at time 0", [] if all(take.clip.start < 1e-6 for take in subject.takes) else ["late start"])

    # 2. The same faults, repaired.
    rig, body, idle, wave = faulty()
    flipped = ak.fix_quaternion_flips(idle)
    report("fix_quaternion_flips puts the far key back",
           [] if flipped == 1 and not said(ak.check_action(idle), "long way round") else [f"{flipped} flipped"])
    changed = ak.make_loop(idle)
    after = ak.check_action(idle, spec["idle"])
    report("make_loop closes the loop and matches its tangents",
           [] if changed and not said(after, "does not end") and not said(after, "changes speed") else [str(after)])
    count = ak.bake(idle, 30) + ak.bake(wave, 30)
    linear = all(point.interpolation == "LINEAR" for action in (idle, wave) for curve in ak.fcurves(action)
                 for point in curve.keyframe_points)
    per_curve = sorted({len(curve.keyframe_points) for curve in ak.fcurves(idle)})
    report("bake writes one linear key per frame", [] if linear and per_curve == [61] and count == 4 * 61 + 4 * 31 else [f"{per_curve} keys, {count}"])
    unit = max(abs(math.sqrt(sum(curve.keyframe_points[i].co[1] ** 2 for curve in ak.fcurves(idle))) - 1) for i in range(61))
    report("baked quaternions are unit length", [] if unit < 1e-5 else [f"off by {unit}"])
    remarks = [m for m in ak.check_action(idle, spec["idle"]) + ak.check_action(wave, spec["wave"])
               if m[0] != "info" and "first key" not in m[1]]      # wave still starts at frame 10; the sampled export slides it
    report("check_action passes the repaired actions", [] if not remarks else [str(remarks)])
    ak.push_to_nla(rig)
    good = os.path.join(work, "golem.glb")
    ak.export_glb(good, [rig, body])
    left, subject = validate(good)
    report("the repaired, baked export validates clean", [] if not left else [f"found {left}"])
    sampled_keys = sum(take.clip.keys for take in subject.takes)
    reference = {take.name: take.points for take in subject.takes}

    # 3. Curves cleaned, exported with the keys as they are.
    samples = {curve.array_index: [point.co[1] for point in curve.keyframe_points] for curve in ak.fcurves(idle)}
    removed = ak.clean_curves(idle) + ak.clean_curves(wave)
    worst = 0.0
    for curve in ak.fcurves(idle):
        worst = max(worst, max(abs(curve.evaluate(frame) - samples[curve.array_index][frame]) for frame in range(61)))
    report("clean_curves drops keys and keeps the curve", [] if removed > 100 and worst < 2e-3 else [f"removed {removed}, off by {worst:.4f}"])
    moved = ak.slide_to_zero(wave)
    report("slide_to_zero moves the action to frame 0", [] if moved == 10 and ak.frame_range(wave) == (0.0, 30.0) else [f"moved {moved}"])
    small = os.path.join(work, "golem_small.glb")
    ak.export_glb(small, [rig, body], sampled=False)
    left, subject = validate(small)
    keys = sum(take.clip.keys for take in subject.takes)
    drift = max(float(np.abs(take.points - reference[take.name]).max()) for take in subject.takes) if not left else 9.0
    report("the cleaned export is smaller and moves the same",
           [] if not left and keys < sampled_keys * 0.6 and drift < 0.01 else [f"found {left}, {keys} keys against {sampled_keys}, off by {drift:.3f}"])

    # 4. key_action: a generator's poses, with a quaternion that wanders across the sphere.
    rig, body = _rig(bpy)
    frames = []
    for index in range(61):
        value = _q(20 * math.sin(2 * math.pi * index / 60))
        frames.append({"arm": {"rotation": tuple(-v for v in value) if index % 7 == 3 else tuple(v * 1.5 for v in value)},
                       "hand": {"rotation": _q(10 * math.sin(2 * math.pi * index / 60), (0, 0, 1)), "scale": 1.0}})
    action = ak.key_action(rig, "idle", frames)
    problems = [m for m in ak.check_action(action, spec["idle"]) if m[0] != "info"]
    report("key_action keys unit quaternions on one side, linear", [] if not problems else [str(problems)])
    ak.push_to_nla(rig)
    keyed = os.path.join(work, "golem_keyed.glb")
    ak.export_glb(keyed, [rig, body])
    findings, _, _ = kit.validate(keyed, profile("character", {"idle": spec["idle"]}))
    left = sorted({f.check for f in findings if f.severity != "info"})
    report("an action keyed with key_action validates clean", [] if not left else [f"found {left}"])
