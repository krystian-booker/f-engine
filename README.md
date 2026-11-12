# f-engine

A C++20 engine for HD-2D role-playing games: pixel-art sprites, and rigged 3D creatures where a scene wants them, standing in lit 3D dioramas. Every game rule is the game's own Lua.

The C++ engine provides **services only**: windowing, input action maps with keyboard and gamepad hot-swap, a 3D world of entities (sprites, static and skinned meshes, lights, particles) with HD-2D post-processing (mip-chain bloom, depth-aware depth of field, ambient occlusion, distortion, FXAA, game post shaders), several views per frame (screen rects and render targets), freeze-frame transitions, grid navigation, 2D canvas drawing, text layout, audio, saves, hot reload, replays and an in-game editor. It contains no JRPG concepts and **ships no Lua**: HP, parties, items, battles, menus, shops, quests and encounters are written by each game against the bindings in [docs/api.md](docs/api.md). `tools/ci/check_boundary.py` enforces the first half in CI, and the packaging job checks that the shipped `builtin.pak` holds no Lua.

Two samples show games built that way. `samples/starfall` is a whole small RPG: a town, a walkable world map with a mount and an airship, a dungeon, and round-based battles with a break gauge and unison attacks, in about three thousand lines of Lua. `samples/aetherbound` is a ten-minute vertical slice staged like a commercial game's opening: a burning fortress, an active-time boss battle against a rigged war machine, and two scenes cut to music (see [The samples](#the-samples)).

## Build and run

You need CMake 3.25+, Ninja and a C++20 compiler (Xcode command-line tools on macOS). CPM fetches pinned dependencies on the first configure.

```sh
cmake -S . -B build -G Ninja
cmake --build build --parallel 4
ctest --test-dir build --output-on-failure
./build/fengine samples/starfall      # or samples/aetherbound
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
| `fengine_headless --check <game>` | Validate maps, sprites, referenced assets and the game's own `validate` |
| `fengine_headless <game> --replay <file>` | Run a replay without a window or GPU (screenshots skipped) |
| `fengine_headless --bake <game>` | Derive each map's walkability from its terrain and placed models |
| `fengine_headless --write-api <dir>` | Write the Lua API stubs (`fengine.d.lua`) and `api.md` |
| `fengine_assetc atlas` / `sheet` / `cook` | Pack sprite atlases, convert uniform sheets to animations, and compress world and mesh textures to BC7 `.ktx` |
| `fengine_assetc pack <folder> <out.pak>` / `list <archive.pak>` | Archive a game folder, list an archive |

## Make a game

A game is a folder holding `game.json`, `main.lua` and whatever it needs: `maps/<id>/`, `sprites/`, `audio/`, data files, Lua modules. `require` finds the game's own modules and nothing else. The smallest `main.lua` that shows a map and walks a sprite around it:

```lua
local world, hero

