#!/usr/bin/env python3
"""Test renders of the chamber and the ruins in the real engine.

    python3 tools/aetherbound/chamber/shots.py --out <folder> [name ...]

Each named shot (see SHOTS) runs tools/aetherbound/shot.py and is saved as
<folder>/<name>.png; --frame is the tick captured (the haze veils of the
chamber take some seconds to gather, so 300 or more shows the map as played).
Heroes use the game's sprite sheets (Cael's for any that is missing); the
Colossus is its model when models/colossus.glb exists.
"""
import argparse
import json
import os
import shutil
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(os.path.dirname(HERE)))
GAME = os.path.join(ROOT, "samples", "aetherbound")
SCRATCH = "/tmp/claude-0/-home-user-f-engine/c427e0fc-76b4-5426-b107-9b52cd2adb5c/scratchpad/chamber"
sys.path.insert(0, HERE)
from layout import POINTS, RUINS_POINTS, DAIS_TOP, crater_depth, DX, DZ  # noqa: E402
import math  # noqa: E402

HEROES = ["cael", "rowan", "lyra", "darius"]
FACE = {"w": [-1, 0], "e": [1, 0], "n": [0, -1], "s": [0, 1], "sw": [-0.7, 0.7]}


def sprite(name):
    for candidate in (name, "cael"):
        if os.path.exists(os.path.join(GAME, "sprites", f"{candidate}.anim.json")):
            return f"sprites/{candidate}.anim.json"
    return "sprites/cael.anim.json"


def hero(name, point, clip="idle", facing=None, ruined=False, y=None):
    table = dict(POINTS, **RUINS_POINTS)
    x, z, f = table[point] if isinstance(point, str) else point
    if y is None:
        y = -crater_depth(math.hypot(x - DX, z - DZ)) if ruined else 0.0
    return {"position": [x, y, z], "facing": FACE[facing or f], "clip": clip,
            "spawn": {"sprite": {"animation": sprite(name), "mode": "standing", "cast_shadow": True}}}


def colossus(play=None):
    x, z, _ = POINTS["boss"]
    if os.path.exists(os.path.join(GAME, "models", "colossus.glb")):
        entity = {"position": [x, DAIS_TOP, z], "keep_height": True, "spawn": {"yaw": 90, "mesh": {"model": "models/colossus.glb"}}}
        if play:
            entity["play"] = play
        return entity
    return None


def party(points, clip="ready", ruined=False):
    return [hero(name, f"{points}{i + 1}", clip, ruined=ruined) for i, name in enumerate(HEROES)]


def camera(target, pitch, distance, yaw=0, fov=30):
    return {"target": target, "yaw": yaw, "pitch": pitch, "distance": distance, "fov": fov, "far": 600}


