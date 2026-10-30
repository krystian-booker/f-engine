local Battle = require("fe.battle.core")
local battle_kit = require("fe.battle")
local data = require("fe.data")
local effects = require("fe.effects")
local inventory = require("fe.rpg.inventory")
local party = require("fe.rpg.party")
local round_order = require("fe.battle.schedulers.round_order")
local state = require("fe.state")
local stats = require("fe.rpg.stats")
local tablex = require("fe.tablex")
local task = require("fe.task")

local function setup()
  if not data.has_table("skills") then require("fe.rpg.database").load() end
  Battle.settings.inventory, Battle.settings.party = inventory, party
  state.reset()
  party.reset()
  inventory.reset()
  party.join("hero")
  party.join("sage")
end

local function battle_with(scheduler, troop, seed, rules)
  local b = Battle({ scheduler = scheduler, troop = troop, random = fe.random.new(seed or 3), rules = rules })
  b:populate(party.members(), troop)
  return b
end

local schedulers = {
  round_order = function() return require("fe.battle.schedulers.round_order").new() end,
  atb = function() return require("fe.battle.schedulers.atb").new({ mode = "active", speed = 8 }) end,
  initiative = function() return require("fe.battle.schedulers.initiative").new() end,
}

local function count(events, name, predicate)
  local n = 0
  for _, e in ipairs(events) do
    if e.name == name and (not predicate or predicate(e.payload)) then n = n + 1 end
  end
  return n
end

