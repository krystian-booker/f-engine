--- Vehicles the party boards on the field: Octopath II's canoe that only
--- moves over water, a ship, an airship that flies over everything, a
--- chocobo the party rides in plain sight. A mount is a vehicle with
--- `fly = false` and a visible rider: boarding, leaving, control and saves
--- are the same.
---
--- ```lua
--- local vehicle = require("fe.vehicle")
--- vehicle.define("canoe", {sprite = "sprites/canoe.anim.json", required_tags = {"water"}, speed = 4})
--- vehicle.define("airship", {sprite = "sprites/airship.anim.json", speed = 9, altitude = 3,
---   camera = {zoom = 2, pitch = 55, far = 600}, shadow = "sprites/blob.anim.json", encounters = false})
--- vehicle.define("chocobo", {sprite = "sprites/chocobo.anim.json", fly = false, speed = 6,
---   blocked_tags = {"water", "mountain"}, rider = {visible = true, offset = {x = 0, y = 0.35, z = 0}},
---   encounters = false, speed_on_tags = {forest = 0.6}, footstep = "chocobo_step"})
--- -- map.json: {"id": "boat", "type": "vehicle", "position": [12.5, 0, 8.5], "props": {"vehicle": "canoe"}}
--- ```
--- A definition names the sprite and the vehicle's mover: `required_tags`
--- (cells it may enter must carry every one; `{"water"}` for a boat; the
--- names must be in the map's tag list), `blocked_tags` (cells it may not
--- enter), `fly` (default true: unwalkable cells and height steps are no
--- obstacle, so a boat crosses water authored as unwalkable), `speed` and
--- `run_speed`, `radius`,
--- `board_range` (how close the player must stand to board, default
--- `settings.board_range`) and `leave_search` (how far from the vehicle a
--- landing cell may be, in cells, default `settings.leave_search`). The
--- party's own mover keeps them off the water: set
--- `field.settings.player_mover = {blocked_tags = {"water"}}` (or replace
--- `field.make_player`).
---
--- Flight and riding: `altitude` (world units above the ground, default 0)
--- lifts the vehicle while the party is aboard, rising and sinking at
--- `altitude_speed` units/second (default `settings.altitude_speed`); a
--- lifted body passes over colliders and bodies below its feet (the engine's
--- `mover.altitude`). `shadow` names a sprite drawn flat on the ground under
--- it (attached with `ground = true`). `rider` says how the player rides:
--- `visible` (default false: hidden inside the vehicle), `offset` (a Vec3
--- from the vehicle, `{x = 0, y = 0.35, z = 0}` seats them on a mount's
--- back), `clip` (a sprite clip to play while riding, restored after) and
--- `depth_bias` (default 0.02, so a visible rider draws over the mount).
--- `camera` (`{zoom?, pitch?, far?}`) is applied to the field's rig and the
--- world camera while aboard and restored after. `encounters` is `false`
--- for none while aboard, an encounter table id to use one, or nil for the
--- map's `props.encounters_sea` (else the usual lookup). `footstep` names
--- the sound the vehicle's own `footstep` animation events play (`false`:
--- none); the party's footsteps, touches and doors are quiet while aboard,
--- while zone handlers still run (music and camera zones apply from the
--- air or the water). `speed_on_tags` (`{forest = 0.6}`) multiplies the
--- speed by the entries of the tags of the cell the vehicle is on
--- (`vehicle.speed_factor`).
---
--- The placement type `vehicle` (`props.vehicle = <definition id>`) spawns
--- the vehicle entity where it was last left on that map, not at the
--- placement, once the party has used it (and not at all while it is
--- recorded on another map or the party is aboard: then it comes with the
--- player). Boarding: the "Board" verb on Confirm within `board_range`, or
--- `vehicle.board(entity)`. Aboard, the player entity rides the vehicle
--- (`entity:attach`), the followers hide and stand still (also those that
--- join while aboard), only verbs flagged `aboard = true` apply, and the
--- player's inputs drive the vehicle's mover through `field.control`
--- (wrapped: `vehicle.control` runs while aboard). Cancel, or the "Leave"
--- verb on Confirm, calls `vehicle.leave()`: the player steps onto the
--- nearest cell within `leave_search` that their own mover may stand on
--- (`vehicle.landing`), facing away from the vehicle, and the followers
--- return (`vehicle.disembark`); with no shore near it shows
--- `fe.vehicle.no_shore` as a toast and returns false. A vehicle in the
--- air first comes down (control locked, the party still aboard,
--- `vehicle.leave` already true) and the party steps off once it has
--- landed. `vehicle.current()` is the vehicle entity (and its id) while
--- aboard; `vehicle.aboard()` says whether anyone is.
---
--- Saved: the `vehicle` state section keeps each used vehicle's map,
--- position, layer and facing and whether the party is aboard. When a map
--- starts (`field.entered`) with the party aboard a vehicle recorded on
--- that map, the vehicle spawns under the player and they stay aboard, so
--- a save made at sea resumes at sea (and one made in the air takes off
--- again); starting another map while aboard leaves the vehicle where it
--- was and the party on foot.
---
--- Random encounters at sea: a map whose `props.encounters_sea` names an
--- encounter table uses it while the party is aboard (`encounter.table_at`
--- is wrapped) unless the definition's `encounters` says otherwise.
--- Signals: `vehicle.boarded(id, entity)` and `vehicle.left(id, entity, cell)`.
---
--- Hooks on other kits (`field.control`, `field.update_followers`,
--- `field.spawn_followers`, `field.verbs_for`, `field.touch`,
--- `field.handle_events`, `encounter.table_at`) are installed once by
--- `vehicle.install` (called by the first `define` and on hot reload) and
--- dispatch to this module's current functions (`vehicle.hooked`), so a
--- reload never wraps twice; a game that replaces one of those functions
--- afterwards calls `vehicle.install()` again.
local encounter = require("fe.encounter")
local field = require("fe.field")
local i18n = require("fe.i18n")
local signal = require("fe.signal")
local sound = require("fe.sound")
local state = require("fe.state")
local tablex = require("fe.tablex")
local task = require("fe.task")
local vec = require("fe.vec")

