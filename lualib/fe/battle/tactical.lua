--- Grid tactics battles fought on a map: deployment, move/act/wait turns
--- ordered by initiative, ranges and areas of effect, height and facing
--- bonuses, follow-up (pincer) attacks, zones of control, knockback, terrain
--- reactions (fire burns grass, ice freezes water, lightning spreads through
--- water) and objectives.
---
--- ```lua
--- local outcome = tactical.start{
---   map = "ridge_pass",                         -- or the current field map
---   deploy = {tag = "deploy", max = 6},         -- players place units first
---   enemies = {{enemy = "bandit", cell = {x = 9, z = 3}}, ...},
---   win = {tactical.objectives.defeat_all()},
---   lose = {tactical.objectives.defeat_all("party"), tactical.objectives.protect("leader")},
--- }
--- ```
--- Skills use `range` and `area` ({shape, min, max, height}) from their data;
--- the attack skill falls back to settings.attack_range. Every rule is a
--- setting or hook, so a game can turn mechanics on or off.
local Battle = require("fe.battle.core")
local camera = require("fe.camera")
local data = require("fe.data")
local effects = require("fe.effects")
local party = require("fe.rpg.party")
local scene = require("fe.scene")
local sound = require("fe.sound")
local tablex = require("fe.tablex")
local task = require("fe.task")
local transition = require("fe.transition")
local tween = require("fe.tween")
local ui = require("fe.ui")
local vec = require("fe.vec")

local tactical = {
  settings = {
    world_name = "tactical",
    height_bonus = 0.12,       -- damage per unit of height above the target (capped at +/- 3)
    back_bonus = 0.3,          -- extra damage from behind
    side_bonus = 0.15,         -- extra damage from the side
    follow_up = true,          -- allies opposite a melee target attack too
    zone_of_control = true,    -- entering a cell next to a foe ends movement
    fall_damage = 0.08,        -- fraction of max HP per unit fallen beyond 2 when pushed
    attack_range = { shape = "diamond", min = 1, max = 1 },
    default_move = 4,
    default_jump = 2,
    camera = { pitch = 48, distance = 19, fov = 32 },
    ct_cost = { base = 100, no_move = -20, no_act = -20 },
    colors = { move = 0x4aa3ff88, attack = 0xff6a5a88, area = 0xffd04a99, deploy = 0x6aff9a66, path = 0xffffffcc },
  },
  --- Terrain reactions: when an element hits a cell with `tag`.
  reactions = {
    { element = "fire", tag = "grass", add = { "burning" }, remove = { "grass" }, turns = 3 },
    { element = "ice", tag = "water", add = { "ice" }, remove = { "water" }, walkable = true },
    { element = "fire", tag = "ice", add = { "water" }, remove = { "ice" } },
    { element = "thunder", tag = "water", spread = true, damage = 0.12 },
  },
  objectives = {},
  --- The battle in progress, if any.
  active = nil,
}

-------------------------------------------------------------------------------
-- Geometry helpers

local function key(cell) return vec.cell_key(cell) end

local function direction_between(a, b)
  local dx, dz = b.x - a.x, b.z - a.z
  if math.abs(dx) >= math.abs(dz) then return { x = dx >= 0 and 1 or -1, z = 0 } end
  return { x = 0, z = dz >= 0 and 1 or -1 }
end

--- Whether `cell` is within a shape centred on `origin`.
function tactical.in_shape(origin, cell, area, direction)
  local dx, dz = cell.x - origin.x, cell.z - origin.z
  local manhattan = math.abs(dx) + math.abs(dz)
  local chebyshev = math.max(math.abs(dx), math.abs(dz))
  local min, max = area.min or 0, area.max or 0
  local shape = area.shape or "diamond"
  if shape == "diamond" then return manhattan >= min and manhattan <= max end
  if shape == "ring" then return manhattan >= math.max(1, min) and manhattan <= max end
  if shape == "square" then return chebyshev >= min and chebyshev <= max end
  if shape == "cross" then return (dx == 0 or dz == 0) and manhattan >= min and manhattan <= max end
  direction = direction or { x = 0, z = 1 }
  local forward = dx * direction.x + dz * direction.z
  local lateral = dx * -direction.z + dz * direction.x
  if shape == "line" then return lateral == 0 and forward >= math.max(1, min) and forward <= max end
  if shape == "cone" then return forward >= math.max(1, min) and forward <= max and math.abs(lateral) < forward end
  return false
end

local function cell_height(world, cell)
  local c = world:cell(cell.x, cell.z, cell.layer or 0)
  return c and c.height or 0
end

local function range_of(skill)
  if skill.range then return skill.range end
  return tactical.settings.attack_range
end

local function area_of(skill)
  return skill.area or { shape = "diamond", min = 0, max = 0 }
end

-------------------------------------------------------------------------------
-- Battle-state helpers (battle.tactical holds the world and occupancy)

local function unit_at(battle, cell)
  return battle.tactical.occupied[key(cell)]
end

