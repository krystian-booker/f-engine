--- The prologue from start to finish: five scenes, one task.
---
--- `prologue.start(from)` begins it (from the top, or from a later scene
--- for testing: "chamber", "remains"); `prologue.stop()` drops it.
local actors = require("game.actors")
local audio = require("game.audio")
local dialogue = require("game.dialogue")
local field = require("game.field")
local fx = require("game.fx")
local party = require("game.party")
local scenes = require("game.scenes")
local task = require("game.task")
local ui = require("game.ui")

local prologue = { running = nil, scene = nil, ended = false }

local function end_card()
  local card = { age = 0 }
  function card:update(dt) self.age = self.age + dt end
  function card:draw()
    fe.canvas.rect(0, 0, 1920, 1080, 0x000000ff)
    local a = math.min(1, self.age / 2.5)
    fe.canvas.push({ alpha = a })
    ui.centred("AETHERBOUND", 960, 430, { font = "card", color = ui.colors.text })
    ui.centred("End of the prologue", 960, 580, { font = "label", color = ui.colors.dim })
    fe.canvas.pop()
  end
  scenes.push(card)
  task.wait(3)
  local waited = 0
  task.wait_until(function()
    waited = waited + 1 / 60
    return fe.input.pressed("confirm") or waited > 12
  end)
  scenes.pop(card)
end

function prologue.start(from)
  prologue.stop()
  party.reset()
  prologue.ended = false
  prologue.running = task.spawn(function()
    local order = { "fortress", "chamber", "last_light", "remains" }
    local skip = from ~= nil
    local stage
    for _, name in ipairs(order) do
      if name == from or (from == "last_light" and name == "chamber") then skip = false end
      if not skip then
        prologue.scene = name
        local result = require("story." .. name).run(stage)
        if result then stage = result end
      end
    end
    prologue.scene = "end"
    field.leave()
    fx.reset()
    fx.fade(0, 0)
    end_card()
    prologue.scene, prologue.ended = nil, true
    require("game.title").open()
  end, "prologue")
end

function prologue.stop()
  if prologue.running then task.cancel(prologue.running) end
  prologue.running, prologue.scene = nil, nil
  task.clear()
  dialogue.clear()
  scenes.clear()
  actors.clear()
  fx.world_changed()
  for _, name in ipairs(fe.world.list()) do fe.world.unload(name) end
  field.world, field.actors, field.on_zone, field.locked = nil, {}, {}, 0
  audio.stop_all(0.5)
  fx.reset()
end

return prologue
