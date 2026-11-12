local dialogue = require("game.dialogue")
local state = require("game.state")

return function(context)
  local portrait = { portrait = "portraits/elder.png" }
  if state.flag("guardian_defeated") and not state.flag("skiff") then
    dialogue.say("Elder Rowan", "The crystal burns again. You have done what none of us could.", portrait)
    dialogue.say("Elder Rowan", "My old skiff is moored south of the gate. Take her: there is a shrine on the "
      .. "islet to the east that no road reaches.", portrait)
    state.set_flag("skiff")
    context.field.grant_vehicle("skiff")
    require("game.ui").toast("Received the skiff.")
  elseif state.flag("skiff") then
    dialogue.say("Elder Rowan", "The skiff lands only on open ground. Mind the forest.", portrait)
  elseif state.flag("met_elder") then
    dialogue.say("Elder Rowan", "The grotto lies east, across the river. Ask the stablehand for a strider: "
      .. "they ford water we cannot.", portrait)
  else
    dialogue.say("Elder Rowan", "The crystal in the grotto has gone dark, and a drake has made its nest "
      .. "before it.", portrait)
    dialogue.say("Elder Rowan", "Strike what a foe is weak to and its guard will break. Break them all at once "
      .. "and your echoes will answer together.", portrait)
    state.set_flag("met_elder")
  end
end
