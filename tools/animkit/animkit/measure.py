"""What the checks and the review sheet both need of a file, measured once:
the model at rest, a few points to follow on every joint, and for each
animation the poses the engine would show.

Everything is in the model's own space and units (one unit is one grid cell;
Y is up; the ground is y = 0 for a model whose origin is at its base), and
sizes are also given relative to the model's `size`: the largest side of its
bounds at rest, so one threshold suits a hero and a seven-unit boss.
"""
import numpy as np

from . import rig as rigs


class Take:
    """One animation, played: its poses on a regular grid of times.

    The grid runs at the rate the animation is keyed at (clamped between 20
    and the engine's 60 a second), so one step of the grid is one key
    interval: what "from one frame to the next" means for this clip."""

    def __init__(self, clip, spec):
        self.clip, self.spec = clip, spec or {}
        self.name = clip.name
        self.declared = spec is not None
        self.loop = bool(self.spec.get("loop"))
        self.rate = 30.0
        self.times = np.zeros(1)            # the grid
        self.tracked = np.zeros((1, 0, 3))  # (grid, followed points, 3)
        self.points = None                  # (samples, vertices, 3): the skinned mesh at `sample_times`
        self.sample_index = np.zeros(1, dtype=np.int64)  # which grid steps `points` holds
        self.first = self.last = None       # the mesh at time 0 and at the clip's end
        self.world = None                   # (grid, nodes, 4, 4)
        self.pose = None                    # local (t, r, s) on the grid

    @property
    def sample_times(self):
        return self.times[self.sample_index]

    def steps(self):
        """Displacement of each followed point from one grid step to the next: (grid - 1, points, 3)."""
        return np.diff(self.tracked, axis=0)

    def speed(self):
        """The fastest followed point at each grid step, in units a second: (grid - 1,)."""
        if self.tracked.shape[1] == 0 or len(self.times) < 2:
            return np.zeros(max(0, len(self.times) - 1))
        return np.linalg.norm(self.steps(), axis=2).max(axis=1) * self.rate


class Subject:
    """A file, measured: what `check` hands to the review sheet."""

    def __init__(self, rig):
        self.rig = rig
        self.takes = []
        self.rest = np.zeros((0, 3))        # the mesh at rest
        self.low, self.high = np.zeros(3), np.ones(3)
        self.size = 1.0                     # the largest side of the rest bounds
        self.centre = np.zeros(3)
        self.follow = np.zeros(0, dtype=np.int64)       # vertex index followed for each tracked joint
        self.follow_node = np.zeros(0, dtype=np.int64)  # ... and the node that owns it
        self.owner = np.zeros(0, dtype=np.int64)        # per vertex: the node that carries most of it
        self.root_node, self.root_home = -1, np.zeros(3)  # the joint the body stands by (its hips), and where it is at rest
        self.stats = {}

    def take(self, name):
        for take in self.takes:
            if take.name == name:
                return take
        return None

    def depth(self, node):
        depth = 0
        while self.rig.parent[node] >= 0 and depth < 4096:
            node, depth = self.rig.parent[node], depth + 1
        return depth


def key_rate(clip, low=20.0, high=60.0):
    """Keys a second of the clip's moving channels (the median interval between their keys), clamped."""
    intervals = [np.diff(channel.times) for channel in clip.channels
                 if len(channel.times) > 2 and not channel.constant and np.isfinite(channel.times).all()]
    intervals = np.concatenate(intervals) if intervals else np.zeros(0)
    intervals = intervals[intervals > 1e-6]
    if not len(intervals):
        return 30.0
    return float(min(high, max(low, round(1.0 / float(np.median(intervals)), 3))))


def raw_key_rate(clip):
    """Keys a second of the densest moving channel, unclamped (0 when nothing moves)."""
    best = 0.0
    for channel in clip.channels:
        if len(channel.times) > 2 and not channel.constant and np.isfinite(channel.times).all():
            span = channel.times[-1] - channel.times[0]
            if span > 1e-6:
                best = max(best, (len(channel.times) - 1) / span)
    return best