local function record(b)
  local events = {}
  b.events:connect(function(name, payload) events[#events + 1] = { name = name, payload = payload } end)
  return events
end

return {
  ["round order: everyone acts by speed, rewards are granted once"] = function(t)
    setup()
    local order_seen = {}
    local b = battle_with(require("fe.battle.schedulers.round_order").new(), "slimes")
    b.events:connect(function(name, payload)
      if name == "action" and b.round == 1 then order_seen[#order_seen + 1] = payload.user:name() end
    end)
    local outcome, rewards = b:run()
    t.eq(outcome, "victory")
    t.eq(order_seen[1], "Hero") -- fastest first
    t.eq(rewards.exp, 60)
    t.eq(inventory.money(), 24)
    t.eq(inventory.count("potion"), 2)
  end,

  ["initiative preview is deterministic and matches turns taken"] = function(t)
    setup()
    local scheduler = require("fe.battle.schedulers.initiative").new()
    local b = battle_with(scheduler, "golem")
    for _, battler in ipairs(b.battlers) do battler.data.ct = 0 end
    local preview = scheduler.preview(b, 4)
    t.eq(#preview, 4)
    t.eq(preview[1]:name(), "Hero")
    local first = scheduler.next(b)
    t.ok(first == preview[1])
  end,

  ["initiative previews the order after a CT cost"] = function(t)
    setup()
    local scheduler = require("fe.battle.schedulers.initiative").new()
    local b = battle_with(scheduler, "golem")
    for _, battler in ipairs(b.battlers) do battler.data.ct = 0 end
    local hero = scheduler.next(b)
    t.eq(hero:name(), "Hero")
    local cheap = scheduler.preview(b, 3, { battler = hero, cost = 10 })
    local full = scheduler.preview(b, 3, { battler = hero, cost = 100 })
    t.ok(cheap[1] == hero, "a cheap turn acts again first")
    t.ok(full[1] ~= hero, "a full-cost turn waits")
  end,



  ["every scheduler gives upkeep turns to battlers that cannot act"] = function(t)
    for _, name in ipairs({ "round_order", "atb", "initiative" }) do
      setup()
      local hero_turns, hero_acted = 0, false
      local rules = {
        -- Nobody hurts anybody: the battle ends once the stunned hero acts.
        before_action = function(_, action)
          if action.user:name() == "Hero" then hero_acted = true end
          action.cancelled = true
        end,
        on_turn_end = function(_, b)
          if b:name() == "Hero" then hero_turns = hero_turns + 1 end
        end,
        outcome = function() if hero_acted then return "victory" end end,
      }
      local b = battle_with(schedulers[name](), "slimes", 5, rules)
      local events = record(b)
      b.battlers[1]:add_state("stun")
      b:run()
      t.ok(hero_acted, name .. ": the hero acts again")
      t.ok(hero_turns >= 3, name .. ": two stunned turns, then an action (" .. hero_turns .. ")")
      t.eq(count(events, "states_expired", function(p) return p.battler:name() == "Hero" and p.states[1] == "stun" end), 1,
        name .. ": stun wore off")
    end
  end,

  ["states tick, prevent actions and wear off"] = function(t)
    setup()
    local b = battle_with(require("fe.battle.schedulers.round_order").new(), "slimes")
    local hero = b.battlers[1]
    hero:add_state("poison")
    local before = hero:resource("hp")
    b:turn_start(hero)
    t.eq(hero:resource("hp"), before - 12)
    hero:add_state("sleep")
    t.ok(not hero:can_act())
    effects.resolve({ user = b.battlers[3], targets = { hero }, source = data.get("skills", "attack"), battle = b,
      random = fe.random.new(1) })
    t.ok(not hero:has_state("sleep"), "damage wakes a sleeper")
    for _ = 1, 3 do b:turn_end(hero) end
    t.ok(not hero:has_state("poison"))
  end,

  ["guarding halves damage and custom actions are rules hooks"] = function(t)
    setup()
    local b = battle_with(require("fe.battle.schedulers.round_order").new(), "slimes")
    local slime, hero = b.battlers[3], b.battlers[1]
    local attack = data.get("skills", "attack")
    local plain = effects.resolve({ user = slime, targets = { hero }, source = attack, battle = b, preview = true })[1].amount
    hero.data.guarding = true
    local guarded = effects.resolve({ user = slime, targets = { hero }, source = attack, battle = b, preview = true })[1].amount
    t.eq(guarded, math.floor(plain * 0.5 + 0.5))
    b.rules.actions = { steal = function(battle, action)
      inventory.add("ether", 1)
      return {}
    end }
    b:perform({ kind = "steal", user = hero })
    t.eq(inventory.count("ether"), 1)
  end,

  ["fire against a weak enemy uses the affinity multiplier"] = function(t)
    setup()
    local b = battle_with(require("fe.battle.schedulers.round_order").new(), "slimes")
    local sage, slime = b.battlers[2], b.battlers[3]
    local results = effects.resolve({ user = sage, targets = { slime }, source = data.get("skills", "fire"), battle = b,
      preview = true })
    t.eq(results[1].affinity, 2)
  end,

  ["rule layers all run; outcome composes; single answers come from the most specific layer"] = function(t)
    setup()
    local calls = {}
    local game = {
      on_start = function() calls[#calls + 1] = "game" end,
      escape_chance = function() return 0 end,
      outcome = function(b) if b.data.game_over then return "defeat" end end,
    }
    local troop = {
      on_start = function() calls[#calls + 1] = "troop" end,
      escape_chance = function() return 1 end,
      outcome = function(b) if b.data.troop_won then return "victory" end end,
    }
    local b = Battle({ layers = { game, troop }, scheduler = { run = function() end }, random = fe.random.new(1) })
    b:populate(party.members(), "slimes")
    b:notify("on_start")
    t.eq(calls, { "game", "troop" })
    t.eq(b:rule("escape_chance")(b), 1)
    b.data.game_over = true
    t.eq(b:outcome(), "defeat")
    b.data.troop_won = true
    t.eq(b:outcome(), "victory", "the more specific layer answers first")
  end,

  ["round order hooks can reorder a round and the preview shows the next round"] = function(t)
    setup()
    local rules = {
      order = function(_, entries)
        -- Send the hero to the back.
        for i, e in ipairs(entries) do
          if e.battler:name() == "Hero" then table.insert(entries, table.remove(entries, i)) break end
        end
      end,
      outcome = function(b) if b.round > 1 then return "victory" end end,
      before_action = function(_, action) action.cancelled = true end,
    }
    local scheduler = require("fe.battle.schedulers.round_order").new()
    local b = battle_with(scheduler, "slimes", 2, rules)
    local current, next_round = scheduler.preview(b)
    t.eq(next_round[#next_round]:name(), "Hero")
    t.eq(#current, 4)
    local first_turn
    b.events:connect(function(name, payload)
      if name == "order" and not first_turn then first_turn = payload end
    end)
    b:run()
    t.eq(first_turn[#first_turn]:name(), "Hero")
  end,

  ["each knock-out is presented once, whatever caused it"] = function(t)
    setup()
    local b = battle_with(require("fe.battle.schedulers.round_order").new(), "slimes", 4)
    local events = record(b)
    local hero, slime_a, slime_b = b.battlers[1], b.battlers[3], b.battlers[4]
    slime_a:set_resource("hp", 1)
    slime_b:set_resource("hp", 1)
    b:perform({ kind = "skill", user = hero, skill = "flurry", hits = 6 })
    t.eq(count(events, "knocked_out"), 2, "multi-hit")
    hero:add_state("poison")
    hero:set_resource("hp", 1)
    b:turn_start(hero)
    t.ok(not hero:alive())
    t.eq(count(events, "knocked_out", function(p) return p == hero end), 1, "state damage")
    b:settle()
    t.eq(count(events, "knocked_out"), 3)
  end,

  ["random-target skills hit living foes, as many times as the action says"] = function(t)
    setup()
    local b = battle_with(require("fe.battle.schedulers.round_order").new(), "slimes", 6)
    local hero = b.battlers[1]
    local results = b:perform({ kind = "skill", user = hero, skill = "flurry", hits = 5 })
    t.eq(#results, 5)
    local slime = b.battlers[3]
    slime:set_resource("hp", 0)
    b:settle()
    results = b:perform({ kind = "skill", user = hero, skill = "flurry" })
    t.eq(#results, 3)
    for _, r in ipairs(results) do t.ok(r.target ~= slime, "fallen foes are not picked") end
  end,

  ["items with consume = false stay in the inventory"] = function(t)
    setup()
    local b = battle_with(require("fe.battle.schedulers.round_order").new(), "slimes")
    local hero = b.battlers[1]
    inventory.add("charm", 1)
    inventory.add("potion", 1)
    hero:set_resource("hp", 50)
    t.ok(b:perform({ kind = "item", user = hero, item = "charm", targets = { hero } }) ~= nil)
    t.eq(inventory.count("charm"), 1)
    t.eq(hero:resource("hp"), 60)
    b:perform({ kind = "item", user = hero, item = "potion", targets = { hero } })
    t.eq(inventory.count("potion"), 0)
    t.eq(b:perform({ kind = "item", user = hero, item = "potion", targets = { hero } }), nil, "none left")
  end,

  ["a reaction hook lets hit battlers counter once"] = function(t)
    setup()
    local rules = {
      reaction = function(_, b, action)
        if b.side == "enemy" then return { kind = "skill", skill = "attack", targets = { action.user } } end
      end,
    }
    local b = battle_with(require("fe.battle.schedulers.round_order").new(), "golem", 8, rules)
    local events = record(b)
    local hero = b.battlers[1]
    b:perform({ kind = "skill", user = hero, skill = "attack", targets = { b.battlers[3] } })
    t.eq(count(events, "reaction"), 1)
    t.eq(count(events, "action"), 2, "the counter does not trigger another counter")
  end,

  ["mutual knock-outs follow the setting"] = function(t)
    setup()
    local b = battle_with(require("fe.battle.schedulers.round_order").new(), "slimes")
    for _, battler in ipairs(b.battlers) do battler:set_resource("hp", 0) end
    t.eq(b:outcome(), "defeat")
    Battle.settings.mutual_ko = "draw"
    t.eq(b:outcome(), "draw")
    Battle.settings.mutual_ko = "defeat"
  end,

  ["cancelling in any phase leaves the target untouched"] = function(t)
    setup()
    local b = battle_with(require("fe.battle.schedulers.round_order").new(), "slimes")
    local hero, slime = b.battlers[1], b.battlers[3]
    for _, phase in ipairs({ "before", "hit", "calculate", "modify", "apply" }) do
      local remove = b:effect_hook(phase, function(ctx) ctx.cancelled = true end)
      local hp = slime:resource("hp")
      local results = effects.resolve({ user = hero, targets = { slime }, source = data.get("skills", "attack"), battle = b,
        random = b.random })
      t.ok(results[1].cancelled, phase)
      t.eq(slime:resource("hp"), hp, phase)
      remove()
    end
  end,

  ["per-battle effect hooks apply only to their battle and stop when it ends"] = function(t)
    setup()
    local rules = { effect_hooks = { { phase = "modify", fn = function(ctx) ctx.multiplier = ctx.multiplier * 3 end } },
      outcome = function(b) if b.round > 0 then return "victory" end end }
    local b = battle_with(require("fe.battle.schedulers.round_order").new(), "slimes", 1, rules)
    local other = battle_with(require("fe.battle.schedulers.round_order").new(), "slimes")
    local attack = data.get("skills", "attack")
    local function amount(battle)
      return effects.resolve({ user = battle.battlers[1], targets = { battle.battlers[3] }, source = attack, battle = battle,
        preview = true })[1].amount
    end
    t.ok(amount(b) > amount(other) * 2)
    b:run()
    t.eq(amount(b), amount(other))
    -- Global hooks registered again with the same id replace the old one.
    local before = #effects.hooks.modify
    effects.hook("modify", function() end, 1, "test.once")
    effects.hook("modify", function() end, 1, "test.once")
    t.eq(#effects.hooks.modify, before + 1)
    effects.hook("modify", function() end, 1, "test.once")()
    t.eq(#effects.hooks.modify, before)
  end,

  ["forecasts report hit chance without consuming randomness"] = function(t)
    setup()
    local b = battle_with(require("fe.battle.schedulers.round_order").new(), "slimes")
    local hero, slime = b.battlers[1], b.battlers[3]
    local swing = data.get("skills", "wild_swing")
    local random = fe.random.new(11)
    local preview = effects.resolve({ user = hero, targets = { slime }, source = swing, battle = b, preview = true,
      random = random })
    t.near(preview[1].hit_chance, 0.7)
    t.near(effects.resolve({ user = hero, targets = { slime }, source = swing, battle = b, preview = true })[1].hit_chance, 0.7)
    t.eq(random:float(), fe.random.new(11):float(), "the stream is untouched")
  end,

  ["ATB gauges hold still during actions and battlers added mid-battle act"] = function(t)
    setup()
    local scheduler = require("fe.battle.schedulers.atb").new({ mode = "active", speed = 4 })
    local added, added_acted = nil, false
    local turns = 0
    local rules = {
      -- Every action plays for half a second; gauges must not jump afterwards.
      actions = { wait = function(_, action)
        if action.user == added then added_acted = true end
        task.wait(0.5)
        return {}
      end },
      on_turn_end = function(battle)
        turns = turns + 1
        if turns == 2 then added = battle:add({ enemy = "slime", side = "enemy" }) end
      end,
      outcome = function() if added_acted then return "victory" end end,
    }
    local b = battle_with(scheduler, "slimes", 7, rules)
    for _, battler in ipairs(b.battlers) do battler.auto = true end
    local ai = require("fe.battle.ai")
    local default = ai.default
    ai.default = function(_, me) return { kind = "wait", user = me } end
    local biggest, running = 0, true
    local last = {}
    local watcher = task.spawn(function()
      while running do
        for _, battler in ipairs(b.battlers) do
          local g = battler.data.gauge
          if g and last[battler] and g > last[battler] then biggest = math.max(biggest, g - last[battler]) end
          last[battler] = g
        end
        task.frame()
      end
    end)
    local ok, err = pcall(b.run, b)
    running = false
    ai.default = default
    watcher:cancel()
    t.ok(ok, tostring(err))
    t.ok(added_acted, "the reinforcement got a gauge and acted")
    t.ok(biggest < 0.2, "largest gauge step per frame " .. biggest)
  end,

  ["buffs stack up to a cap, and a self-buff lasts its full turns"] = function(t)
    setup()
    local b = battle_with(require("fe.battle.schedulers.round_order").new(), "slimes")
    local hero = b.battlers[1]
    local base = hero:stat("attack")
    for _ = 1, 5 do hero:buff({ stat = "attack", add = 2, cap = 3 }, 2) end
    t.eq(hero:stat("attack"), base + 6)
    t.eq(#hero:buffs_of("buff"), 1)
    hero:buff({ stat = "defense", mul = 0.5 }, 1)
    t.eq(#hero:buffs_of("debuff"), 1)
    hero:clear_buffs()
    -- Might buffs the user during its own turn: that turn's end does not count.
    b:turn(hero, { kind = "skill", user = hero, skill = "might" })
    t.eq(hero:stat("attack"), base + 5, "still up after the turn it was cast")
    b:turn(hero, false)
    t.eq(hero:stat("attack"), base + 5, "one full turn")
    b:turn(hero, false)
    t.eq(hero:stat("attack"), base, "gone after two turns")
  end,

  ["battle-only resources reset at the start and end of a battle"] = function(t)
    setup()
    stats.resources({ hp = { max = "max_hp" }, mp = { max = "max_mp" }, bp = { max = 5, start = 1, battle_only = true } })
    local hero = party.actor("hero")
    hero:set_resource("bp", 4)
    local seen
    local b = battle_with(require("fe.battle.schedulers.round_order").new(), "slimes", 3,
      { setup = function(battle) seen = battle.battlers[1]:resource("bp") battle.battlers[1]:set_resource("bp", 5) end })
    b:run()
    t.eq(seen, 1)
    t.eq(hero:resource("bp"), 1)
    stats.resources({ hp = { max = "max_hp" }, mp = { max = "max_mp" } })
  end,

  ["turn-choice rounds: battlers choose on their turn and the preview is the next round"] = function(t)
    setup()
    local scheduler = round_order.new({ choose = "turn", jitter = 0.4 })
    local choices, predicted, actual = {}, nil, nil
    local rules = {
      player_controller = function(battle, b)
        choices[#choices + 1] = battle.current == b
        return { kind = "defend", user = b }
      end,
      on_turn_start = function(battle)
        if battle.round == 1 and not predicted then predicted = select(2, scheduler.preview(battle)) end
      end,
      before_action = function(_, action) if action.kind ~= "defend" then action.cancelled = true end end,
      outcome = function(b) if b.round > 2 then return "victory" end end,
    }
    local b = battle_with(scheduler, "slimes", 12, rules)
    b.events:connect(function(name, payload) if name == "order" and b.round == 2 then actual = payload end end)
    b:run()
    t.eq(#choices, 4, "both party members chose in both rounds")
    for _, on_turn in ipairs(choices) do t.ok(on_turn, "each choice happens on the chooser's own turn") end
    t.eq(#actual, 4)
    for i = 1, 4 do t.ok(predicted[i] == actual[i], "next-round preview slot " .. i) end
  end,

  ["order hooks reorder the rest of a round when a battler is disabled mid-round"] = function(t)
    setup()
    local stunned, turns = false, {}
    local rules = {
      order = function(_, entries)
        local front, back = {}, {}
        for _, e in ipairs(entries) do table.insert(e.battler:has_state("stun") and back or front, e) end
        for i, e in ipairs(front) do entries[i] = e end
        for i, e in ipairs(back) do entries[#front + i] = e end
      end,
      before_action = function(battle, action)
        if not stunned then
          stunned = true
          battle.battlers[2]:add_state("stun")
        end
        action.cancelled = true
      end,
      on_turn_start = function(battle, b) if battle.round == 1 then turns[#turns + 1] = b:name() end end,
      outcome = function(b) if b.round > 1 then return "victory" end end,
    }
    battle_with(round_order.new(), "slimes", 2, rules):run()
    t.eq(turns, { "Hero", "Slime", "Slime", "Sage" })
  end,

  ["back-row swaps hand over the turn and both battlers keep their state"] = function(t)
    setup()
    party.join("scout", { active = false })
    local swapped = false
    local acted_by = {}
    local rules = {
      player_controller = function(battle, b)
        if b:name() == "Hero" and not swapped then
          swapped = true
          local partner = battle:partner(b)
          battle:swap(b, partner)
          return { kind = "defend", user = partner }
        end
        return { kind = "defend", user = b }
      end,
      on_turn_end = function(battle, b) if battle.round == 1 then acted_by[#acted_by + 1] = b:name() end end,
      before_action = function(_, action) if action.kind ~= "defend" then action.cancelled = true end end,
      outcome = function(b) if b.round > 2 then return "victory" end end,
    }
    local b = Battle({ scheduler = round_order.new({ choose = "turn" }), troop = "slimes", random = fe.random.new(3), rules = rules })
    b:populate(party.members(), "slimes", party.reserve())
    local hero, scout = b.battlers[1], b.reserve[1]
    t.ok(b:partner(hero) == scout)
    scout:buff({ stat = "attack", add = 3 }, 9)
    hero.data.marker = "kept"
    local events = record(b)
    b:run()
    t.ok(b.battlers[1] == scout and b.reserve[1] == hero, "the scout took the hero's place")
    t.eq(acted_by[1], "Scout", "the scout took the hero's turn")
    t.ok(not tablex.contains(acted_by, "Hero"))
    t.eq(count(events, "battler_removed", function(p) return p == hero end), 1)
    t.eq(count(events, "battler_added", function(p) return p == scout end), 1)
    t.eq(#scout:buffs_of("buff"), 1, "the reserve's buff came along")
    t.eq(hero.data.marker, "kept")
    t.ok(b:partner(scout) == hero)
    b:swap(scout, hero)
    t.ok(b.battlers[1] == hero, "and they can swap back")
  end,

  ["summons leave after their turns, when knocked out or when their owner falls"] = function(t)
    setup()
    local b = battle_with(round_order.new(), "slimes")
    local events = record(b)
    local hero = b.battlers[1]
    local knight = b:summon({ enemy = "old_knight", side = "party" }, { turns = 2 })
    t.ok(knight.auto, "summons are computer-controlled")
    b:turn(knight, false)
    t.ok(knight.battle == b)
    b:turn(knight, false)
    t.eq(knight.battle, nil, "gone after two turns")
    local guard = b:summon({ enemy = "old_knight", side = "party" }, { owner = hero })
    local pet = b:summon({ enemy = "old_knight", side = "party" })
    pet:set_resource("hp", 0)
    b:settle()
    t.eq(pet.battle, nil, "knocked out")
    t.ok(guard.battle == b)
    hero:set_resource("hp", 0)
    b:settle()
    t.eq(guard.battle, nil, "its owner fell")
    t.eq(count(events, "dismissed"), 3)
    t.eq(#b.battlers, 4)
  end,

  ["companions act at the end of a round and leave again"] = function(t)
    setup()
    local assist = require("fe.battle.assist")
    assist.join("kay", { enemy = "old_knight", skill = "attack", chance = 1, name = "Sir Kay" })
    t.eq(state.snapshot().assists.kay.enemy, "old_knight", "companions are saved")
    local helped = 0
    local rules = assist.rules({
      before_action = function(_, action) if not action.assist then action.cancelled = true end end,
      after_action = function(_, action) if action.assist then helped = helped + 1 end end,
      outcome = function(b) if b.round > 3 then return "victory" end end,
    })
    local b = battle_with(round_order.new(), "golem", 4, rules)
    local events = record(b)
    local golem = b.battlers[3]
    b:run()
    t.eq(helped, 2, "after rounds 1 and 2")
    t.eq(count(events, "assist"), 2)
    t.ok(golem:resource("hp") < golem:max_resource("hp"))
    t.eq(#b.battlers, 3, "the companion is not left in the battle")
    assist.leave("kay")
    t.eq(#assist.list(), 0)
  end,

  ["the limit gauge fills from damage and falls, and pays for its skills"] = function(t)
    setup()
    stats.resources({ hp = { max = "max_hp" }, mp = { max = "max_mp" }, lp = { max = 2, start = 0, battle_only = true } })
    local limit = require("fe.battle.limit")
    local b = battle_with(round_order.new(), "slimes", 3, limit.rules())
    local events = record(b)
    local hero, sage, slime = b.battlers[1], b.battlers[2], b.battlers[3]
    hero:set_resource("lp", 0)
    b:perform({ kind = "skill", user = slime, skill = "attack", targets = { hero } })
    t.eq(hero:resource("lp"), 1)
    t.ok(not limit.ready(hero))
    t.eq(slime:resource("lp"), 0, "enemies have no gauge")
    b:perform({ kind = "skill", user = slime, skill = "attack", targets = { hero } })
    t.ok(limit.ready(hero))
    t.eq(count(events, "limit_ready"), 1)
    t.ok(b:perform({ kind = "skill", user = hero, skill = "starfall", targets = b:opponents(hero) }) ~= nil)
    t.eq(hero:resource("lp"), 0, "the skill spent the gauge")
    sage:set_resource("hp", 0)
    b:settle()
    t.eq(hero:resource("lp"), 1, "an ally falling fills it too")
    stats.resources({ hp = { max = "max_hp" }, mp = { max = "max_mp" } })
  end,

  ["counterattacks need the ability, a melee hit and luck"] = function(t)
    setup()
    local reactions = require("fe.battle.reactions")
    local rules = { reaction = reactions.counter({ requires = "counter" }) }
    local b = battle_with(round_order.new(), "brute", 8, rules)
    local events = record(b)
    local hero, sage, brute = b.battlers[1], b.battlers[2], b.battlers[3]
    b:perform({ kind = "skill", user = hero, skill = "attack", targets = { brute } })
    t.eq(count(events, "reaction"), 1, "the brute knows Counter")
    b:perform({ kind = "skill", user = sage, skill = "fire", targets = { brute } })
    t.eq(count(events, "reaction"), 1, "spells from afar are not countered")
    b:perform({ kind = "skill", user = brute, skill = "attack", targets = { hero } })
    t.eq(count(events, "reaction"), 1, "the hero cannot counter")
    b.rules.reaction = reactions.counter({ requires = "counter", chance = 0.5 })
    for _ = 1, 40 do
      hero:set_resource("hp", 999)
      brute:set_resource("hp", 999)
      b:perform({ kind = "skill", user = hero, skill = "attack", targets = { brute } })
    end
    local countered = count(events, "reaction") - 1
    t.ok(countered > 8 and countered < 32, "about half are countered (" .. countered .. ")")
  end,

  ["buff multipliers are capped and capped stacks are reported"] = function(t)
    setup()
    local b = battle_with(round_order.new(), "slimes")
    local hero = b.battlers[1]
    local base = hero:stat("attack")
    local first = hero:buff({ stat = "attack", mul = 1.5, cap = 2 }, 3)
    t.ok(not first.capped)
    hero:buff({ stat = "attack", mul = 1.5, cap = 2 }, 3)
    local third = hero:buff({ stat = "attack", mul = 1.5, cap = 2 }, 3)
    t.ok(third.capped, "a third application adds nothing")
    t.eq(hero:stat("attack"), math.floor(base * 2 + 0.5), "1.5 x 1.5 is held at the x2 limit")
    hero:buff({ stat = "attack", mul = 0.5, id = "weak", cap = 3 }, 3)
    hero:buff({ stat = "attack", mul = 0.5, id = "weak", cap = 3 }, 3)
    t.eq(hero:stat("attack"), math.floor(base * 2.25 * 0.25 + 0.5), "buffs and debuffs combine before the limit")
    hero:clear_buffs()
    local result = effects.resolve({ user = hero, targets = { hero }, source = data.get("skills", "might"), battle = b })[1]
    t.eq(result.capped, false)
  end,

  ["mock battles change nothing and never end the game"] = function(t)
    setup()
    local hero = party.actor("hero")
    hero:set_resource("hp", 30)
    inventory.add("potion", 2)
    local defeated = false
    local on_defeat = battle_kit.on_defeat
    battle_kit.on_defeat = function() defeated = true end
    local quiet = { new = function() return {} end }
    local outcome = battle_kit.mock(function()
      t.eq(party.actor("hero"):resource("hp"), party.actor("hero"):max_resource("hp"), "the roster starts recovered")
      t.ok(battle_kit.mocking)
      return battle_kit.start({ troop = "golem", presentation = quiet, scheduler = round_order.new(), rules = {
        on_start = function(b)
          b:perform({ kind = "item", user = b.battlers[1], item = "potion", targets = { b.battlers[1] } })
          for _, m in ipairs(b:side("party")) do m:set_resource("hp", 0) end
        end,
      } })
    end)
    battle_kit.on_defeat = on_defeat
    t.eq(outcome, "defeat")
    t.ok(not defeated, "a mock defeat is not a game over")
    t.ok(not battle_kit.mocking)
    t.eq(party.actor("hero"):resource("hp"), 30, "HP is back")
    t.eq(inventory.count("potion"), 2, "the potion is back")
    t.ok(not pcall(battle_kit.mock, function() party.actor("hero"):set_resource("hp", 1) error("stop") end))
    t.eq(party.actor("hero"):resource("hp"), 30, "restored after an error too")
  end,

  ["battle core items and rewards go through a replaceable inventory"] = function(t)
    setup()
    inventory.add("potion", 1)
    local rpg_inventory, rpg_party = Battle.settings.inventory, Battle.settings.party
    Battle.settings.inventory, Battle.settings.party = nil, nil
    local b = battle_with(round_order.new(), "slimes")
    local hero = b.battlers[1]
    hero:set_resource("hp", 50)
    t.eq(b:perform({ kind = "item", user = hero, item = "potion", targets = { hero } }), nil, "no inventory, no items")
    local store = { items = { potion = 1 }, money = 0 }
    function store.has(id) return (store.items[id] or 0) > 0 end
    function store.remove(id, n) store.items[id] = store.items[id] - n return true end
    function store.add(id, n) store.items[id] = (store.items[id] or 0) + n end
    function store.add_money(n) store.money = store.money + n end
    b.rules.inventory = function() return store end
    t.ok(b:perform({ kind = "item", user = hero, item = "potion", targets = { hero } }) ~= nil)
    t.eq(store.items.potion, 0)
    t.eq(inventory.count("potion"), 1, "the party inventory is untouched")
    local rewards = b:grant({ exp = 50, money = 7, jp = 0, items = { "ether" } })
    t.eq(rewards.levels, nil, "no party, no experience")
    t.eq(store.money, 7)
    t.eq(store.items.ether, 1)
    Battle.settings.inventory, Battle.settings.party = rpg_inventory, rpg_party
  end,

  ["revealed weaknesses are remembered per enemy type"] = function(t)
    setup()
    local shields = require("fe.battle.break")
    local breaker
    local b = battle_with(round_order.new(), "wisps", 1, shields.rules({ on_break = function(_, _, by) breaker = by end }))
    b:notify("setup")
    local sage, wisp = b.battlers[2], b.battlers[3]
    t.ok(not shields.revealed(wisp, "fire"))
    b:perform({ kind = "skill", user = sage, skill = "fire", targets = { wisp } })
    t.ok(shields.is_broken(wisp))
    t.ok(breaker == sage, "on_break names the breaker")
    t.eq(shields.known("wisp"), { "fire" })
    local again = battle_with(round_order.new(), "wisps", 1, shields.rules())
    again:notify("setup")
    t.ok(shields.revealed(again.battlers[3], "fire"), "known from the last battle")
    shields.settings.remember = false
    t.ok(not shields.revealed(again.battlers[3], "fire"))
    shields.settings.remember = true
  end,

  ["reloading battle modules mid-battle keeps the battle working"] = function(t)
    setup()
    local shields = require("fe.battle.break")
    local function ids(phase)
      return tablex.map(effects.hooks[phase], function(h) return tostring(h.id) .. "@" .. h.priority end)
    end
    local before = { modify = ids("modify"), hit = ids("hit") }
    local rules = shields.rules({
      player_controller = function(_, b) return { kind = "wait", user = b } end,
      actions = { wait = function() task.wait(0.05) return {} end },
      outcome = function(b) if b.round > 4 then return "victory" end end,
    })
    local b = battle_with(round_order.new({ choose = "turn" }), "wisps", 6, rules)
    for _, enemy in ipairs(b:side("enemy")) do enemy:set_resource("hp", 9999) end
    local outcome
    local job = task.spawn(function() outcome = b:run() end)
    t.wait_until(function() return b.round >= 2 end, 600)
    for _, name in ipairs({ "fe.effects", "fe.battle.core", "fe.battle.battler", "fe.battle.ai", "fe.battle",
      "fe.battle.schedulers.round_order", "fe.battle.schedulers.atb", "fe.battle.break", "fe.battle.reactions",
      "fe.battle.presenter_common", "fe.battle.sideview", "fe.battle.sideview_hud", "fe.battle.sideview_commands",
      "fe.rpg.party", "fe.rpg.actor", "fe.rpg.combatant", "fe.rpg.database" }) do
      t.ok(fe.harness.reload(name), "reloaded " .. name)
    end
    t.eq(fe.harness.errors(), 0)
    t.wait_until(function() return job.status ~= "running" end, 1200)
    t.eq(outcome, "victory")
    t.eq(ids("modify"), before.modify, "no effect hook was duplicated or lost")
    t.eq(ids("hit"), before.hit)
    t.ok(Battle.settings.inventory == inventory)
    local next_battle = battle_with(round_order.new(), "wisps", 1, shields.rules())
    next_battle:notify("setup")
    local wisp = next_battle.battlers[3]
    next_battle:perform({ kind = "skill", user = next_battle.battlers[2], skill = "fire", targets = { wisp } })
    t.ok(shields.is_broken(wisp), "the reloaded break rules apply")
  end,
  ["side view: a summon knocked out and dismissed at once leaves the presenter nothing to touch"] = function(t)
    setup()
    local outcome, dismissed
    local job = task.spawn(function()
      outcome = battle_kit.start({ stage = "arena", scheduler = round_order.new(), allow_defeat = true,
        setup = function(b)
          local hero = b:add({ actor = party.actor("hero"), side = "party", position = { slot = 1 } })
          hero.auto = true
          b:add({ enemy = "golem", side = "enemy", position = { slot = 1 } })
        end,
        rules = {
          on_start = function(b)
            local imp = b:summon({ enemy = "slime", side = "enemy" }, { owner = b:side("enemy")[1] })
            b.events:connect(function(name, payload) if name == "dismissed" and payload == imp then dismissed = true end end)
            imp:set_resource("hp", 0)
            b:settle()
          end,
          outcome = function(b) if b.round > 1 then return "draw" end end,
        } })
    end)
    t.wait_until(function() return job.status ~= "running" end, 60 * 60)
    t.eq(job.status, "done", tostring(job.error))
    t.eq(outcome, "draw")
    t.ok(dismissed, "the fallen summon left")
    t.tick(60)
    t.eq(fe.harness.errors(), 0, "no tween, popup or clip ran on the destroyed sprite")
  end,

  ["side view: an aborted battle (error or cancel) removes its scene and stage at once"] = function(t)
    require("fe.input").use_defaults()
    local scene = require("fe.scene")
    for _, how in ipairs({ "error", "cancel" }) do
      setup()
      local started, ended, ok, err
      local stop_started = battle_kit.started:connect(function(b) started = b end)
      local stop_finished = battle_kit.finished:connect(function() ended = true end)
      local job = task.spawn(function()
        ok, err = pcall(battle_kit.start, { stage = "arena", scheduler = round_order.new(), allow_defeat = true,
          setup = function(b)
            b:add({ actor = party.actor("hero"), side = "party", position = { slot = 1 } })
            b:add({ enemy = "golem", side = "enemy", position = { slot = 1 } })
          end,
          rules = {
            player_controller = function(_, me) return { kind = "defend", user = me } end,
            on_round = function(_, round) if round == 2 and how == "error" then error("abort the battle") end end,
          } })
      end)
      t.wait_until(function() return started ~= nil and started.round >= 1 end, 60 * 30, how)
      t.ok(scene.find("battle") ~= nil and fe.world.get("battle") ~= nil, how .. ": the battle is shown")
      if how == "cancel" then job:cancel() end
      t.wait_until(function() return job.status ~= "running" end, 60 * 60, how)
      stop_started(); stop_finished()
      if how == "error" then
        t.eq(ok, false)
        t.ok(tostring(err):find("abort the battle", 1, true), tostring(err))
      end
      t.ok(started.aborted, how)
      t.ok(not ended, how .. ": battle.finished is not emitted")
      t.eq(battle_kit.active, nil, how)
      t.tick(1)
      t.eq(scene.find("battle"), nil, how .. ": the battle scene is gone")
      t.eq(fe.world.get("battle"), nil, how .. ": the stage is unloaded")
    end
    t.eq(fe.harness.errors(), 0)
  end,

  ["only a living back-row partner can swap in"] = function(t)
    setup()
    local b = Battle({ scheduler = round_order.new({ choose = "turn" }), random = fe.random.new(1) })
    local hero = b:add({ actor = party.actor("hero"), side = "party", position = { slot = 1 } })
    local sage = b:add_reserve({ actor = party.actor("sage"), side = "party", position = { slot = 1 } })
    b:add({ enemy = "golem", side = "enemy" })
    t.ok(b:partner(hero) == sage)
    sage:set_resource("hp", 0)
    t.eq(b:partner(hero), nil, "a knocked-out partner is no partner")
    t.error(function() b:swap(hero, sage) end, "knocked%-out")
    t.ok(b.battlers[1] == hero and b.reserve[1] == sage, "nothing changed")
    sage:set_resource("hp", 10)
    t.ok(b:partner(hero) == sage)
  end,

  ["mock battles roll back battle state but not the clock, time of day or location"] = function(t)
    setup()
    for _, kit in ipairs({ "fe.game", "fe.daynight", "fe.field" }) do require(kit) end
    local clock, daytime, location = state.section("clock"), state.section("daynight"), state.section("field")
    clock.playtime, daytime.minutes, location.map = 100, 600, "town"
    local before = party.actor("hero"):resource("hp")
    battle_kit.mock(function()
      clock.playtime = 160
      daytime.minutes = 660
      location.map = "camp"
      state.set_flag("mock_flag")
      party.actor("hero"):set_resource("hp", 1)
      task.wait(0.05)
    end)
    t.ok(state.section("clock").playtime >= 160, "playtime keeps counting")
    t.eq(state.section("daynight").minutes, 660)
    t.eq(state.section("field").map, "camp")
    t.ok(not state.flag("mock_flag"), "flags roll back")
    t.eq(party.actor("hero"):resource("hp"), before, "HP rolls back")
  end,

  ["choose = round: while choosing, the preview shows this round and then the next"] = function(t)
    setup()
    local seen, executed = {}, {}
    local scheduler = round_order.new({ jitter = 0.5 })
    local b = Battle({ scheduler = scheduler, random = fe.random.new(5), rules = {
      player_controller = function(battle, me)
        local current, next_round = battle.scheduler.preview(battle)
        seen[battle.round] = seen[battle.round] or { current = current, next_round = next_round }
        return { kind = "defend", user = me }
      end,
      before_action = function(_, action) if action.kind ~= "defend" then action.cancelled = true end end,
      outcome = function(bb) if bb.round > 3 then return "draw" end end,
    } })
    b:populate(party.members(), "slimes")
    b.events:connect(function(name, payload) if name == "order" then executed[b.round] = payload end end)
    b:run()
    for round = 1, 3 do
      local s = seen[round]
      t.ok(s.current ~= s.next_round, "two lists")
      for i, battler in ipairs(executed[round]) do t.ok(s.current[i] == battler, "round " .. round .. " slot " .. i) end
      if executed[round + 1] then
        for i, battler in ipairs(executed[round + 1]) do t.ok(s.next_round[i] == battler, "next round slot " .. i) end
      end
    end
  end,

  ["boost is paid only when the action goes ahead, and multiplies the attack's own hits"] = function(t)
    setup()
    local shields = require("fe.battle.break")
    local saved = stats.resource_list
    local map = {}
    for _, d in ipairs(saved) do map[d.id] = d end
    map.bp = { max = 5, start = 1, name = "BP", battle_only = true }
    stats.resources(map)
    local cancel = false
    local b = Battle({ layers = { shields.rules(), { before_action = function(_, a) a.cancelled = cancel end } },
      scheduler = { run = function() end }, random = fe.random.new(1) })
    b:populate(party.members(), "wisps")
    local hero, wisp = b.battlers[1], b:side("enemy")[1]
    wisp:set_resource("hp", 200)
    hero:set_resource("bp", 3)
    cancel = true
    local action = { kind = "skill", user = hero, skill = "attack", targets = { wisp } }
    shields.boost(action, 2)
    t.eq(b:perform(action), nil)
    t.eq(hero:resource("bp"), 3, "a cancelled action costs no BP")
    cancel = false
    local attack_skill = Battle.settings.attack_skill
    Battle.settings.attack_skill = "flurry"
    action = { kind = "skill", user = hero, skill = "flurry", targets = { wisp } }
    shields.boost(action, 2)
    hero:set_resource("bp", 1)
    local results = b:perform(action)
    t.eq(action.boost, 1, "only the points the hero still had were paid")
    t.eq(#results, 6, "three hits, repeated once per point paid")
    t.eq(hero:resource("bp"), 0)
    Battle.settings.attack_skill = attack_skill
    stats.resources(map)
    stats.resource_list, stats.resource_by_id = saved, {}
    for _, d in ipairs(saved) do stats.resource_by_id[d.id] = d end
  end,

  ["a killing weakness hit still reveals the weakness and takes the shield point"] = function(t)
    setup()
    local shields = require("fe.battle.break")
    local b = battle_with(round_order.new(), "wisps", 1, shields.rules())
    b:notify("setup")
    local sage, wisp = b.battlers[2], b.battlers[3]
    wisp:set_resource("hp", 1)
    b:perform({ kind = "skill", user = sage, skill = "fire", targets = { wisp } })
    t.ok(not wisp:alive())
    t.ok(shields.revealed(wisp, "fire"))
    t.eq(wisp.data.shield, 0)
    t.ok(not shields.is_broken(wisp), "the fallen do not break")
  end,

  ["obstacles take no side and give no rewards"] = function(t)
    setup()
    local Battler = require("fe.battle.battler")
    t.error(function() Battler({ enemy = "crate", side = "enemy" }) end, "cannot take side")
    local b = battle_with(round_order.new(), "slimes")
    local block = b:add({ enemy = "slime", obstacle = true })
    t.eq(block.side, "obstacle")
    block:set_resource("hp", 0)
    t.eq(b:default_rewards().exp, 0, "a destroyed obstacle is no fallen foe")
  end,

  ["enemies with several actions per turn act that many times, each one presented"] = function(t)
    setup()
    for _, enemy in ipairs({ "hydra", "warlock" }) do
      local b = Battle({ scheduler = round_order.new(), random = fe.random.new(2), rules = {
        player_controller = function(_, me) return { kind = "defend", user = me } end,
        outcome = function(bb) if bb.round > 4 then return "draw" end end,
      } })
      b:populate(party.members())
      local foe = b:add({ enemy = enemy, side = "enemy" })
      local per_turn, current = {}, nil
      b:add_rules({ on_turn_start = function(_, who)
        current = who == foe and #per_turn + 1 or nil
        if current then per_turn[current] = 0 end
      end })
      b.events:connect(function(name, payload)
        if name == "action" and payload.user == foe then per_turn[current] = per_turn[current] + 1 end
      end)
      b:run()
      if enemy == "hydra" then
        t.eq(per_turn, { 2, 2, 2, 2 })
      else
        t.eq(per_turn, { 1, 2, 1, 2 }, "the warlock's function decides each turn")
      end
    end
    local b = battle_with(round_order.new(), "slimes")
    local hydra = b:add({ enemy = "hydra", side = "enemy" })
    hydra:add_state("sleep", 2)
    local acted = 0
    b.events:connect(function(name, payload) if name == "action" and payload.user == hydra then acted = acted + 1 end end)
    b:turn(hydra)
    t.eq(acted, 0, "a battler that cannot act takes none of its actions")
  end,

  ["kit rules(extra) adds extra as its own layer instead of overwriting hooks"] = function(t)
    setup()
    local shields = require("fe.battle.break")
    local limit = require("fe.battle.limit")
    local saved = stats.resource_list
    local map = {}
    for _, d in ipairs(saved) do map[d.id] = d end
    map.bp = { max = 5, start = 1, name = "BP", battle_only = true }
    stats.resources(map)
    local rounds = {}
    local layer = shields.rules(limit.rules({ on_round = function(_, r) rounds[#rounds + 1] = r end }))
    local b = battle_with(round_order.new(), "slimes", 1, layer)
    local hero = b.battlers[1]
    hero:set_resource("bp", 1)
    b:notify("on_round", 2)
    t.eq(hero:resource("bp"), 2, "the boost layer still gains BP")
    t.eq(rounds, { 2 }, "and the extra on_round ran too")
    t.ok(b:rule("on_round") ~= layer.on_round, "the extra layer is more specific")
    stats.resource_list, stats.resource_by_id = saved, {}
    for _, d in ipairs(saved) do stats.resource_by_id[d.id] = d end
  end,

  ["TP: gained each turn, spent on skills; a quietus works once per battle"] = function(t)
    setup()
    local tp = require("fe.battle.tp")
    local saved = stats.resource_list
    tp.declare({ max = 4, start = 1 })
    local b = battle_with(round_order.new(), "golem", 1, tp.rules())
    local hero, golem = b.battlers[1], b:side("enemy")[1]
    hero:reset_battle_resources()
    t.eq(hero:resource("tp"), 1, "battles start at the start value")
    b:turn_start(hero)
    t.eq(hero:resource("tp"), 2)
    local strike = data.get("skills", "last_stand")
    t.ok(b:usable(hero, strike))
    t.ok(b:perform({ kind = "skill", user = hero, skill = "last_stand", targets = { golem } }) ~= nil)
    t.eq(hero:resource("tp"), 0)
    for _ = 1, 3 do b:turn_start(hero) end
    t.eq(hero:resource("tp"), 3)
    t.ok(not b:usable(hero, strike), "a quietus is spent for the rest of the battle")
    t.eq(b:perform({ kind = "skill", user = hero, skill = "last_stand", targets = { golem } }), nil)
    t.eq(hero:resource("tp"), 3, "and costs nothing when refused")
    stats.resource_list, stats.resource_by_id = saved, {}
    for _, d in ipairs(saved) do stats.resource_by_id[d.id] = d end
  end,

  ["ATB battles run to a result with gauges"] = function(t)
    setup()
    local scheduler = require("fe.battle.schedulers.atb").new({ mode = "active", speed = 8 })
    local b = battle_with(scheduler, "slimes", 9)
    for _, battler in ipairs(b.battlers) do battler.data.gauge = 0 end
    local turns, first, overfull = {}, nil, false
    b:add_rules({ on_turn_start = function(_, who)
      first = first or who:name()
      turns[who:name()] = (turns[who:name()] or 0) + 1
    end })
    local watcher = task.spawn(function()
      while true do
        for _, x in ipairs(b.battlers) do
          if (x.data.gauge or 0) > 1 then overfull = true end
        end
        task.frame()
      end
    end)
    local outcome = b:run()
    watcher:cancel()
    t.eq(outcome, "victory")
    t.eq(first, "Hero", "from empty gauges the fastest battler acts first")
    t.ok((turns.Hero or 0) >= (turns.Sage or 0), "the faster member takes at least as many turns")
    t.ok(not overfull, "gauges stop at full")
    t.ok(b.battlers[1].data.gauge <= 1)
  end,

  ["Battle:perform names its refusal"] = function(t)
    setup()
    local b = battle_with(round_order.new(), "slimes")
    local hero, slime = b.battlers[1], b.battlers[3]
    local attack = { kind = "skill", user = hero, skill = "attack", targets = { slime } }
    hero:add_state("stun")
    t.eq({ b:perform(attack) }, { nil, "cannot_act" })
    hero:remove_state("stun")
    b.rules.before_action = function(_, a) a.cancelled = true end
    t.eq({ b:perform({ kind = "skill", user = hero, skill = "attack", targets = { slime } }) }, { nil, "cancelled" })
    b.rules.before_action = nil
    hero:set_resource("mp", 0)
    t.eq({ b:perform({ kind = "skill", user = hero, skill = "cleave", targets = { slime } }) }, { nil, "unaffordable" })
    b.rules.usable = function() return false end
    t.eq({ b:perform({ kind = "skill", user = hero, skill = "attack", targets = { slime } }) }, { nil, "vetoed" })
    t.eq({ b:usable(hero, data.get("skills", "attack")) }, { false, "vetoed" })
    b.rules.usable = nil
    t.eq({ b:perform({ kind = "item", user = hero, item = "potion", targets = { hero } }) }, { nil, "unaffordable" })
    local store = Battle.settings.inventory
    Battle.settings.inventory = nil
    t.eq({ b:perform({ kind = "item", user = hero, item = "potion", targets = { hero } }) }, { nil, "no_inventory" })
    Battle.settings.inventory = store
    t.ok(b:perform({ kind = "skill", user = hero, skill = "attack", targets = { slime } }) ~= nil)
  end,

  ["previews run no apply hooks and keep the random stream"] = function(t)
    setup()
    local b = battle_with(round_order.new(), "slimes")
    local hero, slime = b.battlers[1], b.battlers[3]
    local applied, after = 0, 0
    b:effect_hook("apply", function() applied = applied + 1 end)
    b:effect_hook("after", function() after = after + 1 end)
    local attack = data.get("skills", "attack")
    effects.resolve({ user = hero, targets = { slime }, source = attack, battle = b, preview = true })
    t.eq(applied, 0, "apply hooks may have side effects, so previews skip them")
    t.eq(after, 0)
    effects.resolve({ user = hero, targets = { slime }, source = attack, battle = b, random = b.random })
    t.eq(applied, 1)
    t.eq(after, 1)
  end,

  ["skills whose targets fell keep their target kind"] = function(t)
    setup()
    local b = battle_with(round_order.new(), "slimes", 6)
    local hero, slime_a, slime_b = b.battlers[1], b.battlers[3], b.battlers[4]
    local golem = b:add({ enemy = "golem", side = "enemy" })
    slime_a:set_resource("hp", 0)
    b:settle()
    local results = b:perform({ kind = "skill", user = hero, skill = "cleave", targets = { slime_a } })
    local hit = {}
    for _, r in ipairs(results) do hit[r.target] = true end
    t.ok(hit[slime_b] and hit[golem] and not hit[slime_a], "an all-enemies skill falls back to every living foe")
    results = b:perform({ kind = "skill", user = hero, skill = "attack", targets = { slime_a } })
    t.eq(#results, 1, "a single-target skill picks one")
    b.random = fe.random.new(99)
    b:perform({ kind = "skill", user = hero, skill = "might", targets = {} })
    t.eq(#hero:buffs_of("buff"), 1, "a self skill falls back to the user")
    t.eq(b.random:float(), fe.random.new(99):float(), "without drawing from the random stream")
  end,

  ["rewards go to the battlers who fought; benched actives get the reserve share; JP reaches both classes"] = function(t)
    setup()
    party.join("scout", { active = false })
    local hero, sage, scout = party.actor("hero"), party.actor("sage"), party.actor("scout")
    hero:set_secondary("mage")
    local b = Battle({ scheduler = round_order.new(), random = fe.random.new(2) })
    b:add({ actor = hero, side = "party", position = { slot = 1 } }).auto = true
    b:add({ actor = scout, side = "party", position = { slot = 2 } }).auto = true
    b:add({ enemy = "slime", side = "enemy" }):set_resource("hp", 1)
    t.eq(#b:participants(), 2)
    local outcome, rewards = b:run()
    t.eq(outcome, "victory")
    t.eq(rewards.exp, 30)
    t.eq(hero:exp(), 30)
    t.eq(scout:exp(), 30, "a reserve member who fought gets the full share")
    t.eq(sage:exp(), 15, "an active member who stayed out gets the reserve share")
    t.eq(hero:jp("fighter"), 5)
    t.eq(hero:jp("mage"), 5, "the secondary class learns too")
    t.eq(scout:jp(), 5)
    t.eq(sage:jp(), 0)
    local named = tablex.map(rewards.levels, function(e) return e.actor.id .. (e.reserve and "*" or "") end)
    t.eq(named, { "hero", "scout", "sage*" })
  end,

  ["a revived enemy comes back with full shields"] = function(t)
    setup()
    local shields = require("fe.battle.break")
    local b = battle_with(round_order.new(), "wisps", 1, shields.rules())
    b:notify("setup")
    local events = record(b)
    local sage, wisp = b.battlers[2], b.battlers[3]
    wisp:set_resource("hp", 1)
    b:perform({ kind = "skill", user = sage, skill = "fire", targets = { wisp } })
    t.ok(not wisp:alive())
    t.eq(wisp.data.shield, 0)
    wisp:set_resource("hp", 50)
    b:settle()
    t.eq(wisp.data.shield, wisp.data.max_shield, "refilled on revival")
    t.ok(not shields.is_broken(wisp))
    t.eq(count(events, "shield_restored"), 1)
  end,

  ["ATB cancels a player's decision when the chooser falls"] = function(t)
    setup()
    local scheduler = require("fe.battle.schedulers.atb").new({ mode = "active", speed = 8 })
    local choosing
    local rules = {
      player_controller = function(_, b)
        if b:name() == "Hero" then
          choosing = task.current()
          task.wait_until(function() return false end)
        end
        return { kind = "defend", user = b }
      end,
      outcome = function(bb) if bb.data.done then return "draw" end end,
    }
    local b = battle_with(scheduler, "slimes", 5, rules)
    local hero = b.battlers[1]
    hero:set_resource("hp", 1)
    local job = task.spawn(function() b:run() end)
    t.wait_until(function() return choosing ~= nil end, 600)
    t.wait_until(function() return not hero:alive() end, 60 * 30, "a slime finishes the hero while he chooses")
    t.tick(2)
    t.eq(choosing.status, "cancelled", "the menu task is cancelled")
    t.eq(hero.data.gauge, 0)
    t.ok(not hero.data.queued)
    b.data.done = true
    t.wait_until(function() return job.status ~= "running" end, 600)
    t.eq(job.status, "done", tostring(job.error))
  end,

  ["states re-applied during the bearer's own action stay fresh that turn"] = function(t)
    setup()
    local b = battle_with(round_order.new(), "slimes")
    local hero = b.battlers[1]
    hero:add_state("focus", 2)
    b.rules.actions = { refocus = function(_, action) action.user:add_state("focus", 2) return {} end }
    b:turn(hero, { kind = "refocus", user = hero })
    t.eq(hero.state_turns.focus, 2, "re-applied with the same duration: not counted down")
    b:turn(hero, false)
    t.eq(hero.state_turns.focus, 1)
  end,

  ["ambushes and pre-emptive strikes skip one side's first turns in every scheduler"] = function(t)
    for _, name in ipairs({ "round_order", "atb", "initiative" }) do
      for opening, skipping in pairs({ ambush = "party", preemptive = "enemy" }) do
        setup()
        local turns, first_action, openings = {}, {}, {}
        local rules = {
          before_action = function(_, action)
            local user = action.user
            if first_action[user] == nil then first_action[user] = turns[user] or 0 end
            action.cancelled = true
          end,
          on_turn_end = function(_, who) turns[who] = (turns[who] or 0) + 1 end,
          outcome = function(bb)
            for _, who in ipairs(bb.battlers) do
              if (turns[who] or 0) < 2 then return nil end
            end
            return "draw"
          end,
        }
        local b = Battle({ scheduler = schedulers[name](), troop = "slimes", random = fe.random.new(3), rules = rules,
          opening = opening })
        b:populate(party.members(), "slimes")
        b.events:connect(function(event, payload) if event == "opening" then openings[#openings + 1] = payload end end)
        t.eq(b:run(), "draw")
        t.eq(#openings, 1, name .. ": the opening is presented once")
        t.eq(openings[1].kind, opening)
        t.eq(openings[1].side, skipping)
        for _, who in ipairs(b.battlers) do
          local label = name .. " " .. opening .. ": " .. who:name()
          t.eq(first_action[who], who.side == skipping and 1 or 0, label .. " first acts on turn " .. tostring(first_action[who]))
        end
      end
    end
    t.error(function() Battle({ scheduler = {}, opening = "surprise" }) end, "unknown opening")
  end,

  ["Battle.speed scales the presenters' waits"] = function(t)
    local common = require("fe.battle.presenter_common")
    local config = require("fe.config")
    local saved = config.values.battle_speed
    config.values.battle_speed = 4
    Battle.settings.speed = nil
    t.eq(Battle.speed(), 4, "the player's setting")
    Battle.settings.speed = 2
    t.eq(Battle.speed(), 2, "the game's setting wins")
    local elapsed
    local job = task.spawn(function()
      local started = task.time()
      common.wait(0.4)
      elapsed = task.time() - started
    end)
    t.wait_until(function() return job.status ~= "running" end, 120)
    t.near(elapsed, 0.2, 0.04)
    Battle.settings.speed = 0
    t.eq(Battle.speed(), 0.1, "never stops time")
    Battle.settings.speed = nil
    config.values.battle_speed = saved
  end,

  ["counter forecasts give the chance without rolling"] = function(t)
    setup()
    local reactions = require("fe.battle.reactions")
    local rules = { reaction = reactions.counter({ requires = "counter", chance = 0.5 }) }
    local b = battle_with(round_order.new(), "brute", 8, rules)
    local hero, sage, brute = b.battlers[1], b.battlers[2], b.battlers[3]
    b.random = fe.random.new(7)
    t.near(reactions.forecast(b, brute, { kind = "skill", user = hero, skill = "attack", targets = { brute } }), 0.5)
    t.eq(reactions.forecast(b, hero, { kind = "skill", user = brute, skill = "attack", targets = { hero } }), 0,
      "the hero cannot counter")
    t.eq(reactions.forecast(b, brute, { kind = "skill", user = sage, skill = "fire", targets = { brute } }), 0,
      "spells from afar are not countered")
    brute:add_state("stun")
    t.eq(reactions.forecast(b, brute, { kind = "skill", user = hero, skill = "attack", targets = { brute } }), 0,
      "a stunned defender cannot counter")
    brute:remove_state("stun")
    t.eq(b.random:float(), fe.random.new(7):float(), "nothing was rolled")
    b.rules.reaction = function() end
    t.eq(reactions.forecast(b, brute, { kind = "skill", user = hero, skill = "attack", targets = { brute } }), nil,
      "a plain function has no forecast")
  end,

  ["damage and healing caps apply after variance"] = function(t)
    setup()
    local b = battle_with(round_order.new(), "slimes")
    local hero, sage, slime = b.battlers[1], b.battlers[2], b.battlers[3]
    effects.settings.max_damage = 5
    effects.settings.max_heal = 7
    local hit = effects.resolve({ user = hero, targets = { slime }, source = data.get("skills", "attack"), battle = b,
      random = b.random })[1]
    t.eq(hit.amount, 5)
    hero:set_resource("hp", 1)
    local heal = effects.resolve({ user = sage, targets = { hero }, source = data.get("skills", "heal"), battle = b,
      random = b.random })[1]
    t.eq(heal.amount, 7)
    effects.settings.max_damage, effects.settings.max_heal = nil, nil
    t.ok(effects.resolve({ user = hero, targets = { slime }, source = data.get("skills", "attack"), battle = b,
      random = b.random })[1].amount > 5)
  end,

  ["weaknesses can be revealed and changed mid-battle"] = function(t)
    setup()
    local shields = require("fe.battle.break")
    local b = battle_with(round_order.new(), "wisps", 1, shields.rules())
    b:notify("setup")
    local events = record(b)
    local hero, wisp = b.battlers[1], b.battlers[3]
    t.ok(shields.reveal(b, wisp, "fire"), "hidden until now")
    t.ok(shields.revealed(wisp, "fire"))
    t.ok(not shields.reveal(b, wisp, "fire"), "already known")
    t.eq(count(events, "weakness_revealed"), 1)
    shields.set_weaknesses(wisp, { "sword" })
    t.eq(shields.weaknesses(wisp), { "sword" })
    b:perform({ kind = "skill", user = hero, skill = "attack", targets = { wisp } })
    t.ok(shields.is_broken(wisp), "the sword is a weakness now")
    shields.set_weaknesses(wisp, nil)
    t.eq(shields.weaknesses(wisp), { "fire" }, "back to the data")
  end,

  ["experience per action is granted at once and presented"] = function(t)
    setup()
    Battle.settings.exp_per_action = function(_, action) return action.kind == "skill" and 30 or 0 end
    local b = battle_with(round_order.new(), "slimes")
    local events = record(b)
    local hero, slime = b.battlers[1], b.battlers[3]
    b:perform({ kind = "skill", user = hero, skill = "attack", targets = { slime } })
    Battle.settings.exp_per_action = nil
    t.eq(hero.actor:exp(), 30)
    t.eq(hero.actor:level(), 2)
    t.eq(count(events, "exp", function(p) return p.battler == hero and p.amount == 30 and p.gains[1].level == 2 end), 1)
    b:perform({ kind = "skill", user = slime, skill = "attack", targets = { hero } })
    t.eq(count(events, "exp"), 1, "enemies gain nothing, and the setting is off again")
  end,

  ["the side-view HUD computes the turn preview once per battle event"] = function(t)
    setup()
    local hud = require("fe.battle.sideview_hud")
    local scheduler = round_order.new()
    local calls, preview = 0, scheduler.preview
    scheduler.preview = function(...) calls = calls + 1 return preview(...) end
    local b = battle_with(scheduler, "slimes")
    local p = { battle = b, stamp = 0 }
    local order = hud.preview(p)
    t.ok(hud.preview(p) == order)
    t.eq(calls, 1)
    p.stamp = 1
    hud.preview(p)
    t.eq(calls, 2, "an event refreshes it")
    b.current = b.battlers[1]
    hud.preview(p)
    t.eq(calls, 3, "so does a change of the acting battler")
    local presenter = require("fe.battle.sideview").new(b, {})
    b.presenter = nil
    b:present("round", 1)
    t.eq(presenter.stamp, 1, "presenters count events")
  end,

  ["a cancelled effect shows as nullified in the side view and aborts nothing"] = function(t)
    require("fe.input").use_defaults()
    setup()
    local outcome, popups
    local job = task.spawn(function()
      outcome = battle_kit.start({ stage = "arena", scheduler = round_order.new(), allow_defeat = true,
        setup = function(b)
          local hero = b:add({ actor = party.actor("hero"), side = "party", position = { slot = 1 } })
          hero.auto = true
          b:add({ enemy = "golem", side = "enemy", position = { slot = 1 } })
        end,
        rules = {
          effect_hooks = { { phase = "before", fn = function(ctx) ctx.cancelled = true end } },
          after_action = function(b, _, results)
            if results and results[1] and results[1].cancelled then
              b.data.nullified = true
              popups = tablex.map(b.presenter.popups, function(popup) return popup.text end)
            end
          end,
          outcome = function(b) if b.data.nullified then return "draw" end end,
        } })
    end)
    t.wait_until(function() return job.status ~= "running" end, 60 * 60)
    t.eq(job.status, "done", tostring(job.error))
    t.eq(outcome, "draw")
    t.eq(popups, { "Nullified" })
    t.tick(30)
    t.eq(fe.harness.errors(), 0)
  end,

  ["effect records are validated against their handlers' schemas"] = function(t)
    setup()
    local function problems(effect) return effects.validate(effect, "e") end
    t.eq(problems({ type = "damage", formula = "physical", power = 100 }), {})
    t.eq(problems({ type = "push", distance = 1 }), {}, "the tactical kit's push is known through its provider")
    t.ok(problems({ type = "steal" })[1]:find("no effect handler"), "a type without a handler")
    effects.handler("steal", function() return {} end)
    t.eq(problems({ type = "steal", anything = true }), {}, "a handler without a schema accepts anything")
    effects.handlers.steal = nil
    t.ok(problems({ type = "add_state", state = "nonsense" })[1]:find("unknown states"))
    t.ok(problems({ type = "add_state", state = "poison", chance = 2 })[1]:find("above maximum"))
    t.ok(problems({ type = "buff", mul = 1.5 })[1]:find("stat: missing"))
    t.ok(problems({ type = "revive", fraction = "half" })[1]:find("expected number"))
    t.ok(problems({ power = 3 })[1]:find("type: missing"))
    local database = require("fe.rpg.database")
    t.eq(database.validate_effects("skills"), {})
    local member = data.get("troops", "arena_slimes").members[1]
    t.eq(member.id, "left")
    t.eq(member.facing, { x = 1, z = 0 })
    t.eq(member.position.layer, 0)
    t.ok(data.validate("troops", { { id = "bad", members = { { enemy = "slime", facing = { x = "east" } } } } }) == false)
    t.ok(data.get("skills", "attack").uses_weapon == true and data.get("skills", "longshot").los == true)
    t.eq(data.get("skills", "fire").uses_weapon, false, "schema defaults")
  end,
}
