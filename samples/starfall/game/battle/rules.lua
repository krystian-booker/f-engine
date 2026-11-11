--- Starfall's battle rules, with no drawing in them: the battle scene (and the
--- tests) ask what happens next and are handed lists of events to show.
---
--- A battle runs in rounds. Every unit acts once a round in order of speed;
--- the order is known, so it is shown as a timeline.
---
---   * Break. Each foe has a break gauge. Damage chips it, weaknesses chip it
---     twice as fast. At zero the foe is broken: it loses its next turn and
---     takes half again as much damage until then.
---   * Bonus phase. Whoever broke a foe acts once more at the end of the
---     round. When every foe standing is broken at once (a sweep), the whole
---     party gets the bonus action, and then a Unison: the party picks one of
---     its equipped echoes and plays that echo's finishing move.
---   * Surge. Taking and dealing blows fills each hero's surge gauge; a full
---     gauge buys their strongest move.
---   * Echoes are equipment: each hero wears one, which adds its skills, a
---     stat bonus, sometimes a passive (Vanguard covers wounded allies) and
---     the Unison it offers.
---   * Elements. A foe's affinity to an element is weak, resist, null or
---     absorb. It is unknown until the party has seen it, and remembered in
---     the bestiary afterwards.
---   * Openings. A battle may start preemptive (the foes miss the first
---     round) or as an ambush (the party does).
---
--- Nothing here is fixed by the engine: sides, resources, the attack skill and
--- every number come from data/*.json and this file.
local data = require("game.data")

local rules = {}
rules.__index = rules

rules.ALLIES, rules.FOES = "allies", "foes"
rules.settings = {
  preemptive = 0.12, ambush = 0.05,
  weak = 1.5, resist = 0.5, broken = 1.5, defend = 0.5, cover = 0.7, crit = 0.05, crit_scale = 1.5,
  flee = 0.6, surge_hit = 120, surge_act = 6, surge_defend = 10,
}

local function other(side)
  return side == rules.ALLIES and rules.FOES or rules.ALLIES
end

-- ---------------------------------------------------------------- units

local function unit(side, key, record, stats)
  return {
    key = key, side = side, id = record.id, name = record.name, record = record,
    stats = stats, max_hp = stats.hp, hp = stats.hp, max_mp = stats.mp, mp = stats.mp,
    break_max = record["break"] or 0, break_left = record["break"] or 0, broken = false, lost_turn = false,
    affinity = record.affinity or {}, status = {}, defending = false, surge = 0,
    skills = {}, hero = false,
  }
end

--- A party member. `record` is the hero's data, `stats` their stats now,
--- `saved` = {hp, mp, surge, echo} from the save, `rank` their echo's rank.
function rules.hero(record, stats, saved, rank)
  local u = unit(rules.ALLIES, record.id, record, stats)
  u.hero = true
  u.hp = math.max(0, math.min(stats.hp, saved.hp or stats.hp))
  u.mp = math.max(0, math.min(stats.mp, saved.mp or stats.mp))
  u.surge = saved.surge or 0
  u.attack, u.surge_skill = record.attack, record.surge
  for _, id in ipairs(record.skills or {}) do u.skills[#u.skills + 1] = id end
  local echo = saved.echo and data.find("echoes", saved.echo)
  if echo then
    u.echo, u.passive = echo.id, echo.passive
    for _, entry in ipairs(echo.skills or {}) do
      if (rank or 1) >= (entry.rank or 1) then u.skills[#u.skills + 1] = entry.id end
    end
  end
  return u
end

local function foe(record, key)
  local stats = {}
  for name, value in pairs(record.stats) do stats[name] = value end
  local u = unit(rules.FOES, key, record, stats)
  u.ai = record.skills or {}
  return u
end

function rules.alive(u)
  return u.hp > 0 and not u.gone
end

-- ---------------------------------------------------------------- setup

--- options: party (list of rules.hero units), troop (id), rng (RandomStream),
--- opening ("normal" | "preemptive" | "ambush"; rolled when nil), known (the
--- bestiary: enemy id -> {element = affinity}), consume(item id) -> boolean.
function rules.new(options)
  local troop = data.get("troops", options.troop)
  local self = setmetatable({
    rng = options.rng or fe.random.new(), troop = troop, boss = troop.boss == true,
    allies = options.party, foes = {}, round = 0, queue = {}, breakers = {}, phase = "round",
    known = options.known or {}, consume = options.consume or function() return true end,
    damage_taken = 0, outcome = nil, sweep = false, unison_done = false, flee_tries = 0,
  }, rules)
  local counts = {}
  for _, id in ipairs(troop.enemies) do
    counts[id] = (counts[id] or 0) + 1
    self.foes[#self.foes + 1] = foe(data.get("enemies", id), id .. counts[id])
  end
  for _, f in ipairs(self.foes) do
    local several = counts[f.id] > 1
    if several then f.name = f.name .. " " .. string.char(64 + tonumber(f.key:match("%d+$"))) end
  end
  self.opening = options.opening or self:roll_opening()
  return self
end

function rules:roll_opening()
  if self.boss then return "normal" end
  local roll = self.rng:float()
  if roll < rules.settings.preemptive then return "preemptive" end
  if roll > 1 - rules.settings.ambush then return "ambush" end
  return "normal"
end

function rules:side(name)
  return name == rules.ALLIES and self.allies or self.foes
end

function rules:living(side)
  local out = {}
  for _, u in ipairs(self:side(side)) do
    if rules.alive(u) then out[#out + 1] = u end
  end
  return out
end

--- A stat with the unit's buffs and debuffs applied.
function rules:stat(u, name)
  local value = u.stats[name] or 0
  for id in pairs(u.status) do
    local status = data.get("statuses", id)
    if status.stat == name then value = value * status.scale end
  end
  return value
end

local function by_speed(self, units)
  local order = {}
  for index, u in ipairs(units) do order[#order + 1] = { u = u, spd = self:stat(u, "spd"), index = index } end
  table.sort(order, function(a, b)
    if a.spd ~= b.spd then return a.spd > b.spd end
    if a.u.side ~= b.u.side then return a.u.side == rules.ALLIES end
    return a.index < b.index
  end)
  local out = {}
  for i, entry in ipairs(order) do out[i] = entry.u end
  return out
end

function rules:everyone()
  local all = {}
  for _, u in ipairs(self.allies) do if rules.alive(u) then all[#all + 1] = u end end
  for _, u in ipairs(self.foes) do if rules.alive(u) then all[#all + 1] = u end end
  return all
end

function rules:start_round()
  self.round = self.round + 1
  self.phase, self.breakers, self.sweep, self.unison_done = "round", {}, false, false
  local order = by_speed(self, self:everyone())
  self.queue = {}
  for _, u in ipairs(order) do
    local sits_out = self.round == 1 and ((self.opening == "preemptive" and u.side == rules.FOES) or
      (self.opening == "ambush" and u.side == rules.ALLIES))
    if not sits_out then self.queue[#self.queue + 1] = u end
  end
end

--- Who acts next this round, then (after "next") the coming round's order.
function rules:timeline()
  local now, later = {}, {}
  for _, u in ipairs(self.queue) do
    if rules.alive(u) then now[#now + 1] = u end
  end
  for _, u in ipairs(by_speed(self, self:everyone())) do later[#later + 1] = u end
  return now, later
end

-- ---------------------------------------------------------------- flow

local function check_outcome(self)
  if #self:living(rules.FOES) == 0 then
    self.outcome = "victory"
  else
    local standing = false
    for _, u in ipairs(self.allies) do
      if u.hero and rules.alive(u) then standing = true end
    end
    if not standing then self.outcome = "defeat" end
  end
  return self.outcome
end

local function end_round(self, events)
  for _, u in ipairs(self:everyone()) do
    for id, turns in pairs(u.status) do
      if turns <= 1 then
        u.status[id] = nil
        events[#events + 1] = { type = "status_end", unit = u, id = id }
      else
        u.status[id] = turns - 1
      end
    end
    if u.broken and u.lost_turn then
      u.broken, u.lost_turn, u.break_left = false, false, u.break_max
      events[#events + 1] = { type = "recover", unit = u }
    end
  end
end

--- What happens next. Returns one of
---   {type = "act", unit, events, bonus}  the unit chooses an action (perform)
---   {type = "skip", unit, reason, events}  it loses this turn (broken, asleep)
---   {type = "unison", events}             the party picks an echo (unison)
---   {type = "round", events}              a new round began
---   {type = "over", outcome}
--- `events` are things that already happened on the way (poison, a summon leaving).
function rules:advance()
  if check_outcome(self) then return { type = "over", outcome = self.outcome } end
  if self.round == 0 then
    self:start_round()
    return { type = "round", events = { { type = "opening", opening = self.opening } } }
  end
  while #self.queue > 0 do
    local u = table.remove(self.queue, 1)
    if rules.alive(u) then
      local events = {}
      u.defending = false
      if self.phase == "bonus" then
        return { type = "act", unit = u, events = events, bonus = true }
      end
      if u.broken then
        u.lost_turn = true
        return { type = "skip", unit = u, reason = "broken", events = events }
      end
      if u.status.sleep then
        return { type = "skip", unit = u, reason = "sleep", events = events }
      end
      if u.status.poison then
        local loss = math.min(u.hp - 1, math.max(1, math.floor(u.max_hp * data.get("statuses", "poison").tick)))
        if loss > 0 then
          u.hp = u.hp - loss
          events[#events + 1] = { type = "damage", unit = u, amount = loss, poison = true }
        end
      end
      if u.summoned then
        u.turns_left = u.turns_left - 1
        if u.turns_left < 0 then
          -- Its time is up: a parting move, then it is gone.
          local finale = u.record.finale
          if finale then
            for _, event in ipairs(self:perform(u, { skill = finale })) do events[#events + 1] = event end
          end
          u.gone = true
          events[#events + 1] = { type = "leave", unit = u }
          if check_outcome(self) then return { type = "over", outcome = self.outcome, events = events } end
          return { type = "skip", unit = u, reason = "leave", events = events }
        end
      end
      return { type = "act", unit = u, events = events }
    end
  end
  if self.phase == "round" then
    -- The bonus phase: everyone after a sweep, else whoever broke a foe.
    self.phase = "bonus"
    local bonus = {}
    if self.sweep then
      for _, u in ipairs(self.allies) do
        if u.hero and rules.alive(u) and not u.status.sleep then bonus[#bonus + 1] = u end
      end
    else
      for _, u in ipairs(self.breakers) do
        if rules.alive(u) and not u.status.sleep then bonus[#bonus + 1] = u end
      end
    end
    self.queue = bonus
    if #bonus > 0 then return self:advance() end
  end
  if self.sweep and not self.unison_done then
    self.unison_done = true
    local echoes = self:unisons()
    if #echoes > 0 then return { type = "unison", echoes = echoes, events = {} } end
  end
  local events = {}
  end_round(self, events)
  self:start_round()
  return { type = "round", events = events }
end

--- The echoes the party can call a Unison from: those its living heroes wear.
function rules:unisons()
  local out, seen = {}, {}
  for _, u in ipairs(self.allies) do
    if u.hero and rules.alive(u) and u.echo and not seen[u.echo] then
      seen[u.echo] = true
      out[#out + 1] = u.echo
    end
  end
  return out
end

-- ---------------------------------------------------------------- actions

function rules:affinity(target, element)
  return element and target.affinity[element] or "normal"
end

--- What the party knows of a foe's affinity to an element: the affinity, or
--- nil while it has not been seen.
function rules:known_affinity(target, element)
  local seen = self.known[target.id]
  return seen and seen[element] or nil
end

local function reveal(self, target, element, events)
  if target.side ~= rules.FOES or not element then return end
  self.known[target.id] = self.known[target.id] or {}
  if self.known[target.id][element] == nil then
    local affinity = self:affinity(target, element)
    self.known[target.id][element] = affinity
    if affinity ~= "normal" then
      events[#events + 1] = { type = "reveal", unit = target, element = element, affinity = affinity }
    end
  end
end

local function add_surge(u, amount)
  if u.hero then u.surge = math.min(100, u.surge + amount) end
end

local function knock_out(target, events)
  target.hp = 0
  target.status = {}
  target.broken = false
  events[#events + 1] = { type = "ko", unit = target }
end

local function try_break(self, user, target, skill, affinity, events)
  if target.side ~= rules.FOES or target.break_max <= 0 or target.broken or not rules.alive(target) then return end
  local chip = skill["break"] or 1
  if affinity == "weak" then chip = chip * 2 elseif affinity ~= "normal" then chip = 0 end
  if chip <= 0 then return end
  target.break_left = math.max(0, target.break_left - chip)
  if target.break_left > 0 then return end
  target.broken, target.lost_turn = true, false
  events[#events + 1] = { type = "break", unit = target, by = user }
  if user.hero then
    local listed = false
    for _, u in ipairs(self.breakers) do listed = listed or u == user end
    if not listed then self.breakers[#self.breakers + 1] = user end
  end
  local all = true
  for _, f in ipairs(self:living(rules.FOES)) do all = all and f.broken end
  if all and not self.sweep then
    self.sweep = true
    events[#events + 1] = { type = "sweep" }
  end
end

--- An ally who takes a single blow aimed at `target`, or nil.
local function coverer(self, target)
  if not target.hero or target.hp >= target.max_hp * 0.5 then return nil end
  for _, u in ipairs(self.allies) do
    if u ~= target and rules.alive(u) and u.passive == "cover" and not u.status.sleep then return u end
  end
end

local function strike(self, user, target, skill, single, events)
  local physical = skill.kind == "physical"
  local element = skill.element or (physical and "physical" or nil)
  if physical and user.status.blind and self.rng:chance(data.get("statuses", "blind").miss) then
    events[#events + 1] = { type = "miss", unit = target }
    return
  end
  local covered
  if single and physical and target.side == rules.ALLIES then
    local guard = coverer(self, target)
    if guard then covered, target = target, guard end
  end
  local affinity = self:affinity(target, element)
  reveal(self, target, element, events)
  local attack = self:stat(user, physical and "atk" or "mag")
  local defence = self:stat(target, physical and "def" or "res")
  local amount = math.max(1, (attack * 2 - defence) * (skill.power or 1))
  amount = amount * self.rng:range(0.92, 1.08)
  local crit = physical and self.rng:chance(rules.settings.crit)
  if crit then amount = amount * rules.settings.crit_scale end
  if affinity == "weak" then amount = amount * rules.settings.weak end
  if affinity == "resist" then amount = amount * rules.settings.resist end
  if target.broken then amount = amount * rules.settings.broken end
  if target.defending then amount = amount * rules.settings.defend end
  if covered then amount = amount * rules.settings.cover end
  amount = math.max(1, math.floor(amount + 0.5))
  if affinity == "null" then
    events[#events + 1] = { type = "damage", unit = target, amount = 0, affinity = affinity, element = element }
    return
  end
  if affinity == "absorb" then
    local healed = math.min(target.max_hp - target.hp, amount)
    target.hp = target.hp + healed
    events[#events + 1] = { type = "heal", unit = target, amount = healed, affinity = affinity, element = element }
    return
  end
  local was_broken = target.broken
  target.hp = math.max(0, target.hp - amount)
  events[#events + 1] = { type = "damage", unit = target, amount = amount, affinity = affinity, element = element,
    crit = crit or nil, covered = covered }
  if target.side == rules.ALLIES then
    self.damage_taken = self.damage_taken + amount
    add_surge(target, amount / target.max_hp * rules.settings.surge_hit)
  end
  if physical and target.status.sleep and data.get("statuses", "sleep").wakes then
    target.status.sleep = nil
    events[#events + 1] = { type = "status_end", unit = target, id = "sleep" }
  end
  if target.hp <= 0 then
    knock_out(target, events)
    if target.side == rules.FOES then self.last_hit_broken = was_broken end
  else
    try_break(self, user, target, skill, affinity, events)
  end
end

local function apply_status(self, target, spec, events)
  if not rules.alive(target) then return end
  local applied = self.rng:chance(spec.chance or 1)
  if applied then target.status[spec.id] = spec.turns or 3 end
  events[#events + 1] = { type = "status", unit = target, id = spec.id, applied = applied }
end

local function restore(target, amount, events)
  local healed = math.min(target.max_hp - target.hp, math.floor(amount + 0.5))
  target.hp = target.hp + healed
  events[#events + 1] = { type = "heal", unit = target, amount = healed }
end

--- The units an action reaches. `picked` is the player's (or the AI's) choice
--- for single-target skills.
function rules:targets(user, target_kind, picked, include_fallen)
  if target_kind == "self" then return { user } end
  local side = (target_kind == "foe" or target_kind == "foes") and other(user.side) or user.side
  if target_kind == "foe" or target_kind == "ally" then
    if picked and (rules.alive(picked) or include_fallen) and not picked.gone then return { picked } end
    local living = self:living(side)
    return { living[1] }
  end
  local out = {}
  for _, u in ipairs(self:side(side)) do
    if not u.gone and (rules.alive(u) or include_fallen) then out[#out + 1] = u end
  end
  return out
end

--- Whether the unit can use a skill now, and if not, why.
function rules:usable(u, skill_id)
  local skill = data.get("skills", skill_id)
  if (skill.cost or 0) > u.mp then return false, "Not enough MP" end
  if u.status.silence and skill.kind ~= "physical" then return false, "Silenced" end
  if skill.kind == "summon" then
    for _, ally in ipairs(self.allies) do
      if ally.summoned and not ally.gone then return false, "A warden already fights" end
    end
  end
  return true
end

local function use_skill(self, user, skill, picked, events)
  local single = skill.target == "foe" or skill.target == "ally"
  local revive = skill.revive
  local targets = self:targets(user, skill.target, picked, revive ~= nil)
  events[#events + 1] = { type = "use", unit = user, skill = skill, targets = targets }
  if skill.kind == "physical" or skill.kind == "magic" then
    for _, target in ipairs(targets) do
      if rules.alive(target) then
        strike(self, user, target, skill, single, events)
        if skill.status and rules.alive(target) then apply_status(self, target, skill.status, events) end
      end
    end
  elseif skill.kind == "heal" then
    for _, target in ipairs(targets) do
      if not rules.alive(target) and revive then
        target.hp = math.max(1, math.floor(target.max_hp * revive))
        events[#events + 1] = { type = "revive", unit = target, amount = target.hp }
      elseif rules.alive(target) then
        restore(target, self:stat(user, "mag") * 2 * (skill.power or 1), events)
      end
      if skill.cure and rules.alive(target) then
        for id in pairs(target.status) do
          if data.get("statuses", id).kind ~= "buff" then target.status[id] = nil end
        end
        events[#events + 1] = { type = "cure", unit = target }
      end
      if skill.status then apply_status(self, target, skill.status, events) end
    end
  elseif skill.kind == "cure" then
    for _, target in ipairs(targets) do
      for id in pairs(target.status) do
        if data.get("statuses", id).kind ~= "buff" then target.status[id] = nil end
      end
      events[#events + 1] = { type = "cure", unit = target }
    end
  elseif skill.kind == "buff" or skill.kind == "debuff" then
    for _, target in ipairs(targets) do apply_status(self, target, skill.status, events) end
  elseif skill.kind == "summon" then
    local record = data.get("enemies", skill.summon)
    local stats = {}
    for name, value in pairs(record.stats) do stats[name] = value end
    local ally = unit(user.side, record.id, record, stats)
    ally.summoned, ally.turns_left, ally.ai = true, record.turns or 3, record.skills or {}
    self:side(user.side)[#self:side(user.side) + 1] = ally
    events[#events + 1] = { type = "summon", unit = ally, by = user }
  end
end

local function use_item(self, user, item, picked, events)
  local effect = item.effect
  local target = picked or user
  events[#events + 1] = { type = "use", unit = user, item = item, targets = { target } }
  if effect.kind == "heal" and rules.alive(target) then
    restore(target, effect.amount, events)
  elseif effect.kind == "mp" and rules.alive(target) then
    local gained = math.min(target.max_mp - target.mp, effect.amount)
    target.mp = target.mp + gained
    events[#events + 1] = { type = "mp", unit = target, amount = gained }
  elseif effect.kind == "cure" and rules.alive(target) then
    for id in pairs(target.status) do
      if data.get("statuses", id).kind ~= "buff" then target.status[id] = nil end
    end
    events[#events + 1] = { type = "cure", unit = target }
  elseif effect.kind == "revive" and not rules.alive(target) then
    target.hp = math.max(1, math.floor(target.max_hp * effect.fraction))
    events[#events + 1] = { type = "revive", unit = target, amount = target.hp }
  else
    events[#events + 1] = { type = "nothing", unit = target }
  end
end

--- Carry out an action and return what happened, in order. Actions:
---   {skill = id, target = unit}   a skill (the hero's attack is one)
---   {surge = true, target = unit} the hero's surge skill (empties the gauge)
---   {item = id, target = unit}    an item from the bag (options.consume)
---   {defend = true}               halve damage until this unit's next turn
---   {flee = true}                 try to run
function rules:perform(user, action)
  local events = {}
  if action.defend then
    user.defending = true
    add_surge(user, rules.settings.surge_defend)
    events[#events + 1] = { type = "defend", unit = user }
  elseif action.flee then
    self.flee_tries = self.flee_tries + 1
    local escaped = not self.boss and self.rng:chance(math.min(1, rules.settings.flee + 0.15 * (self.flee_tries - 1)))
    if escaped then self.outcome = "fled" end
    events[#events + 1] = { type = "flee", unit = user, success = escaped, boss = self.boss }
  elseif action.item then
    local item = data.get("items", action.item)
    if self.consume(item.id) then
      use_item(self, user, item, action.target, events)
    else
      events[#events + 1] = { type = "nothing", unit = user }
    end
  else
    local skill = data.get("skills", action.surge and user.surge_skill or action.skill)
    if action.surge then
      user.surge = 0
    else
      user.mp = user.mp - (skill.cost or 0)
      add_surge(user, rules.settings.surge_act)
    end
    use_skill(self, user, skill, action.target, events)
  end
  check_outcome(self)
  return events
end

--- The Unison after a sweep: the echo's finishing move, played by the party.
function rules:unison(echo_id)
  local echo = data.get("echoes", echo_id)
  local lead
  for _, u in ipairs(self.allies) do
    if u.hero and rules.alive(u) and (u.echo == echo_id or not lead) then lead = u end
  end
  local events = { { type = "unison", echo = echo, unit = lead } }
  use_skill(self, lead, data.get("skills", echo.unison), nil, events)
  check_outcome(self)
  return events
end

--- What a unit not controlled by the player does: a weighted pick among the
--- skills it can use now, aimed at a random standing opponent (or the most
--- wounded ally for support).
function rules:ai(u)
  local options, total = {}, 0
  for _, entry in ipairs(u.ai or {}) do
    if self:usable(u, entry.id) then
      options[#options + 1] = entry
      total = total + (entry.weight or 1)
    end
  end
  if #options == 0 then return { defend = true } end
  local roll, chosen = self.rng:range(0, total), options[#options]
  for _, entry in ipairs(options) do
    roll = roll - (entry.weight or 1)
    if roll < 0 then
      chosen = entry
      break
    end
  end
  local skill = data.get("skills", chosen.id)
  local target
  if skill.target == "foe" then
    target = self.rng:pick(self:living(other(u.side)))
  elseif skill.target == "ally" then
    for _, ally in ipairs(self:living(u.side)) do
      if not target or ally.hp / ally.max_hp < target.hp / target.max_hp then target = ally end
    end
  end
  return { skill = chosen.id, target = target }
end

--- What a won battle pays: exp and mastery per hero, gold, and the bonuses.
function rules:rewards()
  local exp, gold, mastery = 0, 0, 0
  for _, f in ipairs(self.foes) do
    exp, gold, mastery = exp + (f.record.exp or 0), gold + (f.record.gold or 0), mastery + (f.record.mastery or 0)
  end
  local out = { exp = exp, gold = gold, mastery = mastery, bonuses = {} }
  if self.damage_taken == 0 then
    out.exp = math.floor(out.exp * 1.25)
    out.bonuses[#out.bonuses + 1] = "Untouched: more EXP"
  end
  if self.last_hit_broken then
    out.mastery = math.floor(out.mastery * 1.5)
    out.bonuses[#out.bonuses + 1] = "Break finish: more mastery"
  end
  return out
end

return rules
