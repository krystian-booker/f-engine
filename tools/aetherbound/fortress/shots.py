#!/usr/bin/env python3
"""Renders the fortress's review shots in the engine (through ../shot.py).

    python3 tools/aetherbound/fortress/shots.py <out folder> [names...] [--tick N] [--env '<json>']

Without names: the twelve standard shots (opening, bridge, gate, enc1,
enc1_battle, west, enc2, enc2_battle, east, north, stair, landing). Others:
opening30, opening_b, opening_c, opening_wide, opening_up, bridge_n,
enc1_battle_n, enc2_battle_n (battle cameras looking south), x_* (corners and
ends of each leg of the walk, for checking that nothing gets in the way),
top, tower, side. --tick is the frame captured (60 per second; particles
need about 150 to settle), --env overrides environment fields.
sheet.py <out folder> makes contact sheets of them.
"""
import json
import os
import subprocess
import sys
from concurrent.futures import ThreadPoolExecutor

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(os.path.dirname(HERE)))
GAME = os.path.join(ROOT, "samples", "aetherbound")


def sprite_for(i):
    names = ["cael", "rowan", "lyra", "darius"]
    real = os.path.join(GAME, "sprites", names[i] + ".anim.json")
    if os.path.exists(real):
        return "sprites/" + names[i] + ".anim.json"
    return f"fx/fortress/scale_{i + 1}.anim.json"


def enemy_sprite():
    return "sprites/guard.anim.json" if os.path.exists(os.path.join(GAME, "sprites", "guard.anim.json")) else \
        "fx/fortress/scale_2.anim.json"


def figure(i, x, y, z, facing=(0, -1), clip="idle", sprite=None):
    return {"position": [x, y, z], "facing": list(facing), "clip": clip,
            "spawn": {"sprite": {"animation": sprite or sprite_for(i), "mode": "standing", "cast_shadow": True}}}


def party(x, y, z, facing=(0, -1)):
    offs = [(0, 0), (-0.9, 0.7), (0.9, 0.9), (0.1, 1.7)]
    return [figure(i, x + dx, y, z + dz, facing) for i, (dx, dz) in enumerate(offs)]


def field(x, y, z):
    return {"target": [x, y, z], "yaw": 0, "pitch": 27, "distance": 13, "fov": 30, "far": 900}


