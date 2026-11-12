--- The battle system: active-time gauges, commands, damage. No drawing and
--- no world: the battle scene (game/battle/scene.lua) asks this module what
--- happens and shows it, and the tests (tests/rules.lua) drive it directly.
---
--- Every unit has a gauge that fills at a rate set by its speed. A unit whose
--- gauge is full is *ready*: an enemy acts at once (its pattern picks the
--- skill), a hero waits for a command. Time stands still while a command is
--- being chosen or an action is playing; the scene only calls `advance`
--- when neither is the case.
---
--- ```lua
--- local battle = rules.new { heroes = {"cael", "rowan"}, troop = "gate", state = party.state }
--- local ready = battle:advance(dt)          -- units whose gauge just filled, in order
--- local events = battle:act(unit, { skill = "attack", target = foe })
--- battle:outcome()                          -- "victory", "defeat", "threshold" or nil
--- ```
local rules = {}
local Battle = {}
Battle.__index = Battle

--- Seconds a unit of speed 60 takes to fill its gauge.
rules.FILL_SECONDS = 4.0

local tables = {}
local function data(name)
  tables[name] = tables[name] or fe.json.read("data/" .. name .. ".json")
  return tables[name]
end

function rules.reload()
  tables = {}
end

function rules.skill(id)
  return data("skills")[id] or error("no skill '" .. tostring(id) .. "'")
end

function rules.item(id)
  return data("items")[id] or error("no item '" .. tostring(id) .. "'")
end

local function hero_unit(id, saved)
  local record = data("heroes")[id] or error("no hero '" .. tostring(id) .. "'")
  saved = saved or {}
  return { id = id, name = record.name, side = "party", hero = true, record = record,
    max_hp = record.hp, hp = saved.hp or record.hp, max_mp = record.mp, mp = saved.mp or record.mp,
    atk = record.atk, def = record.def, mag = record.mag, res = record.res, spd = record.spd,
    corruption = record.corruption and (saved.corruption or 0) or nil,
    gauge = 0, status = {}, ko = (saved.hp or record.hp) <= 0 }
end

local function enemy_unit(id, index, weakened)
  local record = data("enemies")[id] or error("no enemy '" .. tostring(id) .. "'")
  return { id = id, name = record.name, side = "enemy", record = record, index = index, boss = record.boss,
    max_hp = record.hp, hp = math.floor(record.hp * (weakened or 1)), max_mp = 0, mp = 0,
    atk = record.atk, def = record.def, mag = record.mag, res = record.res, spd = record.spd,
    pattern = record.pattern, step = 0, gauge = 0, status = {}, ko = false }
end

