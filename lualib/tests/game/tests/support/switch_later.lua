-- A parallel page script that turns its own page off after a wait and
-- would carry on afterwards.
local pages = require("tests.support.pages")
local task = require("fe.task")

return function()
  pages.runs = pages.runs + 1
  task.wait(0.1)
  require("fe.state").set_flag("off_later")
  task.wait(1)
  pages.late = true
end
