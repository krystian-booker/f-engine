# Scripting a game in Lua

Every rule of a game is Lua. Engine bindings live under the global `fe` (see [api.md](api.md)). The library kits under `lualib/fe/` are ordinary modules: `require` them, change their settings, hook them, or replace a module by putting a file with the same path in your game (for example `fe/battle/sideview.lua`). Each kit's header comment documents its options.

Conventions (the full list opens [api.md](api.md)):

- **Angles are degrees**: camera yaw, pitch and fov, `entity.yaw`, `world:spawn{yaw = 90}` and `fe.canvas.push{rotation = 45}`.
- **World values are tables**: positions are `{x, y, z}` (x and z required; leave y out to stand on the ground), ground-plane vectors `{x, z}` (`entity:face`, `entity:drive`), and grid cells `{x, z, layer}` with integer, 0-based coordinates. Every grid function takes and returns cells this way: `world:cell(cell)`, `world:set_cell(cell, changes)`, `world:path(from, to)`, `world:reachable(start, budget)`; `entity:cell()` gives the cell under an entity. In a `world:reachable` result the first entry is the start cell, with no `parent`; every other entry's `parent` is the list index it is reached from.
- **Canvas positions and sizes are numbers**: `fe.canvas.rect(x, y, w, h)`, `fe.input.mouse()` returns `x, y`, `entity:screen_position()` returns `x, y, visible`. A rectangle inside options is `{x, y, w, h}`.
- **Getters are nouns, setters `set_<noun>`**: `fe.app.title()` and `fe.app.set_title()`, `fe.app.fullscreen()` and `fe.app.set_fullscreen()`, `fe.audio.bus_volume()` and `fe.audio.set_bus_volume()`, `fe.audio.paused()` and `fe.audio.set_paused()`. Handles expose plain values as properties (`camera.yaw = 90`, `entity.yaw`, `world.paused`). Vector properties return a copy, so move the camera with `camera.target = {x = 4, y = 0, z = 6}`; `camera.target.x = 4` changes a copy and nothing else.
- **Colours** are `0xRRGGBBAA` integers, colour names or `"#rrggbb[aa]"` strings, or `{r, g, b, a}` tables with 0..1 components (positional `{1, 0.5, 0, 1}` works too; that is the form getters such as `fe.screen.get` return). A number is always a packed `0xRRGGBBAA` integer, so `0.5` is an error, not a grey.
- **Tables you pass are read raw**: options, settings, vectors and colours must hold their fields themselves; a field inherited through a metatable's `__index` is not seen, and no metamethod of a table you pass ever runs inside an engine call. Lights, emitters and `world:set_environment` take the same vector and colour forms as everything else (`light = {color = "#ffaa66", direction = {y = -1}}`).
- **Invalid input is an error**, never a silent default: arguments and option fields of the wrong type (`{visible = 0}`, a string where a layer belongs), unknown names (a sprite `mode`, text `align`, compass direction, an input action that was never defined), fractions where an integer belongs (a grid cell `{x = 2.5, z = 1}`), NaN, vectors without their components (`entity:move_to{path = cells}` passes options where the target belongs and is rejected; write `entity:move_to(nil, {path = cells})`), and values outside a documented range (a mover radius above 0.49, a duck amount above 1). Leaving an optional argument out (`nil`) uses its default.

Asset paths are relative to the game folder; `@fe/...` refers to files shipped in `lualib/fe/`.

## Entry point

`main.lua` returns a table the engine calls: `init()`, `update(dt)` (60 Hz, before the worlds move), `post_update(dt)` (after the worlds moved and produced this tick's events), `draw(alpha)`, `file_changed(path)` and `validate()` (for `--check`). Every function is optional. Two helpers build it:

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

`fe.starter` installs the RPG kit only when the game uses it (`rpg = true`, or by default when `data/actors.json` exists); without it the field, events, dialogue, saves and title still work, and `menu` can be `false` or your own function. New Game and Continue cancel running tasks and reset field locks and event holds before starting. `setup` runs before the title, so game-defined input actions exist when saved rebindings are applied (bindings for actions that no longer exist are skipped with a warning).

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

A task ends as `done`, `failed` (it raised, also inside a `wait_until` predicate) or `cancelled`. A task started inside another task is its child: cancelling or failing a task cancels its running children, and when a task finishes normally its still-running children are handed to its own parent, so effects started at the end of a script complete. Handed-on children keep what they inherited from the finished task: its world binding, speed and skipping. `task.detached(name, fn)` starts a task with no parent, for work that must outlive the caller (a door's warp, a pending autosave). `Task:spawn(name, fn)` starts a child of a given task from anywhere, and `Task:set_parent(parent)` moves a task (keeping what it inherited) under another one, or makes it top-level with nil.

Each task has its own clock (`task.clock()`), which `wait` and `wait_until` timeouts use. One rule sets its pace: a task bound to a world runs on that world's time, any other task on game time. `task.bind(world)` binds the current task and the tasks it starts to a world: while the world is paused the task is frozen, and otherwise its clock advances exactly as far as the world did that tick (the tick times the world's `time_scale`, which already contains `game.time_scale`, hit-stop and fast-forward, so nothing is applied twice). An unbound task, or one bound to a world that is not loaded, advances by the tick times `game.time_scale`, like `task.time()`. Event pages, interactions and cutscenes are bound to the field world this way. `Task.speed` multiplies a task's clock on top of that, and a task marked `skipping` finishes every `task.wait` at once (`task.skipping()`; `task.end_skip()` clears it and runs the skipping task's `on_end_skip`).

