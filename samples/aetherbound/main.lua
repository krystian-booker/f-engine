--- Aetherbound: Prologue. A ten-minute vertical slice in the HD-2D style:
--- the last battle of a war, and what it costs.
---
---   game/task.lua, scenes.lua    coroutines and the scene stack
---   game/input.lua, ui.lua       actions, button prompts, panels and gauges
---   game/audio.lua, fx.lua       music and sounds by id; fades, shakes, camera moves
---   game/dialogue.lua            speech boxes, the machine's voice, barks
---   game/actors.lua, director.lua  characters in a world and how scenes stage them
---   game/field.lua               walking a map with the party in tow
---   game/battle/rules.lua        the active-time battle system (no drawing)
---   game/battle/scene.lua        the battle on screen
---   game/vfx.lua                 short-lived effects
---   story/*.lua                  the five scenes
local actors = require("game.actors")
local dialogue = require("game.dialogue")
local editor = require("game.editor")
local fx = require("game.fx")
local input = require("game.input")
local party = require("game.party")
local scenes = require("game.scenes")
local task = require("game.task")
local ui = require("game.ui")

local main = { editor = editor }

function main.init()
  local settings = fe.save.read_config()
  input.setup(settings.bindings)
  ui.setup()
  party.reset()
  require("game.title").open()
end

function main.update(dt)
  task.update(dt)
  actors.update(dt)
  fx.update(dt)
  dialogue.update(dt)
  ui.update(dt)
  require("story.fortress").update(dt)
  require("story.chamber").update(dt)
  scenes.update(dt)
end

function main.post_update(dt)
  scenes.post_update(dt)
end

function main.draw(alpha)
  scenes.draw(alpha)
  require("story.chamber").draw()
  require("story.last_light").draw()
  ui.draw_caption()
  dialogue.draw()
  if require("story.fortress").prompt then
    ui.hints({ { "up", "Move" }, { "run", "Run" } }, math.min(1, require("story.fortress").prompt))
  end
end

return main
