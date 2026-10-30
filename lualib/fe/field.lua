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
--- Map placements are spawned by type. Built-in types (fe.field.placements):
--- npc, chest, door, sign, light, emitter, prop, sound, save_point, symbol,
--- hidden_item. Any of them can be replaced, and games add their own.
--- Placement props are documented with fe.schema.entity so the editor
--- shows the right fields.
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
---     `field.target`, `field.verbs_for`, `field.interact`,
---     `field.choose_verb`, `field.touch`, `field.retouch`,
---     `field.handle_events`, `field.update_routines`,
---     `field.update_followers`, `field.update_encounters`, `field.give`,
---     `field.announce`, `field.on_save_point`, `field.on_encounter`,
---     `field.follower_sprites`, `field.player_sprite`, `field.make_player`,
---     `field.load_world`, `field.spawn_placements`, `field.spawn_followers`,
---     `field.apply_pages`, `field.map_scripts`, `field.stop_map_tasks`,
---     `field.wants_autosave`;
---   * `field.Scene` is the scene class (`field.scene` its instance);
---   * the placement types live in fe.field.placements and the follower
---     trail in fe.field.followers; a game replaces either module by path.
---
--- Phases: `Scene:update` (before the world moves) handles control, event
--- pages and routines; `Scene:post_update` (after it moved) handles this
--- tick's world events, encounters, followers and the camera. Both keep
--- running while a message box or choice is on screen: the box does not
--- block the scenes under it, and the field gates its input instead
--- (`field.can_control` needs the field on top of the scene stack).
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
--- from fe.follower. Placements whose id is following are not spawned. A
--- party change (fe.rpg.party's `changed`, when that module is loaded)
--- re-reads the leader's sprite and respawns the trail.
---
--- Pickups: `field.give` announces treasure with `field.announce`, a toast
--- (`ui.toast`) unless `settings.pickup_toasts` is false, then a message.
---
--- Control: the player moves while `field.can_control()`. `field.lock()`
--- takes a lock (cutscenes, warps, verbs) and returns its release; a lock
--- taken in a task is released when that task ends at the latest. Touch
--- triggers, encounters, doors and "auto" event pages only fire while
--- `field.can_trigger()` (no locks, no blocking event). A touch that
--- happened under a lock fires once the lock lifts if the player still
--- stands on the entity (within `settings.touch_range`); touches on the
--- tick a map starts never do, so arriving on a door does not warp back.
---
--- Warps: one at a time. `field.warp` cancels a warp another task still
--- runs (a door's detached warp when a script warps), whose lock and
--- screen cover are removed by its cleanup.
---
--- Random encounters (fe.encounter, loaded on first use) are on while
--- `settings.encounters` is true; movement is measured between field
--- updates, so it does not depend on when the engine moves entities.
---
--- Hot reload: `__reload` adopts edited `settings` values the game did not
--- change at runtime (see fe.tablex.adopt).
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
    facing_dot = 0.45,      -- how squarely the player must face a target (cosine)
    touch_range = 0.8,      -- a suppressed touch fires later within this distance
    player_radius = 0.22,
    footstep_sound = "step",
    footstep_volume = 0.35,
    followers = 0,          -- party members trailing the leader
    follower_spacing = 0.85,
    follower_speed = 9,     -- followers' mover speed
    follower_settle = 0.02, -- distance under which a follower stands still
    trail_step = 0.1,       -- leader movement between trail samples
    trail_length = 200,     -- trail samples kept
    --- Autosave after map transitions (field.warp): true, false or
    --- fn(map_id, point) -> boolean. The save waits until no event or lock
    --- is active, so it never captures the middle of a script.
    autosave = true,
    encounters = true,      -- random encounters and encounter symbols
    player_sprite = nil,    -- string or function() -> sprite path
    --- Extra mover fields for the player (`blocked_tags`, `required_tags`,
    --- `fly`, `jump`...), merged over speed and radius by `field.make_player`.
    player_mover = nil,
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
  --- The default listener (lowest priority) announces it with
  --- `field.announce` (a toast by default).
  treasure = signal.new(),
  locks = 0,
}

--- The file's settings, for `__reload`.
field.file_defaults = { settings = tablex.deep_copy(field.settings) }

--- Adopt edited settings on hot reload (values the game changed at runtime
--- are kept).
function field.__reload(live, fresh)
  live.file_defaults.settings = tablex.adopt(live.settings, fresh.settings, live.file_defaults.settings)
end

i18n.defaults({ fe = { field = {
  empty = "It's empty.", found_item = "Found {item}.", found_items = "Found {item} x{count}.",
  found_money = "Found {money} money.", loading = "Now loading...",
} } })

local location = state.section("field", function() return {} end)

require("fe.save").add_meta(function(meta) meta.location = location.map_name end, "fe.field.location")

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

--- No lock and no blocking event: touches, doors, encounters and auto
--- pages may fire.
function field.can_trigger()
  return field.locks == 0 and not event.busy()
end

--- The player may move and interact.
function field.can_control()
  return field.can_trigger() and scene.top() == field.scene
end

-- Auto pages wait for field locks too (see fe.event).
event.auto_allowed = function() return field.can_trigger() end

--- The player's sprite path: `settings.player_sprite` (a path or a
--- function returning one; the RPG kit sets the leader's).
---@return string?
function field.player_sprite()
  local s = field.settings.player_sprite
  if type(s) == "function" then return s() end
  return s
end

--- Spawn the player for `field.start` on a loaded map: `point` is the spot
--- ({position, layer, facing}) and `sprite` the path `field.player_sprite()`
--- gave. The default is a sprite, a mover at `settings.walk_speed` with
--- the fields of `settings.player_mover` on top (`blocked_tags = {"water"}`
--- keeps the party off lakes the vehicle kit crosses) and a non-solid
--- collider. Replace it to give the player other components.
---@param world World
---@param point {position: table, layer?: integer, facing?: table}
---@param sprite? string
---@return Entity
function field.make_player(world, point, sprite)
  local mover = { speed = field.settings.walk_speed, radius = field.settings.player_radius }
  for key, value in pairs(field.settings.player_mover or {}) do mover[key] = value end
  return world:spawn({
    id = "player", position = point.position, layer = point.layer, facing = point.facing, tags = { "player" },
    sprite = { animation = sprite, move_clip = "walk", idle_clip = "idle" },
    mover = mover,
    collider = { radius = field.settings.player_radius, solid = false },
  })
end

--- Attach a placement's event pages (`props.pages`, or a single page from
--- `props.script` and `props.trigger`) to its entity; `defaults` describe
--- the entity without an active page (see fe.event.attach).
---@param entity Entity
---@param placement table
---@param defaults table
function field.apply_pages(entity, placement, defaults)
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
local zone_lookup = {}
local live = { last_position = nil, pending_autosave = nil, scripts = nil, warp = nil, settled = false,
  pending_touches = {} }

