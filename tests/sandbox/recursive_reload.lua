-- A module whose __reload reloads it again (tests/recursion.lua).
local M = { reloads = 0 }
function M.__reload(live)
  live.reloads = live.reloads + 1
  fe.harness.reload("recursive_reload")
end
return M
