--- Shared behaviour for anything with stats, resources and states: field
--- actors and battle participants. A class mixing this in provides:
---   self:base_stats()  -> table of base values
---   self:modifiers()   -> list of stat modifiers (equipment, passives...)
---   self.resources     -> table resource id -> current value
---   self.state_turns   -> table state id -> remaining turns (0 = lasting)
---
--- Computed stats are cached per combatant. Every mutator in the kit
--- (states here, equipment, class, level and supports on Actor, buffs on
--- Battler) calls `self:invalidate()`; a Battler sharing an Actor's states
--- invalidates both. Changes the kit cannot see (an edited
--- `actor:custom().modifiers` table, a changed Battler.settings limit)
--- need `combatant:invalidate()` or `Combatant.invalidate_all()`, which
--- data reloads and stat redefinitions (fe.rpg.stats) call through the
--- stats version.
local data = require("fe.data")
local stats = require("fe.rpg.stats")
local tablex = require("fe.tablex")

local Combatant = {}

--- The resource whose depletion means knocked out.
Combatant.life = "hp"

--- Bumped by Combatant.invalidate_all; every cache is keyed by it.
Combatant.epoch = 0

function Combatant.mixin(class)
  for key, value in pairs(Combatant) do
    if type(value) == "function" and key ~= "mixin" and key ~= "invalidate_all" and class[key] == nil then
      class[key] = value
    end
  end
end

--- Drop every combatant's cached stats (data reloads, redefined stats,
--- changed kit settings).
function Combatant.invalidate_all()
  Combatant.epoch = Combatant.epoch + 1
end

data.changed:connect(function() Combatant.invalidate_all() end, 0, "fe.rpg.combatant.cache")

--- Drop this combatant's cached stats (and its actor's, when it wraps one).
function Combatant:invalidate()
  self.stat_version = (self.stat_version or 0) + 1
  if self.actor and self.actor.invalidate then self.actor:invalidate() end
end

