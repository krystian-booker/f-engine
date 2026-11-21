# Build, test and ship

## Build options

| CMake option | Default | Purpose |
| --- | --- | --- |
| `FENGINE_BUILD_RUNTIME` | ON | SDL/bgfx runtime (`fengine`) and editor. OFF gives a headless build with no GPU or display. |
| `FENGINE_BUILD_TESTS` | ON | C++ doctests, the engine's and the sample's Lua tests, sample checks and CI lint tests |
| `FENGINE_BUILD_TOOLS` | ON | `fengine_assetc` |
| `FENGINE_DEV` | AUTO | F1 debug overlay, hot reload, the error popup and source/build-tree fallbacks for `builtin` and shaders. AUTO turns them on for Debug and off for Release, RelWithDebInfo and MinSizeRel; `ON` or `OFF` forces either. Builds with it off embed no build-machine paths (`-ffile-prefix-map` on GCC/Clang, `/pathmap` on MSVC, which also gets `/Brepro`) and leave out the ImGui demo window. |
| `FENGINE_BUILD_EDITOR` | AUTO | `fengine_editor`, the editor application. AUTO builds it when Qt 6.5 or newer is found with its private Gui headers (Fedora: `qt6-qtbase-devel qt6-qtbase-private-devel`; Debian and Ubuntu: `qt6-base-dev qt6-base-private-dev`); ON makes a missing Qt an error. It is a development tool: never installed, packaged or shipped with a game. Developed and tested on Linux (Wayland and X11) |
| `FENGINE_WARNINGS_AS_ERRORS` | OFF | Treat warnings in engine code as errors (CI turns this on) |
| `FENGINE_RUN_REPLAYS` | OFF | Also register `samples/*/replays/*.json` as windowed CTest tests (label `windowed`; needs a display and GPU) |
| `FENGINE_ENABLE_SANITIZERS` | OFF | Address and undefined-behaviour sanitizers |
| `FENGINE_SHADERC` / `FENGINE_PRECOMPILED_SHADER_DIR` | empty | Host shader compiler, or verified shader binaries for cross builds |
| `FENGINE_MACOS_BUNDLE` | OFF | macOS: build `fengine.app` with the shaders and `builtin` in `Contents/Resources` |
| `FENGINE_UNIVERSAL` | OFF | macOS: build for arm64 and x86_64 |

Every target compiles with `-ffp-contract=off` (MSVC `/fp:precise`), so floating-point results, and therefore seeds and replays, don't depend on the compiler fusing multiply-adds. Every dependency archive is pinned by SHA-256 in `cmake/Dependencies.cmake`.

`CPM_SOURCE_CACHE` caches downloaded dependencies between build trees.

