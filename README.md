# f-engine

A C++20 engine for HD-2D role-playing games: pixel-art sprites standing in lit 3D dioramas, with every game rule written in Lua.

The C++ engine provides **services only**: windowing, input action maps, a 3D world of entities (sprites, meshes, lights, particles), grid navigation and tactical queries, 2D canvas drawing, text layout, audio, saves, hot reload, replays and an in-game editor. It contains no JRPG concepts. HP, parties, items, battles, menus, shops, quests and encounters are Lua. `tools/ci/check_boundary.py` enforces this in CI.

The engine ships a Lua library of replaceable kits in `lualib/fe/`. It covers the field, events, cutscenes, dialogue, UI widgets and default screens, an RPG data model, an effect pipeline, and battle cores with ATB, round-order and initiative schedulers, side-view and grid-tactics presentations. A game can use the kits, extend them through hooks, override any module by adding a file with the same path, or ignore them.

Three original samples show the range:

| Sample | Shows |
| --- | --- |
| `samples/vale` | Classic town and dungeon, quests, shops, inn, side-view **ATB** battles with scripted enemy AI |
| `samples/wayfarers` | Round-order battles with turn preview, **weakness shields / break** and a **boost** resource, custom stats, NPC verbs, two protagonists with chapters, a world map, day/night |
| `samples/ridgeline` | A **council vote** with persuasion and branching flags, then a **grid tactics** battle: deployment, height, facing, terrain reactions (burning grass, freezing or electrified water), knockback and objectives |

None of the sample mechanics touch `engine/`.

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
  libxinerama-dev libgl1-mesa-dev libasound2-dev libpulse-dev libudev-dev libvulkan-dev
```

Renderers are Metal on macOS, D3D11 on Windows and Vulkan on Linux, with OpenGL as a fallback.

| Program | Use |
| --- | --- |
| `fengine <game> [--mute] [--replay file] [--seed n] [--frames n] [--user-data dir]` | Play or develop a game. F1 opens the editor (dev builds). |
| `fengine_headless --test <game> [--filter text]` | Run a game's Lua tests (`tests/*.lua`) without a window |
| `fengine_headless --check <game>` | Validate data tables, maps, scripts and assets |
| `fengine_headless --write-api <dir>` | Write the Lua API stubs (`fengine.d.lua`) and `api.md` |
| `fengine_assetc atlas` / `sheet` | Pack sprite atlases and convert uniform sheets to animations |

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
- [Scripting](docs/scripting.md): tasks, scenes, the field, events, battles, effects and tactics in Lua.
- [Authoring](docs/authoring.md): the game folder, maps, Blender, sprites, data tables and the editor.
- [Lua API reference](docs/api.md): every engine binding (generated; `lualib/types/fengine.d.lua` is the LuaLS stub).
- [Build, test and ship](docs/verification.md): tests, replays, CI and packaging.
- [Debugging](docs/debugging.md): the error overlay, console and tests, and the VS Code debugger evaluation.

Sample art and audio are generated placeholders. `python3 tools/generate_samples.py <vale|wayfarers|ridgeline|fixture>` regenerates them.

## Ship

```sh
cmake -S . -B build-release -G Ninja -DCMAKE_BUILD_TYPE=Release -DFENGINE_DEV=OFF
cmake --build build-release
python3 tools/package_game.py --runtime build-release/fengine --shaders build-release/shaders \
  --game path/to/game --validator build-release/fengine_headless --licenses build-release/licenses \
  --output dist/my-game --zip
```