SHOTS = {
    # chamber
    "battle": ("chamber", camera([16, 2.5, 16], 22, 24), lambda: party("party") + [colossus("idle")]),
    "battle_low": ("chamber", camera([16, 2.5, 16], 18, 26), lambda: party("party") + [colossus("idle")]),
    "battle_close": ("chamber", camera([16, 2.5, 16], 25, 20), lambda: party("party") + [colossus("idle")]),
    "entrance": ("chamber", camera([28.0, 1.0, 16], 22, 22), lambda: [
        hero("cael", (28.5, 16.0, "w"), "walk"), hero("rowan", (29.3, 15.3, "w"), "walk"),
        hero("lyra", (29.6, 16.7, "w"), "walk"), hero("darius", (30.3, 16.1, "w"), "walk"), colossus("dormant")]),
    "intro": ("chamber", camera([23.6, 1.0, 16], 22, 22), lambda: party("intro", "idle") + [colossus("dormant")]),
    "dialogue_party": ("chamber", camera([21.0, 1.1, 16.2], 16, 9), lambda: party("party", "idle") + [colossus("idle")]),
    "dialogue_dais": ("chamber", camera([12.5, 3.0, 16], 12, 16), lambda: [
        hero("rowan", "rowan_walk", "idle"), hero("cael", "cael_stop", "reach"), colossus("idle")]),
    "dialogue_rowan": ("chamber", camera([17.0, 1.1, 16], 14, 8.5), lambda: [
        hero("rowan", "rowan_walk", "idle"), hero("cael", "cael_stop", "reach"), colossus("idle")]),
    "top": ("chamber", camera([16, 0, 16], 89, 62), lambda: party("party") + [colossus("idle")]),
    "tilt_chamber": ("chamber", camera([16, 6, 12], -8, 20), lambda: [colossus("idle")]),
    # ruins
    "ruins_wide": ("ruins", camera([16, 1.5, 15], 24, 20), lambda: [
        hero("cael", "cael_wake", "ko", ruined=True), hero("darius", "darius_wake", "ko", ruined=True),
        hero("lyra", "lyra_kneel", "kneel", ruined=True)]),
    "ruins_wide2": ("ruins", camera([16, 2.5, 16], 22, 24), lambda: [
        hero("cael", "cael_run_end", "idle", ruined=True), hero("lyra", "lyra_kneel", "kneel", ruined=True)]),
    "ruins_crater": ("ruins", camera([13.2, 0.7, 16], 14, 8.5), lambda: [
        hero("lyra", "lyra_kneel", "kneel", ruined=True), hero("cael", "cael_run_end", "idle", ruined=True)]),
    "ruins_tilt0": ("ruins", camera([12, 1, 16], 2, 11), lambda: [
        hero("lyra", "lyra_kneel", "kneel", ruined=True), hero("cael", "cael_run_end", "hold", ruined=True)]),
    "ruins_tilt1": ("ruins", camera([13, 7.5, 12], -20, 11), lambda: []),
    "ruins_tilt2": ("ruins", camera([14, 14, 8], -40, 11), lambda: []),
    "ruins_top": ("ruins", camera([16, 0, 16], 89, 62), lambda: []),
    "ruins_entrance": ("ruins", camera([24.0, 1.0, 16], 24, 20), lambda: [
        hero("cael", "cael_wake", "rise", ruined=True), hero("darius", "darius_wake", "ko", ruined=True)]),
}


def run(name, out, frame=150, environment=None):
    map_id, cam, entities = SHOTS[name]
    shot = {"map": map_id, "camera": cam, "entities": [e for e in entities() if e], "frames": [frame]}
    if environment:
        shot["environment"] = environment
    work = tempfile.mkdtemp(prefix="chamber-shot-", dir=SCRATCH if os.path.isdir(SCRATCH) else None)
    path = os.path.join(work, "shot.json")
    json.dump(shot, open(path, "w"))
    result = subprocess.run([sys.executable, os.path.join(ROOT, "tools", "aetherbound", "shot.py"), path, "--out", work],
                            capture_output=True, text=True)
    lines = [l for l in result.stdout.splitlines() if not l.startswith("wrote")]
    if lines:
        print(name, "\n".join(lines[:12]))
    source = os.path.join(work, "shot_1.png")
    if os.path.exists(source):
        os.makedirs(out, exist_ok=True)
        shutil.copy(source, os.path.join(out, f"{name}.png"))
        print("wrote", os.path.join(out, f"{name}.png"))
    else:
        print(name, "FAILED", result.stdout[-2000:], result.stderr[-2000:])
    shutil.rmtree(work, ignore_errors=True)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("names", nargs="*")
    parser.add_argument("--out", required=True)
    parser.add_argument("--frame", type=int, default=150)
    args = parser.parse_args()
    names = args.names or list(SHOTS)
    from concurrent.futures import ThreadPoolExecutor
    with ThreadPoolExecutor(3) as pool:
        list(pool.map(lambda n: run(n, args.out, args.frame), names))


if __name__ == "__main__":
    main()