local function place(battle, b, cell)
  local t = battle.tactical
  if b.cell then t.occupied[key(b.cell)] = nil end
  b.cell = { x = cell.x, z = cell.z, layer = cell.layer or 0 }
  t.occupied[key(b.cell)] = b
  if b.entity then
    b.entity:place(vec.cell_centre(b.cell), b.cell.layer)
  end
end

local function step_rules(battle, b)
  local blocked, no_stop = {}, {}
  for _, other in ipairs(battle:living()) do
    if other ~= b and other.cell then
      if other.side == b.side then no_stop[#no_stop + 1] = other.cell else blocked[#blocked + 1] = other.cell end
    end
  end
  local jump = b:stat("jump")
  if jump <= 0 then jump = tactical.settings.default_jump end
  return { max_up = jump, max_down = jump + 1, blocked = blocked, no_stop = no_stop, blocked_tags = { "blocked" },
    tag_costs = { mud = 1, water = 1 } }
end

local function move_points(b)
  local move = b:stat("move")
  return move > 0 and move or tactical.settings.default_move
end

--- Cells a unit can move to this turn (zone of control applied). Each
--- entry carries `path`, the cells walked to reach it.
function tactical.reachable(battle, b)
  local world = battle.tactical.world
  local all = world:reachable(b.cell, move_points(b), step_rules(battle, b))
  local foes = battle:opponents(b)
  local function adjacent_to_foe(c)
    for _, foe in ipairs(foes) do
      if foe.cell and vec.manhattan(foe.cell, c) == 1 then return true end
    end
    return false
  end
  local out = {}
  for _, c in ipairs(all) do
    local path, blocked = {}, false
    local index = c.parent
    local node = c
    while index and index > 0 do
      table.insert(path, 1, { x = node.x, z = node.z, layer = node.layer })
      local parent = all[index]
      -- Moving out of a cell next to a foe is not allowed (except the start).
      if tactical.settings.zone_of_control and index > 1 and adjacent_to_foe(parent) then blocked = true end
      node, index = parent, parent.parent
    end
    if c.stop and not blocked then
      c.path = path
      out[#out + 1] = c
    end
  end
  return out
end

--- Path (list of cells) to a reachable entry.
function tactical.path_to(cells, target)
  for _, c in ipairs(cells) do
    if c.x == target.x and c.z == target.z and (c.layer or 0) == (target.layer or 0) then return c.path or {} end
  end
  return {}
end

--- Cells a skill can target from a cell.
function tactical.range_cells(battle, user, skill, from)
  from = from or user.cell
  local world = battle.tactical.world
  local r = range_of(skill)
  local cells = world:area(from, r.shape or "diamond", r.min or 0, r.max or 0, user.entity and user.entity.facing)
  local h0 = cell_height(world, from)
  return tablex.filter(cells, function(c)
    if r.height and math.abs(cell_height(world, c) - h0) > r.height then return false end
    if skill.los then
      local eye = { x = from.x + 0.5, y = h0 + 1.2, z = from.z + 0.5 }
      local goal = { x = c.x + 0.5, y = cell_height(world, c) + 0.8, z = c.z + 0.5 }
      if not world:line_of_sight(eye, goal, from.layer or 0) then return false end
    end
    return true
  end)
end

--- Units a skill aimed at `cell` would affect.
function tactical.affected(battle, user, skill, cell)
  local world = battle.tactical.world
  local a = area_of(skill)
  local cells = world:area(cell, a.shape or "diamond", a.min or 0, a.max or 0, direction_between(user.cell, cell))
  local units = {}
  for _, c in ipairs(cells) do
    local u = unit_at(battle, c)
    if u then
      local friendly = u.side == user.side
      local wants_allies = skill.target == "ally" or skill.target == "all_allies" or skill.target == "self"
      local wants_dead = skill.target == "dead_ally"
      local ok = (skill.target == "any" or skill.target == "everyone") or (wants_allies and friendly)
        or (wants_dead and friendly and not u:alive()) or (not wants_allies and not wants_dead and not friendly)
      if ok and (u:alive() or wants_dead) then units[#units + 1] = u end
    end
  end
  return units, cells
end

-------------------------------------------------------------------------------
-- Rules (registered once; active only for tactical battles)

effects.hook("modify", function(ctx)
  local battle = ctx.battle
  if not battle or not battle.tactical or ctx.effect.type ~= "damage" then return end
  local user, target = ctx.user, ctx.target
  if not user.cell or not target.cell then return end
  local world = battle.tactical.world
  local diff = cell_height(world, user.cell) - cell_height(world, target.cell)
  diff = math.max(-3, math.min(3, diff))
  ctx.height_bonus = diff * tactical.settings.height_bonus
  ctx.multiplier = ctx.multiplier * (1 + ctx.height_bonus)
  local facing = target.entity and target.entity.facing or { x = 0, z = 1 }
  local incoming = direction_between(target.cell, user.cell)
  local dot = facing.x * incoming.x + facing.z * incoming.z
  if dot < -0.5 then
    ctx.from_behind = true
    ctx.multiplier = ctx.multiplier * (1 + tactical.settings.back_bonus)
  elseif dot < 0.5 then
    ctx.from_side = true
    ctx.multiplier = ctx.multiplier * (1 + tactical.settings.side_bonus)
  end
end, 40)

--- {"type": "push", "distance": 1}: knock the target away; falls hurt.
effects.handler("push", function(ctx)
  local battle = ctx.battle
  if not battle or not battle.tactical or not ctx.target:alive() then return { moved = 0 } end
  local world = battle.tactical.world
  local dir = direction_between(ctx.user.cell, ctx.target.cell)
  local moved = 0
  for _ = 1, ctx.effect.distance or 1 do
    local next_cell = { x = ctx.target.cell.x + dir.x, z = ctx.target.cell.z + dir.z, layer = ctx.target.cell.layer }
    local c = world:cell(next_cell.x, next_cell.z, next_cell.layer)
    if not c or not c.walkable or unit_at(battle, next_cell) or c.height - cell_height(world, ctx.target.cell) > 0.5 then
      break
    end
    local drop = cell_height(world, ctx.target.cell) - c.height
    place(battle, ctx.target, next_cell)
    moved = moved + 1
    if drop > 2 then
      local life = require("fe.rpg.combatant").life
      ctx.target:change_resource(life, -math.floor(ctx.target:max_resource(life) * tactical.settings.fall_damage * (drop - 2)))
    end
  end
  return { moved = moved }
end)

--- Apply terrain reactions for an element landing on cells.
function tactical.react(battle, element, cells)
  if not element then return end
  local world = battle.tactical.world
  local timers = battle.tactical.cell_timers
  for _, cell in ipairs(cells) do
    local info = world:cell(cell.x, cell.z, cell.layer or 0)
    for _, reaction in ipairs(tactical.reactions) do
      if info and reaction.element == element and tablex.contains(info.tags, reaction.tag) then
        if reaction.spread then
          tactical.spread(battle, cell, reaction)
        else
          world:set_cell(cell.x, cell.z, cell.layer or 0, { add_tags = reaction.add or {}, remove_tags = reaction.remove or {},
            walkable = reaction.walkable })
          if reaction.turns then timers[key(cell)] = { cell = cell, turns = reaction.turns, tags = reaction.add } end
          battle:present("terrain", { cell = cell, reaction = reaction })
        end
      end
    end
  end
  tactical.update_overlays(battle)
end

--- Lightning through connected water: everyone standing in it is hurt.
function tactical.spread(battle, origin, reaction)
  local world = battle.tactical.world
  local seen, queue = {}, { origin }
  local life = require("fe.rpg.combatant").life
  while #queue > 0 do
    local cell = table.remove(queue)
    local k = key(cell)
    if not seen[k] then
      seen[k] = true
      local info = world:cell(cell.x, cell.z, cell.layer or 0)
      if info and tablex.contains(info.tags, reaction.tag) then
        local u = unit_at(battle, cell)
        if u and u:alive() then
          local amount = -u:change_resource(life, -math.floor(u:max_resource(life) * reaction.damage))
          battle:present("results", { action = {}, results = { { target = u, type = "damage", amount = amount } } })
        end
        for _, d in ipairs({ { 1, 0 }, { -1, 0 }, { 0, 1 }, { 0, -1 } }) do
          queue[#queue + 1] = { x = cell.x + d[1], z = cell.z + d[2], layer = cell.layer }
        end
      end
    end
  end
end

-------------------------------------------------------------------------------
-- Objectives: functions(battle) -> "victory" | "defeat" | nil

--- All units of a side defeated ("enemy" by default).
function tactical.objectives.defeat_all(side)
  side = side or "enemy"
  return { text = side == "enemy" and "Defeat all enemies" or "All allies fall", check = function(battle)
    if #battle:side(side) == 0 then return side == "enemy" and "victory" or "defeat" end
  end }
end

--- A specific unit (by id) is defeated.
function tactical.objectives.defeat(id, name)
  return { text = "Defeat " .. (name or id), check = function(battle)
    for _, b in ipairs(battle.battlers) do
      if (b.id == id or b.unit_id == id) and not b:alive() then return "victory" end
    end
  end }
end

--- Lose if this unit falls.
function tactical.objectives.protect(id, name)
  return { text = (name or id) .. " must survive", check = function(battle)
    for _, b in ipairs(battle.battlers) do
      if (b.id == id or b.unit_id == id) and not b:alive() then return "defeat" end
    end
  end }
end

--- Win when any unit of `side` stands on a cell with `tag`.
function tactical.objectives.reach(tag, side)
  side = side or "party"
  return { text = "Reach the marked area", check = function(battle)
    for _, b in ipairs(battle:side(side)) do
      local c = battle.tactical.world:cell(b.cell.x, b.cell.z, b.cell.layer or 0)
      if c and tablex.contains(c.tags, tag) then return "victory" end
    end
  end }
end

--- Win after `turns` rounds of the party.
function tactical.objectives.survive(turns)
  return { text = "Survive " .. turns .. " turns", check = function(battle)
    if (battle.tactical.party_turns or 0) >= turns * math.max(1, #battle:side("party", true)) then return "victory" end
  end }
end

-------------------------------------------------------------------------------
-- AI

local function expected(battle, user, skill, targets)
  local results = effects.resolve({ user = user, targets = targets, source = skill, battle = battle, preview = true })
  local life = require("fe.rpg.combatant").life
  local score = 0
  for _, r in ipairs(results) do
    local t = r.target
    local chance = r.hit_chance or 1
    if r.type == "damage" then
      local value = math.min(1, (r.amount or 0) / math.max(1, t:resource(life))) * chance
      if (r.amount or 0) >= t:resource(life) then value = value + 0.6 * chance end
      score = score + (t.side == user.side and -value * 1.5 or value)
    elseif r.type == "heal" then
      local missing = 1 - t:resource(life) / math.max(1, t:max_resource(life))
      score = score + (t.side == user.side and math.min(missing, (r.amount or 0) / math.max(1, t:max_resource(life))) or -0.5)
    end
  end
  return score
end

--- Plan a turn for a computer-controlled unit: {move = path, action = action, facing = dir}.
--- Games can replace it per battle with `rules.plan(battle, unit)`.
function tactical.plan(battle, b)
  local world = battle.tactical.world
  local cells = tactical.reachable(battle, b)
  local skills = b:skills()
  local attack = data.find("skills", "attack")
  if attack and not tablex.find(skills, function(s) return s.id == "attack" end) then
    table.insert(skills, 1, attack)
  end
  local foes = battle:opponents(b)
  local best
  local origin = b.cell
  for _, dest in ipairs(cells) do
    local from = { x = dest.x, z = dest.z, layer = dest.layer }
    local danger = 0
    for _, foe in ipairs(foes) do
      if vec.manhattan(foe.cell, from) <= 1 then danger = danger + 0.05 end
    end
    for _, skill in ipairs(skills) do
      if b:can_pay(skill.cost) and not skill.passive then
        local candidates = (skill.target == "ally" or skill.target == "all_allies") and battle:allies(b) or foes
        for _, target in ipairs(candidates) do
          if target.cell then
            local r = range_of(skill)
            local dir = direction_between(from, target.cell)
            if tactical.in_shape(from, target.cell, r, dir)
              and (not r.height or math.abs(cell_height(world, target.cell) - cell_height(world, from)) <= r.height) then
              place(battle, b, from)
              local affected = tactical.affected(battle, b, skill, target.cell)
              local score = #affected > 0 and expected(battle, b, skill, affected) - danger or -1
              place(battle, b, origin)
              if score > 0 and (not best or score > best.score) then
                best = { score = score, dest = from, skill = skill, cell = target.cell, targets = affected }
              end
            end
          end
        end
      end
    end
  end
  local plan = {}
  if best then
    plan.move = tactical.path_to(cells, best.dest)
    plan.action = { kind = "skill", user = b, skill = best.skill.id, targets = best.targets, cell = best.cell }
    plan.facing = direction_between(best.dest, best.cell)
  else
    -- Close in on the nearest foe.
    local target, distance = nil, math.huge
    for _, foe in ipairs(foes) do
      local d = vec.manhattan(foe.cell, b.cell)
      if d < distance then target, distance = foe, d end
    end
    if target then
      local dest, best_d = nil, distance
      for _, c in ipairs(cells) do
        local d = vec.manhattan(target.cell, c)
        if d < best_d then dest, best_d = c, d end
      end
      if dest then plan.move = tactical.path_to(cells, dest) end
      plan.facing = direction_between(dest or b.cell, target.cell)
    end
  end
  return plan
end

-------------------------------------------------------------------------------
-- Turn flow

--- Walk a unit along a path of cells (jumping between heights).
function tactical.walk(battle, b, path)
  local world = battle.tactical.world
  for _, cell in ipairs(path) do
    local from_h = cell_height(world, b.cell)
    local to_h = cell_height(world, cell)
    local goal = vec.cell_centre(cell, to_h)
    if math.abs(to_h - from_h) > 0.6 then
      b.entity:jump(goal, { height = math.max(0.6, to_h - from_h + 0.6), duration = 0.35, layer = cell.layer or 0 })
    else
      b.entity:move_to(goal, { path = false })
    end
    task.wait_until(function() return not b.entity:moving() end, 3)
    place(battle, b, cell)
  end
end

local function follow_ups(battle, action, results)
  if not tactical.settings.follow_up or not action.skill then return end
  local skill = data.find("skills", action.skill)
  if not skill or (skill.range and (skill.range.max or 1) > 1) or skill.target ~= "enemy" then return end
  for _, r in ipairs(results or {}) do
    local target = r.target
    if r.type == "damage" and target:alive() and target.cell then
      local opposite = { x = target.cell.x * 2 - action.user.cell.x, z = target.cell.z * 2 - action.user.cell.z,
        layer = target.cell.layer }
      local ally = unit_at(battle, opposite)
      if ally and ally.side == action.user.side and ally:can_act() and vec.manhattan(ally.cell, target.cell) == 1 then
        battle:present("follow_up", { user = ally, target = target })
        battle:perform({ kind = "skill", user = ally, skill = "attack", targets = { target }, cell = target.cell,
          follow_up = true })
      end
      break
    end
  end
end

local function tick_cells(battle)
  local world = battle.tactical.world
  for k, timer in pairs(battle.tactical.cell_timers) do
    timer.turns = timer.turns - 1
    if timer.turns <= 0 then
      world:set_cell(timer.cell.x, timer.cell.z, timer.cell.layer or 0, { remove_tags = timer.tags })
      battle.tactical.cell_timers[k] = nil
    end
  end
  tactical.update_overlays(battle)
end

local function take_turn(battle, b)
  battle:turn_start(b)
  local plan
  local world = battle.tactical.world
  local cell = world:cell(b.cell.x, b.cell.z, b.cell.layer or 0)
  if cell and tablex.contains(cell.tags, "burning") and b:alive() then
    local life = require("fe.rpg.combatant").life
    local amount = -b:change_resource(life, -math.floor(b:max_resource(life) * 0.1))
    battle:present("results", { action = {}, results = { { target = b, type = "damage", amount = amount } } })
  end
  if b:can_act() and not battle:outcome() then
    if b:is_party() and not b.auto then
      plan = battle:decide(b) or {}
    else
      plan = (battle.rules.plan or tactical.plan)(battle, b) or {}
    end
    if plan.move and #plan.move > 0 then tactical.walk(battle, b, plan.move) end
    if plan.action and not battle:outcome() then
      if plan.action.cell then
        b.entity:face({ x = plan.action.cell.x + 0.5 - b.entity.position.x, z = plan.action.cell.z + 0.5 - b.entity.position.z })
      end
      local results = battle:perform(plan.action)
      local skill = plan.action.skill and data.find("skills", plan.action.skill)
      if skill and plan.action.cell then
        local _, cells = tactical.affected(battle, b, skill, plan.action.cell)
        tactical.react(battle, skill.element, cells)
      end
      follow_ups(battle, plan.action, results)
    end
    if plan.facing and b.entity then b.entity:face(plan.facing) end
  end
  if b:is_party() then battle.tactical.party_turns = (battle.tactical.party_turns or 0) + 1 end
  battle:turn_end(b)
  tick_cells(battle)
  for _, u in ipairs(battle.battlers) do
    if not u:alive() and u.cell then
      battle.tactical.occupied[key(u.cell)] = nil
    end
  end
  local cost = tactical.settings.ct_cost
  local ct = cost.base
  local moved = plan and (plan.moved or (plan.move and #plan.move > 0))
  if not moved then ct = ct + cost.no_move end
  if not plan or not plan.action then ct = ct + cost.no_act end
  return { ct = ct }
end

-------------------------------------------------------------------------------
-- Presentation and player input

local Presenter = {}
Presenter.__index = Presenter
tactical.Presenter = Presenter

function tactical.update_overlays(battle)
  local world = battle.tactical.world
  local burning = world:cells_with_tag("burning")
  world:overlay("terrain:burning", burning, { color = 0xff7a2a66, pulse = 2, order = -1 })
  world:overlay("terrain:ice", world:cells_with_tag("ice"), { color = 0xbfe8ff55, order = -1 })
end

local TacticalScene = { name = "tactical", opaque = true }
TacticalScene.__index = TacticalScene

function TacticalScene:update(dt)
  local p = self.presenter
  if fe.input.pressed("camera_left") then p.rig:rotate(-90, true) end
  if fe.input.pressed("camera_right") then p.rig:rotate(90, true) end
  if fe.input.pressed("info") then p.top_down = not p.top_down p.rig:set_top_down(p.top_down) end
  p.rig:update(dt)
  for _, popup in ipairs(p.popups) do popup.age = popup.age + dt end
  p.popups = tablex.filter(p.popups, function(popup) return popup.age < 1.2 end)
end

function TacticalScene:draw()
  local p = self.presenter
  p.world:draw()
  tactical.draw_hud(p)
end

function Presenter:start(battle)
  if self.scene then return end -- deployment may start the presentation early
  self.battle = battle
  self.popups = {}
  local t = battle.tactical
  self.world = t.world
  for _, b in ipairs(battle.battlers) do
    if not b.entity then
      b.entity = self.world:spawn({
        position = vec.cell_centre(b.cell), layer = b.cell.layer, facing = b.facing or { x = 0, z = b:is_party() and -1 or 1 },
        sprite = { animation = b:sprite(), move_clip = "walk", idle_clip = "idle" }, mover = { speed = 4, collide = false },
        tags = { "unit", b.side },
      })
    end
    place(battle, b, b.cell)
  end
  self.rig = camera.rig(self.world, tactical.settings.camera)
  self.rig:look_at(vec.cell_centre(battle.battlers[1].cell))
  self.rig:snap()
  self.scene = setmetatable({ presenter = self }, TacticalScene)
  scene.push(self.scene)
  tactical.update_overlays(battle)
end

function Presenter:turn(payload)
  self.order = payload.order
  self.active = payload.battler
  self.rig:follow(payload.battler.entity)
  self:banner(payload.battler:name() .. "'s turn", 0.8)
  task.wait(0.3)
end

function Presenter:banner(text, duration)
  self.banner_text = text
  task.spawn(function()
    task.wait(duration or 1)
    if self.banner_text == text then self.banner_text = nil end
  end)
end

function Presenter:popup(b, text, color)
  local x, y = b.entity:screen_position(1.4)
  self.popups[#self.popups + 1] = { x = x, y = y, text = text, color = color or 0xffffffff, age = 0 }
end

function Presenter:action(action)
  self:banner(action.source.name, 0.9)
  local clips = fe.app.sprite_clips(action.user.entity:sprite().animation)
  if clips.attack then action.user.entity:clip("attack") end
  task.wait(0.35)
end

function Presenter:results(payload)
  for _, r in ipairs(payload.results) do
    if r.missed then
      self:popup(r.target, "Miss", ui.theme.muted)
    elseif r.type == "damage" then
      self:popup(r.target, tostring(r.amount), r.critical and 0xffd060ff or 0xffffffff)
      r.target.entity:sprite({ flash = { 1, 1, 1, 0.8 } })
      task.spawn(function()
        tween.run(0.25, function(k) if r.target.entity:valid() then r.target.entity:sprite({ flash = { 1, 1, 1, 0.8 * (1 - k) } }) end end)
      end)
    elseif (r.type == "heal" or r.type == "revive") and (r.amount or 0) > 0 then
      self:popup(r.target, tostring(r.amount), ui.theme.good)
    end
  end
  task.wait(0.4)
end

function Presenter:knocked_out(b)
  task.spawn(function()
    tween.run(0.4, function(k) b.entity:sprite({ tint = { 1, 1, 1, 1 - k } }) end)
    b.entity:sprite({ visible = false })
  end)
end

function Presenter:follow_up(payload)
  self:banner("Follow-up: " .. payload.user:name(), 0.8)
end

function Presenter:terrain(payload)
  sound.play("terrain_" .. tostring(payload.reaction.element))
end

function Presenter:finish(payload)
  self:banner(payload.outcome == "victory" and "Victory!" or payload.outcome == "defeat" and "Defeat..." or "Battle over", 2)
  if payload.outcome == "victory" and payload.rewards then
    task.wait(0.5)
  end
  task.wait(1.5)
  self.world:clear_overlay()
  self.world:cursor(nil)
  scene.remove(self.scene)
end

-- Cursor control on the grid: arrows move relative to the camera; the mouse picks.
local function move_cursor(p, cursor)
  local dx, dz = 0, 0
  if fe.input.repeating("left") then dx = -1 elseif fe.input.repeating("right") then dx = 1 end
  if fe.input.repeating("up") then dz = -1 elseif fe.input.repeating("down") then dz = 1 end
  if dx ~= 0 or dz ~= 0 then
    local yaw = math.floor((p.rig:yaw() % 360) / 90 + 0.5) % 4
    for _ = 1, yaw do dx, dz = dz, -dx end
    local gw, gd = p.world:grid_size()
    cursor.x = math.max(0, math.min(gw - 1, cursor.x + dx))
    cursor.z = math.max(0, math.min(gd - 1, cursor.z + dz))
    ui.sound("move")
  end
  if fe.input.mouse_moved() then
    local cell = p.world:pick(fe.input.mouse())
    if cell then cursor.x, cursor.z, cursor.layer = cell.x, cell.z, cell.layer end
  end
  p.world:cursor(cursor)
  p.hover = unit_at(p.battle, cursor)
  return cursor
end

local function contains_cell(cells, cell)
  for _, c in ipairs(cells) do
    if c.x == cell.x and c.z == cell.z then return c end
  end
end

--- Pick a cell from `cells`; returns it, or nil on cancel. Pressing the
--- optional `finish` action returns nil, true.
local function pick_cell(p, cells, start, on_hover, finish)
  local cursor = { x = start.x, z = start.z, layer = start.layer or 0 }
  task.frame()
  while true do
    move_cursor(p, cursor)
    if on_hover then on_hover(cursor) end
    if fe.input.pressed("confirm") then
      local c = contains_cell(cells, cursor)
      if c then ui.sound("confirm") return c end
      ui.sound("buzzer")
    elseif fe.input.pressed("cancel") then
      ui.sound("cancel")
      return nil
    elseif finish and fe.input.pressed(finish) then
      ui.sound("confirm")
      return nil, true
    end
    task.frame()
  end
end

local function choose_skill(p, b)
  local entries = {}
  local attack = data.find("skills", "attack")
  if attack then entries[#entries + 1] = { label = "Attack", value = attack } end
  for _, skill in ipairs(b:skills()) do
    if skill.id ~= "attack" and skill.scope ~= "field" then
      local cost = {}
      for resource, amount in pairs(skill.cost) do cost[#cost + 1] = amount .. resource:upper() end
      entries[#entries + 1] = { label = skill.name, right = table.concat(cost, " "), value = skill, enabled = b:can_pay(skill.cost) }
    end
  end
  local _, item = ui.choose(entries, { title = "Act", anchor = "bottom-left", width = 150, rows = 6, cancel = true })
  return item and item.value
end

--- Player turn: Move / Act / Wait, with undoable movement.
function Presenter:command(b, battle)
  local world = self.world
  local plan = { move = {} }
  local origin = { x = b.cell.x, z = b.cell.z, layer = b.cell.layer }
  local moved, acted = false, false
  while true do
    local options = {
      { label = "Move", value = "move", enabled = not moved },
      { label = "Act", value = "act", enabled = not acted },
      { label = "Wait", value = "wait" },
    }
    if moved and not acted then options[#options + 1] = { label = "Undo move", value = "undo" } end
    local _, choice = ui.choose(options, { anchor = "bottom-left", width = 90, rows = 4 })
    choice = choice and choice.value
    if choice == "move" then
      local cells = tactical.reachable(battle, b)
      world:overlay("move", cells, { color = tactical.settings.colors.move, pulse = 1.5 })
      local dest = pick_cell(self, cells, b.cell, function(cursor)
        local path = tactical.path_to(cells, cursor)
        table.insert(path, 1, b.cell)
        world:overlay_path("path", #path > 1 and path or {}, tactical.settings.colors.path)
      end)
      world:clear_overlay("move")
      world:clear_overlay("path")
      if dest then
        local path = tactical.path_to(cells, dest)
        tactical.walk(battle, b, path)
        moved = true
        plan.moved = true
      end
    elseif choice == "undo" then
      place(battle, b, origin)
      moved = false
      plan.moved = false
    elseif choice == "act" then
      local skill = choose_skill(self, b)
      if skill then
        local cells = tactical.range_cells(battle, b, skill)
        world:overlay("range", cells, { color = tactical.settings.colors.attack })
        local target = pick_cell(self, cells, cells[1] or b.cell, function(cursor)
          if contains_cell(cells, cursor) then
            local units, area = tactical.affected(battle, b, skill, cursor)
            world:overlay("area", area, { color = tactical.settings.colors.area, order = 1 })
            self.forecast = { skill = skill, units = units,
              results = #units > 0 and effects.resolve({ user = b, targets = units, source = skill, battle = battle, preview = true }) or {} }
          else
            world:clear_overlay("area")
            self.forecast = nil
          end
        end)
        world:clear_overlay("range")
        world:clear_overlay("area")
        self.forecast = nil
        if target then
          local units = tactical.affected(battle, b, skill, target)
          plan.action = { kind = "skill", user = b, skill = skill.id, targets = units, cell = { x = target.x, z = target.z, layer = target.layer } }
          acted = true
          -- The action resolves after the facing choice, ending the turn.
          plan.facing = direction_between(b.cell, target)
          world:cursor(nil)
          return plan
        end
      end
    elseif choice == "wait" then
      world:cursor(nil)
      local facing = self:choose_facing(b)
      plan.facing = facing
      return plan
    end
  end
end

--- Arrow keys pick a facing direction; Confirm ends the turn.
function Presenter:choose_facing(b)
  local directions = { { x = 0, z = -1 }, { x = 1, z = 0 }, { x = 0, z = 1 }, { x = -1, z = 0 } }
  local index = 1
  self.choosing_facing = true
  task.frame()
  while not fe.input.pressed("confirm") do
    for i, action in ipairs({ "up", "right", "down", "left" }) do
      if fe.input.pressed(action) then index = i end
    end
    b.entity:face(directions[index])
    task.frame()
  end
  self.choosing_facing = false
  return directions[index]
end

--- HUD: turn order, active/hover unit panels, forecast, objectives.
function tactical.draw_hud(p)
  local cw, ch = fe.canvas.size()
  local life = require("fe.rpg.combatant").life
  local bx = 8
  for i, b in ipairs(p.order or {}) do
    if i > 10 then break end
    ui.window(bx, 6, 30, 14)
    ui.text(b:name():sub(1, 4), bx + 4, 9, { color = b:is_party() and ui.theme.good or ui.theme.bad })
    bx = bx + 32
  end
  local function panel(b, x, y)
    ui.window(x, y, 128, 38)
    ui.text(b:name(), x + 6, y + 5, { color = b:is_party() and ui.theme.good or ui.theme.bad })
    ui.text(string.format("%s %d/%d", life:upper(), b:resource(life), b:max_resource(life)), x + 6, y + 16)
    ui.gauge(x + 6, y + 27, 116, 4, b:resource(life), b:max_resource(life), ui.theme.gauges.hp)
  end
  if p.active then panel(p.active, 8, 30) end -- clear of the command menu at the bottom left
  if p.hover and p.hover ~= p.active then panel(p.hover, cw - 136, ch - 44) end
  if p.forecast and #p.forecast.results > 0 then
    local lines = { "{color:heading}" .. p.forecast.skill.name .. "{color:reset}" }
    for _, r in ipairs(p.forecast.results) do
      if r.type == "damage" or r.type == "heal" then
        lines[#lines + 1] = string.format("%s: %s%d  %d%%", r.target:name(), r.type == "heal" and "+" or "-",
          r.amount or 0, math.floor((r.hit_chance or 1) * 100 + 0.5))
      end
    end
    local text = table.concat(lines, "\n")
    local _, h = ui.measure(text, { width = 150 })
    ui.window(cw - 168, 28, 160, h + 12)
    ui.text(text, cw - 162, 34, { width = 150 })
  end
  if p.battle.objectives_text then
    ui.text(p.battle.objectives_text, cw - 8 - ui.measure(p.battle.objectives_text), 9, { color = ui.theme.muted })
  end
  if p.choosing_facing then
    ui.text("Choose a facing, then " .. require("fe.input").prompt("confirm"), 0, ch - 58, { width = cw, align = "center" })
  end
  if p.banner_text then
    local bw = ui.measure(p.banner_text) + 24
    ui.window(math.floor((cw - bw) / 2), 26, bw, 18)
    ui.text(p.banner_text, math.floor((cw - bw) / 2) + 12, 31, { color = ui.theme.heading })
  end
  for _, popup in ipairs(p.popups) do
    fe.canvas.text(popup.text, popup.x - 40, popup.y - math.min(1, popup.age * 4) * 10,
      { color = popup.color, outline = 0x000000ff, width = 80, align = "center" })
  end
end

-------------------------------------------------------------------------------
-- Deployment

local function deploy(p, battle, options, reveal)
  local world = battle.tactical.world
  local tag = options.deploy.tag or "deploy"
  local cells = world:cells_with_tag(tag)
  local max = math.min(options.deploy.max or 6, #cells)
  world:overlay("deploy", cells, { color = tactical.settings.colors.deploy, pulse = 1 })
  local roster = party.roster()
  local placed = {}
  local slot = 1
  for _, actor in ipairs(roster) do
    if slot > max then break end
    local b = battle:add({ actor = actor, side = "party" })
    b.cell = cells[slot]
    place(battle, b, cells[slot])
    placed[#placed + 1] = b
    slot = slot + 1
  end
  p:start(battle)
  reveal()
  p:banner("Deploy your units - Confirm to swap, Menu to begin", 3)
  local selected
  while true do
    local cell, done = pick_cell(p, cells, cells[1], nil, "menu")
    if done then break end
    if not cell then
      selected = nil
    else
      local u = unit_at(battle, cell)
      if not selected then
        selected = u
      else
        local from = selected.cell
        if u then place(battle, u, from) end
        place(battle, selected, cell)
        selected = nil
      end
    end
  end
  world:clear_overlay("deploy")
  return placed
end

-------------------------------------------------------------------------------
-- Entry point

local function initiative()
  local scheduler = require("fe.battle.schedulers.initiative").new({ cost = function(result) return result and result.ct end })
  scheduler.take_turn = take_turn
  return scheduler
end

--- Run a tactics battle (inside a task). See the module comment.
--- Party units with `auto` (or every party unit with `options.auto`) are
--- planned like enemies: guests, or unattended tests and replays.
---@param options {map?: string, units?: table[], enemies?: table[], deploy?: table, win?: table[], lose?: table[], music?: string, rules?: table, auto?: boolean}
function tactical.start(options)
  local world = options.map and fe.world.load(tactical.settings.world_name, options.map) or require("fe.field").world
  local field_scene_hidden = options.map ~= nil
  local rules = tablex.copy(options.rules or {})
  local win = options.win or { tactical.objectives.defeat_all("enemy") }
  local lose = options.lose or { tactical.objectives.defeat_all("party") }
  rules.outcome = function(battle)
    for _, o in ipairs(lose) do
      if o.check(battle) == "defeat" then return "defeat" end
    end
    for _, o in ipairs(win) do
      if o.check(battle) == "victory" then return "victory" end
    end
  end
  local b = Battle({ rules = rules, scheduler = initiative(), escapable = false })
  tactical.active = b
  b.tactical = { world = world, occupied = {}, cell_timers = {} }
  local texts = {}
  for _, o in ipairs(win) do texts[#texts + 1] = o.text end
  b.objectives_text = table.concat(texts, " / ")
  local presenter = setmetatable({}, Presenter)
  b.presenter = presenter
  for _, spec in ipairs(options.units or {}) do
    local u = b:add({ actor = party.actor(spec.actor), side = "party" })
    u.cell, u.facing, u.unit_id, u.auto = spec.cell, spec.facing, spec.actor, spec.auto
  end
  for i, spec in ipairs(options.enemies or {}) do
    local u = b:add({ enemy = spec.enemy, side = "enemy", name = spec.name })
    u.cell, u.facing, u.unit_id = spec.cell, spec.facing, spec.id or (spec.enemy .. i)
  end
  if options.music then sound.music(options.music) end
  local hidden = field_scene_hidden
  if hidden then transition.out("fade", 0.3) end
  local function reveal()
    if hidden then transition.into("fade", 0.3) end
    hidden = false
  end
  if options.deploy then
    deploy(presenter, b, options, reveal)
  else
    presenter:start(b)
    reveal()
  end
  if options.auto then
    for _, u in ipairs(b:side("party", true)) do u.auto = true end
  end
  local outcome, rewards = b:run()
  tactical.active = nil
  if options.map then fe.world.unload(tactical.settings.world_name) end
  return outcome, rewards
end

tactical.place = place
tactical.unit_at = unit_at
return tactical
