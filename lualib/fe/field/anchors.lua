--- Safe resume anchors and save refusals on the field. fe.field installs
--- this module (`anchors.install(field)`): the `anchor` placement type,
--- the `no_save` zone type and `field.remember_location`; replace any of
--- them on fe.field, or this whole module with your own
--- `fe/field/anchors.lua`.
---
--- Anchors: on a map with anchors, the saved location (what `field.resume`
--- restores after loading) is the nearest valid anchor to the player
--- instead of the exact spot, so a save never resumes on a ladder, a
--- moving bridge or inside a set piece. An anchor is an `anchor`
--- placement (its position, layer and facing; `props.when` holds event
--- page conditions, so an anchor can be valid only after a bridge is
--- down) or an entry of the map's `props.anchors`: a point name or
--- `{point, when}`. Anchors on the player's layer are preferred. A map
--- without anchors stores the exact position (`field.exact_location`).
---
--- `no_save` zones: while the player stands inside one, saving is refused
--- with the zone's `reason` (an i18n key or text, default
--- "fe.field.no_save"). `anchors.can_save()` returns `ok, reason`; the
--- menu's default `allow_save` asks it, so the Save page is disabled and
--- shows the reason.
local event = require("fe.event")
local i18n = require("fe.i18n")
local state = require("fe.state")
local vec = require("fe.vec")

local anchors = {}

-- fe.field requires this module while it loads, so it is fetched when needed.
local function field()
  return require("fe.field")
end

i18n.defaults({ fe = { field = { no_save = "You can't save here." } } })

local location = state.section("field", function() return {} end)

local function valid(when, world)
  if not when then return true end
  local ok, result = pcall(event.matches, { when = when }, { world = world })
  return ok and result
end

-- Kept across hot reload: the anchors of the last world asked about
-- (placements and points do not change while a map runs).
local cache = { world = nil, candidates = nil }

local function candidates(world)
  if cache.world == world and cache.candidates then return cache.candidates end
  local out = {}
  for _, p in ipairs(world:placements()) do
    if p.type == "anchor" then
      out[#out + 1] = { id = p.id, position = p.position, layer = p.layer, facing = p.facing, when = p.props.when }
    end
  end
  local listed = world:props().anchors
  if type(listed) == "table" then
    local points = world:points()
    for _, entry in ipairs(listed) do
      local name = type(entry) == "table" and entry.point or entry
      local point = points[name]
      if point then
        out[#out + 1] = { id = name, position = point.position, layer = point.layer, facing = point.facing,
          when = type(entry) == "table" and entry.when or nil }
      end
    end
  end
  cache.world, cache.candidates = world, out
  return out
end

--- Forget the cached anchors (after the editor changed the map's anchors).
function anchors.forget()
  cache.world, cache.candidates = nil, nil
end

--- The valid anchors of a world: {id, position, layer, facing}.
---@param world World
---@return table[]
function anchors.list(world)
  local out = {}
  for _, anchor in ipairs(candidates(world)) do
    if valid(anchor.when, world) then out[#out + 1] = anchor end
  end
  return out
end

--- Whether a world has any anchors (valid or not).
---@param world World
---@return boolean
function anchors.has_any(world)
  return #candidates(world) > 0
end

--- The valid anchor nearest `position` on `world`, preferring `layer`.
---@param world World
---@param position table
---@param layer? integer
---@return table?
function anchors.nearest(world, position, layer)
  local best, best_score
  for _, anchor in ipairs(anchors.list(world)) do
    local score = vec.flat_distance_in(world, position, anchor.position)
    if layer and anchor.layer ~= layer then score = score + 1e6 end
    if not best or score < best_score then best, best_score = anchor, score end
  end
  return best
end

--- Record where the player resumes: the nearest valid anchor on a map
--- with anchors, else the exact position.
function anchors.remember_location()
  local f = field()
  local player = f.player
  if not player or not player:valid() or not f.world then return end
  local anchor = anchors.nearest(f.world, player.position, player.layer)
  if not anchor then return f.exact_location() end
  local p = anchor.position
  location.x, location.z, location.layer = p.x, p.z, anchor.layer
  location.facing = anchor.facing and { x = anchor.facing.x, z = anchor.facing.z } or player.facing
  location.anchor = anchor.id
end

--- Whether the player may save now: false and the reason inside a
--- `no_save` zone.
---@return boolean, string?
function anchors.can_save()
  local f = field()
  local player = f.player
  if not (f.world and player and player:valid()) then return true end
  for _, zone in ipairs(f.world:zones_at(player.position, player.layer)) do
    if zone.type == "no_save" then
      return false, i18n.t((zone.props or {}).reason or "fe.field.no_save")
    end
  end
  return true
end

-- What this module installed on fe.field, so a reload replaces its own
-- registrations and leaves a game's alone.
local installed = {}

--- Register the `anchor` placement type and the `no_save` zone type and
--- give `field` its `remember_location` (each unless the game replaced it).
---@param field table the fe.field module
function anchors.install(field)
  if not field.types.anchor or field.types.anchor == installed.anchor then
    installed.anchor = function() return nil end
    field.define("anchor", installed.anchor, {
      when = { type = "table", optional = true, doc = "Conditions (as on event pages) for the anchor to be valid" },
    }, "A safe spot a save resumes at (the nearest valid one on the map).")
  end
  if not field.zone_types.no_save or field.zone_types.no_save == installed.no_save then
    installed.no_save = function() end
    field.zone("no_save", installed.no_save, {
      reason = { type = "string", optional = true, doc = "Why saving is refused here (text or i18n key)" },
    }, "Saving is refused while the player is inside.")
  end
  if field.remember_location == nil or field.remember_location == field.exact_location
      or field.remember_location == installed.remember_location then
    field.remember_location = anchors.remember_location
    installed.remember_location = anchors.remember_location
  end
end

--- Install the fresh functions after a hot reload of this file.
function anchors.__reload(live_module)
  live_module.install(require("fe.field"))
end

return anchors
