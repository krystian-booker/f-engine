-- The path up to the ridge: hold the council, then fight. The ending
-- depends on the route the council chose.
local council = require("council")
local dialogue = require("fe.dialogue")
local field = require("fe.field")
local party = require("fe.rpg.party")
local ridge_battle = require("ridge_battle")
local sound = require("fe.sound")
local state = require("fe.state")

local endings = {
  hold = "The raiders broke against the ridge. The village sleeps safely tonight.",
  flank = "With Captain Vey fallen, the raiders scattered into the hills.",
}

return function(ev)
  if state.flag("ridge_won") then
    dialogue.say("Cora", "The ridge is quiet now.", { portrait = "portraits/cora.png" })
    return
  end
  if not state.flag("council_done") then
    sound.music("council")
    council.run()
  end
  if dialogue.choice({ "Head up the ridge", "Not yet" }) ~= 1 then
    sound.music(field.world:props().music or "camp")
    return
  end
  dialogue.close()
  local outcome = ridge_battle.fight(state.var("route", "hold"))
  party.recover()
  sound.music(field.world:props().music or "camp")
  if outcome == "victory" then
    dialogue.say("Cora", endings[state.var("route", "hold")], { portrait = "portraits/cora.png" })
  else
    dialogue.say("Cora", "We fall back and regroup. We can try again.", { portrait = "portraits/cora.png" })
  end
end
