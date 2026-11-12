#!/usr/bin/env python3
"""Make a texture's review sheet, and record the verdict of the review.

    python3 tools/texturekit/review.py --manifest tools/aetherbound/textures.json maps/fortress/geometry.glb
    python3 tools/texturekit/review.py --manifest tools/aetherbound/textures.json maps/fortress/geometry.glb \\
        --sign pass --by "who" --notes "what was judged for the group, and any flaw accepted"

The sheet is one image per texture: the whole texture, a detail enlarged with
nearest filtering, the texture on black and on white as the game samples it
(or tiled 3x3, or lit as a normal map), its channels, its mips, its values
and its palette beside the art direction's colours, with every finding and
the questions to answer. Naming several textures (a folder, a model, a glob)
also writes a group sheet that shows them side by side at one scale.

`--sign` writes the verdict into the manifest with the hash of the file (for
an embedded texture: of the model), so a later change voids it. See
tools/texturekit/README.md for the questions and the reviewer's rules.
"""
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.dont_write_bytecode = True
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.dirname(HERE))

from texturekit.kit import TextureKit  # noqa: E402

if __name__ == "__main__":
    sys.exit(TextureKit(HERE).review_main(description=__doc__))
