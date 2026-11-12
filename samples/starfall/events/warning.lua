local dialogue = require("game.dialogue")

-- The grotto's `drake_warning` zone runs this once, as the party nears the dais.
return function()
  dialogue.say(nil, "The air turns cold. Something large shifts in the dark ahead.")
end
