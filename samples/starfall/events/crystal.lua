local dialogue = require("fe.dialogue")
local inventory = require("fe.rpg.inventory")
local state = require("fe.state")

return function(ev)
  if not state.flag("guardian_defeated") then
    dialogue.say(nil, "The crystal flickers weakly. Something is feeding on its light.")
    return
  end
  if not state.flag("crystal_restored") then
    state.set_flag("crystal_restored")
    ev.self:clip("glow")
    inventory.add("vale_crystal")
    require("fe.sound").play("chime")
    dialogue.say(nil, "Warm light pours from the crystal. You take a shard to show the elder.")
  else
    dialogue.say(nil, "The crystal glows steadily.")
  end
end
