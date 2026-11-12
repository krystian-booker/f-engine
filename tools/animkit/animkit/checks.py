"""The automated checks: keyframe integrity, motion quality, loops and
hand-overs, deformation and game readiness. Each returns Findings; kitcommon
decides what they mean for the verdict.

The keys are judged as the file holds them; the motion is judged as the
engine would play it (rig.py evaluates poses exactly as Model.cpp does), on
the points of the skinned mesh rather than on the curves: what is measured is
what the player would see move.

What an animation is *meant* to be comes from the profile, under
"animations": {name: {"loop", "seconds", "from", "to", "grounded",
"in_place", "tremble", "impacts", "optional"}} and "required": [names]. A
game's manifest puts them under an entry's "overrides".
"""
import fnmatch
import re

import numpy as np

from kitcommon.kit import ERROR, INFO, WARN, Finding

from . import measure as measures
from . import rig as rigs

GROUPS = [
    ("keys", "Keyframe integrity"),
    ("motion", "Motion quality"),
    ("loops", "Loops and hand-overs"),
    ("deform", "Deformation"),
    ("ready", "Game readiness"),
]

SPEC_KEYS = {"loop", "seconds", "from", "to", "grounded", "in_place", "tremble", "impacts", "optional"}


def _names(items, limit=4):
    items = list(dict.fromkeys(items))
    return ", ".join(items[:limit]) + (f" and {len(items) - limit} more" if len(items) > limit else "")


def _clip(name):
    return f"animation '{name}'" if name else "an unnamed animation"


def _percent(value, size):
    return f"{100.0 * value / size:.1f}% of its size"


def _where(rig, channel):
    return f"{rig.node_name(channel.node)}.{channel.path}"


# --------------------------------------------------------------------- keys

def check_keys(subject, profile):
    out = []
    rig = subject.rig
    tolerance = profile.get("quaternion_tolerance", 0.002)
    for clip in rig.clips:
        where = _clip(clip.name)
        grouped = {}
        for check, severity, message, count in clip.problems:
            grouped.setdefault((check, severity), []).append((message, count))
        fixes = {
            "keys.target": "Remove the channel, or export without shape-key animation: the engine animates the translation, rotation "
                           "and scale of nodes only.",
            "keys.sampler": "Export the animation again; a sampler needs one value per key time (three for a cubic spline) of the "
                            "right type.",
            "keys.duplicate_channels": "Keep one channel per node and property: two NLA strips or two actions were exported into one "
                                       "animation.",
            "keys.interpolation": "Use LINEAR or STEP (ak.export_glb samples every curve).",
        }
        for (check, severity), entries in grouped.items():
            out.append(Finding(check, severity, ("channel at fault: " if len(entries) == 1 else "channels at fault; the first: ") + entries[0][0],
                               len(entries), where, fixes.get(check)))
        bad_numbers, bad_times, off_unit, zero_quat, negative, zero, uneven, cubic = [], [], [], [], [], [], [], []
        worst_unit, worst_cubic = 0.0, 0.0
        for channel in clip.channels:
            name = _where(rig, channel)
            if not (np.isfinite(channel.times).all() and np.isfinite(channel.values).all()):
                bad_numbers.append(name)
                continue
            if len(channel.times) > 1 and (np.diff(channel.times) <= 0).any() or channel.times[0] < 0:
                bad_times.append(name)
            if channel.path == "rotation":
                length = np.linalg.norm(channel.values, axis=1)
                if (length < 1e-6).any():
                    zero_quat.append(name)
                off = np.abs(length - 1)
                if off.max() > tolerance:
                    off_unit.append((name, int((off > tolerance).sum())))
                    worst_unit = max(worst_unit, float(off.max()))
            elif channel.path == "scale":
                if (channel.values < -1e-9).any():
                    negative.append((name, int((channel.values < -1e-9).any(axis=1).sum())))
                elif (np.abs(channel.values) < 1e-9).any():
                    zero.append((name, int((np.abs(channel.values) < 1e-9).any(axis=1).sum())))
                spread = np.abs(channel.values).max(axis=1) / np.maximum(np.abs(channel.values).min(axis=1), 1e-12)
                if (spread > profile.get("uniform_scale_ratio", 1.02)).any():
                    uneven.append(name)
            if channel.interpolation == "CUBICSPLINE" and len(channel.times) > 1:
                times = np.linspace(channel.times[0], channel.times[-1], min(2000, 8 * len(channel.times)))
                authored, played = rigs.sample_channel(channel, times, spline=True), rigs.sample_channel(channel, times)
                if channel.path == "rotation":
                    difference = float(rigs.quat_angle(authored, played).max())
                    cubic.append((name, f"{difference:.1f} degrees"))
                else:
                    difference = float(np.abs(authored - played).max())
                    cubic.append((name, f"{difference:.3g}"))
                    difference = 100.0 * difference / subject.size if channel.path == "translation" else difference * 100
                worst_cubic = max(worst_cubic, difference)
        if bad_numbers:
            out.append(Finding("keys.nan", ERROR, f"channels with keys that are not numbers (NaN or infinite): {_names(bad_numbers)}",
                               len(bad_numbers), where, "The generator divided by zero or normalised a zero vector; find the key and its source."))
        if bad_times:
            out.append(Finding("keys.times", ERROR, f"channels whose key times are negative or do not increase: {_names(bad_times)}",
                               len(bad_times), where, "Key times must start at 0 or later and increase strictly: remove doubled keys, "
                                                      "sort them, and do not key before frame 0."))
        if clip.start > profile.get("start_tolerance", 0.001) and clip.channels:
            out.append(Finding("keys.start", WARN,
                               f"its first key is at {clip.start:.2f} s; the engine's clock starts at 0, so it holds the first pose "
                               f"for {clip.start:.2f} s every time it plays (and at every turn of a loop)",
                               None, where, "Start the action at frame 0 (ak.export_glb slides every action to zero)."))
        if zero_quat or off_unit:
            keys = sum(count for _, count in off_unit) + len(zero_quat)
            names = _names(zero_quat + [name for name, _ in off_unit])
            severe = bool(zero_quat) or worst_unit > 10 * tolerance
            out.append(Finding("keys.quaternion", ERROR if severe else WARN,
                               ("zero-length rotation keys" if zero_quat else
                                f"rotation keys that are not unit length (off by up to {worst_unit:.3f})") +
                               f" on {names}: the engine does not renormalise, so the part swells, shrinks or collapses",
                               keys, where, "Normalise every quaternion before keying it (ak.check_action reports them)."))
        if negative:
            out.append(Finding("keys.scale", ERROR, f"negative scale keys on {_names([name for name, _ in negative])}: "
                                                    "a mirrored part is wound backwards and its faces are culled",
                               sum(count for _, count in negative), where, "Animate scale between small positive values; never through zero."))
        if zero:
            out.append(Finding("keys.scale", WARN, f"scale keys of exactly 0 on {_names([name for name, _ in zero])}: "
                                                   "the part collapses to a point and its normals are undefined",
                               sum(count for _, count in zero), where, "Hide a part with a tiny scale such as 0.0001, not 0."))
        if uneven:
            out.append(Finding("keys.scale_nonuniform", INFO, f"channels with non-uniform scale ({_names(uneven)}): "
                                                              "squash and stretch, which the engine shades with slightly skewed normals",
                               len(uneven), where))
        if cubic:
            out.append(Finding("keys.interpolation", WARN,
                               f"cubic-spline channels ({_names([name for name, _ in cubic], 3)}): the engine plays their key "
                               f"values with linear interpolation and ignores the tangents (two thirds of their data); "
                               f"the pose differs from the authored curve by up to {max(cubic, key=lambda c: float(c[1].split()[0]))[1]}",
                               len(cubic), where, "Export sampled keys: ak.bake(action, fps), then ak.export_glb (which forces sampling)."))
        if (not clip.channels or clip.duration <= 0) and not any(check == "keys.sampler" for check, _, _, _ in clip.problems):
            out.append(Finding("keys.empty", ERROR,
                               "it has no keys the engine can play: its length is 0, it shows the rest pose and a script waiting "
                               "for it to finish waits for ever",
                               None, where, "Key at least two frames on at least one bone, or delete the animation."))
    return out


