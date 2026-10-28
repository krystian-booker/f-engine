# f-engine

A C++20 JRPG runtime and in-game editor. Games are folders of JSON, Lua, pixel sprites and static glTF maps. The included **Crystal of the Vale** game demonstrates a town, dungeon, shops, inns, a quest, random encounters and a side-view ATB boss battle.

The renderer combines upright pixel sprites with lit 3D scenery, sprite shadows, point lights, fog, water and particles, bloom, depth-aware tilt-shift blur and 3D color grading. The gameplay framework includes event pages, coroutine cutscenes, party growth, equipment, spells, menus and versioned saves.

## Build and play

Install CMake 3.25+, Ninja and a C++20 compiler. On macOS, install the Xcode command-line tools. Dependencies are pinned and fetched by CPM during configuration; the first build needs network access and builds bgfx's shader compiler.

```sh
cmake -S . -B build -G Ninja
cmake --build build --parallel 4
ctest --test-dir build --output-on-failure
./build/apps/runtime/fengine samples/sandbox
```

Windows with Visual Studio 2022's C++ desktop workload:

```powershell
cmake -S . -B build -A x64
cmake --build build --config Release --parallel 4
ctest --test-dir build -C Release --output-on-failure
.\build\apps\runtime\Release\fengine.exe samples/sandbox
```

Ubuntu/Debian prerequisites:

```sh
sudo apt-get install build-essential cmake ninja-build libx11-dev libxrandr-dev libxi-dev libxcursor-dev libxinerama-dev libgl1-mesa-dev libasound2-dev libpulse-dev libudev-dev libvulkan-dev
```

The defaults are Metal on macOS, D3D11 on Windows and Vulkan on Linux. Linux also builds GL shaders. The Linux window integration currently uses X11; Wayland desktops need XWayland.

| Action | Keyboard | Gamepad |
| --- | --- | --- |
| Move / select | Arrows or WASD | Left stick / D-pad |
| Confirm | Enter, Z, Space | South |
| Cancel | Escape, X | East |
| Main menu | Tab | North |
| Previous / next actor | Q / E | Shoulders |
| Developer tools | F1 | — |

The Config screen saves key bindings, audio buses and wait/active ATB mode. Saves and settings use SDL's per-user application directory. `--user-data path` gives a separate profile for testing.

## Make a game

Copy `samples/sandbox` to a new directory, change `game.json`, then run `fengine path/to/game`. No C++ project is needed. The sample artwork and music are generated original placeholders; regenerate them with `python3 tools/generate_sandbox.py`.

- [Game and map authoring](docs/authoring.md): folder conventions, Blender, sprites, database, UI, editor.
- [Lua API](docs/scripting.md): event commands, rules, AI and custom effects.
- [Architecture and extension points](docs/architecture.md).
- [Build, packaging and verification](docs/verification.md).
- [M0–M7 coverage and verification scope](docs/milestones.md).

Use `fengine_assetc validate path/to/game` before shipping. F1 exposes nav painting, entities and event pages, Blender marker import, lights, camera zones, post effects, flags/variables, Lua console, database inspection and a battle sandbox. Save map changes explicitly in the editor.

## Ship

Build with `-DCMAKE_BUILD_TYPE=Release -DFENGINE_DEV=OFF` (or `--config Release` for Visual Studio), then install or create a ZIP:

```sh
cmake --install build --prefix build/package --config Release
cpack --config build/CPackConfig.cmake -C Release -G ZIP
python3 tools/package_game.py --runtime build/apps/runtime/fengine \
  --shaders build/shaders --game samples/sandbox --licenses build/licenses \
  --validator build/tools/assetc/fengine_assetc --output build/my-game --zip
```

Packages target the OS and architecture used to build the runtime. macOS signing/notarization, Windows signing and store submission remain distribution steps for the game developer. Collected dependency licenses are included by CMake installation and by the packager when `--licenses` is supplied.