Cleanup is cancellation-safe. `task.finally(fn)` (or `Task:finally`) runs `fn` when the task ends for any reason, newest first. `task.ensure(fn, cleanup)` runs `cleanup` after `fn` returns, raises or is cancelled. `task.cleanup(fn)` returns a run-once function that also runs if the task ends first; kit locks (`field.lock()`, `event.block()`) use it, so a lock never outlives its task. `task.wait_signal` disconnects when cancelled. `task.all` and `task.race` raise a child's failure in the caller and cancel the others; `race` returns the index of the first child to finish normally.

`fe.scene` is a stack of tables with optional `enter`, `leave`, `update(dt)`, `post_update(dt)` and `draw()` methods. `update` runs before the worlds move and `post_update` after, when this tick's movement and world events are known. A scene above another stops the one below from updating (`blocks_update`, true by default) but lets it draw unless `opaque` is set. Draw worlds with `world:draw()` and UI with `fe.canvas` / `fe.ui`.

### Pause and time

`fe.game` drives every loaded world's `paused` and `time_scale`. `game.pause()` pauses every world, including worlds loaded meanwhile, until `game.resume()`, and stops playtime; `game.pause(world)` pauses one world. Both return a release function, and a pause taken in a task ends with that task at the latest. `game.paused` is true while every world is paused. Scenes, UI and tasks that are not bound to a paused world keep running, so pause screens work. The field's main menu pauses the field world, so sprites, particles and field scripts stand still behind it. `game.time_scale` multiplies gameplay time and every world's time scale, once: world-bound tasks follow their world and other tasks game time; `game.hit_stop(seconds, scale, world?)` slows one world (or all) for a moment (scale 0 freezes it), and `game.scale_time(key, scale, world?)` holds a named scale until it is set to nil. Held scales and hit-stops multiply in the order they were first set. A world's values are written only when the game's value changes, so a game can still set `world.paused` by hand.

`fe.ui` also has toasts, `ui.toast(text, {icon, duration, sound})`, which queue without waiting and show a few at a time over everything, and button glyphs: `ui.prompt(action)` is markup with the glyph of the action's button for the device in use (`fe.input.device_type()`: keyboard, Xbox, PlayStation or Switch; Steam Deck and other pads use the Xbox set). The placeholder glyph sheets live in `@fe/assets/ui/glyphs_*.png` and are registered as `{icon:glyph.<set>.<button>}`; replace `ui.glyph_sets` to ship your own.

Screens follow one pattern: `screen.open(...)` runs in a task, shows its view with `ui.run_screen(view)` and returns a result, or nil when the player backs out with Cancel (`save.open` returns a slot, `name_entry.open` a name, `worldmap.open` a place, `story.open` a protagonist, `ui.choose` an index and item). The view finishes with `ui.close(view, result)` and is removed however the task ends. `ui.with_view(panel, fn, ...)` shows a side panel while `fn` runs, and `ui.message(text)` waits for the player to read a line.

The save screen shows each slot's thumbnail (`fe.save.thumbnail_path`) and when it was saved in local time (`fe.time.local_date`, formatted by the `fe.save.date` string). Name entry takes typed text and IME composition (`fe.input.text_begin`) and offers its letter grid for gamepads; its length limit counts characters.

## State and saves

