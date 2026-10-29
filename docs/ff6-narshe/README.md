# Final Fantasy VI Narshe opening implementation package

Recreate the US SNES Final Fantasy III opening in f-engine, through Terra and Locke reaching the Narshe departure boundary. The complete scope includes the prologue, snowy march, assault, mines, Whelk, Tritoch, Arvis, flashbacks, Locke, three-party moogle defense, and secret passage escape. Stop before loading the world map or beginning the journey to Figaro.

The engine has the main services and Lua RPG kits needed. The largest new game systems are the three-party defense, the two-part Whelk encounter, Magitek presentation, and the opening's cinematic choreography. Extraction is complete for the supported native data groups; runtime conversion and the playable recreation are future implementation work.

## Playable slice

A first playable slice now exists in [samples/ff6_narshe](../../samples/ff6_narshe/README.md):
prologue, overlook, a shortened musical march, Narshe exploration and the
straight-north route's two-Guard battle. It includes an offline original-ROM
music/SFX renderer, source asset compiler, four prototype 3D scenes, tests and
windowed replays. The complete opening described below remains future work;
the sample README records its current fidelity and art-production limits.

| Deliverable | Location |
| --- | --- |
| Implementation specification and acceptance criteria | [SPEC.md](SPEC.md) |
| Scene order, dependencies, dialogue references and checkpoints | [scenes.json](scenes.json) |
| Asset catalog, export status and conversion requirements | [ASSETS.md](ASSETS.md) |
| Ordered implementation work and effort ranges | [BACKLOG.md](BACKLOG.md) |
| Reproducible extractor and provenance | [tools/ff6/README.md](../../tools/ff6/README.md) |
| Local extraction manifest | [manifest.json](../../rom-dump/ff6-narshe/manifest.json) |
| Extraction validation results | [verification.json](../../rom-dump/ff6-narshe/verification.json) |
| Decoded opening dialogue | [narshe_dialogue.json](../../rom-dump/ff6-narshe/text/narshe_dialogue.json) |
| Map catalog with NPCs, triggers and exits | [maps.json](../../rom-dump/ff6-narshe/indices/maps.json) |
| Fixed and random encounter catalogs | [fixed](../../rom-dump/ff6-narshe/indices/opening_encounters.json), [random](../../rom-dump/ff6-narshe/indices/opening_random_encounters.json) |

The ROM and extracted assets are local under the repository's existing ignored `rom-dump/` directory. The extraction tool and implementation documentation can be versioned independently. No game engine source or existing sample was changed for this package.

## Inspection results

The supplied file is an unheadered 3,145,728-byte US Rev 1 ROM. CRC32 is `C0FA0464`; SHA256 is `10eccc5d2fab81346dd759f6be478dcb682eef981e8d3d662da176e1f9a996bc`.

The exporter produces **2,338 native files**, **85 song sequences**, and **41 selected map records** covering IDs 18–51 plus 5, 104–107, 244 and 250. Selected maps include variants and nearby interiors; they are not 41 distinct required scenes. Shared source graphics and tilemaps remain shared in the catalog. PNG previews include static map layers, character poses, portraits, selected enemies, window tiles and vehicle tiles.

Native files carry per-file hashes and ROM offsets. Static previews preserve source pixels, but do not reproduce running palette animation, animated tiles, background color math, camera parallax, or event-driven tile edits. Music sequences and BRR instrument samples require offline rendering before the engine can play them. Animation atlases preserve poses; their final clips and durations still need authoring.

Validation checked all 2,338 native hashes and source bounds, all 119 PNG files, and the 26-scene/16-checkpoint specification. An independent upstream extraction matched 2,209 corresponding native files with zero differences. The other exports are supplemental regions, song sequences and preserved text binaries. This validates the resource export, not a running recreation.

## First implementation milestone

Build the asset importer and one lit Narshe street with three Magitek riders, snow, dialogue and a fixed Guard encounter. Separately prove party switching on the defense map. Resolve those two risks before producing every environment and cinematic. The complete planned deliverable remains the entire opening through departure.
