-- A parallel page script that turns its own page off at once.
local pages = require("tests.support.pages")

return function()
  pages.runs = pages.runs + 1
  require("fe.state").set_flag("off_now")
end
