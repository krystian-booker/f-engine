-- The inn: pay to fully recover the party.
local dialogue = require("fe.dialogue")
local inventory = require("fe.rpg.inventory")
local party = require("fe.rpg.party")
local transition = require("fe.transition")

local PRICE = 20

return function(ev)
  dialogue.say("Innkeeper", "A warm bed and a hot meal for " .. PRICE .. " " .. inventory.settings.currency .. ". Stay the night?")
  if dialogue.choice({ "Rest", "Not now" }) ~= 1 then return end
  if not inventory.spend(PRICE) then
    dialogue.say("Innkeeper", "I'm afraid that's not enough coin.")
    return
  end
  dialogue.close()
  transition.out("fade", 0.6)
  party.recover()
  require("fe.sound").play("heal")
  require("fe.task").wait(0.6)
  transition.into("fade", 0.6)
  dialogue.say("Innkeeper", "Good morning! Safe travels.")
end