local vehicle = {
  --- Definitions by id (see `vehicle.define`).
  defs = {},
  settings = {
    board_range = 1.2,   -- the player boards from this close (world units)
    leave_search = 2,    -- landing cells are looked for this many cells around the vehicle
    speed = 4,           -- default vehicle speed
    radius = 0.3,        -- default mover radius
    altitude_speed = 3,  -- default rise and descent rate, units/second
    leave_input = "cancel", -- the input that leaves the vehicle (nil: only the Confirm verb)
  },
  --- (id, entity) when the party boards.
  boarded = signal.new(),
  --- (id, entity, cell) when the party steps off onto `cell`.
  left = signal.new(),
  --- The functions this module wrapped on other kits: name -> {base, wrapper}.
  hooks = {},
}

--- The file's settings, for `__reload`.
vehicle.file_defaults = { settings = tablex.deep_copy(vehicle.settings) }

i18n.defaults({ fe = { vehicle = {
  board = "Board", leave = "Leave", no_shore = "There's nowhere to land here.",
} } })

--- Saved: `vehicles[id] = {map, x, z, layer, facing = {x, z}}` and `aboard`
--- (the id the party rides, or nil).
local record = state.section("vehicle", function() return { vehicles = {} } end)

-- The vehicle entity the party is aboard (kept in place across reloads), what
-- boarding changed on the player and the camera (to restore on disembarking)
-- and the task bringing a flying vehicle down.
local live = { entity = nil, rider = nil, camera = nil, landing = nil }

-------------------------------------------------------------------------------
-- Definitions and placements

