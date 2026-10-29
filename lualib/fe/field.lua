--- The exploration scene: walk around a map, talk to people, open chests,
--- use doors, trigger events and random encounters.
---
--- ```lua
--- field.start("town", "entry")                 -- immediately
--- field.warp("cave", "entrance")               -- in a task, with a fade
--- field.define("statue", function(world, placement) ... return entity end)
--- field.zone("music", function(zone, entered) sound.music(zone.props.track) end)
--- field.verb("steal", {input = "info", label = "Steal", when = has_wares, run = steal})
--- ```
--- Map placements are spawned by type. Built-in types: npc, chest, door,
--- sign, light, emitter, prop, sound, save_point, symbol. Any of them can be
--- replaced, and games add their own. Placement props are documented with
--- fe.schema.entity so the editor shows the right fields.
---
--- Extending without forking this file:
---   * registries: `field.define` (placement types), `field.zone` (zone
---     handlers and their props for the editor), `field.routine` (NPC
---     routines), `field.verb` (what the player can do to the entity in
---     front of them, per input action), `field.command` (actions that need
---     no target, such as a day/night toggle);
---   * signals (many listeners; return true to mark an event handled):
---     `entered`, `interacted`, `hud`, `menu_requested`, `treasure`;
---   * module functions the scene calls, each replaceable: `field.control`,
---     `field.target`, `field.interact`, `field.choose_verb`, `field.touch`,
---     `field.handle_events`, `field.update_routines`,
---     `field.update_followers`, `field.update_encounters`, `field.give`,
---     `field.announce`, `field.on_save_point`, `field.on_encounter`,
---     `field.follower_sprites`, `field.load_world`;
---   * `field.Scene` is the scene class (`field.scene` its instance).
---
--- Phases: `Scene:update` (before the world moves) handles control, event
--- pages and routines; `Scene:post_update` (after it moved) handles this
--- tick's world events, encounters, followers and the camera.
---
--- Time: event pages, interactions and cutscenes run in tasks bound to the
--- field world (`task.bind`), so they freeze while it is paused (the main
--- menu pauses it) and follow its time scale. Event pages and interactions
--- start under one task per map (`event.scope`), so they and every task
--- they start (also tasks that outlive the script) are cancelled when
--- another map starts, except a script that changes maps itself.
---
--- Followers: the party's (`settings.followers`, sprites from
--- `field.follower_sprites`) walk the leader's trail, then NPC followers
--- from fe.follower. Placements whose id is following are not spawned.
---
--- Pickups: `field.give` announces treasure with `field.announce`, a toast
--- (`ui.toast`) unless `settings.pickup_toasts` is false, then a message.
---
--- Control: the player moves while `field.can_control()`. `field.lock()`
--- takes a lock (cutscenes, warps, verbs) and returns its release; a lock
--- taken in a task is released when that task ends at the latest. Touch
--- triggers, encounters and doors only fire while `field.can_trigger()`
--- (no locks, no blocking event).
---
--- Random encounters (fe.encounter, loaded on first use) are on while
--- `settings.encounters` is true; movement is measured between field
--- updates, so it does not depend on when the engine moves entities.
local camera = require("fe.camera")
local dialogue = require("fe.dialogue")
local event = require("fe.event")
local i18n = require("fe.i18n")
local scene = require("fe.scene")
local schema = require("fe.schema")
local signal = require("fe.signal")
local sound = require("fe.sound")
local state = require("fe.state")
local tablex = require("fe.tablex")
local task = require("fe.task")
local transition = require("fe.transition")
local vec = require("fe.vec")

local field = {
  world = nil,
  player = nil,
  rig = nil,
  settings = {
    world_name = "field",
    walk_speed = 3.2,
    run_speed = 5.6,
    interact_range = 1.4,
    counter_range = 2.3,
    player_radius = 0.22,
    footstep_sound = "step",
    followers = 0,          -- party members trailing the leader
    follower_spacing = 0.85,
    --- Autosave after map transitions (field.warp): true, false or
    --- fn(map_id, point) -> boolean. The save waits until no event or lock
    --- is active, so it never captures the middle of a script.
    autosave = true,
    encounters = true,      -- random encounters and encounter symbols
    player_sprite = nil,    -- string or function() -> sprite path
    pickup_toasts = true,   -- announce pickups with toasts instead of messages
    loading_delay = 0.3,    -- seconds of loading before the indicator shows
  },
  types = {},
  zone_types = {},
  routines = {},
  verbs = {},
  commands = {},
  entered = signal.new(),       -- (map_id, point_name)
  interacted = signal.new(),    -- (entity, input)
  hud = signal.new(),           -- draw overlays above the field
  menu_requested = signal.new(),
  --- (item, count, money) when treasure is found; return true when handled.
  --- The default listener (lowest priority) shows a message.
  treasure = signal.new(),
  locks = 0,
}