def views(points):
    def pt(name):
        return points[name]["position"]

    heroes = [figure(i, *pt(n)) for i, n in enumerate(("cael", "rowan", "lyra", "darius"))]
    e1p = [figure(i, *pt(f"enc1_party{i + 1}"), facing=(-1, 0), clip="ready") for i in range(4)]
    e1e = [figure(1, *pt(f"enc1_enemy{i + 1}"), facing=(1, 0), clip="ready", sprite=enemy_sprite()) for i in range(2)]
    e2p = [figure(i, *pt(f"enc2_party{i + 1}"), facing=(1, 0), clip="ready") for i in range(4)]
    e2e = [figure(1, *pt(f"enc2_enemy{i + 1}"), facing=(-1, 0), clip="ready", sprite=enemy_sprite()) for i in range(3)]
    c1, c2 = pt("enc1_centre"), pt("enc2_centre")
    v = {
        "opening": ({"target": [16, 3, 52], "yaw": 0, "pitch": 9, "distance": 21, "fov": 52, "far": 900}, heroes),
        "opening30": ({"target": [16, 3, 52], "yaw": 0, "pitch": 11, "distance": 20, "fov": 30, "far": 900}, heroes),
        "opening_up": ({"target": [16, 12, 44], "yaw": 0, "pitch": -4, "distance": 30, "fov": 55, "far": 900}, heroes),
        "opening_wide": ({"target": [16, 7.6, 52], "yaw": 0, "pitch": 1, "distance": 24, "fov": 70, "far": 900}, heroes),
        "opening_b": ({"target": [16, 5, 52], "yaw": 0, "pitch": 8, "distance": 22, "fov": 60, "far": 900}, heroes),
        "opening_c": ({"target": [16, 3, 52], "yaw": 0, "pitch": 12, "distance": 18, "fov": 45, "far": 900}, heroes),
        "bridge": (field(16, 0, 60), party(16, 0, 60)),
        "bridge_n": (field(16, 0, 53), party(16, 0, 53)),
        "gate": (field(16, 0, 47.5), party(16, 0, 47.5)),
        "enc1": (field(11, 0, 42.5), e1p + e1e),
        "enc1_battle": ({"target": [c1[0], 0.8, c1[2]], "yaw": 0, "pitch": 17, "distance": 13, "fov": 30, "far": 900},
                        e1p + e1e),
        "enc1_battle_n": ({"target": [c1[0], 0.8, c1[2]], "yaw": 180, "pitch": 17, "distance": 13, "fov": 30,
                           "far": 900}, e1p + e1e),
        "west": (field(6.5, 0, 38), party(6.5, 0, 38)),
        "enc2": (field(17.5, 0, 35.5), e2p + e2e),
        "enc2_battle": ({"target": [c2[0], 0.8, c2[2]], "yaw": 0, "pitch": 17, "distance": 13, "fov": 30, "far": 900},
                        e2p + e2e),
        "enc2_battle_n": ({"target": [c2[0], 0.8, c2[2]], "yaw": 180, "pitch": 17, "distance": 13, "fov": 30,
                           "far": 900}, e2p + e2e),
        "east": (field(23.5, 0, 29), party(23.5, 0, 29)),
        "north": (field(17, 0, 24.5), party(17, 0, 24.5)),
        "stair": (field(16, 1.33, 18.5), party(16, 1.33, 18.5)),
        "landing": (field(16, 3, 10.5), party(16, 3, 10.5)),
        # edge cases of the walk: corners and the ends of each leg
        "x_sw": (field(5, 0, 44.6), party(5, 0, 44.6)),
        "x_wn": (field(5, 0, 34), party(5, 0, 34)),
        "x_es": (field(24.6, 0, 36.4), party(24.6, 0, 36.4)),
        "x_en": (field(24.6, 0, 24), party(24.6, 0, 24)),
        "x_nw": (field(12.8, 0, 24.4), party(12.8, 0, 24.4)),
        "x_landw": (field(11.8, 3, 12.0), party(11.8, 3, 12.0)),
        "x_lande": (field(20.2, 3, 8.8), party(20.2, 3, 8.8)),
        "x_stairtop": (field(16, 2.8, 14.6), party(16, 2.8, 14.6)),
        "x_bridges": (field(16, 0, 68.2), party(16, 0, 68.2)),
        "x_yarde": (field(17.3, 0, 41), party(17.3, 0, 41)),
        "x_corr": (field(11, 0, 35), party(11, 0, 35)),
        "x_fissure": (field(21.5, 0, 37.5), party(21.5, 0, 37.5)),
        "top": ({"target": [16, 0, 36], "yaw": 0, "pitch": 80, "distance": 95, "fov": 30, "far": 900}, heroes),
        "tower": ({"target": [16, 24, 6], "yaw": 0, "pitch": -8, "distance": 70, "fov": 45, "far": 900}, []),
        "side": ({"target": [16, 3, 40], "yaw": 60, "pitch": 18, "distance": 60, "fov": 40, "far": 900}, heroes),
    }
    return v


def main():
    out = sys.argv[1]
    args = sys.argv[2:]
    names = [a for i, a in enumerate(args) if not a.startswith("--") and (i == 0 or args[i - 1] not in ("--tick", "--env"))]
    tick = 150
    if "--tick" in sys.argv:
        tick = int(sys.argv[sys.argv.index("--tick") + 1])
    env = None
    if "--env" in sys.argv:
        env = json.loads(sys.argv[sys.argv.index("--env") + 1])
    points = json.load(open(os.path.join(GAME, "maps", "fortress", "map.json")))["points"]
    crystal = os.path.join(GAME, "models", "aether_crystal.glb")
    all_views = views(points)
    names = names or ["opening", "bridge", "gate", "enc1", "enc1_battle", "west", "enc2", "enc2_battle", "east",
                      "north", "stair", "landing"]
    os.makedirs(out, exist_ok=True)

    def run(name):
        cam, ents = all_views[name]
        ents = list(ents)
        if os.path.exists(crystal):
            ents.append({"position": points["crystal"]["position"], "play": "idle", "keep_height": True,
                         "spawn": {"mesh": {"model": "models/aether_crystal.glb"}}})
        shot = {"map": "fortress", "camera": cam, "entities": ents, "frames": [tick]}
        if env:
            shot["environment"] = env
        folder = os.path.join(out, "_" + name)
        os.makedirs(folder, exist_ok=True)
        path = os.path.join(folder, "shot.json")
        json.dump(shot, open(path, "w"))
        r = subprocess.run([sys.executable, os.path.join(ROOT, "tools", "aetherbound", "shot.py"), path, "--out",
                            folder], capture_output=True, text=True)
        src = os.path.join(folder, "shot_1.png")
        if os.path.exists(src):
            os.replace(src, os.path.join(out, name + ".png"))
        msg = "\n".join(line for line in r.stdout.splitlines() if not line.startswith("wrote"))
        return name, msg.strip()

    with ThreadPoolExecutor(max_workers=3) as pool:
        for name, msg in pool.map(run, names):
            print(name, "ok" if os.path.exists(os.path.join(out, name + ".png")) else "FAILED", msg[:600])


if __name__ == "__main__":
    main()