local function clear(t)
  for key in pairs(t) do t[key] = nil end
end

--- Spawn a world's placements by type (`field.types`), skipping ones that
--- are following the party; a spawner that raises is reported and skipped.
---@param world World
function field.spawn_placements(world)
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

-- Placement types and followers (see the header).
require("fe.field.placements").install(field)
require("fe.field.followers").install(field)

--- The task the current map's event scripts and interactions run under
--- (started when needed).
---@return Task
function field.map_scripts()
  if not (live.scripts and live.scripts:running()) then
    live.scripts = task.detached("map scripts", function()
      task.wait_until(function() return false end)
    end)
  end
  return live.scripts
end
event.scope = function() return field.map_scripts() end

--- Cancel the scripts and interactions of the current map. A script of the
--- map that is calling (an event script may warp and carry on) is moved out
--- first and returned.
---@return Task?
function field.stop_map_tasks()
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
  local carried = field.stop_map_tasks()
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
  field.player = field.make_player(world, spot, field.player_sprite())
  field.spawn_placements(world)
  field.spawn_followers(world, field.player.position)
  require("fe.field.followers").watch_party()
  clear(zone_lookup)
  for _, zone in ipairs(world:zones()) do zone_lookup[zone.id] = zone end
  live.last_position = nil
  live.settled = false
  live.pending_touches = {}
  field.rig = camera.rig(world, { follow = field.player })
  location.map = map_id
  location.map_name = world.map_name
  remember_location()
  local music = world:props().music
  if music and options.music ~= false then sound.music(music) end
  if field.settings.encounters then require("fe.encounter").reset() end
  if not scene.contains(field.scene) then scene.push(field.scene) end
  if carried then carried:set_parent(field.map_scripts()) end
  field.entered:emit(map_id, point)
  return world
