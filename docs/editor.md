# Editor guide

How to build a scene with the editor, for someone who has never opened the engine's code. The editor is its own application, `fengine_editor`, laid out like Unity's: open it on a game folder.

```sh
./build/fengine_editor samples/starfall
```

Started without a folder it asks: reopen the last project, open another, or make a new one (a folder with a `game.json`, a `main.lua` and a first scene).

The window is a workspace of panels that dock, tab and float:

- **Scene** (middle) shows the scene being edited, through the editor's own camera. **Game** beside it shows the game while it plays, and **Code** is the script editor.
- **Hierarchy** (left) lists everything in the scene. **Inspector** (right) edits what you selected; with nothing selected it shows the scene itself (name, environment, props).
- **Project** (bottom) is the game's folder. **Console**, **Scripts**, **Data** and **Build** share its space as tabs.

The editor edits a scene without running the game. **Play** (Ctrl+P) starts the game in the Game view, from the scene you have open; **Stop** ends it, and nothing the game did while playing reaches the scene.

The guide uses the sample game, whose code tells the editor what its placement types are and how to start the game on a scene. The editor does not need that: in a new project every step about scenes, models, walkability, lights and saving works the same. Without the game's description, props are edited as plain fields instead of the game's own forms, and Play starts the game from its beginning.

## Look around

- Right-drag (or Alt and left-drag) turns the camera, middle-drag slides it, the wheel zooms.
- **F** frames whatever is selected, **Home** the whole scene.
- The Scene view draws without the scene's fog, which is tuned for the game's camera; View > **Scene Fog** shows it.

## Make a scene

File > **New Scene...**: give it an id (lower-case letters, digits, `_`, `-`; doors name scenes by it), a name and a size in cells (one cell is one world unit, about one character). It opens as flat ground with a `start` point. Build it from models: drag floor tiles, cliff blocks and stairs from **Project > fx/kit** and houses, trees, fences and lamps from **fx** onto the ground in the Scene view (or right-click a file > **Place in Scene**, which puts it where the camera looks). Everything you place blocks or carries the player by its own shape; nothing has to be painted.

If you model the terrain in Blender, keep it to the ground: paths, slopes, cliffs, water. Model at one unit per cell with the origin where cell (0, 0) begins, apply transforms, export glTF 2.0 with normals, and pick the `.glb` with File > **Import Scene Geometry...**. Buildings and other objects are better placed as props, so they can be moved. Custom properties on objects mark spawn points (`{"spawn": "entry"}`) and lights; File > **Import Markers from Geometry** reads them.

A scene is a folder, `maps/<id>/`, holding `map.json` and its geometry. File has Save (Ctrl+S), Save Scene As, Rename and Delete.

## Where characters can walk

The editor works it out. Whenever you place, move, turn, scale or remove a model, the walkable cells are baked again from the terrain and the models standing there, inside the same undo step. Level ground with head room is walkable; walls, posts and fences become thin boxes bodies stop at, so the player walks right up to a wall; water and steep ground are blocked; slopes become ramps. GameObject > **Bake Scene Walkability** does it on demand.

View > **Show Walkability** marks the cells nobody can walk on with a red cross and the cells fixed by hand with a blue outline; orange outlines are the boxes the bake made (View > Show Baked Collision).

When the bake gets a cell wrong, paint it: the **Paint Walkability** tool (B) with, in the toolbar, what to paint (Walkable, Blocked, Set height, a ramp in one of the four directions, a cell tag such as `water` to add or remove, or Clear hand-painted to give a cell back to the bake), the shape (a brush with a radius, a rectangle dragged from corner to corner, or a flood fill of every connected cell like the one clicked) and the grid layer. A stroke is one undo step, and hand-painted cells survive every later bake.

With nothing selected, the Inspector's **Walkability bake** group holds Auto (bake again whenever a model moves) and the bake's limits (slope, head room, step height, coverage); changing a limit bakes again with it. **Layer links** beside it lists the steps and ladders that join the grid's layers.

## Put things in the scene

