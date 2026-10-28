--- A battle participant: a party Actor (sharing its HP/MP/states, so damage
--- persists after battle) or an enemy built from data.
local Combatant = require("fe.rpg.combatant")
local class = require("fe.class")
local data = require("fe.data")
local tablex = require("fe.tablex")

---@class Battler
local Battler = class("Battler")
Combatant.mixin(Battler)

local next_id = 0

---@param spec {actor?: Actor, enemy?: string, side?: string, name?: string, position?: table}
function Battler:init(spec)
  next_id = next_id + 1
  self.key = next_id
  self.side = spec.side or (spec.actor and "party" or "enemy")
  self.position = spec.position
  self.data = {}          -- free-form per-battle state (shields, boosts...)
  self.buffs = {}         -- temporary modifiers {stat, add|mul, turns}
  if spec.actor then
    self.actor = spec.actor
    self.id = spec.actor.id
    self.resources = spec.actor.resources
    self.state_turns = spec.actor.state_turns
  else
    self.def = data.get("enemies", spec.enemy)
    self.id = spec.enemy .. "#" .. next_id
    self.resources = {}
    self.state_turns = {}
    self:init_resources(true)
  end
  self.label = spec.name
end

function Battler:name()
  if self.label then return self.label end
  return self.actor and self.actor:name() or self.def.name
end

function Battler:is_party() return self.side == "party" end

function Battler:base_stats()
  return self.actor and self.actor:base_stats() or self.def.stats
end

function Battler:modifiers()
  local out = self.actor and self.actor:modifiers() or {}
  for _, buff in ipairs(self.buffs) do out[#out + 1] = buff end
  return out
end

function Battler:resistances()
  if self.actor then return self.actor:resistances() end
  return { self.def.resist }
end

function Battler:weapon()
  return self.actor and self.actor:weapon()
end

function Battler:level()
  return self.actor and self.actor:level() or self.def.level
end

--- Usable skills (optionally one type).
function Battler:skills(skill_type)
  if self.actor then return self.actor:skills(skill_type) end
  local out = {}
  for _, id in ipairs(self.def.skills) do
    local skill = data.get("skills", id)
    if not skill_type or skill.type == skill_type then out[#out + 1] = skill end
  end
  return out
end

function Battler:commands()
  return self.actor and self.actor:commands() or {}
end

function Battler:sprite()
  if self.actor then return self.actor:battle_sprite() end
  return self.def.sprite
end

--- Whether the battler can pay a skill's costs.
function Battler:can_pay(cost)
  for resource, amount in pairs(cost or {}) do
    if self:resource(resource) < amount then return false end
  end
  return true
end

function Battler:pay(cost)
  for resource, amount in pairs(cost or {}) do
    self:change_resource(resource, -amount)
  end
end

--- Add a temporary modifier lasting `turns` of this battler's turns.
function Battler:buff(modifier, turns)
  local entry = tablex.copy(modifier)
  entry.turns = turns or 3
  self.buffs[#self.buffs + 1] = entry
end

function Battler:tick_buffs()
  self.buffs = tablex.filter(self.buffs, function(b)
    b.turns = b.turns - 1
    return b.turns > 0
  end)
end

return Battler
