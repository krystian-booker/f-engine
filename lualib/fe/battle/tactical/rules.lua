--- Tactical battle rules: height and facing bonuses, knockback, follow-up
--- attacks, the move/act/wait turn, CT costs, reinforcements, and the rule
--- layer every tactical battle gets (objectives, rounds, corpses,
--- destroyed obstacles and arriving reinforcements).
local Battle = require("fe.battle.core")
local effects = require("fe.effects")
local grid = require("fe.battle.tactical.grid")
local i18n = require("fe.i18n")
local objectives = require("fe.battle.tactical.objectives")
local config = require("fe.battle.tactical.settings")
local tablex = require("fe.tablex")
local terrain = require("fe.battle.tactical.terrain")
local vec = require("fe.vec")

local rules = {}

local settings = config.settings

i18n.defaults({ fe = { tactics = { collision = "Collision", fall = "Fall" } } })

--- How `user` stands to `target` on the board: {height_bonus (signed
--- damage share from the height difference), from_behind, from_side,
--- multiplier}. nil when either is off the board. The forecast panel
--- shows it; rules.positional applies it.
function rules.positional_info(battle, user, target)
  if not user.cell or not target.cell or not battle.tactical then return nil end
  local world = battle.tactical.world
  local diff = grid.cell_height(world, user.cell) - grid.cell_height(world, target.cell)
  local info = { height_bonus = tablex.clamp(diff, -3, 3) * settings.height_bonus, from_behind = false, from_side = false }
  info.multiplier = 1 + info.height_bonus
  if not target.obstacle and user ~= target then
    local facing = target.facing or { x = 0, z = 1 }
    local incoming = grid.direction_between(target.cell, user.cell)
    local dot = facing.x * incoming.x + facing.z * incoming.z
    if dot < -0.5 then
      info.from_behind = true
      info.multiplier = info.multiplier * (1 + settings.back_bonus)
    elseif dot < 0.5 then
      info.from_side = true
      info.multiplier = info.multiplier * (1 + settings.side_bonus)
    end
  end
  return info
end

--- Height and facing: attacking from above, the side or behind deals more
--- (obstacles have no back). Skipped for self-targeted and environmental
--- damage (poison, terrain).
function rules.positional(ctx)
  if ctx.effect.type ~= "damage" or ctx.environmental or ctx.user == ctx.target then return end
  local info = rules.positional_info(ctx.battle, ctx.user, ctx.target)
  if info then ctx.multiplier = ctx.multiplier * info.multiplier end
end

local function percent_damage(name, fraction)
  return { name = name, effects = { { type = "damage", formula = "percent", power = fraction * 100, variance = 0 } } }
end

local function enterable(world, cell)
  local c = world:cell(cell)
  if not c or not c.walkable then return nil end
  for _, tag in ipairs(settings.blocked_tags) do
    if tablex.contains(c.tags, tag) then return nil end
  end
  return c
end

--- {"type": "push", "distance": 1}: knock the target away. It stops at
--- walls, blocked cells and steps up; slamming into a unit hurts both
--- (settings.collision_damage) and falling more than 2 hurts
--- (settings.fall_damage per extra unit). Obstacles do not move.
--- Presents "pushed".
effects.handler("push", function(ctx)
  local battle = ctx.battle
  if not battle or not battle.tactical or not ctx.target:alive() or ctx.target.obstacle then return { moved = 0 } end
  local world = battle.tactical.world
  local target = ctx.target
  local dir = grid.direction_between(ctx.user.cell, target.cell)
  local moved = 0
  local from = target.cell
  for _ = 1, ctx.effect.distance or 1 do
    local next_cell = { x = target.cell.x + dir.x, z = target.cell.z + dir.z, layer = target.cell.layer }
    local c = enterable(world, next_cell)
    if not c or c.height - grid.cell_height(world, target.cell) > 0.5 then break end
    local other = grid.unit_at(battle, next_cell)
    if other then
      if settings.collision_damage > 0 then
        terrain.harm(battle, { target, other }, percent_damage(i18n.t("fe.tactics.collision"), settings.collision_damage))
      end
      break
    end
    local drop = grid.cell_height(world, target.cell) - c.height
    grid.place(battle, target, next_cell)
    moved = moved + 1
    if drop > 2 then
      terrain.harm(battle, { target }, percent_damage(i18n.t("fe.tactics.fall"), settings.fall_damage * (drop - 2)))
    end
  end
  if moved > 0 then battle:present("pushed", { battler = target, from = from, to = target.cell }) end
  return { moved = moved }
end)
effects.schema("push", { distance = { type = "integer", min = 1, optional = true } })

