# Build, test and distribution

## Build options

| CMake option | Default | Purpose |
| --- | --- | --- |
| FENGINE_BUILD_RUNTIME | ON | SDL/bgfx runtime and editor |
| FENGINE_BUILD_TESTS | ON | Headless doctest suites |
| FENGINE_BUILD_TOOLS | ON | Asset validator / atlas tools |
| FENGINE_DEV | ON | F1 editor and file watching |
| FENGINE_ENABLE_SANITIZERS | OFF | Address + undefined behavior sanitizers |
| FENGINE_GAME_MODULE | empty | Directory defining an optional native module |
| FENGINE_SHADERC | empty | Override shader compiler with a host executable |
| FENGINE_PRECOMPILED_SHADER_DIR | empty | Cross-build input containing verified backend/*.bin |

`CPM_SOURCE_CACHE` can cache downloaded dependencies. A gameplay-only build needs no window server or GPU:

```sh
cmake -S . -B build-headless -G Ninja -DFENGINE_BUILD_RUNTIME=OFF -DFENGINE_ENABLE_SANITIZERS=ON
cmake --build build-headless --parallel 4
ctest --test-dir build-headless --output-on-failure
```

CTest runs the gameplay/asset suites and validates the sandbox when tools are enabled. Tests cover collision/ramp/pathfinding invariants, triggers and event pages, Lua isolation/budgets/formulas/effects, save migrations/round trips, inventory/equipment/growth, ATB/effects/rewards, text pagination, animation, mesh markers and editor picking.

## Runtime checks and replays

```sh
build/apps/runtime/fengine samples/sandbox --mute --skip-title \
  --replay tests/replays/town.json --user-data build/replay-user \
  --dump-state build/qa/town-state.json
build/apps/runtime/fengine samples/sandbox --mute \
  --replay tests/replays/load.json --user-data build/replay-user
build/apps/runtime/fengine samples/sandbox --mute --skip-title \
  --replay tests/replays/systems.json --user-data build/systems-user
build/apps/runtime/fengine samples/sandbox --mute --start-battle slimes \
  --replay tests/replays/battle.json --user-data build/battle-user
build/apps/runtime/fengine samples/sandbox --mute --skip-title \
  --replay tests/replays/gamepad.json --user-data build/gamepad-user
build/apps/runtime/fengine samples/sandbox --skip-title --replay-realtime \
  --replay tests/replays/audio.json --user-data build/audio-user
```

Replays are sorted arrays of tick-stamped commands. Logic advances exactly 1/60 second each frame; `--replay-realtime` also paces wall time for audio/listening checks. Supported fields: key/down, gamepad (connect/disconnect), button/down, axis/value (-1..1), script, resize, capture, assert_map, assert_flags, assert_gold, assert_inventory and assert_position ([x,y,z], tolerance 0.15). The virtual gamepad passes through SDL's actual device/event plumbing. Unknown fields, failed assertions and incomplete replays fail the process. `--frames` must allow all commands to run. `--capture basepath` requests a screenshot; bgfx writes `basepath.tga`. `--tools` opens F1 tools; `--shaders` overrides the backend shader directory.

Town exercises dialogue, quest acceptance, menus, saving, resize, editor and two map transitions. Load uses the slot created by Town. Systems exercises long dialogue, a shop purchase, equipment, healing/inn, camera/particles and another save. Battle drives actual command and target menus through a slime victory. Audio checks nonzero loop points, a music crossfade and SFX. Each replay should use the indicated dedicated user profile; reusing modified settings may change key mappings.

The optional `tests/replays/quest.json` runs the original guardian and elder scripts with a prepared party (equipment and EXP supplied by its test harness). Run it with `--skip-title --mute` and a separate user profile. Controller button names in replay files use SDL mapping names (`a`, `b`, `x`, `y`, `dpup`, etc.).

Development hot reload can be checked by changing an event, PNG, map or database while running. Changed Lua events take effect on their next start. Invalid candidates log errors. In the original build tree, editing `shaders/*.sc` starts a shader-only build. An invalid shader leaves the last working program active; fix the source to recover. Packaged games use their adjacent compiled shaders.

## Cross-platform checks

`.github/workflows/build.yml` configures macOS, Windows and Ubuntu native builds, tests, installation and ZIP artifacts, plus a Linux sanitizer job. Push the repository or trigger workflow_dispatch to run it. CI configuration is checked in; a local build does not mean the remote workflow has run.

`tools/ci/Dockerfile` and `tools/ci/linux-build.sh` build/test/install the full Linux runtime and run Vulkan under Xvfb/Mesa. `Dockerfile.windows-cross` and `windows-cross-build.sh` compile x64 Windows programs with MinGW. `Dockerfile.wine` and `windows-wine.sh` execute Windows tests/validation and shaderc. Wine's builtin HLSL compiler may lack needed intrinsics; use a legally obtained Microsoft SDK `d3dcompiler_47.dll` beside shaderc and `WINEDLLOVERRIDES=d3dcompiler_47=n`. It is a verification dependency, not a bundled game asset. The Wine script writes `build-windows/wine-shaders`; pass that directory through FENGINE_PRECOMPILED_SHADER_DIR when completing a cross-build. Set `SDL_JOYSTICK_DIRECTINPUT=0` for the Wine runtime smoke check when DirectInput initialization is unavailable; the native Windows default stays enabled. Native Windows CI uses its normal shader compiler directly. Wine testing supplements native Windows testing and does not certify MSVC or hardware drivers.

## Package a game

`cmake --install` installs the runtime, compiled shaders, sandbox and collected third-party notices. CPack ZIP bundles the same layout. The Python game packager accepts a different data folder, checks required files and complete shader sets, optionally runs asset validation and creates a launcher and SHA-256 manifest. It refuses to overwrite an existing output or package into the source game folder. Include `--licenses build/licenses` for redistribution.

Build a fresh Release tree with FENGINE_DEV=OFF for shipping. Test the resulting launcher from a different working directory. Packages are OS/architecture specific; Linux system libraries remain platform prerequisites, and macOS/Windows signing is handled by your distribution pipeline. Saves are outside the package. Never bundle developer profiles or player saves.

CMake's MSVC install collects the required Visual C++ runtime DLLs. When using the Python packager on an MSVC build, pass those installed DLLs with repeatable `--runtime-dependency path/to/file.dll`, or require the appropriate Visual C++ Redistributable in your installer. Static MinGW builds do not need those MSVC DLLs.