def measure(rig, profile):
    """Pose the model through every animation. Animations whose keys cannot be
    evaluated at all (NaN times, no channels) get a Take with no poses."""
    subject = Subject(rig)
    rest_pose = rigs.sample(rig, None, [0.0])
    rest_world = rigs.world_matrices(rig, rest_pose)
    subject.rest = rigs.skin(rig, rest_world)[0] if rig.vertices else np.zeros((0, 3))
    finite = np.isfinite(subject.rest).all(axis=1) if len(subject.rest) else np.zeros(0, dtype=bool)
    if finite.any():
        subject.low, subject.high = subject.rest[finite].min(axis=0), subject.rest[finite].max(axis=0)
    subject.size = float(max((subject.high - subject.low).max(), 1e-6))
    subject.centre = (subject.low + subject.high) / 2
    subject.owner = rigs.joint_dominance(rig)
    # One point to follow per joint that carries vertices: the vertex farthest from the joint,
    # so a rotation of the joint shows as movement of the point.
    follow, nodes = [], []
    origins = rest_world[0, :, :3, 3]
    for node in np.unique(subject.owner[subject.owner >= 0]).tolist():
        members = np.nonzero(subject.owner == node)[0]
        distance = np.linalg.norm(subject.rest[members] - origins[node], axis=1)
        follow.append(int(members[int(np.argmax(np.nan_to_num(distance)))]))
        nodes.append(node)
    subject.follow, subject.follow_node = np.array(follow, dtype=np.int64), np.array(nodes, dtype=np.int64)
    # Where the body stands: the joint nearest the root that carries vertices (the hips of a figure),
    # of those the one that carries most.
    if nodes:
        depth = [subject.depth(node) for node in nodes]
        carried = [int((subject.owner == node).sum()) for node in nodes]
        subject.root_node = int(nodes[min(range(len(nodes)), key=lambda i: (depth[i], -carried[i]))])
        subject.root_home = origins[subject.root_node].copy()

    specs = profile.get("animations") or {}
    budget = int(profile.get("deform_vertex_poses", 4000000))
    for clip in rig.clips:
        take = Take(clip, specs.get(clip.name) if clip.name else None)
        subject.takes.append(take)
        if clip.duration <= 0 or not np.isfinite(clip.duration) or not clip.channels:
            continue
        take.rate = key_rate(clip, high=float(profile.get("engine_fps", 60)))
        take.times = rigs.frame_times(clip, take.rate)
        take.pose = rigs.sample(rig, clip, take.times)
        take.world = rigs.world_matrices(rig, take.pose)
        count = len(take.times)
        keep = max(2, min(count, budget // max(1, rig.vertices)))
        take.sample_index = np.unique(np.round(np.linspace(0, count - 1, keep)).astype(np.int64))
        if rig.vertices:
            take.points = rigs.skin(rig, take.world[take.sample_index])
            take.first, take.last = take.points[0], take.points[-1]
            if len(take.sample_index) == count:
                take.tracked = take.points[:, subject.follow]
            else:
                take.tracked = _follow(rig, subject, take.world)
        else:
            take.tracked = np.zeros((count, 0, 3))
    subject.stats = {
        "animations": len(rig.clips), "keys": sum(clip.keys for clip in rig.clips),
        "bytes": sum(clip.bytes for clip in rig.clips), "nodes": len(rig.names),
        "joints": sum(len(skin["joints"]) for skin in rig.skins), "vertices": rig.vertices,
        "skinned_vertices": sum(len(part.positions) for part in rig.parts if part.skin >= 0),
        "skinned_parts": sum(1 for part in rig.parts if part.skin >= 0), "size": subject.size,
    }
    return subject


def _follow(rig, subject, world):
    """The followed points alone on the whole grid (when the full mesh is only skinned at some steps)."""
    out = np.zeros((len(world), len(subject.follow), 3))
    for part in rig.parts:
        inside = (subject.follow >= part.offset) & (subject.follow < part.offset + len(part.positions))
        if not inside.any():
            continue
        local = subject.follow[inside] - part.offset
        rest = np.c_[part.positions[local], np.ones(len(local))]
        if part.skin < 0:
            out[:, inside] = np.einsum("tij,vj->tvi", world[:, part.node, :3, :], rest)
            continue
        entry = rig.skins[part.skin]
        joints = world[:, entry["joints"]] @ entry["inverse_bind"]
        total = np.zeros((len(world), len(local), 3))
        for k in range(4):
            weight = part.weights[local, k]
            total += np.einsum("tvij,vj->tvi", joints[:, part.joints[local, k]][:, :, :3, :], rest) * weight[None, :, None]
        out[:, inside] = total
    return out
