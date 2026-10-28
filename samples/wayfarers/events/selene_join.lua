-- Selene joins: a second protagonist with her own chapter.
local cutscene = require("fe.cutscene")
local dialogue = require("fe.dialogue")
local party = require("fe.rpg.party")
local quest = require("fe.quest")
local state = require("fe.state")

return function(ev)
  local selene = cutscene.actor(ev.self)
  selene:face(ev.player)
  selene:emote("idea", 0.6)
  dialogue.say("Selene", "A hunter heading for the cliffs? My research leads to the same shrine.", { portrait = "portraits/selene.png" })
  if dialogue.choice({ "Travel together.", "I work alone." }) == 1 then
    dialogue.say("Selene", "Splendid. I'll try not to slow you down.", { portrait = "portraits/selene.png" })
    party.join("selene")
    state.set_flag("selene_joined")
    quest.start("selene_1")
    quest.set_chapter("selene", 1)
  else
    dialogue.say("Selene", "Suit yourself. I'll be here.", { portrait = "portraits/selene.png" })
  end
end