--- Modifiers from active states (in sorted state id order).
function Combatant:state_modifiers()
  local out = {}
  for _, id in ipairs(self:states()) do
    local def = data.find("states", id)
    if def then
      for _, m in ipairs(def.modifiers) do out[#out + 1] = m end
    end
  end
  return out
end

--- Compute the stats now (base + every modifier), ignoring the cache.
function Combatant:compute_stats()
  local modifiers = {}
  for _, m in ipairs(self:modifiers()) do modifiers[#modifiers + 1] = m end
  for _, m in ipairs(self:state_modifiers()) do modifiers[#modifiers + 1] = m end
  return stats.compute(self:base_stats(), modifiers)
end

-- The cached stat table, recomputed when this combatant, its actor, the
-- stat definitions or the global epoch changed since it was stored.
local function cached(self)
  local cache = self.stat_cache
  local actor_version = self.actor and self.actor.stat_version or 0
  if cache and cache.version == (self.stat_version or 0) and cache.actor_version == actor_version
    and cache.epoch == Combatant.epoch and cache.stats_version == stats.version then
    return cache.values
  end
  cache = { values = self:compute_stats(), version = self.stat_version or 0, actor_version = actor_version,
    epoch = Combatant.epoch, stats_version = stats.version }
  self.stat_cache = cache
  return cache.values
end

--- Computed stats (base + every modifier); a copy of the cached table.
function Combatant:stats()
  return tablex.copy(cached(self))
end

function Combatant:stat(id)
  return cached(self)[id] or 0
end

function Combatant:max_resource(id)
  return stats.resource_max(id, cached(self))
end

function Combatant:resource(id)
  return self.resources[id] or 0
end

--- Set a resource, clamped to 0..max. Returns the new value.
function Combatant:set_resource(id, value)
  local max = self:max_resource(id)
  value = tablex.clamp(math.floor(value + 0.5), 0, max)
  self.resources[id] = value
  if id == Combatant.life and value <= 0 then
    self:on_knocked_out()
  end
  return value
end

--- Change a resource by delta; returns the actual change.
function Combatant:change_resource(id, delta)
  local before = self:resource(id)
  return self:set_resource(id, before + delta) - before
end

--- Fill resources that start full; set others to their start value.
function Combatant:init_resources(full)
  local values = cached(self)
  for _, def in ipairs(stats.resource_list) do
    if full or self.resources[def.id] == nil then
      local start = def.start == "max" and stats.resource_max(def.id, values) or def.start
      self.resources[def.id] = start
    end
  end
end

function Combatant:alive()
  return self:resource(Combatant.life) > 0
end

function Combatant:on_knocked_out()
  local removed = false
  for id in pairs(self.state_turns) do
    local def = data.find("states", id)
    if def and tablex.contains(def.removed_by, "death") then
      self.state_turns[id] = nil
      removed = true
    end
  end
  if removed then self:invalidate() end
end

function Combatant:has_state(id)
  return self.state_turns[id] ~= nil
end

--- Add a state; returns true if it took effect. Each application bumps
--- `state_serial` and records it in `state_added[id]`, so a caller can
--- tell which states were (re)applied since a point in time.
function Combatant:add_state(id, turns)
  local def = data.get("states", id)
  local current = self.state_turns[id]
  turns = turns or def.turns
  if current ~= nil then
    if def.stacking == "ignore" then return false end
    if def.stacking == "extend" and current > 0 then turns = current + turns end
  end
  self.state_turns[id] = turns
  self.state_serial = (self.state_serial or 0) + 1
  self.state_added = self.state_added or {}
  self.state_added[id] = self.state_serial
  self:invalidate()
  return true
end

function Combatant:remove_state(id)
  local had = self.state_turns[id] ~= nil
  self.state_turns[id] = nil
  if had then self:invalidate() end
  return had
end

--- Remove states carrying a tag (e.g. "debuff"); returns removed ids.
function Combatant:remove_states_tagged(tag)
  local removed = {}
  for id in pairs(self.state_turns) do
    local def = data.find("states", id)
    if def and tablex.contains(def.tags, tag) then
      self.state_turns[id] = nil
      removed[#removed + 1] = id
    end
  end
  if #removed > 0 then self:invalidate() end
  table.sort(removed)
  return removed
end

--- Sorted active state ids.
function Combatant:states()
  local ids = {}
  for id in pairs(self.state_turns) do ids[#ids + 1] = id end
  table.sort(ids)
  return ids
end

--- Count down states with a duration; returns ids that expired. States in
--- `skip` (a set of ids) keep their count this time.
---@param skip? table<string, boolean>
function Combatant:tick_states(skip)
  local expired = {}
  for id, turns in pairs(self.state_turns) do
    if turns > 0 and not (skip and skip[id]) then
      if turns == 1 then
        expired[#expired + 1] = id
      else
        self.state_turns[id] = turns - 1
      end
    end
  end
  for _, id in ipairs(expired) do self.state_turns[id] = nil end
  if #expired > 0 then self:invalidate() end
  table.sort(expired)
  return expired
end

--- Clear states removed by an occasion ("battle_end", "rest").
function Combatant:clear_states(occasion)
  local removed = false
  for id in pairs(self.state_turns) do
    local def = data.find("states", id)
    if not def or tablex.contains(def.removed_by, occasion) then
      self.state_turns[id] = nil
      removed = true
    end
  end
  if removed then self:invalidate() end
end

function Combatant:can_act()
  if not self:alive() then return false end
  for id in pairs(self.state_turns) do
    local def = data.find("states", id)
    if def and def.prevents_action then return false end
  end
  return true
end

--- Damage multiplier for an element or damage type (1 = normal).
function Combatant:resist(key)
  if not key then return 1 end
  local value = 1
  for _, source in ipairs(self:resistances()) do
    if source[key] then value = value * source[key] end
  end
  return value
end

--- Tables of multipliers by element/damage type; classes override.
function Combatant:resistances()
  return {}
end

--- Hot reload: the reloaded functions must not trust caches built by the
--- old ones.
function Combatant.__reload(live)
  live.epoch = (live.epoch or 0) + 1
end

return Combatant
