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

--- The editor changed (added, deleted, retyped or edited the props of) a
--- placement of a loaded world: respawn that one entity from the live map
--- so the change shows at once (fe.field.respawn_placement).
---@param world_name string
---@param id string
function devtools.placement_changed(world_name, id)
  local world = fe.world.get(world_name)
  if not world then return end
  local field = require("fe.field")
  if field.world ~= world then return end -- a stage or another world: nothing of the field's to respawn
  if field.respawn_placement then field.respawn_placement(world, id) end
end

--- Running tasks as JSON text for the editor's Scripts panel, oldest first,
--- each with an `index` that `cancel_task` takes.
---@return string
function devtools.tasks()
  local out = {}
  for i, entry in ipairs(task.list()) do
    out[#out + 1] = { index = i, name = entry.name, status = entry.status, parent = entry.parent,
      world = entry.world or nil, age = entry.age, children = entry.children, helper = entry.helper or false }
  end
  return fe.json.encode(out)
end

--- Cancel the task `devtools.tasks()` listed at `index`.
---@param index integer
function devtools.cancel_task(index)
  local entry = task.list()[index]
  if entry then entry.task:cancel() end
end

local function entity_info(world, id)
  local entity = world:find(id)
  if not entity or not entity:valid() then return nil end
  local event = require("fe.event")
  local d = entity:data()
  local info = { id = id, spawned = true, pages = {}, running = nil, interact = d.interact ~= nil,
    on_touch = d.on_touch ~= nil, routine = d.routine, auto_done = d.auto_done or false }
  if d.pages then
    local active = event.active_index(entity, d)
    for i, page in ipairs(d.pages) do
      info.pages[#info.pages + 1] = { index = i, script = page.script, trigger = page.trigger or "action",
        when = page.when, active = i == active, error = d.page_errors and d.page_errors[i] or nil }
    end
  end
  local job = event.running(entity)
  if job then info.running = { name = job.name, status = job.status } end
  if d.parallel and d.parallel.status == "running" then
    info.parallel = { name = d.parallel.name, status = d.parallel.status }
  end
  return info
end

--- What the field knows about a placement's entity (its pages, which one is
--- active, whether a script of its is running, its routine), as JSON text;
--- "null" when it has no entity. `world_name` defaults to the field's world.
---@param id string
---@param world_name? string
---@return string
function devtools.inspect_entity(id, world_name)
  local world = world_name and fe.world.get(world_name) or require("fe.field").world
  if not world then return "null" end
  return fe.json.encode(entity_info(world, id))
end

--- The same for every placement of a world: JSON text of {id = info}.
---@param world_name? string
---@return string
function devtools.inspect_entities(world_name)
  local world = world_name and fe.world.get(world_name) or require("fe.field").world
  if not world then return "{}" end
  local out = {}
  for _, p in ipairs(world:placements()) do
    local info = entity_info(world, p.id)
    if info then out[p.id] = info end
  end
  return fe.json.encode(out)
end

--- Run one of a placement entity's pages now (the editor's "Run page").
---@param id string
---@param index integer
function devtools.run_page(id, index)
  local field = require("fe.field")
  local world = field.world
  local entity = world and world:find(id)
  if not entity or not entity:valid() then error("no entity '" .. tostring(id) .. "'") end
  local d = entity:data()
  local page = d.pages and d.pages[index]
  if not page then error("no page " .. tostring(index)) end
  require("fe.event").run(entity, page, { player = field.player })
end

--- The last world events the field handled (newest last), JSON text; kept
--- by fe.field.hooks for the editor's Scripts panel.
---@return string
function devtools.recent_events()
  local hooks = require("fe.field.hooks")
  return fe.json.encode(hooks.recent_events or {})
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
