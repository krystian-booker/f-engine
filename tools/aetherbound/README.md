# Aetherbound: Prologue — how its assets are made

Everything in `samples/aetherbound` that is not Lua, JSON data or a font is
written by the scripts in this folder, so it can be regenerated and carries no
third-party licence. `SPEC.md` is the brief each generator was written to:
the look, the palette, the world's scale, and the names (clips, animations,
points, audio ids) the game code relies on.

The generators need Python 3.11+ with `numpy` and `pillow`; the maps and the
models also need Blender as a Python module (`pip install bpy`, 5.x) and the
audio needs `scipy` and `soundfile`. Blender runs headless (Cycles on the CPU
for the occlusion bakes).

| Folder | Writes | Run |
| --- | --- | --- |
| `sprites/` | `sprites/*`: six characters (nineteen clips, four directions, normal maps), Rowan's glowing variant, the broken sword. A small ray-caster on one posable rig, reduced to pixel art; numpy only, about 5 s | `python tools/aetherbound/sprites/generate.py [name ...] [--sheets DIR]` |
| `colossus/` | `models/colossus.glb` (rigged, eleven animations) with `colossus.json` (named points, facing), `models/aether_crystal.glb`, `models/aether_shard.glb` | `python tools/aetherbound/colossus/build_colossus.py`, `build_crystals.py`; `verify.py` checks the result against the brief |
| `fortress/` | `maps/fortress/*` and `fx/fortress/*`: the bridge, gate, courtyard, stair and tower, their lights, emitters and sky | see the folder's `build.py` |
| `chamber/` | `maps/chamber/*`, `maps/ruins/*` and `fx/chamber/*`: one generator, two maps on one footprint | `python tools/aetherbound/chamber/build.py [chamber] [ruins] [--fast]`; `shots.py --out DIR` renders review frames |
| `audio/` | `audio/*.ogg` (8 tracks, 3 ambiences, 43 effects) and `data/audio.json` | `python tools/aetherbound/audio/generate.py [-j 4] [id ... \| music \| ambience \| sfx]`; `analyze.py` measures them |
| `ui/` | `ui/*.png` (panels, cursor, button glyphs) and `fx/battle/*` (battle effects, the light column, the star, the silhouette) | `python tools/aetherbound/ui/generate.py` |

`shot.py` renders any of it in the real engine without the game's code: a
JSON file names a map, a camera and entities (sprites with a clip, models
with an animation) and the ticks to capture; see the docstring at its top.
`preview/` is the tiny game it runs.

After regenerating, `./build/fengine_headless --check samples/aetherbound` and
`--test samples/aetherbound` say whether the game still has everything it
names.