--- The skill an ally follows up with: its first affordable skill tagged
--- "follow_up" (an archer's volley), else the attack skill.
function rules.follow_up_skill(ally)
  for _, skill in ipairs(ally:skills()) do
    if tablex.contains(skill.tags or {}, "follow_up") and ally.battle:usable(ally, skill) then return skill end
  end
  return Battle.attack_skill()
end

-- Whether `ally` flanks `target` against an attack from `attacker`:
-- on the opposite side (settings decide whether diagonal cells and ranged
-- attacks from further back count).
local function flanks(battle, ally, target, attacker)
  local d = grid.direction_between(target.cell, attacker.cell)
  local ox, oz = ally.cell.x - target.cell.x, ally.cell.z - target.cell.z
  local behind = -(ox * d.x + oz * d.z)
  local aside = math.abs(ox * d.z - oz * d.x)
  if behind == 1 and (aside == 0 or (aside == 1 and settings.follow_up_diagonal)) then return true end
  if settings.follow_up_ranged and behind >= 1 and aside <= behind then
    local skill = rules.follow_up_skill(ally)
    return skill ~= nil and grid.contains_cell(grid.range_cells(battle, ally, skill), target.cell) ~= nil
  end
  return false
end

--- Pincer attacks: allies flanking a melee attack's first damaged target
--- join in (settings.follow_up, follow_up_diagonal, follow_up_ranged,
--- follow_up_max), nearest first, each with rules.follow_up_skill.
function rules.follow_ups(battle, action, results)
  if not settings.follow_up or action.follow_up or action.reaction then return end
  local skill = grid.skill_of(action)
  if not skill or (skill.range and (skill.range.max or 1) > 1) or skill.target ~= "enemy" then return end
  for _, r in ipairs(results or {}) do
    local target = r.target
    if r.type == "damage" and target:alive() and target.cell and not target.obstacle then
      local allies = tablex.filter(battle:allies(action.user), function(ally)
        return ally ~= action.user and ally.cell ~= nil and ally:can_act() and flanks(battle, ally, target, action.user)
      end)
      tablex.sort_by(allies, function(ally)
        return vec.manhattan(ally.cell, target.cell) * Battle.settings.priority_scale + ally.key
      end)
      for i, ally in ipairs(allies) do
        if (settings.follow_up_max and i > settings.follow_up_max) or not target:alive() then break end
        battle:present("follow_up", { user = ally, target = target })
        grid.face(battle, ally, grid.direction_between(ally.cell, target.cell))
        battle:perform({ kind = "skill", user = ally, skill = rules.follow_up_skill(ally).id, targets = { target },
          cell = target.cell, follow_up = true })
      end
      break
    end
  end
end

--- Schedule reinforcements: `spec` {enemy, cell, id?, name?, facing?,
--- side? ("enemy"), turn? (round number), when? fn(battle) -> boolean,
--- required?}. They arrive at the start of round `turn`, or at the start of
--- the first turn when `when` answers true, on `cell` or the nearest free
--- cell, and never once the battle is decided. `required` ones keep
--- defeat_all for their side from completing until they have arrived.
--- Presents "reinforcements" ({units}) when units arrive.
function rules.reinforce(battle, spec)
  assert(spec.turn or spec.when, "reinforcements need a turn or a when condition")
  local pending = battle.tactical.reinforcements
  pending[#pending + 1] = tablex.copy(spec)
end

--- Add a unit to the battle and put it on its cell: `spec` is a Battler
--- spec with a `cell` (and optionally `id`, the unit id). Before the
--- battle starts the board places it here; afterwards the tactical layer's
--- on_battler_added does (Battle:add presents it too).
function rules.add_unit(battle, spec)
  local id = spec.id
  spec = tablex.copy(spec)
  spec.id = nil
  local u = battle:add(spec)
  u.unit_id = id or u.unit_id
  if not (battle.started and not battle.finished) and u.cell then grid.place(battle, u, u.cell) end
  return u
end

--- Bring in every scheduled reinforcement that is due; returns the units.
--- One with no free cell near its own stays pending and tries again at the
--- next round or turn.
function rules.arrive(battle)
  local t = battle.tactical
  local arrived = {}
  for _, spec in ipairs(tablex.copy(t.reinforcements)) do
    if battle:outcome() then break end
    if (spec.turn and battle.round >= spec.turn) or (spec.when and spec.when(battle)) then
      local cell = grid.nearest_free(battle, spec.cell)
      if cell then
        tablex.remove_value(t.reinforcements, spec)
        arrived[#arrived + 1] = rules.add_unit(battle, { enemy = spec.enemy, side = spec.side or "enemy", name = spec.name,
          cell = cell, facing = spec.facing or { x = 0, z = 1 }, id = spec.id or spec.enemy })
      end
    end
  end
  if #arrived > 0 then battle:present("reinforcements", { units = arrived }) end
  return arrived
end

--- CT a turn costs: settings.ct_cost.base, less no_move / no_act.
function rules.ct_cost(moved, acted)
  local cost = settings.ct_cost
  local ct = cost.base
  if not moved then ct = ct + cost.no_move end
  if not acted then ct = ct + cost.no_act end
  return ct
end

--- Terrain reactions for a performed action (the tactical layer's
--- after_action, so counters, follow-ups and companions' actions count
--- too): the skill's element lands on its area when it targets a cell
--- (`target = "cell"`) or at least one of its results hit (not missed, not
--- nullified).
function rules.terrain_reaction(battle, action, results)
  local skill = action.performed and action.cell and grid.skill_of(action)
  if not skill or not skill.element then return end
  local user = action.user
  if not user.cell then return end
  local landed = skill.target == "cell" or tablex.find(results or {}, function(r)
    return not r.missed and not r.cancelled
  end) ~= nil
  if not landed then return end
  local _, cells = grid.affected(battle, user, skill, action.cell)
  terrain.react(battle, skill.element, cells)
end

--- One unit's turn: upkeep and hazards, then a plan (player input through
--- the `player_controller` hook or the presenter's `command`, the `plan`
--- rule hook or the AI; all return a plan {move = path?, action = action?,
--- facing = dir?, choose_facing = boolean?}): move, act, face. A turn a
--- rule layer skips (Battle:skips_turn) is upkeep only. Terrain reacts
--- (rules.terrain_reaction) and follow-ups happen only if the action was
--- actually performed.
function rules.take_turn(battle, b)
  battle:turn_start(b)
  terrain.hazards(battle, b)
  local plan
  if b:can_act() and not battle:outcome() and not battle:skips_turn(b) then
    if b:is_party() and not b.auto then
      plan = battle:decide(b) or {}
    else
      plan = (battle:rule("plan") or require("fe.battle.tactical.ai").plan)(battle, b) or {}
    end
    if plan.move and #plan.move > 0 then
      grid.walk(battle, b, plan.move)
      plan.moved = true
    end
    if plan.action and not battle:outcome() then
      if plan.action.cell then grid.face(battle, b, grid.direction_between(b.cell, plan.action.cell)) end
      local results = battle:perform(plan.action)
      if plan.action.performed then
        rules.follow_ups(battle, plan.action, results)
        battle:settle()
      end
    end
    if plan.choose_facing and b:alive() and not battle:outcome() and battle.presenter and battle.presenter.choose_facing then
      plan.facing = battle.presenter:choose_facing(b, battle)
    end
    if plan.facing and b:alive() then grid.face(battle, b, plan.facing) end
  end
  battle:turn_end(b)
  return { ct = rules.ct_cost(plan and plan.moved, plan and plan.action and plan.action.performed) }
