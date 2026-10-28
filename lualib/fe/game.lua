--- The standard game loop. A game's main.lua usually ends with:
---
--- ```lua
--- return require("fe.game").main{
---   start = function() scene.push(require("scenes.title")) end,
--- }
--- ```
--- It runs the task scheduler and scene stack, tracks playtime, reloads
--- data files and Lua modules during development, and gives games hooks.
local data = require("fe.data")
local scene = require("fe.scene")
local signal = require("fe.signal")
local state = require("fe.state")
local task = require("fe.task")

local game = {
  --- Emitted every tick with dt after tasks and scenes update.
  updated = signal.new(),
  --- Emitted during draw after scenes; draw overlays here.
  drawn = signal.new(),
  --- Emitted with a game-relative path when a non-Lua file changes.
  file_changed = signal.new(),
  started = false,
  paused = false,
  time_scale = 1,
}

--- Build the entry module table the engine calls.
---@param options {start: fun(), update?: fun(dt: number), draw?: fun(), validate?: fun()}
function game.main(options)
  local entry = {}

  function entry.init()
    game.started = true
    task.named("start", options.start)
  end

  function entry.update(dt)
    if game.paused then
      return
    end
    dt = dt * game.time_scale
    state.add("playtime", dt)
    task.update(dt)
    scene.update(dt)
    if options.update then options.update(dt) end
    game.updated:emit(dt)
  end

  function entry.draw(alpha)
    game.alpha = alpha
    scene.draw()
    if options.draw then options.draw() end
    game.drawn:emit()
  end

  function entry.file_changed(path)
    data.reload(path)
    game.file_changed:emit(path)
  end

  entry.validate = options.validate
  return entry
end

return game
