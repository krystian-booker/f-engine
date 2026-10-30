--- Tactical AI: score every (destination, skill, target) a unit could
--- choose this turn by expected value, using the same reach, range, line of
--- sight, area and forecast rules as the player. The weights and the
--- preview budget (`max_previews`) live in tactical.settings.ai; see
--- `ai.plan` for how the search is pruned and memoised.
local Battle = require("fe.battle.core")
local data = require("fe.data")
local effects = require("fe.effects")
local grid = require("fe.battle.tactical.grid")
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
--- against; obstacles caught in an area count for nothing.
function ai.expected(battle, user, skill, targets)
  local results = effects.resolve({ user = user, targets = targets, source = skill, battle = battle, preview = true })
  local life = require("fe.rpg.combatant").life
  local w = settings.ai
  local score = 0
  for _, r in ipairs(tablex.filter(results, function(r) return not r.target.obstacle end)) do
    local t = r.target
    local friendly = t.side == user.side
    local chance = (r.hit_chance or 1) * (r.chance or 1)
    local effect = r.effect or {}
    if r.type == "damage" then
      local value = math.min(1, (r.amount or 0) / math.max(1, t:resource(life))) * chance
      if (r.amount or 0) >= t:resource(life) then value = value + w.ko * chance end
      score = score + (friendly and -value * w.ally_damage or value)
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

-- Height difference bucket (half cells, clamped like the height bonus) and
-- the side a target is attacked from, which fix the forecast for a given
-- skill and target: previews from destinations that agree on them are
-- shared within one plan.
local function preview_key(world, skill, from, target)
  local diff = grid.cell_height(world, from) - grid.cell_height(world, target.cell)
  local bucket = tablex.clamp(math.floor(diff * 2 + 0.5) / 2, -3, 3)
  local side = grid.direction_between(target.cell, from)
  return skill.id .. "|" .. grid.key(target.cell) .. "|" .. bucket .. "|" .. side.x .. "," .. side.z
end

--- Plan a turn for a computer-controlled unit: {move = path, action = action, facing = dir}.
--- Games can replace it with a `plan(battle, unit)` rule hook.
---
--- Cost: for each usable skill only destinations from which at least one
--- target lies within the skill's bound (`ai.range_bound`) are tried, the
--- exact `range_cells` test comes after; previews are memoised per (skill,
--- target, height bucket, side); at most `settings.ai.max_previews` previews
--- run per plan, then the best plan so far is used. The plan carries
--- `previews`, how many it ran.
function ai.plan(battle, b)
  local cells = grid.reachable(battle, b)
  local foes = battle:opponents(b)
  local origin = b.cell
  local world = battle.tactical.world
  local budget = settings.ai.max_previews or math.huge
  local options = {}
  for _, skill in ipairs(usable_skills(b)) do
    if battle:usable(b, skill) then
      local targets = tablex.filter(candidates(battle, b, skill), function(u) return u.cell ~= nil end)
      if #targets > 0 then
        local bound, manhattan = ai.range_bound(battle, b, skill)
        options[#options + 1] = { skill = skill, targets = targets, bound = bound, manhattan = manhattan }
      end
    end
  end
  local memo, previews, exhausted = {}, 0, false
  local best
  for _, dest in ipairs(cells) do
    if exhausted then break end
    local from = { x = dest.x, z = dest.z, layer = dest.layer }
    local danger = 0
    for _, foe in ipairs(foes) do
      if foe.cell and vec.manhattan(foe.cell, from) <= 1 then danger = danger + settings.ai.danger end
    end
    grid.place(battle, b, from)
    for _, option in ipairs(options) do
      if exhausted then break end
      local near = tablex.filter(option.targets, function(u) return within_bound(from, u, option.bound, option.manhattan) end)
      local reach = #near > 0 and grid.range_cells(battle, b, option.skill, from) or nil
      for _, target in ipairs(near) do
        if grid.contains_cell(reach, target.cell) then
          local key = preview_key(world, option.skill, from, target)
          local entry = memo[key]
          if not entry then
            if previews >= budget then
              exhausted = true
              break
            end
            previews = previews + 1
            local affected = grid.affected(battle, b, option.skill, target.cell)
            entry = { targets = affected, score = #affected > 0 and ai.expected(battle, b, option.skill, affected) or nil }
            memo[key] = entry
          end
          local score = entry.score and entry.score - danger or -1
          if score > 0 and (not best or score > best.score) then
            best = { score = score, dest = from, skill = option.skill, cell = target.cell, targets = entry.targets }
          end
        end
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
