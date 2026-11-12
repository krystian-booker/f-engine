local dialogue = require("game.dialogue")
local state = require("game.state")

return function(context)
  if state.flag("strider") then
    dialogue.say("Stablehand", "She will wait wherever you leave her. Nothing out there dares attack a strider.")
  elseif not state.flag("met_elder") then
    dialogue.say("Stablehand", "The striders are spoken for today. Have a word with the elder.")
  else
    dialogue.say("Stablehand", "The elder sent you? Then take my best bird. She is saddled outside the south gate.")
    state.set_flag("strider")
    context.field.grant_vehicle("strider")
    require("game.ui").toast("A strider waits outside the gate.")
  end
end
