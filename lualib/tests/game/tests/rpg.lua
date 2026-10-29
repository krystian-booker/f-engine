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

  ["a secondary class adds its stat modifiers and commands"] = function(t)
    setup()
    local hero = party.join("hero")
    local spirit = hero:stat("spirit")
    hero:set_secondary("mage")
    t.eq(hero:stat("spirit"), spirit + 4)
    t.eq(hero:commands(), { "attack", "skill", "item", "magic", "defend", "flee" })
    hero:set_secondary(nil)
    t.eq(hero:stat("spirit"), spirit)
  end,

  ["a single-command class can take a secondary class"] = function(t)
    setup()
    local hero = party.join("hero")
    hero:set_class("monk")
    t.eq(hero:commands(), { "attack" })
    hero:set_secondary("mage")
    t.eq(hero:commands(), { "attack", "magic", "item", "defend", "flee" })
  end,

  ["support abilities apply only while equipped in a slot"] = function(t)
    setup()
    local hero = party.join("hero")
    hero:learn("quick_step")
    hero:learn("iron_skin")
    local speed, defense = hero:stat("speed"), hero:stat("defense")
    t.eq(hero:support_slots(), nil)
    t.eq(hero:stat("speed"), speed, "learned before: already counted without slots")
    hero:set_class("monk")
    local monk_speed, monk_defense = hero:stat("speed"), hero:stat("defense")
    t.eq(hero:support_slots(), 1)
    t.ok(hero:equip_support(1, "quick_step"))
    t.eq(hero:stat("speed"), monk_speed + 4)
    t.eq(hero:stat("defense"), monk_defense)
    t.ok(hero:equip_support(1, "iron_skin"))
    t.eq(hero:stat("speed"), monk_speed)
    t.eq(hero:stat("defense"), monk_defense + 3)
    t.ok(not hero:equip_support(2, "quick_step"), "only one slot")
    t.ok(not hero:equip_support(1, "guard_up"), "not a support ability")
    t.ok(speed > 0 and defense > 0)
  end,

  ["level ups do not revive knocked-out members"] = function(t)
    setup()
    local hero = party.join("hero")
    local sage = party.join("sage")
    sage:set_resource("hp", 0)
    party.gain_exp(hero.exp_for_level(4))
    t.eq(sage:resource("hp"), 0, "active members: no experience while down")
    party.settings.exp_for_knocked_out = true
    sage:gain_exp(sage.exp_for_level(5))
    party.settings.exp_for_knocked_out = false
    t.eq(sage:resource("hp"), 0)
    t.ok(sage:level() >= 5)
    local before = hero:resource("hp")
    hero:gain_exp(hero.exp_for_level(6))
    t.ok(hero:resource("hp") > before, "living members still grow")
  end,

  ["set modifiers follow priority, then a fixed order"] = function(t)
    setup()
    local hero = party.join("hero")
    hero:add_state("heavy")
    t.eq(hero:stat("move"), 1)
    hero:add_state("anchored")
    t.eq(hero:stat("move"), 2, "higher priority wins")
    hero:remove_state("anchored")
    t.eq(stats.compute({ move = 4 }, { { stat = "move", set = 1 }, { stat = "move", set = 3 } }).move, 3,
      "equal priority: the later modifier wins")
  end,

  ["the RPG kit takes treasure through the field signal and installs once"] = function(t)
    setup()
    local rpg = require("fe.rpg")
    local field = require("fe.field")
    local announce = rpg.announce
    local shown = {}
    rpg.announce = function(item, count, money) shown[#shown + 1] = { item, count, money } end
    rpg.install()
    local listeners = field.treasure:count()
    rpg.install()
    t.eq(field.treasure:count(), listeners, "a second install adds nothing")
    field.give("potion", 2)
    field.give(nil, 1, 50)
    rpg.announce = announce
    t.eq(inventory.count("potion"), 2)
    t.eq(inventory.money(), 50)
    t.eq(shown, { { "potion", 2 }, { nil, 1, 50 } })
    t.ok(require("fe.i18n").t("fe.rpg.obtained_items", { item = "Potion", count = 2 }):find("Potion") ~= nil)
  end,

  ["RPG pickups are toasts named from the item data, or messages when toasts are off"] = function(t)
    setup()
    local rpg = require("fe.rpg")
    local field = require("fe.field")
    local ui = require("fe.ui")
    rpg.install()
    local listeners = field.treasure:count()
    rpg.installed = false
    rpg.install()
    t.eq(field.treasure:count(), listeners, "installing again replaces the keyed listener")
    ui.clear_toasts()
    field.give("potion", 2, 40)
    local toasts = ui.toasts()
    t.eq(#toasts, 2)
    t.ok(toasts[1].text:find("Potion", 1, true) and toasts[1].text:find("x2", 1, true), toasts[1].text)
    t.ok(toasts[2].text:find("40", 1, true), toasts[2].text)
    t.eq(inventory.count("potion"), 2)
    ui.clear_toasts()
    field.settings.pickup_toasts = false
    local job = require("fe.task").spawn(function() field.give("ether", 1) end)
    t.tick(1)
    t.ok(require("fe.dialogue").current().line.text:find("Ether", 1, true), "a message names the item")
    job:cancel()
    field.settings.pickup_toasts = true
    t.eq(#ui.toasts(), 0)
    require("fe.scene").clear()
  end,

  ["actor objects follow restored saves"] = function(t)
    setup()
    local hero = party.join("hero")
    local saved = state.snapshot()
    hero:set_resource("hp", 1)
    state.restore(saved)
    local again = party.actor("hero")
    t.ok(again ~= hero, "a fresh object wraps the restored record")
    t.eq(again:resource("hp"), again:max_resource("hp"))
    t.ok(party.actor("hero") == again, "and it is cached")
  end,
}
