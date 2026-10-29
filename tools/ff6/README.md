# Local FF6 asset extraction

`extract.py` reads the supplied US Rev 1 ROM and writes native assets, JSON indices and PNG reference previews. It uses format definitions and codecs from the public [FF6 disassembly](https://github.com/everything8215/ff6/tree/813013276c952fdd27edcf7b8b86f17291542cbf) and its romtools submodule. It downloads no ROMs and requires no emulator.

## Reproduce

From the f-engine repository root, using Python 3:

```sh
git clone https://github.com/everything8215/ff6.git /tmp/ff6-reference
git -C /tmp/ff6-reference checkout 813013276c952fdd27edcf7b8b86f17291542cbf
git -C /tmp/ff6-reference submodule update --init tools/romtools
python3 tools/ff6/extract.py \
  'rom-dump/Final Fantasy III (USA) (Rev 1).sfc' \
  --reference /tmp/ff6-reference \
  --output rom-dump/ff6-narshe
```

Use a full clone: the pinned commit may no longer be the default branch tip. The script rejects a different ROM hash or reference revision. The required romtools commit is `f023d6b2ebdafd5a23a79264a9a3775ff5ea537e`. The reference checkout is read only during extraction, and its native extraction/build step is unnecessary. The external reference is a development dependency, never a shipped runtime dependency; retain its upstream license when redistributing that toolchain.

The current workstation's reference checkout is `/private/tmp/ff6-reference`, at the same pinned revisions. The export uses the Python standard library plus romtools and the reference LZ decoder; Pillow, NumPy, ca65 and a ROM rebuild are unnecessary for this script.

## Output

```text
manifest.json                   ROM identity and every native output hash/offset
definitions.json                source ranges, pointer definitions and asset names
native/gfx/                     planar tiles, palettes, tile layouts, fonts, portraits
native/data/                    field and battle arrays and decompressed map data
native/events/                  original scripts, triggers, NPCs and exit pointers
native/battle/                  stats, AI scripts, battle event animation region
native/sound/                   BRR samples and all 85 song sequences
text/*.json                     decoded text with control tokens retained
text/narshe_dialogue.json        opening story dialogue plus key optional messages
indices/maps.json               selected map metadata, NPCs, triggers and exits
indices/songs.json              song identifiers and native sequence paths
indices/monsters.json           all 384 monster records with selected decoded stats
indices/opening_encounters.json fixed event groups and formation members
indices/opening_random_encounters.json selected maps with random encounters enabled
indices/sprite_poses.json       source pose indices and atlas rectangles
maps/*.json                     map layers, cells, priority bits and metadata
previews/maps/*.png             static BG1 and BG2, separate transparent layers
previews/sprites/*.png          16 by 24 source poses in an atlas
previews/enemies/*.png          selected palette-correct enemy tile grids
previews/portraits/*.png        source portrait tile grids
previews/tiles/*.png            vehicle and window tile references
```

The native catalog covers the reference's English rip-list groups plus explicitly listed event, AI, stats, animation and song regions. It is not a disassembly of every byte in the ROM. Source offsets are offsets into the headerless file; a SNES address such as `CC985B` maps to file offset `0C985B` for these HiROM data ranges. Source spans are stored as `source_offset` and `source_bytes`; `bytes` is the decoded output size. Shared pointer targets can produce multiple logical exports.

Map cell coordinates use the source's x/y axes. JSON `layer` numbers 1–3 designate SNES backgrounds; they are unrelated to f-engine navigation layer numbers. Packed NPC flags and exit flags remain in `raw`; special NPCs need interpretation before becoming placements. Map names are descriptive labels, with generic location labels retained where a story-specific label is unnecessary.

An exit's `destination_map` value of 511 means return to the saved parent map. It must not be treated as map 511 or filtered out merely because it is not world map ID 0–2. The departure exit on map 20 is one such record: horizontal x 0–43 inclusive at y 62, file offset `0x2DF8AC`. For long entrances, `raw` byte 2 gives axis in bit 7 and inclusive displacement in bits 0–6.

## Verification and limits

The script verifies the input hash before writing and rereads every native output to verify its size and SHA256. LZ graphics are decompressed; monster stencils are expanded. Additional validation should compare exported native files with an independent reference extraction and inspect map/sprite previews, as recorded in the spec.

Running again deterministically replaces generated files with the same names. Use a fresh output directory after changing exporter naming rules; the script deliberately does not remove unrelated files from an existing destination. `manifest.json` and `indices/maps.json` define the current export set.

The output is reference material, not a loadable f-engine game. BG3, sprite vehicle composition, animation timing, map collision, battle stage assembly, title/credits layouts, and PCM audio rendering still require conversion. Every original text control token is retained; it is not automatically compatible with engine markup. No gameplay interpreter for original event bytecode is included.
