local dialogue = require("game.dialogue")
local state = require("game.state")

return function(context)
  dialogue.say(nil, "The drake uncoils, frost on its breath.")
  local outcome = require("game.battle.scene").start("guardian")
  if outcome == "victory" then
    state.set_flag("guardian_defeated")
    if context.entity:valid() then context.entity:destroy() end
    dialogue.say(nil, "The way to the crystal is open.")
  end
end
