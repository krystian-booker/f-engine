#!/usr/bin/env python3
"""Are this model's animations ready for the game?

    python3 tools/animkit/validate.py models/golem.glb --profile character
    python3 tools/animkit/validate.py --manifest tools/aetherbound/animations.json [models/colossus.glb]

Runs the five automated checks (keyframe integrity, motion quality, loops and
hand-overs, deformation, game readiness) on the animations of a .glb/.gltf,
posed exactly as the engine poses them, and, with a manifest, enforces the
sixth: that someone looked at the review sheet and signed the motion off
against the game's art direction.

What each animation is meant to be (a loop or not, its length, what it hands
over to, whether it stays on the ground and over its origin) is stated once,
in the manifest entry's "overrides": {"animations": {...}, "required": [...]};
without it only what needs no intent is checked, and a warning says so.

Exit code 0 when every model passes: no errors, and no warnings unless the
manifest waives them with a reason. `--lenient` lets warnings through.
See tools/animkit/README.md.
"""
import os
import sys

sys.dont_write_bytecode = True
HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.dirname(HERE))

from animkit.kit import AnimKit  # noqa: E402

kit = AnimKit(HERE)

if __name__ == "__main__":
    sys.exit(kit.validate_main(description=__doc__))
