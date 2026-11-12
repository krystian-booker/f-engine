"""map.json (format 2) of the chamber and the ruins: grid rows that match the
geometry, points, lights, emitters and the environment (light, fog, sky, post)."""
import json
import math
import os
from collections import deque
from layout import (SIZE, CX, CZ, RADIUS, DX, DZ, DAIS, DAIS_TOP, RING, PASSAGE, POINTS, RUINS_POINTS, FACING, CONDUITS,
                    CRATER_R, crater_depth, polar, ring_point)
from paint import linear, write_cube
import numpy as np

FX = "fx/chamber/"


def lin(hex_colour, scale=1.0):
    return linear(hex_colour, scale)


# ---------------------------------------------------------------------------- grid

def grid_rows(ruined, blocked):
    heights = [[0.0] * SIZE for _ in range(SIZE)]
    walk = [[False] * SIZE for _ in range(SIZE)]
    x0, z0, x1, z1 = PASSAGE
    for z in range(SIZE):
        for x in range(SIZE):
            corners = [math.hypot(cx - CX, cz - CZ) for cx in (x, x + 1) for cz in (z, z + 1)]
            inside = max(corners) < RADIUS - 0.25
            passage = x0 <= x < x1 and z0 <= z < z1
            walk[z][x] = (inside or passage) and (x, z) not in blocked
            if not ruined and x == int(DX) and z in (int(DZ) - 1, int(DZ)):
                walk[z][x] = True     # the Colossus's own footing: an island on the dais that no path reaches
            d = math.hypot(x + 0.5 - DX, z + 0.5 - DZ)
            if ruined:
                heights[z][x] = -round(crater_depth(d) * 20) / 20
            else:
                for r, h in DAIS:
                    if d < r:
                        heights[z][x] = h
    return heights, walk


def reachable(walk, start):
    seen = {start}
    queue = deque([start])
    while queue:
        x, z = queue.popleft()
        for dx, dz in ((1, 0), (-1, 0), (0, 1), (0, -1)):
            nx, nz = x + dx, z + dz
            if 0 <= nx < SIZE and 0 <= nz < SIZE and walk[nz][nx] and (nx, nz) not in seen:
                seen.add((nx, nz))
                queue.append((nx, nz))
    return seen


def points_json(ruined, heights, walk):
    names = dict(POINTS)
    if ruined:
        names.update(RUINS_POINTS)
    out = {}
    seen = reachable(walk, (int(names["start"][0]), int(names["start"][1])))
    problems = []
    for name, (x, z, facing) in names.items():
        cell = (int(math.floor(x)), int(math.floor(z)))
        y = heights[cell[1]][cell[0]]
        if name == "boss" and not ruined:
            y = DAIS_TOP          # on the dais, which nothing walks on
        elif cell not in seen:
            problems.append(name)
        out[name] = {"position": [x, y, z], "facing": list(FACING[facing]), "layer": 0}
    if problems:
        raise SystemExit(f"points not on walkable ground reachable from start: {problems}")
    return out


# ---------------------------------------------------------------------------- lights and emitters

def light(position, colour, intensity, radius, flicker=0.0, speed=6.0, curve=None, shadows=False, spot=None):
    out = {"type": "point", "position": [round(v, 3) for v in position], "color": colour, "intensity": intensity,
           "radius": radius, "flicker": flicker, "flicker_speed": speed, "flicker_curve": curve or [], "shadows": shadows}
    if spot:
        out.update(type="spot", direction=spot[0], inner_cone=spot[1], outer_cone=spot[2])
    return out


