--- Developer hooks used by the F1 editor: named launchers (jump straight to
--- a battle, a cutscene or a map) and the functions the editor calls.
---
--- ```lua
--- devtools.register("Battle: slimes", function() require("fe.battle").encounter("slimes") end)
--- devtools.register("Warp: grotto", function() require("fe.field").warp("grotto", "entry") end)
--- devtools.defaults.entities = "npc"     -- type of new placements in the map editor
--- ```
--- Editor hooks:
---   * `describe()`: JSON with `schemas`, `entities` (placement types),
---     `zones` (zone types), `tables` and `defaults = {entities, zones}`;
---   * `play_from(map, point)`: "Play from here" — cancels running scripts
---     and menus and starts the field there, keeping the game's state. With
---     no game in progress (the title screen) it first runs
---     `devtools.new_game` (fe.starter sets it to its New Game);
---   * `validate_table(name, records)`: the Data tab's check before saving
---     (`fe.data.validate`); returns true, or false and the problems;
---   * `list()` and `launch(name)`: the Launch tab.
local task = require("fe.task")

local devtools = {
  launchers = {},
  order = {},
  --- Types the editor gives new placements and zones.
  defaults = { entities = "npc", zones = "camera" },
  --- Prepares a new game for `play_from` when none is running (fe.starter
  --- sets it).
  new_game = nil,
}

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

--- JSON text describing schemas, entity and zone types, data tables and
--- the default types for new placements and zones (editor).
function devtools.describe()
  local described = require("fe.schema").export()
  described.tables = require("fe.data").describe()
  described.defaults = devtools.defaults
  return fe.json.encode(described)
end

--- Start playing on `map` at `point` (the editor's "Play from here").
--- Returns the task doing it.
---@param map string
---@param point string
---@return Task
function devtools.play_from(map, point)
  return task.detached("play_from", function()
    local field = require("fe.field")
    local scene = require("fe.scene")
    task.cancel_all()
    field.reset()
    require("fe.game").reset_time()
    local in_game = field.world ~= nil and scene.contains(field.scene)
    if not in_game and devtools.new_game then devtools.new_game() end
    scene.clear()
    field.start(map, point)
  end)
end

--- Check a data table's records before the editor saves them; returns true,
--- or false and every problem, one per line.
---@param name string
---@param records table
---@return boolean, string?
function devtools.validate_table(name, records)
  local ok, errors = require("fe.data").validate(name, records)
  if ok then return true end
  return false, table.concat(errors, "\n")
end

return devtools
