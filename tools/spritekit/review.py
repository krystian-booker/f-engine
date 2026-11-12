#!/usr/bin/env python3
"""The sixth check: look at the sprite.

    python3 tools/spritekit/review.py sprites/hero.anim.json --profile character --out /tmp/hero.png
    python3 tools/spritekit/review.py --manifest tools/aetherbound/sprites.json sprites/cael.anim.json --engine build/fengine
    python3 tools/spritekit/review.py --manifest tools/aetherbound/sprites.json sprites/cael.anim.json \
        --sign pass --by "name" --notes "what was looked at and judged"

Without --sign it writes the sprite's review sheet: page 1 is the overview
(the line-up with the other sprites of its profile, silhouettes, its size
next to a hero-high bar, the palette, the normal map lit from three sides,
the emissive image, every finding of validate.py and the questions to
answer), and the pages after it (<name>_2.png, _3 ...) are every clip as a
filmstrip per direction with the frame, the pivot and the ground row drawn
in, an onion skin and the loop's seam. With --engine it adds shots from the
real renderer. Read every page against the game's art direction.

With --sign it records the verdict (pass or fail) in the manifest, against
the hash of the .anim.json and its images: validate.py refuses a sprite
whose review is missing, failed, or older than its files.
"""
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.dont_write_bytecode = True
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.dirname(HERE))

from spritekit.kit import SpriteKit  # noqa: E402

if __name__ == "__main__":
    sys.exit(SpriteKit().review_main(description=__doc__))