Programs find `builtin` (the engine's own assets: the default font, the window skin, the input glyph sheets, the weather presets) and the compiled shaders next to their executable (development builds also look in parent folders), so every build tree stages both in the executables' folder: `<build>/builtin` with a single-configuration generator, each configuration's folder (`<build>/Release/builtin`, `<build>/Release/shaders`) with a multi-configuration one (Xcode, Ninja Multi-Config, Visual Studio). They are links to the sources and to `<build>/shaders`; on Windows they are copies each build brings up to date, and Windows development configurations remove the `builtin` copy and use the source tree, so hot reload sees edits. Builds anywhere, with any generator and `FENGINE_DEV` on or off, therefore run their tests. The build tree also gets `shader-include/bgfx_shader.sh` for compiling game shaders.

## What CTest runs

| Test | Checks |
| --- | --- |
| `fengine_tests` | C++ core: pak archives (round trip, compression, damaged and path-traversing archives, mounts and loose-file precedence, the asset cache, `require` and `fe.fs` reading a packed game, content packs laid over a packed and a loose game), `user://` images and `fe.save.thumbnail_path`, grid, paths, reachability, line of sight, text layout and markup, world entities, movement and contacts, saves (backends, the size cap, sidecar summaries, the backup kept by rename and the guard against truncated files), input maps (the `+`/`-` suffix rule, per-action collapse thresholds, recorders and focus loss, gamepad connection events and their `pads` replay record, which device is in use after key presses and releases, stick drift, mouse nudges and gamepads plugged in and out, key labels), glTF skins (loading joints and weights, skinning, blending poses, the crossfade and its `anim_finished`), the clipboard and its backends, canvas and `canvas_fit`, the Lua sandbox and bindings, editor map edits and undo, the replay harness and recorder (ordered inputs, `wait_for` timeouts), text input queueing, display requests, adaptive vsync, the frame cap, background world loading, asset eviction and the referenced set, the log file, level filter, sink hook and crash report, `game.json` HDR fields and `fe.app.display().hdr`, game ids, executable self-location, local dates, the platform backends (stat tolerance and coalesced writes) and protected table reads, and the renderer's CPU side (light binning, shadow cascades, light shadow selection and tiles, decal projection, pixel snapping, blend pass order, tangents, glTF material import, KTX headers, the HDR output transform) |
| `fengine_runtime_tests` | Runtime services without a window: audio through the mixer with no device (volumes, fades, voice release, playing/position, music seek, stem alignment, Ogg Vorbis decoding, pitch, the pitch-preserving shifter's spectrum and duration, global pause, low-pass, reverb tail, ducking envelope, positional gain and pan), SDL input translation (focus, gamepads, mouse buttons, text and IME events), gamepad layouts and rumble on SDL virtual gamepads, window focus tracking, packed audio played from inside an archive, and on bgfx's no-op renderer the GPU asset cache (map geometry merged by material and chunked) and the GPU warm-up (one step per call, decodes left to the worker, failed and reloaded assets) |
| `mirror_folder` | `cmake/MirrorFolder.cmake` in copy mode (how Windows builds stage `builtin` and shaders): a first copy, an update after files change, appear and disappear, and removal |
| `check_unicode_path` | `fengine_headless --check` on a copy of `samples/starfall` in a folder named `Józef-łódź-ゲーム` (on Windows, the executables' manifest makes arguments and paths UTF-8) |
| `lua_engine` | The engine as scripts see it, with no game code between: `fengine_headless --test tests/game` runs `tests/game/tests/engine.lua` against the fixture game the C++ tests also use (the test runner's `_before_each`/`_after_each` hooks, `require` finding only the game's own modules, a mover walking a path and reporting `arrived`, injected input, text measuring, a wrapping map) |
| `lua_sandbox` | Hostile and careless scripts (`tests/sandbox/tests`, with the fixture game's `arena` map): out-of-range sizes, speeds, positions and integers, long lines of sight, handles of destroyed entities after their slot is reused, backtracking patterns and huge `table.move`/string results stopped by the watchdog or the library limits, resolver tags that expand too much, paths with a NUL, and deep recursion through metamethods, error handlers, path cost functions, ticks and reloads |
| `check_aetherbound`, `lua_aetherbound` | The second sample: `--check` (maps, sprite sheets, models and every file its data names), then its tests: the battle rules (gauges, commands, damage, guard, ward, cover, the boss's threshold, invulnerability) and the whole story played headless, scene by scene, by a hand that presses confirm; `replay_headless_aetherbound_<name>` run its four replays (`opening`, `colossus` and `ending`, which in a window also compare twelve screenshots with `samples/aetherbound/replays/reference/vulkan/`, and `playthrough`: the prologue from the title to the end card through `dev/autoplay.lua`, which walks, talks and fights through real inputs) |
| `check_starfall` | `fengine_headless --check samples/starfall`: every map loads with its geometry and has its points on walkable ground, every sprite sheet loads with its atlas, every file, Lua module, map and audio id that maps, data tables and sprite sheets name exists, the entry module loads and its `init` runs, its `validate()` passes (the sample checks every reference between its data tables there), and one tick and one frame run without a script error (and a warning when a map's walkability bake is out of date) |
| `bake_check_starfall` | `fengine_headless --bake samples/starfall --check-only`: every map's committed walkability is the bake of its terrain and placed models (`python3 tools/generate_samples.py starfall --headless build/fengine_headless` regenerates the sample's placeholder assets and re-bakes its maps; add `--fresh` to rewrite placements, zones and map props from the generator too, which discards editor edits) |
| `lua_starfall` | The sample's `tests/*.lua` (25 tests): `tests/battle.lua` runs its battle rules against seeded random streams, and `tests/game.lua` plays the game through injected input: the title, walking, events, a chest, the world map with the strider and the skiff, the danger sensor and a battle won through the menus, a save and load, and button prompts following the device |
| `replay_headless_starfall_<name>` | Each replay of the sample (`explore`, `battle`, `devices`) run by `fengine_headless --replay` (no window; screenshots skipped) |
| `engine_boundary` | `tools/ci/check_boundary.py`: no JRPG vocabulary in `engine/` |
| `api_stubs_current` | `tools/ci/check_api.py`: `docs/api.md` and `builtin/types/fengine.d.lua` match the bindings |
| `pak_starfall`, `pak_check_starfall`, `pak_lua_starfall` | `fengine_assetc pack` archives `samples/starfall`; `--check` and `--test` then run on the archive alone |
| `package_starfall` | `tools/package_game.py --zip --self-check` on `samples/starfall`: a pak-based package for the host OS whose packaged runtime passes `--check` (runtime builds). Text read or written without naming an encoding fails it (`-X warn_default_encoding`), since Windows would use the locale's code page |
| `modelkit_selftest`, `animkit_selftest`, `texturekit_selftest`, `spritekit_selftest` | Each asset kit's `selftest.py`: its validator catches one flawed fixture per check, stays silent on clean ones, its helpers fix what they claim, and waivers and review sign-offs behave as documented (registered when `python3` has `numpy` and `pillow`) |
| `models_aetherbound`, `animations_aetherbound`, `textures_aetherbound`, `sprites_aetherbound` | `tools/<kit>/validate.py --manifest tools/aetherbound/<section>.json --engine fengine_headless`: every 3D model, animation, texture and sprite sheet `samples/aetherbound` ships passes the measured checks for its profile, loads in the engine, and has a visual review recorded against its current hash. Flaws a manifest waives are capped at their recorded count, and a file the manifest does not list fails. See each kit's `README.md` under `tools/` |
| `package_launchers` | The packager's `play.cmd` ends with a single CRLF and `play` has no CR, on any host |
| `assetc_atlas*` | `fengine_assetc atlas` packs frames into a new folder and writes each clip's `loop` |
| `assetc_cook*` | `fengine_assetc cook` turns a copy of `samples/starfall/fx` into BC7 `.ktx` files with mip chains (normal maps linear) and leaves sprite images alone |
| `replay_starfall_<name>` | With `FENGINE_RUN_REPLAYS=ON`, the windowed runtime plays the same replay and must finish with no failed assertions, screenshot differences or Lua errors (label `windowed`; `ctest -LE windowed` skips them) |

When you change a binding, refresh the API files with `python3 tools/ci/check_api.py build/fengine_headless --update`.

The names follow the folders: the replays of any folder under `samples/` are registered by themselves (`replay_headless_<sample>_<name>`), and a second sample gets `check_<sample>` and `lua_<sample>` once it is added to the list in `tests/CMakeLists.txt`.

## Lua tests

`fengine_headless --test <game> [--filter text]` runs a game's `tests/*.lua` with the test runner built into the program (`TestRunner` in `engine/runtime/Headless.cpp`); neither the engine nor a game ships a test library. Each file returns a table of named test functions, called with the helpers `t.ok`, `t.eq`, `t.near`, `t.error`, `t.tick(n)` and `t.wait_until(predicate, ticks, message, wall_seconds)`. Keys starting with `_` are not tests, and `_before_each(t)` and `_after_each(t)` run around every test of their file. Tests run synchronously in one engine and one Lua state for the whole run, with a fixed seed and a fresh temporary user-data folder: `t.tick` advances the engine (update, worlds, post_update, a draw) and returns, and `fe.harness.start()` loads the entry module and runs its `init` when a test wants the game running. A test fails when it or a hook raises or when a script error was reported while it ran; the process prints `FAIL tests.<file> :: <name>` with a traceback for each and `<n> passed, <m> failed`, and exits non-zero on any failure. [scripting.md](scripting.md#tests) has an example and the helpers in full.

The engine tests itself the same way: `tests/game` (the `lua_engine` test) holds maps, a sprite, a rigged model and a sound but no game code, and `tests/sandbox` (`lua_sandbox`) the hostile scripts.

On macOS the windowed replays need an awake display: while it sleeps, Metal presents no frames and the replays stall until CTest's timeout. Run them under `caffeinate -du` (for example `caffeinate -du ctest --test-dir build -L windowed`) on an unattended machine.


Setting `FENGINE_HDR_FORMAT=rgb10a2` makes a `--hdr` run skip the scRGB swap chain, so the RGB10A2 (HDR10) fallback can be exercised on a GPU that would otherwise take RGBA16F; `--capture` decodes either format back to sRGB.

## Replays

A replay is a JSON array of commands, each with a `tick`, or an object `{"seed": n, "commands": [...]}`. Logic advances exactly 1/60 s per tick, so a replay is deterministic for its seed (`--seed` overrides it; the default is 1). Background loads do not break that: under a replay or a recording (and in `fengine_headless --test`) `fe.world.load_progress` and `fe.app.progress` wait for the load the first time they are read and return 1, so a loading screen takes the same ticks on a fast machine, a slow one and a headless one.

| Command | Meaning |
| --- | --- |
| `{"input": [["key:Return", 1]]}` or `{"input": {"key:Return": 1}}` | Set raw inputs (see [input names](authoring.md#input-names)) in list order (what `--record` writes) or, for an object, in key order; 0 releases |
| `{"mouse": [x, y]}`, `{"wheel": n}` | Move the mouse (canvas pixels), scroll |
| `{"text": "Aria"}`, `{"text": "かな", "cursor": 2}` | Commit typed text, or set the IME's pre-edit string and cursor (as `fe.input.feed_text`) |
| `{"pads": [{"connected": true, "pad": 1, "name": "DualSense", "type": "playstation", "rumble": true, "trigger_rumble": true}, {"connected": false, "pad": 2}]}` | Connect or disconnect gamepads (as `fe.input.feed_pad`), applied before the command's inputs; `fe.input.pads()`, `device_type(pad)` and `pad_events()` follow. A disconnect needs only `pad`; a connect defaults to name `Gamepad`, type `generic`, no rumble |
| `{"lua": "code"}` | Run Lua in the game |
| `{"wait_for": "expr", "timeout": ticks}` | Pause the replay until a Lua expression is true; later ticks shift by the wait. A timeout (default 600 ticks) fails the replay; the command's other fields still run |
| `{"assert": "expr", "message": "text"}` | Fail unless the expression is true |
| `{"capture": "path.png"}` | Screenshot (windowed runs only). It fails unless it looks like a rendered frame: in a 64-level brightness histogram no level may cover more than 95% of the pixels and at least 4 levels must each hold 0.5% of them, so a black, cleared or single-colour frame fails. |
| `{"capture": "out.png", "compare": "reference/{backend}/out.png", "tolerance": 0.02}` | Screenshot and compare with a reference image (relative to the replay file; `{backend}` is the renderer backend's folder, see below). The capture is box-filtered to the reference's size (320 pixels across) and fails when the aspect ratios differ or the mean per-channel difference exceeds the tolerance (0..1, default 0.02). When the running backend has no reference, only the rendered-frame check applies (logged). |
| `{"resize": [w, h]}` | Resize the window (headless: the viewport) |

```sh
# Windowed: renders, captures and compares screenshots
./build/fengine samples/starfall --mute --user-data /tmp/starfall-user \
  --replay samples/starfall/replays/battle.json
# Headless: no window or GPU; captures and comparisons are skipped
./build/fengine_headless samples/starfall --replay samples/starfall/replays/battle.json
```

A failed assertion, a timed-out wait, a screenshot difference or a Lua error makes the process exit non-zero. Use a separate `--user-data` folder so saved settings don't change key bindings; headless replays use a fresh temporary folder unless you pass `--user-data`. The windowed runtime also writes `log.txt` (and `crash.txt` after a crash) into the user-data folder, which is the first place to look when a replay fails on a CI machine.

To make a replay by playing, run `fengine <game> --record session.json`. It writes the seed and every raw input, mouse position, wheel movement, typed text, IME pre-edit string and gamepad connection or disconnection (`pads`, including the pads present at start, written before that tick's inputs) per tick; add `assert` or `capture` commands by hand afterwards. Recordings and replays use the engine's in-memory clipboard and the `en-US` locale, so neither depends on the machine.

### Reference images

Rendering differs between backends, so references are kept per backend: `{backend}` in a `compare` path is bgfx's renderer name in lower case with only letters and digits (`metal`, `vulkan`, `direct3d11`, `opengl21`). They are stored 320 pixels across, which makes the 3D scene independent of the window's pixel density and averages out small rasterisation differences between GPUs of one backend. The canvas is scaled by whole multiples of the virtual resolution, so canvas drawing lands differently at another pixel size: capture references at the window size the replays run at in CI (1280x720 on Linux, the Retina size of the default window on macOS). The sample's replays (`samples/starfall/replays/explore.json`, `battle.json` and `devices.json`) compare every capture with `reference/{backend}/<name>.png`; the sample ships Vulkan and OpenGL references (`samples/starfall/replays/reference/vulkan/`, `reference/opengl43/`, captured on Linux under Xvfb with Mesa's lavapipe and llvmpipe, the renderers CI uses). On another backend (Metal, Direct3D) those replays only check that each capture is a rendered frame until references exist for it.

To add references for a backend (or refresh them after an intended rendering change), run the replay on a machine with that backend with `--write-references`, inspect the written images, and commit them:

```sh
cd build/tests   # captures are relative to the working directory, as under ctest
for replay in explore battle devices; do
  ../fengine ../../samples/starfall --mute --user-data /tmp/starfall-ref \
    --replay ../../samples/starfall/replays/$replay.json --write-references
done
```

A long replay need not render every tick. `--replay-skip <n>` lets a replay that is not paced in real time tick up to `n` times without rendering between frames; the tick of every capture is still rendered (as are the frames while a capture is written and while the screen is frozen), so the screenshots are the ones a run without it takes. The windowed CTest replays run with `--replay-skip 30`: `samples/aetherbound/replays/playthrough.json` is ten minutes of game, and a software renderer draws a handful of frames a second.

This writes `reference/vulkan/` (or `direct3d11/`, `opengl43/`...) next to the replay. Linux picks Vulkan where available and OpenGL otherwise (`VK_ICD_FILENAMES=/nonexistent.json` forces OpenGL, for its references); Windows uses Direct3D 11. On Linux, run it under `xvfb-run -a -s "-screen 0 1280x720x24"` as CI does. A run with `--write-references` still applies the rendered-frame check but compares nothing.

## Hot reload checks

Run the sample in a development build (`./build/fengine samples/starfall`), then:

1. Edit a rule or a screen (a formula in a function of `game/battle/rules.lua`, a label drawn by `game/menu.lua`); the next call uses the new code and the game in progress is kept (see [Hot reload](architecture.md#hot-reload) for exactly what is kept: functions are replaced, a module's data, including its settings tables, keeps its live values). `ScriptTests.cpp` covers the contract, and `fe.harness.reload` reloads a module from a Lua test.
2. Edit a data table (`data/items.json`); the sample's `file_changed` reloads it, and an invalid edit logs an error and keeps the old table.
3. Edit an image, a sprite sheet or a model; it reloads. A `map.json` edited outside the editor is read the next time the map is loaded.
4. Introduce a Lua error; the overlay shows the traceback until the file is fixed.

From the original build tree, editing `shaders/*.sc` rebuilds the shaders in the background, and the last working program stays active if compilation fails. Editing a game's own `<game>/shaders/fs_*.sc` recompiles that shader within a second; a failed compile is logged and, until it is fixed, a post pass is skipped and a mesh or sprite draws with the engine's shader.

The sample's replays exercise the game and the presentation it uses in the windowed runtime: `explore.json` the title, the town with bloom, graded colour and tilt-shift depth of field, a message box, the wrapping world map and the strider; `battle.json` the freeze-frame swirl into a battle stage, command and target menus, a broken foe and the rigged, skinned boss; `devices.json` the button prompts on a keyboard, after a virtual gamepad is plugged in, and in the menu. Their captures land in `replay-out/` under the working directory, and each is compared with the backend's reference image (see [Reference images](#reference-images)). No replay draws render targets, screen rects, distortion, save thumbnails or game shaders at present (the sample's `shaders/` folder holds an example post, mesh and sprite shader that its maps and scripts do not use): for those only the CPU side is tested, by `PostTests.cpp` and `RenderTests.cpp` in `fengine_tests` and `ShaderTests.cpp` in `fengine_runtime_tests`.

## Continuous integration

`.github/workflows/build.yml` runs these jobs:

- **Format**: `clang-format` 23.1.3 (`pipx run --spec clang-format==23.1.3 clang-format -i <files>` fixes it) against `.clang-format`.
- **Desktop** (macOS, Windows, Ubuntu): development builds (`FENGINE_DEV=ON`, optimised) with warnings as errors and every headless test, including each sample replay run headless. The Ubuntu job installs `numpy` and `pillow`, so it also runs the asset validation tests.
- **Ship** (macOS, Windows, Ubuntu): `FENGINE_DEV=OFF` builds, their tests, then `package_game.py --zip --self-check` on `samples/starfall`. The ZIP is unpacked; `fengine_assetc list` must show `game.pak` holding the game and `builtin.pak` the engine's assets (`assets/weather.json` among them), without tests, replays or saves, with no loose copies beside them and with no `.lua` file in `builtin.pak`, since the engine ships no Lua; and the packaged runtime runs `--check` on the packaged game. The engine install ZIP (`cpack`) and the game ZIP are the artifacts.
- **Universal** (macOS): a `FENGINE_UNIVERSAL=ON` ship build; `lipo -archs` must show arm64 and x86_64 in `fengine`, `fengine_headless` and `fengine_assetc`, and the headless tests run on the runner's native slice.
- **Replays**: the windowed replays, each capture checked as a rendered frame and compared with references for the backend where they exist. Ubuntu runs them under Xvfb with Mesa (lavapipe Vulkan, falling back to OpenGL), compares with the Vulkan or OpenGL references and must pass; a capture with no reference for the backend fails the job, so a backend change cannot quietly skip the comparison. macOS (Metal) and Windows (D3D11 on the basic render driver, not always available) have no references for the sample, so their captures are only checked as rendered frames, and both jobs are reported without failing the workflow.
- **Sanitizers** (Ubuntu): address and undefined-behaviour sanitizers over every test that needs no display, including the runtime tests and headless replays.

Day-to-day development builds and tests on macOS only (the development, ship and sanitizer configurations above); CI covers Linux and Windows. Run the local Linux and Windows reproductions below only when chasing a CI failure on those platforms: they take much longer.

The scripts in `tools/ci/` reproduce Linux and Windows (MinGW cross build and Wine test run) in Docker:

- `linux-build.sh` runs the headless tests, then the windowed replays under `xvfb-run`.
- `windows-cross-build.sh tools` builds `shaderc.exe`, the doctests, `fengine_headless.exe` and `fengine_assetc.exe`.
- `windows-wine.sh` runs the doctests, the engine's Lua tests (`tests/game`) and the sandbox tests, every sample's `--check`, `--test` and headless replays under Wine, and compiles the D3D11 shaders with `shaderc.exe`, which loads Microsoft's `d3dcompiler_47.dll` from bgfx's sources (Wine's built-in HLSL compiler lacks functions bgfx's shader library uses).
- `windows-cross-build.sh runtime` then builds `fengine.exe` with those shaders (`FENGINE_PRECOMPILED_SHADER_DIR`), which places them in `build-windows/shaders/dx11` where the runtime and the packager read them. `windows-wine.sh runtime` runs the runtime tests and `fengine.exe --check` on each sample.

## Package a game

```sh
python3 tools/package_game.py --runtime build-release/fengine --assetc build-release/tools/assetc/fengine_assetc \
  --shaders build-release/shaders --game path/to/game --validator build-release/fengine_headless \
  --licenses build-release/licenses --output dist/my-game --zip --self-check
```

The packager:

- runs `--check` on the game first;
- verifies complete shader sets;
- archives the game (without tests, replays, saves, `shaders/` or `packs/`) as `game.pak` and the engine's `builtin` folder (without `types/`, the editor stubs) as `builtin.pak` with `fengine_assetc pack`, and reads each archive back to check it holds exactly those files. `--builtin <folder>` names the folder (default: `builtin/` next to `tools/`); it must hold `assets/weather.json`;
- copies the game's content packs, `<game>/packs/*.pak` (DLC, built with `fengine_assetc pack` from their own folders), to `game/packs/` beside `game.pak`, where the runtime mounts them over the game in name order at start; the manifest hashes their entries like every other archive's. A pack sold later is the same file dropped into that folder (or, by the player, into `<user data>/packs/`);
- compiles the game's own `shaders/vs_*.sc` and `fs_*.sc` with `--shaderc` (include folders from `--shader-include`, e.g. `<build>/shader-include`) for every backend in `--shaders`, into `game/shaders/<backend>/*.bin`, which stay loose beside `game.pak`;
- with `--cook`, runs `fengine_assetc cook <folder>` over the staged game (it must list a `cook` command) and ships the `.ktx` files it writes loose in `game/`, next to the images they replace;
- lays out the package for the runtime's platform (detected from the executable, or `--platform`):
  - Windows and Linux: the runtime in `bin/`, the shaders in `bin/shaders`, `bin/builtin.pak`, `game.pak` (and `game/` when there are loose files), and a `play.cmd` or `play` launcher;
  - macOS: `<title>.app`, with the runtime as `Contents/MacOS/fengine` and the shaders, `builtin.pak`, `game.pak` and `game/` in `Contents/Resources`. It opens like any application; no Terminal window.
- signs:
  - macOS: ad hoc when `codesign` is available, or with `--sign-identity "Developer ID Application: ..."` (hardened runtime, secure timestamp); `--notarize-profile <keychain profile>` then submits the app with `xcrun notarytool submit --wait` and staples the ticket with `xcrun stapler staple`;
  - Windows: `--signtool "signtool sign /fd SHA256 /a /tr <timestamp url> /td SHA256"` runs that command with every `.exe` and `.dll` in `bin/` appended;
- with `--appimage` (Linux, needs `appimagetool` on `PATH` or `--appimagetool`), also writes `<name>-x86_64.AppImage` holding the same runtime and archives;
- writes `README.txt`, the licenses and a `manifest.json` with the SHA-256 of every file and of every archive entry, then checks the output folder, its archives and the ZIP against it;
- with `--self-check`, runs the packaged runtime's `--check` on the packaged game (packages for the host OS).

Run without a game folder, `fengine` plays the game packaged next to it (`<base>/game` or `<base>/../game`, as a folder or `game.pak`, where `<base>` is the executable's folder or the app's `Resources`). Shaders and `builtin` (or `builtin.pak`) are looked up from the same base path, never from the build tree. A package holds no Lua but the game's own. In ship builds an archive entry wins over a loose file at the same path; development builds prefer the loose file, so an unpacked folder next to an archive can override it while testing.

It refuses to overwrite existing output. Build release packages with `FENGINE_DEV` off (the default for Release). On MSVC builds, either link the C runtime statically (`-DCMAKE_MSVC_RUNTIME_LIBRARY=MultiThreaded`, as CI does) or pass the Visual C++ runtime DLLs with `--runtime-dependency`. Saves go to the platform's per-user folder, never into the package.

`cmake --install build --prefix <dir>` installs the runtime, `fengine_headless`, the shaders and `builtin` under `bin/`, and the sample under `games/`. `cpack -G ZIP` zips the same layout.
