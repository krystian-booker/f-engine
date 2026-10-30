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

File > **New map...**: give it an id (lower-case letters, digits, `_`, `-`; doors name maps by it), a name and a size in cells (one cell is one world unit, about one character). If you have a Blender export, **Browse...** picks the `.glb` as the map's geometry; you can add it later with File > **Import geometry**. The map opens with the player at its `start` point.

In Blender, model at one unit per cell with the origin where cell (0, 0) begins, apply transforms, export glTF 2.0 with normals. Custom properties on objects mark spawn points (`{"spawn": "entry"}`), collision (`{"collision": true}` for whole cells, `{"collision": {"box": true}}` for an exact box) and lights; Map > **Import Blender markers** reads them.

## Mark where characters can walk

Open the **Map** tab (beside the Inspector). The red overlay shows blocked cells on the current layer.

1. **Bake walkability...** reads the geometry and makes a first pass: level ground becomes walkable, houses and water blocked, slopes become ramps, and things smaller than a cell get collider boxes. Press **Preview**: green is walkable, red blocked, yellow is where the bake was unsure (look at those), orange is ground nothing leads to, blue is water. **Apply** keeps it; Undo (Ctrl+Z) takes it back.
2. Fix the rest with the brush: pick **Walkable**, tick **Paint with the left mouse button**, and paint in the Scene. **Rectangle** fills an area, **Flood fill** everything connected.
3. For a fence post or a sign that only takes up part of a cell, use the **Block (quarter cells)** brush: it paints small collider boxes (Shift erases). Characters are stopped by the box and walk around it; routes avoid the cells it covers (shown in orange). A model placed in the scene can get a box from **Add collider box from the model** in its Inspector.

## Put things in the scene

Drag a sprite (a little character picture) or a model from **Assets** onto the ground. It becomes a placement: an NPC for sprites, a prop for models. With it selected:

- drag it along the ground to move it (it snaps to cell centres; hold Ctrl to place it freely),
- **W**, **E** and **R** show the move, rotate and scale gizmo; **Snap** at the top right turns the steps on and off,
- the **Inspector** has its type, position, yaw and scale, and its props: the sprite or model, its name, its script, and so on. Every field that names a file or an id is a drop-down you can search; you can also drop a file from Assets onto it.
- **Ctrl+D** duplicates, **Del** deletes, Ctrl+click selects several at once.

Points (where the player can appear), zones (areas that change the camera or start random battles), lights, particle emitters and collider boxes are added from the **Hierarchy** or the Map menu and edited the same way. They are drawn as handles in the Scene; View toggles each kind.

## Give an NPC something to do

An NPC's **pages** say what happens when the player talks to it (**action**), walks into it (**touch**), once when the page becomes active (**auto**) or every tick (**parallel**). Each page names a script module in the `events` folder; the Inspector offers the ones that exist. Pages further down the list win when their conditions (`when`) hold, so a second page with `{"flag": "quest_done"}` replaces the first after the quest. The Inspector shows which page is active right now and a **Run** button plays it; the **Scripts** tab lists every scripted thing on the map, what is running, and the last events (touches, zone enters) the game saw. Badges over the NPCs in the Scene say how each one fires.

Doors are placements of type `door`: pick the target map and one of its points.

## Bring in art and sound

**Assets > Import...** (or drop files onto the Assets panel) copies files into the game folder: images and `.anim.json` files into `sprites/`, models into `fx/`, sounds into `audio/`. The game reloads them as they land. A loose image in `sprites/` opens the **Sprite sheet** window: give the frame size, the number of directions (1, 4 or 8 rows per clip) and the clips (idle, walk, attack...), check the yellow grid over the image, and **Create**. The result is a sprite you can drop into the scene.

Sounds get an id in `data/audio.json` (the Data tab edits it); pickers offer both ids and files. The **missing** button in Assets lists anything a map or table names that has no file.

## Save and play

**Save map** (Ctrl+S) writes `maps/<id>/map.json`. Everything else you changed in the running game that is not part of the map (an entity you nudged in the Live entity section) is gone on the next load; that is on purpose. **Play (F5)** to try it, and **Play from here** on a point in the Hierarchy starts the game at that point.

Keep the game folder in version control: the editor overwrites files, it does not keep history.
