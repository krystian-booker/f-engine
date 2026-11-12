"""Blender-side helpers for animation generators (run inside Blender, or with
the `bpy` module): key actions cleanly, repair the usual faults while the
action still has its real curves, and export so that every action arrives in
the engine under its own name.

    import sys; sys.path.insert(0, "tools/animkit/blender")
    import animkit_bpy as ak

    action = ak.key_action(armature, "idle", frames)       # one clean key per frame from a generator's poses
    ak.fix_quaternion_flips(action)                        # hand-keyed curves: no long way round
    ak.make_loop(action)                                   # last key = first key, tangents matched
    ak.bake(action, 30)                                    # sample the curves: eases survive the engine's linear playback
    for severity, message in ak.check_action(action, {"loop": True, "seconds": 3}):
        print(severity, message)
    ak.push_to_nla(armature)                               # one NLA track per action, named after it
    ak.export_glb("samples/<game>/models/golem.glb", [armature, mesh])

The exported file is then judged by tools/animkit/validate.py; these are the
steps that make it pass. Written against Blender 4.4 and later, where an
action's F-curves live in a channelbag per slot (`action.fcurves` is gone in
5.x); older, legacy actions are read through `action.fcurves`. Tested with
bpy 5.3 (python3 tools/animkit/selftest.py --blender).
"""
import math
import re

import bpy

QUATERNION = "rotation_quaternion"


# ------------------------------------------------------------------ curves

def fcurves(action):
    """Every F-curve of an action: all its layers, strips and slots (4.4+), or the legacy list."""
    if hasattr(action, "layers"):
        found = []
        for layer in action.layers:
            for strip in layer.strips:
                for slot in action.slots:
                    bag = strip.channelbag(slot)
                    if bag is not None:
                        found.extend(bag.fcurves)
        return found
    return list(action.fcurves)


def groups(action):
    """{data_path: [curves ordered by array index]}: the components of one property together."""
    out = {}
    for curve in fcurves(action):
        out.setdefault(curve.data_path, []).append(curve)
    for curves in out.values():
        curves.sort(key=lambda curve: curve.array_index)
    return out


def _frames(curve):
    return [point.co[0] for point in curve.keyframe_points]


def _values(curve):
    return [point.co[1] for point in curve.keyframe_points]


def _aligned(curves):
    """True when the curves of one property are keyed on the same frames (so a key is one value of the property)."""
    first = _frames(curves[0])
    return all(len(curve.keyframe_points) == len(first) and
               all(abs(a - b) < 1e-4 for a, b in zip(_frames(curve), first)) for curve in curves[1:])


def _rewrite(curve, frames, values, interpolation="LINEAR"):
    """Replace a curve's keys."""
    curve.keyframe_points.clear()
    curve.keyframe_points.add(len(frames))
    flat = [0.0] * (2 * len(frames))
    flat[0::2], flat[1::2] = frames, values
    curve.keyframe_points.foreach_set("co", flat)
    for point in curve.keyframe_points:
        point.interpolation = interpolation
        point.handle_left_type = point.handle_right_type = "AUTO_CLAMPED"
    curve.update()


def frame_range(action):
    """(first, last) frame that any curve of the action keys."""
    frames = [frame for curve in fcurves(action) for frame in _frames(curve)]
    return (min(frames), max(frames)) if frames else (0.0, 0.0)


# ------------------------------------------------------------------ authoring

def new_action(obj, name):
    """A new action assigned to `obj` (with its own slot on 4.4+), ready to receive curves."""
    data = obj.animation_data or obj.animation_data_create()
    action = bpy.data.actions.new(name)
    data.action = action
    if hasattr(action, "slots"):
        slot = action.slots.new(id_type="OBJECT", name=obj.name)
        data.action_slot = slot
    return action


def _bag(obj, action):
    """Where the curves of `obj` in `action` are created."""
    if hasattr(action, "slots"):
        from bpy_extras import anim_utils
        slot = obj.animation_data.action_slot if obj.animation_data and obj.animation_data.action == action else None
        if slot is None:
            slot = next((s for s in action.slots if s.target_id_type in ("OBJECT", "UNSPECIFIED")), None) or \
                action.slots.new(id_type="OBJECT", name=obj.name)
        return anim_utils.action_ensure_channelbag_for_slot(action, slot)
    return action


