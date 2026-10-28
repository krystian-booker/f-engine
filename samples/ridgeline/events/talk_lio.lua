local dialogue = require("fe.dialogue")
local state = require("fe.state")

return function(ev)
  dialogue.say("Lio", "It hasn't rained in weeks. The grass under the ridge would go up like tinder.", { portrait = "portraits/lio.png" })
  state.set_flag("info_grass")
end
