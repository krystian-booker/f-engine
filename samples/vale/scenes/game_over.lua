-- Shown by fe.battle.on_defeat after a lost battle: a fade to black, then
-- the load screen. Loading a save resumes it through fe.starter.continue,
-- which cancels the battle's tasks and resets locks the way Continue on
-- the title does; backing out quits.
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
    require("fe.starter").continue(slot)
  else
    fe.app.quit()
  end
end

return game_over