return {
  init = function()
    fe.input.define_axis("x", { negative = { "key:Left" }, positive = { "key:Right" }, analog = { "pad:leftx" } })
    fe.input.define_axis("z", { negative = { "key:Up" }, positive = { "key:Down" }, analog = { "pad:lefty" } })
    world = fe.world.load("field", "town")                      -- maps/town/map.json
    hero = world:spawn { position = world:points().entry.position,
      sprite = { animation = "sprites/hero.anim.json", move_clip = "walk", idle_clip = "idle" },
      mover = { speed = 3 }, collider = { radius = 0.25 } }
  end,
  update = function()
    hero:drive({ x = fe.input.axis("x") * 3, z = fe.input.axis("z") * 3 })
    world:camera().target = hero.position
  end,
  draw = function()
    world:draw()
    fe.canvas.text("Playing with: " .. fe.input.device_type(), 8, 8)
  end,
}
```

Maps are made in the F1 editor (terrain from a glTF file, placed models, walkable cells baked from the geometry and painted over, zones, lights, environment) and saved as `maps/<id>/map.json`. What a placement *is* (an NPC, a chest, a door) is up to the game: the editor shows forms for the types the game describes through the optional `editor` hooks of its entry module, and without any hooks still opens, previews, edits and saves maps.

- [Architecture](docs/architecture.md): the engine and game layers and how a frame runs.
- [Scripting](docs/scripting.md): writing a game against the bindings: the entry module, worlds, input, tasks and tests.
- [Authoring](docs/authoring.md): the game folder, maps, Blender, sprites, skinned models and the editor.
- [Editor guide](docs/editor.md): building a scene with the F1 editor, for artists and designers.
- [Lua API reference](docs/api.md): every engine binding (generated; `builtin/types/fengine.d.lua` is the LuaLS stub).
- [Build, test and ship](docs/verification.md): tests, replays, CI and packaging.
- [Debugging](docs/debugging.md): the error overlay, console and tests.

## The samples

`samples/starfall` is a small original RPG in the style of the HD-2D Final Fantasy games. Nothing in it is special to the engine; it is what a game's own code looks like.

| Part | Files | What it shows |
| --- | --- | --- |
| Foundations | `game/task.lua`, `scenes.lua`, `data.lua`, `state.lua`, `audio.lua` | Coroutines for anything that takes time, a scene stack, JSON data tables, the save file |
| Input and UI | `game/input.lua`, `ui.lua`, `dialogue.lua` | Actions, button prompts that follow the device in use (press a key, then a gamepad button, or plug a controller in), windows, lists, message boxes |
| Exploration | `game/field.lua`, `events/*.lua` | Placement types, talking, chests, doors and warps, a walkable world map that wraps, a strider that fords rivers, a skiff that flies and lands on open ground, step-based encounters with a danger sensor |
| Battles | `game/battle/rules.lua`, `game/battle/scene.lua` | Rounds on a speed timeline, a break gauge, bonus actions, sweeps and unison attacks, surges, equippable echoes with mastery ranks, cover, elemental affinities (weak, resist, null, absorb) hidden until seen, ailments, a summon that stays three turns, preemptive strikes and ambushes; the boss is a rigged, animated 3D creature |
| Screens | `game/menu.lua`, `shop.lua`, `title.lua` | Items, echoes, status, save and load, a shop |
| Tools | `game/editor.lua`, `tests/*.lua`, `replays/*.json` | The editor hooks, 25 headless tests that play the game through its input, three replays with reference screenshots |

Its art and audio are generated placeholders. `python3 tools/generate_samples.py starfall --headless build/fengine_headless` regenerates them and bakes the maps' walkability (`--fresh` also rewrites the placements; `tools/generate_creature.py` writes the rigged creature).

`samples/aetherbound` ("Aetherbound: Prologue") is a vertical slice: five scenes, ten minutes, one map walked by the player and one boss. It shares no code with Starfall and makes other choices, which is the point of an engine without kits: a 1920 × 1080 canvas with TrueType fonts (`canvas_scale: "smooth"`), an active-time battle system fought in place on the map instead of on a stage, a party that follows the leader's trail, cutscenes written as straight-line scripts with a director for the cast and a camera, two scenes timed to their music, and a boss fight whose last phase is scripted through hooks of the battle scene. Its art, animation, music and sound are generated by the scripts in `tools/aetherbound/` (Blender through Python, numpy); `samples/aetherbound/README.md` describes the game and `tools/aetherbound/README.md` the pipeline.

3D models go through `tools/modelkit/` before they ship: a guide for whoever generates them (AI agents included), Blender helpers, and a validator that checks mesh integrity, normals, topology, UVs and game readiness and requires a recorded visual review. `python3 tools/modelkit/validate.py --manifest tools/aetherbound/models.json --engine build/fengine_headless` is the gate for the sample's models, and a CTest.

## Ship

```sh
cmake -S . -B build-release -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build-release
python3 tools/package_game.py --runtime build-release/fengine --assetc build-release/tools/assetc/fengine_assetc \
  --shaders build-release/shaders --game path/to/game --validator build-release/fengine_headless \
  --licenses build-release/licenses --output dist/my-game --zip --self-check
```

The game ships as `game.pak` and the engine's own assets (the default font, input glyphs, weather presets) as `builtin.pak`. Release builds have `FENGINE_DEV` off: no editor, and shaders and `builtin` are found only next to the executable. Signing hooks: `--sign-identity` and `--notarize-profile` (macOS), `--signtool` (Windows); `--appimage` builds a Linux AppImage. See [verification.md](docs/verification.md#package-a-game). On macOS the package is an `.app` bundle (add `-DFENGINE_UNIVERSAL=ON` for arm64 and x86_64); on Windows `fengine.exe` is a GUI program with no console window.
