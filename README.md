# f-engine

A C++20 engine for HD-2D role-playing games: pixel-art sprites standing in lit 3D dioramas, with every game rule written in Lua.

The C++ engine provides **services only**: windowing, input action maps, a 3D world of entities (sprites, meshes, lights, particles) with HD-2D post-processing (mip-chain bloom, depth-aware depth of field, ambient occlusion, distortion, FXAA, game post shaders), several views per frame (screen rects and render targets), freeze-frame transitions, grid navigation, 2D canvas drawing, text layout, audio, saves, hot reload, replays and an in-game editor. It contains no JRPG concepts. HP, parties, items, battles, menus, shops, quests and encounters are Lua. `tools/ci/check_boundary.py` enforces this in CI.

The engine ships a Lua library of replaceable kits in `lualib/fe/`. It covers the field, events, cutscenes (fast-forward and skip), dialogue (voice-over, lip-flap, auto-advance), UI widgets and default screens with toasts, per-controller button glyphs and accessibility settings, NPC followers, multi-protagonist chapters, an RPG data model, an effect pipeline, and a battle core with ATB and round-order schedulers and a side-view presentation. A game can use the kits, extend them through hooks, override any module by adding a file with the same path, or ignore them.

One original sample, `samples/vale`, shows the range: a classic town and dungeon, quests (with update toasts), shops, an inn, side-view **ATB** battles with scripted enemy AI, and a rendering showcase (`fx/showcase.lua`, run by `replays/render.json`) with normal-mapped crates and sprites, game shaders, shadow-casting lights and reflective water. None of its mechanics touch `engine/`.

## Build and run

You need CMake 3.25+, Ninja and a C++20 compiler (Xcode command-line tools on macOS). CPM fetches pinned dependencies on the first configure.

```sh
cmake -S . -B build -G Ninja
cmake --build build --parallel 4
ctest --test-dir build --output-on-failure
./build/fengine samples/vale
```

On Windows (Visual Studio 2022), configure with `-A x64` and build with `--config Release`. Ubuntu needs:

```sh
sudo apt-get install build-essential cmake ninja-build libx11-dev libxrandr-dev libxi-dev libxcursor-dev \
  libxinerama-dev libgl1-mesa-dev libegl-dev libwayland-dev libxkbcommon-dev wayland-protocols \
  libasound2-dev libpulse-dev libudev-dev libvulkan-dev
```

Renderers are Metal on macOS, D3D11 on Windows and Vulkan on Linux (Wayland or X11), with OpenGL as a fallback.

Debug builds include the F1 editor, hot reload and the error overlay; Release builds leave them out. `-DFENGINE_DEV=ON` or `OFF` overrides that (see [verification.md](docs/verification.md)).

| Program | Use |
| --- | --- |
| `fengine <game> [--mute] [--replay file] [--record file] [--seed n] [--frames n] [--max-fps n] [--user-data dir]` | Play or develop a game. F1 opens the editor (dev builds; `--tools` starts with it open). `--record` saves the session's input as a replay. Saves, settings and `log.txt` go to the per-user folder named by `game.json`'s `id`. |
| `fengine_headless --test <game> [--filter text]` | Run a game's Lua tests (`tests/*.lua`) without a window |
| `fengine_headless --check <game>` | Validate data tables, maps, scripts and assets |
| `fengine_headless <game> --replay <file>` | Run a replay without a window or GPU (screenshots skipped) |
| `fengine_headless --write-api <dir>` | Write the Lua API stubs (`fengine.d.lua`) and `api.md` |
| `fengine_assetc atlas` / `sheet` / `cook` | Pack sprite atlases, convert uniform sheets to animations, and compress world and mesh textures to BC7 `.ktx` |
| `fengine_assetc pack <folder> <out.pak>` / `list <archive.pak>` | Archive a game or `lualib` folder, list an archive |

The default controls come from `fe.input` and can be rebound on the Config screen. Move with the arrows or WASD, confirm with Enter/Z, cancel with Escape/X, and open the menu with Tab.

## Make a game

A game is a folder holding `game.json`, `main.lua`, `data/*.json`, `maps/<id>/`, `sprites/`, `audio/` and any Lua modules you like. The smallest RPG-style `main.lua`:

```lua
return require("fe.starter").main({
  title = "My Game",
  new_game = function()
    require("fe.rpg.party").join("hero")
    require("fe.field").start("town", "entry")
  end,
})
```

- [Architecture](docs/architecture.md): the engine/library/game layers and how a frame runs.
- [Scripting](docs/scripting.md): tasks, scenes, the field, events, battles and effects in Lua.
- [Authoring](docs/authoring.md): the game folder, maps, Blender, sprites, data tables and the editor.
- [Editor guide](docs/editor.md): building a scene with the F1 editor, for artists and designers.
- [Lua API reference](docs/api.md): every engine binding (generated; `lualib/types/fengine.d.lua` is the LuaLS stub).
- [Build, test and ship](docs/verification.md): tests, replays, CI and packaging.
- [Debugging](docs/debugging.md): the error overlay, console and tests, and the VS Code debugger evaluation.

Sample art and audio are generated placeholders. `python3 tools/generate_samples.py <vale|fixture> --headless build/fengine_headless` regenerates them and bakes the maps' walkability.

## Ship

```sh
cmake -S . -B build-release -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build-release
python3 tools/package_game.py --runtime build-release/fengine --assetc build-release/tools/assetc/fengine_assetc \
  --shaders build-release/shaders --game path/to/game --validator build-release/fengine_headless \
  --licenses build-release/licenses --output dist/my-game --zip --self-check
```

The game and `lualib` ship as `game.pak` and `lualib.pak` archives. Release builds have `FENGINE_DEV` off: no editor, and shaders and `lualib` are found only next to the executable (every build tree carries `lualib`, so this also holds for builds outside the source tree). Signing hooks: `--sign-identity` and `--notarize-profile` (macOS), `--signtool` (Windows); `--appimage` builds a Linux AppImage. See [verification.md](docs/verification.md#package-a-game). On macOS the package is an `.app` bundle (add `-DFENGINE_UNIVERSAL=ON` for arm64 and x86_64); on Windows `fengine.exe` is a GUI program with no console window.
