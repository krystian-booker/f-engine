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
    local old_list = stats.list
    local declared = {}
    for _, def in ipairs(old_list) do declared[#declared + 1] = def end
    declared[#declared + 1] = { id = "luck", name = "Luck", max = 50 }
    stats.define(declared)
    stats.resources({ hp = { max = "max_hp" }, mp = { max = "max_mp" }, bp = { max = 5, start = 1 } })
    local sage = party.join("sage")
    sage:init_resources(false)
    t.eq(sage:resource("bp"), 1)
    t.eq(sage:max_resource("bp"), 5)
    t.eq(sage:stat("luck"), 0, "a declared stat the data lacks reads 0")
    sage:set_custom_modifiers({ { stat = "luck", add = 80 } })
    t.eq(sage:stat("luck"), 50, "clamped to the stat's max")
    effects.formula("lucky", function(ctx) return ctx.user:stat("luck") end)
    local hit = effects.resolve({ user = sage, targets = { sage }, preview = true,
      source = { always_hit = true, effects = { { type = "damage", formula = "lucky", variance = 0 } } } })[1]
    t.eq(hit.amount, 50, "formulas read game-defined stats")
    effects.formulas.lucky = nil
    sage:set_custom_modifiers(nil)
    stats.define(old_list)
    stats.resources({ hp = { max = "max_hp" }, mp = { max = "max_mp" } })
    t.eq(sage:stat("luck"), 0, "undeclared again")
    t.eq(sage:stats().luck, nil)
  end,

  ["equipping refuses with a reason and never loses the old item"] = function(t)
    setup()
    local hero = party.join("hero")
    inventory.add("bronze_sword", 99)
    t.eq({ hero:equip("weapon", nil) }, { false, "inventory_full" })
    t.eq(hero:equipped("weapon"), "bronze_sword", "still equipped")
    t.eq(inventory.count("bronze_sword"), 99)
    inventory.add("oak_staff")
    t.eq({ hero:equip("weapon", "oak_staff") }, { false, "cannot_equip" })
    inventory.remove("bronze_sword", 1)
    hero:set_class("mage")
    t.eq({ hero:equip("weapon", "oak_staff") }, { true })
    t.eq(inventory.count("bronze_sword"), 99)
    t.eq({ hero:equip("weapon", "bronze_sword") }, { false, "cannot_equip" })
    hero:set_class("fighter")
    inventory.remove("bronze_sword", 99)
    t.eq({ hero:equip("weapon", "bronze_sword") }, { false, "not_held" })
    t.eq(hero:equipped("weapon"), "oak_staff")
    -- Re-equipping the equipped item is a no-op, even with a full stack of it
    -- in the bag (the copy taken off used to vanish).
    inventory.add("oak_staff", 99)
    t.eq(inventory.count("oak_staff"), 99)
    t.eq({ hero:equip("weapon", "oak_staff") }, { true })
    t.eq(hero:equipped("weapon"), "oak_staff")
    t.eq(inventory.count("oak_staff"), 99, "nothing lost")
  end,

  ["stat caches follow every change, hot reloads included"] = function(t)
    setup()
    local Battler = require("fe.battle.battler")
    local Combatant = require("fe.rpg.combatant")
    local hero = party.join("hero")
    local b = Battler({ actor = hero, side = "party" })
    t.eq(b:stat("attack"), 28)
    local computed = 0
    local compute = hero.compute_stats
    hero.compute_stats = function(self) computed = computed + 1 return compute(self) end
    hero:stat("attack") hero:stat("speed") hero:max_resource("hp")
    t.eq(computed, 0, "cached")
    hero:add_state("slow")
    t.eq(b:stat("speed"), 7, "the battler sees a state added through the actor")
    b:remove_state("slow")
    t.eq(hero:stat("speed"), 14, "and the actor one removed through the battler")
    b:buff({ stat = "attack", add = 2 }, 2)
    t.eq(b:stat("attack"), 30)
    t.eq(hero:stat("attack"), 28, "buffs belong to the battler")
    b:tick_buffs() b:tick_buffs()
    t.eq(b:stat("attack"), 28)
    b:buff({ stat = "attack", add = 2 }, 2)
    b:clear_buffs()
    t.eq(b:stat("attack"), 28)
    hero:equip("weapon", nil)
    t.eq(b:stat("attack"), 20)
    hero:equip("weapon", "bronze_sword")
    t.eq(hero:stat("attack"), 28)
    hero:set_custom_modifiers({ { stat = "attack", add = 3 } })
    t.eq(b:stat("attack"), 31)
    hero:set_custom_modifiers(nil)
    hero:learn("guard_up")
    t.eq(hero:stat("defense"), 15, "a learned passive")
    hero:gain_exp(hero.exp_for_level(2))
    t.eq(hero:stat("attack"), 31, "a level")
    hero:set_secondary("mage")
    t.eq(b:stat("spirit"), 10)
    hero:set_secondary(nil)
    hero:set_class("monk")
    t.ok(hero:equip_support(1, "quick_step") == false, "not learned yet")
    hero:learn("quick_step")
    local speed = hero:stat("speed")
    hero:equip_support(1, "quick_step")
    t.eq(hero:stat("speed"), speed + 4)
    hero:equip_support(1, nil)
    t.eq(b:stat("speed"), speed)
    hero:set_class("fighter")
    t.eq(hero:stat("speed"), 19, "level 2, and the quick step passive applies without slots")
    hero:add_state("slow", 1)
    t.eq(hero:stat("speed"), 10)
    hero:tick_states()
    t.eq(b:stat("speed"), 19)
    hero:add_state("slow")
    hero:clear_states("battle_end")
    t.eq(hero:stat("speed"), 19)
    hero:add_state("slow")
    b:remove_states_tagged("nothing")
    t.eq(hero:stat("speed"), 10)
    hero.state_turns.slow = nil -- a change the kit cannot see
    t.eq(hero:stat("speed"), 10, "still cached")
    hero:invalidate()
    t.eq(hero:stat("speed"), 19, "until told")
    hero:add_state("slow")
    t.eq(hero:stat("speed"), 10)
    local slow = data.get("states", "slow")
    slow.modifiers[1].mul = 0.25
    t.eq(hero:stat("speed"), 10, "edited data is not seen...")
    data.changed:emit("states")
    t.eq(hero:stat("speed"), 5, "...until the table reloads")
    slow.modifiers[1].mul = 0.5
    Combatant.invalidate_all()
    t.eq(b:stat("speed"), 10)
    hero.compute_stats = nil
    t.ok(fe.harness.reload("fe.rpg.combatant"))
    t.ok(fe.harness.reload("fe.rpg.actor"))
    t.ok(fe.harness.reload("fe.battle.battler"))
    t.eq(fe.harness.errors(), 0)
    hero:remove_state("slow")
    t.eq(b:stat("speed"), 19, "a reloaded kit keeps the cache honest")
    b:buff({ stat = "speed", add = 5 }, 1)
    t.eq(b:stat("speed"), 24)
    t.ok(computed > 0)
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
