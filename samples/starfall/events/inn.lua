local audio = require("game.audio")
local dialogue = require("game.dialogue")
local state = require("game.state")
local task = require("game.task")

return function()
  local price = 20
  if dialogue.choose("Innkeeper", "A bed for the night is " .. price .. " gil.", { "Stay", "Not now" }, { cancel = 2 }) ~= 1 then
    return
  end
  if state.data.gold < price then
    dialogue.say("Innkeeper", "Come back when your purse is heavier.")
    return
  end
  state.data.gold = state.data.gold - price
  task.over(0.5, function(t) fe.screen.set { fade = { 0, 0, 0, t } } end)
  state.heal_party()
  audio.sfx("heal")
  task.wait(0.6)
  task.over(0.5, function(t) fe.screen.set { fade = { 0, 0, 0, 1 - t } } end)
  dialogue.say("Innkeeper", "Rested? Safe roads.")
end
