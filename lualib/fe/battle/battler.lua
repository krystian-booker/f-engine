--- A battle participant: a party Actor (sharing its HP/MP/states, so damage
--- persists after battle) or an enemy built from data.
---
--- Obstacles (`obstacle = true` in the spec or the enemy data) are
--- battlers with HP that never take turns or act: barricades, crates and
--- other destructibles. Their side is always "obstacle" (a spec naming
--- another side is an error), so they never count for either side's
--- outcome.
local Combatant = require("fe.rpg.combatant")
local class = require("fe.class")
local data = require("fe.data")
local tablex = require("fe.tablex")

---@class Battler
local Battler = class("Battler")
Combatant.mixin(Battler)

Battler.settings = {
  buff_cap = 3, -- stacks per buff id (a modifier's `cap` overrides)
  --- Bounds on the combined multiplier of all buffs and debuffs on a stat
  --- ({min, max}); `buff_limits[stat]` overrides it for one stat.
  buff_limit = { min = 0.5, max = 2 },
  buff_limits = {},
}

local next_id = 0

---@param spec {actor?: Actor, enemy?: string, side?: string, name?: string, position?: table, cell?: table, facing?: table, obstacle?: boolean}
function Battler:init(spec)
  next_id = next_id + 1
  self.key = next_id
  self.position = spec.position
  self.cell = spec.cell     -- grid cell in tactical battles
  self.facing = spec.facing -- {x, z} ground direction in tactical battles
  self.data = {}          -- free-form per-battle state (shields, boosts...)
  self.buffs = {}         -- temporary modifiers (see Battler:buff)
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
  self.obstacle = spec.obstacle or (self.def and self.def.obstacle) or nil
  if self.obstacle and spec.side ~= nil and spec.side ~= "obstacle" then
    error("obstacle '" .. tostring(spec.enemy) .. "' cannot take side '" .. tostring(spec.side) .. "'", 2)
  end
  self.side = (self.obstacle and "obstacle") or spec.side or (spec.actor and "party") or "enemy"
  self.label = spec.name
end

function Battler:name()
  if self.label then return self.label end
  return self.actor and self.actor:name() or self.def.name
end

function Battler:is_party() return self.side == "party" end

--- Whether the battler can act now (obstacles never do).
function Battler:can_act()
  return not self.obstacle and Combatant.can_act(self)
end

--- Whether schedulers give this battler turns (obstacles get none).
function Battler:takes_turns()
  return not self.obstacle and self:alive()
end

--- How many actions the battler takes in each of its turns: the enemy
--- data's `actions` (an integer, or a function path "module.fn" called as
--- fn(battle, battler) each turn), else 1.
function Battler:actions_per_turn(battle)
  local actions = self.def and self.def.actions or 1
  if type(actions) == "string" then actions = require("fe.battle.ai").resolve(actions)(battle, self) end
  local count = type(actions) == "number" and math.tointeger(actions)
  if not count or count < 1 then
    error(self:name() .. ": actions per turn must be a positive integer, got " .. tostring(actions), 2)
  end
  return count
end

function Battler:base_stats()
  return self.actor and self.actor:base_stats() or self.def.stats
end

--- Equipment, passive and class modifiers, then buffs. The buffs'
--- multipliers on one stat combine and are kept within
--- Battler.settings.buff_limit (or `buff_limits[stat]`).
function Battler:modifiers()
  local out = self.actor and self.actor:modifiers() or {}
  local factors, order = {}, {}
  for _, buff in ipairs(self.buffs) do
    if buff.mul then
      if not factors[buff.stat] then order[#order + 1] = buff.stat end
      factors[buff.stat] = (factors[buff.stat] or 1) * buff.mul ^ buff.stacks
    end
    if buff.add or buff.set then
      out[#out + 1] = { stat = buff.stat, add = buff.add and buff.add * buff.stacks, set = buff.set,
        priority = buff.priority }
    end
  end
  for _, stat in ipairs(order) do
    local limit = Battler.settings.buff_limits[stat] or Battler.settings.buff_limit
    out[#out + 1] = { stat = stat, mul = tablex.clamp(factors[stat], limit.min, limit.max) }
  end
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

--- Add a temporary modifier ({stat, add|mul|set, id?, cap?, priority?})
--- lasting `turns` of this battler's turns. Buffs with the same id (default
--- "<stat>:<kind>") stack up to `cap` (Battler.settings.buff_cap); each
--- stack adds `add` again or multiplies by `mul` again, and every new
--- application refreshes the duration to the longer of the two. `kind` is
--- "debuff" when the modifier lowers the stat, else "buff". A buff applied
--- during the bearer's own turn is not counted down at the end of that
--- turn, so a self-buff lasts `turns` full turns. Returns the entry, whose
--- `capped` is true when this application could not add a stack.
function Battler:buff(modifier, turns)
  turns = turns or 3
  local lowers = (modifier.add and modifier.add < 0) or (modifier.mul and modifier.mul < 1)
  local kind = modifier.kind or (lowers and "debuff" or "buff")
  local id = modifier.id or (modifier.stat .. ":" .. kind)
  local entry = tablex.find(self.buffs, function(b) return b.id == id end)
  if entry then
    entry.capped = entry.stacks >= entry.cap
    entry.stacks = math.min(entry.cap, entry.stacks + 1)
    entry.turns = math.max(entry.turns, turns)
  else
    entry = { id = id, stat = modifier.stat, add = modifier.add, mul = modifier.mul, set = modifier.set,
      priority = modifier.priority, kind = kind, cap = modifier.cap or Battler.settings.buff_cap, stacks = 1,
      turns = turns, capped = false }
    self.buffs[#self.buffs + 1] = entry
  end
  entry.fresh = self.acting == true
  return entry
end

--- Active buffs, optionally of one kind ("buff" or "debuff").
function Battler:buffs_of(kind)
  return tablex.filter(self.buffs, function(b) return not kind or b.kind == kind end)
end

--- Remove buffs of a kind (or all); returns how many were removed.
function Battler:clear_buffs(kind)
  local before = #self.buffs
  self.buffs = tablex.filter(self.buffs, function(b) return kind and b.kind ~= kind end)
  return before - #self.buffs
end

--- Count down buff durations at the end of the bearer's turn.
function Battler:tick_buffs()
  self.buffs = tablex.filter(self.buffs, function(b)
    if b.fresh then
      b.fresh = nil
      return true
    end
    b.turns = b.turns - 1
    return b.turns > 0
  end)
end

--- Reset battle-only resources (boost points...) to their start value.
function Battler:reset_battle_resources()
  for _, def in ipairs(require("fe.rpg.stats").resource_list) do
    if def.battle_only then
      self:set_resource(def.id, def.start == "max" and self:max_resource(def.id) or def.start)
    end
  end
end

return Battler
