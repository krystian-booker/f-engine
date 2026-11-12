#!/usr/bin/env python3
"""The sixth check: look at the motion.

    python3 tools/animkit/review.py models/golem.glb --profile character --out /tmp/golem.png
    python3 tools/animkit/review.py --manifest tools/aetherbound/animations.json models/colossus.glb --engine build/fengine
    python3 tools/animkit/review.py --manifest tools/aetherbound/animations.json models/colossus.glb \
        --sign pass --by "name" --notes "what was looked at and judged"

Without --sign it writes the review sheet: an overview (a strip of poses and
the speed over time for every animation, the findings of validate.py and the
questions to answer) and, beside it, one page per animation named
<sheet>_<animation>.png: filmstrips from the game's side and side on with the
ground line, the seam of a loop or the hand-overs of a one-shot drawn over
each other, the trails of the extremities, and the speed graph. With --engine
(the windowed build/fengine) each page also has shots of the real renderer at
moments of the filmstrip.

Open the overview and every page, judge them against the game's art
direction (the manifest names the document) and the questions in README.md.

With --sign it records the verdict (pass or fail) in the manifest, against
the file's hash: validate.py refuses a model whose review is missing, failed,
or older than the file.
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
    sys.exit(kit.review_main(description=__doc__))
