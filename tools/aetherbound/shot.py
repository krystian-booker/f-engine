#!/usr/bin/env python3
"""Screenshot game assets in the engine, without the game's code.

    python3 tools/aetherbound/shot.py shot.json --out /some/folder

shot.json:
  {"map": "chamber",                       # a map of samples/aetherbound, or "flat"
   "camera": {"target": [16, 2, 16], "yaw": 0, "pitch": 22, "distance": 24, "fov": 30, "far": 600},
   "environment": {...},                   # optional world:set_environment overrides
   "entities": [                           # optional
     {"position": [8, 0, 8], "facing": [0, 1], "clip": "walk",
      "spawn": {"sprite": {"animation": "sprites/cael.anim.json", "mode": "standing"}}},
     {"position": [8, 0, 6], "play": "idle",
      "spawn": {"yaw": 90, "mesh": {"model": "models/colossus.glb"}}}],
   "frames": [30, 45]}                     # ticks (1/60 s) at which to capture; default [30]

Writes shot_1.png, shot_2.png ... into --out. The preview game sees the
folders of samples/aetherbound (maps, sprites, models, fx, ui, audio) plus a
built-in map "flat": a 16 x 16 dark floor with neutral light.
Runs the windowed engine under xvfb (software Vulkan), about 5-15 s a run.
"""
import argparse, json, os, shutil, subprocess, sys, tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
GAME = os.path.join(ROOT, "samples", "aetherbound")
PREVIEW = os.path.join(ROOT, "tools", "aetherbound", "preview")


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("shot")
    parser.add_argument("--out", required=True)
    parser.add_argument("--engine", default=os.path.join(ROOT, "build", "fengine"))
    parser.add_argument("--keep", action="store_true", help="keep the temporary game folder and print the log")
    args = parser.parse_args()
    shot = json.load(open(args.shot))
    work = tempfile.mkdtemp(prefix="aeshot-")
    game = os.path.join(work, "game")
    shutil.copytree(PREVIEW, game)
    for name in sorted(os.listdir(GAME)):
        source = os.path.join(GAME, name)
        if os.path.isdir(source) and name not in ("game", "story", "tests", "replays", "data"):
            target = os.path.join(game, name)
            if os.path.isdir(target):  # the preview's own maps live next to the game's
                for entry in os.listdir(source):
                    subprocess.check_call(["cp", "-al", os.path.join(source, entry), os.path.join(target, entry)])
            else:
                subprocess.check_call(["cp", "-al", source, target])
    json.dump(shot, open(os.path.join(game, "shot.json"), "w"))
    user = os.path.join(work, "user")
    command = ["xvfb-run", "-a", "-s", "-screen 0 1280x720x24", args.engine, game, "--mute", "--user-data", user]
    result = subprocess.run(command, capture_output=True, text=True, timeout=300)
    log = result.stdout + result.stderr
    problems = [line for line in log.splitlines() if " error " in line or " warning " in line]
    os.makedirs(args.out, exist_ok=True)
    shots = os.path.join(user, "screenshots")
    written = []
    if os.path.isdir(shots):
        for name in sorted(os.listdir(shots)):
            shutil.copy(os.path.join(shots, name), os.path.join(args.out, name))
            written.append(os.path.join(args.out, name))
    for line in problems[:40]:
        print(line)
    print("wrote", ", ".join(written) if written else "nothing")
    if args.keep:
        print(log[-4000:])
        print("kept", work)
    else:
        shutil.rmtree(work, ignore_errors=True)
    return 0 if written else 1


if __name__ == "__main__":
    sys.exit(main())
