local dialogue = require("game.dialogue")
local state = require("game.state")

return function()
  if state.grant_echo("emberheart") then
    require("game.audio").sfx("chime")
    dialogue.say(nil, "The crystal flares. Its warmth settles in you.")
    dialogue.say(nil, "Received the echo Emberheart. Equip it from the menu.")
  else
    dialogue.say(nil, "The crystal hums quietly. The elder will want to hear of this.")
  end
end
