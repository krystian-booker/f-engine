-- Shown after a defeat: back to the title or the last save.
local scene = require("fe.scene")
local ui = require("fe.ui")

local game_over = {}

function game_over.open()
  local view = { name = "game_over", opaque = true }
  function view:draw()
    local w, h = fe.canvas.size()
    fe.canvas.rect(0, 0, w, h, 0x000000ff)
    ui.text("The light fades...", 0, h / 2 - 20, { width = w, align = "center", color = ui.theme.bad })
  end
  scene.switch(view)
  require("fe.task").wait(1.5)
  local slot = require("fe.ui.screens.save").open("load")
  scene.remove(view)
  if slot then
    require("fe.field").resume()
  else
    fe.app.quit()
  end
end

return game_over
