# Scripting a game in Lua

Every rule of a game is Lua. Engine bindings live under the global `fe` (see [api.md](api.md)). The library kits under `lualib/fe/` are ordinary modules: `require` them, change their settings, hook them, or replace a module by putting a file with the same path in your game (for example `fe/battle/sideview.lua`). Each kit's header comment documents its options.

Conventions: vectors are `{x, y, z}`, grid cells are `{x, z, layer}`, and colours are `0xRRGGBBAA` or `{r, g, b, a}`. Asset paths are relative to the game folder; `@fe/...` refers to files shipped in `lualib/fe/`.

## Entry point

`main.lua` returns a table the engine calls: `init()`, `update(dt)` (60 Hz), `draw(alpha)`, `file_changed(path)` and `validate()` (for `--check`). Two helpers build it:

```lua
-- Full control: your own scenes.
return require("fe.game").main({ start = function() scene.push(require("scenes.title")) end })

-- RPG defaults: input, sounds, data tables, title, menu, saves.
return require("fe.starter").main({
  title = "Ridgeline",
  new_game = function() party.join("cora"); field.start("camp", "start") end,
  setup = function() devtools.register("Battle", function() ... end) end,
})
```

## Tasks and scenes

Code that waits runs in a task, which is a coroutine scheduled by `fe.task`:

```lua
local task = require("fe.task")
task.spawn(function()
  task.wait(0.5)
  task.wait_until(function() return door:valid() end)
  local first = task.race({ function() task.wait(3) end, function() task.wait_signal(boss.died) end })
  task.all({ function() a:walk_to(p) end, function() b:walk_to(q) end })
end)
```

Tasks can be named, joined and cancelled. `task.finally(fn)` registers cleanup that runs even on cancellation. Event scripts, cutscenes and battles all run as tasks.

`fe.scene` is a stack of tables with optional `enter`, `leave`, `update(dt)` and `draw()` methods. A scene above another stops the one below from updating (`blocks_update`, true by default) but lets it draw unless `opaque` is set. Draw worlds with `world:draw()` and UI with `fe.canvas` / `fe.ui`.

## State and saves

`fe.state` holds flags and variables of any JSON type; it is what saves contain. Kits register their own sections, such as the party, inventory and quests. `fe.save.write(slot, {meta = ...})` and `fe.save.load(slot)` save and restore everything. `save.add_meta(fn)` adds metadata for the save screen (location, party, playtime), and `save.version` with migrations upgrades old saves. Keep live game state in `fe.state` so it survives hot reload.

## Field and events

`fe.field.start(map, point)` loads a map and spawns its placements by type: npc, chest, door, sign, light, emitter, prop, sound, save_point and symbol. Add your own types with `field.define(type, spawner, fields)`; the fields describe props for the editor. `field.zone(type, handler)` reacts to zones.

Placements run event pages. A page names a script module, a trigger (`action`, `touch`, `auto` or `parallel`) and `when` conditions; the last page whose conditions hold is active. Scripts return a function:

```lua
-- events/elder.lua
local cutscene = require("fe.cutscene")
local dialogue = require("fe.dialogue")
local state = require("fe.state")

return function(ev)
  local elder = cutscene.actor(ev.self)
  elder:face(ev.player)
  dialogue.say("Elder", "Will you help us, {player}?", { portrait = "portraits/elder.png" })
  if dialogue.choice({ "Yes", "Not yet" }) == 1 then
    state.set_flag("quest_started")
    require("fe.field").warp("grotto", "entry")
  end
end
```

`cutscene.actor` wraps an entity with `walk_to`, `walk`, `face`, `anim`, `emote`, `jump_to`, `say`, `bark`, `hide` and `show`. `cutscene.play(fn)` adds letterboxing and locks control; `cutscene.parallel` runs several in parallel. `field.rig` is the camera rig (`pan_to`, `follow`, `release`). Text markup supports `{color:name}`, `{icon:id}`, `{wait:s}` and `{speed:cps}`, `{t:key}` for localised strings, and any tag added with `fe.i18n.add_resolver` (the RPG kit adds `{player}`, `{leader}` and `{actor:id}`).

