# Editor guide

How to build a scene with the F1 editor, for someone who has never opened the engine's code. Start a game with the editor open:

```sh
./build/fengine samples/vale --tools
```

The window is a workspace: the **Scene** in the middle shows the game, **Hierarchy** on the left lists everything on the map, **Inspector** on the right edits what you selected, and **Assets** at the bottom shows the game's files. The game is paused while you edit; **Play (F5)** at the top right hides the panels and lets you play, **Edit (F5)** brings them back. F1 closes the editor altogether.

## Look around

- Right-drag turns the camera, middle-drag (or Shift and right-drag) slides it, the wheel zooms.
- **F** frames whatever is selected, **Home** the whole map, **O** switches between perspective and top-down.
- The camera is the editor's own; the game's camera is untouched. View > Game camera shows the game's framing instead.

## Make a map

File > **New map...**: give it an id (lower-case letters, digits, `_`, `-`; doors name maps by it), a name and a size in cells (one cell is one world unit, about one character). The map opens as flat ground with the player at its `start` point. Build it from models: drag floor tiles, cliff blocks and stairs from **Assets > fx/kit** and houses, trees, fences and lamps from **fx** onto the ground. Everything you place blocks or carries the player by its own shape; nothing has to be painted.

If you model the terrain in Blender, keep it to the ground: paths, slopes, cliffs, water. Model at one unit per cell with the origin where cell (0, 0) begins, apply transforms, export glTF 2.0 with normals, and pick the `.glb` with **Browse...** in New map or later with File > **Import geometry**. Buildings and other objects are better placed as props, so they can be moved. Custom properties on objects mark spawn points (`{"spawn": "entry"}`) and lights; Map > **Import Blender markers** reads them.

## Where characters can walk

The editor works it out. The **Walkability** section of the **Map** tab (beside the Inspector) shows **Auto bake**: whenever you place, move, turn, scale or remove a model, the cells around it are baked again from the terrain and the models standing there. Level ground with head room is walkable; walls, posts and fences become thin boxes bodies stop at, so the player walks right up to a wall; water and steep ground are blocked; slopes become ramps; the outer ring of cells is blocked. **Bake now** does the whole map at once; **Options...** has the limits (slope, head room, step) and a height band for a bridge deck on another layer. "Bake out of date" means a model or the terrain changed while Auto bake was off.

In the Scene the red overlay shows blocked cells, orange cells are walkable but have a box standing on them (routes avoid them), orange outlines are the boxes themselves, and violet marks cells you fixed by hand.

To fix an exception, open **Fix walkability by hand**: pick **Walkable**, **Height**, **Ramp** or a tag, tick **Paint with the left mouse button**, and paint in the Scene (**Rectangle** fills an area, **Flood fill** everything connected). What you paint is an override: it keeps its value when the walkability is baked again. **Clear override** hands a cell back to the bake. A door on the map's edge needs one: paint its cell walkable. For an invisible wall, add a collider from the **Hierarchy** and move it with the gizmo.

A map made before the bake existed (a hand-painted grid) stays as it is until you press **Bake now** or tick **Auto bake**.

## Put things in the scene

Drag a sprite (a little character picture) or a model from **Assets** onto the ground. It becomes a placement: an NPC for sprites, a prop for models. With it selected:

- drag it along the ground to move it (sprites snap to cell centres; a model snaps so its footprint lines up with the cells; hold Ctrl to place it freely),
- **W**, **E** and **R** show the move, rotate and scale gizmo; **Snap** at the top right turns the steps on and off,
- the **Inspector** has its type, position, yaw and scale, and its props: the sprite or model, its name, its script, and so on. Every field that names a file or an id is a drop-down you can search; you can also drop a file from Assets onto it. A prop's Inspector says how it blocks and how many cells it covers.
- **Ctrl+D** duplicates, **Del** deletes, Ctrl+click selects several at once.

A prop model should have its origin at the centre of its base and a footprint of whole cells (a 4 x 3 house, a 1 x 1 tree, a 2-cell fence); the bake reads its visible mesh, or, when the model has a node with the custom property `{"collision": true}` (usually hidden with `"visible": false`), that node alone: a box over a house's walls, a trunk under a tree's crown, a slope under stairs. Make collision pieces at least a quarter of a cell thick. A prop with `solid` off is decoration bodies walk through.

Points (where the player can appear), zones (areas that change the camera or start random battles), lights, particle emitters and collider boxes are added from the **Hierarchy** or the Map menu and edited the same way. They are drawn as handles in the Scene; View toggles each kind.

## Give an NPC something to do

An NPC's **pages** say what happens when the player talks to it (**action**), walks into it (**touch**), once when the page becomes active (**auto**) or every tick (**parallel**). Each page names a script module in the `events` folder; the Inspector offers the ones that exist. Pages further down the list win when their conditions (`when`) hold, so a second page with `{"flag": "quest_done"}` replaces the first after the quest. The Inspector shows which page is active right now and a **Run** button plays it; the **Scripts** tab lists every scripted thing on the map, what is running, and the last events (touches, zone enters) the game saw. Badges over the NPCs in the Scene say how each one fires.

Doors are placements of type `door`: pick the target map and one of its points.

## Bring in art and sound

**Assets > Import...** (or drop files onto the Assets panel) copies files into the game folder: images and `.anim.json` files into `sprites/`, models into `fx/`, sounds into `audio/`. The game reloads them as they land. A loose image in `sprites/` opens the **Sprite sheet** window: give the frame size, the number of directions (1, 4 or 8 rows per clip) and the clips (idle, walk, attack...), check the yellow grid over the image, and **Create**. The result is a sprite you can drop into the scene.

Sounds get an id in `data/audio.json` (the Data tab edits it); pickers offer both ids and files. The **missing** button in Assets lists anything a map or table names that has no file.

## Save and play

**Save map** (Ctrl+S) writes `maps/<id>/map.json`. Everything else you changed in the running game that is not part of the map (an entity you nudged in the Live entity section) is gone on the next load; that is on purpose. **Play (F5)** to try it, and **Play from here** on a point in the Hierarchy starts the game at that point. If the game is about to change maps while you have unsaved edits (you walked through a door or into a warp zone while playing), the editor pauses and asks: **Save, then continue**, **Discard edits and continue**, or **Cancel**, which keeps you on the map with your edits (press Play again to carry on; that warp is dropped).

Keep the game folder in version control: the editor overwrites files, it does not keep history.
