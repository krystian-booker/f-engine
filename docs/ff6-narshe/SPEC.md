# Final Fantasy VI Narshe opening specification

## 1 Objective and completion boundary

Produce a playable f-engine demonstration of the entire US SNES opening, starting with the title and prologue and ending when Terra and Locke are ready to leave Narshe. It must demonstrate exploration, event choreography, animation, dialogue, weather, transitions, ATB combat, scripted battle events, persistent state and three simultaneous field parties. The intended presentation uses original pixel sprites in authored HD-2D environments, consistent with this engine's design.

The final checkpoint places Terra and Locke at the south departure boundary after the secret passage escape. Both belong to the active party, Magitek status is gone, moogle parties are disbanded, the secret entrance is restored, and control has returned. Reaching the world-map exit displays the end card and offers restart/checkpoint selection. It must stop before `fe.worldmap` or a world-map scene loads. Figaro and travel outside Narshe are outside this build.

The concrete source exit is map **20**, horizontal long entrance at **y 62, x 0 through 43 inclusive**. Its raw destination map field is **511**, the engine script's return-to-parent sentinel, rather than an ordinary map-zero exit. Intercept this entrance when `departure_ready` is true and before resolving the parent destination. The associated ROM record starts at file offset `0x2DF8AC` with bytes `00 3E 2B FF 21 53 24`. The optional north rejection is triggered at x 37–39, y 49 on map 20. Both boundary types need explicit phase handling.

Preserve all mandatory story beats and their order. Include available local interactions, school/tutorial rooms, save point, chests, equipment/items, and the optional guard rejection when attempting to head back north. Stage flags must determine access; copying every map exit as always enabled would expose later story content. No later World of Ruin NPCs or post-opening events may appear.

The complete sequence is the acceptance scope. A short pitch presentation can jump between checkpoints, but its shortened runtime must not replace the playable opening. A planning allowance is 25–45 minutes for a first playthrough; this is an estimate, not a measurement of the original cartridge. The demo's actual playtime will depend on reading, battles and optional exploration.

## 2 Source of truth and deliverable status

The supplied ROM is US Rev 1, CRC32 `C0FA0464`, with the full identity in the [export manifest](../../rom-dump/ff6-narshe/manifest.json). The baseline script uses **Vicks** and Wedge. Preserve the US script and terminology such as Whelk, Tritoch, Slave Crown and GP in reference content. Terra initially appears under her unknown identity; naming screens must update party names and dialogue references consistently.