def _new_curve(bag, path, index, group):
    if hasattr(bag, "bl_rna") and bag.bl_rna.identifier == "Action":      # legacy
        return bag.fcurves.new(path, index=index, action_group=group)
    return bag.fcurves.new(path, index=index, group_name=group)


def key_action(obj, name, frames, start=0):
    """Key an action from a generator's poses: `frames` is a list (one entry
    per frame, from frame `start`) of {bone name: {"location": (x, y, z),
    "rotation": (w, x, y, z), "scale": s or (x, y, z)}}; a name of None keys
    the object itself. Every key is LINEAR (the poses are the samples),
    quaternions are normalised and kept on one side of the sphere so nothing
    turns the long way round, and bones are put in quaternion mode.
    Returns the action, assigned to `obj`."""
    action = new_action(obj, name)
    bag = _bag(obj, action)
    tracks = {}
    last = {}
    for index, pose in enumerate(frames):
        for bone, values in pose.items():
            for prop, value in values.items():
                if prop == "rotation":
                    length = math.sqrt(sum(v * v for v in value)) or 1.0
                    value = [v / length for v in value]
                    before = last.get(bone)
                    if before is not None and sum(a * b for a, b in zip(before, value)) < 0:
                        value = [-v for v in value]
                    last[bone] = value
                elif prop == "scale" and not hasattr(value, "__len__"):
                    value = (value, value, value)
                tracks.setdefault((bone, prop), []).append((start + index, tuple(value)))
    paths = {"location": "location", "rotation": QUATERNION, "scale": "scale"}
    for (bone, prop), keys in tracks.items():
        if bone is not None:
            obj.pose.bones[bone].rotation_mode = "QUATERNION"
            path = f'pose.bones["{bone}"].{paths[prop]}'
        else:
            obj.rotation_mode = "QUATERNION"
            path = paths[prop]
        for component in range(len(keys[0][1])):
            curve = _new_curve(bag, path, component, bone or obj.name)
            _rewrite(curve, [frame for frame, _ in keys], [value[component] for _, value in keys])
    return action


def bake(action, fps=None, scene=None):
    """Sample every curve of the action `fps` times a second (default: the
    scene's frame rate) between its first and last key, replacing its keys
    with LINEAR ones. The engine plays keys with linear interpolation, so the
    eases of Bezier curves reach the game only as samples. Quaternions are
    renormalised. Returns the number of keys written.

    This samples the curves themselves; a rig driven by constraints or IK
    needs Blender's visual bake first (bpy_extras.anim_utils.bake_action)."""
    scene = scene or bpy.context.scene
    step = (scene.render.fps / scene.render.fps_base) / float(fps or scene.render.fps)
    first, last = frame_range(action)
    count = max(1, int(round((last - first) / step)))
    frames = [first + index * step for index in range(count)] + [last]
    written = 0
    for path, curves in groups(action).items():
        sampled = [[curve.evaluate(frame) for frame in frames] for curve in curves]
        if path.endswith(QUATERNION) and len(curves) == 4:
            for index in range(len(frames)):
                length = math.sqrt(sum(values[index] ** 2 for values in sampled)) or 1.0
                for values in sampled:
                    values[index] /= length
        for curve, values in zip(curves, sampled):
            _rewrite(curve, frames, values)
            written += len(frames)
    return written


def fix_quaternion_flips(action):
    """Negate quaternion keys that sit on the far side of the sphere from the
    key before them (q and -q are the same rotation, but Blender interpolates
    between them the long way round, and a bake then samples that). Returns
    the number of keys negated. Curves of one bone keyed on different frames
    are left alone: bake first."""
    flipped = 0
    for path, curves in groups(action).items():
        if not path.endswith(QUATERNION) or len(curves) != 4 or not _aligned(curves):
            continue
        points = [list(curve.keyframe_points) for curve in curves]
        for index in range(1, len(points[0])):
            # The key before has already been put right, so each key only has to agree with it.
            if sum(points[c][index - 1].co[1] * points[c][index].co[1] for c in range(4)) < 0:
                for c in range(4):
                    point = points[c][index]
                    point.co[1], point.handle_left[1], point.handle_right[1] = -point.co[1], -point.handle_left[1], -point.handle_right[1]
                flipped += 1
        for curve in curves:
            curve.update()
    return flipped


