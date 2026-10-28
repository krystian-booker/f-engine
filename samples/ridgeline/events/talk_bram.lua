local dialogue = require("fe.dialogue")
local state = require("fe.state")

return function(ev)
  dialogue.say("Bram", "I scouted the ridge at dusk. Two archers on the high terraces, east and west.", { portrait = "portraits/bram.png" })
  dialogue.say("Bram", "Anyone standing still below them is a target.", { portrait = "portraits/bram.png" })
  state.set_flag("info_archers")
end