--- Define (or replace) a vehicle.
---@param id string
---@param def {sprite: string, required_tags?: string[], blocked_tags?: string[], fly?: boolean, speed?: number, run_speed?: number, radius?: number, board_range?: number, leave_search?: number, altitude?: number, altitude_speed?: number, rider?: {visible?: boolean, offset?: table, clip?: string, depth_bias?: number}, encounters?: boolean|string, camera?: {zoom?: number, pitch?: number, far?: number}, footstep?: boolean|string, shadow?: string, speed_on_tags?: table<string, number>}
---@return table
function vehicle.define(id, def)
  if type(def) ~= "table" or type(def.sprite) ~= "string" then
    error("vehicle.define('" .. tostring(id) .. "'): a definition needs a sprite", 2)
  end
  def = tablex.copy(def)
  def.id = id
  if def.fly == nil then def.fly = true end
  local rider = def.rider or {}
  def.rider = { visible = rider.visible == true, offset = rider.offset or { x = 0, y = 0, z = 0 },
    clip = rider.clip, depth_bias = rider.depth_bias or 0.02 }
  vehicle.defs[id] = def
  vehicle.install()
  return def
end

--- A vehicle's definition (raises for an unknown id).
---@param id string
---@return table
function vehicle.get(id)
  local def = vehicle.defs[id]
  if not def then error("no vehicle '" .. tostring(id) .. "' is defined", 2) end
  return def
end

local function setting(def, key)
  local value = def[key]
  if value == nil then value = vehicle.settings[key] end
  return value
end

--- Spawn a vehicle entity on `world`: a sprite, a mover with the
--- definition's tags, speed, `fly` and `altitude_speed`, and a non-solid
--- collider; with `def.shadow`, a flat sprite attached to the ground under
--- it (`entity:data().shadow`, destroyed with the vehicle by
--- `vehicle.destroy`). The entity is tagged "vehicle" and its props carry
--- `vehicle = id`.
---@param world World
---@param id string
---@param position table
---@param layer? integer
---@param facing? table
---@return Entity
function vehicle.spawn(world, id, position, layer, facing)
  local def = vehicle.get(id)
  local entity = world:spawn({
    id = "vehicle:" .. id, position = position, layer = layer or 0, facing = facing, tags = { "vehicle" },
    sprite = { animation = def.sprite, move_clip = "walk", idle_clip = "idle" },
    mover = { speed = setting(def, "speed"), radius = setting(def, "radius"), fly = def.fly,
      altitude_speed = setting(def, "altitude_speed"),
      required_tags = def.required_tags, blocked_tags = def.blocked_tags },
    collider = { radius = math.max(setting(def, "radius"), 0.4), solid = false },
    props = { vehicle = id },
  })
  if def.shadow then
    local shadow = world:spawn({ position = position, layer = layer or 0,
      sprite = { animation = def.shadow, mode = "flat", cast_shadow = false } })
    shadow:attach(entity, nil, { ground = true })
    entity:data().shadow = shadow
  end
  return entity
end

--- Destroy a vehicle entity and its shadow.
---@param entity Entity
function vehicle.destroy(entity)
  if not entity:valid() then return end
  local shadow = entity:data().shadow
  if shadow and shadow:valid() then shadow:destroy() end
  entity:destroy()
end

--- Remember where a vehicle entity is (its map, position, layer and facing).
---@param entity Entity
function vehicle.remember(entity)
  local id = entity:props().vehicle
  local p, f = entity.position, entity.facing
  record.vehicles[id] = { map = entity:world().map_id, x = p.x, z = p.z, layer = entity.layer, facing = { x = f.x, z = f.z } }
end

--- The saved whereabouts of a vehicle, or nil before it was used.
---@param id string
---@return {map: string, x: number, z: number, layer: integer, facing: table}?
function vehicle.saved(id)
  return record.vehicles[id]
end

--- The `vehicle` placement type: spawns `props.vehicle` where it was last
--- left on this map (its placement position before the party used it); nil
--- when it is recorded on another map or the party is aboard it (then it
--- arrives with the player, see `vehicle.restore`).
---@param world World
---@param p table
---@return Entity?
function vehicle.placement(world, p)
  local id = p.props.vehicle
  vehicle.get(id)
  if record.aboard == id then return nil end
  local saved = record.vehicles[id]
  if saved and saved.map ~= world.map_id then return nil end
  local entity
  if saved then
    entity = vehicle.spawn(world, id, { x = saved.x, z = saved.z }, saved.layer, saved.facing)
  else
    entity = vehicle.spawn(world, id, p.position, p.layer, p.facing)
  end
  entity:data().placement = p.id
  return entity
