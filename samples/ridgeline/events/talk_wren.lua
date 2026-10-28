local dialogue = require("fe.dialogue")
local state = require("fe.state")

return function(ev)
  dialogue.say("Wren", "The stream splits the ridge in two. Freeze it and you could walk straight across.", { portrait = "portraits/wren.png" })
  if state.flag("info_stream") then return end
  dialogue.say("Wren", "Lio's frost would do it. Lightning in that water would hurt, though.", { portrait = "portraits/wren.png" })
  state.set_flag("info_stream")
end