end

--- Whether a warp to `map_id`/`point` autosaves: `options.autosave`, else
--- `settings.autosave` (a boolean or a function of map and point).
---@return boolean
function field.wants_autosave(map_id, point, options)
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
--- World. Call from a task. If the task is cancelled while waiting, the
--- load is cancelled (`fe.world.cancel_load`): the engine discards what a
--- worker is still parsing, keeps what it decoded in its asset cache (as it
--- keeps every loaded asset) and never creates the world.
---@param map_id string
---@return World
function field.load_world(map_id)
  local ticket = fe.world.load_async(field.settings.world_name, map_id)
  local started = task.time()
  local shown, ready = false, false
  task.ensure(function()
    task.wait_until(function()
      if not shown and task.time() - started >= field.settings.loading_delay then
        shown = true
        scene.push(loading_view)
      end
      return fe.world.load_progress(ticket) >= 1
    end)
    ready = true
  end, function()
    if shown then scene.remove(loading_view) end
    if not ready then fe.world.cancel_load(ticket) end
  end)
  return fe.world.finish_load(ticket)
end

-- Whether the current task is `job` or was started by it.
local function inside(job)
  local current = task.current()
  while current do
    if current == job then return true end
    current = current.parent
  end
  return false
end

--- Change maps with a transition, then autosave (see settings.autosave;
--- `options.autosave` overrides it). Call from a task. Control is locked
--- meanwhile; if the task is cancelled the lock and the screen cover are
--- removed. The map loads with `field.load_world` while the screen is
--- covered. In a skipping task (a skipped cutscene, which keeps the screen
--- covered until it ends) the warp leaves the screen alone. A warp another
--- task still runs is cancelled first: one warp owns the field at a time.
---@param options? {transition?: string, duration?: number, autosave?: boolean, music?: boolean}
function field.warp(map_id, point, options)
  options = options or {}
  local kind = options.transition or "fade"
  local duration = options.duration or 0.35
  local covered = task.skipping()
  local previous = live.warp
  if previous and previous:running() and not inside(previous) then previous:cancel() end
  local me = task.current()
  live.warp = me
  local unlock = field.lock()
  task.ensure(function()
    if not covered then transition.out(kind, duration) end
    field.start(map_id, point, { music = options.music, world = field.load_world(map_id) })
    if not covered then transition.into(kind, duration) end
  end, function()
    unlock()
    if not covered then transition.clear() end
    if live.warp == me then live.warp = nil end
  end)
  if field.wants_autosave(map_id, point, options) then field.autosave() end
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

--- The verbs of `input` that apply to an entity (their `when`), in
--- registration order.
---@return table[]
function field.verbs_for(entity, input, player)
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
--- unless the verb sets facing = false, the player must face it
--- (`settings.facing_dot`).
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
        for _, verb in ipairs(field.verbs_for(entity, input, player)) do
          if distance <= (verb.range or default_range) and (verb.facing == false or facing_dot > field.settings.facing_dot) then
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
  local job = field.map_scripts():spawn("interact:" .. target.id, function()
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
--- symbols react (only while `field.can_trigger()`; otherwise the touch is
--- kept for `field.retouch`, unless the map just started).
function field.touch(other)
  if not field.can_trigger() then
    if live.settled and not tablex.contains(live.pending_touches, other) then
      live.pending_touches[#live.pending_touches + 1] = other
    end
    return
  end
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

--- Fire the touches suppressed under a lock once triggers are allowed
--- again, for entities the player still stands on (`settings.touch_range`,
--- same layer).
function field.retouch()
  if not field.can_trigger() then return end
  if not live.settled then
    live.settled = true
    return
  end
  if #live.pending_touches == 0 then return end
  local pending = live.pending_touches
  live.pending_touches = {}
  local player = field.player
  for _, other in ipairs(pending) do
    if not field.can_trigger() then return end
    if other:valid() and other.layer == player.layer
        and vec.flat_distance(other.position, player.position) <= field.settings.touch_range then
      field.touch(other)
    end
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
      sound.play(field.settings.footstep_sound, { volume = field.settings.footstep_volume })
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
  field.retouch()
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
