# Implementation coverage

The M0–M7 scope in `plan.md` is implemented. The sandbox is a playable data-only game and the reference authoring project. Art and music are original procedural placeholders; producing a full-length JRPG and its final artwork remains game content work.

| Milestone | Implemented systems | Verification |
| --- | --- | --- |
| M0 | CMake/CPM bootstrap, SDL3 native windows, single-threaded bgfx, ImGui, fixed tick, logging, watcher, shader builds | Native Metal; resize replay; SDL controller connect/disconnect; Windows D3D11 and Linux Vulkan |
| M1 | Static glTF, follow/zone camera, alpha-tested animated billboards, sprite/mesh shadows, point lights, fog, transparency/particles, HDR bloom, depth-aware tilt-shift, vignette and 3D LUT | Rendered town, grotto and battle captures; shader compilation on all three targets |
| M2 | Height/ramp nav collision, eight-direction keyboard/stick movement, NPC routines/pathing, action/touch/autorun/parallel pages, counters, encounters, fade/warp | Nav/property tests; event/page tests; town/map-transition and virtual-gamepad replays |
| M3 | Sandboxed Lua coroutines, messages/portraits, inline codes, typewriter, word wrap/pages, choices, persistent flags/variables, cutscene commands, reload | Lua tests; long-dialogue/camera/particle replay; live shader compiler error and recovery |
| M4 | Validated RPG tables, jobs/growth/learning/elements/equipment, party/inventory, Items/Magic/Equip/Status/Config/Save, shops/inn, versioned atomic slots | State/save/migration tests; shop/equip/inn/save replay; title Continue/slot load replay |
| M5 | Wait/active ATB, command queue and typed effects, Lua formulas/effects/AI, states, rewards, diorama stage, timeline movement/FX/popups, targeting UI | Headless battle tests; slime victory replay; guardian → crystal → elder reward replay; four-party visual check |
| M6 | F1 nav painter, entities/pages, spawns, Blender marker import, lights, camera zones/post, undo/redo/save/play-from, ECS/state inspectors, console, battle sandbox, database JSON editing | Editor captures; map round trips; exact picking and marker-import tests; battle sandbox/runtime checks |
| M7 | Streaming looped/crossfaded BGM, animation/action SFX, volume buses/rebinding, native module hooks, three-platform build setup, CI matrix, install/CPack and custom game ZIP tool | Audible real-time replay; atlas/animation tool smoke tests; native module lifecycle; platform tests and packaged launcher |

## Local verification

On the development machine, all **34 doctest cases and 15,497 assertions** pass; CTest's separate sandbox validation also passes. The same headless suite passes with AddressSanitizer and UndefinedBehaviorSanitizer. Validation covers four maps, nine sprite definitions, eleven Lua files and their RPG references.

macOS was built and executed natively on Apple silicon with Metal. Linux was built and executed in an Ubuntu ARM64 container, including Vulkan rendering under Xvfb/Mesa, tests and installation. Windows x64 was cross-built with MinGW; its actual PE executables ran under Wine, including all tests, sample validation and D3D11 rendering with clean shutdown. Windows shaders were compiled with Microsoft's D3DCompiler. Wine's unavailable DirectInput backend was disabled for that runtime check via an environment hint.

The native GitHub Actions matrix is configured but has not been run from this workspace. Native MSVC and physical Windows/Linux GPU drivers therefore remain CI/device verification, as do physical controller models beyond SDL's virtual controller plumbing. Release signing and store delivery are outside the engine build.

Runtime captures, logs, save profiles and generated distributions are in ignored `build*` directories. Repeatable commands and platform caveats are in [verification.md](verification.md). Build and authoring instructions are in [the README](../README.md).
