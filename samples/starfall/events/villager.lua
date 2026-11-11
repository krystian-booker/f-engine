local cutscene = require("fe.cutscene")
local state = require("fe.state")

return function(ev)
  local me = cutscene.actor(ev.self)
  me:face(ev.player)
  if state.flag("crystal_restored") then
    me:emote("music")
    me:say("Can you feel it? Spring is coming!")
  else
    me:emote("sweat")
    me:say("Brr. It's been cold since the crystal dimmed.")
  end
end