## RPG data model

`fe.rpg.stats.define{...}` and `stats.resources{...}` declare the stat block. The default is a classic HP/MP block. Wayfarers replaces MP with SP and adds BP, a battle-only boost resource:

```lua
stats.resources({
  hp = { max = "max_hp", name = "HP" },
  sp = { max = "max_sp", name = "SP" },
  bp = { max = 5, start = 1, name = "BP", battle_only = true },
})
```

Actors and enemies are combatants with stat modifiers from equipment, states and passives (`{stat, add | mul | set}`). The party kit manages the roster and the active members. The inventory kit manages items, equipment slots and money. `fe.rpg.database.load()` reads the standard data tables; see [authoring](authoring.md).

## Effects

Skills and items list effects (`damage`, `heal`, `add_state`, `remove_state`, `revive`, `push`, ...). `fe.effects.resolve` runs every effect on every target through the phases `before → hit → calculate → modify → apply → after`. Change the rules with hooks instead of editing kit code:

```lua
local effects = require("fe.effects")
effects.formula("physical", function(ctx) return ctx.user:stat("attack") * ctx.power / 100 end)
effects.hook("modify", function(ctx)                  -- broken targets take double damage
  if ctx.target:has_state("broken") then ctx.multiplier = ctx.multiplier * 2 end
end)
effects.handler("steal", function(ctx) ... end)        -- a new effect type
```

`preview = true` computes results without applying them, for forecasts and AI.

## Battles

A battle is three replaceable parts:

- **Rules** (`fe.battle.core`): hooks on `battle.rules`: `setup`, `on_start`, `on_round`, `on_turn_start`, `on_turn_end`, `before_action`, `after_action`, `on_end`, `outcome`, `escape_chance`, `rewards`, `player_controller`, and `actions[kind]` for custom commands.
- **Scheduler**: `atb` (classic active time battle, wait or active mode), `round_order` (everyone chooses, then actions resolve by speed, with a preview of the next round), `initiative` (CT-based, for tactics). Any table with `run(battle)` works.
- **Presentation**: `fe.battle.sideview` (a stage map, timelines from `data/timelines.json`, HUD, popups) or `fe.battle.tactical`. A presenter is a table of methods named after events (`start`, `turn`, `action`, `results`, `finish`, ...).

```lua
local battle = require("fe.battle")
battle.settings.scheduler = function() return require("fe.battle.schedulers.round_order").new() end
battle.rules = require("battle.rules")
local outcome = battle.encounter("wolves")          -- inside a task: "victory", "defeat" or "escaped"
```

Enemy AI is a Lua module per enemy (`ai/slime.lua`) or the default weighted chooser. Wayfarers' `battle/rules.lua` shows a complete custom mechanic: shields, weaknesses, breaks, boosting and a custom HUD.

## Tactics

`fe.battle.tactical.start{...}` runs a grid battle on a map:

```lua
local outcome = tactical.start({
  map = "ridge",
  deploy = { tag = "deploy", max = 4 },              -- players place units on tagged cells
  enemies = { { enemy = "captain", id = "captain", cell = { x = 12, z = 1 } } },
  win = { tactical.objectives.defeat("captain", "Captain Vey") },
  lose = { tactical.objectives.defeat_all("party"), tactical.objectives.protect("cora") },
})
```

Skills use `range` and `area` (`{shape, min, max, height}` with shapes diamond, square, cross, line, cone or ring) and optional `los`. Height, facing and back attacks, follow-up attacks, zones of control, knockback with fall damage, and CT costs are in `tactical.settings`. Terrain reactions are data in `tactical.reactions`: fire burns grass, ice freezes water so it can be crossed, and thunder spreads through connected water. `rules.plan(battle, unit)` replaces the AI. `auto = true` lets the AI play the party, which tests and replays use.

## Tests

`fengine_headless --test <game>` runs every `tests/*.lua` file. Each returns named functions that receive `t` (`ok`, `eq`, `near`, `error`, `tick`, `wait_until`). Tests run as tasks against the real game, so they can start a new game, run events and fight battles. See `samples/*/tests/game.lua`.