def emitter(position, sprite, rate, lifetime, size, colour, colour_end, velocity=(0, 0, 0), jitter=(0, 0, 0), area=(0, 0, 0),
            size_end=None, blend="additive", unlit=True, gravity=(0, 0, 0), drag=0.0, life_jitter=0.0, burst=0, maximum=256,
            distort=0.0, stretch=0.0):
    return {"position": [round(v, 3) for v in position], "sprite": (FX + sprite + ".anim.json") if sprite else "", "rate": rate,
            "burst": burst, "lifetime": lifetime, "lifetime_jitter": life_jitter, "velocity": list(velocity),
            "velocity_jitter": list(jitter), "gravity": list(gravity), "area": list(area), "size": size,
            "size_end": size if size_end is None else size_end, "drag": drag, "stretch": stretch, "color": list(colour),
            "color_end": list(colour_end), "blend": blend, "unlit": unlit, "world_space": True, "distort": distort,
            "max": maximum}


FIRELIGHT = [1.0, 0.5, 0.2]
VIOLET = [0.5, 0.25, 1.0]


def brazier_fx(x, y, z, scale=1.0):
    return [
        emitter((x, y + 0.12, z), "flame", 16, 0.75, 0.42 * scale, (2.6, 2.0, 1.5, 1), (1.6, 0.5, 0.15, 0), (0, 1.25, 0),
                (0.18, 0.3, 0.18), (0.2, 0.04, 0.2), size_end=0.16 * scale, life_jitter=0.2, maximum=40),
        emitter((x, y + 0.5, z), "ember", 3.5, 2.6, 0.05, (3.0, 1.6, 0.6, 1), (2.0, 0.4, 0.1, 0), (0, 1.1, 0), (0.45, 0.5, 0.45),
                (0.25, 0.1, 0.25), size_end=0.02, gravity=(0, -0.08, 0), drag=0.2, life_jitter=0.9, maximum=24),
        emitter((x, y + 0.2, z), "glow", 0.6, 3.0, 1.5 * scale, (0.9, 0.42, 0.14, 0.13), (0.9, 0.42, 0.14, 0.0), (0, 0.05, 0),
                life_jitter=0.5, burst=2, maximum=6),
        emitter((x, y + 0.7, z), "haze", 5, 1.3, 0.5, (1, 1, 1, 0.8), (1, 1, 1, 0.0), (0, 0.9, 0), (0.12, 0.2, 0.12),
                (0.15, 0.05, 0.15), size_end=0.9, blend="alpha", distort=0.35, maximum=12),
    ]


