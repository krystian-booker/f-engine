-- The tavern by day: rest and swap party members.
local dialogue = require("fe.dialogue")
local inventory = require("fe.rpg.inventory")
local party = require("fe.rpg.party")
local transition = require("fe.transition")

return function(ev)
  dialogue.say("Barkeep", "Welcome to the Driftwood! A room is 30 " .. inventory.settings.currency .. ".")
  local answer = dialogue.choice({ "Rest", "Change party", "Leave" })
  if answer == 1 and inventory.spend(30) then
    dialogue.close()
    transition.out("fade", 0.5)
    party.recover()
    require("fe.daynight").set_hour(8)
    transition.into("fade", 0.5)
    dialogue.say("Barkeep", "Morning already! Safe roads.")
  elseif answer == 2 then
    dialogue.close()
    require("fe.ui.screens.menu").pages[5].run()
  end
end
