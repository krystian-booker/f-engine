-- A page script that changes maps, then starts a task on the new one.
local pages = require("tests.support.pages")
local task = require("fe.task")

return function()
  require("fe.field").warp("arena", "east", { autosave = false })
  pages.arrived = true
  task.spawn(function()
    while true do
      task.wait(0.05)
      pages.count = pages.count + 1
    end
  end)
end
