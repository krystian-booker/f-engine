--- Developer hooks used by the F1 editor: named launchers (jump straight to
--- a battle, a cutscene or a map) and helpers the editor calls.
---
--- ```lua
--- devtools.register("Battle: slimes", function() require("fe.battle").encounter("slimes") end)
--- devtools.register("Warp: grotto", function() require("fe.field").warp("grotto", "entry") end)
--- ```
local task = require("fe.task")

local devtools = { launchers = {}, order = {} }

function devtools.register(name, fn)
  if not devtools.launchers[name] then devtools.order[#devtools.order + 1] = name end
  devtools.launchers[name] = fn
end

function devtools.list()
  return devtools.order
end

--- Run a launcher in a task.
function devtools.launch(name)
  local fn = devtools.launchers[name]
  if not fn then error("no launcher '" .. tostring(name) .. "'") end
  task.named("launch:" .. name, fn)
end

--- JSON text describing schemas, entity types and data tables (editor).
function devtools.describe()
  local described = require("fe.schema").export()
  described.tables = require("fe.data").describe()
  return fe.json.encode(described)
end

return devtools
