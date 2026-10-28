-- The signpost at the edge of town opens the travel map.
local dialogue = require("fe.dialogue")
local worldmap = require("fe.worldmap")

return function(ev)
  dialogue.say(nil, "A weathered signpost. Where to?")
  dialogue.close()
  worldmap.open()
end
