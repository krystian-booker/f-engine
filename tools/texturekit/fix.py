#!/usr/bin/env python3
"""Apply the fixes that change no visible pixel to image files.

    python3 tools/texturekit/fix.py ui/panel.png maps/keep/textures/*.png            # says what it would do
    python3 tools/texturekit/fix.py ui/panel.png --write                            # does it
    python3 tools/texturekit/fix.py sky.png --bleed --write                         # also fill colour under alpha 0

What it does to a PNG (or to any image the engine decodes, which it rewrites
as PNG next to the original only when the name already ends in .png):

- strips every ancillary chunk: gamma, ICC profile, text, time stamps, EXIF
  (the engine reads none of them; note that stripping a profile does not
  convert the colours, it only makes every tool show what the engine shows);
- writes 8 bits per channel, not interlaced, at full compression, as a
  palette PNG when 256 colours are enough;
- drops an alpha channel that is 255 everywhere;
- with `--bleed`: carries the colour of visible texels into the fully
  transparent ones (tools.bleed_edges), which removes the halo that linear
  filtering makes at the edges. Alpha and every visible texel stay as they
  were. A 16-bit file loses its low bytes, which the engine never read.

Nothing is written without `--write`. Textures embedded in a .glb are not
touched: fix those in the generator that writes the model (save the image
with texturekit's tools.save_png before the exporter picks it up).

It does not fix anything that needs a decision: seams, palettes, sizes,
normal-map conventions. Those are the generator's.
"""
import argparse
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.dont_write_bytecode = True
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.dirname(HERE))

import numpy as np  # noqa: E402

from texturekit import image as ti  # noqa: E402
from texturekit import tools  # noqa: E402


def plan(path, bleed=False):
    """(what would change, the new file's bytes or None when nothing would)."""
    texture = ti.load(path, {"role": "colour"})
    if texture.is_lut or texture.rgba is None:
        return ["not an image"], None
    if not path.lower().endswith(".png"):
        return [f"a {texture.format} file: save it as PNG in the generator (this tool only rewrites .png files)"], None
    pixels = texture.rgba
    steps = []
    extra = sorted(kind for kind in texture.chunks if kind not in ("IHDR", "PLTE", "IDAT", "IEND", "tRNS"))
    if extra:
        steps.append("strip " + ", ".join(extra))
    if texture.bit_depth > 8:
        steps.append(f"{texture.bit_depth} bits to 8")
    if texture.interlaced:
        steps.append("remove interlacing")
    if texture.stores_alpha and texture.alpha.min() == 255:
        steps.append("drop the unused alpha channel")
    if bleed and texture.alpha.min() == 0 and texture.alpha.max() > 0:
        bled = tools.bleed_edges(pixels)
        changed = int((bled != pixels).any(axis=2).sum())
        if changed:
            steps.append(f"bleed edge colour into {changed:,} transparent texels")
            pixels = bled
    data = tools.encode_png(pixels)
    check = ti.decode(data)
    visible = texture.alpha > 0
    if not (np.array_equal(check.alpha, texture.alpha) and np.array_equal(check.rgba[visible], texture.rgba[visible])):
        raise SystemExit(f"{path}: the rewritten file would not decode to the same visible pixels; nothing written")
    if not steps and len(data) < len(texture.data):
        steps.append("recompress")
    if not steps or (len(data) >= len(texture.data) and steps == ["recompress"]):
        return [], None
    steps.append(f"{len(texture.data):,} -> {len(data):,} bytes")
    return steps, data


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("files", nargs="+", help="image files (not textures inside a .glb)")
    parser.add_argument("--bleed", action="store_true", help="also carry edge colour into fully transparent texels")
    parser.add_argument("--write", action="store_true", help="write the files (without it: only say what would change)")
    args = parser.parse_args(argv)
    changed = 0
    for path in args.files:
        try:
            steps, data = plan(path, args.bleed)
        except (OSError, ValueError) as error:
            print(f"{path}: cannot be read: {error}")
            continue
        if data is None:
            print(f"{path}: {'; '.join(steps) if steps else 'nothing to fix'}")
            continue
        changed += 1
        if args.write:
            with open(path, "wb") as out:
                out.write(data)
        print(f"{path}: {'fixed' if args.write else 'would fix'}: {'; '.join(steps)}")
    if changed and not args.write:
        print(f"{changed} file(s) would change; run again with --write. A changed file needs its visual review signed again.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
