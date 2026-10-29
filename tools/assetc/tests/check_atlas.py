"""Checks fengine_assetc's atlas output: the folder exists, clips carry their loop flags."""
import json
import sys
from pathlib import Path

output = Path(sys.argv[1])
clips = json.loads((output / "atlas.anim.json").read_text())["clips"]
assert clips["idle"]["loop"] is True, clips["idle"]
assert clips["blink"]["loop"] is False, clips["blink"]
assert (output / "atlas.png").is_file()
