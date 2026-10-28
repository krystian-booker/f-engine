-- Elder Rowan starts the main quest and rewards its end.
local dialogue = require("fe.dialogue")
local inventory = require("fe.rpg.inventory")
local quest = require("fe.quest")
local state = require("fe.state")

return function(ev)
  local say = function(text) dialogue.say("Elder Rowan", text, { portrait = "portraits/elder.png" }) end
  if quest.status("fading_crystal") == "completed" then
    say("The Vale shines again thanks to you, {leader}.")
    return
  end
  if state.flag("crystal_restored") then
    say("You've done it! The crystal's warmth reaches even here.")
    if not state.flag("elder_reward") then
      state.set_flag("elder_reward")
      inventory.add_money(400)
      dialogue.say(nil, "Received {color:highlight}400 " .. inventory.settings.currency .. "{color:reset}.")
      quest.complete("fading_crystal")
    end
    return
  end
  if quest.stage("fading_crystal").id == "grotto" then
    say("The grotto lies north of the plaza. Mind the guardian.")
    return
  end
  say("The Vale Crystal is dimming, {leader}.{wait:0.4} Without it, winter will never leave.")
  local answer = dialogue.choice({ "We'll find out why.", "Let us prepare first." })
  if answer == 1 then
    quest.advance("fading_crystal")
    say("Bless you. Take supplies from the merchant, and rest at the inn if you need.")
  else
    say("Of course. Return when you're ready.")
  end
end