def chamber_lighting(info, fires):
    lights, emitters = [], []
    # Braziers: pools of flickering firelight; the two that stand behind the action cast shadows.
    for i, (x, y, z) in enumerate(fires):
        behind = (x, z) in ((18.6, 10.6), (13.2, 8.6))
        lights.append(light((x, y + 0.75, z), FIRELIGHT, 3.3 if z < 18 else 2.6, 8.2 if z < 18 else 7.0, 0.2, 7.5,
                            shadows=behind))
        emitters += brazier_fx(x, y, z)
    # The dais and the conduits: Aetherite light from below, round the Colossus rather than inside it.
    pulse = [[0.0, 1.0], [1.4, 1.28], [2.8, 1.0], [4.2, 0.86], [5.6, 1.0]]
    for a in (45, 135, 225, 315):
        x, z = ring_point(a, 3.2)
        lights.append(light((x, 1.0, z), VIOLET, 2.2, 6.0, 0.06, 2.0, curve=pulse))
    for a in (0, 90, 180, 270):
        x, z = ring_point(a)
        lights.append(light((x, 0.45, z), VIOLET, 1.1, 3.2, 0.05, 2.0, curve=pulse))
    for x, z in ((18.3, 16.0), (21.6, 16.0), (24.8, 16.0), (17.0, 8.3), (15.9, 11.6), (11.5, 9.4), (6.6, 11.3), (5.0, 16.0),
                 (6.0, 22.6), (11.5, 23.4), (11.5, 26.8), (17.3, 22.6), (17.4, 26.6)):
        lights.append(light((x, 0.4, z), VIOLET, 0.9, 2.9, 0.05, 2.0, curve=pulse))
    for x, y, z in info["slits"]:
        lights.append(light((x, y, z + 0.5), VIOLET, 1.3, 4.0, 0.05, 3.0))
    # Machinery: the furnace; and the burning fortress above, down the stair and through the door.
    fx, fy, fz = info["furnace"]
    lights.append(light((fx, fy, fz + 0.6), [1.0, 0.36, 0.1], 2.8, 6.0, 0.25, 9.0))
    lights.append(light((30.4, 2.0, 16.0), [1.0, 0.4, 0.14], 1.6, 5.5, 0.3, 5.0))
    for x, y, z in info["door_light"]:
        lights.append(light((x, y, z), [1.0, 0.38, 0.12], 3.0, 8.0, 0.3, 5.0))
    # Film light for the Colossus: a warm key from the front, a cold rim from behind, both from above.
    def aim(source, target):
        d = [t - s for s, t in zip(source, target)]
        length = math.sqrt(sum(v * v for v in d))
        return [round(v / length, 4) for v in d]
    key, rim = (19.0, 9.0, 22.0), (6.0, 12.0, 9.0)
    lights.append(light(key, [1.0, 0.56, 0.26], 6.0, 22.0, 0.08, 5.0, shadows=True, spot=(aim(key, (11.5, 3.4, 16.0)), 15.0, 25.0)))
    lights.append(light(rim, [0.42, 0.58, 1.0], 6.5, 22.0, shadows=True, spot=(aim(rim, (11.6, 4.6, 16.0)), 13.0, 22.0)))
    # A cold shaft of light down the tower onto the floor where the heroes stand.
    lights.append(light((21.0, 17.0, 12.0), [0.4, 0.55, 1.0], 4.2, 26.0, shadows=True,
                        spot=([0.02, -1.0, 0.22], 14.0, 24.0)))
    # ... and a soft warm bounce from the camera's side, so their faces read.
    lights.append(light((22.5, 3.2, 21.5), [1.0, 0.62, 0.36], 1.5, 9.0, 0.1, 5.0))

    # Air: dust in the firelight, embers, sparks off the conduits, steam from the pipes.
    emitters.append(emitter((CX, 3.6, CZ - 1), "dust", 26, 9.0, 0.035, (1.0, 0.85, 0.7, 0.55), (1.0, 0.85, 0.7, 0.0),
                            (0.05, 0.03, 0.02), (0.1, 0.06, 0.1), (12, 3.4, 11), unlit=False, blend="alpha", life_jitter=3.0,
                            burst=220, maximum=320))
    emitters.append(emitter((CX, 1.0, CZ - 2), "ember", 5, 7.0, 0.04, (2.6, 1.3, 0.5, 1), (1.6, 0.3, 0.1, 0), (0.05, 0.42, 0.0),
                            (0.2, 0.2, 0.2), (11, 0.8, 9), size_end=0.015, life_jitter=2.0, burst=24, maximum=64))
    emitters.append(emitter((DX, 0.3, DZ), "mote", 9, 4.2, 0.09, (1.6, 1.0, 2.6, 1), (1.2, 0.6, 2.4, 0), (0, 0.55, 0),
                            (0.12, 0.25, 0.12), (RING, 0.1, RING), size_end=0.03, life_jitter=1.2, burst=30, maximum=64))
    emitters.append(emitter((20.3, 0.15, 16.0), "mote", 4, 3.4, 0.08, (1.6, 1.0, 2.6, 1), (1.2, 0.6, 2.4, 0), (0, 0.4, 0),
                            (0.08, 0.2, 0.08), (4.8, 0.05, 0.15), size_end=0.02, life_jitter=1.0, burst=10, maximum=32))
    emitters.append(emitter((DX, DAIS_TOP + 0.4, DZ), "glow", 0.5, 4.0, 3.4, (0.5, 0.25, 1.0, 0.1), (0.5, 0.25, 1.0, 0.0),
                            (0, 0.05, 0), life_jitter=0.5, burst=2, maximum=6))
    emitters.append(emitter((fx, fy, fz), "ember", 4, 1.8, 0.045, (3.0, 1.6, 0.6, 1), (2.0, 0.4, 0.1, 0), (0, 0.5, 0.9),
                            (0.4, 0.4, 0.3), (0.6, 0.3, 0.05), size_end=0.02, gravity=(0, 0.3, 0), life_jitter=0.6, maximum=16))
    # Haze: slow, faint veils that take the colour of whatever light they drift through; they make the far
    # wall recede and show the cold shaft that falls on the heroes.
    for x, y, z, area, rate, size in ((16.0, 2.2, 8.2, (9.0, 1.6, 1.6), 0.9, 4.2), (11.5, 2.0, 16.0, (3.5, 1.4, 3.5), 0.5, 3.6),
                                      (21.3, 6.0, 14.0, (1.0, 5.0, 1.0), 1.4, 2.2), (16.0, 6.5, 6.5, (10.0, 2.0, 1.5), 0.6, 5.0)):
        emitters.append(emitter((x, y, z), "veil", rate, 12.0, size, (0.8, 0.85, 1.0, 0.1), (0.8, 0.85, 1.0, 0.1), (0.06, 0.03, 0.02),
                                (0.05, 0.03, 0.05), area, size_end=size * 1.3, blend="alpha", unlit=False, life_jitter=0.0,
                                maximum=int(rate * 14) + 2))
    for x, y, z in ((12.4, 7.6, 4.6), (19.6, 6.2, 4.6), (23.3, 5.0, 7.4)):
        emitters.append(emitter((x, y, z), "smoke", 1.6, 4.5, 0.5, (0.55, 0.6, 0.75, 0.3), (0.55, 0.6, 0.75, 0.0), (0.0, 0.5, 0.25),
                                (0.12, 0.12, 0.12), (0.1, 0.1, 0.1), size_end=1.6, blend="alpha", unlit=False, drag=0.25,
                                life_jitter=1.0, maximum=12))
    return lights, emitters


