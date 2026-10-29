# Narshe opening asset catalog

## Export identity and organization

The local archive is [rom-dump/ff6-narshe](../../rom-dump/ff6-narshe). The [manifest](../../rom-dump/ff6-narshe/manifest.json) is authoritative for filenames, sizes, SHA256 hashes, source offsets, source lengths and raw/compressed encodings. It contains 2,338 native exports. An export is a logical file, not a count of unique source spans; pointer aliases can share data. Derived JSON and preview PNG files are additional outputs.

The archive intentionally extracts broad shared resource groups. Final game packaging should contain only assets used in the opening. Asset ID, palette ID, pose ID and map ID are different namespaces. Source SNES background numbers are also different from engine navigation layers.

| Category | Local native location | Available now | Runtime work |
| --- | --- | --- | --- |
| Character sprites | `native/gfx/map_sprite_gfx/` | 165 named graphic entries, source poses retained | Assemble metasprites, clip timing, direction mapping, atlas padding. |
| Character palettes | `native/gfx/map_sprite_pal/` | 32 palette entries | Assign per actor/scene; bake shared-art variants or configure engine swaps. |
| Vehicle graphics | `native/gfx/vehicle.4bpp` | Raw common vehicle tiles and tile preview | Assemble field armor/riders and battle armor independently. |
| Town/cave/map graphics | `native/gfx/map_gfx/`, `map_gfx_bg3/`, `map_anim.4bpp`, `map_anim_gfx_bg3/` | Static and animated tile sources | Tile animation sequences, BG3 layouts, effects and geometry texturing. |
| Map layouts | `native/data/field/sub_tilemap/`, `map_tileset/`, `map_tile_prop/` | Decompressed cell layouts, metatile definitions and tile properties | Compile engine maps, route collision, layers, stairs, masks and triggers. |
| Map palettes | `native/gfx/map_pal/`, `map_pal_anim/` | 48 base map palettes and animation palette data | Reproduce cycles, scripted modifications and correct color math. |
| NPCs and story triggers | `native/events/`, `maps/*.json` | Original bytes; selected map NPCs, event links, trigger cells and exits decoded | Evaluate visibility/event conditions; special NPC flags; convert to game placements and scripts. |
| Enemy art | `native/gfx/monster_gfx/` | Expanded tile stencils and palette variants | Correct sprite/palette pairing, anchors, multipart alignment and effects. |
| Battle backgrounds | `native/gfx/battle_bg_gfx/`, `battle_bg_tiles/`, `battle_bg_pal/` | Decompressed layers/tiles and palettes | Assemble source stages or author diorama equivalents and camera slots. |
| Battle data | `native/data/battle/`, `native/battle/`, `indices/monsters.json` | Formation arrays, selected decoded stats, original AI and event animation region | Required skills/items/actor setup and source formulas still need a scoped gameplay-data import. |
| Attack/effect art | `native/gfx/attack*`, `native/data/btlgfx/` | Planar art, palettes, tile layouts, animation-frame array | Assemble Magitek beams, strikes, healing, electrical effects and Esper sequence timelines. |
| Dialogue | `text/*.json`, `native/text/` | All English text groups decoded with tokens; original bytes retained | Convert controls, paginate, map names and localize input prompts. |
| UI/font art | `native/gfx/window/`, `menu*`, `small_font.en.2bpp`, `large_font.en.1bpp`, `status.en.4bpp` | Source tiles/palettes/fonts | Glyph advances, baseline, digit/cursor/icon mapping and engine UI theme. |
| Portraits | `native/gfx/portrait/` | 19 source entries with PNG tile-grid previews | Map glyph/tile ordering, confirm palette and use only where reference UI calls for it. |
| Title/opening/credits | `native/gfx/title_opening.en.4bpp`, `credits.4bpp`, `native/bin/cutscene_code.bin.lz` | Original graphics and compressed cutscene-program region | Assemble logo/credit layouts, extract timings and use a Lua timeline. |
| Music | `native/sound/sequences/`, `indices/songs.json` | All 85 native SPC song sequences | Render through a compatible SPC pipeline, export WAV/OGG and validate loops. |
| Instrument and sound samples | `native/sound/sample_brr/`, `sfx_brr/` | 63 instrument entries and SFX sample blocks | Driver/SFX-sequence interpretation, mix/envelopes, pitch, reverb and PCM renders. |
| 3D environments | No source equivalent | Production requirements specified | Author geometry.glb, materials, lights, colliders, effects and camera zones. |

