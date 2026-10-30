--- Tactical AI: score every (destination, skill, target) a unit could
--- choose this turn by expected value, using the same reach, range, line of
--- sight, area and forecast rules as the player. The weights and the
--- preview budget (`max_previews`) live in tactical.settings.ai; see
--- `ai.plan` for how the search is pruned: one preview per (skill, target),
--- and a cheap positional factor per destination.
local Battle = require("fe.battle.core")
local data = require("fe.data")
local effects = require("fe.effects")
local grid = require("fe.battle.tactical.grid")
local reactions = require("fe.battle.reactions")
local rules = require("fe.battle.tactical.rules")
local config = require("fe.battle.tactical.settings")
local tablex = require("fe.tablex")
local vec = require("fe.vec")

local ai = {}

local settings = config.settings

-- Whether a buff effect lowers its stat (a debuff).
local function lowers(effect)
  return (effect.add and effect.add < 0) or (effect.mul and effect.mul < 1) or false
end

--- Expected value of using `skill` on `targets` now. Damage counts as the
--- forecast share of the target's remaining life (plus `ai.ko` for a
--- likely knock-out) and healing as the share of its missing life, each
--- weighted by the forecast hit chance; buffs, debuffs, added and removed
--- states count a flat weight (`ai.buff`, `ai.state`, `ai.disable`) times
--- their chance, positive when they help the user's side and negative
--- when they help the other. Hurting allies counts `ai.ally_damage` times
--- against; obstacles caught in an area count for nothing. Also returns
--- whether the skill hurt a foe (what positional bonuses and counters
--- apply to).
---@return number score, boolean damaging
function ai.expected(battle, user, skill, targets)
  local results = effects.resolve({ user = user, targets = targets, source = skill, battle = battle, preview = true })
  local life = require("fe.rpg.combatant").life
  local w = settings.ai
  local score, damaging = 0, false
  for _, r in ipairs(tablex.filter(results, function(r) return not r.target.obstacle end)) do
    local t = r.target
    local friendly = t.side == user.side
    local chance = (r.hit_chance or 1) * (r.chance or 1)
    local effect = r.effect or {}
    if r.type == "damage" then
      local value = math.min(1, (r.amount or 0) / math.max(1, t:resource(life))) * chance
      if (r.amount or 0) >= t:resource(life) then value = value + w.ko * chance end
      score = score + (friendly and -value * w.ally_damage or value)
      damaging = damaging or not friendly
    elseif r.type == "heal" then
      local missing = 1 - t:resource(life) / math.max(1, t:max_resource(life))
      score = score + (friendly and math.min(missing, (r.amount or 0) / math.max(1, t:max_resource(life))) or -w.enemy_heal)
    elseif r.type == "revive" then
      score = score + (friendly and w.revive or -w.revive)
    elseif r.type == "buff" then
      local helps = friendly ~= lowers(effect) -- a buff on an ally, a debuff on a foe
      score = score + (helps and w.buff or -w.buff) * chance
    elseif r.type == "add_state" then
      local state = effect.state and data.find("states", effect.state)
      if state and not t:has_state(effect.state) then
        local weight = w.state + (state.prevents_action and w.disable or 0)
        score = score + (friendly and -weight or weight) * chance
      end
    elseif r.type == "remove_state" then
      local has = (effect.state and t:has_state(effect.state))
        or (effect.tag and tablex.find(t:states(), function(id)
          local def = data.find("states", id)
          return def ~= nil and tablex.contains(def.tags, effect.tag)
        end) ~= nil)
      if has then score = score + (friendly and w.state or -w.state) * chance end
    end
  end
  return score, damaging
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

--- How far a skill can reach at most, as a cell distance and whether that
--- distance is Manhattan (diamond, cross and line shapes) or Chebyshev
--- (square, cone, ring): destinations further than this from every target
--- are not previewed. A game whose `range` rule hook extends a skill past
--- its `range.max` (a bow from high ground) replaces this to return the
--- larger bound.
---@return number max, boolean manhattan
function ai.range_bound(battle, user, skill)
  local r = grid.range_of(skill)
  local shape = r.shape or "diamond"
  return r.max or 0, shape == "diamond" or shape == "cross" or shape == "line"
end

-- Whether `target` could be within `bound` cells of `from`.
local function within_bound(from, target, bound, manhattan)
  local dx, dz = math.abs(target.cell.x - from.x), math.abs(target.cell.z - from.z)
  return (manhattan and dx + dz or math.max(dx, dz)) <= bound
end

--- The preview of `skill` aimed at `target` from where `user` stands:
--- `targets` (the units its area catches), `score` (`ai.expected`, nil
--- when the area catches no one), `damaging` (it hurts a foe) and
--- `multiplier` (the positional multiplier on `target` from here, which the
--- score already includes; `ai.positional` divides it out).
---@return {targets: table[], score?: number, damaging?: boolean, multiplier?: number}
function ai.forecast(battle, user, skill, target)
  local affected = grid.affected(battle, user, skill, target.cell)
  local forecast = { targets = affected }
  if #affected > 0 then
    forecast.score, forecast.damaging = ai.expected(battle, user, skill, affected)
    local info = rules.positional_info(battle, user, target)
    forecast.multiplier = info and info.multiplier or 1
  end
  return forecast
end