def chamber_environment():
    return {
        "ambient": [0.024, 0.028, 0.055],
        "sun_color": [0.07, 0.1, 0.2],
        "sun_direction": [0.16, -1.0, 0.36],
        "fog_color": [0.016, 0.019, 0.05],
        "fog_density": 0.026, "fog_height": -1.0, "fog_height_falloff": 0.03, "fog_max": 0.85,
        "shadows": True, "shadow_distance": 30.0, "shadow_strength": 0.85, "shadow_bias": 0.035,
        "sprite_light_height": 0.8, "anisotropic": True, "pixel_snap": False,
        "sky": {"mode": "color", "top": [0.003, 0.003, 0.01], "horizon": [0.003, 0.003, 0.01], "bottom": [0.003, 0.003, 0.01],
                "cloud_cover": 0.0, "backdrops": []},
        "weather": {"kind": "none", "intensity": 0.0},
        "post": {
            "bloom": True, "bloom_strength": 0.75, "bloom_threshold": 0.9, "bloom_knee": 0.6, "bloom_radius": 0.75,
            "bloom_levels": 6,
            "depth_of_field": True, "dof_mode": "tilt", "focus": 0.56, "focus_range": 0.27, "dof_radius": 7.0, "aperture": 1.2,
            "protect_sprites": True,
            "ssao": True, "ssao_radius": 0.7, "ssao_strength": 1.0, "ssao_bias": 0.03,
            "vignette": True, "vignette_strength": 0.6, "exposure": 1.15, "tonemap": "aces",
            "color_grade": True, "lut": "maps/chamber/grade.cube", "antialias": "auto", "distortion": 1.0,
        },
    }


def chamber_grade(c):
    """Deep indigo shadows, warm highlights, a little more contrast and colour."""
    luma = (c * [0.3, 0.59, 0.11]).sum(axis=1, keepdims=True)
    c = luma + (c - luma) * 1.12
    c = c + (0.5 - np.abs(c - 0.5)) * (c - 0.5) * 0.35           # gentle S curve
    shadow = np.clip(1 - luma * 3.2, 0, 1)
    high = np.clip(luma * 1.6 - 0.6, 0, 1)
    c = c + shadow * np.array([-0.004, 0.0, 0.022]) + high * np.array([0.03, 0.008, -0.03])
    return c