def make_loop(action):
    """Make the action loop: on every curve the last key takes the first
    key's value (a key is added at the action's last frame where a curve has
    none there), and where the keys are Bezier the tangents at both ends are
    set to one slope, the slope of the curve running through the seam, so the
    motion does not hitch as the loop turns. Returns the number of curves
    changed. The engine wraps from the last key to time 0 and never shows the
    last key itself, so the copy does not stall the loop."""
    _, end = frame_range(action)
    changed = 0
    for curve in fcurves(action):
        points = curve.keyframe_points
        if len(points) < 2:
            continue
        if points[-1].co[0] < end - 1e-4:
            points.insert(end, points[0].co[1])
        first, last = points[0], points[-1]
        before = (last.co[1], tuple(last.handle_left), tuple(first.handle_right))
        last.co[1] = first.co[1]
        if first.interpolation == "BEZIER" and len(points) >= 3:
            after, ahead = points[1], points[-2]
            run_out, run_in = after.co[0] - first.co[0], last.co[0] - ahead.co[0]
            slope = (after.co[1] - ahead.co[1]) / (run_out + run_in)
            for point in (first, last):
                point.handle_left_type = point.handle_right_type = "FREE"
                point.handle_right = (point.co[0] + run_out / 3, point.co[1] + slope * run_out / 3)
                point.handle_left = (point.co[0] - run_in / 3, point.co[1] - slope * run_in / 3)
        if before != (last.co[1], tuple(last.handle_left), tuple(first.handle_right)):
            changed += 1
        curve.update()
    if hasattr(action, "use_cyclic"):
        action.use_frame_range = True
        action.frame_start, action.frame_end = frame_range(action)
        action.use_cyclic = True
    return changed


def clean_curves(action, tolerance=0.0002, degrees=0.05):
    """Drop the keys that say nothing: those linear interpolation between the
    keys kept around them reproduces within `tolerance` (in the property's
    own units; for quaternions `degrees`). The components of one property keep
    the same key frames. For baked (LINEAR) curves; Bezier keys are left
    alone. Returns the number of keys removed.

    The sampled export (export_glb's default) writes a key per frame whatever
    the curves hold; cleaned curves pay off with export_glb(sampled=False)."""
    removed = 0
    for path, curves in groups(action).items():
        if not _aligned(curves) or len(curves[0].keyframe_points) < 3 or \
                any(point.interpolation != "LINEAR" for curve in curves for point in curve.keyframe_points):
            continue
        frames = _frames(curves[0])
        values = [_values(curve) for curve in curves]
        rotation = path.endswith(QUATERNION) and len(curves) == 4

        def error(a, b, index):
            t = (frames[index] - frames[a]) / (frames[b] - frames[a])
            between = [v[a] + (v[b] - v[a]) * t for v in values]
            if rotation:
                length = math.sqrt(sum(x * x for x in between)) or 1.0
                dot = abs(sum(x / length * v[index] for x, v in zip(between, values)))
                return math.degrees(2 * math.acos(min(1.0, dot)))
            return max(abs(x - v[index]) for x, v in zip(between, values))

        limit = degrees if rotation else tolerance
        keep, a = [0], 0
        while a < len(frames) - 1:
            b = a + 1
            while b + 1 < len(frames) and all(error(a, b + 1, index) <= limit for index in range(a + 1, b + 1)):
                b += 1
            keep.append(b)
            a = b
        if len(keep) < len(frames):
            removed += (len(frames) - len(keep)) * len(curves)
            for curve, series in zip(curves, values):
                _rewrite(curve, [frames[index] for index in keep], [series[index] for index in keep])
    return removed


def slide_to_zero(action):
    """Move every key so the action starts at frame 0 (the engine's clock
    does). Returns how many frames it moved."""
    first, _ = frame_range(action)
    if abs(first) < 1e-6:
        return 0.0
    for curve in fcurves(action):
        for point in curve.keyframe_points:
            point.co[0] -= first
            point.handle_left[0] -= first
            point.handle_right[0] -= first
        curve.update()
    if getattr(action, "use_frame_range", False):
        action.frame_start, action.frame_end = action.frame_start - first, action.frame_end - first
    return first


# ------------------------------------------------------------------ inspection

