--- Data-driven timelines: lists of timed commands, played as a task.
---
--- ```json
--- [{"time": 0, "command": "step", "distance": 0.7},
---  {"time": 0.3, "command": "flash", "color": [1, 0.8, 0.6, 0.5]},
---  {"time": 0.3, "command": "effect"}]
--- ```
--- Command handlers are plain functions(context, params) registered by the
--- system that plays the timeline (side-view battles register step, anim,
--- effect, return...). Handlers may wait; later commands start on time.
local task = require("fe.task")

local timeline = {}

--- Play a timeline. `handlers[command](context, params)` runs each entry.
---@param entries table[]
---@param handlers table<string, fun(context: any, params: table)>
---@param context any
function timeline.play(entries, handlers, context)
  local sorted = {}
  for i, entry in ipairs(entries) do sorted[i] = entry end
  table.sort(sorted, function(a, b) return (a.time or 0) < (b.time or 0) end)
  local start = task.time()
  local running = {}
  for _, entry in ipairs(sorted) do
    local delay = (entry.time or 0) - (task.time() - start)
    if delay > 0 then task.wait(delay) end
    local handler = handlers[entry.command]
    if not handler then
      error("timeline command '" .. tostring(entry.command) .. "' has no handler", 2)
    end
    running[#running + 1] = task.spawn(handler, context, entry)
  end
  task.wait_until(function()
    for _, t in ipairs(running) do
      if t.status == "running" then return false end
    end
    return true
  end)
end

return timeline
