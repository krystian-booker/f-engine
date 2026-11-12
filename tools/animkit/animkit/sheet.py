"""The review sheet: pictures of the motion for a person (or an agent) to
judge, drawn without the engine from the same poses the checks measured.

One overview image (a strip of poses and a speed graph per animation, with
the findings and the questions), and one page per animation:

  - a filmstrip of evenly spaced poses from the game's side, and from the
    side on with the ground line (anything under the ground is red, triangles
    stretched or collapsed beyond the profile's limits are magenta), and the
    consecutive frames around the moment the game acts on (or the fastest
    moment), where a glitch would be;
  - the seam pair of a loop, or the hand-overs of a one-shot: the two poses
    that must be one, drawn over each other in blue and orange (where they
    agree the picture is grey);
  - the trails of the extremities: a dot per frame, so arcs show as curves and
    speed as the spacing of the dots;
  - the speed of the fastest point over time, with the moments the game acts
    on, the lowest point of the mesh against the ground, and any pop;
  - with --engine, shots of the real renderer at the same moments as some of
    the filmstrip's poses.

The software renderer is modelkit's idea (painter's algorithm with PIL),
adapted to draw a skinned pose inside a fixed frame so that poses can be
compared.
"""
import json
import math
import os
import re
import shutil
import subprocess
import tempfile

import numpy as np
from PIL import Image, ImageDraw

from kitcommon import kit as common

from . import checks

BACKGROUND = (38, 40, 52)
TRAIL_COLOURS = [(255, 120, 90), (110, 200, 255), (255, 215, 90), (150, 255, 150), (240, 140, 255), (255, 255, 255)]


def _basis(yaw, pitch):
    """Camera axes for a view (degrees): yaw 0 looks at the model's front (+Z), pitch looks down on it."""
    y, p = math.radians(yaw), math.radians(pitch)
    forward = np.array([-math.sin(y) * math.cos(p), -math.sin(p), -math.cos(y) * math.cos(p)])
    right = np.array([math.cos(y), 0.0, -math.sin(y)])
    up = np.cross(right, forward)
    return right, up / np.linalg.norm(up), forward


class Camera:
    """An orthographic view fitted to a fixed box, so every pose of a strip is drawn at one scale and place."""

    def __init__(self, view, size, low, high, margin=0.05, fit=None):
        """`fit` (points, any shape ending in 3): frame what those points cover in this view, not the whole box."""
        self.right, self.up, self.forward = _basis(*view)
        self.size = size
        self.centre = (np.asarray(low) + np.asarray(high)) / 2
        if fit is None:
            fit = np.array([[a, b, c] for a in (low[0], high[0]) for b in (low[1], high[1]) for c in (low[2], high[2])])
        relative = np.asarray(fit).reshape(-1, 3) - self.centre
        x, y = relative @ self.right, relative @ self.up
        self.scale = min(size[0] / max(np.ptp(x), 1e-6), size[1] / max(np.ptp(y), 1e-6)) / (1 + 2 * margin)
        self.mid = ((x.max() + x.min()) / 2, (y.max() + y.min()) / 2)

    def project(self, points):
        relative = np.asarray(points) - self.centre
        px = (relative @ self.right - self.mid[0]) * self.scale + self.size[0] / 2
        py = self.size[1] / 2 - (relative @ self.up - self.mid[1]) * self.scale
        return px, py, relative @ self.forward


class Surface:
    """What does not change from pose to pose: triangles, their colours, which are double sided."""

    def __init__(self, subject):
        rig = subject.rig
        self.faces = rig.faces
        colours, two_sided = [], []
        for part in rig.parts:
            primitive = part.primitive
            material = rig.model.materials[primitive.material] if 0 <= primitive.material < len(rig.model.materials) else {}
            two_sided.append(np.full(len(primitive.indices), bool(material.get("doubleSided"))))
            colours.append(_albedo(rig.model, primitive, material))
        self.albedo = np.concatenate(colours) if colours else np.zeros((0, 3))
        self.two_sided = np.concatenate(two_sided) if two_sided else np.zeros(0, dtype=bool)


