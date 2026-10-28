local Battle = require("fe.battle.core")
local data = require("fe.data")
local effects = require("fe.effects")
local inventory = require("fe.rpg.inventory")
local party = require("fe.rpg.party")
local state = require("fe.state")

local function setup()
  if not data.has_table("skills") then require("fe.rpg.database").load() end
  state.reset()
  party.reset()
  inventory.reset()
  party.join("hero")
  party.join("sage")
end

local function battle_with(scheduler, troop, seed)
  local b = Battle({ scheduler = scheduler, troop = troop, random = fe.random.new(seed or 3) })
  b:populate(party.members(), troop)
  return b
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

  ["ATB battles run to a result with gauges"] = function(t)
    setup()
    local scheduler = require("fe.battle.schedulers.atb").new({ mode = "active", speed = 8 })
    local b = battle_with(scheduler, "slimes", 9)
    local outcome = b:run()
    t.eq(outcome, "victory")
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
}