--- How much better or worse `skill` on `target` is from where `user` now
--- stands than `forecast` (previewed from another cell) says, without
--- running any effect: 1 for a skill that hurts no foe; otherwise the
--- positional multiplier here (height, back and side bonuses from
--- rules.positional_info) over the one the preview included, times
--- 1 - `settings.ai.counter` x the chance the target counters from here
--- (reactions.forecast; 0 without a reaction rule).
---@return number
function ai.positional(battle, user, skill, target, forecast)
  if not forecast.damaging then return 1 end
  local info = rules.positional_info(battle, user, target)
  local factor = (info and info.multiplier or 1) / (forecast.multiplier or 1)
  local action = { kind = "skill", user = user, skill = skill.id, targets = forecast.targets, cell = target.cell }
  local counter = reactions.forecast(battle, target, action) or 0
  return factor * math.max(0, 1 - settings.ai.counter * counter)
end

--- The class a destination falls in for `ai.positional`: the rules only see
--- the height difference (in half cells, clamped like `positional_info`)
--- and which side of the target it attacks from, so every destination of
--- one class has the same factor. nil when the forecast is not positional.
function ai.positional_class(battle, from, skill, target, forecast)
  if not forecast.damaging or not target.cell or not battle.tactical then return nil end
  local world = battle.tactical.world
  local diff = tablex.clamp(grid.cell_height(world, from) - grid.cell_height(world, target.cell), -3, 3)
  local side = "front"
  if not target.obstacle then
    local facing = target.facing or { x = 0, z = 1 }
    local incoming = grid.direction_between(target.cell, from)
    local dot = facing.x * incoming.x + facing.z * incoming.z
    side = dot < -0.5 and "behind" or (dot < 0.5 and "side" or "front")
  end
  return skill.id .. "|" .. tostring(target.id or target) .. "|" .. math.floor(diff * 2 + 0.5) .. "|" .. side
end

--- Plan a turn for a computer-controlled unit: {move = path, action = action, facing = dir}.
--- Games can replace it with a `plan(battle, unit)` rule hook.
---
--- Cost, in three passes. Reach: for each usable skill, every reachable
--- destination from which at least one target lies within the skill's
--- bound (`ai.range_bound`) is range-tested (`grid.range_cells`), listing
--- the shots (destination, skill, target) possible. Previews: each (skill,
--- target) some destination can shoot is previewed once (`ai.forecast`,
--- from the unit's own cell), whatever the number of destinations, until
--- `settings.ai.max_previews` of them ran; (skill, target) pairs past the
--- budget are not considered. Scores: each shot is its preview's score
--- times `ai.positional` at its destination, minus `settings.ai.danger` per
--- adjacent foe; the best positive one is the plan. No effect runs per
--- destination, and neither do the rules: `ai.positional` is computed once
--- per positional class (`ai.positional_class`: skill, target, half-cell
--- height difference, attack side) and looked up for every other
--- destination of that class. The plan carries `previews`, how many it ran.
function ai.plan(battle, b)
  local cells = grid.reachable(battle, b)
  local foes = battle:opponents(b)
  local origin = b.cell
  local w = settings.ai
  local budget = w.max_previews or math.huge
  local options = {}
  for _, skill in ipairs(usable_skills(b)) do
    if battle:usable(b, skill) then
      local targets = tablex.filter(candidates(battle, b, skill), function(u) return u.cell ~= nil end)
      if #targets > 0 then
        local bound, manhattan = ai.range_bound(battle, b, skill)
        -- `wanted`: the targets some destination reaches, in the order found
        -- (`seen` is its set); `forecasts`: their previews, by target.
        options[#options + 1] = { skill = skill, targets = targets, bound = bound, manhattan = manhattan,
          wanted = {}, seen = {}, forecasts = {} }
      end
    end
  end
  local shots = {}
  for _, dest in ipairs(cells) do
    local from = { x = dest.x, z = dest.z, layer = dest.layer }
    local danger = 0
    for _, foe in ipairs(foes) do
      if foe.cell and vec.manhattan(foe.cell, from) <= 1 then danger = danger + w.danger end
    end
    grid.place(battle, b, from)
    for _, option in ipairs(options) do
      local near = tablex.filter(option.targets, function(u) return within_bound(from, u, option.bound, option.manhattan) end)
      local reach = #near > 0 and grid.range_cells(battle, b, option.skill, from) or nil
      for _, target in ipairs(near) do
        if grid.contains_cell(reach, target.cell) then
          if not option.seen[target] then
            option.seen[target] = true
            option.wanted[#option.wanted + 1] = target
          end
          shots[#shots + 1] = { from = from, danger = danger, option = option, target = target }
        end
      end
    end
  end
  grid.place(battle, b, origin)
  local previews = 0
  for _, option in ipairs(options) do
    for _, target in ipairs(option.wanted) do
      if previews >= budget then break end
      previews = previews + 1
      option.forecasts[target] = ai.forecast(battle, b, option.skill, target)
    end
  end
  local best
  local factors = {} -- positional factor per class: the rules run once per class, not per destination
  for _, shot in ipairs(shots) do
    local forecast = shot.option.forecasts[shot.target]
    if forecast and forecast.score then
      local class = ai.positional_class(battle, shot.from, shot.option.skill, shot.target, forecast)
      local factor = class and factors[class]
      if not factor then
        if class then grid.place(battle, b, shot.from) end
        factor = ai.positional(battle, b, shot.option.skill, shot.target, forecast)
        if class then factors[class] = factor end
      end
      local score = forecast.score * factor - shot.danger
      if score > 0 and (not best or score > best.score) then
        best = { score = score, dest = shot.from, skill = shot.option.skill, cell = shot.target.cell,
          targets = forecast.targets }
      end
    end
  end
  grid.place(battle, b, origin)
  local plan = { previews = previews }
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
