local dialogue = require("fe.dialogue")

return function(ev)
  ev.self:face(ev.player)
  dialogue.say("Merchant", "Potions, blades and charms! What'll it be?")
  dialogue.close()
  require("fe.ui.screens.shop").open("vale_general")
end
