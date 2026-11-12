#!/usr/bin/env python3
"""Run the editor walkthrough scripts against samples/starfall and report."""
import json
import os
import pathlib
import shutil
import subprocess
import sys
import tempfile

here = pathlib.Path(__file__).resolve().parent
root = here.parent.parent
runtime = pathlib.Path(sys.argv[1] if len(sys.argv) > 1 else root / "build" / "fengine")
game = root / "samples" / "starfall"
town = game / "maps" / "town" / "map.json"
out = here / "out"
out.mkdir(exist_ok=True)
original = town.read_bytes()
failed = 0
try:
    for script in sorted(here.glob("[0-9]*.json")):
        # Captures are written relative to this folder.
        steps = json.loads(script.read_text(encoding="utf-8"))
        for step in steps:
            if "capture" in step:
                step["capture"] = str(here / step["capture"])
        resolved = out / ("resolved_" + script.name)
        resolved.write_text(json.dumps(steps), encoding="utf-8")
        with tempfile.TemporaryDirectory() as user:
            env = dict(os.environ, FENGINE_EDITOR_SCRIPT=str(resolved))
            result = subprocess.run(
                [str(runtime), str(game), "--tools", "--mute", "--replay", str(here / "start_town.json"),
                 "--user-data", user],
                env=env, capture_output=True, text=True, encoding="utf-8", errors="replace")
        lines = [line for line in (result.stdout + result.stderr).splitlines() if "editor script:" in line]
        print(f"== {script.name}")
        for line in lines:
            print("  " + line.split("editor script: ", 1)[1])
        bad = [line for line in lines if " FAIL " in line] or not any(" passed, " in line for line in lines)
        if bad or result.returncode not in (0, 1):
            failed += 1
            tail = (result.stdout + result.stderr).splitlines()[-5:]
            print("  (exit %d) %s" % (result.returncode, " | ".join(tail)))
        # Each script starts from the pristine sample.
        town.write_bytes(original)
        shutil.rmtree(game / "maps" / "walk_test", ignore_errors=True)
        for leftover in (game / "sprites").glob("walkthrough_sheet*"):
            leftover.unlink()
finally:
    town.write_bytes(original)
    shutil.rmtree(game / "maps" / "walk_test", ignore_errors=True)
    for leftover in (game / "sprites").glob("walkthrough_sheet*"):
        leftover.unlink()
print("walkthrough: %s" % ("all scripts passed" if failed == 0 else f"{failed} script(s) failed"))
sys.exit(1 if failed else 0)
