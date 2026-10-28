local cutscene = require("fe.cutscene")

return function(ev)
  local child = cutscene.actor(ev.self)
  child:face(ev.player)
  child:emote("music", 0.6)
  child:say("Are you an adventurer? Mum says the road's full of wolves.")
end
