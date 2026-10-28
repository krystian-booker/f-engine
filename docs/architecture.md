# Runtime architecture

`fengine_logic` contains platform-independent gameplay, asset decoding and tests. `fengine_core` adds SDL, bgfx, audio, the game UI and editor. `fengine` is a thin command-line application. The engine uses C++20 and Lua 5.4; dependencies and exact revisions live in `cmake/Dependencies.cmake`.

## Frame and scene lifetime

The main thread pumps SDL events and collects input edges. A bounded accumulator advances 60 Hz logic; rendering interpolates transforms and animation time. bgfx is built in single-threaded mode, including on macOS. SDL supplies a Metal layer, HWND or X11 window to bgfx's swap chain.

`SceneStack` defers mutations during callbacks. Scenes independently block updates and rendering below them. The session implements Title, Field, Dialogue, Choice, Menu, Shop and Battle layers. Message boxes keep the field visible. Battles render their own map. Field movement stops while blocking events or modal screens own control; parallel event coroutines can continue when appropriate.

`World` owns an EnTT registry for field entities. Transform, SpriteRenderer, Mover, Interactable, NpcRoutine, Light, CameraTarget and particles are scene components. RPG state lives separately as plain structs: actors, party, inventory, gold, flags, variables and playtime. Changing maps therefore does not discard the party.

## Rendering

Coordinates are right-handed and Y-up, one world unit per tile. A configurable pixels-per-unit value converts sprite dimensions. Maps use static glTF triangle meshes, optional albedo/normal textures and opaque, masked or blended materials. Upright Y-axis billboards select a directional clip relative to camera yaw.

Views render a directional shadow map, opaque scenery and depth-writing alpha-tested sprites, then sorted blended geometry and emissive particles. Sprites participate in the shadow pass. Lighting combines ambient, Lambert sunlight, PCF shadows, rim light, up to eight point lights and height/distance fog. HDR scene color and depth feed bloom and depth-aware tilt-shift blur; tone mapping, exposure, vignette and a 16³ grading LUT finish the image. Game UI draws in a centered virtual pixel canvas, with integer scaling when the viewport permits it. ImGui supplies the final draw lists and the developer interface.

## Data and scripts

Map movement resolves circles against height/walkability cells, ramps and other entities, with substeps to prevent tunneling. Cached breadth-first paths handle scripted movement and patrols. Event pages choose the last matching condition set; action, touch, autorun and parallel triggers start Lua coroutines. A running blocking event may finish after setting a flag that changes its page. Obsolete parallel page tasks are canceled.

Lua events yield typed host requests. The host resumes each request when its dialogue, movement, transition or battle completes. Events get separate global/library tables. File execution, OS/debug libraries and dynamic code loading are unavailable. Instruction budgets, nesting limits and task limits contain accidental infinite scripts; this is a game scripting environment, not a hardened hostile-code boundary.

`Battle` contains no graphics dependency. It ticks speed-based ATB, validates queued commands, applies typed effects, advances states and emits presentation events. Lua provides formulas, AI and custom effect expansion. Victory rewards apply once. The session translates action events into data timelines, staged sprite movement, particles, sounds, flashes, camera shake, popups and animated gauges.

## Reload and persistence

Typed generational asset handles provide a reusable transactional registry. Rendering caches use paths and rebuild GPU resources when watched files change. Development builds poll data and compiled shaders every quarter second. Source shader changes start an asynchronous shader build when running from the original build tree. Program creation checks stage signatures; failed reloads keep valid programs. Database edits during a battle wait until it ends, and candidate data must validate against live state.

Events read source on each start; changing an event affects its next invocation, while an already running coroutine finishes its old code. `scripts/rules.lua` reloads transactionally. Explicit map reloads preserve the current position where possible. Save slots use atomic JSON writes, version 2 with version 1 variable migration. Only persistent state is saved; active event coroutines and mid-battle presentation are not serialized.

## Optional native module

Set `-DFENGINE_GAME_MODULE=samples/modules/logging` to compile the example. A module directory defines `fengine_game_module`, links `fengine_core`, and implements `fe::createGameModule()` returning a `GameModule`. Hooks are `onStart(GameSession&)`, `onFixedUpdate(GameSession&, double)` and `onStop(GameSession&)`. The session exposes its world, database, state, scripts and renderer. Ordinary games can stay entirely data-driven.

The intended scope is compact JRPG maps and side-view battles. Static geometry, simple cell navigation and a focused RPG UI keep the engine small. Skeletal animation, physics, networking, tactical battles and compressed/binary asset cooking are extension work outside this implementation.