def check_action(action, spec=None, fps=None):
    """What is wrong with an action before export, as a list of (severity,
    message); empty when it is clean. `spec` is the animation's entry in the
    manifest ({"loop": true, "seconds": 3, ...}): with it the loop's seam and
    the length are checked too. It sees what the exported samples no longer
    show: which way round Blender will interpolate, and curves that are still
    Bezier."""
    spec = spec or {}
    fps = fps or bpy.context.scene.render.fps / bpy.context.scene.render.fps_base
    out = []
    curves = fcurves(action)
    if not curves or all(len(curve.keyframe_points) == 0 for curve in curves):
        return [("error", f"{action.name}: no keys")]
    if re.fullmatch(r"(Action|.*Action)(\.\d+)?", action.name) or re.search(r"\.\d{3}$", action.name):
        out.append(("warn", f"{action.name}: a default name; name the action for what the game plays (idle, attack)"))
    first, last = frame_range(action)
    if any(math.isnan(value) or math.isinf(value) for curve in curves for value in _values(curve)):
        out.append(("error", f"{action.name}: keys that are not numbers"))
        return out
    if abs(first) > 1e-4:
        out.append(("warn", f"{action.name}: its first key is at frame {first:g}, not 0 (slide_to_zero; the sampled export slides it for you)"))
    seconds = spec.get("seconds")
    if seconds is not None:
        length = (last - first) / fps
        low, high = seconds if isinstance(seconds, (list, tuple)) else (seconds - 0.05, seconds + 0.05)
        if not low <= length <= high:
            out.append(("error", f"{action.name}: {length:.2f} s long; its spec says {seconds}"))
    bezier = sum(1 for curve in curves if any(point.interpolation not in ("LINEAR", "CONSTANT") for point in curve.keyframe_points))
    if bezier:
        out.append(("info", f"{action.name}: {bezier} curves are not LINEAR; the sampled export bakes them, export_glb(sampled=False) "
                            "would write cubic splines the engine plays as straight lines (bake first)"))
    for path, group in groups(action).items():
        if path.endswith(QUATERNION) and len(group) == 4:
            if not _aligned(group):
                out.append(("warn", f"{action.name}: {path} is keyed on different frames per component (bake it)"))
                continue
            keys = list(zip(*[_values(curve) for curve in group]))
            lengths = [math.sqrt(sum(v * v for v in key)) for key in keys]
            if any(abs(length - 1) > 0.002 for length in lengths):
                out.append(("error", f"{action.name}: {path} has keys that are not unit quaternions (off by up to "
                                     f"{max(abs(length - 1) for length in lengths):.3f})"))
            flips, wide = 0, 0.0
            for a, b in zip(keys, keys[1:]):
                dot = sum(x * y for x, y in zip(a, b)) / ((math.sqrt(sum(x * x for x in a)) * math.sqrt(sum(y * y for y in b))) or 1.0)
                flips += dot < 0
                wide = max(wide, math.degrees(2 * math.acos(min(1.0, abs(dot)))))
            if flips:
                out.append(("warn", f"{action.name}: {path} changes sign between {flips} pairs of keys, so Blender interpolates "
                                    "the long way round (fix_quaternion_flips)"))
            if wide > 140:
                out.append(("warn", f"{action.name}: {path} turns {wide:.0f} degrees between two keys; key the turn in steps of 90 or less"))
        if path.endswith("scale"):
            values = [value for curve in group for value in _values(curve)]
            if min(values) < 0:
                out.append(("error", f"{action.name}: {path} has negative scale keys"))
            elif min(values) == 0:
                out.append(("warn", f"{action.name}: {path} has scale keys of exactly 0 (use 0.0001)"))
        if spec.get("loop"):
            worst, jolt = 0.0, 0.0
            for curve in group:
                points = curve.keyframe_points
                if len(points) < 2:
                    continue
                worst = max(worst, abs(points[-1].co[1] - points[0].co[1]), abs(curve.evaluate(last) - curve.evaluate(first)))
                slopes = [abs(curve.evaluate(frame + 0.5) - curve.evaluate(frame - 0.5)) for frame in range(int(first) + 1, int(last))]
                leaving = (curve.evaluate(first + 0.5) - curve.evaluate(first)) * 2
                arriving = (curve.evaluate(last) - curve.evaluate(last - 0.5)) * 2
                if slopes and max(slopes) > 1e-6:
                    jolt = max(jolt, abs(leaving - arriving) / max(slopes))
            if worst > 1e-4:
                out.append(("error", f"{action.name}: {path} does not end where it starts (off by {worst:.4g}); a loop pops there (make_loop)"))
            elif jolt > 0.5:
                out.append(("warn", f"{action.name}: {path} meets itself at the seam but changes speed there (make_loop matches the tangents)"))
    return out


