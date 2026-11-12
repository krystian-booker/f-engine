local dialogue = require("game.dialogue")

return function()
  dialogue.say("Merchant", "Potions, ethers, a tent for the road. Have a look.")
  require("game.shop").open("merchant")
end
