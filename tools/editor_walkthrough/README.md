# Editor walkthrough

Scripted runs of the F1 editor that exercise what replays cannot: clicks,
drags, gizmos, pickers, menus and dialogs. Each `*.json` here is a list of
steps for `FENGINE_EDITOR_SCRIPT` (see `engine/editor/EditorScript.cpp` for
the step kinds); `start_town.json` is the replay that opens the sample's town map.
The coordinates assume the default dock layout in a 1280x720-point window
(the sample's `game.json` size), so run them from a fresh user-data folder.

```sh
python3 tools/editor_walkthrough/run.py build/fengine
```

runs every script in order against `samples/starfall` (selection and drags, gizmo, undo and redo, pickers, Run page, dropping an asset into the scene, New map, the override brush, dropping a house model that the walkability bake blocks at once, save and undo, Ctrl+click multi-select with group move and delete, Hierarchy context menus, Map props and Environment list edits, the Scripts panel, the sprite sheet wizard from an imported image, the Missing report, task cancel, Import geometry into a new map, the OS file dialog, moving a prop and a light with the gizmo's arrow, and the Save / Cancel prompt when the playing game warps away from a map with unsaved edits, with the editor open or closed), prints the PASS/FAIL
lines the editor logs, writes screenshots to `tools/editor_walkthrough/out/`,
and restores the sample map afterwards (`04_drop_house_bake_save_undo.json` saves
it). Exit code 1 when any check failed. Windowed: it needs a display and the
Metal/Vulkan/D3D runtime, not `fengine_headless`.

On macOS ImGui reads Cmd as Ctrl, so the scripts send `"super": true` where a user would hold Cmd (Cmd+Z, Cmd+click).

The OS file dialog is SDL's; its contents cannot be scripted, so the `import` and `drop` steps feed the editor what the dialog or a drop would deliver and everything after it (copying, the wizard, geometry import and marker import) is exercised. `10_file_dialog.json` opens the real dialog (a sheet on the window, so the editor keeps running underneath), cancels it from inside the process and checks that the callback fired; the cancel step is macOS only.
