# Build, test and ship

## Build options

| CMake option | Default | Purpose |
| --- | --- | --- |
| `FENGINE_BUILD_RUNTIME` | ON | SDL/bgfx runtime (`fengine`) and editor. OFF gives a headless build with no GPU or display. |
| `FENGINE_BUILD_TESTS` | ON | C++ doctests, Lua tests, sample checks and CI lint tests |
| `FENGINE_BUILD_TOOLS` | ON | `fengine_assetc` |
| `FENGINE_DEV` | ON | F1 editor, hot reload and the error overlay |
| `FENGINE_WARNINGS_AS_ERRORS` | OFF | Treat warnings in engine code as errors (CI turns this on) |
| `FENGINE_RUN_REPLAYS` | OFF | Register `samples/*/replays/*.json` as CTest tests (needs a display and GPU) |
| `FENGINE_ENABLE_SANITIZERS` | OFF | Address and undefined-behaviour sanitizers |
| `FENGINE_SHADERC` / `FENGINE_PRECOMPILED_SHADER_DIR` | empty | Host shader compiler, or verified shader binaries for cross builds |

`CPM_SOURCE_CACHE` caches downloaded dependencies between build trees.

## What CTest runs

| Test | Checks |
| --- | --- |
| `fengine_tests` | C++ services: grid, paths, reachability, line of sight, text layout and markup, world entities, movement and contacts, saves, input maps, canvas, the Lua sandbox and bindings |
| `lua_library` | Every kit, against the fixture game in `lualib/tests/game` (`fengine_headless --test`) |
| `check_<sample>` | `fengine_headless --check`: data tables against schemas, maps, placements against their types, script syntax and asset paths |
| `lua_<sample>` | Each sample's `tests/*.lua`, which drive real game flows headless: events, council votes, battles and AI-vs-AI tactics |
| `engine_boundary` | `tools/ci/check_boundary.py`: no JRPG vocabulary in `engine/` |
| `api_stubs_current` | `tools/ci/check_api.py`: `docs/api.md` and `lualib/types/fengine.d.lua` match the bindings |
| `replay_<sample>_<name>` | With `FENGINE_RUN_REPLAYS=ON`, the windowed runtime plays a recorded input script and must finish with no failed assertions or Lua errors |

When you change a binding, refresh the API files with `python3 tools/ci/check_api.py build/fengine_headless --update`.

## Replays

A replay is a JSON array of commands, each with a `tick`. Logic advances exactly 1/60 s per frame, so a replay is deterministic for a given `--seed`.

| Command | Meaning |
| --- | --- |
| `{"input": {"key:Return": 1}}` | Set raw inputs (`key:`, `pad:`, `mouse:` names); 0 releases |
| `{"lua": "code"}` | Run Lua in the game |
| `{"wait_for": "expr", "timeout": ticks}` | Pause the replay until a Lua expression is true; later ticks shift by the wait |
| `{"assert": "expr", "message": "text"}` | Fail unless the expression is true |
| `{"capture": "path.png"}` | Screenshot |
| `{"resize": [w, h]}` | Resize the window |

```sh
./build/fengine samples/ridgeline --mute --user-data /tmp/ridge-user \
  --replay samples/ridgeline/replays/battle.json
```

A failed assertion, a timed-out wait or a Lua error makes the process exit non-zero. Use a separate `--user-data` folder so saved settings don't change key bindings.

## Hot reload checks

Run a sample, then:

1. Edit a battle rule or UI module; the next call uses the new code and the game state is kept.
2. Edit a data table; it reloads after validation, and an invalid edit logs an error and keeps the old data.
3. Edit a map or image; it reloads.
4. Introduce a Lua error; the overlay shows the traceback until the file is fixed.

From the original build tree, editing `shaders/*.sc` rebuilds the shaders in the background, and the last working program stays active if compilation fails.

## Continuous integration

`.github/workflows/build.yml` runs these jobs:

- **Format**: `clang-format` 23.1.3 (`pipx run --spec clang-format==23.1.3 clang-format -i <files>` fixes it) against `.clang-format`.
- **macOS, Windows and Ubuntu** native builds with warnings as errors, all headless tests, the boundary and API checks, install and a ZIP artifact.
- **Ubuntu replays** under Xvfb with Mesa (lavapipe Vulkan, falling back to OpenGL).
- **Ubuntu headless** with address and undefined-behaviour sanitizers.

The scripts in `tools/ci/` reproduce Linux and Windows (MinGW cross build and Wine test run) in Docker:

- `linux-build.sh` runs the headless tests, then the replays under `xvfb-run`.
- `windows-wine.sh` runs the doctests, the library tests and every sample's `--check` and `--test` under Wine. It also compiles D3D shaders with shaderc; Wine's built-in HLSL compiler may need Microsoft's `d3dcompiler_47.dll`.

## Package a game

```sh
python3 tools/package_game.py --runtime build-release/fengine --shaders build-release/shaders \
  --game path/to/game --validator build-release/fengine_headless --licenses build-release/licenses \
  --output dist/my-game --zip
```

The packager:

- runs `--check` on the game first;
- verifies complete shader sets;
- copies the runtime to `bin/`, the shaders to `bin/shaders`, `lualib/` (without tests) to `bin/lualib`, and the game (without tests, replays or saves) to `game/`;
- writes a `play` or `play.cmd` launcher and a SHA-256 manifest.

It refuses to overwrite existing output. Build release packages with `-DFENGINE_DEV=OFF`. On MSVC builds, pass the Visual C++ runtime DLLs with `--runtime-dependency`. Saves go to the platform's per-user folder, never into the package.

`cmake --install build --prefix <dir>` installs the runtime, `fengine_headless`, shaders, `lualib` and the samples. `cpack -G ZIP` zips the same layout.