The extract is a resource archive, not every original gameplay table or a complete emulation state. In particular, native skill/item/actor initialization tables and SFX driver/sequence conversion need an additional scoped data pass before gameplay/audio fidelity can be signed off. Their implementation tasks are explicit in the backlog.

## Required actor and enemy sets

| Actor or set | Requirements |
| --- | --- |
| Terra | Field walk/idle four directions, mounted rider, battle poses, controlled/headache/weak/fallen/wake poses, naming and unknown identity, flashback actions. |
| Vicks and Wedge | Shared soldier art with correct palette, separate positions, mounts, turn/idle/walk poses, battle Magitek and independent disappearance timing. |
| Locke | Field/battle poses, gestures, introduction/naming, rescue/carry/set-down staging, post-rescue lineup. |
| Arvis | Correct sprite/palette from scene NPC record, talk/turn/walk/open-route poses. The US script's speaker can be generic even when the game ID is Arvis. |
| Mog and ten temporary moogles | Shared art, independent names/stats/equipment/resources, field leaders and side-view combat; Mog's distinct available command set. |
| Narshe guards and dogs | Field walk/turn/jump, chase/approach motions, per-phase NPC visibility, collision/contact triggers. |
| Kefka, Gestahl and soldiers | Flashback staging, Terra control scene, mass salute and cinematic battle participants. |
| Local NPCs | Only the residents/tutorial actors valid before departure; palettes and event pages from the relevant phase. |
| Battle enemies | Guard, Lobo, Vomammoth, Were-Rat, Repo Man, Vaporite, Marshal, Whelk shell, opening Head, Tritoch and flashback Soldier. |

Selected sprite previews are [Terra](../../rom-dump/ff6-narshe/previews/sprites/terra.png), [Locke](../../rom-dump/ff6-narshe/previews/sprites/locke.png), [Mog](../../rom-dump/ff6-narshe/previews/sprites/mog.png) and [soldier](../../rom-dump/ff6-narshe/previews/sprites/soldier.png). Pose rectangles are in [sprite_poses.json](../../rom-dump/ff6-narshe/indices/sprite_poses.json). These sheets contain source poses, not final animation order. NPC preview palettes other than the explicitly verified main actors are inspection defaults and must be replaced using the scene's NPC palette.

Enemy preview art includes [Whelk shell](../../rom-dump/ff6-narshe/previews/enemies/whelk.png), [opening head art](../../rom-dump/ff6-narshe/previews/enemies/whelk_head_1.png), [Marshal](../../rom-dump/ff6-narshe/previews/enemies/marshal.png), [Vomammoth](../../rom-dump/ff6-narshe/previews/enemies/vomammoth.png), [Were-Rat](../../rom-dump/ff6-narshe/previews/enemies/were_rat.png) and [Vaporite](../../rom-dump/ff6-narshe/previews/enemies/vaporite.png). Marshal shares Guard graphics with its own palette. Repo Man similarly needs its source graphics/palette mapping; a display-name match is not a reliable graphics lookup.

## Scene environments and source maps