# ---------------------------------------------------------------------------- ruins

MOON = [0.3, 0.4, 0.72]


def ruins_lighting(info, fires):
    lights, emitters = [], []
    # Afterglow in the crater, a few dying embers of Aetherite along the split conduits.
    slow = [[0.0, 1.0], [2.5, 1.35], [5.0, 1.0], [7.0, 0.8], [9.0, 1.0]]
    lights.append(light((DX, 0.5, DZ), VIOLET, 1.5, 6.0, 0.05, 1.5, curve=slow))
    lights.append(light((DX + 0.6, -0.1, DZ - 0.3), [0.7, 0.45, 1.0], 0.9, 2.6, 0.1, 2.0, curve=slow))
    for x, z in ((17.0, 16.0), (11.5, 10.4), (8.0, 19.4)):
        lights.append(light((x, 0.3, z), VIOLET, 0.55, 2.6, 0.1, 2.0, curve=slow))
    # The first paleness of dawn in the north-east: a wide, faint, rosy rim on whatever faces it.
    lights.append(light((46.0, 16.0, -12.0), [0.95, 0.72, 0.9], 1.5, 70.0))
    # The last fire: one brazier still smouldering, almost out.
    for x, y, z in fires:
        lights.append(light((x, y + 0.5, z), FIRELIGHT, 1.1, 4.5, 0.35, 5.0))
        emitters.append(emitter((x, y + 0.1, z), "ember", 1.6, 2.4, 0.045, (2.6, 1.2, 0.4, 1), (1.6, 0.3, 0.1, 0), (0.1, 0.6, 0),
                                (0.2, 0.3, 0.2), (0.25, 0.05, 0.25), size_end=0.02, life_jitter=0.8, maximum=12))
        emitters.append(emitter((x, y + 0.3, z), "smoke", 1.2, 6.0, 0.4, (0.32, 0.36, 0.5, 0.3), (0.32, 0.36, 0.5, 0.0),
                                (0.25, 0.6, 0.05), (0.1, 0.1, 0.1), (0.1, 0.05, 0.1), size_end=1.8, blend="alpha", unlit=False,
                                drag=0.1, life_jitter=1.5, maximum=14))
    # Violet motes falling like snow through the whole space, to well above the tower stumps.
    emitters.append(emitter((CX, 13.0, CZ - 1.0), "mote", 120, 13.0, 0.085, (1.2, 0.8, 2.1, 1.0), (1.0, 0.55, 1.9, 0.0),
                            (0.06, -0.38, 0.03), (0.12, 0.12, 0.12), (21, 13, 19), size_end=0.06, life_jitter=4.0, burst=1400,
                            maximum=1900))
    # Thin smoke off the rubble, drifting east.
    for x, y, z in info.get("smoke", []):
        emitters.append(emitter((x, y, z), "smoke", 0.9, 11.0, 0.7, (0.2, 0.23, 0.36, 0.26), (0.2, 0.23, 0.36, 0.0),
                                (0.35, 0.55, 0.05), (0.12, 0.1, 0.1), (0.5, 0.1, 0.5), size_end=3.4, blend="alpha", unlit=False,
                                drag=0.05, life_jitter=3.0, burst=3, maximum=18))
    # Dust lifting off the crater in the afterglow.
    emitters.append(emitter((DX, 0.2, DZ), "mote", 5, 5.0, 0.08, (1.4, 0.9, 2.4, 1), (1.2, 0.6, 2.2, 0), (0, 0.3, 0),
                            (0.1, 0.15, 0.1), (2.6, 0.1, 2.6), size_end=0.02, life_jitter=1.5, burst=20, maximum=48))
    return lights, emitters