# ------------------------------------------------------------------- motion

def needed_keys(channel, tolerance):
    """How many of a channel's keys carry information: what is left after
    dropping, pass by pass, every key that linear interpolation between the
    keys kept around it (as the engine plays it) reproduces, along with every
    key already dropped there, within `tolerance` (degrees for rotations).
    Returns the indices of the keys to keep."""
    times, values = channel.times, channel.values
    count = len(times)
    if channel.interpolation == "STEP":
        change = np.abs(np.diff(values, axis=0)).max(axis=1) > 1e-9 if count > 1 else np.zeros(0, dtype=bool)
        return np.r_[0, np.nonzero(change)[0] + 1]
    kept = np.arange(count)
    rotation = channel.path == "rotation"
    while len(kept) >= 3:
        dropped = False
        for offset in (1, 2):
            candidates = np.arange(offset, len(kept) - 1, 2)          # every other key still kept: their spans do not overlap
            if not len(candidates):
                continue
            a, b = kept[candidates - 1], kept[candidates + 1]
            lengths = b - a - 1
            span = np.repeat(np.arange(len(candidates)), lengths)
            inside = np.arange(len(span)) - np.repeat(np.cumsum(lengths) - lengths, lengths)
            index = a[span] + 1 + inside                               # every original key between the two kept ones
            t = ((times[index] - times[a][span]) / np.maximum(times[b][span] - times[a][span], 1e-12))[:, None]
            if rotation:
                error = rigs.quat_angle(rigs.slerp(values[a][span], values[b][span], t), values[index])
            else:
                error = np.abs(values[a][span] + (values[b][span] - values[a][span]) * t - values[index]).max(axis=1)
            worst = np.zeros(len(candidates))
            np.maximum.at(worst, span, error)
            fine = worst <= tolerance
            if fine.any():
                kept = np.delete(kept, candidates[fine])
                dropped = True
        if not dropped:
            break
    return kept


