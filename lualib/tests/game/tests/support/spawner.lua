-- A page script that starts a task outliving it, then returns.
local pages = require("tests.support.pages")
local task = require("fe.task")

return function()
  task.spawn(function()
    while true do
      task.wait(0.05)
      pages.count = pages.count + 1
    end
  end)
end