end

field.define("vehicle", function(world, p) return vehicle.placement(world, p) end, {
  vehicle = { type = "string", doc = "A vehicle.define id (fe.vehicle)" },
}, "A boat or other vehicle the party boards; it stays where it was left.")

--- The vehicle entity with `id` on the current field map, if any.
---@param id string
---@return Entity?
function vehicle.entity(id)
  if not field.world then return nil end
  for _, e in ipairs(field.world:query({ tag = "vehicle" })) do
    if e:props().vehicle == id then return e end
  end
  return nil
end

-------------------------------------------------------------------------------
-- Boarding and leaving

--- The vehicle entity the party is aboard and its id, or nil.
---@return Entity?, string?
function vehicle.current()
  local e = live.entity
  if e and e:valid() then return e, e:props().vehicle end
  live.entity = nil
  return nil
end

--- Whether the party is aboard a vehicle.
---@return boolean
function vehicle.aboard()
  return vehicle.current() ~= nil
end

local function set_visible(entity, visible)
  if entity:valid() and entity:sprite() then entity:sprite({ visible = visible }) end
end

-- The followers hide and stand still (boarding, and joining while aboard).
local function stow_followers()
  for _, follower in ipairs(field.followers()) do
    if follower:valid() then
      if follower:mover() then follower:stop() end
      set_visible(follower, false)
    end
  end
end

--- Apply a definition's `camera` to the field's rig (zoom, base pitch) and
--- the world camera (far plane), remembering what they were. The rig is
--- rebuilt per map and `vehicle.restore` boards again, so a map started
--- aboard captures that map's own defaults.
---@param def table
function vehicle.apply_camera(def)
  local rig, world = field.rig, field.world
  if not (rig and world) then return end
  local cam = world:camera()
  live.camera = { zoom = rig.zoom, pitch = rig.base.pitch, far = cam.far }
  local wanted = def.camera or {}
  if wanted.zoom then rig:set_zoom(wanted.zoom) end
  if wanted.pitch then rig.base.pitch = wanted.pitch end
  if wanted.far then cam.far = wanted.far end
end

--- Put the camera back the way `vehicle.apply_camera` found it.
function vehicle.restore_camera()
  local saved = live.camera
  live.camera = nil
  if not saved or not (field.rig and field.world) then return end
  field.rig:set_zoom(saved.zoom)
  field.rig.base.pitch = saved.pitch
  field.world:camera().far = saved.far
end

-- Seat the player on the vehicle the way the definition's `rider` says.
local function seat_rider(player, entity, def)
  local rider = def.rider
  player:attach(entity, rider.offset)
  local sprite = player:sprite()
  live.rider = sprite and { clip = sprite.clip, depth_bias = sprite.depth_bias } or nil
  set_visible(player, rider.visible)
  if sprite and rider.visible then player:sprite({ depth_bias = rider.depth_bias }) end
  if sprite and rider.clip then player:clip(rider.clip) end
end

-- Undo `seat_rider`: the player stands on their own feet again.
local function unseat_rider(player)
  player:detach()
  set_visible(player, true)
  local before = live.rider
  live.rider = nil
  if before and player:sprite() then
    player:sprite({ depth_bias = before.depth_bias })
    if before.clip and player:sprite().clip ~= before.clip then player:clip(before.clip) end
  end
end