def find_pops(take, size, profile):
    """Single-frame glitches in the followed points: a point that leaps and
    stays (one step far larger than the steps on either side of it) or jumps
    out and straight back (two opposite steps, far larger than the steps
    outside them and than the motion bends anywhere around them). Returns
    [(grid step, point index, kind, distance)].

    An authored impact is not one: there the speed builds over several frames
    and stops, so the last step has large neighbours before it and nothing
    comes back. Neither is a tremble: there every frame bends as much as the
    next. A pop inside a fast swing is not found; it is not seen either."""
    steps = take.steps()
    if steps.shape[0] < 3 or steps.shape[1] == 0:
        return []
    length = np.linalg.norm(steps, axis=2)                    # (steps, points)
    floor = profile.get("pop_size_fraction", 0.01) * size
    ratio = profile.get("pop_ratio", 4.0)
    reach = max(2, int(round(profile.get("pop_neighbourhood_seconds", 0.25) * take.rate)))
    count = len(length)
    found = []
    padded = np.pad(length, ((1, 1), (0, 0)), constant_values=np.nan)
    before, after = padded[:-2], padded[2:]                    # the steps on either side (nan at the clip's ends)

    def usual(values, index, skip):
        """The 75th percentile of `values` for one point around `index`, leaving out `skip` entries from it."""
        low, high = max(0, index - reach), min(len(values), index + skip + reach)
        around = np.r_[values[low:index], values[index + skip:high]]
        return float(np.percentile(around, 75)) if len(around) else 0.0

    # A leap: one step, isolated.
    leap = (length >= profile.get("leap_size_fraction", 0.03) * size) & \
        (length >= ratio * np.nan_to_num(np.fmax(before, after), nan=np.inf))
    for index, point in zip(*np.nonzero(leap)):
        if length[index, point] >= ratio * usual(length[:, point], index, 1):
            found.append((int(index), int(point), "leaps", float(length[index, point])))
    # Out and back: two opposite steps, against how sharply the motion bends where the spike is not.
    if count >= 2:
        first, second = length[:-1], length[1:]
        with np.errstate(invalid="ignore", divide="ignore"):
            cosine = np.einsum("spx,spx->sp", steps[:-1], steps[1:]) / (first * second)
        amount = np.minimum(first, second)                     # (steps - 1, points): the spike is at grid step s + 1
        bend = np.linalg.norm(np.diff(steps, axis=0), axis=2) / 2
        outer_before, outer_after = before[:-1], after[1:]     # the steps just outside the pair
        calm = np.nan_to_num(np.fmin(outer_before, outer_after), nan=np.inf)
        busy = np.nan_to_num(np.fmax(outer_before, outer_after), nan=np.inf)
        # Far larger than the calmer side, and clearly larger than the busier one (a turn-around in
        # even motion has steps of its own size on both sides; a spike before a swing has not).
        spike = (amount >= floor) & (cosine < -0.5) & (amount >= ratio * calm) & (amount >= ratio / 2 * busy)
        for index, point in zip(*np.nonzero(spike)):
            around = bend[:, point].copy()
            if amount[index, point] >= ratio * usual(around, index - 1, 3):
                found.append((int(index) + 1, int(point), "jumps out and back", float(amount[index, point])))
    return found


def pops_of(subject, take, profile):
    """find_pops, less the points that a STEP channel moves: a step written as a step is a decision, not a glitch."""
    rig = subject.rig
    stepped = [channel.node for channel in take.clip.channels if channel.interpolation == "STEP" and not channel.constant]
    pops = find_pops(take, subject.size, profile)
    if not stepped or not pops:
        return pops
    held = {point for point in {p for _, p, _, _ in pops}
            if any(_under(rig, int(subject.follow_node[point]), node) for node in stepped)}
    return [pop for pop in pops if pop[1] not in held]


def find_jitter(take, size, profile):
    """Followed points that shake: their direction of travel reverses at
    `jitter_hz` or faster for a third of a second or more, by more than a
    hair. Returns {point index: seconds of it}."""
    steps = take.steps()
    window = max(3, int(round(profile.get("jitter_window_seconds", 0.3) * take.rate)))
    if steps.shape[0] <= window + 1 or steps.shape[1] == 0:
        return {}
    length = np.linalg.norm(steps, axis=2)
    floor = profile.get("jitter_size_fraction", 0.003) * size
    with np.errstate(invalid="ignore", divide="ignore"):
        cosine = np.einsum("spx,spx->sp", steps[:-1], steps[1:]) / (length[:-1] * length[1:])
    reversal = (cosine < -0.3) & (length[:-1] >= floor) & (length[1:] >= floor)
    running = np.cumsum(np.r_[np.zeros((1, reversal.shape[1])), reversal], axis=0)
    per_second = (running[window:] - running[:-window]) / (window / take.rate)
    hot = per_second >= 2 * profile.get("jitter_hz", 6.0)       # two reversals to a cycle
    return {int(point): float(hot[:, point].sum() / take.rate + window / take.rate) for point in np.nonzero(hot.any(axis=0))[0]}


def _allowed_tremble(spec, name):
    tremble = spec.get("tremble")
    if tremble is True:
        return True
    return bool(tremble) and any(fnmatch.fnmatch(name, pattern) for pattern in ([tremble] if isinstance(tremble, str) else tremble))


