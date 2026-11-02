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
--- This file holds the scene, map starts, locks and saved state. The rest
--- of the field is split into modules that install their functions on this
--- one table (`require("fe.field")` is the whole field); a game replaces a
--- function on fe.field, or a whole module by putting its own file at the
--- same path: fe.field.placements (the built-in types, `spawn_placements`,
--- `apply_pages`), fe.field.followers (the follower trail), fe.field.warp
--- (`warp`, `load_world` with its loading indicator, `autosave`),
--- fe.field.interaction (`control`, `verb`, `command`, `target`,
--- `interact`, `touch`, `retouch`, `run_command`, the "talk" verb) and
--- fe.field.hooks (`handle_events`: touches, zones, footsteps; routines,
--- encounters, `make_rig`, `give`, `announce`, `on_save_point`),
--- fe.field.traverse (the `traverse` placement and verb, `crouch` zones,
--- paired actions), fe.field.anchors (safe resume anchors, `no_save` zones,
--- `remember_location`) and fe.field.waypoint (objective markers across
--- maps, compass bearings, `hint` zones). Each file's header documents its
--- part.
---
--- Extending without forking a file: registries (`field.define`,
--- `field.zone`, `field.routine`, `field.verb`, `field.command`), signals
--- (`entered`, `interacted`, `hud`, `menu_requested`, `treasure`; many
--- listeners, return true to mark an event handled) and the module
--- functions above plus `field.player_sprite`, `field.make_player`,
--- `field.map_scripts` and `field.stop_map_tasks`, each replaceable;
--- `field.Scene` is the scene class (`field.scene` its instance).
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
--- Control: the player moves while `field.can_control()`. `field.lock()`
--- takes a lock (cutscenes, warps, verbs) and returns its release; a lock
--- taken in a task is released when that task ends at the latest. Touch
--- triggers, encounters, doors and "auto" event pages only fire while
--- `field.can_trigger()` (no locks, no blocking event).
---
--- Hot reload: `__reload` adopts edited `settings` values the game did not
--- change at runtime (see fe.tablex.adopt).
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
  --- Player speed caps by holder (crouch zones); `field.move_speed` uses
  --- the lowest.
  speed_caps = {},
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

i18n.defaults({ fe = { field = { empty = "It's empty." } } })

-------------------------------------------------------------------------------
-- Saved state

local location = state.section("field", function() return {} end)

require("fe.save").add_meta(function(meta) meta.location = location.map_name end, "fe.field.location")

--- Record exactly where the player stands in the saved state.
function field.exact_location()
  if not field.player or not field.player:valid() then return end
  local p = field.player.position
  location.x, location.z, location.layer = p.x, p.z, field.player.layer
  location.facing = field.player.facing
end

--- Record where the player resumes in the saved state (the field does this
--- every update; the autosave before it writes): `field.exact_location`,
--- or on a map with safe anchors the nearest valid one (fe.field.anchors
--- installs that version).
field.remember_location = field.exact_location

--- Reload the current map from the saved location (after loading a save).
function field.resume()
  if not location.map then return false end
  field.start(location.map, { x = location.x, z = location.z, layer = location.layer,
    facing = location.facing and { x = location.facing.x, z = location.facing.z } })
  return true
end

-------------------------------------------------------------------------------
-- Registries

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

-------------------------------------------------------------------------------
-- Locks

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

-------------------------------------------------------------------------------
-- The player and the map's entities

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

-- Lists the module keeps across hot reload are cleared in place, never
-- replaced: the state.changed listener below reads `entities`.
local entities = {}
local zone_lookup = {}

--- Entities spawned from placements on the current map
--- (`field.spawn_placements` fills it, clearing it in place).
function field.entities()
  return entities
end

--- The current map's zone with `id` (from `world:zones()` when the map
--- started), if any.
---@param id string
---@return table?
function field.map_zone(id)
  return zone_lookup[id]
end

-- What a map's free `props` may hold, for the editor's Map panel (games add
-- keys with schema.extend("map_props", {...}); unknown keys are kept).
require("fe.schema").define("map_props", {
  music = { type = { asset = "audio" }, optional = true, doc = "Music id (data/audio.json) or file played on the map" },
  battle_stage = { type = { map_id = true }, optional = true, doc = "Map used as the battle stage for fights here" },
  camera = { type = { record = {
    pitch = { type = "number", optional = true }, yaw = { type = "number", optional = true },
    distance = { type = "number", optional = true, min = 0 }, fov = { type = "number", optional = true, min = 1, max = 170 },
    orthographic = { type = "boolean", optional = true, doc = "Parallel projection" },
    ortho_height = { type = "number", optional = true, min = 0, doc = "World units shown vertically when orthographic" },
    min = { type = { list = "number" }, optional = true, doc = "Lower [x, z] bound of the camera target" },
    max = { type = { list = "number" }, optional = true, doc = "Upper [x, z] bound of the camera target" },
  } }, optional = true, doc = "The camera rig's defaults on this map" },
  daynight = { type = "boolean", optional = true, doc = "Run the day/night cycle here" },
  exits = { type = { map = "string" }, optional = true,
    doc = "Point on this map leading towards each other map (objective markers on other maps point there)" },
  anchors = { type = { list = "any" }, optional = true,
    doc = "Safe resume anchors: point names, or {point, when} records (see fe.field.anchors)" },
}, { doc = "A map's props.", open = true })

-- The modules of the header install their functions on this table.
require("fe.field.placements").install(field)
require("fe.field.followers").install(field)
require("fe.field.warp").install(field)
require("fe.field.interaction").install(field)
require("fe.field.hooks").install(field)
require("fe.field.traverse").install(field)
require("fe.field.anchors").install(field)
require("fe.field.waypoint").install(field)

-------------------------------------------------------------------------------
-- Map starts

-- Kept across hot reload: the current map's script task.
local live = { scripts = nil }

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
  require("fe.field.interaction").reset()
  require("fe.field.hooks").reset()
  require("fe.field.traverse").reset()
  for id in pairs(zone_lookup) do zone_lookup[id] = nil end
  for _, zone in ipairs(world:zones()) do zone_lookup[zone.id] = zone end
  field.rig = field.make_rig(world, field.player)
  location.map = map_id
  location.map_name = world.map_name
  field.remember_location()
  local music = world:props().music
  if music and options.music ~= false then sound.music(music) end
  if field.settings.encounters then require("fe.encounter").reset() end
  if not scene.contains(field.scene) then scene.push(field.scene) end
  if carried then carried:set_parent(field.map_scripts()) end
  field.entered:emit(map_id, point)
  return world
end

-------------------------------------------------------------------------------
-- Scene

local Scene = { name = "field", opaque = true }
Scene.__index = Scene
field.Scene = Scene
field.scene = setmetatable({}, Scene)

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
  field.remember_location()
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

return field