--- Board a vehicle entity with the player: the player rides it (hidden, or
--- seated as the definition's `rider` says), the followers hide and stop,
--- the vehicle rises to its `altitude`, the camera takes the definition's
--- settings and the party's inputs drive it. Returns false (doing nothing)
--- when already aboard or without a player.
---@param entity Entity
---@return boolean
function vehicle.board(entity)
  local player = field.player
  if live.entity or not (player and player:valid()) or not entity:valid() then return false end
  local id = entity:props().vehicle
  local def = vehicle.get(id)
  live.entity = entity
  record.aboard = id
  vehicle.remember(entity)
  if player:mover() then player:stop() end
  seat_rider(player, entity, def)
  stow_followers()
  if (def.altitude or 0) > 0 then entity:mover({ altitude = def.altitude }) end
  vehicle.apply_camera(def)
  vehicle.boarded:emit(id, entity)
  return true
end

local function has_all(tags, wanted)
  for _, tag in ipairs(wanted or {}) do
    if not tablex.contains(tags, tag) then return false end
  end
  return true
end

local function has_any(tags, unwanted)
  for _, tag in ipairs(unwanted or {}) do
    if tablex.contains(tags, tag) then return true end
  end
  return false
end

--- The cell the player would step onto from `entity`: the nearest walkable
--- cell within `leave_search` cells (centre to vehicle, in world units) on
--- the vehicle's layer that the player's own mover may stand on (its
--- `blocked_tags`/`required_tags` and `can_occupy`), the cell ahead of the
--- vehicle winning ties. Nil when there is none.
---@param entity Entity
---@param player? Entity defaults to the field's player
---@return Cell?
function vehicle.landing(entity, player)
  player = player or field.player
  local world = entity:world()
  local def = vehicle.get(entity:props().vehicle)
  local search = setting(def, "leave_search")
  local origin = entity:cell()
  local position, facing = entity.position, entity.facing
  local mover = (player and player:valid() and player:mover()) or {}
  local best, best_score
  for _, c in ipairs(world:area(origin, "square", 1, math.max(1, math.ceil(search)))) do
    local info = world:cell(c)
    if info and info.walkable and not has_any(info.tags, mover.blocked_tags) and has_all(info.tags, mover.required_tags) then
      local centre = vec.cell_centre(c)
      local distance = vec.flat_distance(centre, position)
      if distance <= search then
        local ahead = ((centre.x - position.x) * facing.x + (centre.z - position.z) * facing.z) / math.max(distance, 0.01)
        local score = distance - ahead * 0.01
        if (not best or score < best_score) and (not player or player:can_occupy(centre, c.layer)) then
          best, best_score = c, score
        end
      end
    end
  end
  return best
end

--- Step off `entity` (the vehicle the party is aboard, with its id) onto
--- `cell`, facing away from it: the player stands on their own feet, the
--- camera goes back to what it was and the followers come back behind the
--- player. `vehicle.leave` calls this once the vehicle is on the ground.
---@param entity Entity
---@param id string
---@param cell Cell
function vehicle.disembark(entity, id, cell)
  local player = field.player
  local from = entity.position
  live.entity = nil
  live.landing = nil
  record.aboard = nil
  vehicle.remember(entity)
  unseat_rider(player)
  local centre = vec.cell_centre(cell)
  player:place(centre, cell.layer)
  local away = { x = centre.x - from.x, z = centre.z - from.z }
  if away.x ~= 0 or away.z ~= 0 then player:face(away) end
  vehicle.restore_camera()
  field.refresh_followers()
  vehicle.left:emit(id, entity, cell)
end

--- Leave the vehicle onto `vehicle.landing`'s cell. Returns false, after a
--- toast, when no shore is within reach (or nobody is aboard). A vehicle
--- in the air comes down first: control is locked, the party stays aboard
--- and steps off (`vehicle.disembark`) once its lift is gone; true is
--- returned at once, and again while the descent is under way.
---@return boolean
function vehicle.leave()
  local entity, id = vehicle.current()
  local player = field.player
  if not entity or not (player and player:valid()) then return false end
  if live.landing and live.landing:running() then return true end
  local cell = vehicle.landing(entity, player)
  if not cell then
    require("fe.ui").toast(i18n.t("fe.vehicle.no_shore"))
    return false
  end
  local mover = entity:mover()
  if mover and mover.lift > 0 then
    live.landing = task.detached("vehicle.land", function()
      field.lock()
      entity:mover({ altitude = 0 })
      task.wait_until(function()
        local m = entity:valid() and entity:mover()
        return not m or m.lift <= 0
      end)
      if vehicle.current() ~= entity or not (player:valid()) then return end
      vehicle.disembark(entity, id, vehicle.landing(entity, player) or cell)
    end)
    return true
  end
  vehicle.disembark(entity, id, cell)
  return true
end

--- The speed multiplier of a definition on `cell` (a `world:cell` table):
--- the product of its `speed_on_tags` entries for the cell's tags, 1 with
--- none (or no cell).
---@param def table
---@param cell table?
---@return number
function vehicle.speed_factor(def, cell)
  local factor = 1
  if def.speed_on_tags and cell then
    for _, tag in ipairs(cell.tags) do
      factor = factor * (def.speed_on_tags[tag] or 1)
    end
  end
  return factor
end

--- The party's inputs while aboard (`field.control` runs this instead of
--- its own control): movement relative to the camera at the vehicle's
--- speed (`run_speed` while running, times `vehicle.speed_factor` for the
--- cell it is on), the Confirm verbs (the vehicle's "Leave"),
--- `settings.leave_input`, field commands and the menu. The vehicle's
--- record follows it, so a save made at sea knows where it is.
function vehicle.control()
  local entity, id = vehicle.current()
  if not entity then return end
  local def = vehicle.get(id)
  local mx, my = fe.input.axis("move_x"), fe.input.axis("move_y")
  local length = math.sqrt(mx * mx + my * my)
  if length > 1 then mx, my = mx / length, my / length end
  if length > 0.05 then
    local yaw = math.rad(field.rig:yaw())
    local c, s = math.cos(yaw), math.sin(yaw)
    local speed = (fe.input.held("run") and def.run_speed) or setting(def, "speed")
    speed = speed * vehicle.speed_factor(def, entity:world():cell(entity:cell()))
    entity:drive({ x = (c * mx + s * my) * speed, z = (-s * mx + c * my) * speed })
  end
  vehicle.remember(entity)
  if fe.input.pressed("confirm") and field.interact("confirm") then return end
  local leave = vehicle.settings.leave_input
  if leave and fe.input.pressed(leave) then
    vehicle.leave()
    return
  end
  if field.run_command() then return end
  if fe.input.pressed("menu") then field.menu_requested:emit() end
end

-- Board: Confirm next to a vehicle (within its board_range, from any side).
field.verb("board", {
  input = "confirm",
  label = "fe.vehicle.board",
  facing = false,
  range = math.huge,
  when = function(entity, player)
    if live.entity or not entity:has_tag("vehicle") then return false end
    local def = vehicle.defs[entity:props().vehicle]
    return def ~= nil
      and vec.flat_distance_in(entity:world(), entity.position, player.position) <= setting(def, "board_range")
  end,
  run = function(entity) vehicle.board(entity) end,
})

-- Leave: Confirm while aboard (the vehicle is the entity under the player).
-- `aboard` marks the verbs that apply while riding (`vehicle.hooked.verbs_for`).
field.verb("leave_vehicle", {
  input = "confirm",
  label = "fe.vehicle.leave",
  facing = false,
  range = 1,
  aboard = true,
  when = function(entity) return live.entity ~= nil and entity == live.entity end,
  run = function() vehicle.leave() end,
})

-------------------------------------------------------------------------------
-- Map starts and saves

--- A map started: with the party aboard a vehicle recorded on this map,
--- spawn it under the player (at its recorded spot when the player stands
--- there) and board again; aboard a vehicle of another map, the party is
--- on foot and the vehicle stays where it was. `field.entered` calls this.
---@param map_id string
function vehicle.restore(map_id)
  live.entity = nil
  local id = record.aboard
  if not id then return end
  local saved = record.vehicles[id]
  local player = field.player
  if not (saved and saved.map == map_id and vehicle.defs[id] and player and player:valid()) then
    record.aboard = nil
    return
  end
  local p = player.position
  local position, layer = { x = p.x, z = p.z }, player.layer
  if vec.flat_distance(p, saved) < 1.5 then position, layer = { x = saved.x, z = saved.z }, saved.layer end
  local entity = vehicle.spawn(field.world, id, position, layer, saved.facing)
  table.insert(field.entities(), entity)
  vehicle.board(entity)
end

field.entered:connect(function(map_id) vehicle.restore(map_id) end, 100, "fe.vehicle.restore")

-- A loaded save or a new game: nobody is aboard until the field resumes.
state.reloaded:connect(function() live.entity = nil end, 0, "fe.vehicle.reload")

--- Adopt edited settings on hot reload and keep the hooks installed.
function vehicle.__reload(live_module, fresh)
  live_module.file_defaults.settings = tablex.adopt(live_module.settings, fresh.settings, live_module.file_defaults.settings)
  live_module.install()
end

-------------------------------------------------------------------------------
-- Hooks on the field and the encounter kit

--- What the wrapped functions do; each receives the function it replaced
--- first. Replaceable like any module function.
vehicle.hooked = {
  --- `field.control`: drive the vehicle while aboard.
  control = function(base, ...)
    if vehicle.current() then return vehicle.control() end
    return base(...)
  end,
  --- `field.update_followers`: the followers stand still while aboard.
  update_followers = function(base, ...)
    if vehicle.current() then return end
    return base(...)
  end,
  --- `field.spawn_followers`: followers spawned while aboard (a party
  --- change, someone joining) hide and stand still like the rest.
  spawn_followers = function(base, ...)
    base(...)
    if vehicle.current() then stow_followers() end
  end,
  --- `field.verbs_for`: aboard, only the verbs flagged `aboard` apply.
  verbs_for = function(base, entity, input, player)
    local verbs = base(entity, input, player)
    if not vehicle.current() then return verbs end
    local out = {}
    for _, verb in ipairs(verbs) do
      if verb.aboard then out[#out + 1] = verb end
    end
    return out
  end,
  --- `field.touch`: nothing is touched from aboard (doors, touch pages,
  --- encounter symbols; `field.retouch` goes through here too).
  touch = function(base, ...)
    if vehicle.current() then return end
    return base(...)
  end,
  --- `field.handle_events`: aboard, this tick's events (one pass; the
  --- queue is consumed) still run the player's zone handlers (music and
  --- camera zones apply from the air or the water) and play the vehicle's
  --- own `footstep` animation events with the definition's `footstep`
  --- sound; the party's contacts and footsteps wait until it is on foot again.
  handle_events = function(base, world)
    local entity, id = vehicle.current()
    if not entity then return base(world) end
    local step, player = vehicle.get(id).footstep, field.player
    for _, e in ipairs(world:events()) do
      if (e.type == "zone_enter" or e.type == "zone_exit") and e.a == player then
        local zone = field.map_zone(e.name)
        local handler = zone and field.zone_types[zone.type]
        if handler then handler(zone, e.type == "zone_enter", player) end
      elseif step and e.type == "anim_event" and e.name == "footstep" and e.a == entity then
        sound.play(step, { volume = field.settings.footstep_volume })
      end
    end
  end,
  --- `encounter.table_at`: aboard, the definition's `encounters` (false:
  --- none; a table id), else the map's `props.encounters_sea` table when it
  --- has one, else the usual lookup.
  table_at = function(base, world, position, layer)
    local entity, id = vehicle.current()
    if entity then
      local wanted = vehicle.get(id).encounters
      if wanted == false then return nil end
      if type(wanted) == "string" then return wanted, 1, {} end
      local sea = world:props().encounters_sea
      if sea then return sea, 1, {} end
    end
    return base(world, position, layer)
  end,
}

local function wrapper(name)
  return function(...)
    local v = require("fe.vehicle")
    return v.hooked[name](v.hooks[name].base, ...)
  end
end

--- Wrap `field.control`, `field.update_followers`, `field.spawn_followers`,
--- `field.verbs_for`, `field.touch`, `field.handle_events` and
--- `encounter.table_at` unless this module's wrapper is already in place
--- (then nothing changes).
function vehicle.install()
  local targets = { control = field, update_followers = field, spawn_followers = field, verbs_for = field,
    touch = field, handle_events = field, table_at = encounter }
  for name, module in pairs(targets) do
    local hook = vehicle.hooks[name]
    if not (hook and module[name] == hook.wrapper) then
      hook = { base = module[name], wrapper = wrapper(name) }
      vehicle.hooks[name] = hook
      module[name] = hook.wrapper
    end
  end
end

return vehicle