def check_motion(subject, profile):
    out = []
    rig, size = subject.rig, subject.size
    for take in subject.takes:
        clip, where = take.clip, _clip(take.name)
        if take.pose is None:
            continue
        # Rotations too far apart for "the short way round" to be the way that was meant.
        limit = profile.get("max_key_rotation", 140)
        flips, worst = [], 0.0
        for channel in clip.channels:
            if channel.path == "rotation" and len(channel.values) > 1 and np.isfinite(channel.values).all():
                angle = rigs.quat_angle(channel.values[1:], channel.values[:-1])
                if (angle > limit).any():
                    flips.append((rig.node_name(channel.node), int((angle > limit).sum()),
                                  float(channel.times[1:][angle > limit][0])))
                    worst = max(worst, float(angle.max()))
        if flips:
            out.append(Finding("motion.flips", WARN,
                               f"rotations of up to {worst:.0f} degrees from one key to the next on {_names([n for n, _, _ in flips])} "
                               f"(first at {min(t for _, _, t in flips):.2f} s): the engine turns the short way round, which this close "
                               f"to half a turn may be the wrong way",
                               sum(count for _, count, _ in flips), where,
                               "Key the turn in steps of 90 degrees or less (ak.bake does), and run ak.fix_quaternion_flips on the action."))
        moving = take.tracked.shape[1] > 0 and len(take.times) > 1
        travel = float(np.linalg.norm(take.tracked - take.tracked[:1], axis=2).max()) if moving else 0.0
        changing = [channel for channel in clip.channels if not channel.constant]
        if travel < profile.get("static_size_fraction", 0.0005) * size and \
                (rig.vertices or not changing) and not any(c.path == "scale" for c in changing):
            out.append(Finding("motion.static", WARN, f"nothing moves in its {clip.duration:.2f} s", None, where,
                               "Give a held pose some life (breathing, a sway), or if a still pose is meant keep it to two keys "
                               "and waive this with that reason."))
            continue
        # Pops.
        pops = pops_of(subject, take, profile)
        if pops:
            moments = {}
            for index, point, kind, distance in pops:
                moments.setdefault(index, []).append((subject.depth(subject.follow_node[point]), point, kind, distance))
            described = []
            for index in sorted(moments):
                _, point, kind, distance = min(moments[index])
                described.append(f"{rig.node_name(subject.follow_node[point])} {kind} {_percent(distance, size)} at {take.times[index]:.2f} s"
                                 + (f" (with {len(moments[index]) - 1} more joints)" if len(moments[index]) > 1 else ""))
            out.append(Finding("motion.pops", WARN,
                               "single-frame pops: " + "; ".join(described[:3]) + (f"; and {len(described) - 3} more" if len(described) > 3 else ""),
                               len(moments), where,
                               "Look at those frames in the review sheet. A limb that flips for a frame is an IK solve crossing a "
                               "singularity or an angle wrapping past 180 degrees: fix it in the generator. A cut that is meant "
                               "should ease over two or three frames, or be waived as by design."))
        # Jitter.
        shaking = find_jitter(take, size, profile)
        shaking = {point: seconds for point, seconds in shaking.items()
                   if not _allowed_tremble(take.spec, rig.node_name(subject.follow_node[point]))}
        if shaking:
            names = [rig.node_name(subject.follow_node[point]) for point in sorted(shaking, key=lambda p: -shaking[p])]
            out.append(Finding("motion.jitter", WARN,
                               f"joints shake (direction reversing {profile.get('jitter_hz', 6.0):g} times a second or "
                               f"faster, for up to {max(shaking.values()):.1f} s): {_names(names)}",
                               len(shaking), where,
                               "If it is noise (an unstable IK solve, a baked simulation), smooth it in the generator. If the shake "
                               "is the animation, say so in its spec: \"tremble\": true, or a list of the joints that may, such as "
                               "[\"shard_*\"]."))
        # Sparse keys played without easing.
        corners, cornered = 0, []
        sparse = profile.get("sparse_key_seconds", 0.1)
        for channel in clip.channels:
            if channel.interpolation == "STEP" or len(channel.times) < 3 or channel.constant or not np.isfinite(channel.values).all():
                continue
            spans = np.diff(channel.times)
            if channel.path == "rotation":
                change = np.radians(rigs.quat_angle(channel.values[1:], channel.values[:-1]))
                signed = channel.values[1:] * np.sign(np.sum(channel.values[1:] * channel.values[:-1], axis=1, keepdims=True) + 1e-30)
                velocity = (signed - channel.values[:-1]) * 2 / spans[:, None]                  # about radians a second
                reach = float(change.sum())
                visible = reach > np.radians(profile.get("no_easing_min_degrees", 8))
            else:
                velocity = np.diff(channel.values, axis=0) / spans[:, None]
                reach = float(np.abs(np.diff(channel.values, axis=0)).sum())
                visible = reach > (profile.get("no_easing_min_size_fraction", 0.02) * size if channel.path == "translation" else 0.05)
            if not visible:
                continue
            speed = np.linalg.norm(velocity, axis=1)
            jump = np.linalg.norm(np.diff(velocity, axis=0), axis=1)
            hard = (jump >= 0.5 * speed.max()) & (spans[:-1] >= sparse) & (spans[1:] >= sparse)
            if hard.any():
                corners += int(hard.sum())
                cornered.append(_where(rig, channel))
        if corners >= profile.get("no_easing_corners", 3):
            out.append(Finding("motion.no_easing", WARN,
                               f"keys where the motion changes speed or direction in a single frame between stretches of "
                               f"constant speed ({_names(cornered)}): sparse keys played with linear interpolation, which the engine "
                               f"does not ease",
                               corners, where,
                               "Author the eases and bake them into the keys: ak.bake(action, 30) samples the curves, so the "
                               "slow-in and slow-out survive export."))
        # Bloat.
        constant = [channel for channel in clip.channels if channel.constant]
        if constant:
            at_rest = sum(1 for channel in constant if np.allclose(
                channel.values[0], {"translation": rig.rest_t, "rotation": rig.rest_r, "scale": rig.rest_s}[channel.path][channel.node],
                atol=1e-5) or (channel.path == "rotation" and np.allclose(-channel.values[0], rig.rest_r[channel.node], atol=1e-5)))
            keys = sum(len(channel.times) for channel in constant)
            out.append(Finding("motion.constant_channels", INFO,
                               f"of {len(clip.channels)} channels never change ({keys:,} keys; {at_rest} of them only "
                               f"repeat the rest pose)", len(constant), where))
        if changing:
            angle = profile.get("redundant_key_degrees", 0.05)
            distance = profile.get("redundant_key_size_fraction", 0.0002) * size
            total = sum(len(channel.times) for channel in changing)
            needed = sum(len(needed_keys(channel, angle if channel.path == "rotation" else distance if channel.path == "translation" else 0.001))
                         if np.isfinite(channel.values).all() and np.isfinite(channel.times).all() else len(channel.times)
                         for channel in changing)
            spare = 100.0 * (total - needed) / max(total, 1)
            take.needed_keys, take.changing_keys = needed, total
            heavy = total >= profile.get("redundant_key_floor", 3000) and spare >= profile.get("redundant_key_percent", 90)
            out.append(Finding("motion.redundant_keys", WARN if heavy else INFO,
                               f"of the {total:,} keys on its moving channels say nothing ({spare:.0f}%): {needed:,} would play the "
                               f"same motion within {angle:g} degrees and {100 * distance / size:.2f}% of its size",
                               total - needed, where,
                               "ak.clean_curves(action) after baking drops the keys linear interpolation reproduces anyway."))
    return out


