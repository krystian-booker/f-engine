# Architecture

```
game folder      main.lua · data/*.json · maps/ · sprites/ · audio/ · any Lua modules   (owned by the game)
     │ require: the game folder is searched first, so a game overrides any kit module
lualib/fe/       scene · task · signal · tween · state · save · schema · data · test
                 field · event · cutscene · dialogue · ui · rpg · effects · battle · tactical · ...
     │ sol2 bindings (fe.*), described by generated docs/api.md and lualib/types/fengine.d.lua
engine/          platform · input · assets · world · nav · render · canvas · text · audio · saves
                 Lua host · hot reload · editor · replays                         (C++ services only)
```

## The boundary rule

If a concept would appear in a JRPG design document, it does not belong in `engine/` C++. The engine knows about entities, sprites, meshes, lights, grids, paths, reachability, raycasts, text, sounds and files. It does not know about HP, parties, items, skills, battles, turns, shops, encounters or quests. `tools/ci/check_boundary.py` scans `engine/` for that vocabulary and is a CTest test.

## Libraries and programs

| Target | Contents |
| --- | --- |
| `fengine_core` | Headless: `core` (paths, JSON, asset cache, random), `assets` (images, glTF models, sprite animations), `text`, `nav`, `world`, `services` (input state, canvas command list, screen effects, audio interface, saves), `script` (Lua host and bindings), `runtime/Engine` and the headless runner |
| `fengine_runtime` | SDL3 window and input, miniaudio, bgfx renderers (world, canvas, ImGui), the editor, `MainLoop` and `ReplayHarness` |
| `fengine` | The player and development runtime |
| `fengine_headless` | `--test`, `--check` and `--write-api`; needs no GPU or display |

Services in the core are plain data. For example, the canvas records quads and the world stores entities, lights and overlays. The runtime's renderers consume that data, so all game logic, including UI, runs and is tested headless.

## A frame

`MainLoop` pumps SDL events into `InputState` and advances logic at a fixed 60 Hz. Each tick calls the game's `update(dt)`; `fe.game` runs the task scheduler and the scene stack there. Rendering then calls `draw(alpha)`. Scenes ask for worlds to be drawn (`world:draw()`) and draw UI on the canvas. The renderer draws the requested worlds, then the canvas, then the editor and error overlay.

## Lua host

- **Sandbox.** Lua 5.4 through sol2. `require` searches the game folder, then `lualib/`. `io`, `os`, `debug`, `package` loaders, `load` and `dofile` are unavailable. Development builds add a watchdog that stops runaway scripts.
- **Entry.** `main.lua` returns `{init, update, draw, file_changed, validate}`. `fe.game.main` and `fe.starter.main` build that table.
- **Waiting is Lua.** `fe.task` is a pure-Lua coroutine scheduler: `wait`, `wait_until`, `wait_signal`, `all`, `race`, `join`, `cancel` and `finally`. Engine calls return immediately; scripts wait on state, signals or `world:events()`.
- **Hot reload.** Changed Lua modules are reloaded and their tables patched in place, so live references see the new functions. Data tables reload with validation, and a broken edit keeps the old data. Maps, images and shaders reload too. Errors show in a dev overlay with a traceback and are logged in release builds.

## World

`fe.world.load(name, map)` creates a named world, and several can exist at once (the field stays loaded while a battle stage renders). A world owns:

- **Entities.** Generational handles with a transform, sprite (directional clips, tint, flash, outline, palette swap), mesh (glTF node animation), light, emitter, mover, collider, tags and a free `props` table.
- **Map grid.** Multiple layers with per-cell height, walkability, ramps and tags; links between layers for bridges; points; zones; placements. C++ never interprets placements or props; `fe.field` spawns them by type.
- **Queries.** A* paths, flood-fill reachability with step-height and blocked cells, line of sight, shapes, raycasts and mouse picking.
- **Movement.** Movers walk or jump to targets, colliding against the grid and other solid entities through a spatial hash. The resulting events are delivered through `world:events()`: `contact_begin`, `contact_end`, `arrived`, `blocked`, `landed`, `anim_event`, `anim_finished`, `zone_enter` and `zone_exit`.
- **Presentation.** Environment (ambient, sun, fog, sky gradient and backdrops, weather, post-processing with two blended LUTs, bloom, tilt-shift, pixelation), particles, tile overlays with pulse, path arrows and a cursor, and a camera.

## Rendering

The renderer uses one bgfx view set per drawn world:

1. A shadow map fitted to the camera.
2. Sky and backdrop cards.
3. Chunked map geometry, frustum-culled, lit by the 8 nearest point or spot lights (any number per world, with flicker curves).
4. Mesh entities.
5. Sprites with lighting, shadows, flash, outline and palette swap.
6. Particles, water (scrolling normals, depth tint), tile overlays and the cursor.
7. Post-processing (bloom, tilt-shift, vignette, LUT grading and blending, tint, screen transitions such as swirl, mosaic, wipe and iris).

The canvas renderer then draws the 2D UI in virtual pixels, with integer scaling where possible.

## Library kits

Each kit is ordinary Lua, documented at the top of its file. Games change behaviour through settings tables, hooks and signals, or by overriding a module.

| Kit | Purpose |
| --- | --- |
| `fe.game`, `fe.starter` | Entry loop, title, new game, continue, standard setup |
| `fe.scene`, `fe.task`, `fe.signal`, `fe.tween`, `fe.timeline` | Scene stack, coroutines, events, animation |
| `fe.state`, `fe.save`, `fe.config` | Flags and variables, save slots with metadata and migrations, player settings |
| `fe.schema`, `fe.data`, `fe.i18n` | Validated data tables (which also drive the editor's forms), localisation |
| `fe.field`, `fe.event`, `fe.cutscene`, `fe.dialogue`, `fe.camera` | Exploration, event pages and triggers, scripted scenes, message boxes, camera rigs |
| `fe.ui` | Widgets, themes and default screens (title, menu, save, config, shop, name entry) |
| `fe.rpg` | Game-declared stats and resources, modifiers, classes, actors, party, inventory |
| `fe.effects` | Phased action pipeline (before, hit, calculate, modify, apply, after) with hooks, formulas and handlers |
| `fe.battle` | Battle core with rule hooks; schedulers `atb`, `round_order`, `initiative`; presentations `sideview` and `tactical` |
| `fe.encounter`, `fe.quest`, `fe.worldmap`, `fe.daynight`, `fe.sound`, `fe.devtools` | Supporting systems |