end

--- The initiative scheduler with tactical turns.
function rules.scheduler()
  local scheduler = require("fe.battle.schedulers.initiative").new({ cost = function(result) return result and result.ct end })
  scheduler.take_turn = rules.take_turn
  return scheduler
end

--- Where a battler added without a cell stands: the free cell nearest its
--- summoner (`summon.owner`), else nearest the first unit of its side on
--- the board. Raises an error when there is neither or no free cell.
function rules.arrival_cell(battle, unit)
  local owner = unit.summon and unit.summon.owner
  local anchor = owner and owner.cell and owner
    or tablex.find(battle:side(unit.side), function(b) return b ~= unit and b.cell ~= nil end)
  if not anchor then
    error("tactical battles need a cell for " .. unit:name() .. ": give it a `cell`, or an owner or ally on the board", 0)
  end
  local cell = grid.nearest_free(battle, anchor.cell)
  if not cell then error("no free cell for " .. unit:name() .. " on the board", 0) end
  unit.facing = unit.facing or anchor.facing
  return cell
end

local function arrive_on_board(battle, unit)
  if unit.obstacle and not unit.cell then error("obstacles need a cell", 0) end
  grid.place(battle, unit, unit.cell or rules.arrival_cell(battle, unit))
end

--- Set up the board on `world` and add the tactical rule layer: positional
--- damage (per battle), objectives (`win`/`lose`, defaults: defeat all),
--- terrain ticking each round and reacting to performed actions
--- (rules.terrain_reaction), corpses for fallen units, destroyed obstacles
--- changing their cell, placement of every battler when the battle starts
--- and of battlers added later (those without a `cell`: rules.arrival_cell),
--- units leaving the board when removed from the battle, scheduled
--- reinforcements arriving, and every terrain change reverted when the
--- battle ends or is aborted (`on_end` and `on_abort`).
---@param options? {win?: table[], lose?: table[]}
function rules.board(battle, world, options)
  options = options or {}
  battle.tactical = { world = world, occupied = {}, corpses = {}, cell_timers = {}, walkable_before = {},
    original = {}, reinforcements = {} }
  local win = options.win or { objectives.defeat_all("enemy") }
  local lose = options.lose or { objectives.defeat_all("party") }
  battle.objectives = { win = win, lose = lose }
  battle.objectives_text = objectives.describe(win)
  battle:add_rules(objectives.rules(win, lose))
  battle:add_rules({
    effect_hooks = { { phase = "modify", fn = rules.positional, priority = 40 } },
    setup = function(b)
      for _, unit in ipairs(b.battlers) do
        if not unit.cell or grid.unit_at(b, unit.cell) ~= unit then arrive_on_board(b, unit) end
      end
    end,
    on_round = function(b, round)
      if round > 1 then terrain.tick(b) end
      rules.arrive(b)
    end,
    on_turn_start = function(b) rules.arrive(b) end,
    after_action = rules.terrain_reaction,
    on_knocked_out = function(b, unit)
      grid.lay_down(b, unit)
      if unit.obstacle then terrain.destroyed(b, unit) end
    end,
    on_revived = function(b, unit) grid.stand_up(b, unit) end,
    on_battler_added = arrive_on_board,
    on_battler_removed = function(b, unit) grid.unplace(b, unit) end,
    on_end = function(b) terrain.revert(b) end,
    on_abort = function(b) terrain.revert(b) end,
  })
  return battle.tactical
end

return rules
