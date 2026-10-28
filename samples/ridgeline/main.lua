-- Ridgeline: talk to your companions, settle a council vote, then fight a
-- grid battle on the ridge where height, facing and terrain matter.
local devtools = require("fe.devtools")
local field = require("fe.field")
local inventory = require("fe.rpg.inventory")
local party = require("fe.rpg.party")

party.settings.max_active = 6

return require("fe.starter").main({
  title = "Ridgeline",
  subtitle = "An f-engine sample",
  new_game = function()
    for _, id in ipairs({ "cora", "bram", "lio", "wren" }) do party.join(id) end
    inventory.add("salve", 3)
    field.start("camp", "start")
  end,
  setup = function()
    devtools.register("Council", function() require("council").run() end)
    devtools.register("Battle: hold the pass", function() require("ridge_battle").fight("hold") end)
    devtools.register("Battle: strike the captain", function() require("ridge_battle").fight("flank") end)
  end,
})
