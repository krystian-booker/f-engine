-- Wayfarers: two travellers with their own chapters, round-order battles
-- with shields and boosts, verbs for townsfolk (including guiding one as a
-- follower), a world map and an Octopath II-style day/night switch (T, or
-- the left stick button).
local battle = require("fe.battle")
local daynight = require("fe.daynight")
local devtools = require("fe.devtools")
local field = require("fe.field")
local inventory = require("fe.rpg.inventory")
local party = require("fe.rpg.party")
local quest = require("fe.quest")
local stats = require("fe.rpg.stats")
local story = require("fe.story")
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
  lp = { max = 3, start = 0, name = "LP", battle_only = true },
})

battle.settings.scheduler = function()
  return require("fe.battle.schedulers.round_order").new({ choose = "turn", jitter = 0.1 })
end
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
    story.recruit("rook")
    story.begin("rook")
    daynight.set_hour(9)
    field.start("riverford", "start")
  end,
  setup = function()
    story.define("rook", { name = "Rook", actor = "rook", chapters = {
      { title = "The Lost Trail" },
      { title = "The Shrine Below", level = 5, requires = { flags = { "found_cliffhaven" } } },
    } })
    story.define("selene", { name = "Selene", actor = "selene", chapters = { { title = "Pages in the Wind" } } })
    require("verbs").install()
    worldmap.load()
    daynight.start({ minutes_per_second = 4 })
    fe.input.define("toggle_time", { "key:T", "pad:leftstick" })
    field.command("daynight", { input = "toggle_time", run = function() daynight.toggle() end })
    devtools.register("Battle: wolves", function() battle.encounter("wolves") end)
    devtools.register("Battle: bandits", function() battle.encounter("bandits") end)
    devtools.register("Night", function() daynight.set_hour(23) end)
    devtools.register("World map", function() worldmap.open() end)
    devtools.register("Chapter select", function() story.select() end)
  end,
})