# -------------------------------------------------------------------- loops

def _gap(points_a, points_b):
    """How far the mesh is from one pose to another: (the furthest a vertex has to travel, which vertex)."""
    if points_a is None or points_b is None or not len(points_a):
        return 0.0, -1
    distance = np.linalg.norm(points_a - points_b, axis=1)
    return float(distance.max()), int(np.argmax(distance))


def _handover(spec_value):
    """A spec's "from"/"to": a name (cross-faded) or {"animation": name, "fade": seconds} (0 is a cut)."""
    if isinstance(spec_value, str):
        return spec_value, None
    if isinstance(spec_value, dict):
        return spec_value.get("animation"), spec_value.get("fade")
    return None, None


def check_loops(subject, profile):
    out = []
    size = subject.size
    for take in subject.takes:
        if take.points is None or not take.declared:
            continue
        where = _clip(take.name)
        steps = np.linalg.norm(take.steps(), axis=2) if take.tracked.shape[1] and len(take.times) > 1 else np.zeros((0, 0))
        typical = float(np.median(steps.max(axis=1))) if steps.size else 0.0     # the usual largest step of a frame
        if take.loop:
            gap, vertex = _gap(take.first, take.last)
            floor = profile.get("seam_size_fraction", 0.003) * size
            if gap > floor and gap > profile.get("seam_step_ratio", 0.5) * typical:
                severe = gap > profile.get("seam_error_size_fraction", 0.02) * size
                out.append(Finding("loops.seam_pose", ERROR if severe else WARN,
                                   f"it is declared a loop, but its last pose is not its first: {subject.rig.node_name(subject.owner[vertex])} "
                                   f"is {gap:.3f} units ({_percent(gap, size)}) away, {gap / max(typical, 1e-9):.1f} times what it moves "
                                   f"in one of its own frames. The engine wraps from the last key to time 0, so it pops at every turn",
                                   None, where,
                                   "Make the last key a copy of the first (ak.make_loop(action)); the engine never shows the last "
                                   "key itself, so the copy does not stall the loop."))
            elif steps.shape[0] >= 4:
                # The seam joins the last step to the first: the change of velocity there against the clip's own.
                moves = take.steps()
                inside = np.linalg.norm(np.diff(moves, axis=0), axis=2)                   # (steps - 1, points)
                across = np.linalg.norm(moves[0] - moves[-1], axis=1)                     # (points,)
                usual = inside.max(axis=0)                                                 # the sharpest change inside the loop
                floor = profile.get("seam_velocity_size_fraction", 0.004) * size
                jolt = (across > floor) & (across > profile.get("seam_velocity_ratio", 3.0) * usual)
                if jolt.any():
                    point = int(np.argmax(np.where(jolt, across, 0)))
                    names = [subject.rig.node_name(subject.follow_node[p]) for p in np.nonzero(jolt)[0]]
                    out.append(Finding("loops.seam_velocity", WARN,
                                       f"joints hitch at the seam ({_names(names)}): the poses meet there but the motion does not; the velocity "
                                       f"changes by {across[point] * take.rate:.2f} units a second in one frame, "
                                       f"{min(across[point] / max(float(usual[point]), 1e-9), 999):.0f} times the sharpest change inside the loop",
                                       int(jolt.sum()), where,
                                       "Let the motion run through the seam: ak.make_loop(action) matches the tangents at both ends; "
                                       "in a generator, use whole cycles of every periodic layer."))
        # Hand-overs: where this animation says it comes from and goes to.
        for key in ("from", "to"):
            other_name, fade = _handover(take.spec.get(key))
            if not other_name:
                continue
            other = subject.take(other_name)
            if other is None or other.points is None:
                out.append(Finding("loops.handover", ERROR,
                                   f"its spec says it {'starts from' if key == 'from' else 'hands over to'} '{other_name}', "
                                   f"which the file does not have", None, where, "Fix the name in the spec, or add the animation."))
                continue
            if key == "to":
                gap, vertex = _gap(take.last, other.first)
                moment = f"its last pose and the first pose of '{other_name}'"
                worst = gap
            else:
                # A loop is left at any moment of its cycle; a one-shot holds its last pose.
                gap, vertex = _gap(take.first, other.first if other.loop else other.last)
                moment = f"its first pose and the {'first' if other.loop else 'last'} pose of '{other_name}'"
                worst = max(_gap(take.first, pose)[0] for pose in other.points) if other.loop else gap
            cut = fade is not None and fade <= 0
            limit = profile.get("cut_size_fraction" if cut else "fade_size_fraction", 0.005 if cut else 0.05) * size
            how = "the game cuts between them, so this shows as a pop" if cut else \
                "the engine's cross-fade has to carry it that far, which shows as a slide"
            if gap > limit:
                out.append(Finding("loops.handover", ERROR if cut and gap > 4 * limit else WARN,
                                   f"{moment} are {gap:.3f} units ({_percent(gap, size)}) apart at "
                                   f"{subject.rig.node_name(subject.owner[vertex])}; {how}",
                                   None, where,
                                   f"End (or start) the animation in that pose exactly: key it from the other animation's "
                                   f"{'first' if key == 'to' or other.loop else 'last'} frame."))
            elif cut and worst > limit:
                out.append(Finding("loops.handover", WARN,
                                   f"the game cuts into it from the loop '{other_name}' at any moment, and that loop moves up to "
                                   f"{worst:.3f} units ({_percent(worst, size)}) from the pose this starts in: a pop of up to that size",
                                   None, where,
                                   "Cross-fade into it (entity:play(name, false, 0.2) and say so in the spec's \"from\"), or start it "
                                   "when the loop is at its first frame."))
    return out


