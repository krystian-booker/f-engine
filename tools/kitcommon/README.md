# kitcommon

What the asset kits share: `tools/modelkit` (3D models), `tools/animkit`
(skeletal and node animations), `tools/texturekit` (textures) and
`tools/spritekit` (sprite sheets). `kit.py` holds findings, profiles, the
manifest rules (waivers with a reason and a cap, a visual review bound to the
files' hash, unlisted assets fail), the report, the review-sheet layout, a
helper that loads something with the engine, and the `validate.py` and
`review.py` command lines. A kit adds only what knows its kind of asset: its
checks, its review sheet, its profiles and its guide.

So every kit is used the same way:

```sh
python3 tools/<kit>/validate.py <file> --profile <name> [--engine build/fengine_headless]
python3 tools/<kit>/validate.py --manifest tools/<game>/<section>.json [--engine build/fengine_headless]
python3 tools/<kit>/review.py --manifest tools/<game>/<section>.json <asset ...> [--engine build/fengine]
python3 tools/<kit>/review.py --manifest tools/<game>/<section>.json <asset ...> --sign pass --by "..." --notes "..."
python3 tools/<kit>/selftest.py
```
