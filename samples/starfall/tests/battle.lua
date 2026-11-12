-- The battle rules, without a scene: every mechanic is a function of the
-- data tables and a seeded random stream.
local data = require("game.data")
local rules = require("game.battle.rules")
local state = require("game.state")

local function party(ids)
  state.new_game()
  local units = {}
  for _, id in ipairs(ids or state.data.party) do
    local saved = state.data.heroes[id]
    units[#units + 1] = rules.hero(data.get("heroes", id), state.stats(id), saved, state.echo_rank(saved.echo))
  end
  return units
end

local function battle(troop, options)
  options = options or {}
  local b = rules.new { party = options.party or party(options.ids), troop = troop, rng = fe.random.new(options.seed or 7),
    opening = options.opening or "normal", known = options.known, consume = options.consume }
  local first = b:advance()
  assert(first.type == "round")
  return b
end

local function hero(b, id)
  for _, u in ipairs(b.allies) do
    if u.id == id then return u end
  end
end

local function find(events, kind, predicate)
  for _, event in ipairs(events) do
    if event.type == kind and (not predicate or predicate(event)) then return event end
  end
end

--- Play turns with `choose(unit, turn)` for heroes and the AI for foes until
--- `stop(turn)` is true or the battle ends; returns every event in order.
local function play(b, choose, stop, limit)
  local all = {}
  for _ = 1, limit or 200 do
    local turn = b:advance()
    for _, event in ipairs(turn.events or {}) do all[#all + 1] = event end
    if turn.type == "over" or (stop and stop(turn)) then return all, turn end
    if turn.type == "act" then
      local action = turn.unit.hero and choose(turn.unit, turn) or b:ai(turn.unit)
      for _, event in ipairs(b:perform(turn.unit, action)) do all[#all + 1] = event end
    elseif turn.type == "unison" then
      for _, event in ipairs(b:unison(turn.echoes[1])) do all[#all + 1] = event end
    end
  end
  return all
end

return {
  ["the data tables reference each other consistently"] = function(t)
    data.validate()
    t.ok(#data.list("skills") > 30)
  end,

  ["units act once a round in order of speed"] = function(t)
    local b = battle("slimes")
    local now = b:timeline()
    local names = {}
    for i, u in ipairs(now) do names[i] = u.id end
    t.eq(names, { "bram", "sel", "aster", "mira", "slime", "slime" })
    t.eq(b.round, 1)
  end,

  ["a preemptive opening costs the foes their first round, an ambush the party's"] = function(t)
    local b = battle("slimes", { opening = "preemptive" })
    for _, u in ipairs((b:timeline())) do t.eq(u.side, rules.ALLIES) end
    local ambushed = battle("slimes", { opening = "ambush" })
    for _, u in ipairs((ambushed:timeline())) do t.eq(u.side, rules.FOES) end
    -- A boss is never caught off guard, whatever the roll.
    for seed = 1, 40 do
      t.eq(rules.new { party = party(), troop = "guardian", rng = fe.random.new(seed) }.opening, "normal")
    end
    local seen = {}
    for seed = 1, 300 do
      seen[rules.new { party = party(), troop = "slimes", rng = fe.random.new(seed) }.opening] = true
    end
    t.ok(seen.normal and seen.preemptive and seen.ambush, "all three openings occur")
  end,

  ["a weakness deals more damage, is revealed, and chips the break gauge twice"] = function(t)
    local b = battle("slimes")
    local slime = b.foes[1]
    t.eq(b:known_affinity(slime, "fire"), nil)
    local aster = hero(b, "aster")
    local plain = b:perform(aster, { skill = "slash", target = slime })
    local hit = find(plain, "damage")
    t.eq(hit.affinity, "normal")
    t.eq(slime.break_left, 2)
    b.foes[2].hp, b.foes[2].max_hp = 500, 500 -- it must survive to show its gauge
    local burning = b:perform(aster, { skill = "ember_edge", target = b.foes[2] })
    local burn = find(burning, "damage")
    t.eq(burn.affinity, "weak")
    t.ok(burn.amount > hit.amount * 1.5)
    t.ok(find(burning, "reveal", function(e) return e.element == "fire" and e.affinity == "weak" end))
    t.eq(b:known_affinity(b.foes[2], "fire"), "weak")
    t.eq(b.foes[2].break_left, 1) -- 3 - 1 x 2
  end,

  ["resist halves, null stops and absorb heals"] = function(t)
    local b = battle("embers") -- ember, wisp, ember
    local ember, wisp = b.foes[1], b.foes[2]
    local sel, mira = hero(b, "sel"), hero(b, "mira")
    mira.echo = "emberheart"
    ember.hp = 10
    local absorbed = b:perform(mira, { skill = "flare", target = ember })
    t.ok(find(absorbed, "heal", function(e) return e.unit == ember and e.affinity == "absorb" end))
    t.ok(ember.hp > 10)
    t.eq(ember.break_left, ember.break_max, "absorbed hits do not chip the gauge")
    local nulled = b:perform(sel, { skill = "gloom" })
    t.eq(find(nulled, "damage", function(e) return e.unit == wisp end).amount, 0)
    local resisted = b:perform(hero(b, "aster"), { skill = "slash", target = wisp })
    t.eq(find(resisted, "damage").affinity, "resist")
  end,

  ["breaking a foe costs it a turn and earns the breaker a bonus action"] = function(t)
    local b = battle("golem") -- golem (break 6, weak to bolt), wisp
    local golem = b.foes[1]
    local bram = hero(b, "bram")
    -- It resists plain blows, which leave its gauge alone; its weakness chips it double.
    local events = b:perform(bram, { skill = "piercing_shot", target = golem })
    t.eq(golem.break_left, 6)
    events = b:perform(bram, { skill = "volt_arrow", target = golem }) -- 2 x 2 (weak)
    t.eq(golem.break_left, 2)
    t.ok(not golem.broken)
    events = b:perform(bram, { skill = "volt_arrow", target = golem })
    t.ok(find(events, "break", function(e) return e.unit == golem and e.by == bram end))
    t.ok(golem.broken)
    t.ok(not b.sweep, "the wisp still stands unbroken")
    -- The round: heroes defend; the golem's turn is skipped; then Bram's bonus action.
    local skipped, bonus
    play(b, function(u, turn)
      if turn.bonus then bonus = u end
      return { defend = true }
    end, function(turn)
      if turn.type == "skip" and turn.unit == golem then skipped = turn.reason end
      return turn.type == "round"
    end)
    t.eq(skipped, "broken")
    t.eq(bonus, bram)
    t.ok(not golem.broken, "it recovers at the end of the round it lost")
    t.eq(golem.break_left, golem.break_max)
  end,

  ["breaking every foe is a sweep: the party acts again, then a unison"] = function(t)
    local b = battle("slimes")
    local aster, bram = hero(b, "aster"), hero(b, "bram")
    b:perform(aster, { skill = "guard_break", target = b.foes[1] }) -- break 5 against 3
    t.ok(b.foes[1].broken and not b.sweep)
    local events = b:perform(bram, { skill = "piercing_shot", target = b.foes[2] })
    t.ok(find(events, "sweep"))
    local bonus, unison = {}, nil
    play(b, function(u, turn)
      if turn.bonus then bonus[#bonus + 1] = u.id end
      return { defend = true }
    end, function(turn)
      if turn.type == "unison" then unison = turn.echoes end
      return turn.type == "unison"
    end)
    table.sort(bonus)
    t.eq(bonus, { "aster", "bram", "mira", "sel" })
    t.eq(unison, { "vanguard", "dawnlight", "stormcall", "frostveil" })
    local finale = b:unison("stormcall")
    t.eq(finale[1].type, "unison")
    t.ok(find(finale, "damage", function(e) return e.unit.side == rules.FOES end))
  end,

  ["a broken foe takes half again as much"] = function(t)
    local function damage(broken)
      local b = battle("golem", { seed = 3 })
      b.foes[1].broken = broken
      return find(b:perform(hero(b, "aster"), { skill = "slash", target = b.foes[1] }), "damage").amount
    end
    t.near(damage(true) / damage(false), 1.5, 0.08)
  end,

  ["a vanguard covers a wounded ally from a single blow"] = function(t)
    local b = battle("pack")
    local aster, mira = hero(b, "aster"), hero(b, "mira")
    t.eq(aster.passive, "cover")
    mira.hp = math.floor(mira.max_hp * 0.3)
    local before = mira.hp
    local events = b:perform(b.foes[1], { skill = "bite", target = mira })
    local hit = find(events, "damage")
    t.eq(hit.unit, aster)
    t.eq(hit.covered, mira)
    t.eq(mira.hp, before)
    -- Not when the ally is healthy.
    mira.hp = mira.max_hp
    t.eq(find(b:perform(b.foes[1], { skill = "bite", target = mira }), "damage").unit, mira)
  end,

  ["poison bites when the unit acts, sleep costs turns until struck, silence seals magic"] = function(t)
    local b = battle("slimes")
    local aster, sel = hero(b, "aster"), hero(b, "sel")
    aster.status.poison = 3
    sel.status.sleep = 2
    local poisoned, slept
    play(b, function() return { defend = true } end, function(turn)
      if turn.unit == aster and find(turn.events or {}, "damage", function(e) return e.poison end) then
        poisoned = true
      end
      if turn.type == "skip" and turn.unit == sel then slept = turn.reason end
      return turn.type == "round"
    end)
    t.ok(poisoned)
    t.eq(slept, "sleep")
    t.ok(aster.hp < aster.max_hp)
    local woken = b:perform(b.foes[1], { skill = "tackle", target = sel })
    t.ok(find(woken, "status_end", function(e) return e.id == "sleep" end))
    sel.status.silence = 2
    t.eq(select(2, b:usable(sel, "rime")), "Silenced")
    t.ok(b:usable(sel, "rod"))
  end,

  ["damage fills the surge gauge and a surge empties it"] = function(t)
    local b = battle("pack")
    local aster = hero(b, "aster")
    t.eq(aster.surge, 0)
    b:perform(b.foes[1], { skill = "bite", target = aster })
    t.ok(aster.surge > 0)
    aster.surge = 100
    local events = b:perform(aster, { surge = true })
    t.eq(find(events, "use").skill.id, "starbreaker")
    t.eq(aster.surge, 0)
  end,

  ["a warden is summoned for three turns and leaves with a parting move"] = function(t)
    local b = battle("guardian")
    local mira = hero(b, "mira")
    local events = b:perform(mira, { skill = "call_warden" })
    local warden = find(events, "summon").unit
    t.eq(warden.side, rules.ALLIES)
    t.eq(select(2, b:usable(mira, "call_warden")), "A warden already fights")
    local acted, left = 0, false
    play(b, function() return { defend = true } end, function(turn)
      if turn.unit == warden and turn.type == "act" then acted = acted + 1 end
      if turn.unit == warden and turn.reason == "leave" then
        left = true
        t.ok(find(turn.events, "use", function(e) return e.skill.id == "warden_finale" end))
      end
      return left
    end)
    t.eq(acted, 3)
    t.ok(warden.gone)
    t.ok(b:usable(mira, "call_warden") or mira.mp < 12)
  end,

  ["items come out of the bag and fleeing ends the battle, but never a boss fight"] = function(t)
    local bag = { potion = 1 }
    local b = battle("slimes", { consume = function(id)
      if (bag[id] or 0) <= 0 then return false end
      bag[id] = bag[id] - 1
      return true
    end })
    local mira = hero(b, "mira")
    mira.hp = 10
    t.ok(find(b:perform(mira, { item = "potion", target = mira }), "heal"))
    t.eq(mira.hp, 70)
    t.ok(find(b:perform(mira, { item = "potion", target = mira }), "nothing"), "the bag is empty")
    local fled
    for _ = 1, 10 do
      fled = find(b:perform(mira, { flee = true }), "flee").success
      if fled then break end
    end
    t.ok(fled)
    t.eq(b.outcome, "fled")
    local boss = battle("guardian")
    t.ok(not find(boss:perform(hero(boss, "mira"), { flee = true }), "flee").success)
    t.eq(boss.outcome, nil)
  end,

  ["a whole battle against the guardian can be won, and pays out"] = function(t)
    local b = battle("guardian", { seed = 11 })
    local _, last = play(b, function(u)
      if u.surge >= 100 then return { surge = true, target = b.foes[1] } end
      if u.id == "mira" then
        for _, ally in ipairs(b.allies) do
          if ally.hero and rules.alive(ally) and ally.hp < ally.max_hp * 0.5 and u.mp >= 8 then return { skill = "grace" } end
        end
      end
      if u.id == "bram" and u.mp >= 5 then return { skill = "volt_arrow", target = b.foes[1] } end
      return { skill = u.attack, target = b.foes[1] }
    end, nil, 600)
    t.eq(last.outcome, "victory")
    local rewards = b:rewards()
    t.ok(rewards.exp >= 140 and rewards.gold == 220)
  end,

  ["the party falls when no hero stands"] = function(t)
    local b = battle("pack")
    for _, u in ipairs(b.allies) do u.hp = 1 end
    local _, last = play(b, function() return { defend = true } end, nil, 400)
    t.eq(last.outcome, "defeat")
  end,
}