- **Select** by clicking in the Scene or the Hierarchy; Ctrl+click adds to the selection. Escape deselects.
- **Move** by dragging a selected placement or point across the ground (it snaps to cells; hold Shift for free placement), or with the gizmos: **W** move, **E** rotate, **R** scale, **Q** none. Snap to Grid in the toolbar sets half-unit, 15-degree and quarter steps; hold Ctrl to move freely.
- **Create** from GameObject > Create (or right-click in the Hierarchy): a placement of one of the game's types, a zone, a named point, a light, a particle emitter, a collider. New objects appear where the camera looks.
- **Duplicate** (Ctrl+D) and **Delete** (Del) act on the whole selection. **Undo** (Ctrl+Z) and **Redo** cover every edit to the scene.

What a placement *is* belongs to the game: its **type** says so, and the Inspector shows the form the game describes for that type under **Props** (pickers for sprites, models, sounds, scripts, scenes and table records). Every list and group has an "edit as JSON" button for whatever the form does not cover. An edit that would make the scene unloadable is refused with the reason shown in red.

## Give an NPC something to do

In the sample, an `npc` placement has a `script` prop: a Lua module under `events/`. Pick one in the Inspector, or make a new one: Project > right-click > Create > **Lua Script**, which opens it in the **Code** panel. The code editor completes the engine's API as you type (`fe.` and so on, Ctrl+Space to ask), shows a binding's signature and documentation when the mouse rests on it, and has find and replace (Ctrl+F), go to line (Ctrl+G) and Ctrl+S to save.

**Scripts > Scene Wiring** lists every placement and zone that is wired to a script or to another scene; a red entry names a module that does not exist. Double-click a row to open the script.

## Play and watch it run

**Play** (Ctrl+P) runs the game in the Game view; click the view to give it the keyboard, mouse and gamepad. The game loads scenes from disk, so an unsaved scene asks first: save and play, or play the saved version. **Pause** (Ctrl+Shift+P) holds the game and **Step One Tick** (Ctrl+Alt+P) advances it by one logic tick.

While the game plays:

- The **Hierarchy** shows the live entities of the world on screen (click another world's name to see it): the ones that came from the scene and the ones scripts spawned.
- The **Inspector** shows every component of the selected entity, live, and edits them; edits last until Stop.
- **Scripts** shows the modules the game has loaded, its script errors (double-click to open the file at the line), the tasks it is running (with Cancel) and its recent events, as far as the game's hooks report them, and a **Lua Console** that evaluates code in the running game.
- The **Scene** view shows the game's world, live, through the editor's camera, so you can fly around a running game (View > **Scene Follows Game** switches back to the scene being edited).
- The status bar shows the tick, frame time, draw calls and entity count.
- Saving a script, a data file or art **hot reloads** it in the running game.

## Bring in art and sound

Drop files onto the Project panel, or copy them into the game folder with any file manager: the editor notices. The Project panel renames (F2), moves (drag) and deletes (to the trash) files and folders, and the strip under it says what kind of asset a file is and which scenes and data files use it, so you know before you delete. The Project panel shows images and sprite sheets as pictures and renders a picture of each model.

A sprite is an image with an `.anim.json` beside it that says how it is cut into frames. Right-click a `.png` > **Make Sprite Sheet...**: give the frame size, how many facing directions each clip has (rows), and the clips with the columns they use (`0-3`, or `0,1,2,1`), and the editor writes the `.anim.json` (`fengine_assetc sheet` does the same from the command line; see [Authoring](authoring.md#sprites)).

## Data, settings and builds

- **Data** edits the game's data tables (the ones its description lists) a record at a time, with the game's own check run before a table is saved.
- File > **Project Settings...** edits `game.json`.
- **Build** runs the project-wide jobs and shows their output: **Validate** (every scene, sprite and reference, and the game's own `validate`), **Run Tests** (the game's Lua tests), **Bake Walkability** (every scene, as saved), **Record Replay** and **Run Replay**, and **Package**, which builds a shippable folder from a Release build of the engine.
