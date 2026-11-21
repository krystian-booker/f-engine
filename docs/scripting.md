# Writing a game against the engine

The engine provides services: worlds made of a grid and entities, sprites and meshes, movement and path finding, input, a canvas for UI, text, audio, saves, the F1 editor and a headless test runner. They are the bindings under the global `fe` ([api.md](api.md) lists every one). The engine ships no Lua: there is no field, dialogue, menu, battle or inventory module to `require`, and `require` finds only files in the game folder. A game starts from an empty `main.lua` and every rule and every screen in it is the game's own code.

`samples/starfall` is a complete small turn-based RPG in the HD-2D style written that way. This page shows the entry module and a first `main.lua`, then how the sample builds tasks, scenes, button prompts, a field, dialogue and battles on the bindings, the hooks the editor calls, and tests. Read the sample's files next to it; they are short and commented.

Conventions (the full list opens [api.md](api.md)):

- **Angles are degrees**: camera yaw, pitch and fov, `entity.yaw`, `world:spawn{yaw = 90}` and `fe.canvas.push{rotation = 45}`.
- **World values are tables**: positions are `{x, y, z}` (x and z required; leave y out to stand on the ground), ground-plane vectors `{x, z}` (`entity:face`, `entity:drive`), and grid cells `{x, z, layer}` with integer, 0-based coordinates. Every grid function takes and returns cells this way: `world:cell(cell)`, `world:set_cell(cell, changes)`, `world:path(from, to)`; `entity:cell()` gives the cell under an entity.
- **Canvas positions and sizes are numbers**: `fe.canvas.rect(x, y, w, h)`, `fe.input.mouse()` returns `x, y`, `entity:screen_position()` returns `x, y, visible`. A rectangle inside options is `{x, y, w, h}`.
- **Getters are nouns, setters `set_<noun>`**: `fe.app.title()` and `fe.app.set_title()`, `fe.app.fullscreen()` and `fe.app.set_fullscreen()`, `fe.audio.bus_volume()` and `fe.audio.set_bus_volume()`, `fe.audio.paused()` and `fe.audio.set_paused()`. Handles expose plain values as properties (`camera.yaw = 90`, `entity.yaw`, `world.paused`). Vector properties return a copy, so move the camera with `camera.target = {x = 4, y = 0, z = 6}`; `camera.target.x = 4` changes a copy and nothing else.
- **Colours** are `0xRRGGBBAA` integers, colour names or `"#rrggbb[aa]"` strings, or `{r, g, b, a}` tables with 0..1 components (positional `{1, 0.5, 0, 1}` works too; that is the form getters such as `fe.screen.get` return). A number is always a packed `0xRRGGBBAA` integer, so `0.5` is an error, not a grey.
- **Tables you pass are read raw**: options, settings, vectors and colours must hold their fields themselves; a field inherited through a metatable's `__index` is not seen, and no metamethod of a table you pass ever runs inside an engine call. Lights, emitters and `world:set_environment` take the same vector and colour forms as everything else (`light = {color = "#ffaa66", direction = {y = -1}}`).
- **Entity components** are set by a table, read by no argument and removed by nil: `e:mover{speed = 3}` adds or changes the mover and returns its state, `e:mover()` returns that table, or nil when the entity has no mover (nothing is added; `e:mover{}` adds one with its defaults), and `e:mover(nil)` removes it. The same holds for `sprite`, `mesh`, `light`, `emitter`, `collider`, `platform` and `audio`, so check `e:sprite()` before indexing it on an entity that may have none.
- **Invalid input is an error**, never a silent default: arguments and option fields of the wrong type (`{visible = 0}`, a string where a layer belongs), unknown names (a sprite `mode`, text `align`, compass direction, an input action that was never defined), fractions where an integer belongs (a grid cell `{x = 2.5, z = 1}`), NaN, vectors without their components (`entity:move_to{path = cells}` passes options where the target belongs and is rejected; write `entity:move_to(nil, {path = cells})`), and values outside a documented range (a mover radius above 0.49, a duck amount above 1). Leaving an optional argument out (`nil`) uses its default.

Asset paths are relative to the game folder. `@fe/...` names a file in the engine's `builtin/` folder, which holds assets only: the default font, a window skin, a cursor and a bubble (`@fe/assets/ui/window.png`, `cursor.png`, `bubble.png`), emote and spark sprites (`emotes.anim.json`, `spark.anim.json`), the input glyph sheets (`@fe/assets/ui/glyphs_keyboard.png`, `glyphs_xbox.png`, `glyphs_playstation.png`, `glyphs_switch.png`) and the weather presets. `builtin/types/fengine.d.lua` holds LuaLS type stubs for the bindings; point your editor's Lua language server at it for completion and type checks.

## The entry module

`main.lua` returns a table, and the engine calls the functions it finds there. Every field is optional:

| Field | Called |
| --- | --- |
| `init()` | Once, after the bindings exist and before the first tick |
| `update(dt)` | Every tick (60 Hz, `dt` is 1/60), after input was applied and before the worlds move |
| `post_update(dt)` | Every tick, after the worlds moved and produced this tick's events (`world:events()`) |
| `draw(alpha)` | Every frame; `alpha` is how far the frame lies between the last two ticks. Call `world:draw()` for each world to show and draw UI with `fe.canvas` |
| `file_changed(path)` | In development builds, when a file of the game folder other than a Lua module changed (the path is relative to the game folder); reload what the game keeps in memory, such as data tables |
| `validate()` | By `fengine_headless --check`, after `init`; raise an error to fail the check |
| `editor` | Not a function: a table of functions the editor and the debug overlay call (see [Editor hooks](#editor-hooks)) |

Nothing else in a game is called by the engine. There are no engine scenes, no per-entity callbacks and no events pushed into Lua: a game polls input (`fe.input.pressed`), reads what the worlds reported (`world:events()`) and draws what it wants each frame. Engine calls return at once; anything that takes time (a walk, a fade, a conversation) is a coroutine or a state machine the game steps itself.

A whole game that loads a map, puts a sprite on it and walks it around:

```lua
-- main.lua
local world, hero
local speed = 3

local main = {}

function main.init()
  fe.input.define_axis("move_x", { negative = { "key:Left", "pad:dpleft" }, positive = { "key:Right", "pad:dpright" },
    analog = { "pad:leftx" }, deadzone = 0.25 })
  fe.input.define_axis("move_z", { negative = { "key:Up", "pad:dpup" }, positive = { "key:Down", "pad:dpdown" },
    analog = { "pad:lefty" }, deadzone = 0.25 })
  world = fe.world.load("field", "town")               -- maps/town/map.json
  local entry = world:points().entry
  hero = world:spawn {
    id = "hero", position = entry.position, layer = entry.layer,
    sprite = { animation = "sprites/aster.anim.json", move_clip = "walk", idle_clip = "idle" },
    mover = { speed = speed, radius = 0.25 },
    collider = { radius = 0.25, solid = true },
  }
  local camera = world:camera()
  camera.pitch, camera.distance, camera.fov = 42, 18, 30
end

function main.update(dt)                               -- 60 Hz, before the world moves
  local x, z = fe.input.axis("move_x"), fe.input.axis("move_z")
  if x ~= 0 or z ~= 0 then
    hero:drive({ x = x * speed, z = z * speed })       -- units per second, this tick
    hero:face({ x = x, z = z })
  end
end

function main.post_update(dt)                          -- after the world moved
  local p = hero.position
  world:camera().target = { x = p.x, y = p.y + 0.5, z = p.z }
end

function main.draw(alpha)
  world:draw()
  fe.canvas.text("Arrows or the left stick: walk", 8, 8)
end

return main
```

The map decides where the hero can walk (its grid, baked from the terrain and the placed models in the editor) and the mover and collider keep the body out of walls and other solid entities; the sprite plays `walk` while the mover moves and `idle` otherwise, facing where `face` pointed it. Next to `main.lua` the folder needs a `game.json` (a title is enough; see [authoring](authoring.md#game-folder)), the map and the sprite. The names here are the sample's: to try it, copy `maps/town`, `sprites/aster.anim.json` with `aster.png`, and the two files the town's environment names (`ui/amber.cube`, `ui/hills.png`) out of `samples/starfall`. With an empty `main.lua` (`return {}`) the editor already opens, so the usual first step is `./build/fengine <folder> --tools`, File > New map, and laying the map out ([editor guide](editor.md)).

```sh
./build/fengine path/to/game            # play (F1 opens the debug overlay in development builds)
./build/fengine_headless --check path/to/game
./build/fengine_headless --test path/to/game
```

Callbacks the engine calls in the middle of its own work run under a C call and cannot yield: a path `cost` function (`world:path`, `entity:move_to` rules) runs inside the search and a `fe.text.resolver` inside text layout, so a `coroutine.yield` in them raises "attempt to yield across a C-call boundary". Compute the answer and return it.

## How the sample is built

`samples/starfall/main.lua` is under sixty lines: `init` reads the settings file, defines the input actions, loads the data tables and opens the title; `update` steps the tasks, the top scene and the toasts; `post_update` and `draw` go to the scenes. Everything else is in `game/`:

| File | Role |
| --- | --- |
| `game/task.lua`, `game/scenes.lua` | Coroutines and the scene stack |
| `game/input.lua`, `game/ui.lua` | Actions, button prompts, windows, lists, toasts |
| `game/data.lua`, `game/state.lua`, `game/audio.lua` | Data tables, the save file, music and sounds by id |
| `game/field.lua` | Exploration: placement types, interaction, doors, vehicles, encounters, the camera |
| `game/dialogue.lua`, `events/*.lua` | Message boxes and choices; what the people and things on the maps do |
| `game/battle/rules.lua`, `game/battle/scene.lua` | The battle system without drawing; the battle on screen |
| `game/menu.lua`, `game/shop.lua`, `game/title.lua` | The party menu with save and load, a shop, the title |
| `game/editor.lua` | The editor hooks |
| `tests/battle.lua`, `tests/game.lua` | Headless tests |

None of these names mean anything to the engine. Lay a game out differently if it suits it.

### Tasks

Engine calls never wait, so "show a line, then walk to the door, then fade" needs coroutines. `game/task.lua` is a scheduler in under a hundred lines: `task.spawn(fn, name)` starts a coroutine that first runs on the next update, and `task.update(dt)`, called once a tick from `main.update`, resumes each one with that tick's `dt`.

```lua
local task = require("game.task")
task.spawn(function()
  task.wait(0.5)                                           -- seconds of game time
  task.wait_until(function() return door_open end)
  task.over(0.25, function(t) fe.screen.set { fade = { 0, 0, 0, t } } end)   -- t rises from 0 to 1
end, "open the door")
```

`task.frame()` yields for one tick, `task.cancel(entry)` stops a task before its next resume, `task.clear()` drops them all (the sample does that on the way back to the title) and `task.done(entry)` asks whether one has finished. A task that raises is logged with the coroutine's traceback (`fe.dev.traceback(co, message)`) and dropped; the others keep running. That is the whole design, and it is enough for the sample. A larger game adds what it needs (child tasks, cleanup on cancel, clocks that follow a paused world): it is the game's module.

Functions that take time are then plain blocking functions called from a task: `dialogue.say(...)`, `field.warp(map, point)`, `battle.start(troop)`. An event script is a function that calls them in order.

### Scenes

`game/scenes.lua` is a stack of tables with optional `enter`, `leave`, `update(dt)`, `post_update(dt)` and `draw(alpha)`. Only the top scene updates; scenes draw bottom first, starting at the topmost one marked `opaque = true`. So the field keeps drawing under a message box or the menu and stands still while they are open, and a battle (opaque) hides it. A message box is a scene that a task pushes, waits on and pops:

```lua
scenes.push(box)
task.wait_until(function() return box.closed end)
scenes.pop(box)
```

Whether the world under a menu also stops is a separate switch the engine offers per world: `world.paused = true` stops its tick (it still draws), and `world.time_scale` slows or speeds it.

### Input and button prompts

`game/input.lua` defines the actions with `fe.input.define(action, inputs)` (the defaults, or the player's rebindings from the settings file) and two axes with `fe.input.define_axis`. Scenes then ask `fe.input.pressed("confirm")`, `fe.input.held("run")`, `fe.input.axis("move_x")` and, for menus, `fe.input.repeating(action, delay, interval)`. Raw input names (`key:Return`, `pad:a`, `pad:leftx+`) are listed in [authoring](authoring.md#input-names).

Prompts follow the device in use. `fe.input.device_type()` is `"keyboard"` or the layout of the gamepad used last (`"xbox"`, `"playstation"`, `"switch"`, `"steamdeck"`, `"generic"`), and it changes only on deliberate input: a key or mouse button going down (never a release), a gamepad button or a stick pushed past half way (and not again until it has come back to rest), the mouse travelling 12 canvas pixels after a gamepad was in use, a gamepad being plugged in, or the one in use being unplugged (the prompt falls back to another pad, else the keyboard). `input.prompt(action)` turns that into markup:

```lua
function input.prompt(action)
  local sheet = input.device()                       -- "keyboard", "xbox", "playstation" or "switch"
  ...
  for _, raw in ipairs(fe.input.bindings(action)) do -- the first binding that belongs to this device
    ...
    local id = icons[sheet][kind == "mouse" and raw or name]
    if id then return "{icon:" .. id .. "}" end      -- a glyph registered with fe.text.icon
    fallback = fallback or "[" .. fe.input.label(raw) .. "]"
  end
  return fallback or "[" .. action .. "]"
end
```

The glyphs are cells of the builtin sheets, registered once with `fe.text.icon(id, {image = "@fe/assets/ui/glyphs_xbox.png", rect = {...}})`; Steam Deck and generic pads use the Xbox sheet. A key the keyboard sheet has no picture of falls back to `fe.input.label(raw)`, the key's label under the player's keyboard layout (`key:Z` reads `Y` on a German keyboard). The sample looks prompts up every time it draws them (`ui.hints{{"confirm", "Talk"}, {"menu", "Menu"}}`), so the whole UI follows a switch on the next frame; a UI that caches its layout rebuilds it on the tick `fe.input.device_changed()` is true. `replays/devices.json` plays that switch and compares the screens.

### UI

There is no widget library in the engine. `fe.canvas` draws rectangles, lines, images, nine-slice windows and text (markup with colours, icons, sizes, bold, ruby; see [Text](architecture.md#text)) on a virtual canvas of `game.json`'s `virtual_resolution`, and `fe.text` measures and paginates. `game/ui.lua` is the sample's immediate-mode layer on top, about a hundred lines: `ui.window` (a nine-slice of `@fe/assets/ui/window.png`), `ui.text`, `ui.bar` (a gauge), `ui.cursor` with `ui.list_input` and `ui.list` (a scrolling list with a cursor), `ui.hints` (the prompt row) and `ui.toast`. Scenes call them from `draw` every frame; a scene's state is a cursor index and little else.

### Data and saves

`game/data.lua` loads `data/<name>.json` files with `fe.json.read`, indexes their records by `id` and offers `data.get(name, id)`, `data.find` and `data.list`. `data.validate()` checks every reference between the tables and is what `main.validate` runs under `--check`. The formats are the game's own ([authoring](authoring.md#data-tables)).

`game/state.lua` keeps everything a save holds in one plain table, `state.data` (the party, items, flags, the bestiary, opened chests, vehicles, the map and position), and saving is one call: `fe.save.write(slot, state.data, state.summary())`. The third argument is the slot's `meta`, a small table `fe.save.list()` returns without reading the saves, for the load screen (level, map, playtime). `fe.save.read(slot)` returns the data and the meta. Player settings are a separate file: `fe.save.read_config()` and `fe.save.write_config(settings)`; the sample reads key bindings and bus volumes from it in `init`.

What the engine guarantees about saves:

- Saves, settings and `fe.json.encode` store Lua tables as JSON, so data must be booleans, finite numbers, strings and tables. Sequences become arrays and other tables objects; integer keys are written as `"#<n>"` (and a string key that starts with `#` gets a second `#`), so sparse arrays such as `{[1] = a, [3] = c}` and mixed tables come back with the same keys.
- Every save and settings write is flushed to disk before it atomically replaces the old file, and the previous version is kept as `<file>.bak`. A write that cannot be flushed raises instead of replacing the good file. A save slot reads its `.bak` only when the slot's file is missing or cut short (empty, or JSON that ends early, as an interrupted write leaves it); `fe.save.read` then logs a warning and returns `true` as a third result, and `fe.save.list` marks the slot `backup = true`. A slot file that is complete but unreadable (invalid JSON, nested too deep, not a save) raises an error instead of quietly loading the previous save. The settings file falls back to its `.bak` whenever it is unreadable.
- Saving does not stall a frame: `fe.save.write`, `write_config` and `write_blob` check and serialise the data, hand it to a worker thread and return; the flush to disk happens there. A read or `fe.save.list()` right after a write sees the new data. `fe.save.pending()` is the number of writes still on their way (show a save icon while it is above 0), and `fe.save.flush()` waits for them and raises if one failed (a full disk); the engine flushes when the game closes, so a save followed by quitting is safe. A failed background write is also logged as an error.
- `fe.save.write` raises, writing nothing, for data it could not read back: tables nested deeper than 127 levels (the save file wraps the data in one more) and values `fe.json.encode` rejects. Slot names are 1-32 lowercase letters, digits, `_` or `-`, excluding names Windows reserves (`con`, `nul`, `com1`...).
- `fe.save.thumbnail(slot)` captures the next frame's worlds, without the UI on top, as `<user data>/thumbnails/<slot>.png`, and `fe.save.thumbnail_path(slot)` returns it as `"user://thumbnails/<slot>.png"` (nil until it exists), a path `fe.canvas.image` draws. `user://` images are read only, resolve only inside the user-data folder, and reload when the file changes. Deleting a slot deletes its thumbnail.
- Bytes that are not JSON (a world snapshot, recorded inputs, a compressed log) go next to a slot as blobs: `fe.save.write_blob(slot, name, bytes)` stores any Lua string (binary included, at most 16 MiB) under a slot-style name, `fe.save.read_blob(slot, name)` returns it or nil, `fe.save.remove_blob` and `fe.save.blobs(slot)` manage them, and deleting the slot deletes them. They travel through the same save backend as the slots, so a port's cloud saves carry them.
- Files the player drops in or the game exports live in `<user data>/files/`: `fe.fs.user_list(folder)` (entries, folders ending in `/`), `fe.fs.user_read(path)` (the bytes, or nil), `fe.fs.user_write(path, bytes)` (atomic, creating folders; at most 16 MiB per file and 256 MiB in all) and `fe.fs.user_remove(path)`. Paths never leave that folder (`..`, absolute paths and NUL raise). A mod folder is the typical use: a game lists `mods/`, reads each `mods/<name>/mod.json` and merges what it names on top of its own data; the engine itself loads nothing from there.

### The field

A map is data: a grid, named points, placements (`{id, type, position, props}`), zones, lights and an environment ([authoring](authoring.md#maps)). `fe.world.load(name, map)` gives a `World` with the grid, the map's lights, emitters and collision, and a camera. It spawns nothing for the placements: `world:placements()` returns them and the game decides what each `type` is. In `game/field.lua` that is a table of spawners:

```lua
function field.types.chest(world, placement)
  local key = world.map_id .. "/" .. placement.id
  local spec = visual(placement)                       -- { sprite = ... } or { mesh = ... } from props
  spec.collider = { radius = 0.3, solid = true }
  if spec.sprite then spec.sprite.clip = state.data.opened[key] and "open" or "closed" end
  return remember(spawn(world, placement, spec), placement, {
    label = "Open",
    interact = function(entity) ... end,               -- runs in a task when the player presses confirm
  })
end

for _, placement in ipairs(world:placements()) do field.spawn_placement(world, placement) end
```

`remember` stores the placement and the closures in `entity:data()`, a Lua table that lives as long as the entity, so "what happens when the player acts on this" is a field of the entity's own data. The sample's types are `prop`, `npc`, `chest`, `sign`, `save_point`, `door`, `boss`, `symbol` (a visible enemy), `vehicle`, `emitter` and `sound`; adding one is adding a function to `field.types` and its form to `game/editor.lua`.

The rest of the field is the same pattern, the game reading the world and deciding:

- **Walking.** The field scene's `update` reads the axes and calls `player:drive(velocity)` and `player:face(vector)`; `world:query{near = p, radius = r}` finds the entities in reach and the one ahead of the player becomes the target of the confirm button, with its `label` shown as a prompt.
- **Events.** `post_update` reads `world:events()`: `cell_enter` for the player counts a step and checks for a door on that cell, `contact_begin` with an entity that has a `touch` function starts it. `field.event(fn, name)` runs a function as a task with the player's control locked until it returns.
- **Doors and warps.** `field.warp(map, point)` fades out with `fe.screen.set{fade = ...}`, unloads the world, loads the next and fades in. It first calls `fe.dev.leave_map(world.name)`, which lets the editor ask about unsaved edits of the map being left (it answers `"ok"` at once when there is nothing to ask).
- **The world map** is an ordinary map with `"wrap": true` in its grid: walking off one edge arrives at the other. Positions near a seam are compared with `world:wrap_delta(a, b)` instead of subtracting them.
- **Vehicles** are the player's own mover with other settings: the strider is `mover{fly = true, blocked_tags = {"water", "mountain"}}` at a higher speed (it crosses shallows; cells tagged `water` or `mountain` stop it), the skiff adds `altitude = 2.4` and no blocked tags, so it flies over everything. Boarding swaps the player's sprite and mover (`apply_mount`); leaving checks the cell under it with `world:cell(cell)`.
- **Random encounters** are steps on cells whose tags name a zone (`encounter:plains`): each step adds to a danger value from `data/encounters.json`'s rate with a random factor from a seeded `fe.random` stream, and a battle starts when it passes a limit drawn when the last one ended. The sensor in the corner draws `field.danger()`.
- **The camera** follows the player in `post_update`: `world:camera()` is an orbit (`target`, `yaw`, `pitch`, `distance`, `fov`), and the map's `camera` prop and zones of type `camera` under the player (`world:zones_at(position, layer)`) set the distance and pitch.

### Dialogue and events

`game/dialogue.lua` is a message box scene: `fe.text.paginate(text, {width = w}, lines)` splits the text into pages, `fe.text.units(page).count` is how many units a typewriter reveals, and `fe.canvas.text(markup, x, y, {width = w, visible = n})` draws the first `n`. `dialogue.say(speaker, text, settings)` and `dialogue.choose(speaker, text, options, settings)` block the calling task.

An event script is a module in `events/` that returns a function. A placement names it in its `script` prop, and the field calls it in a task with a context (`entity`, `player`, `world`, `field`):

```lua
-- events/guardian.lua
local dialogue = require("game.dialogue")
local state = require("game.state")

return function(context)
  dialogue.say(nil, "The drake uncoils, frost on its breath.")
  local outcome = require("game.battle.scene").start("guardian")
  if outcome == "victory" then
    state.set_flag("guardian_defeated")
    if context.entity:valid() then context.entity:destroy() end
    dialogue.say(nil, "The way to the crystal is open.")
  end
end
```

Flags are a table in the save (`state.flag`, `state.set_flag`); "this NPC says something else after the boss" is an `if` in its script.

### Battles

The sample splits its battle in two, and the split is what makes it testable:

- `game/battle/rules.lua` holds the rules and no drawing. `rules.new{party, troop, rng, opening, known, consume}` makes a battle; `battle:advance()` returns what comes next (`round`, `act`, `skip`, `unison` or `over`) with a list of events, `battle:perform(unit, action)` and `battle:unison(echo)` return the events an action caused, `battle:ai(unit)` picks a foe's action. Its mechanics are a round timeline by speed, a break gauge per foe, bonus actions for whoever broke one, a sweep (every foe broken at once) followed by a unison, surge gauges, equippable echoes with mastery ranks, cover, elemental affinities (weak, resist, null, absorb) hidden until seen, ailments, a summon that stays three turns, and preemptive and ambush openings. Every number comes from `data/*.json` and `rules.settings`.
- `game/battle/scene.lua` shows it. `battle.start(troop)` freezes the field on screen (`fe.screen.freeze()`), loads a stage map as a second world (`fe.world.load("battle", stage)`), spawns the party and the foes on its named points, swirls in with `fe.screen.set{transition = {kind = "swirl", progress = t}}`, then loops, in outline:

```lua
while true do
  local turn = self.rules:advance()
  show(self, turn.events)                                    -- animations, floating numbers, banners
  if turn.type == "over" then break end
  if turn.type == "act" then
    local action = turn.unit.hero and choose(self, turn.unit, turn.bonus) or self.rules:ai(turn.unit)
    show(self, self.rules:perform(turn.unit, action))
  elseif turn.type == "unison" then
    show(self, self.rules:unison(choose_unison(self, turn.echoes)))
  end
end
```

`choose` is a command menu that blocks the task until the player has picked; `show` plays each event with sprite clips (`entity:clip("attack", true)`), flashes and floating numbers, waiting with `task.wait`. The function returns `"victory"`, `"fled"` or `"defeat"` to the event script or encounter that called it.

The boss is a rigged 3D model among the sprites. A skinned glTF model is spawned like any mesh and its skeleton's animations play by name, with a crossfade:

```lua
local e = world:spawn { position = at, yaw = 90, scale = u.record.scale or 1, mesh = { model = u.record.model } }
e:play("idle", true)                 -- animation, loop
e:play("attack", false, 0.12)        -- blend from idle into attack over 0.12 s
```

`fe.app.model_animations(path)` lists a model's animations with their lengths, and a non-looping one emits `anim_finished` in `world:events()` when it ends, as a sprite clip does ([authoring](authoring.md#models-and-skeletons) covers exporting one).

## Editor hooks

The editor (`fengine_editor`) edits maps, assets and data files without any help from the game. What it cannot know is what the game's placement types are, how to start the game on a map, and what is running. For that it calls optional functions in the `editor` table of the table `main.lua` returns. A game without the table still gets the whole editor: a scene is always shown in a preview world of the editor's own, where placements with a `model` or `sprite` prop are shown as that model or sprite, and Play then starts the game from its beginning.

The three hooks most games want first (`start`, `spawn_placement` and `world` stand for the game's own function that loads a map, its function that spawns one placement's entity, and the world it loaded):

```lua
main.editor = {
  -- Forms for placement props, as JSON text.
  describe = function()
    return fe.json.encode {
      entities = {
        prop = { fields = { model = { type = "asset", asset = "model" }, solid = { type = "boolean", default = true } } },
        npc = { fields = { sprite = { type = "asset", asset = "sprite" }, name = { type = "string", optional = true } } },
      },
      defaults = { entities = "npc" },
    }
  end,
  -- Play in the editor: start the game on this map at this point.
  play_from = function(map, point) start(map, point) end,
  -- A placement was added, removed or edited: respawn its entity from the live map.
  placement_changed = function(world_name, id)
    local placement = world:placement(id)
    if placement then spawn_placement(placement) elseif world:find(id) then world:find(id):destroy() end
  end,
}
```

`samples/starfall/game/editor.lua` implements them in about 150 lines: `describe` (every placement type, the `camera` zone, map props and the data tables), `play_from`, `placement_changed`, `tasks`, `cancel_task` and `task_count` (the Scripts panel and the Profiler), `validate_table` (the Data tab), and `list` and `launch` (the Launch tab: "Battle: golem", "Own both vehicles"). The full list, with the shape of what each returns and the field types a form can use, is in [authoring](authoring.md#editor-hooks).

## Devices, display and the platform

- **Text entry.** `fe.input.text_begin(rect, kind)` opens the IME (its candidate window placed near `rect`, in canvas pixels) or an on-screen keyboard where there is one, such as Steam Deck's; `kind` is `"text"`, `"name"` or `"number"`. Each tick `fe.input.text()` is the text committed since the last tick and `fe.input.composition()` the unfinished IME text and cursor. Keys still arrive as raw inputs, so a name-entry screen should handle Backspace and Enter through `key:` inputs and ignore other actions on ticks that produced text. `fe.input.text_end()` closes it. Tests inject text with `fe.input.feed_text(text)` (and a composition with `fe.input.feed_text(text, cursor)`).
- **Gamepads and rumble.** `fe.input.pads()` lists connected gamepads by number with their layout and rumble support; `fe.input.rumble(low, high, seconds, pad?)` and `fe.input.rumble_triggers(left, right, seconds, pad?)` vibrate them. Gamepads connecting and disconnecting are in `fe.input.pad_events()` on the tick after they happen (`{type = "connected" | "disconnected", pad, name}`); a game that pauses when the player's pad is unplugged reads it in `update`. Tests and replays plug a virtual gamepad in with `fe.input.feed_pad(pad, {name, type, rumble, trigger_rumble})` and unplug it with `fe.input.feed_pad(pad)`; `--record` writes real connections into the replay.
- **Display.** `fe.app.display()` returns the fullscreen mode (`false`, `"borderless"` or `"exclusive"`), window size, vsync (`"on"`, `"off"`, `"adaptive"`), monitor, the lists of monitors and exclusive modes, and `scale`, the window's pixels per screen unit (2 on a Retina display, 1 headless; `fe.app.viewport()` is the window size times it, so a game choosing `post.pixel_scale` or a UI scale by the real pixel count reads it here, and polls `fe.app.display()` again after a window moved between monitors); `fe.app.set_display{...}` changes any of the settings after the frame (`scale` and `hdr` are reports, not settings). The engine doesn't remember them, so keep them in the settings file (`fe.save.write_config`) and apply them in `init`.
- **Time and language.** `fe.time.local_date(fe.time.wall())` gives the player's calendar date and time for save slots; `fe.app.locale()` the system language (`"en-US"` in tests and replays). `fe.app.format_number(n, {decimals, locale})` writes a number with that language's decimal and digit-group separators (`1,234.50` in English, `1.234,50` in German, a no-break space grouping French, Russian, Polish and Swedish; a built-in table covers en, de, fr, es, it, pt, ja, zh, ko, ru, pl, nl, sv and tr, others format as English). Translated text is the game's: keep strings in its own files (`fe.json.read("lang/de.json")`) and, for tags inside markup, install a `fe.text.resolver`.
- **Clipboard.** `fe.app.clipboard()` is the clipboard's text (nil when it holds none; cut at a character boundary beyond 64 KiB) and `fe.app.set_clipboard(text)` puts text there (at most 64 KiB, or it raises). It is the system clipboard in the windowed runtime; tests, headless runs, replays and recordings get an in-memory one that returns what the session wrote, so a replay never depends on the machine. A text field that wants paste reads it when `key:V` is pressed while `fe.input.raw("key:Left Ctrl")` or `fe.input.raw("key:Left GUI")` is down.
- **Content packs.** `fe.app.packs()` lists the `.pak` archives laid over the game at start, in mount order: `packs/<name>.pak` from the game folder, then `user://packs/<name>.pak` from the user-data folder, each set in name order. A file in a later pack replaces the same path in an earlier one and in the game itself, loose or packed, also in development builds; `game.json` is read before they mount, so a pack changes files (maps, data, images, Lua modules), never the configuration. DLC ships as `<game>/packs/*.pak` (the packager copies them next to `game.pak`); a player's mods go in `<user data>/packs/`. A pack added while the game runs is seen after a restart, and hot reload does not watch packs.
- **Frame stats.** `fe.dev.stats()` returns the last frame's `frame_ms`, `lua_update_ms`, `lua_draw_ms`, `draw_calls`, `entities`, `particles` and `memory_kb` (frame time and draw calls are 0 headless), for quality settings that react to the frame rate (lower `post.pixel_scale`, turn off `ssao`) and for debug overlays.
- **Store services.** `fe.platform.unlock(id)`, `fe.platform.unlocked(id)`, `fe.platform.set_stat(name, value)` and `fe.platform.stat(name)` go to the store backend a port installed, or to the user-data folder.
- **Loading screens.** `fe.world.load_async(name, map)` returns a ticket; poll `fe.world.load_progress(ticket)` from a task while drawing a loading indicator, then `fe.world.finish_load(ticket)` returns the `World`. The map is parsed and its assets decoded on worker threads, and what it draws is uploaded to the GPU a little each frame; progress reaches 1 when the first frame of the new world has nothing left to load, so wait for 1 and the change of map costs no long frame. The sample's `field.warp` does this behind its fade (`game/field.lua`). For what a map does not name (the sprites and models of the enemies in the next battle, a summon), `fe.app.preload(paths)` does the same for models, sprite animations with their atlases and images, with `fe.app.progress(ticket)`; the sample's battle prepares its stage and both sides this way while the field is frozen on screen (`prepare` in `game/battle/scene.lua`). Anything not loaded ahead still works: it loads when it is first drawn, in that frame. A load that will not be finished (the waiting task was cancelled) is dropped with `fe.world.cancel_load(ticket)`: the ticket stops advancing and becomes unknown, a worker still parsing the map finishes in the background and its result is discarded, and what was decoded stays in the asset cache like any other asset. A ticket left neither finished nor cancelled keeps advancing (and holding its parsed map) for the rest of the session.
- **Audio.** `fe.audio.music(path, options)` streams and crossfades music, `fe.audio.sound(path, options)` plays a one-shot or looping voice (positional with `position`), buses have volumes and effects, and files are WAV, FLAC, MP3 or Ogg Vorbis ([Audio](architecture.md#audio)). The sample's `game/audio.lua` maps ids to files through `data/audio.json`, which is also where the editor's audio pickers find ids.

## Screen effects and viewports

`fe.screen.set` animates full-screen effects that cover worlds and UI alike: `fade`, `flash`, `letterbox`, `radial_blur` (with `radial_centre`), `chromatic` and `transition` (`fade`, `swirl`, `mosaic`, `wipe`, `iris`, `shatter`, driven by `progress`); `tint` multiplies only the 3D scenes. Per-world looks (bloom, depth of field, occlusion, distortion, chromatic aberration, grading, game post shaders) are `world:set_environment{post = {...}}` fields; see [authoring](authoring.md#maps).

A battle intro freezes the field and transitions to the battle rendering underneath. From the sample's `battle.start`, inside a task:

```lua
fe.screen.freeze()                         -- hold what is on screen now
build(self)                                -- load the stage world; the live frame changes underneath
scenes.push(self)
task.over(0.6, function(t) fe.screen.set { transition = { kind = "swirl", progress = t } } end)
fe.screen.unfreeze()
fe.screen.set { transition = { kind = "none", progress = 0 } }
```

While frozen, a transition reveals the live frame instead of its colour; without a transition the held image stays. `world:draw` takes options for more than one view per frame: `rect = {x, y, w, h}` (canvas pixels) draws into part of the screen, `target = "name"` draws off-screen for `fe.canvas.image("rt:name", x, y, {w = ..., h = ...})` (a minimap, a 3D portrait), and `camera = {target, yaw, pitch, distance, fov, orthographic, ortho_height}` overrides the world's camera for that draw.

The camera is an orbit with a perspective projection; `camera.orthographic = true` makes it parallel for pixel-exact 2D reference modes (a top-down map, a side-on stage): the eye stays on the orbit, `camera.ortho_height` is how many world units the view covers vertically (by default what the perspective view covered at the target, so toggling it mid-game keeps the framing; set it directly to pin a scale such as `canvas_height / pixels_per_unit` for one texel per canvas pixel), and `world:pick_entity`, `camera:project`, `camera:ray`, shadows and `pixel_snap` all follow. Per-world resolution scaling is `post.pixel_scale` (see [authoring](authoring.md#maps)): `world:set_environment{post = {pixel_scale = 1.5}}` renders the scene at two thirds of the output size and filters it up, a whole number keeps crisp pixel blocks.

## Determinism

Tests and replays run with a fixed seed, and the engine keeps everything it controls repeatable: `fe.random` streams, `math.random`, and the order of `pairs` over string and number keys. The order of `pairs` over a table keyed by objects (tables, functions, coroutines, entities and other engine objects) depends on memory addresses and changes from run to run, so wherever that order affects the outcome (who acts first, which target is hit, what is spawned), keep the objects in a list or sort the keys by a stable value such as an id or `entity.key`. The sample's battle rules take their random stream as an argument (`rules.new{rng = fe.random.new(7)}`), which is how its tests pin every roll.

## Tests

`fengine_headless --test <game>` runs every `tests/*.lua` file of the game with a runner built into the program; a game needs no test library. Each file returns a table of named test functions, and each is called with a helper table `t`:

```lua
-- tests/walk.lua
return {
  _before_each = function(t)
    t.ok(fe.harness.start())                       -- load main.lua and run its init
  end,

  ["holding right walks the hero east"] = function(t)
    local hero = fe.world.get("field"):find("hero")
    local before = hero.position.x
    fe.input.feed("key:Right", 1)                  -- raw input, applied on the next tick
    t.tick(30)                                     -- half a second of game
    fe.input.feed("key:Right", 0)
    t.tick(1)
    t.ok(hero.position.x > before + 1, "the hero did not move")
  end,
}
```

| Helper | Meaning |
| --- | --- |
| `t.ok(value, message)` | Fails unless `value` is true |
| `t.eq(actual, expected, message)` | Deep equality of values and tables |
| `t.near(actual, expected, epsilon, message)` | Numbers within `epsilon` (default 1e-6) |
| `t.error(fn, pattern)` | `fn` must raise; with `pattern`, the error must match it (a Lua pattern) |
| `t.tick(n)` | Advance the engine by `n` fixed ticks (default 1): update, worlds, post_update and a draw each |
| `t.wait_until(predicate, ticks, message, wall_seconds)` | Tick until the predicate holds; fails only once `ticks` ticks (default 600) and `wall_seconds` of real time (default 10) have both passed, so background world loads finish however fast the build runs |

Keys starting with `_` are not tests. `_before_each(t)` and `_after_each(t)` run around every test of their file; a game resets its own state there, since every file and every test share one engine and one Lua state for the run (the sample's `tests/game.lua` starts the engine once and reopens the title before each test). Files run in name order and a file's tests in name order. `--filter text` runs only the tests whose name or file contains the text. A test fails when it raises, when a hook raises, or when a script error was reported while it ran (a task of the game that failed during `t.tick`).

Tests are plain function calls, not coroutines: `t.tick` advances the game, including whatever scheduler the game runs in `update`, and returns. To play the game, feed input and tick, as a player would: the sample's `tests/game.lua` presses confirm on the title, walks, talks to NPCs, opens chests, rides the strider, fights and saves that way, and `tests/battle.lua` drives `game/battle/rules.lua` directly without a world. `fe.harness` (`start`, `tick`, `errors`, `begin_test`, `reload`) exists only under `--test`; `fe.harness.reload("game.field")` hot-reloads a module in the middle of a test, as a file change does in a development build. Each test and each tick gets the script watchdog's three seconds.