# ------------------------------------------------------------------- deform

def check_deform(subject, profile):
    out = []
    rig, size = subject.rig, subject.size
    if not rig.vertices:
        return out
    faces = rig.faces
    rest = subject.rest
    corners = rest[faces] if len(faces) else np.zeros((0, 3, 3))
    rest_edges = np.stack([np.linalg.norm(corners[:, (i + 1) % 3] - corners[:, i], axis=1) for i in range(3)], axis=1) \
        if len(faces) else np.zeros((0, 3))
    rest_area = np.linalg.norm(np.cross(corners[:, 1] - corners[:, 0], corners[:, 2] - corners[:, 0]), axis=1) / 2 \
        if len(faces) else np.zeros(0)
    radius = float(np.linalg.norm(rest - subject.centre, axis=1).max()) or size
    ground = bool(profile.get("ground"))
    for take in subject.takes:
        if take.points is None:
            continue
        where = _clip(take.name)
        points, times = take.points, take.sample_times
        if not np.isfinite(points).all():
            out.append(Finding("deform.bounds", ERROR, "the skinned mesh has vertices that are not numbers when it plays", None, where,
                               "A key or an inverse bind matrix is not a number; see the keys findings."))
            continue
        # Flying apart.
        reach = np.linalg.norm(points - subject.centre, axis=2).max(axis=1)
        limit = profile.get("max_reach", 3.0)
        if reach.max() > limit * radius:
            index = int(np.argmax(reach))
            vertex = int(np.argmax(np.linalg.norm(points[index] - subject.centre, axis=1)))
            out.append(Finding("deform.bounds", ERROR,
                               f"sampled frames where the mesh flies apart: it reaches {reach.max():.1f} units from its centre at "
                               f"{times[index]:.2f} s, {reach.max() / radius:.1f} times its reach at rest "
                               f"({rig.node_name(subject.owner[vertex])} flies off)",
                               int((reach > limit * radius).sum()), where,
                               "A translation or scale key is in the wrong units or space (centimetres, world instead of local); "
                               "look at that joint's keys."))
            continue
        # Stretching and collapsing, where no joint is being scaled on purpose.
        if len(faces):
            free = free_faces(subject, take, rest_edges, rest_area)
            stretched = np.zeros(len(faces), dtype=bool)
            collapsed = np.zeros(len(faces), dtype=bool)
            worst_stretch, worst_time, worst_length = 1.0, 0.0, 0.0
            for index in range(len(points)):
                ratio, shrink, longest = strain(points[index], faces, rest_edges, rest_area, free)
                pulled = (ratio > profile.get("max_edge_stretch", 2.5)) & (longest > profile.get("stretch_min_size_fraction", 0.03) * size)
                if pulled.any() and ratio[pulled].max() > worst_stretch:
                    worst = int(np.argmax(np.where(pulled, ratio, 0)))
                    worst_stretch, worst_time, worst_length = float(ratio[worst]), float(times[index]), float(longest[worst])
                stretched |= pulled
                collapsed |= shrink < profile.get("min_area_ratio", 0.05)
            if stretched.sum() >= profile.get("stretch_triangles", 1):
                joints = [rig.node_name(n) for n in np.unique(subject.owner[faces[stretched]])]
                out.append(Finding("deform.stretch", WARN,
                                   f"triangles pulled to more than {profile.get('max_edge_stretch', 2.5):g} times their length at rest and "
                                   f"longer than {100 * profile.get('stretch_min_size_fraction', 0.03):g}% of the model "
                                   f"(up to {worst_stretch:.1f} times, {worst_length:.2f} units long, at {worst_time:.2f} s) around {_names(joints)}",
                                   int(stretched.sum()), where,
                                   "Vertices weighted to a bone that moves away from them: check the weights around those joints "
                                   "(a stray vertex on the wrong bone, a limb weighted to the root). A part that is meant to stretch "
                                   "between two bones (a cable, a bellows) is waived as by design."))
            if collapsed.sum() >= profile.get("collapse_triangles", 8):
                joints = [rig.node_name(n) for n in np.unique(subject.owner[faces[collapsed]])]
                out.append(Finding("deform.collapse", WARN,
                                   f"triangles that collapse to under {100 * profile.get('min_area_ratio', 0.05):g}% of their area at rest "
                                   f"around {_names(joints)}: a joint twisted or bent further than its weights can follow (the candy-wrapper)",
                                   int(collapsed.sum()), where,
                                   "Spread the twist over two bones, or give the joint more edge loops and smoother weights."))
        # The ground.
        lowest = points[:, :, 1].min(axis=1)
        if ground and take.spec.get("grounded", True) is not False:
            sink = profile.get("ground_tolerance", 0.015) * size
            if lowest.min() < -sink:
                index = int(np.argmin(lowest))
                under = np.nonzero(points[index][:, 1] < -sink)[0]
                joints = [rig.node_name(n) for n in np.unique(subject.owner[under])]
                out.append(Finding("deform.underground", WARN,
                                   f"of {len(lowest)} sampled frames under the ground: it sinks {-lowest.min():.2f} units "
                                   f"({_percent(-lowest.min(), size)}) through it at {times[index]:.2f} s ({_names(joints)})",
                                   int((lowest < -sink).sum()), where,
                                   "Keep feet, knees and fists on or above y = 0: raise the IK targets, or lift the hips in that pose."))
        if take.spec.get("grounded"):
            lift = profile.get("float_tolerance", 0.02) * size
            if lowest.max() > lift:
                index = int(np.argmax(lowest))
                out.append(Finding("deform.floating", WARN,
                                   f"of {len(lowest)} sampled frames in the air, though it is declared grounded: its lowest point is "
                                   f"{lowest.max():.2f} units ({_percent(lowest.max(), size)}) above the ground at {times[index]:.2f} s",
                                   int((lowest > lift).sum()), where,
                                   "Plant the feet (lower the hips or the IK targets), or do not declare a jump as grounded."))
        # Staying over the origin: the engine has no root motion.
        if take.spec.get("in_place") and subject.root_node >= 0:
            origin = take.world[:, subject.root_node, :3, 3][:, [0, 2]]
            away = np.linalg.norm(origin - subject.root_home[[0, 2]], axis=1)
            name = rig.node_name(subject.root_node)
            ends = max(away[0], away[-1])
            if ends > profile.get("in_place_tolerance", 0.05) * size:
                out.append(Finding("deform.in_place", WARN,
                                   f"it is declared in place, but {name} is {ends:.2f} units ({_percent(ends, size)}) from where it stands "
                                   f"at rest when the animation {'starts' if away[0] >= away[-1] else 'ends'}: the engine has no root "
                                   f"motion, so the entity does not move with it and the next animation snaps it back",
                                   None, where,
                                   "Bring the body back over its origin by the last frame (move the entity in the game's code instead), "
                                   "or declare the animation \"in_place\": false and chain it with \"from\" and \"to\"."))
            elif take.loop and away.max() > profile.get("in_place_radius", 0.25) * size:
                out.append(Finding("deform.in_place", WARN,
                                   f"it is a loop declared in place, but {name} wanders {away.max():.2f} units "
                                   f"({_percent(away.max(), size)}) from its origin during the cycle",
                                   None, where, "Animate a walk or a run on the spot; the game moves the entity."))
    return out


