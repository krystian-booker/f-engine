# f-engine — Architecture Plan

## Context
`/Users/krystian/repos/f-engine` is empty. The goal is a **lightweight engine built only for JRPGs**: the scope of FF3–6, presented in the "HD-2D" style (simple 3D worlds, 2D billboarded pixel-art characters, as in Octopath Traveler / Triangle Strategy). It is not a general-purpose engine. Every system should make *making a JRPG* easy, and the engine skips anything a JRPG doesn't need (physics, skeletal animation, open worlds, networking).

Decisions confirmed with the user:
- **C++20**. Development happens on macOS (Metal) first. Windows and Linux are supported but lower priority.
- **Lua** for gameplay (events, cutscenes, dialogue, shops, AI, damage formulas).
- **Hybrid map authoring**: Blender → glTF for geometry, plus an in-engine ImGui editor for walkability, triggers, NPCs, cameras and lights.
- **Side-view ATB battles** (FF4–6 style) first. The battle core stays mode-agnostic so a tactical mode could be added later.

## Guiding principles
1. **The engine is a runtime plus an editor. A game is a data folder.** A typical game ships as `game.json` + maps + sprites + JSON database + Lua scripts, with no C++ required. An optional C++ "game module" hook exists for advanced cases.
2. **Conventions over configuration.** Fixed concepts (Map, Actor, Party, Item, Skill, Troop, Event, Flag) instead of generic abstractions.
3. **Hot-reload everything** (Lua, textures, maps, shaders, database) for fast iteration.
4. **Fixed 60 Hz logic tick**, with interpolated rendering. JRPG logic is deterministic and simple.
5. **Small dependency set** of mature, permissively licensed libraries, all fetched via CMake.

## Tech stack
| Concern | Choice | Why |
|---|---|---|
| Rendering | **bgfx** (+bx, bimg) via `bgfx.cmake` | Native Metal/D3D11/D3D12/Vulkan/GL backends, lightweight, mature, BSD-2. *Over Diligent:* Diligent's Metal backend is commercial-only, so on macOS it would run Vulkan through MoltenVK, and it is heavier than this project needs. |
| Window/input/gamepad/filesystem | **SDL3** | Cross-platform, excellent gamepad database. On macOS, `SDL_Metal_CreateView` provides a CAMetalLayer for bgfx. |
| Scripting | **Lua 5.4 + sol2** | Coroutines give blocking-style event scripts. |
| ECS (field entities) | **EnTT** | Header-only and fast. Used only for world entities, not for RPG data. |
| Editor/debug UI | **Dear ImGui** (docking branch) | In-engine editor and debug overlays. |
| Model loading | **cgltf** | Minimal glTF 2.0 loader. |
| Images | **stb_image** (bimg for runtime textures) | |
| Audio | **miniaudio** | Single file. Handles streaming BGM with loop points, plus SFX. |
| Math | **glm** | |
| Data | **JSON** (`nlohmann/json`) | Human-editable and easy to diff. Can be swapped for a binary cook later. |
| Text | Bitmap fonts (BMFont/AngelCode) + stb_truetype fallback | Pixel-perfect JRPG text. |
| Tests | **doctest** | |
| Build | **CMake ≥ 3.25 + CPM.cmake**, Ninja | One-command bootstrap on all three OSes. |

## Repository layout
```
f-engine/
  CMakeLists.txt, cmake/ (CPM.cmake, shader compile helpers)
  engine/
    platform/   SDL3 window, timing, filesystem, file watcher
    core/       logging, asserts, handles/pools, event bus, JSON helpers, fixed-step loop
    input/      action mapping (Confirm/Cancel/Menu/Move…), keyboard+gamepad, rebinding
    render/     bgfx wrapper, camera, passes, materials, sprite batcher, post-fx, debug draw
    assets/     asset registry, typed handles, async-ready loaders, hot-reload
    audio/      BGM (loop points, crossfade), SFX, volume buses
    script/     Lua VM, sol2 bindings, coroutine scheduler
    ui/         JRPG window/skin system, text renderer w/ inline codes, menus/lists/cursor
    world/      map loading, collision grid, entities (EnTT), field camera, triggers
    rpg/        database, party, inventory, flags/vars, save/load, encounters, shops
    battle/     battle core (ATB), actions/effects, side-view stage & presentation
    scene/      scene stack: Title, Field, Battle, Menu, overlays (dialogue, fade)
    editor/     ImGui editor (map entities, nav grid painter, lights/cameras, DB viewer)
  shaders/      bgfx .sc sources (compiled with shaderc at build time per backend)
  tools/        asset cooker (texture atlas packer, sprite sheet → anim json)
  apps/
    runtime/    `fengine` executable: loads a game folder
  samples/
    sandbox/    demo game: one town, one dungeon, one battle (doubles as the test bed)
  tests/
```

## Core architecture

