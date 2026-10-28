local dialogue = require("fe.dialogue")
local quest = require("fe.quest")

return function(ev)
  dialogue.say("Barkeep", "Late, isn't it? The night crowd says the shrine keeper only speaks to those who ask politely.")
  if quest.status("rook_1") == "active" and quest.stage("rook_1").id == "ask" then
    quest.advance("rook_1")
    require("fe.state").set_flag("found_cliffhaven")
  end
end