`fe.state` holds flags, variables and kit sections such as the party, inventory and quests; it is what saves contain. Everything in it must survive JSON: booleans, finite numbers, strings, and plain tables that are either lists without holes or string-keyed records. `state.set_var` raises with the offending path as soon as a value breaks this, `state.set_flag` takes booleans, and `save.write` checks whole sections the same way (the save screen reports a failure instead of freezing). `fe.save.write(slot, {meta = ...})` and `fe.save.load(slot)` save and restore everything. `save.add_meta(fn)` adds metadata for the save screen (location, party, playtime), and `save.version` with migrations upgrades old saves. Sections in a save that no kit has registered are carried along and written back, but never become defaults for a new game. Keep live game state in `fe.state`; module state also survives hot reload under the rules in [architecture.md](architecture.md#hot-reload).

`state.changed` fires as `(kind, name, value)` when something event pages can read changes: `flag` and `var` (name and new value), `reload` (after `restore` or `reset`), `quest` (quest id and what changed: `active`, `completed`, `failed`, `stage` or `objective`), `hour` (nil and the new hour) and `story` (protagonist id and `recruited`, `started` or `finished`). Setting a flag to its current value fires nothing. Playtime is kept by `fe.game` (`game.playtime()`) without firing it.

Saves, settings and `fe.json.encode` store Lua tables as JSON. Sequences become arrays and other tables objects; integer keys are written as `"#<n>"` (and a string key that starts with `#` gets a second `#`), so sparse arrays such as `{[1] = a, [3] = c}` and mixed tables come back with the same keys. Every save and settings write is flushed to disk before it atomically replaces the old file, and the previous version is kept as `<file>.bak`. A save slot reads its `.bak` only when the slot's file is missing or cut short (empty, or JSON that ends early, as an interrupted write leaves it); `fe.save.read` then logs a warning and returns `true` as a third result, and `fe.save.list` marks the slot `backup = true`. A slot file that is complete but unreadable (invalid JSON, nested too deep, not a save) raises an error instead of quietly loading the previous save. The settings file falls back to its `.bak` whenever it is unreadable. `fe.save.write` raises, writing nothing, for data it could not read back: tables nested deeper than 127 levels (the save file wraps the data in one more) and values `fe.json.encode` rejects. Slot names are 1-32 lowercase letters, digits, `_` or `-`, excluding names Windows reserves (`con`, `nul`, `com1`...).

`fe.save.thumbnail(slot)` captures the next frame as `<user data>/thumbnails/<slot>.png`, and `fe.save.thumbnail_path(slot)` returns it as `"user://thumbnails/<slot>.png"` (nil until it exists), a path `fe.canvas.image` draws. `user://` images are read only, resolve only inside the user-data folder, and reload when the file changes, so a slot overwritten while its thumbnail is on screen shows the new picture. Deleting a slot deletes its thumbnail.

## Field and events

`fe.field.start(map, point)` loads a map and spawns its placements by type: npc, chest, door, sign, light, emitter, prop, sound, save_point and symbol. Add your own types with `field.define(type, spawner, fields)`; the fields describe props for the editor. `field.zone(type, handler)` reacts to zones and `field.routine(name, fn)` adds NPC routines next to `wander` and `patrol`. Event scripts and interactions run under one task per map (`event.scope`), so when a new map starts they are cancelled together with every task they started, including tasks that outlived their script; a script of the old map that called `start` (directly or through `field.warp`) carries on as a script of the new map.

The player acts on the entity in front of them through verbs. `talk` (Confirm) runs an entity's `interact` function or its active "action" page. Games add verbs for other inputs without touching the kit, which is how Wayfarers' Inquire and Purchase work:

```lua
field.verb("steal", {
  input = "info",                                    -- the action that triggers it
  when = function(npc) return npc:props().wares ~= nil end,
  label = "Steal",                                   -- shown when several verbs apply (or ask = true)
  run = function(npc, player) ... end,               -- runs in the interaction task
})
```

`field.command(id, {input, run, when?})` adds an action that needs no target, run in a task holding a field lock; Wayfarers' day/night switch is one. `field.target(input)` returns the entity and verbs the player would use (for HUD prompts), and `field.interact(input)` runs them. These, `field.touch`, `field.handle_events`, `field.update_routines`, `field.update_encounters`, `field.control` and the scene class `field.Scene` are module functions, so a game can replace any one of them.

Control is off while `field.lock()` is held (it returns the release function), while a blocking event runs, or while another scene is on top. Touch triggers, doors and random encounters only fire when no lock or blocking event is active, so a cutscene that walks the player through a door does not warp. `field.warp(map, point, options)` locks control, fades, loads (in the background with `fe.world.load_async`, showing a "Now loading" indicator after `settings.loading_delay` seconds; see `field.load_world`) and then autosaves (`settings.autosave` is true, false or `fn(map, point)`; `options.autosave` overrides it); the save waits until no event or lock is active, so it never captures the middle of a script. Random encounters measure the distance walked between field post-updates, after the world moved. `field.give` announces treasure with `field.announce`: a toast, or a message when `settings.pickup_toasts` is false.

NPC followers (Octopath's Allure and Guide) come from `fe.follower`: `follower.join_entity(npc, {assist = {enemy, skill, chance}})` makes an NPC walk in the field trail behind the party (`follower.max`, default 1, travel at once) and, with `assist`, joins the battle kit's companions (`fe.battle.assist`). Followers are saved; a following placement is not spawned on its map, and `follower.leave(id)` sends it home. `follower.joined` and `follower.left` fire on changes.

Placements run event pages. A page names a script module, a trigger (`action`, `touch`, `auto` or `parallel`) and `when` conditions; the last page whose conditions hold is active. Conditions are `flag`, `not_flag`, `var` (`{"gold": ">= 100"}`), `check` (`"module.function"`), `quest`, `quest_stage`, `objective` and `hours` (loaded from fe.quest and fe.daynight on first use), plus any added with `event.condition(key, fn)`. A condition that raises is reported with the page and entity and counts as false. A `parallel` page runs in its own task next to the entity's other scripts, so the player can still talk to the entity, and it is cancelled when its page stops being active. When its own script turns the page off (by setting a flag, say), the change finishes first, every page is refreshed and every listener hears it; the loop ends after that run, and a run still waiting is cancelled on the next tick. `event.matches` and `event.compare` can be replaced. Scripts return a function that receives `ev` with `self`, `page`, `world` and `player`:

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

`cutscene.actor` wraps an entity with `walk_to`, `walk`, `face`, `anim`, `emote`, `jump_to`, `say`, `bark`, `hide` and `show`. `cutscene.play(fn)` locks field control and adds letterboxing; if the cutscene fails or is cancelled the bars and the lock are removed at once. The cutscene follows the field world (it freezes while the world is paused). Holding the "skip" action fast-forwards it: the world runs `cutscene.settings.fast_forward` times faster, and the cutscene's waits with it because they run on the world's time (the factor applies once; a cutscene with no world speeds up its own task instead). With `{skippable = true}`, the Menu button asks whether to skip; skipping fades out and runs the rest at once (waits end, walks and jumps arrive, lines go only to the backlog), so every state change still happens, the screen stays covered until the cutscene ends (a `field.warp` inside it does not uncover it), and a `dialogue.choice` ends the skip: the screen fades back in, the choice is asked and the scene plays on. `cutscene.parallel` runs several functions at once. `field.rig` is the camera rig (`pan_to`, `path`, `follow`, `release`); a scripted pan holds the camera until `release()` or the end of the task that started it.

The message box belongs to the task that opened it: it stays open between lines and closes with `dialogue.close()` or when that task ends. A line waits while another line is on screen, so lines from different tasks never overwrite each other. `dialogue.Box` and `dialogue.draw_box` can be replaced. A line's `voice = path` plays voice-over on the "voice" bus: it stops when the player advances, and auto-advance (`dialogue.auto` or the `auto_advance` setting) waits until it has finished. `blip = id` plays a typing sound instead. `dialogue.speaking(speaker, talking)` fires when someone starts and stops talking, and `mouth = {paths}` animates the portrait while they do. Holding "skip" shows lines at once and advances them quickly, cutting voice-over short. Every line goes to `dialogue.backlog`, which the "Log" menu page and the Info button (during dialogue) show with `fe.ui.screens.backlog`. Text markup supports `{color:name}`, `{icon:id}`, `{wait:s}` and `{speed:cps}` (a speed of 0, from `{speed:0}`, `say` options or the text speed setting, shows the rest at once), `{size:scale}`…`{/size}`, `{b}`…`{/b}` (the font's `bold` face, or a 1px double strike), `{shake}`/`{wave}` effects, `{ruby:base|reading}` furigana, `{t:key}` for localised strings, and any tag added with `fe.i18n.add_resolver` (the RPG kit adds `{player}`, `{leader}` and `{actor:id}`).

### Hooks

Kits offer four kinds of extension point: settings tables (`field.settings`, `dialogue.style`), registries (`field.define`, `field.verb`, `field.routine`, `event.condition`, `save.add_meta`, `i18n.add_resolver`), signals with any number of listeners (`field.entered`, `field.treasure`, `state.changed`, `quest.changed`, ...; a listener returning true marks the event handled and stops lower-priority ones) and module functions a game may replace (`field.on_encounter`, `field.on_save_point`, `field.follower_sprites`, ...). For example `field.give(item, count, money)` emits `field.treasure`; the field's default listener announces it (a toast) at the lowest priority, so a kit or game listener that handles the treasure replaces that announcement. Kits register listeners with a key (`signal:connect(fn, priority, key)`), so reloading a module replaces its listeners instead of adding more.

### Supporting kits

- **Quests** (`fe.quest`): stages with objectives (`{"id", "text", "count"}` per stage in `data/quests.json`). `quest.progress(id, objective, amount)`, `quest.complete_objective`, `quest.objectives(id)` and `quest.stage_done(id)` track them; progress is saved, starts over on `quest.advance`, and every change fires `quest.changed` and `state.changed`. An unknown quest, stage or objective id raises (`quest.advance` returns false only for a quest that is not active). The journal page lists the objectives.
- **Day and night** (`fe.daynight`): keyframes blend ambient, sun, fog, sky and `lut_blend` between a map's `post.lut` and `post.lut_blend_target` (or `daynight.luts = {day, night}` for every map). `daynight.start` and `daynight.set_hour` install it, after which the time is re-applied whenever the field enters a map, also with the clock stopped. The clock stands still while the field world is paused. `daynight.toggle()` is Octopath II's switch: it fades out, jumps to the other phase's hour (`daynight.phases`) and fades back in (a cancelled switch uncovers the screen).
- **Stories** (`fe.story`): protagonists with chapters (`story.define` or data/story.json). A chapter has a title, optional map, point and script, a recommended level and requirements (party members, flags, other protagonists' chapters). `story.recruit`, `story.begin(id)` (the protagonist leads the party through `story.lead_with`, the field warps and the script runs), `story.finish()` (the chapter being played) or `story.finish(id)` (another protagonist's next chapter), `story.available()` and the chapter select screen `story.open()` / `story.select()`. Progress is saved; pages can test `{"chapter": {"rook": 2}}` and `{"playing": "rook"}`. fe.story is the only record of chapters (a quest's `chapter` field only labels it in the journal). Party membership and leading use fe.rpg's party; games without it replace `story.has_member` and `story.lead_with`.
- **Settings** (`fe.config`): `config.set` and `config.bind` apply at once and `config.save()` writes the file only when something changed; the settings screen saves when it closes. Settings cover master, music, effects and voice volume, text speed, auto-advance, battle mode, text size (`text_scale`, which scales `ui.text` and list rows) and high contrast (`theme.use("high_contrast")`), the language (the player's choice, else fe.starter's `language` option, else the system language from `fe.app.locale()` when the game ships it, else English), fullscreen, window size, vsync and monitor (`fe.app.set_display`). Movement (`move_x`, `move_y`) follows the bindings of up, down, left and right.
- **Localisation** (`fe.i18n`): `i18n.t(key, args)` fills `{name}` placeholders (letters, digits, underscores). Kit UI text uses `fe.*` keys with English defaults registered through `i18n.defaults`, so a game rewords or translates it in `lang/<code>.json`.
- **Sound** (`fe.sound`): `sound.music`, `sound.play` and `sound.voice` look ids up in `data/audio.json` (`music`, `sfx`, `voice`) or take paths. Voice lines play on the `voice` bus and return a voice id for `sound.playing(voice)` and `sound.wait(voice, timeout)` (inside a task); while one is audible music is ducked by half, which `sound.set_ducking({music = 0.4, sfx = 0.2}, {attack, release})` or `sound.set_ducking(false)` changes. `sound.pause(true)` holds everything playing for a menu and `sound.muffle(true, {bus, cutoff, fade})` puts a bus behind a low-pass. Positional effects pass `{position = {x, y, z}, world = w}` to `sound.play`.
- **World map** (`fe.worldmap`): `worldmap.open()` returns the chosen place (and warps there); `worldmap.discover(id)` loads the definition when needed.

## RPG data model

`fe.rpg.stats.define{...}` and `stats.resources{...}` declare the stat block. The default is a classic HP/MP block. Wayfarers replaces MP with SP and adds BP, a battle-only boost resource:

```lua
stats.resources({
  hp = { max = "max_hp", name = "HP" },
  sp = { max = "max_sp", name = "SP" },
  bp = { max = 5, start = 1, name = "BP", battle_only = true },
})
```

Actors and enemies are combatants with stat modifiers from the class and secondary class, equipment, passives, battle buffs and states (`{stat, add | mul | set, priority?}`): additions, then multipliers, then `set`, where the highest `priority` wins and ties go to the later modifier (states count in sorted id order, so results never depend on table order). Passive skills marked `support = true` are support abilities: when the actor or its class has `support_slots`, only those equipped with `actor:equip_support(slot, id)` apply (class passives always do); without slots every known passive applies. Level-ups raise current HP/MP by the growth but never revive a knocked-out member. Battle buffs (`battler:buff(modifier, turns)` or the `buff` effect) stack per id up to `Battler.settings.buff_cap` (an application that adds no stack reports `capped`), are typed `buff`/`debuff`, and a buff gained during the bearer's own turn is not counted down at the end of that turn. All buff and debuff multipliers on one stat combine and stay within `Battler.settings.buff_limit` (x0.5 to x2 by default; `buff_limits[stat]` per stat). The party kit manages the roster and the active members. The inventory kit manages items, equipment slots and money. `fe.rpg.database.load()` reads the standard data tables; see [authoring](authoring.md).

`rpg.install()` (fe.starter calls it once; again does nothing) connects the kit to the field: a `field.treasure` listener puts found items and money in the inventory and announces them with `rpg.announce` (texts `fe.rpg.obtained_*` through `field.announce`: a toast with the item's icon, or a message when `field.settings.pickup_toasts` is false), page conditions `item` and `member`, the leader's sprite for the player unless the game set `field.settings.player_sprite`, party followers, save metadata and `{leader}` / `{actor:id}` markup.

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

Within a phase, hooks run by priority (higher first), then registration order. Any hook may set `ctx.cancelled`: the remaining phases and the handler are skipped and the result is `{cancelled = true}`. `apply` hooks run just before the effect's handler changes the target; `after` hooks see `ctx.result`. `effects.hook(phase, fn, priority, id)` with an `id` replaces an earlier hook with that id, so reloading a module does not stack its hooks; `battle:effect_hook(...)` (or a rule layer's `effect_hooks`) adds hooks that apply only to one battle and stop when it ends.

`preview = true` computes results without applying them, for forecasts and AI: damage and healing report the expected amount and `hit_chance`, without rolling or consuming the random stream. Without a `random` stream effects always land and never crit. `environmental = true` (state ticks, terrain hazards) always hits, never crits and skips positional bonuses.

## Battles

A battle is three replaceable parts:

- **Rules** (`fe.battle.core`): tables of hooks in *rule layers*, general to specific: `fe.battle.rules` (the game's), the caller's `rules`, the troop's `script`, kit layers (tactical objectives) and the battle's own `battle.rules`. A layer's `layers` list holds more layers, added right after it, so kits combine without overwriting each other's hooks. Notification hooks run in every layer, general first: `setup`, `on_start`, `on_round`, `order` (reorder a round), `on_turn_start`, `on_turn_end`, `before_action` (may cancel), `on_perform` (the action goes ahead: not cancelled and its cost paid), `after_action`, `on_knocked_out`, `on_revived`, `on_battler_added` (before presenters show the newcomer), `on_battler_removed`, `on_swap`, `on_end`, and `on_abort` (the battle stopped without finishing, see below). `outcome` is asked specific-first and the first answer wins (`false` means the battle goes on), so a troop's or objective's outcome composes with the game's. `usable(battle, b, skill)` returning `false` in any layer vetoes a skill: `battle:usable(b, skill)` (cost and vetoes) is what the menus, the AI and `battle:perform` check. Single-answer hooks come from the most specific layer that defines them: `player_controller`, `escape_chance`, `rewards`, `reaction` (a counter by a battler the action hit), `inventory` and `actions[kind]` for custom commands. A layer's `effect_hooks` are per-battle effect hooks. `Battle.settings` holds `attack_skill` (the skill behind Attack, AI fallback and follow-ups), `mutual_ko` (the outcome when both sides fall at once; `"defeat"` by default), and `inventory` and `party`, where item actions and rewards go. The core does not require the RPG kit: `fe.battle` points those two at the party's inventory and roster, and a battle without them has no items or experience.
- **Scheduler**: `atb` (classic active time battle, wait or active mode; gauges hold still while actions play), `round_order` (everyone chooses, then turns resolve by skill priority and `order_key`/speed; `preview` returns this round's remaining turns, or while the round's actions are being chosen its predicted order, and the predicted next round; `choose = "turn"` picks each action when its turn comes), `initiative` (CT-based, for tactics; `preview(battle, n, {battler, cost})` shows the order after a CT cost; a round ends once every living unit that was there when it began has had a turn, so units arriving mid-round start at 0 CT and count from the next round). In every scheduler a battler that cannot act (asleep, broken) still takes an upkeep-only turn, so its states run out; obstacles take no turns. Any table with `run(battle)` works; an optional `replace(battle, old, new)` hands a swapped-out battler's place in the order to its replacement.
- **Presentation**: `fe.battle.sideview` (a stage map, timelines from `data/timelines.json`, HUD with the turn preview, popups) or `fe.battle.tactical`. A presenter is a table of methods named after events (`start`, `round`, `turn`, `action`, `results`, `knocked_out`, `revived`, `battler_added`, `battler_removed`, `finish`, ...), plus an optional `abort(battle)` that tears down at once whatever it set up (see below). The core never touches entities; presenters follow events. Every knock-out and revival, whatever caused it, is presented exactly once (`battle:settle()`), and `battle:add` works mid-battle (reinforcements) in every scheduler and presenter; in tactics a battler added without a `cell` stands on the free cell nearest its summoner, else nearest its side's first unit (with neither, adding it is an error). Presenters skip sprites that are already gone (a summon dismissed as it falls). Kit text (commands, banners, rewards, tactics HUD and objectives) uses `fe.battle.*` and `fe.tactics.*` i18n keys; the side-view command menu is a `ui.Menu` shown with `ui.run_screen`.

**Octopath Traveler's turn flow** is `round_order.new{choose = "turn", jitter = 0.1}`: each round's speed order (with its random variation, rolled one round ahead) is known in advance, battlers choose their command when their turn comes, and the side-view HUD shows the rest of this round (the acting battler highlighted) and the next. The `order` hook runs again on the turns still to come before every turn, so a battler broken mid-round moves to the back at once.

**Line-up changes.** `battle:summon(spec, {turns?, owner?, controlled?})` adds a temporary, computer-controlled battler that leaves (event `dismissed`) after its turns, when knocked out or when its owner falls. `battle:add_reserve(spec)` adds a back-row battler (Octopath Traveler II); `battle:swap(front, back)` trades places, formation slot and turn (a swap during the leaver's turn hands it over), presents `battler_removed` / `battler_added` and keeps both battlers' state; only a living back-row battler can swap in, and `battle:partner(b)` is the living back-row battler sharing b's slot (nil while it is knocked out, so the side-view menu offers no Swap). With `battle.settings.back_row = true`, `fe.battle.start` puts the reserve members in the back row, and the side-view menu offers Swap.

**Multi-action enemies.** An enemy whose data has `actions` (an integer, or a function path `"battle.boss.actions"` called as `fn(battle, battler)` at each of its turns) takes that many actions in each turn, in every scheduler (a tactical turn stays one move and one action): the first is the one the scheduler hands over (chosen at round start with `choose = "round"`, or queued by ATB), the rest are decided one at a time, and the turn stops early once the battler cannot act or the battle is decided. Each action is presented (`action`, `results`) like any other. Wayfarers' Bandit Chief acts twice, three times below half HP.

**Mock battles.** `battle.mock(fn, options?)` runs `fn` (a battle) and then rolls back every `fe.state` section except those in `battle.settings.mock_keep` (`clock`, `daynight` and `field` by default): party and roster, actors' HP, MP, states and experience, inventory and money, flags, variables, quests, story, followers, companions, the bestiary and the game's own sections. Playtime, the time of day and the player's location carry on. It starts the roster recovered (`recover = false` keeps it as is), never calls `on_defeat`, and rolls back however `fn` ends: Triangle Strategy's mental mock battles. A tactical battle reverts its own terrain changes when it ends, mock or not.

**Aborted battles.** If a battle raises (a rule hook's error, say) or its task is cancelled before it finished, `fe.battle.start` aborts it however far it got, then lets the error propagate: `battle:abort()` stops its effect hooks, clears actors' battle-only resources and `battle_end` states as a normal end would, and runs every layer's `on_abort(battle)` (tactics revert their terrain there); then the presenter's `abort(battle)` removes its scene and sprites and unloads its battle world at once (side-view and tactics both do, and uncover a screen they covered). `battle.active` is cleared; there is no outcome, no `on_end`, no `finish` event and no `battle.finished` signal. Neither hook may wait. Inside `battle.mock` the state is then rolled back as usual.

```lua
local battle = require("fe.battle")
battle.settings.scheduler = function() return require("fe.battle.schedulers.round_order").new() end
battle.rules = require("battle.rules")
local outcome = battle.encounter("wolves")          -- inside a task: "victory", "defeat" or "escaped"
```

Battle-only resources (such as BP) reset to their start value when a battle starts and ends. Items with `use.consume = false` are not used up.

Enemy AI is a function an enemy names in data with a module path and function name (`"ai": "ai.goblin.think"` is `think` in `ai/goblin.lua`, called as `think(battle, me)` and returning an action); enemies without one use `fe.battle.ai.default`, a weighted chooser. `ai.choose` scores options for short custom AIs.

`fe.battle.break` is an optional rule layer with Octopath-style shields, weakness reveals, Break and Boost: enemies' `shield` points drop on weakness hits (`weak` elements and damage types), the hit that knocks an enemy out included (it reveals the weakness and takes the point, but the fallen do not break); at zero they gain the game's break state (`prevents_action`, 2 turns), take double damage and act last in the round; shields refill when the state wears off. Revealed weaknesses are remembered per enemy type in the saved `bestiary` section (`shields.known(enemy_id)`), so later battles show them from the start. Boost points are committed in the command menu and paid only when the action goes ahead (`on_perform`: after every `before_action` hook and the cost check, at most what the battler has then); each point paid repeats the attack skill's own hits once more (a 2-hit attack boosted by 2 hits six times), other actions gain power, and no points are gained in a round spent boosting. `on_break(battle, battler, breaker)` notifies rule layers. Wayfarers' `battle/rules.lua` uses it with `install_sideview()` for the boost input, shield markers and break banner.

More optional kits, each a rule layer. `kit.rules(extra)` adds the `extra` layer right after the kit's as a layer of its own (through `layers`), so `shields.rules(limit.rules({on_round = ...}))` runs every kit's hooks and the extra ones; nothing is overwritten:

- `fe.battle.limit`: a gauge resource (`settings.resource`, default `lp`) that fills when a party member is damaged, breaks an enemy or sees an ally fall, and pays for skills that cost it: Octopath's divine skills and latent powers. Wayfarers' Divine command lists such skills.
- `fe.battle.assist`: companions who are not party members (NPC followers). `assist.join(id, {enemy, skill, chance?, name?})` keeps one in the saved `assists` section; at the end of every round each acts with its chance, appearing on the party's side for its skill and leaving again (event `assist`). A game connects its field followers to `assist.join` / `assist.leave`.
- `fe.battle.tp`: Triangle Strategy's tactical points. `tp.declare({max, start, name?})` adds a battle-only `tp` resource to the game's resources; with `tp.rules()` every battler gains `settings.per_turn` (1) at the start of each of its turns, skills cost TP through `cost`, and skills tagged `quietus` can be used once per battle by each battler (`battle:usable` says no afterwards, so menus grey them out and the AI skips them). Event `tp_gained`. Ridgeline's arts cost TP and Cora has a quietus.
- `fe.battle.reactions.counter{melee_only?, facing?, chance?, requires?, when?, skill?}` returns a `reaction` hook: a battler damaged by an opponent's melee skill strikes back, optionally only with an ability, from the front or side on a grid, and with a chance from the battle RNG.

## Tactics

`fe.battle.tactical.start{...}` runs a grid battle on a map through `fe.battle.start`, so the game's rules, signals and `on_defeat` apply (`battle.settings.presentation = "fe.battle.tactical"` works too; troop members then need `position` cells):

```lua
local outcome = tactical.start({
  map = "ridge",
  deploy = { tag = "deploy", max = 4 },              -- players place units on tagged cells
  enemies = { { enemy = "captain", id = "captain", cell = { x = 12, z = 1 } } },
  win = { tactical.objectives.defeat("captain", "Captain Vey") },
  lose = { tactical.objectives.defeat_all("party"), tactical.objectives.protect("cora") },
})
```

Skills use `range` and `area` (`{shape, min, max, height}` with shapes diamond, square, cross, line, cone or ring, from the engine's `world:area`) and optional `los`; line and cone ranges cover all four directions because units turn to face their target. Height, facing and back attacks (facing lives on the battler), follow-up attacks, zones of control (a unit may enter a cell next to a foe but not move on), knockback with fall and collision damage, movement tag costs and CT costs are in `tactical.settings`. Follow-ups (pincers) come from allies straight behind the target; `follow_up_diagonal` adds the diagonal cells behind it, `follow_up_ranged` adds allies further back whose follow-up skill (one tagged `follow_up`, else Attack) reaches it, and `follow_up_max` limits how many join.

Obstacles (`obstacles = {{enemy, cell}}` or `tactical.add_obstacle`; enemy data `"obstacle": true`) have HP but no turns and belong to no side (an obstacle spec with a `side` is an error) and give no rewards: they block movement and line of sight, can be hit and pushed into but not pushed, exert no zone of control, and when destroyed leave no corpse and apply their data's `destroyed` terrain reaction to the cell (event `destroyed`). Reinforcements (`reinforcements = {...}` or `tactical.reinforce(battle, {enemy, cell, turn | when, id?, required?})`) arrive at the start of round `turn` or of the first turn `when(battle)` is true, on the nearest free cell, never once the battle is decided; a `required` one keeps `defeat_all` for its side from completing until it has arrived and fallen. `tactical.unit(battle, id)` finds a unit by id. Terrain reactions are data in `tactical.reactions`: fire burns grass for three rounds and may spread to adjacent grass each round (battle RNG), ice freezes water so it can be crossed and melting restores it, and thunder hurts every unit standing in the connected water once; a unit standing on ice when it melts is washed to the nearest free cell (event `pushed`) and loses `sink_damage` of its max HP. `tactical.hazards` hurt units starting a turn on burning ground. All terrain damage goes through `fe.effects`, so resistances apply; reactions happen only when the action was actually performed. Every cell change the kit makes (reactions, their timers, destroyed obstacles' `destroyed` reactions, and script changes made with `tactical.set_cell(battle, cell, changes)`) is remembered and reverted when the battle ends or is aborted, so a battle on the field world, or a mock battle, leaves the map as it was. Fallen units stay on their cell as corpses (several can lie on one cell: `tactical.corpses_at`, with `corpse_at` the latest) that `dead_ally` skills can target, the latest fallen ally per cell first, and stand up (on the nearest free cell) when revived; a unit removed from the battle leaves the board.

Movement uses `world:reachable` with the unit's `jump` stat as the climbing limit (`default_jump` without one). `tactical.settings.gap_jump = {distance, diagonal?}` lets units jump gaps (off by default) and `ladders = false` keeps them off the map's ladders (passed to the engine as `rules.ladders`). Reachable cells' paths keep the `jump` and `climb` marks; the presenter walks each step with `entity:move_to(nil, {path = {cell}})`, so marked steps are jumped or climbed by the engine (other steps up or down more than 0.6 are hopped), and the board scene turns the world's `jump_start`, `landed`, `climb_start` and `climb_end` events into `Presenter:motion(battler, kind)`, which plays `settings.motion_clips[kind]` when the sprite has that clip.

Deployment places the first `max` living roster members (`required` ones first); the player moves and swaps them, adds others on empty cells, and replaces or sends back units from the roster. During a turn the HUD previews the CT order after the current choice (with the choice's CT cost, the acting unit's next turns highlighted), forecasts show damage and hit chance, and after acting the unit chooses its facing. Objectives (`defeat_all`, `defeat`, `protect`, `reach`, `survive(rounds)`) form a rule layer whose `outcome` is checked before the game's. The `plan(battle, unit)` rule hook replaces the AI, which scores options by expected value (forecast times hit chance) with the player's range, line-of-sight and area rules. `auto = true` lets the AI play the party, which tests and replays use. On the field world the unit sprites are removed afterwards; a battle map is unloaded (an aborted battle does both at once, without the closing banner). The kit lives in `fe/battle/tactical/` (`grid`, `rules`, `terrain`, `objectives`, `ai`, `presenter`, `deploy`).

## Devices, display and the platform

- **Text entry.** `fe.input.text_begin(rect, kind)` opens the IME (its candidate window placed near `rect`, in canvas pixels) or an on-screen keyboard where there is one, such as Steam Deck's; `kind` is `"text"`, `"name"` or `"number"`. Each tick `fe.input.text()` is the text committed since the last tick and `fe.input.composition()` the unfinished IME text and cursor. Keys still arrive as raw inputs, so a name-entry screen should handle Backspace and Enter through `key:` inputs and ignore other actions on ticks that produced text. `fe.input.text_end()` closes it. Tests inject text with `fe.input.feed_text(text)` (and a composition with `fe.input.feed_text(text, cursor)`).
- **Prompts and rumble.** `fe.input.device_type()` is `"keyboard"` after keyboard or mouse use, otherwise the last gamepad's layout: `"xbox"`, `"playstation"`, `"switch"`, `"steamdeck"` or `"generic"`. `fe.input.pads()` lists connected gamepads by number with their layout and rumble support; `fe.input.rumble(low, high, seconds, pad?)` and `fe.input.rumble_triggers(left, right, seconds, pad?)` vibrate them.
- **Display.** `fe.app.display()` returns the fullscreen mode (`false`, `"borderless"` or `"exclusive"`), window size, vsync (`"on"`, `"off"`, `"adaptive"`), monitor and the lists of monitors and exclusive modes; `fe.app.set_display{...}` changes any of them after the frame. The engine doesn't remember them, so keep them in `fe.config` and apply them in `init`.
- **Time and language.** `fe.time.local_date(fe.time.wall())` gives the player's calendar date and time for save slots; `fe.app.locale()` the system language (`"en-US"` in tests and replays).
- **Store services.** `fe.platform.unlock(id)`, `fe.platform.unlocked(id)`, `fe.platform.set_stat(name, value)` and `fe.platform.stat(name)` go to the store backend a port installed, or to the user-data folder.
- **Loading screens.** `fe.world.load_async(name, map)` returns a ticket; poll `fe.world.load_progress(ticket)` from a task while drawing a loading indicator, then `fe.world.finish_load(ticket)` returns the `World`.

## Screen effects and viewports

`fe.screen.set` animates full-screen effects that cover worlds and UI alike: `fade`, `flash`, `letterbox`, `radial_blur` (with `radial_centre`), `chromatic` and `transition` (`fade`, `swirl`, `mosaic`, `wipe`, `iris`, `shatter`, driven by `progress`); `tint` multiplies only the 3D scenes. Per-world looks (bloom, depth of field, occlusion, distortion, chromatic aberration, grading, game post shaders) are `world:set_environment{post = {...}}` fields; see [authoring](authoring.md#maps).

A battle intro freezes the field and transitions to the battle rendering underneath:

```lua
local tween = require("fe.tween")
fe.screen.freeze()                         -- hold what is on screen now
start_battle()                             -- the live frame changes underneath
tween.run(0.5, function(t) fe.screen.set{transition = {kind = "shatter", progress = t}, radial_blur = 1 - t} end)
fe.screen.unfreeze()                       -- once the transition is done (run waits, inside a task)
fe.screen.set{transition = {kind = "none", progress = 0}, radial_blur = 0}
```

While frozen, a transition reveals the live frame instead of its colour; without a transition the held image stays. `world:draw` takes options for more than one view per frame: `rect = {x, y, w, h}` (canvas pixels) draws into part of the screen, `target = "name"` draws off-screen for `fe.canvas.image("rt:name", x, y, {w = ..., h = ...})` (a minimap, a 3D portrait), and `camera = {target, yaw, pitch, distance, fov}` overrides the world's camera for that draw. `fe.save.thumbnail(slot)` captures only the worlds of the next frame, without the menu on top.

## Determinism

Tests and replays run with a fixed seed, and the engine keeps everything it controls repeatable: `fe.random` streams, `math.random`, and the order of `pairs` over string and number keys. The order of `pairs` over a table keyed by objects (tables, functions, coroutines, entities and other engine objects) depends on memory addresses and changes from run to run, so wherever that order affects the outcome (who acts first, which target is hit, what is spawned), keep the objects in a list or sort the keys by a stable value such as an id or `entity.key`.

## Tests

`fengine_headless --test <game>` runs every `tests/*.lua` file. Each returns named functions that receive `t` (`ok`, `eq`, `near`, `error`, `tick`, `wait_until`). `t.wait_until(predicate, ticks, message, wall_seconds)` keeps ticking until the predicate holds and fails only once `ticks` ticks (default 600) and `wall_seconds` of real time (default 10) have both passed, so background world loads finish however fast the build runs. Tests run as tasks against the real game, so they can start a new game, run events and fight battles; tasks still running when a test ends are cancelled before the next one. See `samples/*/tests/game.lua`.
