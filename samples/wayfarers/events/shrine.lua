-- The shrine: both protagonists' first chapters end here.
local dialogue = require("fe.dialogue")
local party = require("fe.rpg.party")
local quest = require("fe.quest")

return function(ev)
  if quest.status("rook_1") ~= "active" then
    dialogue.say("Shrine Keeper", "The winds are calm today.")
    return
  end
  dialogue.say("Shrine Keeper", "So the hunter found the trail. The marks you follow were carved by your own teacher.")
  quest.complete("rook_1")
  quest.set_chapter("rook", 2)
  if party.is_active("selene") then
    dialogue.say("Selene", "Then the pages I carry... they're the rest of the map!", { portrait = "portraits/selene.png" })
    quest.complete("selene_1")
    quest.set_chapter("selene", 2)
  end
  dialogue.say(nil, "{color:heading}Chapter 1 complete.{color:reset}")
end