# ------------------------------------------------------------------ export

def push_to_nla(obj, actions=None):
    """Park each action on its own NLA track named after it (one strip, from
    the action's first frame), and leave no action assigned, so the exporter
    writes one animation per action, named for the game. `actions` defaults
    to every action that has curves for this object. Tracks the object
    already has are replaced. Returns the track names."""
    data = obj.animation_data or obj.animation_data_create()
    if actions is None:
        actions = [action for action in bpy.data.actions if _belongs(action, obj)]
    for track in list(data.nla_tracks):
        data.nla_tracks.remove(track)
    data.action = None
    names = []
    for action in actions:
        track = data.nla_tracks.new()
        track.name = action.name
        strip = track.strips.new(action.name, int(math.floor(frame_range(action)[0])), action)
        if hasattr(strip, "action_slot") and strip.action_slot is None and hasattr(action, "slots") and len(action.slots):
            strip.action_slot = action.slots[0]
        names.append(track.name)
    return names


def _belongs(action, obj):
    """Does the action animate this object (its bones, or its own transform)?"""
    paths = [curve.data_path for curve in fcurves(action)]
    if not paths:
        return False
    if obj.type == "ARMATURE":
        return any(path.startswith("pose.bones[") and path.split('"')[1] in obj.pose.bones for path in paths)
    return any(not path.startswith("pose.bones[") for path in paths)


def reset_pose(obj):
    """Put every bone back in its rest pose, so the exported rest pose is not whatever was keyed last."""
    for bone in obj.pose.bones:
        bone.location, bone.scale = (0, 0, 0), (1, 1, 1)
        bone.rotation_quaternion, bone.rotation_euler = (1, 0, 0, 0), (0, 0, 0)


def export_glb(path, objects=None, sampled=True):
    """Export the given objects (or the whole scene) as a .glb with its
    animations the way the engine plays them: Y up, skins of four influences,
    no shape keys, every animation starting at time 0.

    sampled=True (the default): one animation per NLA track (push_to_nla
    first), a key per frame on every bone that moves: what Bezier curves,
    constraints and IK look like, baked. Constant channels are cut to two keys.

    sampled=False: one animation per action, with the keys exactly as they
    are. Much smaller for slow motion, but only right for actions that are
    already LINEAR (bake, then clean_curves) and start at frame 0
    (slide_to_zero), and only with one armature in the export."""
    if objects is not None:
        bpy.ops.object.select_all(action="DESELECT")
        for obj in objects:
            obj.select_set(True)
        bpy.context.view_layer.objects.active = objects[0]
    for obj in (objects if objects is not None else bpy.context.scene.objects):
        if obj.type == "ARMATURE":
            reset_pose(obj)
    options = dict(
        filepath=path, export_format="GLB", export_yup=True, use_selection=objects is not None,
        export_apply=False, export_normals=True, export_texcoords=True, export_tangents=False,
        export_materials="EXPORT", export_image_format="AUTO",
        export_draco_mesh_compression_enable=False, export_morph=False, export_morph_animation=False,
        export_cameras=False, export_lights=False, export_extras=True,
        export_skins=True, export_influence_nb=4, export_all_influences=False, export_def_bones=False,
        export_animations=True, export_animation_mode="NLA_TRACKS" if sampled else "ACTIONS",
        export_force_sampling=sampled, export_optimize_animation_size=True,
        export_optimize_animation_keep_anim_armature=True, export_anim_slide_to_zero=True,
        export_anim_single_armature=True, export_reset_pose_bones=True, export_bake_animation=False,
    )
    # Blender renames export options between versions: pass only the ones this one knows.
    known = bpy.ops.export_scene.gltf.get_rna_type().properties.keys()
    bpy.ops.export_scene.gltf(**{key: value for key, value in options.items() if key in known})
    return path
