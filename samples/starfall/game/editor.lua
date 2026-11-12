--- The hooks the F1 editor calls (main.lua returns this table as `editor`).
--- All are optional; without them the editor still opens, edits and saves
--- maps. With them it knows this game's placement types, can start the game
--- on any map, and shows changes to placements at once.
local data = require("game.data")
local field = require("game.field")
local scenes = require("game.scenes")
local state = require("game.state")
local task = require("game.task")

local editor = { launchers = {}, order = {} }

local function fields(spec)
  return { fields = spec }
end

local sprite = { type = "asset", asset = "sprite", optional = true }
local model = { type = "asset", asset = "model", optional = true }
-- A field typed "module" is a script to the editor: it badges the object in
-- the Scene, lists it in the Scripts panel, and its "New script" button writes
-- this template to events/<id>.lua.
local script = { type = "module", folder = "events", optional = true, template = [[
local dialogue = require("game.dialogue")

-- Runs as an event: the player waits until it returns.
-- context = { entity, zone, player, world, field }
return function(context)
  dialogue.say(nil, "...")
end
]] }

--- Forms for placements, zones, map properties and data tables, as JSON text.
function editor.describe()
  return fe.json.encode {
    entities = {
      prop = fields { model = model, sprite = sprite, solid = { type = "boolean", default = true } },
      npc = fields { name = { type = "string", optional = true }, sprite = sprite, model = model, script = script,
        wander = { type = "boolean", default = false }, solid = { type = "boolean", default = false } },
      chest = fields { item = { type = "ref", table = "items" }, count = { type = "integer", default = 1 }, sprite = sprite },
      sign = fields { text = { type = "string" } },
      save_point = fields { sprite = sprite },
      door = fields { map = { type = "map_id" }, point = { type = "string" }, name = { type = "string", optional = true },
        model = model, solid = { type = "boolean", default = false } },
      boss = fields { name = { type = "string" }, model = model, script = script,
        hide_flag = { type = "string", optional = true }, solid = { type = "boolean", default = false } },
      symbol = fields { troop = { type = "ref", table = "troops" }, sprite = sprite },
      vehicle = fields { kind = { type = "enum", options = { "strider", "skiff" } }, sprite = sprite },
      emitter = fields {},
      sound = fields { sound = { type = "asset", asset = "audio" }, radius = { type = "number", default = 6 },
        volume = { type = "number", default = 1 } },
    },
    zones = {
      camera = fields { distance = { type = "number", optional = true }, pitch = { type = "number", optional = true } },
      event = fields { script = script, on = { type = "enum", options = { "enter", "exit" }, default = "enter" },
        once = { type = "boolean", default = false } },
    },
    schemas = {
      map_props = fields { music = { type = "asset", asset = "audio", optional = true },
        battle_stage = { type = "map_id", optional = true }, overworld = { type = "boolean", default = false } },
      item = fields { id = { type = "string" }, name = { type = "string" }, price = { type = "integer", default = 0 },
        about = { type = "string", optional = true } },
    },
    tables = {
      { name = "items", file = "data/items.json", schema = "item" },
      { name = "heroes", file = "data/heroes.json" },
      { name = "echoes", file = "data/echoes.json" },
      { name = "skills", file = "data/skills.json" },
      { name = "enemies", file = "data/enemies.json" },
      { name = "troops", file = "data/troops.json" },
      { name = "encounters", file = "data/encounters.json" },
      { name = "shops", file = "data/shops.json" },
      { name = "statuses", file = "data/statuses.json" },
    },
    -- New placements, new zones, and what a model dropped into the Scene becomes.
    defaults = { entities = "npc", zones = "event", models = "prop" },
  }
end

--- "Play from here": start (or continue) the game on `map` at `point`.
function editor.play_from(map, point)
  task.clear()
  scenes.clear()
  field.locked, field.epoch = 0, field.epoch + 1
  if not state.data then state.new_game() end
  field.start(map, point)
end

--- A placement was added, removed or edited: respawn its entity.
function editor.placement_changed(world_name, id)
  if not field.world or field.world.name ~= world_name then return end
  local placement = field.world:placement(id)
  if placement then
    field.spawn_placement(field.world, placement)
  else
    local entity = field.world:find(id)
    if entity then entity:destroy() end
  end
end

local function entity_state(world, id)
  local entity = world:find(id)
  if not entity then
    local zone = field.zones[id]
    if zone and zone.type == "event" and zone.props.once then
      return { state = state.flag("zone:" .. world.map_id .. "/" .. id) and "done" or "armed" }
    end
    return zone and {} or { state = "not spawned" }
  end
  local d = entity:data()
  local info = { state = "spawned", type = d.type }
  if d.label then info.action = d.label end
  if d.wander then info.state = entity:moving() and "wandering" or "waiting" end
  if d.type == "chest" then
    info.state = state.data.opened[world.map_id .. "/" .. id] and "opened" or "closed"
  end
  return info
end

--- What the game knows of one placement or zone right now, as JSON text of
--- an object (the Inspector shows it as it is; the Scripts panel its `state`).
function editor.inspect_entity(id, world_name)
  if not field.world or field.world.name ~= world_name or not state.data then return "{}" end
  return fe.json.encode(entity_state(field.world, id))
end

--- The same for every placement and zone of the map: {id = info}.
function editor.inspect_entities(world_name)
  if not field.world or field.world.name ~= world_name or not state.data then return "{}" end
  local out = {}
  for _, placement in ipairs(field.world:placements()) do out[placement.id] = entity_state(field.world, placement.id) end
  for id in pairs(field.zones) do out[id] = entity_state(field.world, id) end
  return fe.json.encode(out)
end

--- Running tasks for the Scripts panel, as JSON text.
function editor.tasks()
  local out = {}
  for index, entry in ipairs(task.list) do
    out[#out + 1] = { index = index, name = entry.name, status = "running", age = entry.age }
  end
  return fe.json.encode(out)
end

function editor.cancel_task(index)
  task.cancel(task.list[index])
end

function editor.task_count()
  return task.count()
end

--- Check a data table before the Data tab saves it.
function editor.validate_table(name, records)
  local seen = {}
  for index, record in ipairs(records) do
    if type(record.id) ~= "string" or record.id == "" then return false, "record " .. index .. " needs a string id" end
    if seen[record.id] then return false, "duplicate id '" .. record.id .. "' in " .. name end
    seen[record.id] = true
  end
  return true
end

--- The Launch tab: jump straight into a situation.
local function launcher(name, fn)
  editor.order[#editor.order + 1] = name
  editor.launchers[name] = fn
end

local function in_game(fn)
  return function()
    if not state.data then state.new_game() end
    if not field.world then field.start("town", "entry") end
    field.event(fn, "launch")
  end
end

for _, troop in ipairs({ "slimes", "embers", "golem", "guardian" }) do
  launcher("Battle: " .. troop, in_game(function() require("game.battle.scene").start(troop) end))
end
launcher("Own both vehicles", in_game(function()
  state.set_flag("strider"); state.set_flag("skiff")
  field.grant_vehicle("strider"); field.grant_vehicle("skiff")
  field.warp("overworld", "town")
end))

function editor.list()
  return editor.order
end

function editor.launch(name)
  local fn = editor.launchers[name]
  if not fn then error("no launcher '" .. tostring(name) .. "'") end
  fn()
end

--- Reload data tables when their files change (hot reload).
function editor.file_changed(path)
  local name = path:match("^data/([%w_]+)%.json$")
  if name and data.tables[name] then
    local ok, err = pcall(data.load, name)
    if not ok then fe.log.error(err) end
  end
end

return editor
