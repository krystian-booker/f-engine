--- The title screen: the name, a star, and one thing to press.
local audio = require("game.audio")
local fx = require("game.fx")
local input = require("game.input")
local scenes = require("game.scenes")
local task = require("game.task")
local ui = require("game.ui")

local title = { scene = { opaque = true, age = 0 } }

function title.open()
  scenes.clear()
  fx.reset()
  title.scene.age, title.scene.leaving = 0, false
  scenes.push(title.scene)
  audio.music("title", 2)
end

function title.scene:update(dt)
  self.age = self.age + dt
  if self.leaving or self.age < 1.2 then return end
  if fe.input.pressed("confirm") then
    self.leaving = true
    audio.sfx("confirm")
    task.spawn(function()
      audio.music(nil, 2)
      fx.fade(1, 1.6)
      scenes.pop(title.scene)
      require("story.prologue").start()
    end, "begin")
  end
end

function title.scene:draw()
  local t = self.age
  fe.canvas.rect(0, 0, 1920, 1080, 0x05060dff)
  -- A violet star, high and alone, and its light on nothing.
  local shimmer = 0.8 + 0.2 * math.sin(t * 2.3) * math.sin(t * 0.7)
  fe.canvas.image("fx/battle/glow.png", 960 - 420, 250 - 420, { w = 840, h = 840, linear = true,
    color = ui.fade(0x5a2fb0ff, 0.5 * shimmer) })
  fe.canvas.image("fx/battle/star.png", 960 - 110, 250 - 110, { w = 220, h = 220, linear = true,
    color = ui.fade(0xcfa8ffff, shimmer) })
  local appear = math.min(1, t / 2)
  fe.canvas.push({ alpha = appear })
  ui.centred("AETHERBOUND", 960, 470, { font = "logo", color = ui.colors.text })
  fe.canvas.rect(660, 640, 600, 2, ui.fade(ui.colors.gold, 0.7))
  ui.centred("P R O L O G U E", 960, 662, { font = "label", color = ui.colors.gold })
  fe.canvas.pop()
  if t > 1.2 and not self.leaving then
    local blink = 0.55 + 0.45 * math.sin(t * 3)
    fe.canvas.push({ alpha = blink })
    ui.centred(input.prompt("confirm") .. "  Begin", 960, 860, { font = "body", color = ui.colors.text })
    fe.canvas.pop()
  end
end

return title