def free_faces(subject, take, rest_edges, rest_area):
    """The triangles whose stretch means something: real ones (not slivers at
    rest) that no joint scales on purpose in this animation (an eye scaled
    shut is not a collapsed weight)."""
    rig, size = subject.rig, subject.size
    solid = (rest_area > (size * 1e-4) ** 2) & (rest_edges.min(axis=1) > size * 1e-5)
    scale = np.abs(take.pose[2])                                                 # (grid, nodes, 3)
    scaled = (np.abs(scale - np.abs(rig.rest_s[None])) > 1e-3).any(axis=(0, 2))  # nodes whose scale is animated
    for node in list(np.nonzero(scaled)[0]):                                     # ... and everything below them
        scaled |= np.array([_under(rig, other, node) for other in range(len(rig.names))])
    owners = subject.owner[rig.faces]
    return solid & ~scaled[np.maximum(owners, 0)].any(axis=1)


def strain(points, faces, rest_edges, rest_area, free):
    """Per triangle of one pose: (how much its most stretched edge has grown
    against rest, its area against its area at rest, the length of that
    edge now); 1 where not `free`."""
    c = points[faces]
    edges = np.stack([np.linalg.norm(c[:, (i + 1) % 3] - c[:, i], axis=1) for i in range(3)], axis=1)
    area = np.linalg.norm(np.cross(c[:, 1] - c[:, 0], c[:, 2] - c[:, 0]), axis=1) / 2
    with np.errstate(invalid="ignore", divide="ignore"):
        growth = np.where(free[:, None], edges / rest_edges, 1.0)
        shrink = np.where(free, area / rest_area, 1.0)
    most = growth.argmax(axis=1)
    rows = np.arange(len(faces))
    return growth[rows, most], shrink, edges[rows, most]


def _under(rig, node, ancestor):
    steps = 0
    while node >= 0 and steps < 4096:
        if node == ancestor:
            return True
        node, steps = rig.parent[node], steps + 1
    return False


# ---------------------------------------------------------------- readiness

