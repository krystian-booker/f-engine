-- Wayfarers: two travellers, round-order battles with shields and boosts,
-- verbs for talking to townsfolk, chapters, a world map and day/night.
local battle = require("fe.battle")
local daynight = require("fe.daynight")
local devtools = require("fe.devtools")
local field = require("fe.field")
local inventory = require("fe.rpg.inventory")
local party = require("fe.rpg.party")
local quest = require("fe.quest")
local stats = require("fe.rpg.stats")
local worldmap = require("fe.worldmap")

-- This game's stat block: SP instead of MP, plus Boost Points in battle.
stats.define({
  { id = "max_hp", name = "Max HP", short = "HP", min = 1, max = 9999 },
  { id = "max_sp", name = "Max SP", short = "SP", max = 999 },
  { id = "attack", name = "Phys. Atk", short = "P.Atk" },
  { id = "defense", name = "Phys. Def", short = "P.Def" },
  { id = "magic", name = "Elem. Atk", short = "E.Atk" },
  { id = "spirit", name = "Elem. Def", short = "E.Def" },
  { id = "speed", name = "Speed", short = "Spd", min = 1 },
  { id = "accuracy", name = "Accuracy", short = "Acc" },
  { id = "evasion", name = "Evasion", short = "Eva" },
  { id = "critical", name = "Critical", short = "Crit", max = 100 },
})
stats.resources({
  hp = { max = "max_hp", name = "HP" },
  sp = { max = "max_sp", name = "SP" },
  bp = { max = 5, start = 1, name = "BP", battle_only = true },
})

battle.settings.scheduler = function() return require("fe.battle.schedulers.round_order").new() end
battle.rules = require("battle.rules")
inventory.settings.currency = "Leaves"
party.settings.max_active = 4

return require("fe.starter").main({
  title = "Wayfarers",
  subtitle = "An f-engine sample",
  new_game = function()
    party.join("rook")
    inventory.add("tonic", 4)
    inventory.add_money(300)
    quest.start("rook_1")
    quest.set_chapter("rook", 1)
    daynight.set_hour(9)
    field.start("riverford", "start")
  end,
  setup = function()
    require("verbs").install()
    worldmap.load()
    daynight.start({ minutes_per_second = 4 })
    daynight.hour_changed:connect(function(hour)
      if field.world and field.world:props().daynight then
        field.world:set_environment({ post = { lut_blend = (hour >= 19 or hour < 6) and 1 or 0 } })
      end
    end)
    devtools.register("Battle: wolves", function() battle.encounter("wolves") end)
    devtools.register("Battle: bandits", function() battle.encounter("bandits") end)
    devtools.register("Night", function() daynight.set_hour(23) end)
    devtools.register("World map", function() worldmap.open() end)
  end,
})
