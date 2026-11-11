local dialogue = require("game.dialogue")
local state = require("game.state")

local lines = {
  "Out on the coast, watch the sensor in the corner. When it turns red, something is about to jump you.",
  "Wolves fear the cold, slimes fear fire. What a creature fears, you only learn by trying.",
  "They say an echo grows the longer you wear it.",
}

return function()
  local index = state.data.steps % #lines + 1
  dialogue.say("Villager", lines[index])
end