def check_readiness(subject, profile):
    out = []
    rig, stats = subject.rig, subject.stats
    for severity, message in rig.model.problems:
        out.append(Finding("ready.structure", severity, message))
    if not rig.clips:
        out.append(Finding("ready.no_animations", ERROR, "the file has no animations", None, None,
                           "Export with animations (ak.export_glb), or take the file out of the animations manifest."))
        return out
    specs = profile.get("animations") or {}
    names = [clip.name for clip in rig.clips]
    seen = {}
    for name in names:
        seen[name] = seen.get(name, 0) + 1
    pattern = profile.get("name_pattern")
    for index, clip in enumerate(rig.clips):
        if not clip.name:
            out.append(Finding("ready.names", ERROR, f"animation {index} has no name (the engine would call it 'animation{index}'; "
                                                     f"scripts play animations by name)", None, None,
                               "Name every action; ak.push_to_nla names each track after its action."))
        elif pattern and not re.match(pattern, clip.name):
            out.append(Finding("ready.names", WARN, f"'{clip.name}' is not a name in the game's convention ({pattern}): "
                                                    f"lower case, digits and underscores", None, None,
                               "Rename the action (Blender's 'ArmatureAction.001' is not a name a script should play)."))
    for name, count in seen.items():
        if name and count > 1:
            out.append(Finding("ready.names", ERROR, f"animations are named '{name}'; the engine plays the first", count, None,
                               "Give every animation its own name: one NLA track per action (ak.push_to_nla)."))
    wanted = list(dict.fromkeys(list(profile.get("required") or []) +
                                [name for name, spec in specs.items() if not (spec or {}).get("optional")]))
    missing = [name for name in wanted if name not in seen]
    if missing:
        out.append(Finding("ready.missing", ERROR, f"animations the spec asks for are not in the file: {_names(missing, 8)} "
                                                   f"(it has {_names(names, 12)})", len(missing), None,
                           "Add the animation, or fix its name: the game plays animations by these names."))
    undeclared = [name for name in names if name and name not in specs]
    if undeclared:
        out.append(Finding("ready.undeclared", WARN,
                           f"animations have no spec ({_names(undeclared, 6)}): whether they loop, what they hand "
                           f"over to, their length and whether they stay on the ground were not checked",
                           len(undeclared), None,
                           "Say what each is meant to be in the manifest entry's \"overrides\": {\"animations\": {\"idle\": "
                           "{\"loop\": true, \"seconds\": [2, 8], \"grounded\": true, \"in_place\": true}, ...}}."))
    for name, spec in specs.items():
        unknown = sorted(set(spec or {}) - SPEC_KEYS)
        if unknown:
            out.append(Finding("ready.spec", ERROR, f"keys in the spec of '{name}' that animkit does not know: {', '.join(unknown)} "
                                                    f"(it knows {', '.join(sorted(SPEC_KEYS))})", len(unknown), None,
                               "Fix the spelling; a misspelt key is an intent nothing checks."))
        elif name in seen and "loop" not in (spec or {}):
            out.append(Finding("ready.spec", ERROR, f"the spec of '{name}' does not say whether it loops", None, None,
                               "Add \"loop\": true or false."))
    # Lengths and the moments the game relies on.
    tolerance = profile.get("duration_tolerance", 0.05)
    for take in subject.takes:
        where = _clip(take.name)
        seconds = take.spec.get("seconds")
        duration = take.clip.duration
        if seconds is not None:
            low, high = (seconds[0], seconds[1]) if isinstance(seconds, (list, tuple)) else (seconds - tolerance, seconds + tolerance)
            if not low <= duration <= high:
                asked = f"{seconds[0]:g} to {seconds[1]:g}" if isinstance(seconds, (list, tuple)) else f"{seconds:g}"
                out.append(Finding("ready.duration", ERROR, f"it is {duration:.2f} s long; its spec says {asked} s "
                                                            f"(the game's waits and sounds are timed to that)", None, where,
                                   "Retime the animation, or correct the spec and the game's timings together."))
        speed = take.speed()
        if take.points is not None and len(speed) and speed.max() > 0:
            for moment in take.spec.get("impacts") or []:
                if moment > duration + 1e-6:
                    out.append(Finding("ready.impact", ERROR, f"the game acts at {moment:.2f} s, after it has ended ({duration:.2f} s)",
                                       None, where, "Fix the time in the spec and in the game's data."))
                    continue
                reach = profile.get("impact_tolerance", 0.1)
                near = (take.times[:-1] >= moment - reach) & (take.times[:-1] <= moment + reach / 2)
                share = float(speed[near].max() / speed.max()) if near.any() else 0.0
                if share < profile.get("impact_speed_ratio", 0.2):
                    peak = float(take.times[int(np.argmax(speed))])
                    out.append(Finding("ready.impact", WARN,
                                       f"the game acts at {moment:.2f} s, but the animation is nearly still there ({100 * share:.0f}% of its "
                                       f"top speed, which comes at {peak:.2f} s): the blow and its effect will not meet",
                                       None, where,
                                       "Move the strike to that time (the review sheet's speed graph marks it), or change the game's timing."))
        rate = measures.raw_key_rate(take.clip)
        limit = profile.get("max_key_rate", 60)
        if rate > limit * 1.02:
            out.append(Finding("ready.key_rate", WARN, f"it is keyed {rate:.0f} times a second; the engine poses a model {limit:g} times a "
                                                       f"second, so the keys between are never shown", None, where,
                               "Bake at 30 frames a second (ak.bake(action, 30))."))

    def budget(key, value, what, fix):
        limit = profile.get(key)
        if limit is not None and value > limit:
            out.append(Finding("ready." + key, ERROR, f"{value:,} {what}; the '{profile['name']}' budget is {limit:,}", None, None, fix))

    budget("max_animations", stats["animations"], "animations", "Split rarely used animations into another model file, or drop them.")
    budget("max_keys", stats["keys"], "keyframes", "ak.clean_curves(action) drops the keys that say nothing; bake at 30 a second, not 60.")
    budget("max_animation_kilobytes", round(stats["bytes"] / 1000), "kilobytes of keys",
           "Fewer keys (ak.clean_curves), and no channels on bones that never move.")
    budget("max_nodes", stats["nodes"], "nodes (the engine composes every node's matrix for every animated entity, every frame)",
           "Remove helper nodes, empties and leaf bones from the export.")
    budget("max_joints", stats["joints"], "joints in its skins", "Remove helper and leaf bones from the deform set.")
    budget("max_skinned_parts", stats["skinned_parts"],
           "skinned mesh parts (the engine animates 16 of an entity's; the rest are drawn in their rest pose)",
           "Join skinned meshes that share a material.")
    return out


CHECKS = {"keys": check_keys, "motion": check_motion, "loops": check_loops, "deform": check_deform, "ready": check_readiness}


def run(rig, profile):
    """(findings, subject) for a loaded rig."""
    subject = measures.measure(rig, profile)
    findings = []
    for group, _ in GROUPS:
        findings.extend(CHECKS[group](subject, profile))
    return findings, subject


def stats_line(subject):
    s = subject.stats
    return (f"{s['animations']} animations, {s['keys']:,} keys ({s['bytes'] / 1000:.0f} kB), {s['nodes']} nodes, {s['joints']} joints, "
            f"{s['skinned_vertices']:,} skinned vertices in {s['skinned_parts']} parts, {s['size']:.1f} units across")
