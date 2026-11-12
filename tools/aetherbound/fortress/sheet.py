#!/usr/bin/env python3
"""Contact sheets of the review shots: sheet.py <folder> [cols] [names...]"""
import os
import sys
from PIL import Image, ImageDraw

folder = sys.argv[1]
cols = int(sys.argv[2]) if len(sys.argv) > 2 else 2
names = sys.argv[3:] or ["opening", "bridge", "gate", "enc1", "enc1_battle", "west", "enc2", "enc2_battle", "east",
                         "north", "stair", "landing"]
names = [n for n in names if os.path.exists(os.path.join(folder, n + ".png"))]
per = cols * 3
for s in range(0, len(names), per):
    group = names[s:s + per]
    rows = (len(group) + cols - 1) // cols
    sheet = Image.new("RGB", (640 * cols, 360 * rows), (0, 0, 0))
    for i, n in enumerate(group):
        im = Image.open(os.path.join(folder, n + ".png")).convert("RGB").resize((640, 360), Image.LANCZOS)
        ImageDraw.Draw(im).text((6, 4), n, fill=(255, 255, 255))
        sheet.paste(im, ((i % cols) * 640, (i // cols) * 360))
    sheet.save(os.path.join(folder, f"sheet_{s // per + 1}.png"))
    print(os.path.join(folder, f"sheet_{s // per + 1}.png"))
