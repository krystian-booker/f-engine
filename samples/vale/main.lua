-- Crystal of the Vale: a classic side-view, active-time-battle sample.
local battle = require("fe.battle")
local devtools = require("fe.devtools")
local field = require("fe.field")
local inventory = require("fe.rpg.inventory")
local party = require("fe.rpg.party")
local quest = require("fe.quest")

battle.settings.presentation = "fe.battle.sideview"
local sideview = require("fe.battle.sideview")
sideview.settings.command_names.art = "Arts"
sideview.settings.command_names.magic = "Magic"
battle.rules = require("battle_rules")

return require("fe.starter").main({
  title = "Crystal of the Vale",
  subtitle = "An f-engine sample",
  new_game = function()
    party.join("aster")
    party.join("mira")
    inventory.add("potion", 3)
    inventory.add_money(150)
    quest.start("fading_crystal")
    field.start("town", "entry")
  end,
  setup = function()
    field.settings.followers = 1
    devtools.register("Battle: slimes", function() battle.encounter("slimes", { stage = "battle_grass" }) end)
    devtools.register("Battle: guardian", function() battle.encounter("guardian") end)
    devtools.register("Warp: grotto", function() field.warp("grotto", "entry") end)
  end,
})
