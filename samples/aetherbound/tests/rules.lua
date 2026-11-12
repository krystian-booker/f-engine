--- The ATB rules, with no world and nothing drawn.
local rules = require("game.battle.rules")


local function new(troop, heroes)
  return rules.new { heroes = heroes or { "cael", "rowan", "lyra", "darius" }, troop = troop or "gate", seed = 7,
    items = { tonic = 2, kindle = 1 } }
end

local function find(battle, id)
  for _, unit in ipairs(battle.units) do
    if unit.id == id then return unit end
  end
end

local function of_type(events, kind)
  local out = {}
  for _, event in ipairs(events) do
    if event.type == kind then out[#out + 1] = event end
  end
  return out
end

return {
  ["gauges fill by speed and the fastest is ready first"] = function(t)
    local battle = new()
    for _, unit in ipairs(battle.units) do unit.gauge = 0 end
    local order = {}
    for _ = 1, 60 * 12 do
      for _, unit in ipairs(battle:advance(1 / 60)) do order[#order + 1] = unit.id end
    end
    t.eq(order[1], "rowan")
    t.eq(order[2], "cael")
    t.eq(order[3], "lyra")
    t.eq(order[4], "darius")
    t.eq(find(battle, "rowan").gauge, 1, "a ready unit waits with a full gauge")
    -- Rowan (speed 60) fills in FILL_SECONDS.
    local again = new()
    local rowan = find(again, "rowan")
    rowan.gauge = 0
    local ticks = 0
    while rowan.gauge < 1 do
      again:advance(1 / 60)
      ticks = ticks + 1
    end
    t.near(ticks / 60, rules.FILL_SECONDS, 0.05)
  end,

  ["an attack empties the gauge, hurts the target and can fell it"] = function(t)
    local battle = new()
    local cael, guard = find(battle, "cael"), battle.enemies[1]
    t.eq(guard.hp, math.floor(420 * 0.6), "the troop is weakened")
    cael.gauge = 1
    local events = battle:act(cael, { skill = "attack", target = guard })
    t.eq(cael.gauge, 0)
    t.eq(events[1].type, "use")
    local hit = of_type(events, "damage")[1]
    t.ok(hit.amount > 100 and hit.amount < 320, "damage in range: " .. hit.amount)
    guard.hp = 1
    events = battle:act(cael, { skill = "attack", target = guard })
    t.ok(of_type(events, "damage")[1].ko)
    t.ok(guard.ko)
    t.eq(battle:outcome(), nil, "one guard still stands")
    battle.enemies[2].hp = 1
    battle:act(cael, { skill = "attack", target = battle.enemies[2] })
    t.eq(battle:outcome(), "victory")
  end,

  ["a blow aimed at a fallen target goes to the next one"] = function(t)
    local battle = new()
    local cael = find(battle, "cael")
    battle.enemies[1].ko = true
    local events = battle:act(cael, { skill = "attack", target = battle.enemies[1] })
    t.eq(of_type(events, "damage")[1].unit, battle.enemies[2])
  end,

  ["resonance is immense and raises Rowan's corruption"] = function(t)
    local battle = new("colossus")
    local rowan, boss = find(battle, "rowan"), battle.enemies[1]
    local plain = of_type(battle:act(rowan, { skill = "attack", target = boss }), "damage")[1].amount
    local events = battle:act(rowan, { skill = "resonance", target = boss })
    local big = of_type(events, "damage")[1].amount
    t.ok(big > plain * 3.5, plain .. " -> " .. big)
    t.eq(rowan.corruption, 25)
    t.eq(of_type(events, "corruption")[1].value, 25)
    for _ = 1, 5 do battle:act(rowan, { skill = "resonance", target = boss }) end
    t.eq(rowan.corruption, 100, "capped")
  end,

  ["magic costs MP, heals stop at the maximum, and a skill without MP is blocked"] = function(t)
    local battle = new()
    local lyra, cael = find(battle, "lyra"), find(battle, "cael")
    cael.hp = 500
    local events = battle:act(lyra, { skill = "mend", target = cael })
    t.eq(lyra.mp, 120 - 8)
    t.ok(of_type(events, "heal")[1].amount > 300)
    cael.hp = cael.max_hp - 10
    t.eq(of_type(battle:act(lyra, { skill = "mend", target = cael }), "heal")[1].amount, 10)
    lyra.mp = 3
    t.eq(battle:blocked(lyra, "frost"), "Not enough MP")
    t.eq(battle:blocked(lyra, "attack"), nil)
    local rowan = find(battle, "rowan")
    rowan.only, rowan.only_reason = "unbound", "Nothing else will reach it."
    t.eq(battle:blocked(rowan, "attack"), "Nothing else will reach it.")
    t.eq(battle:blocked(rowan, "unbound"), nil)
  end,

  ["guard halves damage until the next turn; ward and cover work"] = function(t)
    local battle = new("colossus")
    local boss = battle.enemies[1]
    local cael, darius, lyra = find(battle, "cael"), find(battle, "darius"), find(battle, "lyra")
    local base = of_type(battle:act(boss, { skill = "slam", target = cael }), "damage")[1].amount
    battle:act(cael, { skill = "guard" })
    t.ok(cael.status.guard)
    local guarded = of_type(battle:act(boss, { skill = "slam", target = cael }), "damage")[1].amount
    t.ok(guarded < base * 0.65 and guarded > base * 0.35, base .. " -> " .. guarded)
    battle:act(cael, { skill = "attack", target = boss })
    t.ok(not cael.status.guard, "acting drops the guard")
    battle:act(cael, { skill = "ward", target = lyra })
    t.eq(lyra.status.ward, 3)
    battle:act(boss, { skill = "slam", target = lyra })
    t.eq(lyra.status.ward, 2)
    battle:act(darius, { skill = "bulwark" })
    local events = battle:act(boss, { skill = "slam", target = lyra })
    t.eq(of_type(events, "cover")[1].unit, darius)
    t.eq(of_type(events, "damage")[1].unit, darius)
    battle:act(darius, { skill = "attack", target = boss })
    battle:act(darius, { skill = "attack", target = boss })
    t.ok(not darius.status.cover, "bulwark lasts two of his turns")
  end,

  ["frostbind and shield bash push the target's gauge back"] = function(t)
    local battle = new("colossus")
    local boss = battle.enemies[1]
    boss.gauge = 0.9
    local events = battle:act(find(battle, "lyra"), { skill = "frost", target = boss })
    t.near(boss.gauge, 0.65, 0.001)
    t.eq(#of_type(events, "delay"), 1)
  end,

  ["the Colossus follows its pattern and stops at its threshold"] = function(t)
    local battle = new("colossus")
    local boss = battle.enemies[1]
    local used = {}
    for _ = 1, 6 do used[#used + 1] = battle:enemy_action(boss).skill end
    t.eq(table.concat(used, " "), "slam sweep slam charge pulse slam")
    local sweep = battle:act(boss, { skill = "sweep" })
    t.eq(#of_type(sweep, "damage"), 4, "a sweep hits the whole party")
    boss.threshold = 4000
    boss.hp = 4300
    local rowan = find(battle, "rowan")
    local events = battle:act(rowan, { skill = "resonance", target = boss })
    t.eq(boss.hp, 4000)
    t.eq(of_type(events, "damage")[1].amount, 300, "only what was above the threshold")
    t.eq(battle:outcome(), "threshold")
  end,

  ["an invulnerable unit takes nothing, and no_ko keeps heroes at 1 HP"] = function(t)
    local battle = new("colossus")
    local boss, cael = battle.enemies[1], find(battle, "cael")
    boss.invulnerable = true
    local event = of_type(battle:act(cael, { skill = "attack", target = boss }), "damage")[1]
    t.eq(event.amount, 0)
    t.ok(event.ineffective)
    t.eq(boss.hp, 8000)
    battle.no_ko = true
    cael.hp = 5
    battle:act(boss, { skill = "slam", target = cael })
    t.eq(cael.hp, 1)
    t.ok(not cael.ko)
    boss.avoid = "rowan"
    for _ = 1, 20 do
      local action = battle:enemy_action(boss)
      t.ok(action.target == nil or action.target.id ~= "rowan")
    end
  end,

  ["items heal, revive and run out; saving writes the party back"] = function(t)
    local battle = new()
    local cael, lyra = find(battle, "cael"), find(battle, "lyra")
    cael.hp = 100
    local events = battle:act(lyra, { item = "tonic", target = cael })
    t.eq(cael.hp, 500)
    t.eq(of_type(events, "heal")[1].amount, 400)
    t.eq(battle.items.tonic, 1)
    cael.hp, cael.ko = 0, true
    t.eq(#battle:choices(lyra, "fallen"), 1)
    battle:act(lyra, { item = "kindle", target = cael })
    t.ok(not cael.ko)
    t.eq(cael.hp, math.floor(980 * 0.4))
    lyra.hp, lyra.ko = 0, true
    battle:save()
    t.eq(battle.state.lyra.hp, 1, "the fallen get up with 1 HP")
    t.eq(battle.state.cael.hp, cael.hp)
    local next_battle = rules.new { heroes = { "cael", "lyra" }, troop = "gate", state = battle.state, seed = 1 }
    t.eq(find(next_battle, "lyra").hp, 1)
  end,
}