### Main loop
`platform` pumps SDL events → `input` builds an action snapshot → a fixed 60 Hz `update` runs the scene stack, Lua scheduler and ECS systems → `render(alpha)` builds a frame → `bgfx::frame()`. Use bgfx **single-threaded mode** at first (call `bgfx::renderFrame()` before `bgfx::init`). This avoids macOS main-thread pitfalls and can be revisited later.

### Scene stack
`SceneStack` holds `Scene` objects (`Field`, `Battle`, `MainMenu`, `Title`, `Shop`…). Overlays such as the message box, fades and transitions push on top. Lower scenes can keep rendering underneath; the menu over the field is the standard JRPG pattern. Transitions (fade, battle swirl) are scene-level effects.

### Rendering: the "HD-2D" pipeline
Coordinates: Y-up, right-handed, **1 unit = 1 tile**, plus a project-wide **pixels-per-unit** (e.g. 32 PPU) so every sprite is scaled consistently.

Passes (bgfx views):
1. **Shadow map**: one directional light (sun/moon). Sprites are drawn here too, alpha-tested, so characters cast proper shadows on 3D ground (a key part of the HD-2D look).
2. **World opaque**: glTF meshes with a simple lit material (albedo + optional normal map, Lambert + rim/ambient, up to N point lights per draw for torches and lamps), plus distance/height fog.
3. **Sprites**: camera-facing **Y-axis billboards** (stay upright), point-sampled, alpha-tested, depth-writing so they sort correctly in 3D. Lit by the same lights. Optional outline/rim. Batched per atlas.
4. **Transparent/FX**: particles, water, spell effects (sorted).
5. **Post**: bloom, **tilt-shift depth of field** (the signature HD-2D look), vignette, color grading via 3D LUT. Each effect can be toggled per map.
6. **UI**: orthographic pass in virtual resolution (e.g. 480×270 scaled with integer-ish scaling) for crisp pixel UI.

Camera: perspective with a low FOV (~25–35°) and a fixed pitch, following the player smoothly. **Camera zones** in maps override angle, distance and bounds. Cutscenes can script the camera.

Sprites: an atlas + `*.anim.json` (clips × directions (4 or 8) × frames with durations, events such as footstep SFX). Direction is chosen from the facing relative to the camera.

### Assets
A path-based registry that returns generational `Handle<T>`. Loaders exist for textures, meshes (glTF), sprite anims, maps, audio, fonts, shaders and Lua. A file watcher (dev builds only) triggers reload by handle, and references stay valid. A cooking step (atlas packing, texture compression) comes later. Raw files are used at first.

### World / field
Map = a folder containing:
- `geometry.glb`: visual meshes (from Blender). Custom properties can mark `collision`, `light`, `spawn`.
- `map.json`: authored in the engine editor. Contains:
  - **Nav grid**: per-cell height, walkable, ramp/stair direction, flags (water, counter, encounter region, damage floor). Movement is free 8-directional (Octopath-like) and resolved against this grid, with no physics engine.
  - **Entities**: NPCs, chests, doors, warps, triggers (on-touch / on-action / autorun / parallel), each with Lua event refs and optional page conditions (flag/var based, like RPG Maker event pages).
  - Lights, camera zones, BGM, encounter table, post-fx overrides.

ECS components (EnTT) include `Transform`, `SpriteRenderer`, `Mover` (path/grid movement), `Interactable`, `Trigger`, `NpcRoutine`, `Light`, `CameraTarget`.

### Scripting (Lua)
Each event runs as a **coroutine** managed by a scheduler. Blocking commands yield until done, so scripts read top to bottom:
```lua
-- events/town_elder.lua
return function(ev)
  ev:face_player()
  say("Elder", "The crystal has gone dark, {player}...")
  local c = choice({"I'll go.", "Not yet."})
  if c == 1 then
    set_flag("quest_crystal_started")
    move(ev.self, {dx = 0, dy = -2})
    fade_out(0.5); warp("cave_01", "entry"); fade_in(0.5)
  end
end
```
API groups: message/choice/input, movement/facing/animation, camera, screen fx, audio, flags/vars, party/inventory, battle start, shops/inns, save. Lua also defines **damage formulas, skill effects and enemy AI**, so game rules stay out of C++. Scripts run in sandboxed environments, and Lua state is serialized only through flags/vars (no coroutine persistence), which keeps saves simple.

### RPG framework (`rpg/`)
- **Database** (JSON, hot-reloadable): actors, classes/jobs, stats & growth curves, skills/spells, items/equipment, states (poison, etc.), elements, enemies, troops, encounter tables, shops. Validated on load with clear error messages.
- **Party / inventory / gold / flags & variables**: plain C++ structs, outside the ECS.
- **Save system**: JSON save slots (party, inventory, flags, vars, map + position, playtime). Versioned, with migrations.
- **Encounters**: step-based or distance-based rate per nav-grid region.