i18n.defaults({ fe = { field = {
  empty = "It's empty.", found_item = "Found {item}.", found_items = "Found {item} x{count}.",
  found_money = "Found {money} money.", loading = "Now loading...",
} } })

local location = state.section("field", function() return {} end)

local function remember_location()
  if not field.player or not field.player:valid() then return end
  local p = field.player.position
  location.x, location.z, location.layer = p.x, p.z, field.player.layer
  location.facing = field.player.facing
end

--- Placement spawner: fn(world, placement) -> entity or nil.
---@param type_name string
---@param spawner fun(world: World, placement: table): Entity?
---@param fields? table<string, any> props schema for the editor
---@param doc? string
function field.define(type_name, spawner, fields, doc)
  field.types[type_name] = spawner
  if fields then schema.entity(type_name, fields, doc) end
end

--- Zone handler: fn(zone, entered: boolean, entity) for the player.
--- `fields` describe the zone's props for the editor (see fe.schema.zone).
---@param type_name string
---@param handler fun(zone: table, entered: boolean, entity: Entity)
---@param fields? table<string, any>
---@param doc? string
function field.zone(type_name, handler, fields, doc)
  field.zone_types[type_name] = handler
  if fields then schema.zone(type_name, fields, doc) end
end

--- NPC routine: fn(entity, data, dt, random) runs each tick for idle
--- entities whose `data().routine` names it ("still" has none).
function field.routine(name, fn)
  field.routines[name] = fn
end

