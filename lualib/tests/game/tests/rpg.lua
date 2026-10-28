local data = require("fe.data")
local effects = require("fe.effects")
local inventory = require("fe.rpg.inventory")
local party = require("fe.rpg.party")
local state = require("fe.state")
local stats = require("fe.rpg.stats")

local loaded = false
local function setup()
  if not loaded then
    require("fe.rpg.database").load()
    loaded = true
  end
  state.reset()
  party.reset()
  inventory.reset()
end

return {
  ["database loads and validates references"] = function(t)
    setup()
    t.eq(data.get("skills", "fire").element, "fire")
    t.eq(data.get("items", "potion").kind, "consumable") -- default filled by the schema
    t.ok(data.get("enemies", "slime").resist.fire == 2)
  end,

  ["actors grow by class, equipment and passives"] = function(t)
    setup()
    local hero = party.join("hero")
    t.eq(hero:stat("attack"), 28) -- 20 base + bronze sword 8
    t.eq(hero:resource("hp"), 120)
    local gains = hero:gain_exp(hero.exp_for_level(3))
    t.eq(hero:level(), 3)
    t.eq(#gains, 2)
    t.eq(gains[2].skills[1], "cleave")
    t.eq(hero:max_resource("hp"), 160)
    hero:gain_jp(40)
    t.ok(hero:learn_with_jp("guard_up"))
    t.eq(hero:stat("defense"), 10 + 2 * 2 + 5)
    t.eq(hero:jp(), 10)
  end,

  ["equipment respects class types and returns old items"] = function(t)
    setup()
    local hero = party.join("hero")
    inventory.add("oak_staff")
    t.ok(not hero:equip("weapon", "oak_staff"), "fighters cannot use staves")
    t.ok(hero:equip("weapon", nil))
    t.eq(inventory.count("bronze_sword"), 1)
    t.ok(hero:equip("weapon", "bronze_sword"))
    t.eq(inventory.count("bronze_sword"), 0)
    local preview = hero:preview_equip("weapon", nil)
    t.eq(preview.attack, 20)
    t.eq(hero:stat("attack"), 28)
  end,

  ["party roster, active line-up and shared experience"] = function(t)
    setup()
    party.settings.max_active = 2
    party.join("hero")
    party.join("sage")
    party.join("scout")
    t.eq(#party.members(), 2)
    t.eq(party.reserve()[1].id, "scout")
    party.set_active({ "scout", "hero" })
    t.eq(party.leader().id, "scout")
    local results = party.gain_exp(100)
    t.eq(#results, 3)
    t.eq(party.actor("sage"):exp(), 50) -- reserve members get half
    party.settings.max_active = 4
  end,

  ["inventory stacks, money and saves"] = function(t)
    setup()
    t.eq(inventory.add("potion", 120), 99)
    t.ok(not inventory.remove("ether", 1))
    inventory.add_money(50)
    t.ok(inventory.spend(30))
    t.ok(not inventory.spend(30))
    local snapshot = state.snapshot()
    inventory.reset()
    state.restore(snapshot)
    t.eq(inventory.count("potion"), 99)
    t.eq(inventory.money(), 20)
  end,

  ["custom stats and resources are data, not engine code"] = function(t)
    setup()
    local old_list, old_resources = stats.list, stats.resource_list
    stats.resources({ hp = { max = "max_hp" }, mp = { max = "max_mp" }, bp = { max = 5, start = 1 } })
    local sage = party.join("sage")
    sage:init_resources(false)
    t.eq(sage:resource("bp"), 1)
    t.eq(sage:max_resource("bp"), 5)
    stats.resources({ hp = { max = "max_hp" }, mp = { max = "max_mp" } })
    t.ok(old_list == stats.list)
    t.ok(old_resources ~= nil)
  end,

  ["effects heal, damage with affinity and preview without changing"] = function(t)
    setup()
    local sage = party.join("sage")
    sage:set_resource("hp", 10)
    local results = effects.resolve({ user = sage, targets = { sage }, source = data.get("items", "potion").use and {
      effects = data.get("items", "potion").use.effects, always_hit = true } })
    t.eq(results[1].amount, 50)
    t.eq(sage:resource("hp"), 60)
    local preview = effects.resolve({ user = sage, targets = { sage }, source = data.get("skills", "heal"), preview = true })
    t.ok(preview[1].amount > 0)
    t.eq(sage:resource("hp"), 60)
    t.ok(effects.useful(data.get("skills", "heal"), sage))
  end,
}