def _albedo(model, primitive, material):
    """A colour per face: modelkit's (the base colour texture at the face's centre, plus glow), or the flat factor."""
    try:
        from modelkit import render
        return render._albedo(model, primitive)
    except Exception:  # noqa: BLE001 - the sheet must still draw if modelkit's renderer changes
        factor = np.array(material.get("pbrMetallicRoughness", {}).get("baseColorFactor", [0.7, 0.7, 0.7, 1])[:3])
        return np.tile(factor, (len(primitive.indices), 1))


def draw(surface, points, camera, mode="colour", mark=None, tint=(1.0, 1.0, 1.0), background=BACKGROUND):
    """One pose. mode "colour": the model's own colours, lit; "clay": plain
    grey, lit. `mark` is {(r, g, b): boolean mask over faces}: faces painted
    flat in that colour whatever the light."""
    image = Image.new("RGB", camera.size, background)
    faces = surface.faces
    if not len(faces):
        return image
    px, py, depth = camera.project(points)
    corners = points[faces]
    cross = np.cross(corners[:, 1] - corners[:, 0], corners[:, 2] - corners[:, 0])
    with np.errstate(invalid="ignore", divide="ignore"):
        normals = np.nan_to_num(cross / np.linalg.norm(cross, axis=1, keepdims=True))
    facing = normals @ -camera.forward
    light = -camera.forward * 0.6 + camera.up * 0.65 - camera.right * 0.4
    light /= np.linalg.norm(light)
    shading = np.where((surface.two_sided & (facing < 0))[:, None], -normals, normals)
    lit = np.clip(shading @ light, 0, 1) * 0.8 + np.clip(shading @ -camera.forward, 0, 1) * 0.1 + 0.16
    base = surface.albedo if mode == "colour" else np.tile([0.82, 0.8, 0.78], (len(faces), 1))
    colours = base * (0.5 + 0.65 * lit)[:, None] if mode == "colour" else base * lit[:, None]
    colours = colours * np.asarray(tint)
    for colour, mask in (mark or {}).items():
        colours[mask] = np.asarray(colour) / 255.0
    rgb = (np.clip(colours, 0, 1) * 255).astype(np.uint8)
    visible = (facing >= 0) | surface.two_sided
    order = np.argsort(-depth[faces].mean(axis=1))
    order = order[visible[order]]
    xy = np.stack([px, py], axis=1)[faces]
    painter = ImageDraw.Draw(image)
    for index in order.tolist():
        t = xy[index]
        painter.polygon([(t[0, 0], t[0, 1]), (t[1, 0], t[1, 1]), (t[2, 0], t[2, 1])], fill=tuple(int(v) for v in rgb[index]))
    return image


def _ground_line(image, camera, low, high):
    """The ground (y = 0) as a line across a side-on view."""
    _, py, _ = camera.project(np.array([[0.0, 0.0, 0.0]]))
    y = float(py[0])
    if 0 <= y < image.height:
        ImageDraw.Draw(image).line([(0, y), (image.width, y)], fill=(120, 230, 140), width=1)
    return image


def _frame(take, subject):
    """The box a clip's pictures are fitted to: everything the mesh reaches in it."""
    points = take.points
    low, high = points.min(axis=(0, 1)), points.max(axis=(0, 1))
    low[1] = min(low[1], 0.0)
    pad = (high - low).max() * 0.02
    return low - pad, high + pad


