--- Ready-made responses for the `reaction` rule hook (fe.battle.core): a
--- battler hit by an action may answer it once; reactions never react.
---
--- ```lua
--- local reactions = require("fe.battle.reactions")
--- battle.rules.reaction = reactions.counter({ chance = 0.5, facing = "front", requires = "counter" })
--- ```
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
--- skill's range from the defender's cell.
---@param settings? {skill?: string, melee_only?: boolean, facing?: "any"|"not_behind"|"front", chance?: number|fun(battle: table, b: table, action: table): number, requires?: string, when?: fun(battle: table, b: table, action: table, results: table[]): boolean}
---@return fun(battle: table, b: table, action: table, results: table[]): table|nil
function reactions.counter(settings)
  settings = settings or {}
  return function(battle, b, action, results)
    local attacker = action.user
    if not attacker or attacker.side == b.side or not attacker:alive() or b.obstacle or not action.skill then return nil end
    local hurt = tablex.find(results or {}, function(r)
      return r.target == b and r.type == "damage" and not r.missed and (r.amount or 0) > 0
    end)
    if not hurt then return nil end
    local used = data.find("skills", action.skill)
    if settings.melee_only ~= false then
      if used and ranged(used) then return nil end
      if b.cell and attacker.cell and vec.manhattan(b.cell, attacker.cell) > 1 then return nil end
    end
    if not allowed[settings.facing or "any"][approach(b, attacker)] then return nil end
    if settings.requires and not has_ability(b, settings.requires) then return nil end
    if settings.when and not settings.when(battle, b, action, results) then return nil end
    local skill_id = settings.skill or Battle.settings.attack_skill
    if battle.tactical and b.cell and attacker.cell then
      local grid = require("fe.battle.tactical.grid")
      local skill = data.get("skills", skill_id)
      if not grid.contains_cell(grid.range_cells(battle, b, skill), attacker.cell) then return nil end
    end
    local chance = settings.chance or 1
    if type(chance) == "function" then chance = chance(battle, b, action) end
    if chance < 1 and battle.random:float() >= chance then return nil end
    return { kind = "skill", user = b, skill = skill_id, targets = { attacker }, cell = attacker.cell, counter = true }
  end
end

return reactions
