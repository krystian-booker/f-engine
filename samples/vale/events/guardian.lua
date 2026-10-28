-- The guardian scene: a short cutscene, then a boss battle.
local battle = require("fe.battle")
local cutscene = require("fe.cutscene")
local field = require("fe.field")
local quest = require("fe.quest")
local state = require("fe.state")

return function(ev)
  local guardian = cutscene.actor(ev.self)
  cutscene.play(function()
    field.rig:pan_to(ev.self.position, 0.8, { distance = 12, pitch = 30 })
    guardian:emote("anger", 0.8)
    guardian:say("...NONE... SHALL TAKE THE LIGHT...")
    cutscene.actor("player"):say("It's draining the crystal! We have to stop it.")
    field.rig:release()
  end)
  local outcome = battle.encounter("guardian", { allow_defeat = false })
  if outcome == "victory" then
    state.set_flag("guardian_defeated")
    quest.advance("fading_crystal", "report")
  end
end
