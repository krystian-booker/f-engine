--- Starfall: a small turn-based RPG in the HD-2D style, and the engine's one
--- sample. The engine provides worlds, sprites, input, audio, text and saves;
--- every rule of the game is in this folder:
---
---   game/task.lua, scenes.lua   coroutines and the scene stack
---   game/input.lua, ui.lua      actions, button prompts, windows and lists
---   game/data.lua, state.lua    data tables and the save file
---   game/field.lua              exploration: placements, doors, vehicles, encounters
---   game/dialogue.lua           message boxes and choices
---   game/battle/rules.lua       the battle system (no drawing)
---   game/battle/scene.lua       the battle on screen
---   game/menu.lua, shop.lua, title.lua
---   game/editor.lua             what the F1 editor knows about this game
---   events/*.lua                what the people and things on the maps do
local data = require("game.data")
local editor = require("game.editor")
local input = require("game.input")
local scenes = require("game.scenes")
local task = require("game.task")
local ui = require("game.ui")

local main = { editor = editor }

function main.init()
  local settings = fe.save.read_config()
  input.setup(settings.bindings)
  for bus, volume in pairs(settings.volume or {}) do fe.audio.set_bus_volume(bus, volume) end
  data.reload()
  require("game.title").open()
end

function main.update(dt)
  task.update(dt)
  scenes.update(dt)
  ui.update(dt)
end

function main.post_update(dt)
  scenes.post_update(dt)
end

function main.draw(alpha)
  scenes.draw(alpha)
  ui.draw_toasts()
end

function main.file_changed(path)
  editor.file_changed(path)
end

--- `fengine_headless --check`: the data tables must be consistent.
function main.validate()
  data.validate()
end

return main
