# Debugging Lua: tools available now and debugger evaluation

## Available now

- **Error overlay** (dev builds): every Lua error shows its message and traceback with `file:line`. Fix the file and it hot-reloads.
- **Console** (F1, Console tab): run Lua in the live game, for example `require("fe.state").flag("met")` or `require("fe.battle").active.battlers[1]:resource("hp")`. Up and Down recall earlier commands.
- **`fe.log`** for logging. **`fe.dev`** provides `traceback(thread)` (for a task's coroutine), `errors()`, and `zone`/`clock` timings shown in the Profiler tab. `fe.dev.console(code)` (returns the printable results and whether the code ran without error), `fe.input.feed` and `fe.input.feed_text` exist only in development builds, in `--test`/`--check` runs and in `--replay`/`--record` sessions (including those of a shipped `fengine`); a shipped game played normally has none of them (see the sandbox in architecture.md).
- **Headless tests and replays**: `fengine_headless --test` with `--filter`, and replays with `assert` and `wait_for` (`fengine_headless <game> --replay <file>`). These reproduce most bugs deterministically, without a window. A replay `lua` step that raises an error fails the replay. Each headless run gets its own temporary user-data folder, deleted when it ends. `fengine <game> --record bug.json` turns a session where a bug happened into a replay.

## Debugger evaluation (VS Code breakpoints)

The plan asked for an evaluation of an EmmyLua- or DAP-compatible debugger for dev builds. These constraints in f-engine decide the choice:

1. **The sandbox.** `LuaHost` opens only base, string, table, math, coroutine and utf8. `debug`, `io`, `os` and `package` are absent, so a debugger cannot be `require`d from game code. It has to be installed by C++ in dev builds.
2. **One hook slot.** Lua allows one hook per thread. The dev watchdog (`LuaHost.cpp`, `lua_sethook(..., LUA_MASKCOUNT, 200000)`) already uses it, and every debugger relies on line and call hooks. While a debugger is attached, the watchdog must be disabled or chained into the debugger's hook. Otherwise each one silently replaces the other, and a breakpoint pause trips the watchdog.
3. **Everything runs in coroutines.** Events, cutscenes, battles and tests are `fe.task` coroutines. Lua 5.4 copies the creating thread's hook into new coroutines (`lua_newthread`), so a hook installed before tasks start reaches them. Stepping across `coroutine.yield` must still be handled by the debugger.
4. **The main loop must keep running while paused.** The window, ImGui and the OS event pump must not freeze, or macOS flags the app as hung. A debugger that blocks inside the hook needs the runtime to keep pumping events (or to show a "paused in debugger" state).

| Option | How it works | Fit |
| --- | --- | --- |
| **EmmyLua debugger** (`emmy_core`, used by the EmmyLua VS Code and JetBrains extensions) | Native library for Lua 5.1–5.4; connects to the IDE over TCP and sets its own hooks | **Best fit.** C++ can register it in dev builds without loosening the sandbox. TCP attach works with our own executable. It supports breakpoints, stepping, locals and evaluation. Cost: one more native dependency (dev builds only) and the hook coordination in (2). |
| **local-lua-debugger-vscode** | Pure Lua; the extension launches the debuggee and talks over stdio | Poor fit. It needs the `debug` and `io` libraries in script scope and owns the process's stdio, which conflicts with the sandbox and the engine log. |
| **Own DAP server in C++** | A small Debug Adapter Protocol server over a socket, built on `lua_sethook` and `lua_getinfo`/`lua_getlocal` | Full control, including the watchdog, coroutine stepping and pumping events while paused. It is the most work (roughly 1–2k lines plus protocol tests) and duplicates a mature tool. |

### Recommendation

Integrate the EmmyLua debugger as an opt-in, dev-only feature:

1. Add a CMake option `FENGINE_LUA_DEBUGGER` (default OFF, ignored when `FENGINE_DEV=OFF`) that fetches EmmyLuaDebugger with CPM and links `emmy_core` into `fengine_core` (the static library that holds `LuaHost`, `CMakeLists.txt`), in development builds only; the windowed runtime reaches it through `fengine_runtime` (the static library of `cmake/Runtime.cmake`, which `fengine` links) and the headless tool through `fengine_headless`.
2. Add a runtime flag `--debugger <port>`. `LuaHost` turns the watchdog off, opens `emmy_core` from C++ and starts a TCP listener before `main.lua` runs. Games and release builds never see it.
3. While the debugger holds the game paused, `MainLoop` keeps pumping SDL events and shows a "paused in debugger" overlay instead of advancing logic.
4. Add a VS Code `launch.json` example to the docs.

This keeps the sandbox and the boundary rule intact, and it costs nothing in release builds. If the EmmyLua dependency proves hard to build on one of the three CI platforms, the fallback is the C++ DAP server, which shares steps 2–3. This document records the evaluation and the decision. The integration itself has not been built yet.

## Editor walkthroughs

Replays feed the game's input, not the editor's. `FENGINE_EDITOR_SCRIPT=<file.json>` plays a list of steps through ImGui's input queue instead (mouse moves in window points, clicks and drags, keys with modifiers, typed text, mouse wheel, files as the OS dialog or a drop would deliver them, screenshots, and checks: a Lua expression, the selection's label, the status line, Play or Edit mode); the step kinds are documented in `engine/editor/EditorScript.cpp` and results are logged as `editor script: PASS|FAIL ...`. `tools/editor_walkthrough/run.py` runs the scripts in that folder against vale's town (select, drag, gizmo, pickers, Run page, drag an asset into the scene, New map, brush painting, the walkability bake, save and undo, multi-select, the sprite wizard, imports and the OS file dialog) and restores the sample afterwards. `FENGINE_EDITOR_ASSET_FOLDER=<folder>` picks the folder the Assets panel opens on, for screenshots.

