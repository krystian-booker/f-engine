#!/usr/bin/env python3
"""Is this texture ready for the game?

    python3 tools/texturekit/validate.py ui/panel.png --profile ui
    python3 tools/texturekit/validate.py models/crate.glb                 # every texture embedded in it
    python3 tools/texturekit/validate.py "models/crate.glb#crate_albedo" --profile atlas
    python3 tools/texturekit/validate.py --manifest tools/aetherbound/textures.json --engine build/fengine_headless

Runs the five measured checks (file and encoding, alpha and edges, colour and
pixel discipline, tiling and layout, game readiness) on an image file, a
.cube LUT or a texture embedded in a glTF model and, with a manifest,
enforces the sixth: that someone looked at the texture's review sheet and
signed it off against the game's art direction.

With a manifest, a name may be a group: a folder (`ui`), a model (every
texture in it) or a glob (`maps/fortress/sky_*`).

Exit code 0 when every texture passes: no errors, and no warnings unless a
manifest waives them with a reason. `--lenient` lets warnings through.
See tools/texturekit/README.md.
"""
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.dont_write_bytecode = True
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.dirname(HERE))

from texturekit.kit import TextureKit  # noqa: E402

if __name__ == "__main__":
    sys.exit(TextureKit(HERE).validate_main(description=__doc__))
