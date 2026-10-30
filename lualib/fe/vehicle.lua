--- Vehicles the party boards on the field: Octopath II's canoe that only
--- moves over water, a ship, an airship that flies over everything.
---
--- ```lua
--- local vehicle = require("fe.vehicle")
--- vehicle.define("canoe", {sprite = "sprites/canoe.anim.json", required_tags = {"water"}, speed = 4})
--- vehicle.define("airship", {sprite = "sprites/airship.anim.json", fly = true, speed = 9, leave_search = 3})
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
--- The placement type `vehicle` (`props.vehicle = <definition id>`) spawns
--- the vehicle entity where it was last left on that map, not at the
--- placement, once the party has used it (and not at all while it is
--- recorded on another map or the party is aboard: then it comes with the
--- player). Boarding: the "Board" verb on Confirm within `board_range`, or
--- `vehicle.board(entity)`. Aboard, the player entity rides the vehicle
--- (`entity:attach`) hidden, the followers hide and stand still, and the
--- player's inputs drive the vehicle's mover through `field.control`
--- (wrapped: `vehicle.control` runs while aboard). Cancel, or the "Leave"
--- verb on Confirm, calls `vehicle.leave()`: the player steps onto the
--- nearest cell within `leave_search` that their own mover may stand on
--- (`vehicle.landing`), facing away from the vehicle, and the followers
--- return; with no shore near it shows `fe.vehicle.no_shore` as a toast
--- and returns false. `vehicle.current()` is the vehicle entity (and its
--- id) while aboard.
---
--- Saved: the `vehicle` state section keeps each used vehicle's map,
--- position, layer and facing and whether the party is aboard. When a map
--- starts (`field.entered`) with the party aboard a vehicle recorded on
--- that map, the vehicle spawns under the player and they stay aboard, so
--- a save made at sea resumes at sea; starting another map while aboard
--- leaves the vehicle where it was and the party on foot.
---
--- Random encounters at sea: a map whose `props.encounters_sea` names an
--- encounter table uses it while the party is aboard (`encounter.table_at`
--- is wrapped). Signals: `vehicle.boarded(id, entity)` and
--- `vehicle.left(id, entity, cell)`.
---
--- Hooks on other kits (`field.control`, `field.update_followers`,
--- `encounter.table_at`) are installed once by `vehicle.install` (called by
--- the first `define` and on hot reload) and dispatch to this module's
--- current functions, so a reload never wraps twice; a game that replaces
--- one of those functions afterwards calls `vehicle.install()` again.
local encounter = require("fe.encounter")
local field = require("fe.field")
local i18n = require("fe.i18n")
local signal = require("fe.signal")
local state = require("fe.state")
local tablex = require("fe.tablex")
local vec = require("fe.vec")

local vehicle = {
  --- Definitions by id (see `vehicle.define`).
  defs = {},
  settings = {
    board_range = 1.2,   -- the player boards from this close (world units)
    leave_search = 2,    -- landing cells are looked for this many cells around the vehicle
    speed = 4,           -- default vehicle speed
    radius = 0.3,        -- default mover radius
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

-- The vehicle entity the party is aboard (kept in place across reloads).
local live = { entity = nil }

-------------------------------------------------------------------------------
-- Definitions and placements

--- Define (or replace) a vehicle.
---@param id string
---@param def {sprite: string, required_tags?: string[], blocked_tags?: string[], fly?: boolean, speed?: number, run_speed?: number, radius?: number, board_range?: number, leave_search?: number}
---@return table
function vehicle.define(id, def)
  if type(def) ~= "table" or type(def.sprite) ~= "string" then
    error("vehicle.define('" .. tostring(id) .. "'): a definition needs a sprite", 2)
  end
  def = tablex.copy(def)
  def.id = id
  if def.fly == nil then def.fly = true end
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
--- definition's tags, speed and `fly`, and a non-solid collider. The
--- entity is tagged "vehicle" and its props carry `vehicle = id`.
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
      required_tags = def.required_tags, blocked_tags = def.blocked_tags },
    collider = { radius = math.max(setting(def, "radius"), 0.4), solid = false },
    props = { vehicle = id },
  })
  return entity
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

local function set_visible(entity, visible)
  if entity:valid() and entity:sprite() then entity:sprite({ visible = visible }) end
end

--- Board a vehicle entity with the player: the player rides it hidden, the
--- followers hide and stop, and the party's inputs drive it. Returns false
--- (doing nothing) when already aboard or without a player.
---@param entity Entity
---@return boolean
function vehicle.board(entity)
  local player = field.player
  if live.entity or not (player and player:valid()) or not entity:valid() then return false end
  local id = entity:props().vehicle
  vehicle.get(id)
  live.entity = entity
  record.aboard = id
  vehicle.remember(entity)
  if player:mover() then player:stop() end
  player:attach(entity)
  set_visible(player, false)
  for _, follower in ipairs(field.followers()) do
    if follower:valid() then
      if follower:mover() then follower:stop() end
      set_visible(follower, false)
    end
  end
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

--- Step off the vehicle onto `vehicle.landing`'s cell, facing away from
--- it; the followers come back behind the player. Returns false, after a
--- toast, when no shore is within reach (or nobody is aboard).
---@return boolean
function vehicle.leave()
  local entity, id = vehicle.current()
  local player = field.player
  if not entity or not (player and player:valid()) then return false end
  local cell = vehicle.landing(entity, player)
  if not cell then
    require("fe.ui").toast(i18n.t("fe.vehicle.no_shore"))
    return false
  end
  local from = entity.position
  live.entity = nil
  record.aboard = nil
  vehicle.remember(entity)
  player:detach()
  local centre = vec.cell_centre(cell)
  player:place(centre, cell.layer)
  local away = { x = centre.x - from.x, z = centre.z - from.z }
  if away.x ~= 0 or away.z ~= 0 then player:face(away) end
  set_visible(player, true)
  field.refresh_followers()
  vehicle.left:emit(id, entity, cell)
  return true
end

--- The party's inputs while aboard (`field.control` runs this instead of
--- its own control): movement relative to the camera at the vehicle's
--- speed (`run_speed` while running), the Confirm verbs (the vehicle's
--- "Leave"), `settings.leave_input`, field commands and the menu. The
--- vehicle's record follows it, so a save made at sea knows where it is.
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
    return def ~= nil and vec.flat_distance(entity.position, player.position) <= setting(def, "board_range")
  end,
  run = function(entity) vehicle.board(entity) end,
})

-- Leave: Confirm while aboard (the vehicle is the entity under the player).
field.verb("leave_vehicle", {
  input = "confirm",
  label = "fe.vehicle.leave",
  facing = false,
  range = 1,
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
  --- `encounter.table_at`: at sea, the map's `props.encounters_sea` table.
  table_at = function(base, world, position, layer)
    if vehicle.current() then
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

--- Wrap `field.control`, `field.update_followers` and `encounter.table_at`
--- unless this module's wrapper is already in place (then nothing changes).
function vehicle.install()
  local targets = { control = field, update_followers = field, table_at = encounter }
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
