#!/usr/bin/env python3
"""Is this sprite sheet ready for the game?

    python3 tools/spritekit/validate.py sprites/hero.anim.json --profile character
    python3 tools/spritekit/validate.py --manifest tools/aetherbound/sprites.json --engine build/fengine_headless

Runs the five measured checks (the sheet, pixel hygiene, motion, clips and
timing, game readiness) on an .anim.json and the images it names and, with a
manifest, enforces the sixth: that someone looked at the sprite's review
sheet and signed it off against the game's art direction.

Exit code 0 when every sprite passes: no errors, and no warnings unless a
manifest waives them with a reason. `--lenient` lets warnings through.
See tools/spritekit/README.md.
"""
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.dont_write_bytecode = True
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.dirname(HERE))

from spritekit.kit import SpriteKit  # noqa: E402

if __name__ == "__main__":
    sys.exit(SpriteKit().validate_main(description=__doc__))