def ruins_environment():
    return {
        "ambient": [0.052, 0.068, 0.13],
        "sun_color": [0.72, 0.98, 1.75],
        "sun_direction": [0.84, -0.4, 0.3],           # a low moon in the west: long shadows across the floor
        "fog_color": [0.1, 0.115, 0.215],
        "fog_density": 0.011, "fog_height": 2.0, "fog_height_falloff": 0.035, "fog_max": 0.94,
        "shadows": True, "shadow_distance": 46.0, "shadow_strength": 0.9, "shadow_bias": 0.04,
        "sprite_light_height": 0.8, "anisotropic": True, "pixel_snap": False,
        "sky": {"mode": "gradient", "top": [0.012, 0.016, 0.07], "horizon": [0.27, 0.26, 0.43], "bottom": [0.05, 0.06, 0.12],
                "cloud_cover": 0.0, "backdrops": []},
        "weather": {"kind": "none", "intensity": 0.0},
        "post": {
            "bloom": True, "bloom_strength": 0.6, "bloom_threshold": 0.95, "bloom_knee": 0.6, "bloom_radius": 0.7,
            "bloom_levels": 6,
            "depth_of_field": True, "dof_mode": "tilt", "focus": 0.55, "focus_range": 0.36, "dof_radius": 5.0, "aperture": 1.2,
            "protect_sprites": True,
            "ssao": True, "ssao_radius": 0.7, "ssao_strength": 1.0, "ssao_bias": 0.03,
            "vignette": True, "vignette_strength": 0.5, "exposure": 1.3, "tonemap": "aces",
            "color_grade": True, "lut": "maps/ruins/grade.cube", "antialias": "auto", "distortion": 1.0,
        },
    }


def ruins_grade(c):
    """Cold and quiet: colour pulled towards blue, blacks lifted a little, violet kept."""
    luma = (c * [0.3, 0.59, 0.11]).sum(axis=1, keepdims=True)
    violet = np.clip((c[:, 2:3] - c[:, 1:2]) * 2.0, 0, 1) * np.clip((c[:, 0:1] - c[:, 1:2]) * 4.0, 0, 1)
    c = luma + (c - luma) * (0.86 + 0.3 * violet)
    c = c * 0.97 + 0.012
    shadow = np.clip(1 - luma * 2.5, 0, 1)
    c = c + shadow * np.array([-0.006, 0.002, 0.02]) + np.clip(luma - 0.5, 0, 1) * np.array([-0.01, 0.0, 0.02])
    return c


# ---------------------------------------------------------------------------- writing

def write_map(folder, ruined, builder):
    heights, walk = grid_rows(ruined, builder.blocked)
    points = points_json(ruined, heights, walk)
    if ruined:
        lights, emitters = ruins_lighting(builder.info, builder.fires)
        environment = ruins_environment()
        write_cube(os.path.join(folder, "grade.cube"), ruins_grade, title="ruins")
        props = {"music": "remains", "camera": {"distance": 20, "pitch": 24, "fov": 30}}
    else:
        lights, emitters = chamber_lighting(builder.info, builder.fires)
        environment = chamber_environment()
        write_cube(os.path.join(folder, "grade.cube"), chamber_grade, title="chamber")
        props = {"music": "colossus", "camera": {"distance": 22, "pitch": 22, "fov": 30}}
    grid = {"width": SIZE, "depth": SIZE, "tags": [],
            "layers": [{"name": "ground",
                        "height": [" ".join(f"{h:g}" for h in row) for row in heights],
                        "walk": ["".join("1" if w else "0" for w in row) for row in walk]}]}
    document = {"format": 2, "id": "ruins" if ruined else "chamber",
                "name": "The Chamber, Before Dawn" if ruined else "The Chamber of the Colossus",
                "geometry": "geometry.glb", "grid": grid, "points": points, "placements": [], "zones": [], "props": props,
                "lights": lights, "emitters": emitters, "colliders": [], "environment": environment}
    with open(os.path.join(folder, "map.json"), "w") as f:
        json.dump(document, f, indent=1)
    return document, walk