### Battle (`battle/`)
- **Core (presentation-agnostic)**: `Battler`, ATB gauge ticking (speed-based, wait/active modes), a command queue, an `Action` → `Effect` pipeline (damage, heal, status, revive), state ticking, victory/defeat/escape, EXP/gil/drops. Formulas and AI come from Lua. This layer can be unit-tested headless.
- **Side-view presentation**: a small 3D "diorama" stage per terrain type with enemies as sprites (or flat billboards) on one side and the party on the other. An **action sequencer** plays timelines such as step forward → anim → effect → damage popup → return. Timelines are defined in data/Lua per skill.
- **Battle UI**: command menu, target cursor, ATB bars, HP/MP window, damage numbers.

### UI (`ui/`)
A purpose-built JRPG UI rather than a general GUI:
- 9-slice **window skins**.
- A text renderer with inline codes (`{color:red}`, `{wait:20}`, `{icon:potion}`, `{player}`), a typewriter effect and portraits.
- Selectable lists/grids with cursor memory, scrollable lists, gauges and number tweening.
- Menu screens (Items, Magic, Equip, Status, Config, Save) built from these widgets. Layouts are data-driven, with optional Lua for custom screens.

### Editor (`editor/`)
The editor is built into dev builds of the runtime and toggled with F1, so editing happens while the game runs:
- Map editor: paint the nav grid (height, walkable, regions) over the 3D view; place, move and edit entities; edit lights, camera zones and post-fx; play from here.
- Inspectors: ECS entity viewer, flags/vars watcher, Lua console, battle sandbox (pick a troop and fight).
- Database viewer/editor (read-only at first, editing later).

## Milestones (implementation order)
Each milestone ends with a visible demo in `samples/sandbox`.

- **M0 – Skeleton:** CMake + CPM. Fetch SDL3, bgfx, ImGui, glm, doctest. Open a window on Metal, clear the screen, show an ImGui demo panel. Add the fixed-step loop, logging, file watcher and shader compile step.
- **M1 – HD-2D render slice:** load a glTF diorama, fixed follow camera, directional shadow map, point lights, an animated billboard sprite that casts shadows, then bloom + tilt-shift DOF + LUT. *This proves the look early.*
- **M2 – Field:** map.json + nav grid collision, player movement with gamepad and keyboard, NPCs with simple routines, interaction/touch triggers, map transitions with fades.
- **M3 – Lua & dialogue:** Lua VM, coroutine scheduler, message window with text codes, choices, flags/vars, cutscene commands, hot-reload of scripts.
- **M4 – Data, menus, save:** JSON database, party/inventory, main menu screens, shops/inns, save/load.
- **M5 – Battle:** ATB core (headless tests first), side-view stage, action sequencer, battle UI, enemy AI in Lua, random encounters, victory flow.
- **M6 – Editor:** nav grid painter, entity placement, lights/camera zones, battle sandbox, DB viewer.
- **M7 – Audio & ship:** BGM loop points and crossfades, SFX, config menu (volume, key rebinding), Windows/Linux builds, GitHub Actions CI matrix, packaging a game folder.

## First implementation step (M0) in concrete terms
- `CMakeLists.txt` + `cmake/CPM.cmake`, with targets `fengine_core` (static lib, all `engine/` modules) and `fengine` (the `apps/runtime` executable).
- `engine/platform/Window.{h,cpp}`: SDL3 window. On macOS, `SDL_Metal_CreateView` + `SDL_Metal_GetLayer` supplies `bgfx::PlatformData::nwh`. On Windows/Linux, use the native handles from SDL window properties.
- `engine/render/Renderer.{h,cpp}`: bgfx init/reset/frame, view setup, ImGui bgfx backend.
- `engine/core/App.{h,cpp}`: fixed-timestep loop and scene stack stub.
- `cmake/Shaders.cmake`: compiles `shaders/*.sc` with bgfx `shaderc` for metal / spirv / dx11 / glsl and embeds or emits them per platform.
- `samples/sandbox/game.json`: window title, virtual resolution, PPU, start map.

## Verification
- **M0:** `cmake -B build -G Ninja && cmake --build build && ./build/apps/runtime/fengine samples/sandbox` opens a window with a clear color and an ImGui panel showing the bgfx renderer type (should read *Metal*) and frame time. Resizing works, and gamepad connect/disconnect is logged.
- **Each later milestone:** the sandbox demo shows the feature. `ctest` runs doctest suites for headless systems (nav grid collision, ATB/battle core, damage formulas through Lua, save round-trip, database validation).
- **Cross-platform:** from M7, a GitHub Actions matrix (macos-latest, windows-latest, ubuntu-latest) builds and runs the tests. Before then, the Windows/Linux builds are spot-checked at the end of each milestone so platform drift is caught early.