--- options: `heroes` (ids in formation order), `troop` (an id of
--- data/troops.json), `state` (per hero `{hp, mp, corruption}` carried
--- between battles), `items` (id -> count, shared and changed in place),
--- `seed`.
function rules.new(options)
  local self = setmetatable({}, Battle)
  self.rng = fe.random.new(options.seed)
  self.troop = data("troops")[options.troop] or error("no troop '" .. tostring(options.troop) .. "'")
  self.heroes, self.enemies, self.units = {}, {}, {}
  self.items = options.items or {}
  self.state = options.state or {}
  for _, id in ipairs(options.heroes) do
    local unit = hero_unit(id, self.state[id])
    unit.gauge = self.rng:range(0.3, 0.75)
    self.heroes[#self.heroes + 1] = unit
    self.units[#self.units + 1] = unit
  end
  local names = {}
  for index, id in ipairs(self.troop.enemies) do
    local unit = enemy_unit(id, index, self.troop.weakened)
    unit.gauge = self.rng:range(0, 0.25)
    names[unit.name] = (names[unit.name] or 0) + 1
    self.enemies[#self.enemies + 1] = unit
    self.units[#self.units + 1] = unit
  end
  -- "Fortress Guard A", "Fortress Guard B" when a name repeats.
  local seen = {}
  for _, unit in ipairs(self.enemies) do
    if names[unit.name] > 1 then
      seen[unit.name] = (seen[unit.name] or 0) + 1
      unit.name = unit.name .. " " .. string.char(64 + seen[unit.name])
    end
  end
  self.time = 0
  return self
end

function rules.alive(unit)
  return not unit.ko
end

function Battle:allies_of(unit)
  return unit.side == "party" and self.heroes or self.enemies
end

function Battle:foes_of(unit)
  return unit.side == "party" and self.enemies or self.heroes
end

local function living(list)
  local out = {}
  for _, unit in ipairs(list) do
    if not unit.ko then out[#out + 1] = unit end
  end
  return out
end
rules.living = living

--- Fill the gauges by `dt` seconds. Returns the units that became ready, the
--- fastest first. A ready unit stays ready (its gauge full) until it acts.
function Battle:advance(dt)
  self.time = self.time + dt
  local ready = {}
  for _, unit in ipairs(self.units) do
    if not unit.ko and unit.gauge < 1 then
      local rate = unit.spd / 60 / rules.FILL_SECONDS * (unit.haste or 1)
      unit.gauge = unit.gauge + dt * rate
      if unit.gauge >= 1 then
        unit.gauge = 1
        ready[#ready + 1] = unit
      end
    end
  end
  table.sort(ready, function(a, b) return a.spd > b.spd end)
  return ready
end

--- What an enemy does with its turn: the next skill of its pattern (or what
--- `unit.script(battle, unit)` returns) at a living hero.
function Battle:enemy_action(unit)
  if unit.script then
    local action = unit.script(self, unit)
    if action then return action end
  end
  unit.step = unit.step % #unit.pattern + 1
  local id = unit.pattern[unit.step]
  local skill = rules.skill(id)
  local action = { skill = id }
  if skill.target == "enemy" then
    local targets = living(self.heroes)
    if unit.avoid then
      local kept = {}
      for _, target in ipairs(targets) do
        if target.id ~= unit.avoid then kept[#kept + 1] = target end
      end
      if #kept > 0 then targets = kept end
    end
    action.target = #targets > 0 and self.rng:pick(targets) or nil
  end
  return action
end

--- Who an action lands on: the chosen unit, or everyone of a side.
function Battle:targets(unit, kind, chosen)
  if kind == "self" then return { unit } end
  if kind == "enemy" or kind == "ally" or kind == "fallen" then return { chosen } end
  if kind == "enemies" then return living(self:foes_of(unit)) end
  if kind == "allies" then return living(self:allies_of(unit)) end
  error("unknown target kind '" .. tostring(kind) .. "'")
end

--- Units a hero may point a skill or item at.
function Battle:choices(unit, kind)
  if kind == "enemy" then return living(self:foes_of(unit)) end
  if kind == "ally" then return living(self:allies_of(unit)) end
  if kind == "fallen" then
    local out = {}
    for _, ally in ipairs(self:allies_of(unit)) do
      if ally.ko then out[#out + 1] = ally end
    end
    return out
  end
  return {}
end

--- Why a hero cannot use a skill right now, or nil when they can.
function Battle:blocked(unit, id)
  local skill = rules.skill(id)
  if (skill.mp or 0) > unit.mp then return "Not enough MP" end
  if unit.only and unit.only ~= id then return unit.only_reason or "Not now" end
  return nil
end

local function variance(self)
  return self.rng:range(0.92, 1.08)
end

--- Damage of `power` from `unit` on `target`, before the target's defences
--- of the moment (guard, ward) and after its armour.
function Battle:damage(unit, target, skill)
  local magic = skill.kind == "magic"
  local attack = (magic and unit.mag or unit.atk) * skill.power * 5.2
  local armour = (magic and target.res or target.def) * 2.4
  local amount = math.max(1, attack - armour) * variance(self)
  local crit = not magic and self.rng:chance(0.08)
  if crit then amount = amount * 1.5 end
  return math.floor(amount + 0.5), crit
end

--- Apply damage to a unit: cover, guard, ward, invulnerability, the
--- threshold a scripted boss stops at, and KO. Appends events.
function Battle:hurt(unit, target, amount, crit, element, events)
  -- Someone standing in the way takes the blow meant for an ally.
  if target.side == "party" then
    for _, ally in ipairs(self.heroes) do
      if ally ~= target and not ally.ko and ally.status.cover then
        events[#events + 1] = { type = "cover", unit = ally, target = target }
        target = ally
        break
      end
    end
  end
  if target.invulnerable then
    events[#events + 1] = { type = "damage", unit = target, amount = 0, ineffective = true }
    return
  end
  if target.status.guard then amount = amount * 0.5 end
  if target.status.ward then
    amount = amount * 0.66
    target.status.ward = target.status.ward - 1
    if target.status.ward <= 0 then
      target.status.ward = nil
      events[#events + 1] = { type = "status", unit = target, status = "ward", on = false }
    end
  end
  amount = math.max(1, math.floor(amount + 0.5))
  local floor = 0
  if target.threshold and target.hp > target.threshold then floor = target.threshold end
  if self.no_ko and target.side == "party" then floor = 1 end
  local hp = math.max(floor, target.hp - amount)
  local dealt = target.hp - hp
  target.hp = hp
  local event = { type = "damage", unit = target, amount = dealt, crit = crit, element = element }
  events[#events + 1] = event
  if target.threshold and target.hp <= target.threshold and not target.threshold_hit then
    target.threshold_hit = true
    self.threshold_reached = target
  end
  if target.hp <= 0 then
    target.ko, target.gauge, target.status = true, 0, {}
    event.ko = true
  end
end

local function tick_statuses(unit, events)
  -- Statuses counted in the unit's own turns run down as it acts.
  for _, name in ipairs { "cover" } do
    if unit.status[name] then
      unit.status[name] = unit.status[name] - 1
      if unit.status[name] <= 0 then
        unit.status[name] = nil
        events[#events + 1] = { type = "status", unit = unit, status = name, on = false }
      end
    end
  end
end

--- Carry out an action: `{skill = id, target = unit}` or `{item = id,
--- target = unit}`. Empties the actor's gauge and returns what happened, in
--- order: `use`, then `damage`/`heal`/`mp`/`revive`/`status`/`cover`/
--- `delay`/`corruption`/`message`/`script` events.
function Battle:act(unit, action)
  local events = {}
  unit.gauge = 0
  if unit.status.guard then unit.status.guard = nil end
  tick_statuses(unit, events)
  if action.item then
    local item = rules.item(action.item)
    self.items[action.item] = (self.items[action.item] or 0) - 1
    events[#events + 1] = { type = "use", unit = unit, name = item.name, item = action.item, fx = item.fx,
      targets = { action.target } }
    local target = action.target
    if item.kind == "heal" and not target.ko then
      local amount = math.min(item.amount, target.max_hp - target.hp)
      target.hp = target.hp + amount
      events[#events + 1] = { type = "heal", unit = target, amount = amount }
    elseif item.kind == "mp" and not target.ko then
      local amount = math.min(item.amount, target.max_mp - target.mp)
      target.mp = target.mp + amount
      events[#events + 1] = { type = "mp", unit = target, amount = amount }
    elseif item.kind == "revive" and target.ko then
      target.ko, target.hp, target.gauge = false, math.floor(target.max_hp * item.amount), 0
      events[#events + 1] = { type = "revive", unit = target, amount = target.hp }
    else
      events[#events + 1] = { type = "message", text = "No effect." }
    end
    return events
  end

  local skill = rules.skill(action.skill)
  unit.mp = unit.mp - (skill.mp or 0)
  local targets = self:targets(unit, skill.target, action.target)
  -- A target that fell while the command waited: the blow goes to another.
  if (skill.target == "enemy" or skill.target == "ally") and (not targets[1] or targets[1].ko) then
    local others = living(skill.target == "enemy" and self:foes_of(unit) or self:allies_of(unit))
    targets = { others[1] }
  end
  events[#events + 1] = { type = "use", unit = unit, name = skill.name, skill = action.skill, fx = skill.fx,
    banner = skill.banner, animation = skill.animation, impact = skill.impact, targets = targets,
    element = skill.element, kind = skill.kind }
  if skill.kind == "guard" then
    unit.status.guard = true
    events[#events + 1] = { type = "status", unit = unit, status = "guard", on = true }
  elseif skill.kind == "charge" then
    events[#events + 1] = { type = "message", text = skill.message }
  elseif skill.kind == "script" then
    events[#events + 1] = { type = "script", unit = unit, skill = action.skill }
  elseif skill.kind == "physical" or skill.kind == "magic" then
    for _, target in ipairs(targets) do
      if target and not target.ko then
        local amount, crit = self:damage(unit, target, skill)
        self:hurt(unit, target, amount, crit, skill.element, events)
        if skill.delay and not target.ko and not target.invulnerable then
          target.gauge = math.max(0, target.gauge - skill.delay)
          events[#events + 1] = { type = "delay", unit = target }
        end
      end
    end
    if skill.corruption and unit.corruption then
      unit.corruption = math.min(100, unit.corruption + skill.corruption)
      events[#events + 1] = { type = "corruption", unit = unit, value = unit.corruption }
    end
  elseif skill.kind == "heal" then
    for _, target in ipairs(targets) do
      if target and not target.ko then
        local amount = math.floor((unit.mag * skill.power + skill.base) * variance(self) + 0.5)
        amount = math.min(amount, target.max_hp - target.hp)
        target.hp = target.hp + amount
        events[#events + 1] = { type = "heal", unit = target, amount = amount }
      end
    end
  elseif skill.kind == "status" then
    for _, target in ipairs(targets) do
      if target and not target.ko then
        target.status[skill.status] = skill.turns or true
        events[#events + 1] = { type = "status", unit = target, status = skill.status, on = true }
      end
    end
  end
  return events
end

--- "victory" when no enemy stands, "defeat" when no hero does, "threshold"
--- when a scripted boss reached the HP it may not go below, else nil.
function Battle:outcome()
  if self.threshold_reached then return "threshold" end
  if #living(self.enemies) == 0 then return "victory" end
  if #living(self.heroes) == 0 then return "defeat" end
  return nil
end

--- Write the heroes' HP, MP and corruption back to the state table. The
--- fallen get up with 1 HP: there are no inns on a battlefield.
function Battle:save()
  for _, unit in ipairs(self.heroes) do
    self.state[unit.id] = { hp = math.max(1, unit.hp), mp = unit.mp, corruption = unit.corruption }
  end
end

return rules
