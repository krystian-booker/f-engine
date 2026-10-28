local dialogue = require("fe.dialogue")

return function(ev)
  dialogue.say("Ferryman", "No boats today, the river's too high. Ask me anything, though. {color:muted}(Info: talk options){color:reset}")
end
