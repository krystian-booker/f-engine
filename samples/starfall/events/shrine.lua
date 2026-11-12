local dialogue = require("game.dialogue")
local state = require("game.state")

return function()
  if state.grant_echo("tidecaller") then
    require("game.audio").sfx("chime")
    dialogue.say(nil, "Salt wind, and a voice like a tide going out.")
    dialogue.say(nil, "Received the echo Tidecaller.")
    dialogue.say(nil, "That is the end of this sample. Everything you played is Lua in samples/starfall.")
    state.set_flag("finished")
  else
    dialogue.say(nil, "The shrine is quiet.")
  end
end
