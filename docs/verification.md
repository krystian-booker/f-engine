# Build, test and ship

## Build options

| CMake option | Default | Purpose |
| --- | --- | --- |
| `FENGINE_BUILD_RUNTIME` | ON | SDL/bgfx runtime (`fengine`) and editor. OFF gives a headless build with no GPU or display. |
| `FENGINE_BUILD_TESTS` | ON | C++ doctests, Lua tests, sample checks and CI lint tests |
| `FENGINE_BUILD_TOOLS` | ON | `fengine_assetc` |
| `FENGINE_DEV` | AUTO | F1 editor, hot reload, the error overlay and source/build-tree fallbacks for `lualib` and shaders. AUTO turns them on for Debug and off for Release, RelWithDebInfo and MinSizeRel; `ON` or `OFF` forces either. Builds with it off embed no build-machine paths (`-ffile-prefix-map` on GCC/Clang, `/pathmap` on MSVC, which also gets `/Brepro`) and leave out the ImGui demo window. |
| `FENGINE_WARNINGS_AS_ERRORS` | OFF | Treat warnings in engine code as errors (CI turns this on) |
| `FENGINE_RUN_REPLAYS` | OFF | Also register `samples/*/replays/*.json` as windowed CTest tests (label `windowed`; needs a display and GPU) |
| `FENGINE_ENABLE_SANITIZERS` | OFF | Address and undefined-behaviour sanitizers |
| `FENGINE_SHADERC` / `FENGINE_PRECOMPILED_SHADER_DIR` | empty | Host shader compiler, or verified shader binaries for cross builds |
| `FENGINE_MACOS_BUNDLE` | OFF | macOS: build `fengine.app` with the shaders and `lualib` in `Contents/Resources` |
| `FENGINE_UNIVERSAL` | OFF | macOS: build for arm64 and x86_64 |

Every target compiles with `-ffp-contract=off` (MSVC `/fp:precise`), so floating-point results, and therefore seeds and replays, don't depend on the compiler fusing multiply-adds. Every dependency archive is pinned by SHA-256 in `cmake/Dependencies.cmake`.

`CPM_SOURCE_CACHE` caches downloaded dependencies between build trees.

Programs find `lualib` and the compiled shaders next to their executable (development builds also look in parent folders), so every build tree stages both in the executables' folder: `<build>/lualib` with a single-configuration generator, each configuration's folder (`<build>/Release/lualib`, `<build>/Release/shaders`) with a multi-configuration one (Xcode, Ninja Multi-Config, Visual Studio). They are links to the sources and to `<build>/shaders`; on Windows they are copies each build brings up to date, and Windows development configurations remove the `lualib` copy and use the source tree, so hot reload sees edits. Builds anywhere, with any generator and `FENGINE_DEV` on or off, therefore run their tests. The build tree also gets `shader-include/bgfx_shader.sh` for compiling game shaders.

## What CTest runs

