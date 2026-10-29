--- Tactical AI: score every (destination, skill, target) a unit could
--- choose this turn by expected value, using the same reach, range, line of
--- sight, area and forecast rules as the player.
local Battle = require("fe.battle.core")
local effects = require("fe.effects")
local grid = require("fe.battle.tactical.grid")
local tablex = require("fe.tablex")
local vec = require("fe.vec")

local ai = {}

--- Expected value of using `skill` on `targets` now: forecast damage (as
--- a share of the target's remaining life, plus a bonus for a likely KO)
--- and healing, each weighted by the forecast hit chance. Hurting allies
--- counts against it; obstacles caught in an area count for nothing.
function ai.expected(battle, user, skill, targets)
  local results = effects.resolve({ user = user, targets = targets, source = skill, battle = battle, preview = true })
  local life = require("fe.rpg.combatant").life
  local score = 0
  for _, r in ipairs(tablex.filter(results, function(r) return not r.target.obstacle end)) do
    local t = r.target
    local chance = r.hit_chance or 1
    if r.type == "damage" then
      local value = math.min(1, (r.amount or 0) / math.max(1, t:resource(life))) * chance
      if (r.amount or 0) >= t:resource(life) then value = value + 0.6 * chance end
      score = score + (t.side == user.side and -value * 1.5 or value)
    elseif r.type == "heal" then
      local missing = 1 - t:resource(life) / math.max(1, t:max_resource(life))
      score = score + (t.side == user.side and math.min(missing, (r.amount or 0) / math.max(1, t:max_resource(life))) or -0.5)
    elseif r.type == "revive" then
      score = score + (t.side == user.side and 1 or -1)
    end
  end
  return score
end

local function usable_skills(b)
  local skills = tablex.filter(b:skills(), function(s) return not s.passive and s.scope ~= "field" end)
  local attack = Battle.attack_skill()
  if attack and not tablex.find(skills, function(s) return s.id == attack.id end) then
    table.insert(skills, 1, attack)
  end
  return skills
end

local function candidates(battle, b, skill)
  if skill.target == "dead_ally" then
    return tablex.filter(battle:allies(b, true), function(u) return not u:alive() end)
  elseif skill.target == "ally" or skill.target == "all_allies" or skill.target == "self" then
    return battle:allies(b)
  end
  return battle:opponents(b)
end

--- Plan a turn for a computer-controlled unit: {move = path, action = action, facing = dir}.
--- Games can replace it with a `plan(battle, unit)` rule hook.
function ai.plan(battle, b)
  local cells = grid.reachable(battle, b)
  local skills = usable_skills(b)
  local foes = battle:opponents(b)
  local origin = b.cell
  local best
  for _, dest in ipairs(cells) do
    local from = { x = dest.x, z = dest.z, layer = dest.layer }
    local danger = 0
    for _, foe in ipairs(foes) do
      if foe.cell and vec.manhattan(foe.cell, from) <= 1 then danger = danger + 0.05 end
    end
    grid.place(battle, b, from)
    for _, skill in ipairs(skills) do
      if battle:usable(b, skill) then
        local reach = grid.range_cells(battle, b, skill, from)
        for _, target in ipairs(candidates(battle, b, skill)) do
          if target.cell and grid.contains_cell(reach, target.cell) then
            local affected = grid.affected(battle, b, skill, target.cell)
            local score = #affected > 0 and ai.expected(battle, b, skill, affected) - danger or -1
            if score > 0 and (not best or score > best.score) then
              best = { score = score, dest = from, skill = skill, cell = target.cell, targets = affected }
            end
          end
        end
      end
    end
  end
  grid.place(battle, b, origin)
  local plan = {}
  if best then
    plan.move = grid.path_to(cells, best.dest)
    plan.action = { kind = "skill", user = b, skill = best.skill.id, targets = best.targets, cell = best.cell }
    plan.facing = grid.direction_between(best.dest, best.cell)
  else
    -- Close in on the nearest foe.
    local target, distance = nil, math.huge
    for _, foe in ipairs(foes) do
      local d = foe.cell and vec.manhattan(foe.cell, b.cell) or math.huge
      if d < distance then target, distance = foe, d end
    end
    if target then
      local dest, best_d = nil, distance
      for _, c in ipairs(cells) do
        local d = vec.manhattan(target.cell, c)
        if d < best_d then dest, best_d = c, d end
      end
      if dest then plan.move = grid.path_to(cells, dest) end
      plan.facing = grid.direction_between(dest or b.cell, target.cell)
    end
  end
  return plan
end

return ai
