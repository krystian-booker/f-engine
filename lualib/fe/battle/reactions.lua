--- Ready-made responses for the `reaction` rule hook (fe.battle.core): a
--- battler hit by an action may answer it once; reactions never react.
---
--- ```lua
--- local reactions = require("fe.battle.reactions")
--- battle.rules.reaction = reactions.counter({ chance = 0.5, facing = "front", requires = "counter" })
--- ```
--- A hook made here is callable like a function and carries a
--- `forecast(battle, b, action) -> chance` that answers, without rolling
--- or changing anything, how likely `b` is to respond to `action` if it
--- lands; `reactions.forecast(battle, b, action)` asks the battle's
--- reaction hook (nil for a hook without one). The tactical forecast panel
--- shows it as "Counter n%".
local Battle = require("fe.battle.core")
local data = require("fe.data")
local tablex = require("fe.tablex")
local vec = require("fe.vec")

local reactions = {}

-- Whether `b` has an ability: a skill or passive its actor knows, or a
-- skill in its enemy data.
local function has_ability(b, id)
  if b.actor then return b.actor:knows(id) end
  return b.def ~= nil and tablex.contains(b.def.skills, id)
end

-- A skill reaching further than the next cell (by its range or a "ranged" tag).
local function ranged(skill)
  return (skill.range ~= nil and (skill.range.max or 1) > 1) or tablex.contains(skill.tags or {}, "ranged")
end

-- How the attacker stands relative to the defender's facing: "front",
-- "side" or "back" (tactical battles; "front" without cells or facing).
local function approach(b, attacker)
  if not (b.cell and attacker.cell and b.facing) then return "front" end
  local dx, dz = attacker.cell.x - b.cell.x, attacker.cell.z - b.cell.z
  local length = math.sqrt(dx * dx + dz * dz)
  if length == 0 then return "front" end
  local dot = (b.facing.x * dx + b.facing.z * dz) / length
  if dot > 0.5 then return "front" end
  if dot < -0.5 then return "back" end
  return "side"
end

local allowed = {
  any = { front = true, side = true, back = true },
  not_behind = { front = true, side = true },
  front = { front = true },
}

--- A counterattack: a battler damaged by an opponent's skill strikes back
--- at the attacker with `skill` (default: the attack skill). Settings:
---   melee_only (default true): only against skills that are not ranged
---     (range max 1 and no "ranged" tag) and, on a grid, from the next cell
---   facing: "any" (default), "not_behind" or "front": where the attacker
---     must stand relative to the defender's facing (tactical battles)
---   chance: 0..1 or fn(battle, b, action) -> 0..1 (default 1), rolled
---     with the battle's random stream after every other check
---   requires: a skill or passive id the defender must have
---   when: fn(battle, b, action, results) -> boolean, an extra condition
--- On a grid the counter also needs the attacker within the counter
--- skill's range from the defender's cell. The returned hook's
--- `forecast(battle, b, action)` gives the chance (0 when a check other
--- than `when` fails) without rolling.
---@param settings? {skill?: string, melee_only?: boolean, facing?: "any"|"not_behind"|"front", chance?: number|fun(battle: table, b: table, action: table): number, requires?: string, when?: fun(battle: table, b: table, action: table, results: table[]): boolean}
---@return table hook callable as fn(battle, b, action, results) -> action|nil
function reactions.counter(settings)
  settings = settings or {}

  -- The chance `b` counters `action` if it lands and hurts (nil when it
  -- cannot), and the counter's skill id.
  local function chance_of(battle, b, action)
    local attacker = action.user
    if not attacker or attacker.side == b.side or not attacker:alive() or b.obstacle or not action.skill then return nil end
    local used = data.find("skills", action.skill)
    if settings.melee_only ~= false then
      if used and ranged(used) then return nil end
      if b.cell and attacker.cell and vec.manhattan(b.cell, attacker.cell) > 1 then return nil end
    end
    if not allowed[settings.facing or "any"][approach(b, attacker)] then return nil end
    if settings.requires and not has_ability(b, settings.requires) then return nil end
    local skill_id = settings.skill or Battle.settings.attack_skill
    if battle.tactical and b.cell and attacker.cell then
      local grid = require("fe.battle.tactical.grid")
      local skill = data.get("skills", skill_id)
      if not grid.contains_cell(grid.range_cells(battle, b, skill), attacker.cell) then return nil end
    end
    local chance = settings.chance or 1
    if type(chance) == "function" then chance = chance(battle, b, action) end
    return chance, skill_id
  end

  local hook = {}

  --- Chance 0..1 that `b` counters `action` should it land and hurt.
  function hook.forecast(battle, b, action)
    if not b:can_act() then return 0 end
    return chance_of(battle, b, action) or 0
  end

  return setmetatable(hook, { __call = function(_, battle, b, action, results)
    local hurt = tablex.find(results or {}, function(r)
      return r.target == b and r.type == "damage" and not r.missed and (r.amount or 0) > 0
    end)
    if not hurt then return nil end
    if settings.when and not settings.when(battle, b, action, results) then return nil end
    local chance, skill_id = chance_of(battle, b, action)
    if not chance then return nil end
    if chance < 1 and battle.random:float() >= chance then return nil end
    return { kind = "skill", user = b, skill = skill_id, targets = { action.user }, cell = action.user.cell, counter = true }
  end })
end

--- The chance that `b` responds to `action` according to the battle's
--- `reaction` rule hook, or nil when the hook has no forecast.
function reactions.forecast(battle, b, action)
  local hook = battle:rule("reaction")
  if type(hook) == "table" and hook.forecast then return hook.forecast(battle, b, action) end
  return nil
end

return reactions