| Test | Checks |
| --- | --- |
| `fengine_tests` | C++ core: pak archives (round trip, compression, damaged and path-traversing archives, mounts and loose-file precedence, the asset cache, `require` and `fe.fs` reading a packed game, content packs laid over a packed and a loose game), `user://` images and `fe.save.thumbnail_path`, grid, paths, reachability, line of sight, text layout and markup, world entities, movement and contacts, saves (backends, the size cap, sidecar summaries, the backup kept by rename and the guard against truncated files), input maps (the `+`/`-` suffix rule, per-action collapse thresholds, recorders and focus loss, gamepad connection events and their `pads` replay record), the clipboard and its backends, canvas and `canvas_fit`, the Lua sandbox and bindings, editor map edits and undo, the replay harness and recorder (ordered inputs, `wait_for` timeouts), text input queueing, display requests, adaptive vsync, the frame cap, background world loading, asset eviction and the referenced set, the log file, level filter, sink hook and crash report, `game.json` HDR fields and `fe.app.display().hdr`, game ids, executable self-location, local dates, the platform backends (stat tolerance and coalesced writes) and protected table reads, and the renderer's CPU side (light binning, shadow cascades, light shadow selection and tiles, decal projection, pixel snapping, blend pass order, tangents, glTF material import, KTX headers, the HDR output transform) |
| `fengine_runtime_tests` | Runtime services without a window: audio through the mixer with no device (volumes, fades, voice release, playing/position, music seek, stem alignment, pitch, the pitch-preserving shifter's spectrum and duration, global pause, low-pass, reverb tail, ducking envelope, positional gain and pan), SDL input translation (focus, gamepads, mouse buttons, text and IME events), gamepad layouts and rumble on SDL virtual gamepads, window focus tracking |
| `mirror_folder` | `cmake/MirrorFolder.cmake` in copy mode (how Windows builds stage `lualib` and shaders): a first copy, an update after files change, appear and disappear, and removal |
| `check_unicode_path` | `fengine_headless --check` on a copy of `samples/ridgeline` in a folder named `Józef-łódź-ゲーム` (on Windows, the executables' manifest makes arguments and paths UTF-8) |
| `lua_library` | Every kit, against the fixture game in `lualib/tests/game` (`fengine_headless --test`) |
| `lua_sandbox` | Hostile and careless scripts (`tests/sandbox/tests`, with the fixture game's `arena` map): out-of-range sizes, speeds, positions and integers, long lines of sight, handles of destroyed entities after their slot is reused, backtracking patterns and huge `table.move`/string results stopped by the watchdog or the library limits, resolver tags that expand too much, paths with a NUL, and deep recursion through metamethods, error handlers, path cost functions, ticks and reloads |
| `check_<sample>` | `fengine_headless --check`: data tables against schemas, maps, placements against their types, script syntax and asset paths |
| `lua_<sample>` | Each sample's `tests/*.lua`, which drive real game flows headless: events, council votes, battles and AI-vs-AI tactics |
| `replay_headless_<sample>_<name>` | Each sample replay run by `fengine_headless --replay` (no window; screenshots skipped) |
| `engine_boundary` | `tools/ci/check_boundary.py`: no JRPG vocabulary in `engine/` |
| `api_stubs_current` | `tools/ci/check_api.py`: `docs/api.md` and `lualib/types/fengine.d.lua` match the bindings |
| `pak_vale`, `pak_check_vale`, `pak_lua_vale` | `fengine_assetc pack` archives `samples/vale`; `--check` and `--test` then run on the archive alone |
| `package_vale` | `tools/package_game.py --zip --self-check` on `samples/vale`: a pak-based package for the host OS whose packaged runtime passes `--check` (runtime builds). Text read or written without naming an encoding fails it (`-X warn_default_encoding`), since Windows would use the locale's code page |
| `package_launchers` | The packager's `play.cmd` ends with a single CRLF and `play` has no CR, on any host |
| `assetc_atlas*` | `fengine_assetc atlas` packs frames into a new folder and writes each clip's `loop` |
| `assetc_cook*` | `fengine_assetc cook` turns a copy of `samples/wayfarers/fx` into BC7 `.ktx` files with mip chains (normal maps linear) and leaves sprite images alone |
| `replay_<sample>_<name>` | With `FENGINE_RUN_REPLAYS=ON`, the windowed runtime plays the same replay and must finish with no failed assertions, screenshot differences or Lua errors (label `windowed`; `ctest -LE windowed` skips them) |

When you change a binding, refresh the API files with `python3 tools/ci/check_api.py build/fengine_headless --update`.

On macOS the windowed replays need an awake display: while it sleeps, Metal presents no frames and the replays stall until CTest's timeout. Run them under `caffeinate -du` (for example `caffeinate -du ctest --test-dir build -L windowed`) on an unattended machine.


Setting `FENGINE_HDR_FORMAT=rgb10a2` makes a `--hdr` run skip the scRGB swap chain, so the RGB10A2 (HDR10) fallback can be exercised on a GPU that would otherwise take RGBA16F; `--capture` decodes either format back to sRGB.

## Replays

A replay is a JSON array of commands, each with a `tick`, or an object `{"seed": n, "commands": [...]}`. Logic advances exactly 1/60 s per tick, so a replay is deterministic for its seed (`--seed` overrides it; the default is 1).

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
./build/fengine samples/ridgeline --mute --user-data /tmp/ridge-user \
  --replay samples/ridgeline/replays/battle.json
# Headless: no window or GPU; captures and comparisons are skipped
./build/fengine_headless samples/ridgeline --replay samples/ridgeline/replays/battle.json
```

A failed assertion, a timed-out wait, a screenshot difference or a Lua error makes the process exit non-zero. Use a separate `--user-data` folder so saved settings don't change key bindings; headless replays use a fresh temporary folder unless you pass `--user-data`. The windowed runtime also writes `log.txt` (and `crash.txt` after a crash) into the user-data folder, which is the first place to look when a replay fails on a CI machine.

To make a replay by playing, run `fengine <game> --record session.json`. It writes the seed and every raw input, mouse position, wheel movement, typed text, IME pre-edit string and gamepad connection or disconnection (`pads`, including the pads present at start, written before that tick's inputs) per tick; add `assert` or `capture` commands by hand afterwards. Recordings and replays use the engine's in-memory clipboard and the `en-US` locale, so neither depends on the machine.

### Reference images

Rendering differs between backends, so references are kept per backend: `{backend}` in a `compare` path is bgfx's renderer name in lower case with only letters and digits (`metal`, `vulkan`, `direct3d11`, `opengl21`). They are stored 320 pixels across, which makes the 3D scene independent of the window's pixel density and averages out small rasterisation differences between GPUs of one backend. The canvas is scaled by whole multiples of the virtual resolution, so canvas drawing lands differently at another pixel size: capture references at the window size the replays run at in CI (1280x720 on Linux, the Retina size of the default window on macOS). `samples/vale/replays/postfx.json` and `samples/wayfarers/replays/render.json` compare every capture with `reference/{backend}/<name>.png`; the samples ship Metal references (`samples/*/replays/reference/metal/`, captured on Apple GPU hardware) and Vulkan and OpenGL references (`reference/vulkan/`, `reference/opengl43/`, captured on Linux under Xvfb with Mesa's lavapipe and llvmpipe, the renderers CI uses). On another backend those replays only check that each capture is a rendered frame until references exist for it.

To add references for a backend (or refresh them after an intended rendering change), run the replay on a machine with that backend with `--write-references`, inspect the written images, and commit them:

```sh
cd build/tests   # captures are relative to the working directory, as under ctest
../fengine ../../samples/vale --mute --user-data /tmp/vale-ref \
  --replay ../../samples/vale/replays/postfx.json --write-references
../fengine ../../samples/wayfarers --mute --user-data /tmp/wayfarers-ref \
  --replay ../../samples/wayfarers/replays/render.json --write-references
```

This writes `reference/vulkan/` (or `direct3d11/`, `opengl43/`...) next to the replay. Linux picks Vulkan where available and OpenGL otherwise (`VK_ICD_FILENAMES=/nonexistent.json` forces OpenGL, for its references); Windows uses Direct3D 11. On Linux, run it under `xvfb-run -a -s "-screen 0 1280x720x24"` as CI does. A run with `--write-references` still applies the rendered-frame check but compares nothing.

## Hot reload checks

Run a sample, then:

1. Edit a battle rule or UI module; the next call uses the new code and the game state is kept (see [Hot reload](architecture.md#hot-reload) for exactly what is kept). `ScriptTests.cpp` covers the contract, including reloading `fe.task`, `fe.state` and `fe.scene` with live tasks, flags and scenes; `lualib/tests/game/tests/battle.lua` reloads the battle, effects and RPG modules in the middle of a battle (`fe.harness.reload`) and checks that it finishes with no effect hook duplicated or lost.
2. Edit a data table; it reloads after validation, and an invalid edit logs an error and keeps the old data.
3. Edit a map or image; it reloads.
4. Introduce a Lua error; the overlay shows the traceback until the file is fixed.

From the original build tree, editing `shaders/*.sc` rebuilds the shaders in the background, and the last working program stays active if compilation fails. Editing a game's own `<game>/shaders/fs_*.sc` recompiles that post shader within a second; a failed compile is logged and the pass is skipped until it is fixed.

`samples/vale/replays/postfx.json` exercises the presentation features in the windowed runtime: bloom, occlusion and depth-aware depth of field, a minimap render target and a picture-in-picture rect, distort sprites and particles, the sample's `shaders/fs_sepia.sc` post shader, a world-only save thumbnail, and freeze-frame shatter and radial blur. Its captures land in `replay-out/` under the working directory and the thumbnail at `fe.save` thumbnail path in the user-data folder; each capture is compared with the backend's reference image (see [Reference images](#reference-images)).

## Continuous integration

`.github/workflows/build.yml` runs these jobs:

- **Format**: `clang-format` 23.1.3 (`pipx run --spec clang-format==23.1.3 clang-format -i <files>` fixes it) against `.clang-format`.
- **Desktop** (macOS, Windows, Ubuntu): development builds (`FENGINE_DEV=ON`, optimised) with warnings as errors and every headless test, including each sample replay run headless.
- **Ship** (macOS, Windows, Ubuntu): `FENGINE_DEV=OFF` builds, their tests, then `package_game.py --zip --self-check` on `samples/vale`. The ZIP is unpacked; `fengine_assetc list` must show `game.pak` and `lualib.pak` holding the game and library without tests, replays or saves, with no loose copies beside them; and the packaged runtime runs `--check` on the packaged game. The engine install ZIP (`cpack`) and the game ZIP are the artifacts.
- **Universal** (macOS): a `FENGINE_UNIVERSAL=ON` ship build; `lipo -archs` must show arm64 and x86_64 in `fengine`, `fengine_headless` and `fengine_assetc`, and the headless tests run on the runner's native slice.
- **Replays**: the windowed replays, each capture checked as a rendered frame and compared with references for the backend where they exist. Ubuntu runs them under Xvfb with Mesa (lavapipe Vulkan, falling back to OpenGL), compares with the Vulkan or OpenGL references and must pass; a capture with no reference for the backend fails the job, so a backend change cannot quietly skip the comparison. macOS (compared with the Metal references, captured on Apple GPU hardware, which the runners' virtual Metal device need not match) and Windows (D3D11 on the basic render driver, not always available) are reported without failing the workflow.
- **Sanitizers** (Ubuntu): address and undefined-behaviour sanitizers over every test that needs no display, including the runtime tests and headless replays.

Day-to-day development builds and tests on macOS only (the development, ship and sanitizer configurations above); CI covers Linux and Windows. Run the local Linux and Windows reproductions below only when chasing a CI failure on those platforms: they take much longer.

The scripts in `tools/ci/` reproduce Linux and Windows (MinGW cross build and Wine test run) in Docker:

- `linux-build.sh` runs the headless tests, then the windowed replays under `xvfb-run`.
- `windows-cross-build.sh tools` builds `shaderc.exe`, the doctests, `fengine_headless.exe` and `fengine_assetc.exe`.
- `windows-wine.sh` runs the doctests, the library and sandbox tests, every sample's `--check`, `--test` and headless replays under Wine, and compiles the D3D11 shaders with `shaderc.exe`, which loads Microsoft's `d3dcompiler_47.dll` from bgfx's sources (Wine's built-in HLSL compiler lacks functions bgfx's shader library uses).
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
- archives the game (without tests, replays, saves, `shaders/` or `packs/`) as `game.pak` and `lualib` (without tests) as `lualib.pak` with `fengine_assetc pack`, and reads each archive back to check it holds exactly those files;
- copies the game's content packs, `<game>/packs/*.pak` (DLC, built with `fengine_assetc pack` from their own folders), to `game/packs/` beside `game.pak`, where the runtime mounts them over the game in name order at start; the manifest hashes their entries like every other archive's. A pack sold later is the same file dropped into that folder (or, by the player, into `<user data>/packs/`);
- compiles the game's own `shaders/vs_*.sc` and `fs_*.sc` with `--shaderc` (include folders from `--shader-include`, e.g. `<build>/shader-include`) for every backend in `--shaders`, into `game/shaders/<backend>/*.bin`, which stay loose beside `game.pak`;
- with `--cook`, runs `fengine_assetc cook <folder>` over the staged game (it must list a `cook` command) and ships the `.ktx` files it writes loose in `game/`, next to the images they replace;
- lays out the package for the runtime's platform (detected from the executable, or `--platform`):
  - Windows and Linux: the runtime in `bin/`, the shaders in `bin/shaders`, `bin/lualib.pak`, `game.pak` (and `game/` when there are loose files), and a `play.cmd` or `play` launcher;
  - macOS: `<title>.app`, with the runtime as `Contents/MacOS/fengine` and the shaders, `lualib.pak`, `game.pak` and `game/` in `Contents/Resources`. It opens like any application; no Terminal window.
- signs:
  - macOS: ad hoc when `codesign` is available, or with `--sign-identity "Developer ID Application: ..."` (hardened runtime, secure timestamp); `--notarize-profile <keychain profile>` then submits the app with `xcrun notarytool submit --wait` and staples the ticket with `xcrun stapler staple`;
  - Windows: `--signtool "signtool sign /fd SHA256 /a /tr <timestamp url> /td SHA256"` runs that command with every `.exe` and `.dll` in `bin/` appended;
- with `--appimage` (Linux, needs `appimagetool` on `PATH` or `--appimagetool`), also writes `<name>-x86_64.AppImage` holding the same runtime and archives;
- writes `README.txt`, the licenses and a `manifest.json` with the SHA-256 of every file and of every archive entry, then checks the output folder, its archives and the ZIP against it;
- with `--self-check`, runs the packaged runtime's `--check` on the packaged game (packages for the host OS).

Run without a game folder, `fengine` plays the game packaged next to it (`<base>/game` or `<base>/../game`, as a folder or `game.pak`, where `<base>` is the executable's folder or the app's `Resources`). Shaders and `lualib` (or `lualib.pak`) are looked up from the same base path, never from the build tree. In ship builds an archive entry wins over a loose file at the same path; development builds prefer the loose file, so an unpacked folder next to an archive can override it while testing.

It refuses to overwrite existing output. Build release packages with `FENGINE_DEV` off (the default for Release). On MSVC builds, either link the C runtime statically (`-DCMAKE_MSVC_RUNTIME_LIBRARY=MultiThreaded`, as CI does) or pass the Visual C++ runtime DLLs with `--runtime-dependency`. Saves go to the platform's per-user folder, never into the package.

`cmake --install build --prefix <dir>` installs the runtime, `fengine_headless`, shaders, `lualib` and the samples. `cpack -G ZIP` zips the same layout.