--- Register (or replace) a verb: something the player does to the entity
--- in front of them by pressing `spec.input` (default "confirm").
---   when(entity, player) -> boolean   whether it applies to an entity
---   run(entity, player)               runs in the interaction task
---   label                             string/i18n key or fn(entity), shown
---                                     when several verbs share an input
---   ask                               offer the choice even when this is
---                                     the only verb (to show its label)
---   range, facing                     reach (default settings ranges) and
---                                     whether the player must face it
---@param id string
---@param spec {input?: string, when?: fun(e: Entity, p: Entity): boolean, run: fun(e: Entity, p: Entity), label?: string|fun(e: Entity): string, ask?: boolean, range?: number, facing?: boolean}
function field.verb(id, spec)
  spec.id = id
  spec.input = spec.input or "confirm"
  for i, verb in ipairs(field.verbs) do
    if verb.id == id then
      field.verbs[i] = spec
      return spec
    end
  end
  field.verbs[#field.verbs + 1] = spec
  return spec
end

--- Register (or replace) a command: something the player does by pressing
--- `spec.input` while in control, with no target (a day/night toggle, a
--- skill like Octopath's path actions on the field). `run()` runs in a task
--- that holds a field lock; `when()` may refuse.
---@param id string
---@param spec {input: string, run: fun(), when?: fun(): boolean}
function field.command(id, spec)
  spec.id = id
  field.commands[id] = spec
  return spec
end

--- Remove a verb by id.
function field.remove_verb(id)
  for i, verb in ipairs(field.verbs) do
    if verb.id == id then
      table.remove(field.verbs, i)
      return
    end
  end
end

local locks = {}

--- Disable player control until the returned function is called (or the
--- calling task ends).
---@return fun()
function field.lock()
  local token = {}
  locks[token] = true
  field.locks = field.locks + 1
  return task.cleanup(function()
    if locks[token] then
      locks[token] = nil
      field.locks = field.locks - 1
    end
  end)
end

--- Drop every lock and event hold (new game, loading a save).
function field.reset()
  locks = {}
  field.locks = 0
  event.reset()
end

--- No lock and no blocking event: touches, doors and encounters may fire.
function field.can_trigger()
  return field.locks == 0 and not event.busy()
end

--- The player may move and interact.
function field.can_control()
  return field.can_trigger() and scene.top() == field.scene
end

local function player_sprite()
  local s = field.settings.player_sprite
  if type(s) == "function" then return s() end
  return s
end

local function apply_pages(entity, placement, defaults)
  local props = placement.props
  local pages = props.pages
  if not pages and props.script then
    pages = { { script = props.script, trigger = props.trigger or "action" } }
  end
  if pages then
    event.attach(entity, pages, defaults)
  end
end

-------------------------------------------------------------------------------
-- Built-in placement types

field.define("npc", function(world, p)
  local props = p.props
  local entity = world:spawn({
    id = p.id, position = p.position, layer = p.layer, facing = p.facing, tags = { "npc" },
    sprite = { animation = props.sprite, move_clip = "walk", idle_clip = "idle" },
    collider = { radius = props.radius or 0.3, solid = props.solid ~= false },
    mover = { speed = props.speed or 1.6 },
    props = props,
  })
  apply_pages(entity, p, { sprite = props.sprite, solid = props.solid })
  local d = entity:data()
  d.routine = props.routine
  d.home = p.position
  d.route = props.route
  return entity
end, {
  name = { type = "string", optional = true, doc = "Speaker name" },
  sprite = { type = "string", doc = "Sprite .anim.json" },
  script = { type = "string", optional = true, doc = "Event module for a single action page" },
  pages = { type = { list = "table" }, optional = true, doc = "Event pages" },
  routine = { type = "string", default = "still", doc = "still, wander, patrol or a field.routine name" },
  route = { type = { list = "table" }, optional = true, doc = "Patrol points [{x, z}]" },
  speed = { type = "number", optional = true },
  solid = { type = "boolean", optional = true },
}, "A person or creature with event pages.")

field.define("chest", function(world, p)
  local props = p.props
  local flag = props.flag or ("chest:" .. world.map_id .. ":" .. p.id)
  local opened = state.flag(flag)
  local entity = world:spawn({
    id = p.id, position = p.position, layer = p.layer, facing = p.facing, tags = { "chest" },
    sprite = { animation = props.sprite or "@fe/assets/ui/spark.anim.json", clip = opened and "open" or "closed" },
    collider = { radius = 0.35 }, props = props,
  })
  entity:data().interact = function()
    if state.flag(flag) then
      dialogue.say(nil, props.empty_text or i18n.t("fe.field.empty"))
      return
    end
    state.set_flag(flag)
    entity:clip("open")
    sound.play(props.sound or "chest")
    field.give(props.item, props.count or 1, props.money)
  end
  return entity
end, {
  item = { type = "string", optional = true }, count = { type = "integer", default = 1 },
  money = { type = "integer", optional = true }, sprite = { type = "string", optional = true },
  flag = { type = "string", optional = true, doc = "Defaults to chest:<map>:<id>" },
  empty_text = { type = "string", optional = true },
}, "A treasure chest opened once.")

field.define("door", function(world, p)
  local props = p.props
  local entity = world:spawn({
    id = p.id, position = p.position, layer = p.layer, tags = { "door" },
    collider = { radius = props.radius or 0.45, solid = false },
    sprite = props.sprite and { animation = props.sprite } or nil, props = props,
  })
  local function go()
    task.detached("warp", field.warp, props.map, props.point, { transition = props.transition })
  end
  if (props.trigger or "touch") == "touch" then
    entity:data().on_touch = go
  else
    entity:data().interact = go
  end
  return entity
end, {
  map = { type = "string" }, point = { type = "string" },
  trigger = { type = { enum = { "touch", "action" } }, default = "touch" },
  transition = { type = "string", optional = true }, sprite = { type = "string", optional = true },
}, "Moves the player to another map.")

field.define("sign", function(world, p)
  local entity = world:spawn({ id = p.id, position = p.position, layer = p.layer, collider = { radius = 0.3 },
    sprite = p.props.sprite and { animation = p.props.sprite } or nil, props = p.props })
  entity:data().interact = function() dialogue.say(nil, p.props.text) end
  return entity
end, { text = { type = "string" }, sprite = { type = "string", optional = true } }, "Shows a message.")

field.define("light", function(world, p)
  local light = {}
  for k, v in pairs(p.props) do light[k] = v end
  light.position = { 0, p.props.height or 0, 0 }
  return world:spawn({ id = p.id, position = p.position, layer = p.layer, keep_height = true, light = light })
end, { color = "table", radius = "number", intensity = "number", flicker = { type = "number", optional = true } },
  "A dynamic light (static lights belong in the map's light list).")

field.define("emitter", function(world, p)
  return world:spawn({ id = p.id, position = p.position, layer = p.layer, keep_height = true, emitter = p.props })
end, { sprite = { type = "string", optional = true }, rate = "number" }, "Particles (fire, smoke, sparkles).")

field.define("prop", function(world, p)
  local props = p.props
  local entity = world:spawn({
    id = p.id, position = p.position, layer = p.layer, yaw = props.yaw, scale = props.scale,
    mesh = { model = props.model, animation = props.animation, loop = props.loop ~= false, playing = props.animation ~= nil },
    collider = props.solid and { radius = props.radius or 0.45 } or nil, props = props,
  })
  apply_pages(entity, p, {})
  return entity
end, {
  model = { type = "string", doc = ".glb file" }, animation = { type = "string", optional = true },
  yaw = { type = "number", optional = true, doc = "degrees" }, solid = { type = "boolean", optional = true },
}, "A 3D object (doors, windmills, crates).")

field.define("sound", function(world, p)
  return world:spawn({ id = p.id, position = p.position, layer = p.layer, audio = p.props })
end, { sound = "string", radius = { type = "number", default = 8 } }, "Looping positional sound.")

field.define("save_point", function(world, p)
  local entity = world:spawn({ id = p.id, position = p.position, layer = p.layer, collider = { radius = 0.4 },
    sprite = { animation = p.props.sprite or "@fe/assets/ui/spark.anim.json", unlit = true }, props = p.props })
  entity:data().interact = function() field.on_save_point(entity) end
  return entity
end, { sprite = { type = "string", optional = true } }, "Opens the save screen.")

field.define("symbol", function(world, p)
  return require("fe.encounter").symbol(world, { troop = p.props.troop, position = p.position, sprite = p.props.sprite,
    sight = p.props.sight, speed = p.props.speed })
end, { troop = "string", sprite = "string", sight = { type = "number", default = 4 } }, "A visible enemy.")

-------------------------------------------------------------------------------
-- Built-in routines and verbs

field.routine("wander", function(entity, d, dt, rng)
  if entity:moving() then return end
  d.wait = (d.wait or rng:range(1, 3)) - dt
  if d.wait <= 0 then
    d.wait = rng:range(1.5, 4)
    local target = { x = d.home.x + rng:range(-2, 2), z = d.home.z + rng:range(-2, 2) }
    if entity:can_occupy(target) then entity:move_to(target) end
  end
end)

field.routine("patrol", function(entity, d)
  if not d.route or entity:moving() then return end
  d.leg = (d.leg or 0) % #d.route + 1
  local point = d.route[d.leg]
  entity:move_to({ x = point.x or point[1], z = point.z or point[2] })
end)

--- Talk: an entity's `data().interact` function, or its active "action"
--- event page (the entity turns to the player unless props.face_player is
--- false).
field.verb("talk", {
  input = "confirm",
  when = function(entity)
    if entity:data().interact then return true end
    local page = event.page(entity)
    return page ~= nil and (page.trigger or "action") == "action"
  end,
  run = function(entity, player)
    local d = entity:data()
    if d.interact then
      d.interact(entity)
      return
    end
    if entity:props().face_player ~= false then entity:face(player) end
    event.execute(entity, event.page(entity), { player = player })
  end,
})

-------------------------------------------------------------------------------
-- Hooks games replace

--- Give treasure: emits `field.treasure`; the default listener announces
--- what was found (the RPG kit adds the items to the inventory).
function field.give(item, count, money)
  field.treasure:emit(item, count or 1, money)
end

--- Tell the player about a pickup: a toast (`ui.toast`, with `options.icon`)
--- when `settings.pickup_toasts` is on, else a message (waits).
---@param text string markup
---@param options? {icon?: string, sound?: string}
function field.announce(text, options)
  if field.settings.pickup_toasts then
    require("fe.ui").toast(text, options)
  else
    dialogue.say(nil, text)
  end
end

field.treasure:connect(function(item, count, money)
  if item then
    local key = count > 1 and "fe.field.found_items" or "fe.field.found_item"
    field.announce(i18n.t(key, { item = item, count = count }))
  end
  if money then field.announce(i18n.t("fe.field.found_money", { money = money })) end
  return true
end, -1000, "fe.field.treasure")

--- Sprites for party followers (the RPG kit replaces this).
---@return string[]
function field.follower_sprites()
  return {}
end

--- A save point was used; default opens the save screen.
function field.on_save_point()
  require("fe.ui.screens.save").open("save")
end

--- A random encounter fired; default starts a side-view battle.
function field.on_encounter(troop, entry)
  local battle = require("fe.battle")
  battle.encounter(troop, { stage = entry and entry.stage or field.world:props().battle_stage })
end

-------------------------------------------------------------------------------
-- Scene

local Scene = { name = "field", opaque = true }
Scene.__index = Scene
field.Scene = Scene
field.scene = setmetatable({}, Scene)

-- Lists the module keeps across hot reload are cleared in place, never
-- replaced: the state.changed listener below reads `entities`.
local entities = {}
local trail = {}
local followers = {}
local zone_lookup = {}
local live = { last_position = nil, pending_autosave = nil, scripts = nil }

local function clear(t)
  for key in pairs(t) do t[key] = nil end
end

local function spawn_placements(world)
  clear(entities)
  local following = require("fe.follower")
  for _, p in ipairs(world:placements()) do
    local spawner = field.types[p.type]
    if spawner and not following.has(p.id) then
      local ok, entity = pcall(spawner, world, p)
      if not ok then
        fe.log.error("Placement '" .. p.id .. "' (" .. p.type .. "): " .. tostring(entity))
      elseif entity then
        entities[#entities + 1] = entity
      end
    elseif not spawner and p.type ~= "marker" then
      fe.log.warn("No field type '" .. p.type .. "' for placement " .. p.id)
    end
  end
end

local function spawn_followers(world, position)
  for _, entity in ipairs(followers) do
    if entity:valid() then entity:destroy() end
  end
  clear(followers)
  clear(trail)
  local sprites = {}
  local party = field.follower_sprites()
  for i = 1, math.min(field.settings.followers, #party) do sprites[#sprites + 1] = party[i] end
  for _, entry in ipairs(require("fe.follower").list()) do sprites[#sprites + 1] = entry.sprite end
  for i, sprite in ipairs(sprites) do
    followers[i] = world:spawn({ position = position, layer = field.player.layer, tags = { "follower" },
      sprite = { animation = sprite, move_clip = "walk", idle_clip = "idle" }, mover = { collide = false, speed = 9 } })
  end
end

--- Respawn the followers behind the player (after the party or the NPC
--- followers change; fe.follower changes do this by themselves).
function field.refresh_followers()
  if field.world and field.player and field.player:valid() then
    spawn_followers(field.world, field.player.position)
  end
end

--- The field's followers (party, then NPCs), nearest the leader first.
---@return Entity[]
function field.followers()
  return followers
end

-- The task the current map's event scripts and interactions run under.
local function map_scripts()
  if not (live.scripts and live.scripts:running()) then
    live.scripts = task.detached("map scripts", function()
      task.wait_until(function() return false end)
    end)
  end
  return live.scripts
end
event.scope = map_scripts

-- Cancel the scripts and interactions of the current map. A script of the
-- map that is calling (an event script may warp and carry on) is moved out
-- first and returned.
local function stop_map_tasks()
  local scripts = live.scripts
  live.scripts = nil
  if not (scripts and scripts:running()) then return nil end
  local caller = task.current()
  while caller and caller.parent ~= scripts do caller = caller.parent end
  if caller then caller:set_parent(nil) end
  scripts:cancel()
  return caller
end

--- Load a map and place the player at a named point (or position). Scripts
--- still running on the previous map are cancelled, with every task they
--- started; a calling script of that map carries on as a script of the new
--- one. `options.world` is that map already loaded (see `field.load_world`).
---@param map_id string
---@param point string|table point name or {x, z, layer?, facing?}
---@param options? {music?: boolean, world?: World}
function field.start(map_id, point, options)
  options = options or {}
  require("fe.save").add_meta(function(meta) meta.location = location.map_name end, "fe.field.location")
  local carried = stop_map_tasks()
  dialogue.clear_bubbles()
  local world = options.world or fe.world.load(field.settings.world_name, map_id)
  field.world = world
  local spot
  if type(point) == "table" then
    spot = { position = point, layer = point.layer or 0, facing = point.facing }
  else
    spot = world:points()[point or "start"]
    if not spot then error("map '" .. map_id .. "' has no point '" .. tostring(point) .. "'") end
  end
  field.player = world:spawn({
    id = "player", position = spot.position, layer = spot.layer, facing = spot.facing, tags = { "player" },
    sprite = { animation = player_sprite(), move_clip = "walk", idle_clip = "idle" },
    mover = { speed = field.settings.walk_speed, radius = field.settings.player_radius },
    collider = { radius = field.settings.player_radius, solid = false },
  })
  spawn_placements(world)
  spawn_followers(world, field.player.position)
  clear(zone_lookup)
  for _, zone in ipairs(world:zones()) do zone_lookup[zone.id] = zone end
  live.last_position = nil
  field.rig = camera.rig(world, { follow = field.player })
  location.map = map_id
  location.map_name = world.map_name
  remember_location()
  local music = world:props().music
  if music and options.music ~= false then sound.music(music) end
  if field.settings.encounters then require("fe.encounter").reset() end
  if not scene.contains(field.scene) then scene.push(field.scene) end
  if carried then carried:set_parent(map_scripts()) end
  field.entered:emit(map_id, point)
  return world
end

local function wants_autosave(map_id, point, options)
  if options.autosave ~= nil then return options.autosave end
  local setting = field.settings.autosave
  if type(setting) == "function" then return setting(map_id, point) == true end
  return setting == true
end

--- Write the autosave as soon as no event or lock is active (immediately
--- if none is). Returns the waiting task.
---@return Task
function field.autosave()
  if live.pending_autosave and live.pending_autosave:running() then return live.pending_autosave end
  live.pending_autosave = task.detached("autosave", function()
    task.wait_until(field.can_trigger)
    remember_location()
    require("fe.save").autosave()
  end)
  return live.pending_autosave
end

local loading_view = { name = "loading", blocks_update = false }

function loading_view.draw()
  local ui = require("fe.ui")
  local text = i18n.t("fe.field.loading")
  local w = ui.measure(text) + 24
  local x, y = ui.anchor(w, 18, "bottom-right")
  local dots = math.floor(task.time() * 4) % 4
  ui.text(text:gsub("%.+$", "") .. string.rep(".", dots), x + 12, y + 4, { color = ui.theme.muted })
end

--- Load a map for `field.start` without stopping the game: the map loads
--- on a worker (`fe.world.load_async`) while this task waits, showing a
--- loading indicator after `settings.loading_delay` seconds. Returns the
--- World. Call from a task.
---@param map_id string
---@return World
function field.load_world(map_id)
  local ticket = fe.world.load_async(field.settings.world_name, map_id)
  local started = task.time()
  local shown = false
  task.ensure(function()
    task.wait_until(function()
      if not shown and task.time() - started >= field.settings.loading_delay then
        shown = true
        scene.push(loading_view)
      end
      return fe.world.load_progress(ticket) >= 1
    end)
  end, function()
    if shown then scene.remove(loading_view) end
  end)
  return fe.world.finish_load(ticket)
end

--- Change maps with a transition, then autosave (see settings.autosave;
--- `options.autosave` overrides it). Call from a task. Control is locked
--- meanwhile; if the task is cancelled the lock and the screen cover are
--- removed. The map loads with `field.load_world` while the screen is
--- covered. In a skipping task (a skipped cutscene, which keeps the screen
--- covered until it ends) the warp leaves the screen alone.
---@param options? {transition?: string, duration?: number, autosave?: boolean, music?: boolean}
function field.warp(map_id, point, options)
  options = options or {}
  local kind = options.transition or "fade"
  local duration = options.duration or 0.35
  local covered = task.skipping()
  local unlock = field.lock()
  task.ensure(function()
    if not covered then transition.out(kind, duration) end
    field.start(map_id, point, { music = options.music, world = field.load_world(map_id) })
    if not covered then transition.into(kind, duration) end
  end, function()
    unlock()
    if not covered then transition.clear() end
  end)
  if wants_autosave(map_id, point, options) then field.autosave() end
end

--- Reload the current map from the saved location (after loading a save).
function field.resume()
  if not location.map then return false end
  field.start(location.map, { x = location.x, z = location.z, layer = location.layer,
    facing = location.facing and { x = location.facing.x, z = location.facing.z } })
  return true
end

-------------------------------------------------------------------------------
-- Interaction

local function verbs_for(entity, input, player)
  local out = {}
  for _, verb in ipairs(field.verbs) do
    if verb.input == input and (not verb.when or verb.when(entity, player)) then out[#out + 1] = verb end
  end
  return out
end

--- The entity the player would act on with `input` (default "confirm"),
--- and the verbs that apply to it; nil when there is none. The nearest
--- entity in reach wins; reach is the verb's range (default
--- settings.interact_range, or counter_range across a "counter" cell) and,
--- unless the verb sets facing = false, the player must face it.
---@return Entity?, table[]?
function field.target(input)
  input = input or "confirm"
  local player = field.player
  if not player or not player:valid() then return nil end
  local p, f = player.position, player.facing
  local ahead = field.world:cell({ x = math.floor(p.x + f.x), z = math.floor(p.z + f.z), layer = player.layer })
  local default_range = field.settings.interact_range
  if ahead and tablex.contains(ahead.tags, "counter") then default_range = field.settings.counter_range end
  local best, best_distance, best_verbs
  for _, entity in ipairs(entities) do
    if entity:valid() and entity ~= player and not event.running(entity) then
      local q = entity.position
      local distance = vec.flat_distance(p, q)
      if math.abs(q.y - p.y) < 1 and (not best or distance < best_distance) then
        local facing_dot = distance > 0.01 and ((q.x - p.x) * f.x + (q.z - p.z) * f.z) / distance or 1
        local reached = {}
        for _, verb in ipairs(verbs_for(entity, input, player)) do
          if distance <= (verb.range or default_range) and (verb.facing == false or facing_dot > 0.45) then
            reached[#reached + 1] = verb
          end
        end
        if #reached > 0 then best, best_distance, best_verbs = entity, distance, reached end
      end
    end
  end
  return best, best_verbs
end

--- Pick one of several verbs for an entity (default: a menu titled with
--- the entity's name). Returns the verb or nil when cancelled.
function field.choose_verb(entity, verbs)
  local ui = require("fe.ui")
  local entries = {}
  for i, verb in ipairs(verbs) do
    local label = verb.label
    if type(label) == "function" then label = label(entity) end
    entries[i] = { label = ui.t(label or verb.id), value = verb }
  end
  local _, chosen = ui.choose(entries, { title = entity:props().name, cancel = true, width = 140 })
  return chosen and chosen.value
end

--- Act on the target for `input`: runs the verb (asking with
--- `field.choose_verb` when several apply or the verb sets `ask`) in a task
--- of the map's scripts that holds a blocking event, so control returns
--- when it finishes. An entity running a script (other than a parallel
--- page) is not a target. Returns the task, or nil.
---@param input? string
---@return Task?
function field.interact(input)
  input = input or "confirm"
  local target, verbs = field.target(input)
  if not target then return nil end
  field.interacted:emit(target, input)
  local player = field.player
  local job = map_scripts():spawn("interact:" .. target.id, function()
    task.bind(field.world)
    event.block()
    local verb = verbs[1]
    if #verbs > 1 or verb.ask then verb = field.choose_verb(target, verbs) end
    if verb then verb.run(target, player) end
  end)
  if job:running() then target:data().running = job end
  return job
end

--- The player touched another entity: doors, touch pages and encounter
--- symbols react (only while `field.can_trigger()`).
function field.touch(other)
  if not field.can_trigger() then return end
  local d = other:data()
  if d.on_touch then
    d.on_touch(other)
    return
  end
  local page = event.page(other)
  if page and page.trigger == "touch" then
    event.run(other, page, { player = field.player })
  end
  if other:has_tag("encounter_symbol") and field.can_trigger() then
    local troop = other:props().troop
    other:destroy()
    field.start_encounter(troop, nil)
  end
end

--- Start a battle from the field in its own task, with control locked
--- until it ends.
---@return Task
function field.start_encounter(troop, entry)
  return task.detached("encounter", function()
    field.lock()
    field.on_encounter(troop, entry)
    if field.settings.encounters then require("fe.encounter").reset() end
  end)
end

--- Handle this tick's world events: contacts, zones, footsteps.
function field.handle_events(world)
  local player = field.player
  for _, e in ipairs(world:events()) do
    if e.type == "contact_begin" and (e.a == player or e.b == player) then
      field.touch(e.a == player and e.b or e.a)
    elseif (e.type == "zone_enter" or e.type == "zone_exit") and e.a == player then
      local zone = zone_lookup[e.name]
      local handler = zone and field.zone_types[zone.type]
      if handler then handler(zone, e.type == "zone_enter", player) end
    elseif e.type == "anim_event" and e.name == "footstep" and e.a == player then
      sound.play(field.settings.footstep_sound, { volume = 0.35 })
    end
  end
end

--- Run `field.routines` for idle entities (no script, interaction or
--- parallel page running).
function field.update_routines(dt)
  local rng = field.random or fe.random.new()
  field.random = rng
  for _, entity in ipairs(entities) do
    if entity:valid() then
      local d = entity:data()
      local routine = d.routine and field.routines[d.routine]
      if routine and not event.running(entity) and not (d.parallel and d.parallel:running()) then
        routine(entity, d, dt, rng)
      end
    end
  end
end

--- Move followers along the leader's trail.
function field.update_followers()
  if #followers == 0 then return end
  local p = field.player.position
  local last = trail[1]
  if not last or vec.flat_distance(last, p) > 0.1 then
    table.insert(trail, 1, { x = p.x, y = p.y, z = p.z, layer = field.player.layer })
    while #trail > 200 do table.remove(trail) end
  end
  local spacing = field.settings.follower_spacing
  for i, follower in ipairs(followers) do
    local index = math.min(#trail, math.floor(i * spacing / 0.1) + 1)
    local spot = trail[index]
    if spot and vec.flat_distance(spot, follower.position) > 0.02 then
      follower:move_to(spot, { path = false })
    end
  end
end

--- Encounter symbols and random encounters. Distance walked is measured
--- from the player's position at the previous field update.
function field.update_encounters(world, player)
  if not field.settings.encounters then return end
  local encounter = require("fe.encounter")
  encounter.update_symbols(world, player)
  local p = player.position
  local previous = live.last_position
  live.last_position = { x = p.x, z = p.z, layer = player.layer }
  if not previous or previous.layer ~= player.layer or not field.can_trigger() then return end
  local moved = vec.flat_distance(previous, p)
  if moved > 0 then
    local troop, entry = encounter.step(world, p, moved, player.layer)
    if troop then field.start_encounter(troop, entry) end
  end
end

--- Player input while `field.can_control()`: movement relative to the
--- camera, verbs (each registered input action) and the menu.
function field.control()
  local player = field.player
  local mx, my = fe.input.axis("move_x"), fe.input.axis("move_y")
  local length = math.sqrt(mx * mx + my * my)
  if length > 1 then mx, my = mx / length, my / length end
  if length > 0.05 then
    local yaw = math.rad(field.rig:yaw())
    local c, s = math.cos(yaw), math.sin(yaw)
    local speed = fe.input.held("run") and field.settings.run_speed or field.settings.walk_speed
    player:drive({ x = (c * mx + s * my) * speed, z = (-s * mx + c * my) * speed })
  end
  local tried = {}
  for _, verb in ipairs(field.verbs) do
    if not tried[verb.input] then
      tried[verb.input] = true
      if fe.input.pressed(verb.input) and field.interact(verb.input) then return end
    end
  end
  if field.run_command() then return end
  if fe.input.pressed("menu") then field.menu_requested:emit() end
end

--- Run the first command whose input was pressed (and whose `when`
--- allows it) in a task holding a field lock. Returns the task, or nil.
---@return Task?
function field.run_command()
  local ids = {}
  for id in pairs(field.commands) do ids[#ids + 1] = id end
  table.sort(ids)
  for _, id in ipairs(ids) do
    local command = field.commands[id]
    if fe.input.pressed(command.input) and (not command.when or command.when()) then
      return task.named("command:" .. id, function()
        field.lock()
        command.run()
      end)
    end
  end
  return nil
end

function Scene:update(dt)
  local world, player = field.world, field.player
  if not world or not player or not player:valid() then return end
  if field.can_control() then field.control() end
  event.tick(entities, { player = player })
  field.update_routines(dt)
end

function Scene:post_update(dt)
  local world, player = field.world, field.player
  if not world or not player or not player:valid() then return end
  field.handle_events(world)
  field.update_followers()
  field.update_encounters(world, player)
  remember_location()
  field.rig:update(dt)
end

function Scene:draw()
  if not field.world then return end
  field.world:draw()
  dialogue.draw_bubbles()
  field.hud:emit()
end

-- Keep event pages current as flags, variables and kit state change.
state.changed:connect(function()
  for _, entity in ipairs(entities) do event.refresh(entity) end
end, 0, "fe.field.pages")

local follower_signals = require("fe.follower")
follower_signals.joined:connect(function() field.refresh_followers() end, 0, "fe.field.followers")
follower_signals.left:connect(function() field.refresh_followers() end, 0, "fe.field.followers")

-- Development: saving the current map in the editor reloads it in place.
require("fe.game").file_changed:connect(function(path)
  if field.world and location.map and path == "maps/" .. location.map .. "/map.json" then
    local p = field.player and field.player:valid() and field.player.position
    local ok, err = pcall(field.start, location.map, p and { x = p.x, z = p.z, layer = field.player.layer } or "start",
      { music = false })
    if not ok then fe.log.error("Map reload failed: " .. tostring(err)) end
  end
end, 0, "fe.field.map_reload")

--- Entities spawned from placements on the current map.
function field.entities()
  return entities
end

return field