Facts in the extracted JSON come from the local ROM. Labels, table formats and the narrative order were cross-checked with the pinned [field event disassembly](https://github.com/everything8215/ff6/blob/813013276c952fdd27edcf7b8b86f17291542cbf/src/event/event_main.asm). Its [battle event scripts](https://github.com/everything8215/ff6/blob/813013276c952fdd27edcf7b8b86f17291542cbf/src/btlgfx/battle_event_script.asm) distinguish cinematic encounters from normal battles. The [scene manifest](scenes.json) records original labels and dialogue IDs so implementation can be checked without guessing from recollection.

The repository contains extraction tooling, source-pixel previews, decoded data and this specification. A playable first-battle slice now exists in `samples/ff6_narshe`, with prototype 3D maps, mount clips and rendered original audio. The complete opening, finished environment art and timing-verified recreation remain implementation work. See the sample README for current limits. Runtime performance and audiovisual parity remain acceptance gates.

## 3 Presentation contract

Use a 256 by 224 logical reference canvas to inspect source art and original UI placement. The pitch build may use a wider HD-2D viewport, but camera framing must retain the actors, reveals and story focus of the reference. Make that a declared presentation mode, with separate screenshot baselines. Do not stretch the reference image to fill a widescreen window.

Use 16 source pixels per grid cell as the conversion basis: source `(x, y)` maps to engine `(x + 0.5, height, z = y + 0.5)`, with the final foot anchor verified against a captured frame. Set sprite `pixels_per_unit` consistently to 16. Source characters are generally 16 by 24 pixels, with additional vehicle and effect graphics. Atlas padding, nearest sampling and integer canvas scaling must prevent seams and blended pixel edges.

Author buildings, cliffs, mine walls, bridges, stairs, rails and snow surfaces as dioramas using the exported tiles as texture sources. The ROM has 2D tiles and properties; it contains no ready-made glTF geometry. Geometry, depth, lighting and camera adjustments are authored work. Retain route lengths, obstacles, door locations, combat triggers and actor marks even when geometry changes.

Use the static PNG exports as an import/composition harness before environment production. They are separate SNES backgrounds with priority metadata, not finished scene screenshots. A source-priority compositor can be written in game Lua with `fe.canvas`; it is useful for extraction QA. The main HD-2D field should use world entities, geometry and depth. A complete second playable renderer is not required.

For readable source pixels in the world, begin with restrained lighting and bloom. Match cold outdoor snow against warm windows and lamps, then darker, warmer mine interiors. The camera is scripted during cinematics and follows the active party during play. The present camera is perspective; true orthographic rendering would be an optional generic engine feature if visual tests establish a need. Do not label low-FOV perspective as orthographic.

## 4 Complete scene and shot breakdown

Detailed machine-readable records are in [scenes.json](scenes.json). The following staging rules apply to every scene: scripts own input locks; simultaneous actor movements have explicit joins; camera ownership ends on completion or cancellation; dialogue cannot be overwritten by another task; victory flags commit after the battle result; repeating an already completed trigger must not replay rewards or choreography.

| Scene | Required staging and player experience | Exit condition |
| --- | --- | --- |
| Title and start | Original title/logo composition, opening music, New Game, configuration and usable naming inputs later in the story. | New Game initializes a fresh opening state. |
| Historical prologue | Four text cards over the cliffs, industrial town, Imperial scene and overlook; camera pans, fades and sound changes in source order. Source maps 23, 20, 244 and 18. | Last card yields to the three riders. |
| Overlook | Vicks and Wedge approach their marks, discuss Narshe and the Esper, turn toward Terra, explain the crown, then order the approach. Terra remains under control. Snow footsteps follow movement. | Departure motion completes and dialogue closes. |
| Snow march and credits | Three separate Magitek silhouettes with rider motion, perspective ground travel, distant Narshe, sky/horizon, snow at multiple apparent depths, credits, music and camera pacing. | Credits and march end before arrival fades in. |
| Arrival | Map 19, Terra at `(38,58)`, soldiers near `(37,60)` and `(39,60)`; march north, put Terra on point, collapse the three visible actors into the field formation. Initial 3,000 GP is granted once. | Field control is restored with the Magitek party. |
| Town resistance | One Lobo encounter, two Guards, two Lobos, the two-stage dog/guard ambush, the trapped formation, and the mine defense formation are represented by the source trigger graph. Order follows the traversed route, not a forced list of every trigger. | Each encountered group is cleared once. |
| Mine approach | Soldiers emerge as distinct actors; source discussion identifies the new shaft. Restrict unrelated town entries during the raid. | Enter the north mine route. |
| Mines and save point | Walkable rails and passages, optional containers and save interaction; appropriate random encounters; soldiers reappear for the sealed passage and break it open with a shake, sound and background change. | New route becomes walkable and stays open. |
| Whelk | Guard release, tutorial warning, head and shell targets, repeated withdrawal/emergence, shell retaliation, battle results and field restoration. | Correct boss outcome sets the cleared flag. |
| Frozen Esper | Party approaches the Esper chamber; camera pushes toward the reveal; blue pulses lead into the scripted battle presentation. Wedge disappears, then Vicks, then Terra's reaction and the flash. | Soldiers and armor are removed; Terra is relocated. |
| Arvis and awakening | Door/opening movement, Terra wakes, crown explanation, amnesia, headache/mosaic effects, Terra introduction and naming. | Name and identity state are committed. |
| Guards at the house | Cut outside to the pursuers, knocks and dialogue; return inside; Arvis opens the escape route and directs Terra into the mines. | Terra can leave via the authored back route. |
| Terra pursued | Solo field segment, guard approaches, blocked escape routes, scripted retreat and floor collapse. Fall ends with Terra unconscious in the lower cave. | Collapse cutscene takes ownership of the scene. |
| Imperial flashback | Crown conditioning/Kefka scene, Terra in Magitek destroying the staged soldiers, and Gestahl's address with mass soldier responses. Use the cinematic battle and Imperial maps, not normal player combat. | Flashback fades back to Terra in the cave. |
| Locke introduced | Arvis/Locke conversation, character introduction and naming, explanation of the girl and the Returners, direction toward Figaro. | Locke becomes the controlled rescue actor. |
| Rescue and moogles | Guards close on Terra; Locke arrives; moogles appear and surround the scene; help prompt and optional three-group tutorial. | Defense parties and pursuers initialize once. |
| Three-party defense | Three leaders remain on the cave map. Switch active party, block paths, fight roaming enemy units in side-view ATB, retain each party's health and position, defeat Marshal before an enemy reaches Terra. | Commander victory or explicit failure recovery. |
| Rescue aftermath | Restore normal actors, remove temporary parties, thank moogles, carry Terra, descend the route, set her down near the switch. | Secret-passage event begins. |
| Secret entrance | Locke activates the switch; cut outside to the mechanism; shake/sound; cut back; Terra wakes; amnesia conversation; form Locke/Terra party; move outside and close the entrance behind them. | Post-rescue state and field control are restored. |
| Local exploration | Valid local NPCs, tutorial rooms, items, menus and save access. Attempting to head back north can produce the guard rejection and direction south toward Figaro. | Player approaches the south departure trigger. |
| Departure boundary | End card appears before the world map. Offer checkpoint selection, full restart and quit. | Demo complete; no out-of-scope transition occurs. |

The source explicitly uses map variants sharing tilemaps. For example, maps 19 and 20 share south Narshe art, while maps 40/41 and 50/51 share portions of cave art with different events or palettes. Scene state must choose the variant; map image identity is insufficient.

## 5 Cinematic timing and events

Maintain a shot ledger per scene: source event/label, actors, start positions, facing, path marks, camera marks, animation clips, dialogue IDs, sound/music cues, wait durations, event dependencies and final state. A shot finishes after its required movement, dialogue or effect completes; avoid a single wall-clock timer for the whole intro.

Translate explicit source waits such as 15, 30, 45 and 90 frames into integer engine ticks. The initial timing model uses `ticks / 60` seconds because the engine updates at 60 Hz. Preserve source object-wait semantics separately from fixed waits. Calibrate actor movement and music transitions against a reference capture; a matching wait count alone does not establish matching runtime duration.

Record original actor movements and sound cues before smoothing paths for the diorama camera. The march's armor gait, footfalls, ground motion and credits need a shared timeline. The engine exposes `fe.audio.music_position` and `fe.audio.seek_music`. Use the playback clock for music-led choreography and tick-based movement/waits elsewhere; measure drift against reference capture.

Use `fe.cutscene`, `fe.task`, `fe.camera` and `fe.transition`. `cutscene.play` adds letterboxing by default; use a game-level wrapper when the source shot should use the full reference frame. Movement helpers currently wait until motion stops; that can include a blocked mover. The game must validate that an actor reached the required mark and handle a timeout instead of silently continuing a broken shot.

Checkpoint skipping must load the target checkpoint's canonical state rather than fast-forwarding asynchronous tasks. Cleanup must cancel owned tasks, close dialogue, release locks, stop temporary effects and sounds, restore the camera, and dispose of obsolete entities. A scene cannot continue firing callbacks after a checkpoint jump.

## 6 Asset conversion and environment production

[ASSETS.md](ASSETS.md) defines the asset work by category. Required conversions are:

1. Read native planar graphics and palettes, preserving transparency and separate palette variants. Assemble source metatiles with flip and priority bits.
2. Assemble character, vehicle, battle and effect metasprites. Export padded atlases and `.anim.json` clips with engine direction order south, west, north, east. Right-facing poses frequently use mirrored source art; never infer atlas rows from the ROM's ordering.
3. Import map cell layouts, NPCs, trigger positions, short/long exits and source properties into an intermediate scene format. Compile it to engine format 2 maps and game Lua definitions.
4. Translate tile properties into engine walk masks, navigation layers, ramps and links. Retain directional passage rules and stairs. Validate foreground masking separately from collision; a foreground tile does not necessarily block walking.
5. Author glTF geometry, lights, emitters, named points and camera zones. Split the large packed interiors into practical scene maps while preserving a mapping to original coordinates and exit endpoints.
6. Recreate palette cycling, animated machinery, steam, snow, lamps, cave effects and event tile mutations. Keep base map state separate from changed passage/door states.
7. Convert original dialogue escapes into structured message operations and safe engine markup. Handle page breaks, line breaks, pauses, key waits, names, alignment, choices and typewriter speed. Literal braces must not accidentally become engine tags.
8. Assemble UI windows, cursor, digits, icons, battle menus and fonts. Raw font bitmaps need glyph mapping and advance widths before use as an engine bitmap font.
9. Render the SPC music/SFX pipeline to engine-supported PCM or compressed streams, then author loop points, gains, fades and event cue IDs. BRR instrument samples alone are not finished sound effects or music.

Each compiled asset must refer back to its ROM range or native input hash, conversion recipe and authored overrides. Keep derived PNGs, atlases, sound renders and geometry separate from immutable native exports. Do not use the original event bytes as executable game logic; translate the scoped story into Lua tasks and data.

## 7 Battle behavior

Reuse `fe.battle.core`, `fe.battle.schedulers.atb`, side-view presentation and `fe.effects`, with game-specific formulas, commands, AI and presentation hooks. FF6's numbers and algorithms are not automatically provided by the generic kit. Import the required actor initialization, level growth, equipment, items, skills, enemy flags, rewards, elemental/status rules and encounter settings. Choose whether to reproduce original bugs only after a discrepancy is observed; do not claim byte-identical battle simulation from matching HP alone.

| Event group | Formation | Members | Use |
| --- | --- | --- | --- |
| 0 | 0 | Lobo | Single dog assault |
| 1 | 2 | Guard, Guard | Street resistance, also reused after a dog wave |
| 2 | 1 | Lobo, Lobo | Dog wave |
| 3 | 41 | Lobo, Guard, Guard | Trapped street party |
| 4 | 84 | Vomammoth, Vomammoth, Guard, Guard | Mine defense |
| 5 | 17 | Vomammoth, Lobo | Roaming defense unit |
| 6 | 4 | Lobo, Lobo, Marshal | Defense commander |
| 64 | 432 | Whelk shell, Head | Whelk boss |
| 77 | 447 | Tritoch | Esper cinematic encounter |
| 115 | 485 | Three Soldiers | Flashback cinematic combat |

These IDs are decoded from the local event-group/formation tables. Event-group IDs differ from formation IDs. Normal mine encounters include Were-Rats, Repo Men and Vaporites; preserve the map-specific formation weights in the [random encounter export](../../rom-dump/ff6-narshe/indices/opening_random_encounters.json), including the map flag enabling encounters. A table entry alone does not enable random combat.

Magitek command menus differ for Terra and the soldiers. Terra needs the full set represented by Fire Beam, Bolt Beam, Ice Beam, Bio Blast, Heal Force, Confuser, X-Fer and TekMissile; soldiers have the smaller elemental/healing set. Final labels, target rules, costs, damage and status behavior must come from the source data. Walking mounts and battle armor need independent compositions and animation clips. Remove mounts and Magitek commands after Esper contact.

### Whelk

Represent shell and head as separate battlers with linked encounter state and distinct hit targets. The opening formation uses monster 256 for the shell and **308** for the head. Monster 309 is a different head record and must not be substituted by matching its display name. The ROM gives shell HP 50,000 and opening head HP 1,600.

Implement exposed, withdrawing, hidden and emerging head states. Target selection must exclude the hidden head and queued actions must revalidate their target when they execute. Shell hits provoke the source electrical retaliation. Killing the head ends the normal encounter despite shell HP remaining; also define the shell-kill path and reward logic from the source. Defeat/return to field must not grant the clear flag. Include the introductory battle dialogue and the correct music/transition behavior.

### Esper contact and flashback combat

Group 77 is a scripted presentation. Disable the ordinary command prompt, AI victory evaluation and reward screen while the event owns it. Use a scripted battle scene/presenter for the blue pulses, dialogue, disappearances and Terra's reaction; the result hands control back to the story. Group 115 similarly follows the flashback's scripted actions and has no ordinary player command loop. Both must terminate by an explicit story outcome rather than accidental monster death.

### Three-party moogle defense

Create a game-owned controller with three party records, twelve independent actors, three field leader positions, active group, pursuer states, spawn/timing data, commander state and protected Terra position. The initial formations are:

| Group | Members | Leader start on map 51 |
| --- | --- | --- |
| 1 | Locke, Kupek, Kupop, Kumama | `(14,14)` |
| 2 | Mog, Kuku, Kutan, Kupan | `(13,13)` |
| 3 | Kushu, Kurin, Kuru, Kamog | `(15,13)` |

The source temporarily reuses other character slots for moogles. Give these temporary characters their own game IDs; do not overwrite permanent character records. Retain Mog's source commands and any available equipment/Dance behavior needed within this segment.

Party switching changes control and the camera while preserving inactive leaders and their party resources. Each field contact launches side-view ATB with that group's four actors. Freeze the defense simulation during battle and other modal sequences according to the source behavior validated in capture. Return to the same field coordinates and resolve that pursuer once; do not respawn it because the active party changed.

An opaque battle scene or `blocks_update` alone does not stop engine world ticks. Pause the defense field world explicitly with `world.paused`, and pause the controller's pursuit clock and tasks as well. Restore their prior states on every battle outcome, cancellation and retry. Without this, movers could reach Terra behind the battle screen even while field Lua updates are blocked.

The source initializes six moving pursuer units plus the commander. Import their path instructions, waits and collision callbacks rather than spawning arbitrary waves. If a group loses, resolve the source retreat/remaining-group behavior and return control to an eligible group. If a pursuer reaches Terra, run the failure sequence and offer retry from the defense checkpoint. Commander victory cancels every pursuit task before the rescue aftermath begins. Simultaneous contact, switching and commander defeat must resolve once, in a defined tick order.

This is a field defense with side-view battles; substituting a turn-based tactical battle would change the requested gameplay.

## 8 Engine support and gaps

Status is based on the current workspace implementation, not the README alone. All FF6-specific rules stay in the game/Lua layer under the engine's existing architecture boundary.

| Requirement | Present support | Work required |
| --- | --- | --- |
| Scene orchestration | `lualib/fe/scene.lua`, `task.lua`, `cutscene.lua`, `camera.lua` | Author story tasks, shot data, actor arrival assertions and cleanup. |
| Town/cave traversal | `engine/nav/Grid.*`, movement/collision, `lualib/fe/field.lua` | Import collision/height/layers, author maps, control phase-specific access. |
| Sprite poses and mounts | `engine/assets/SpriteAnimation.*`, entity attachment and sprite APIs | Assemble metasprites, mount/rider offsets and clips. |
| Original background priority | No SNES background loader; canvas supports ordered images | Offline conversion and optional Lua priority compositor; use diorama depth for pitch maps. |
| HD-2D environments | glTF, lights, fog, shadows, particles, weather, bloom and postprocessing | Author all geometry, lighting and scene tuning. |
| Palette effects | Sprite tint/flash/palette facilities and world effects | Import exact swaps or bake palette variants; canvas images have no indexed-palette texture API. |
| Snow march | Cameras, canvas transforms, particles and tasks | Custom game cinematic compositor or 3D ground scene, gait/credits sequencing. |
| Dialogue and choices | `dialogue.lua`, text markup, UI and name-entry screens | Translate source escapes, bitmap glyphs, layouts, tutorial choices and text waits. |
| ATB and battle rules | `battle/core.lua`, scheduler, effects and sideview presenter | FF6 formulas, Magitek, enemy AI, boss links, scripted event presenter and HUD. |
| Three-party field play | `rpg/party.lua` has one active lineup; field assumes one player | New game-owned party/defense controller and field integration. Existing active-lineup switching alone is insufficient. |
| Audio playback | `MiniAudio.*`, layered music, loop points, fades and buses | Offline SPC rendering, cue extraction, loops, source behavior for resume/restart. |
| Audio timeline synchronization | `fe.audio.music_position` and `fe.audio.seek_music` are exposed | Author cue alignment and measure drift against reference capture. |
| Fades/flash/mosaic/shake | Screen effects, transition kit and camera shake | Choreograph source sequences; custom local distortion for exact effects if necessary. |
| Saving and checkpoints | `fe.state`, `fe.save`, starter hooks and migrations | Serializable story/defense state, stable save points, checkpoint fixture factory. |
| Validation and demonstration | Headless tests, input replays, screenshots and profiler | Game tests, visual baselines, scripted pitch tour and measurements. |
| Pixel-exact orthographic world | Camera currently uses perspective | Optional generic projection service; source-pixel reference QA can use canvas. |

No mandatory C++ extension has been established yet. Two targeted experiments should settle whether audio playback time or a generic projection/compositing feature is justified. If needed, expose a general service in C++ and keep FF6 concepts in Lua. Do not embed an emulator or FF6 event interpreter into `engine/` to produce the demo.

## 9 Proposed game structure

```text
samples/ff6_narshe/                 proposed playable game destination
  game.json main.lua
  data/                            actors classes skills items enemies troops
    audio.json timelines.json      converted playback and effect definitions
    opening_scenes.json            compiled story/shot definitions
    defense_routes.json            source pursuit paths and wait instructions
  maps/                            diorama geometry and format 2 maps
    overlook/ narshe/ arvis/ north_mines/ south_mines/
    defense/ imperial/ stages/
  sprites/ portraits/ ui/ fonts/ audio/
  opening/                         director checkpoints state and event tasks
  battle/                          formulas commands Whelk scripted presenter
  defense/                         parties routes contacts outcomes
  tests/ replays/
```

The ROM archive stays in `rom-dump/ff6-narshe/`. Conversion compiles only required assets into the game folder. Native all-game exports are a lookup archive, not a reason to package unrelated ending art or the entire ROM. Keep generated source-art assets in a local build area until the pitch distribution contents are chosen.

The director owns story phase and checkpoint entry, while field/event scripts own local interactions. The battle adapter maps generic kit outcomes into story transitions. The defense controller owns its temporary lineup and world actors. The scene director must never let an ordinary defeat hook turn a scripted flashback or Esper encounter into a game-over screen.

## 10 State and save contract

Create a versioned `opening` section in `fe.state` containing phase, checkpoint ID, completed encounters, passage/door flags, save tutorial seen, player names, claimed containers, optional interaction flags and departure readiness. Store inventory, GP, actor progression and active lineup through the RPG kits. Use original flag mappings in import metadata for traceability, with descriptive names in Lua.

Create a separate `narshe_defense` section containing active group, three group lineups and leader transforms, pursuer positions/path cursors/waits/resolved flags, commander result and defense simulation tick. Actor resources belong to the actual actor records. The state must contain JSON-compatible data, never worlds, entity handles, tasks, functions or audio voice handles.

Save at stable field checkpoints and explicit save points. Disable ordinary saves midway through cinematic choreography. A defense retry rebuilds its initial fixture; a resume during defense, if offered, reconstructs entities and tasks from the saved tick and path cursors. Checkpoint jumps cancel the old scene before resetting state. Starting a new game clears all prior temporary moogles, event flags and GP grants.

Required checkpoint fixtures: prologue, overlook, march, town arrival, mine entry, pre-Whelk, Esper reveal, awakening, solo escape, flashback, Locke introduction, defense start, pre-Marshal, post-defense, secret passage and departure ready. Fixtures include inventory, HP/MP, names, party lineup, map state, music cue and expected flags; they cannot merely warp the player to a map.

## 11 Input and UI contract

Use normal engine bindings for movement, confirm, cancel and menu. Add an explicit `party_next` action for the moogle segment with keyboard and controller bindings; tutorial text must show the mapped action instead of instructing every player to press the SNES Y button. Naming, item selection, target selection, battle menus and save UI must work using a controller alone.

Keep development checkpoint controls and the profiler out of the release player's normal UI. The pitch launcher can present full opening, chapter checkpoints, controls and display/audio settings. The end card offers replay and restart. Preserve a visible distinction between playable content and development evidence such as performance statistics.

Support dialogue completion/advance without accidentally accepting a following choice on the same press. Hold/release behavior across fades, battle return, naming and party changes must not create a double confirm. Pause/focus loss must stop the simulation and audio coherently. An ordinary pause cannot advance pursuit units while the player reads a modal tutorial.

## 12 Validation and acceptance

| Gate | Evidence and passing condition |
| --- | --- |
| Extraction | Input identity checked; every native output hash verified; independent native extraction agrees; selected maps and sprites inspected. |
| Asset conversion | No missing paths, glyphs, directions, palette variants or mount components; every required scene has compiled assets and source provenance. |
| Field traversal | Every mandatory path, door and layer change is traversable; blocked/later routes remain blocked; no trigger fires twice or warps during a cutscene. |
| Complete story | Automated walkthrough reaches the departure end card through every mandatory scene, including the flashback, Whelk and moogle defense. |
| Dialogue | IDs, pages, speakers, names and choices match the US source; local tutorial prompts use actual bindings; no text overflow. |
| Whelk | Exposed/hidden target tests; shell counter; queued-target invalidation; correct head ID; head death ends encounter; defeat grants no clear flag. |
| Scripted battles | Esper and flashback complete without command UI, ordinary rewards or incidental game over; party removal and restoration happen once. |
| Defense | Switching preserves all groups; six pursuers follow their paths; contact launches once; field timer pauses appropriately; failure retries; Marshal victory cancels pursuit and restores the final party. |
| State | Save/load at supported checkpoints preserves story, inventory and names; checkpoint selection rebuilds the right phase; repeated restarts leave no stale actors/tasks/locks. |
| Visual QA | Capture overlook, march, arrival, Whelk exposed/hidden, Esper flash, awakening, flashback, three defense groups, passage and departure. Compare source compositions and separately approve diorama differences. |
| Audio QA | Correct cue order, no missing instruments/SFX, clean loops, intended restart/resume behavior, and no perceptible march/credit cue drift in a complete run. |
| Performance | On a named target machine, 60 Hz gameplay with p95 CPU and GPU frame times within a 16.67 ms budget during snow, battle effects and defense. Record actual resolution/settings, p99 spikes, peak memory, draw calls and entity/particle counts. |
| Stability | Three full playthroughs and twenty checkpoint/defense restarts without crash, runaway tasks, accumulating voices or retained worlds. |
| Scope boundary | End trigger runs before loading a world-map scene; departure lineup is Locke/Terra; no temporary moogle or soldier appears in party UI. |

Visual differences from deliberate 3D staging are approved against a shot list, not treated as pixel-error failures. For the source-pixel import harness, compare exact tiles/palettes with tolerance only for documented palette expansion differences. Headless checks cannot establish GPU rendering, audio quality or frame-time performance; those require windowed capture and listening.

The game's eventual checks should use the existing interfaces:

```sh
./build/fengine_headless --check samples/ff6_narshe
./build/fengine_headless --test samples/ff6_narshe
./build/fengine samples/ff6_narshe --seed 6 \
  --replay samples/ff6_narshe/replays/opening.json
```

These are planned commands for the future sample. They are not evidence that such a sample already exists or passes. Game-level tests should focus on story transitions, boss states, pursuit scheduling, party preservation and save reconstruction. Use windowed replays for camera, text, transitions and screenshot evidence.

## 13 Pitch presentation and implementation order

The pitch should make each claim visible: the overlook and march demonstrate direction and atmosphere; a town encounter demonstrates transition into combat; Whelk demonstrates custom battle rules; the Esper event demonstrates scripted battle presentation; defense demonstrates persistent multi-party state; the passage exit demonstrates continuity back into exploration.

Prepare a four-to-six-minute guided checkpoint tour alongside the complete playable opening. Record its input replay and provide the exact build/seed/settings used for the capture. Use measured profiler results in the pitch. Until the complete run passes, describe this as a planned capability demonstration rather than a demonstrated recreation.

Implement in the order in [BACKLOG.md](BACKLOG.md): provenance/import, a street and battle slice, defense risk prototype, full gameplay/state, cinematic/audio pass, environment production and acceptance. Estimates are planning ranges for engineering and content work; source timing, audio rendering and diorama production can change them. The original ROM's art can establish textures and sprites, but it does not remove those authoring tasks.