def _cover(take, ground=False):
    """Points that span everything a clip's mesh reaches (a thinned copy of its poses), plus the ground under it."""
    points = take.points[:, ::max(1, take.points.shape[1] // 1500)].reshape(-1, 3)
    if ground:
        low, high = points.min(axis=0), points.max(axis=0)
        points = np.concatenate([points, [[low[0], 0.0, low[2]], [high[0], 0.0, high[2]]]])
    return points


def _marks(subject, take, index, profile, strain):
    """Faces to paint: under the ground (red), stretched or collapsed (magenta)."""
    points = take.points[index]
    faces = subject.rig.faces
    out = {}
    if profile.get("ground") and take.spec.get("grounded", True) is not False:
        under = (points[:, 1] < -profile.get("ground_tolerance", 0.015) * subject.size)[faces].any(axis=1)
        if under.any():
            out[(235, 40, 50)] = under
    if strain is not None:
        stretch, shrink, longest = strain.at(points)
        bad = ((stretch > profile.get("max_edge_stretch", 2.5)) & (longest > profile.get("stretch_min_size_fraction", 0.03) * subject.size)) \
            | (shrink < profile.get("min_area_ratio", 0.05))
        if bad.any():
            out[(255, 60, 255)] = bad
    return out


def _strip(surface, subject, take, profile, view, indices, cell, ground=False, marks=True, strain=None):
    low, high = _frame(take, subject)
    camera = Camera(view, cell, low, high, fit=_cover(take, ground))
    strip = Image.new("RGB", (cell[0] * len(indices), cell[1]), BACKGROUND)
    for column, index in enumerate(indices):
        mark = _marks(subject, take, index, profile, strain) if marks else None
        tile = draw(surface, take.points[index], camera, mark=mark)
        if ground:
            _ground_line(tile, camera, low, high)
        painter = ImageDraw.Draw(tile)
        painter.text((4, cell[1] - 13), f"{take.sample_times[index]:.2f} s", fill=(255, 255, 255))
        strip.paste(tile, (column * cell[0], 0))
    return strip


def _overlay(surface, subject, take, view, size, pose_a, pose_b, caption):
    """Two poses that should be one: the first in blue, the second in orange; grey where they agree."""
    both = np.concatenate([pose_a, pose_b])
    low, high = both.min(axis=0), both.max(axis=0)
    pad = (high - low).max() * 0.03
    camera = Camera(view, size, low - pad, high + pad)
    a = np.asarray(draw(surface, pose_a, camera, mode="clay", tint=(0.25, 0.75, 1.0), background=(0, 0, 0)), dtype=np.float64)
    b = np.asarray(draw(surface, pose_b, camera, mode="clay", tint=(1.0, 0.6, 0.2), background=(0, 0, 0)), dtype=np.float64)
    image = Image.fromarray(np.clip(a * 0.62 + b * 0.62 + 18, 0, 255).astype(np.uint8))
    gap = float(np.linalg.norm(pose_a - pose_b, axis=1).max())
    common.label(image, caption)
    ImageDraw.Draw(image).text((4, size[1] - 13), f"largest gap {gap:.3f} units ({100 * gap / subject.size:.1f}% of its size)",
                               fill=(255, 255, 255))
    return image


def _trails(surface, subject, take, view, size):
    """Where the extremities go: the joints that travel furthest, a dot per frame over a ghost of the middle pose."""
    low, high = _frame(take, subject)
    camera = Camera(view, size, low, high, fit=_cover(take))
    middle = take.points[len(take.points) // 2]
    ghost = np.asarray(draw(surface, middle, camera, mode="clay"), dtype=np.float64)
    image = Image.fromarray((ghost * 0.35 + np.array(BACKGROUND) * 0.65).astype(np.uint8))
    painter = ImageDraw.Draw(image)
    tracked = take.tracked
    if tracked.shape[1]:
        rig = subject.rig
        children = {int(parent) for parent in rig.parent if parent >= 0}
        travel = np.linalg.norm(np.diff(tracked, axis=0), axis=2).sum(axis=0)
        tips = [p for p in np.argsort(-travel) if int(subject.follow_node[p]) not in children or travel[p] >= travel.max() * 0.8]
        for rank, point in enumerate(tips[:len(TRAIL_COLOURS)]):
            px, py, _ = camera.project(tracked[:, point])
            colour = TRAIL_COLOURS[rank]
            path = list(zip(px.tolist(), py.tolist()))
            if len(path) > 1:
                painter.line(path, fill=tuple(int(c * 0.6) for c in colour), width=1)
            for x, y in path:
                painter.ellipse([x - 1.5, y - 1.5, x + 1.5, y + 1.5], fill=colour)
            painter.ellipse([path[0][0] - 3, path[0][1] - 3, path[0][0] + 3, path[0][1] + 3], outline=(255, 255, 255))
            painter.text((6, 20 + rank * 12), rig.node_name(subject.follow_node[point]), fill=colour)
    common.label(image, "trails of the extremities: a dot per frame, a ring at the start")
    return image


def _graph(subject, take, profile, size, pops=(), small=False):
    """Speed of the fastest point over time; the lowest point of the mesh against the ground; pops and the game's moments.
    The scale never shows less than half the model's size a second, so a still animation draws a flat line, not its noise."""
    image = Image.new("RGB", size, (26, 27, 36))
    painter = ImageDraw.Draw(image)
    left, right, top = (4, 4, 20) if small else (46, 10, 34)
    band = 0 if small else 44
    bottom = size[1] - (4 if small else 20) - band
    speed = take.speed()
    duration = max(take.clip.duration, 1e-6)

    def x_of(seconds):
        return left + (size[0] - left - right) * seconds / duration

    speed = np.nan_to_num(speed)
    peak = float(speed.max()) if len(speed) else 0.0
    full = max(peak, 0.5 * subject.size)
    scale = (bottom - top) / (full * 1.05)
    if not small:
        for fraction in (0.0, 0.5, 1.0):
            y = bottom - full * fraction * scale
            painter.line([(left, y), (size[0] - right, y)], fill=(52, 54, 68))
            painter.text((2, y - 6), f"{full * fraction:.1f}", fill=(150, 154, 170))
        ticks = max(1, int(math.ceil(duration / 12)))
        for second in range(0, int(duration) + 1, ticks):
            painter.line([(x_of(second), top), (x_of(second), bottom)], fill=(52, 54, 68))
            painter.text((x_of(second) + 2, bottom + 3), f"{second} s", fill=(150, 154, 170))
    for moment in take.spec.get("impacts") or []:
        painter.line([(x_of(moment), top), (x_of(moment), bottom)], fill=(255, 215, 90), width=1)
        if not small:
            painter.text((x_of(moment) + 3, top), f"game acts {moment:g} s", fill=(255, 215, 90))
    if len(speed):
        steps = take.steps()
        typical = np.nan_to_num(np.median(np.linalg.norm(steps, axis=2), axis=1) * take.rate) if steps.shape[1] else speed * 0
        for series, colour in ((typical, (120, 150, 200)), (speed, (235, 238, 250))):   # the body as a whole under its fastest part
            path = []
            for index, value in enumerate(series):
                path += [(x_of(take.times[index]), bottom - value * scale), (x_of(take.times[index + 1]), bottom - value * scale)]
            painter.line(path, fill=colour, width=1)
    for index, _, _, _ in pops:
        x = x_of(take.times[min(index, len(take.times) - 1)])
        painter.line([(x, top), (x, bottom)], fill=(255, 70, 70), width=2)
    if band and take.points is not None and profile.get("ground"):
        lowest = take.points[:, :, 1].min(axis=1)
        base = size[1] - 4
        middle = base - (band - 14) / 2
        span = max(float(np.abs(lowest).max()), 0.03 * subject.size)
        painter.text((left + 2, base - band + 1), f"lowest point of the mesh against the ground (green line): {lowest.min():+.2f} to "
                                                  f"{lowest.max():+.2f} units", fill=(150, 154, 170))
        painter.line([(left, middle), (size[0] - right, middle)], fill=(120, 230, 140))
        path = [(x_of(t), middle - ((band - 14) / 2 - 2) * value / span) for t, value in zip(take.sample_times, lowest)]
        if len(path) > 1:
            under = profile.get("ground") and lowest.min() < -profile.get("ground_tolerance", 0.015) * subject.size
            painter.line(path, fill=(255, 110, 110) if under else (225, 228, 240), width=1)
    if small:
        painter.text((6, 20), f"peak {peak:.1f} u/s", fill=(150, 154, 170))
    else:
        painter.text((size[0] - right - 150, bottom - 13), "yellow: game acts   red: pop", fill=(150, 154, 170))
        common.label(image, f"speed, units/s: fastest point (white, peak {peak:.1f} at "
                            f"{take.times[int(np.argmax(speed))] if len(speed) else 0:.2f} s), typical point (blue)")
    return image


class Strain:
    """Stretch and collapse of every triangle against the rest pose (as checks.check_deform measures them)."""

    def __init__(self, subject, take):
        rig = subject.rig
        self.faces = rig.faces
        corners = subject.rest[self.faces]
        self.edges = np.stack([np.linalg.norm(corners[:, (i + 1) % 3] - corners[:, i], axis=1) for i in range(3)], axis=1)
        self.area = np.linalg.norm(np.cross(corners[:, 1] - corners[:, 0], corners[:, 2] - corners[:, 0]), axis=1) / 2
        self.free = checks.free_faces(subject, take, self.edges, self.area)

    def at(self, points):
        return checks.strain(points, self.faces, self.edges, self.area, self.free)


def engine_shots(path, engine, subject, profile, plan):
    """Shots of the animations in the real renderer on a neutral stage.
    `plan` is [(take, [times])]; returns {take name: [(image, caption)]}."""
    here = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    work = tempfile.mkdtemp(prefix="animkit-shots-")
    game = os.path.join(work, "game")
    shutil.copytree(os.path.join(here, "stage"), game)
    name = "model" + os.path.splitext(path)[1]
    os.makedirs(os.path.join(game, "models"), exist_ok=True)
    shutil.copy(path, os.path.join(game, "models", name))
    yaw, pitch = profile.get("game_view", [0, 25])
    height = float(max(0.5, subject.high[1] - subject.low[1]))
    reach = max(float(np.abs(take.points - [0, height / 2, 0]).max()) for take, _ in plan if take.points is not None)
    shots, tick = [], 20
    for take, times in plan:
        for at in times:
            steps = max(1, int(math.ceil(at * 60 / 900.0)))
            tick += steps + 10
            shots.append({"yaw": yaw, "pitch": pitch, "animation": take.name, "at": float(at), "steps": steps, "tick": tick,
                          "take": take.name})
    # A hero's height beside it: to the camera's right of everything the model reaches, a little towards the camera.
    right, _, forward = _basis(yaw, 0)
    aside = max(float((np.concatenate([take.points.reshape(-1, 3) for take, _ in plan if take.points is not None]) @ right).max()), 0.5) + 0.8
    hero = right * aside - forward * 0.5
    # A model whose origin is not at its base (a floating set piece) is lifted to stand on the stage.
    lowest = min(float(take.points[:, :, 1].min()) for take, _ in plan if take.points is not None)
    lift = 0.0 if profile.get("ground") else max(0.0, -lowest)
    with open(os.path.join(game, "shots.json"), "w", encoding="utf-8") as out:
        json.dump({"model": "models/" + name, "height": height, "lift": lift, "target_y": float(lift + subject.low[1] + height * 0.5),
                   "distance": max(6.0, reach * 4.6), "hero_x": float(hero[0]), "hero_z": float(hero[2]), "shots": shots}, out)
    user = os.path.join(work, "user")
    try:
        subprocess.run(common.windowed([engine, game, "--mute", "--user-data", user]), capture_output=True, text=True, timeout=900)
    except (OSError, subprocess.TimeoutExpired) as error:
        print(f"  (no engine shots: {error})")
        shutil.rmtree(work, ignore_errors=True)
        return {}
    reached = {}
    times_file = os.path.join(user, "files", "times.json")
    if os.path.isfile(times_file):
        with open(times_file, encoding="utf-8") as file:
            listed = json.load(file).get("times", [])
        if isinstance(listed, dict):        # a table with holes arrives as {"#1": ...}
            listed = [listed.get(f"#{index}") for index in range(1, len(shots) + 1)]
        reached = dict(enumerate(listed, 1))
    images = {}
    for index, shot in enumerate(shots, 1):
        file = os.path.join(user, "screenshots", f"shot_{index}.png")
        if os.path.isfile(file):
            image = Image.open(file).convert("RGB")
            w, h = image.size
            at = reached.get(index)
            caption = f"engine: {shot['take']} at {at:.2f} s" if isinstance(at, (int, float)) else f"engine: {shot['take']} at {shot['at']:.2f} s (asked)"
            images.setdefault(shot["take"], []).append((image.crop(((w - h) // 2, 0, (w + h) // 2, h)), caption))
    if not images:
        print("  (the engine wrote no shots)")
    shutil.rmtree(work, ignore_errors=True)
    return images


class Sheet:
    """The overview and its pages. kitcommon saves what `sheet` returns with
    .save(path): this writes the overview there and each animation's page
    beside it as <name>_<animation>.png."""

    def __init__(self, overview, pages):
        self.overview, self.pages = overview, pages

    def save(self, path):
        self.overview.save(path)
        # golem.png -> golem_idle.png; next to a model, golem.review.png -> golem_idle.review.png (git ignores *.review.png).
        ending = ".review.png" if path.lower().endswith(".review.png") else os.path.splitext(path)[1]
        stem = path[:len(path) - len(ending)]
        for name, image in self.pages:
            target = f"{stem}_{re.sub(r'[^A-Za-z0-9_.-]', '-', name) or 'unnamed'}{ending}"
            image.save(target)
            print(f"wrote {target}")


def review(path, profile, findings, subject, lines, engine):
    surface = Surface(subject)
    poses = int(profile.get("review_poses", 8))
    game_view = tuple(profile.get("game_view", [0, 25]))
    side_view = tuple(profile.get("ground_view", [90, 0]))
    usable = [take for take in subject.takes if take.points is not None]
    shots = {}
    if engine and usable:
        plan = []
        for take in usable:
            indices = np.unique(np.round(np.linspace(0, len(take.points) - 1, poses)).astype(int))
            picked = indices[np.unique(np.round(np.linspace(0, len(indices) - 1, 4)).astype(int))]
            plan.append((take, [float(take.sample_times[i]) for i in picked]))
        shots = engine_shots(path, engine, subject, profile, plan)
    pages, rows = [], []
    for take in subject.takes:
        where = checks._clip(take.name)
        mine = [f for f in findings if f.where == where and f.severity != "info"]
        if take.points is None:
            tile = Image.new("RGB", (1200, 60), BACKGROUND)
            common.label(tile, f"'{take.name}': nothing to draw (see the findings)")
            rows.append(tile)
            continue
        indices = np.unique(np.round(np.linspace(0, len(take.points) - 1, poses)).astype(int))
        pops = checks.pops_of(subject, take, profile)
        strain = Strain(subject, take) if len(subject.rig.faces) else None
        # --- the page
        grounded = bool(profile.get("ground"))
        cell = (200, 210)
        width = cell[0] * poses
        strips = [
            common.label(_pad(_strip(surface, subject, take, profile, game_view, indices, cell, strain=strain), width),
                         f"'{take.name}' as the game shows it (view {game_view[0]:g}, {game_view[1]:g}); magenta: stretched or collapsed triangles"),
            common.label(_pad(_strip(surface, subject, take, profile, side_view, indices, cell, ground=grounded, strain=strain), width),
                         f"'{take.name}' level with the ground (view {side_view[0]:g}, {side_view[1]:g}); green line: the ground; red: under it"
                         if grounded else f"'{take.name}' from the side (view {side_view[0]:g}, {side_view[1]:g})"),
        ]
        # Frame by frame where it matters most: around the first moment the game acts on, or the fastest moment.
        speed = np.nan_to_num(take.speed())
        moments = take.spec.get("impacts") or []
        if len(take.points) == len(take.times) and len(take.times) > poses:
            if moments:
                centre, what = int(np.argmin(np.abs(take.times - moments[0]))), f"the moment the game acts on ({moments[0]:g} s)"
            else:
                centre = int(np.argmax(speed)) + 1 if len(speed) else 0
                what = f"its fastest moment ({take.times[centre]:.2f} s)"
            first = min(max(centre - (poses - 3), 0), len(take.times) - poses)
            strips.append(common.label(_pad(_strip(surface, subject, take, profile, game_view, list(range(first, first + poses)), cell,
                                                   strain=strain), width),
                                       f"'{take.name}' frame by frame ({take.rate:g} a second) around {what}"))
        tiles = []
        tile = (width // 4, 300)
        three_quarter = ((game_view[0] + 35) % 360, max(game_view[1], 12))
        if take.loop:
            tiles.append(_overlay(surface, subject, take, three_quarter, tile, take.first, take.last,
                                  "the seam: first pose (blue) under last pose (orange)"))
        for key in ("from", "to"):
            other_name, fade = checks._handover(take.spec.get(key))
            other = subject.take(other_name) if other_name else None
            if other is None or other.points is None:
                continue
            how = "cut" if fade is not None and fade <= 0 else "cross-faded"
            if key == "from":
                tiles.append(_overlay(surface, subject, take, three_quarter, tile, other.first if other.loop else other.last, take.first,
                                      f"from '{other_name}' ({how}): its pose (blue), this start (orange)"))
            else:
                tiles.append(_overlay(surface, subject, take, three_quarter, tile, take.last, other.first,
                                      f"to '{other_name}' ({how}): this end (blue), its start (orange)"))
        tiles = tiles[:2]
        tiles.append(_trails(surface, subject, take, three_quarter, tile))
        graph_width = width - tile[0] * len(tiles)
        tiles.append(_graph(subject, take, profile, (graph_width, tile[1]), pops))
        row = Image.new("RGB", (width, tile[1]), BACKGROUND)
        x = 0
        for image in tiles:
            row.paste(image, (x, 0))
            x += image.width
        parts = strips + [row]
        if take.name in shots:
            shot_size = width // max(4, len(shots[take.name]))
            engine_row = Image.new("RGB", (width, shot_size), BACKGROUND)
            for column, (image, caption) in enumerate(shots[take.name]):
                engine_row.paste(common.label(image.resize((shot_size, shot_size)), caption), (column * shot_size, 0))
            parts.append(engine_row)
        notes = [f"{'waived' if f.waived else f.severity}: {f.check}: {f.message}" for f in mine] or ["no findings in this animation"]
        spec = ", ".join(f"{key}: {json.dumps(value)}" for key, value in take.spec.items()) if take.declared else "no spec"
        header = [f"{os.path.basename(path)}  '{take.name}'  {take.clip.duration:.2f} s, keyed {take.rate:g} times a second, "
                  f"{take.clip.keys:,} keys on {len(take.clip.channels)} channels", f"spec: {spec}"]
        text = Image.new("RGB", (width, 16 * (len(header) + sum(len(common.wrap(note, 230)) for note in notes)) + 10), (20, 21, 28))
        painter = ImageDraw.Draw(text)
        y = 4
        for line in header:
            painter.text((6, y), line, fill=(255, 220, 140))
            y += 16
        for note in notes:
            colour = (255, 120, 120) if note.startswith("error") else (255, 210, 120) if note.startswith("warn") else (200, 204, 216)
            for wrapped in common.wrap(note, 230):
                painter.text((6, y), wrapped, fill=colour)
                y += 16
        page = Image.new("RGB", (width, sum(part.height for part in parts) + text.height), BACKGROUND)
        y = 0
        for part in [text] + parts:
            page.paste(part, (0, y))
            y += part.height
        pages.append((take.name, page))
        # --- its row of the overview
        small = (150, 150)
        picked = indices[np.unique(np.round(np.linspace(0, len(indices) - 1, 6)).astype(int))]
        strip = _strip(surface, subject, take, profile, game_view, picked, small, strain=strain)
        graph = _graph(subject, take, profile, (300, small[1]), pops, small=True)
        tile = Image.new("RGB", (strip.width + graph.width, small[1]), BACKGROUND)
        tile.paste(strip, (0, 0))
        tile.paste(graph, (strip.width, 0))
        state = "error" if any(f.severity == "error" and not f.waived for f in mine) else \
            "warn" if any(not f.waived for f in mine) else "waived" if mine else "clean"
        common.label(tile, f"'{take.name}'  {take.clip.duration:.2f} s  {'loop' if take.loop else 'one-shot' if take.declared else 'no spec'}"
                           f"  [{state}]   poses as the game shows it, then speed over time (page: _{take.name}.png)")
        rows.append(tile)
    if not rows:
        rows.append(common.label(Image.new("RGB", (1200, 60), BACKGROUND), "no animations"))
    title = f"{os.path.relpath(path)}: animations ({len(pages)} pages beside this image)"
    return Sheet(common.compose(rows, title, _overview_lines(lines, findings), columns=1), pages)


def _overview_lines(lines, findings):
    """kitcommon's side text, with each finding named by its animation and check (a page has it in full)."""
    head = [line for line in lines[:2]]
    tail = lines[next((index for index, line in enumerate(lines) if line.startswith("Judge against")), len(lines)):]
    groups, _ = common.verdict(findings, [group for group, _ in checks.GROUPS])
    body = [""]
    for group, text in checks.GROUPS:
        body.append(f"[{groups[group].upper()}] {text}")
        for finding in findings:
            if finding.group == group and finding.severity != "info":
                where = (finding.where or "").replace("animation ", "")
                count = f" ({finding.count:,})" if finding.count else ""
                body.append(f"{'waived' if finding.waived else finding.severity}: {where} {finding.check}{count}: {finding.message}"[:84])
    return head + body + [""] + tail


def _pad(image, width):
    if image.width == width:
        return image
    out = Image.new("RGB", (width, image.height), BACKGROUND)
    out.paste(image, (0, 0))
    return out
