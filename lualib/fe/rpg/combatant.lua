--- Shared behaviour for anything with stats, resources and states: field
--- actors and battle participants. A class mixing this in provides:
---   self:base_stats()  -> table of base values
---   self:modifiers()   -> list of stat modifiers (equipment, passives...)
---   self.resources     -> table resource id -> current value
---   self.state_turns   -> table state id -> remaining turns (0 = lasting)
local data = require("fe.data")
local stats = require("fe.rpg.stats")
local tablex = require("fe.tablex")

local Combatant = {}

--- The resource whose depletion means knocked out.
Combatant.life = "hp"

function Combatant.mixin(class)
  for key, value in pairs(Combatant) do
    if type(value) == "function" and key ~= "mixin" and class[key] == nil then
      class[key] = value
    end
  end
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

--- Computed stats (base + every modifier).
function Combatant:stats()
  local modifiers = {}
  for _, m in ipairs(self:modifiers()) do modifiers[#modifiers + 1] = m end
  for _, m in ipairs(self:state_modifiers()) do modifiers[#modifiers + 1] = m end
  return stats.compute(self:base_stats(), modifiers)
end

function Combatant:stat(id)
  return self:stats()[id] or 0
end

function Combatant:max_resource(id)
  return stats.resource_max(id, self:stats())
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
  local values = self:stats()
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
  for id in pairs(self.state_turns) do
    local def = data.find("states", id)
    if def and tablex.contains(def.removed_by, "death") then
      self.state_turns[id] = nil
    end
  end
end

function Combatant:has_state(id)
  return self.state_turns[id] ~= nil
end

--- Add a state; returns true if it took effect.
function Combatant:add_state(id, turns)
  local def = data.get("states", id)
  local current = self.state_turns[id]
  turns = turns or def.turns
  if current ~= nil then
    if def.stacking == "ignore" then return false end
    if def.stacking == "extend" and current > 0 then turns = current + turns end
  end
  self.state_turns[id] = turns
  return true
end

function Combatant:remove_state(id)
  local had = self.state_turns[id] ~= nil
  self.state_turns[id] = nil
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
  table.sort(expired)
  return expired
end

--- Clear states removed by an occasion ("battle_end", "rest").
function Combatant:clear_states(occasion)
  for id in pairs(self.state_turns) do
    local def = data.find("states", id)
    if not def or tablex.contains(def.removed_by, occasion) then
      self.state_turns[id] = nil
    end
  end
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

return Combatant
