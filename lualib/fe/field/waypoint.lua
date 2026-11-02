--- Where the current objective is: quest markers resolved on the field,
--- across maps, and compass bearings. fe.field installs this module
--- (`waypoint.install(field)`), which registers the `hint` zone type (see
--- fe.hint); replace `waypoint.target` to choose markers another way.
---
--- Markers: a quest stage or objective in data/quests.json may carry
--- `"marker": {"map": "town", "point": "gate"}` (see fe.quest).
--- `waypoint.target()` picks the marker of the first unfinished objective
--- of the first active quest that has one (main quests first), else that
--- quest stage's marker. `waypoint.current()` resolves it from the field's
--- map: `{map, point, position, via?, quest, objective?, hint?}`, where
--- `position` is the point's position when the marker is on this map, or,
--- when it is on another map, the position of this map's exit towards it
--- (`via` names that point). Exits are map props:
--- `"exits": {"town": "east_gate", "mine": "lift"}`; a marker several maps
--- away follows the exits of the maps between (read from their map files)
--- and `"*"` is an exit towards any map not listed. `position` is nil when
--- no route is known.
---
--- `waypoint.bearing(from, to, camera_yaw)` is the compass direction of `to`
--- seen from `from` on screen, in degrees (0 = up the screen, 90 = right),
--- and the ground distance; `camera_yaw` defaults to the field rig's.
---
--- `hint` zones: lingering inside for `props.seconds` (default
--- `fe.hint.settings.boundary`) shows `props.text` once per visit (a
--- nonmodal hint, see fe.hint).
local vec = require("fe.vec")

local waypoint = {}

-- fe.field requires this module while it loads, so it is fetched when needed.
local function field()
  return require("fe.field")
end

-- Kept across hot reload: exits of maps read from their files.
local exits_cache = {}

--- The marker the player is heading for: {map, point, quest, objective?,
--- hint?}, or nil. Replace to choose markers another way.
---@return table?
function waypoint.target()
  local quest = require("fe.quest")
  local active = {}
  for _, q in ipairs(quest.list("active")) do
    if q.def.kind == "main" then active[#active + 1] = q end
  end
  for _, q in ipairs(quest.list("active")) do
    if q.def.kind ~= "main" then active[#active + 1] = q end
  end
  for _, q in ipairs(active) do
    local id = q.def.id
    for _, objective in ipairs(quest.objectives(id)) do
      if not objective.done and objective.marker then
        return { map = objective.marker.map, point = objective.marker.point, quest = id, objective = objective.id,
          hint = objective.hint }
      end
    end
    local stage = quest.stage(id)
    if stage and stage.marker then
      local hint = stage.hint
      if not hint then
        for _, objective in ipairs(quest.objectives(id)) do
          if not objective.done and objective.hint then
            hint = objective.hint
            break
          end
        end
      end
      return { map = stage.marker.map, point = stage.marker.point, quest = id, hint = hint }
    end
  end
  return nil
end

--- The exits of a map ({[map_id] = point_name}): the field world's props
--- for the current map, else the map file's (read once).
---@param map_id string
---@return table
function waypoint.exits(map_id)
  local f = field()
  if f.world and f.world.map_id == map_id then return f.world:props().exits or {} end
  local cached = exits_cache[map_id]
  if cached then return cached end
  local path = "maps/" .. map_id .. "/map.json"
  local exits = {}
  if fe.fs.exists(path) then
    local ok, map = pcall(fe.json.read, path)
    if ok and type(map) == "table" and type(map.props) == "table" and type(map.props.exits) == "table" then
      exits = map.props.exits
    end
  end
  exits_cache[map_id] = exits
  return exits
end

--- Forget exits read from map files (after editing them).
function waypoint.forget()
  for key in pairs(exits_cache) do exits_cache[key] = nil end
end

--- The exit point on `from_map` leading towards `to_map` (searching the
--- exits of the maps between), or nil.
---@param from_map string
---@param to_map string
---@return string?
function waypoint.route(from_map, to_map)
  if from_map == to_map then return nil end
  local first = { [from_map] = false }
  local queue = { from_map }
  local head = 1
  while queue[head] do
    local map = queue[head]
    head = head + 1
    local exits = waypoint.exits(map)
    if exits[to_map] then return first[map] or exits[to_map] end
    local names = {}
    for target in pairs(exits) do
      if target ~= "*" then names[#names + 1] = target end
    end
    table.sort(names)
    for _, target in ipairs(names) do
      if first[target] == nil then
        first[target] = first[map] or exits[target]
        queue[#queue + 1] = target
      end
    end
  end
  return waypoint.exits(from_map)["*"]
end

--- Resolve a marker from the field's current map (see the header).
---@param marker table {map, point, ...}
---@return table
function waypoint.resolve(marker)
  local f = field()
  local out = {}
  for key, value in pairs(marker) do out[key] = value end
  if not f.world then return out end
  local points = f.world:points()
  if marker.map == f.world.map_id then
    local point = points[marker.point]
    out.position = point and point.position
    out.layer = point and point.layer
  else
    local via = waypoint.route(f.world.map_id, marker.map)
    local point = via and points[via]
    out.via = via
    out.position = point and point.position
    out.layer = point and point.layer
  end
  return out
end

--- The current objective's marker resolved on the field's map, or nil.
---@return table?
function waypoint.current()
  local marker = waypoint.target()
  if not marker then return nil end
  return waypoint.resolve(marker)
end

--- Compass direction of `to` from `from` on screen: degrees clockwise from
--- up the screen, and the ground distance. `camera_yaw` (degrees) defaults
--- to the field rig's yaw.
---@param from table
---@param to table
---@param camera_yaw? number
---@return number degrees, number distance
function waypoint.bearing(from, to, camera_yaw)
  if camera_yaw == nil then
    local f = field()
    camera_yaw = f.rig and f.rig:yaw() or 0
  end
  local dx, dz = to.x - from.x, to.z - from.z
  local f = field()
  if f.world then
    local d = f.world:wrap_delta(from, to)
    dx, dz = d.x, d.z
  end
  local yaw = math.rad(camera_yaw)
  local c, s = math.cos(yaw), math.sin(yaw)
  -- Screen up is world (-sin, -cos) and screen right (cos, -sin), the
  -- inverse of the field's camera-relative movement.
  local up = -s * dx - c * dz
  local right = c * dx - s * dz
  local degrees = math.deg(math.atan(right, up)) % 360
  if degrees > 360 - 1e-9 then degrees = 0 end
  return degrees, vec.flat_distance({ x = 0, z = 0 }, { x = dx, z = dz })
end

-- What this module installed on fe.field.
local installed = {}

--- Register the `hint` zone type on `field` (unless the game replaced it).
---@param field table the fe.field module
function waypoint.install(field)
  if not field.zone_types.hint or field.zone_types.hint == installed.hint then
    installed.hint = function(zone, entered)
      local hint = require("fe.hint")
      local props = zone.props or {}
      if entered then
        hint.boundary(zone.id, props.text, props.seconds)
      else
        hint.leave(zone.id)
      end
    end
    field.zone("hint", installed.hint, {
      text = { type = "string", doc = "Hint shown (text or i18n key)" },
      seconds = { type = "number", optional = true, min = 0, doc = "Seconds inside before it shows (default 20)" },
    }, "Shows a hint when the player lingers inside (a blocked path, a locked door).")
  end
end

--- Install the fresh registrations after a hot reload of this file.
function waypoint.__reload(live_module)
  live_module.install(require("fe.field"))
end

return waypoint