| Environment | Source map IDs | Required authored pieces |
| --- | --- | --- |
| Narshe overlook | 18 | Snowy ridge, vista, distant town, cliff edge, actor marks, cold wind/snow. |
| Prologue cliff view | 23 | Separate BG1/BG2/BG3 composition, text-safe framing and camera pan. |
| South Narshe | 19 assault; 20 normal | Streets, buildings, windows, steam, mine entrances, secret entrance, south departure boundary and scripted encounter zones. |
| North approach | 21 and 39 variants | Mine approach, guard placements and correct stage-dependent access. |
| North mines and secret passage | 40/41 variants | Packed cave layout, rails, save point, breakable barrier, Whelk route, Esper chamber and later escape-switch route. |
| Arvis and interiors | 30; other selected IDs as needed | House, bed, door, rear escape and eligible town interiors; split packed rooms during conversion. |
| South mines | 50/51 variants | Pursuit paths, collapse, unconscious Terra mark, moogle arrival marks, three leader starts, six pursuers and commander. |
| Tutorial rooms | 104–107 | Optional instruction interactions, valid loot and exits before departure. |
| Imperial flashback | 250, 244, cinematic stage 5 | Control scene, staged soldier destruction, Imperial address, crowd animations and transitions. |
| Snow march | Custom opening cutscene, not an ordinary field map | Ground/horizon/sky compositing, armor/riders, snowfall, credits and music timeline. |
| Battle stages | Source Narshe exterior/cave and cinematic backgrounds | Party/enemy slots, camera, floor/backdrop, shadows, effects and UI-safe area. |

The 41 exported maps include IDs 18–51, tutorial maps 104–107 and the three extra cinematic maps. Some variants and adjacent rooms are reference coverage, not additional mandatory locations. In particular, maps 32–35 are later Narshe variants, not the beginner school; do not activate them in the opening. The beginner school is reached from map 20 at (33,54) into map 104, then branches to 105–107. Their exact layouts, cells and exits are in `maps/<id>_<label>.json`. Unknown labels stay generic; verify accessibility against event flags before making them playable.

Map preview entry points: [overlook BG1](../../rom-dump/ff6-narshe/previews/maps/018_overlook_bg1.png), [town BG1](../../rom-dump/ff6-narshe/previews/maps/019_narshe_assault_bg1.png), [town BG2](../../rom-dump/ff6-narshe/previews/maps/019_narshe_assault_bg2.png), [Arvis/interiors BG1](../../rom-dump/ff6-narshe/previews/maps/030_narshe_interiors_arvis_bg1.png), [caves BG1](../../rom-dump/ff6-narshe/previews/maps/041_north_mines_and_secret_passage_bg1.png), and [defense BG1](../../rom-dump/ff6-narshe/previews/maps/051_south_mines_defense_bg1.png).

## Audio cue requirements

The [song index](../../rom-dump/ff6-narshe/indices/songs.json) preserves exact IDs and native paths. The scene director must distinguish a source instruction to start a song from a map's default music property. A map reload must not restart a deliberately continuous cue.

| Cue family | Required use and validation |
| --- | --- |
| Prelude and Opening Theme sections | Title/prologue/march, including source section transitions; verify actual start points and credit alignment. |
| Wind | Overlook/arrival atmosphere and source fades; it is represented as a song cue in this ROM. |
| Battle Theme and victory cues | Ordinary encounters, results and return to field, with correct continuation behavior. |
| Decisive Battle and boss context | Follow actual Whelk event settings; validate the selected cue rather than assigning by genre convention. |
| Awakening | Terra at Arvis's house and related beats. |
| Gestahl and flashback cues | Imperial memory sequence and transitions around cinematic combat. |
| Locke | Introduction, rescue/defense and aftermath cues as directed by the source. |
| Narshe and cave ambience | Local post-rescue exploration and appropriate default-map cues. |
| Event SFX | Snow/armor steps, bark, door/knocks, window/UI input, save glow, barrier impact, collapse, switch/secret entrance, beams/healing, Whelk electricity and Esper pulses/disappearances. |

Author a cue ledger with source sound ID, output file, gain, pitch, bus, loop start/end, fade and scene event. Native sequence export does not imply a verified audio render. Render one march cue and one battle cue early to validate the conversion pipeline before capturing the full intro.

## Required conversion metadata

Each game asset record should contain a stable ID, native input path/hash, source indices, palette assignment, output paths, pivot/scale, animation clips, transparent-color treatment, conversion version and any authored override. Map records additionally need original coordinates, engine points/zones, navigation layer mapping, background priority handling and runtime mutation definitions. Audio records additionally need sequence ID, render settings and loop sample positions.

Classify each output as native extracted, decoded reference, engine converted, authored, or validated in runtime. Do not promote a preview PNG to validated runtime art merely because the PNG opens. The asset compiler must fail on missing required assets instead of quietly using the sample game's placeholders.
