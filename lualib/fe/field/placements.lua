--- The field's built-in placement types: npc, chest, door, sign, light,
--- emitter, prop, sound, save_point, symbol and hidden_item. fe.field
--- installs them (`placements.install(field)`); a game replaces one with
--- `field.define(type, spawner, fields)` or this whole module by putting
--- its own `fe/field/placements.lua` in the game folder.
---
--- Each spawner is a module function (`placements.npc(world, placement)`
--- and so on), so a game can wrap one: define its own type that calls
--- `placements.chest` and then changes the entity.
---
--- Chests: `locked = true` keeps a chest shut until its unlock flag
--- (`unlock_flag`, default `unlocked:<map>:<id>`) is set; with `key = <item
--- id>` the chest unlocks itself when the player holds that item (the
--- RPG kit's `item` page condition answers that; without the RPG kit a game
--- registers `event.condition("item", fn)` or sets the flag itself).
---
--- Hidden items (Octopath's searchable spots): an invisible placement the
--- player searches with Confirm; taken once (`flag`, default
--- `hidden:<map>:<id>`), it is not spawned again.
local dialogue = require("fe.dialogue")
local event = require("fe.event")
local i18n = require("fe.i18n")
local sound = require("fe.sound")
local state = require("fe.state")
local task = require("fe.task")

local placements = {}

i18n.defaults({ fe = { field = {
  locked = "It's locked.", unlocked = "Unlocked with {key}.",
} } })

-- fe.field requires this module while it loads, so it is fetched when needed.
local function field()
  return require("fe.field")
end

--- The name of an item for messages: the RPG item table's, else the id.
local function item_name(id)
  local data = require("fe.data")
  if data.has_table("items") then
    local ok, record = pcall(data.get, "items", id)
    if ok and record.name then return record.name end
  end
  return id
end

function placements.npc(world, p)
  local props = p.props
  local entity = world:spawn({
    id = p.id, position = p.position, layer = p.layer, facing = p.facing, tags = { "npc" },
    sprite = { animation = props.sprite, move_clip = "walk", idle_clip = "idle" },
    collider = { radius = props.radius or 0.3, solid = props.solid ~= false },
    mover = { speed = props.speed or 1.6 },
    props = props,
  })
  field().apply_pages(entity, p, { sprite = props.sprite, solid = props.solid })
  local d = entity:data()
  d.routine = props.routine
  d.home = p.position
  d.route = props.route
  return entity
end

--- Whether a locked chest may open: already unlocked, or the player holds
--- its key (through the `item` page condition). Unlocks it when the key
--- fits. Replace for other lock rules.
---@param entity Entity
---@param props table
---@param unlock_flag string
---@return boolean
function placements.chest_unlocks(entity, props, unlock_flag)
  if state.flag(unlock_flag) then return true end
  local has_item = props.key and event.conditions.item
  if has_item and has_item(props.key, { self = entity }) then
    state.set_flag(unlock_flag)
    dialogue.say(nil, props.unlocked_text or i18n.t("fe.field.unlocked", { key = item_name(props.key) }))
    return true
  end
  return false
end

function placements.chest(world, p)
  local props = p.props
  local flag = props.flag or ("chest:" .. world.map_id .. ":" .. p.id)
  local unlock_flag = props.unlock_flag or ("unlocked:" .. world.map_id .. ":" .. p.id)
  local opened = state.flag(flag)
  local entity = world:spawn({
    id = p.id, position = p.position, layer = p.layer, facing = p.facing, tags = { "chest" },
    sprite = { animation = props.sprite or "@fe/assets/ui/spark.anim.json", clip = opened and "open" or "closed" },
    collider = { radius = 0.35 }, props = props,
  })
  entity:data().interact = function()
    if state.flag(flag) then
      dialogue.say(nil, props.empty_text or i18n.t("fe.field.empty"))
      return
    end
    if props.locked and not placements.chest_unlocks(entity, props, unlock_flag) then
      dialogue.say(nil, props.locked_text or i18n.t("fe.field.locked"))
      return
    end
    state.set_flag(flag)
    entity:clip("open")
    sound.play(props.sound or "chest")
    field().give(props.item, props.count or 1, props.money)
  end
  return entity
end

function placements.door(world, p)
  local props = p.props
  local entity = world:spawn({
    id = p.id, position = p.position, layer = p.layer, tags = { "door" },
    collider = { radius = props.radius or 0.45, solid = false },
    sprite = props.sprite and { animation = props.sprite } or nil, props = props,
  })
  local function go()
    task.detached("warp", field().warp, props.map, props.point, { transition = props.transition })
  end
  if (props.trigger or "touch") == "touch" then
    entity:data().on_touch = go
  else
    entity:data().interact = go
  end
  return entity
end

function placements.sign(world, p)
  local entity = world:spawn({ id = p.id, position = p.position, layer = p.layer, collider = { radius = 0.3 },
    sprite = p.props.sprite and { animation = p.props.sprite } or nil, props = p.props })
  entity:data().interact = function() dialogue.say(nil, p.props.text) end
  return entity
end

function placements.light(world, p)
  local light = {}
  for k, v in pairs(p.props) do light[k] = v end
  light.position = { 0, p.props.height or 0, 0 }
  return world:spawn({ id = p.id, position = p.position, layer = p.layer, keep_height = true, light = light })
end

function placements.emitter(world, p)
  return world:spawn({ id = p.id, position = p.position, layer = p.layer, keep_height = true, emitter = p.props })
end

function placements.prop(world, p)
  local props = p.props
  local entity = world:spawn({
    id = p.id, position = p.position, layer = p.layer, yaw = props.yaw, scale = props.scale,
    mesh = { model = props.model, animation = props.animation, loop = props.loop ~= false, playing = props.animation ~= nil },
    collider = props.solid and { radius = props.radius or 0.45 } or nil, props = props,
  })
  -- Pages show and hide the mesh; no sprite is added for a prop.
  field().apply_pages(entity, p, { mesh = true, solid = props.solid })
  return entity
end

function placements.sound(world, p)
  return world:spawn({ id = p.id, position = p.position, layer = p.layer, audio = p.props })
end

function placements.save_point(world, p)
  local entity = world:spawn({ id = p.id, position = p.position, layer = p.layer, collider = { radius = 0.4 },
    sprite = { animation = p.props.sprite or "@fe/assets/ui/spark.anim.json", unlit = true }, props = p.props })
  entity:data().interact = function() field().on_save_point(entity) end
  return entity
end

function placements.symbol(world, p)
  return require("fe.encounter").symbol(world, { troop = p.props.troop, position = p.position, sprite = p.props.sprite,
    sight = p.props.sight, speed = p.props.speed })
end

function placements.hidden_item(world, p)
  local props = p.props
  local flag = props.flag or ("hidden:" .. world.map_id .. ":" .. p.id)
  if state.flag(flag) then return nil end
  local entity = world:spawn({
    id = p.id, position = p.position, layer = p.layer, tags = { "hidden_item" },
    collider = { radius = props.radius or 0.3, solid = false },
    sprite = props.sprite and { animation = props.sprite } or nil, props = props,
  })
  entity:data().interact = function()
    if state.flag(flag) then return end
    state.set_flag(flag)
    sound.play(props.sound or "chest")
    field().give(props.item, props.count or 1, props.money)
    if entity:valid() then entity:destroy() end
  end
  return entity
end

--- The editor's props schema and description of every built-in type.
placements.fields = {
  npc = { {
    name = { type = "string", optional = true, doc = "Speaker name" },
    sprite = { type = "string", doc = "Sprite .anim.json" },
    script = { type = "string", optional = true, doc = "Event module for a single action page" },
    pages = { type = { list = "table" }, optional = true, doc = "Event pages" },
    routine = { type = "string", default = "still", doc = "still, wander, patrol or a field.routine name" },
    route = { type = { list = "table" }, optional = true, doc = "Patrol points [{x, z}]" },
    speed = { type = "number", optional = true },
    solid = { type = "boolean", optional = true },
  }, "A person or creature with event pages." },
  chest = { {
    item = { type = "string", optional = true }, count = { type = "integer", default = 1 },
    money = { type = "integer", optional = true }, sprite = { type = "string", optional = true },
    flag = { type = "string", optional = true, doc = "Defaults to chest:<map>:<id>" },
    empty_text = { type = "string", optional = true },
    locked = { type = "boolean", optional = true, doc = "Shut until its unlock flag is set or the key is held" },
    key = { type = "string", optional = true, doc = "Item that unlocks it (the RPG kit's `item` condition)" },
    unlock_flag = { type = "string", optional = true, doc = "Defaults to unlocked:<map>:<id>" },
    locked_text = { type = "string", optional = true },
  }, "A treasure chest opened once." },
  door = { {
    map = { type = "string" }, point = { type = "string" },
    trigger = { type = { enum = { "touch", "action" } }, default = "touch" },
    transition = { type = "string", optional = true }, sprite = { type = "string", optional = true },
  }, "Moves the player to another map." },
  sign = { { text = { type = "string" }, sprite = { type = "string", optional = true } }, "Shows a message." },
  light = { { color = "table", radius = "number", intensity = "number", flicker = { type = "number", optional = true } },
    "A dynamic light (static lights belong in the map's light list)." },
  emitter = { { sprite = { type = "string", optional = true }, rate = "number" }, "Particles (fire, smoke, sparkles)." },
  prop = { {
    model = { type = "string", doc = ".glb file" }, animation = { type = "string", optional = true },
    yaw = { type = "number", optional = true, doc = "degrees" }, solid = { type = "boolean", optional = true },
    pages = { type = { list = "table" }, optional = true, doc = "Event pages (show and hide the mesh)" },
  }, "A 3D object (doors, windmills, crates)." },
  sound = { { sound = "string", radius = { type = "number", default = 8 } }, "Looping positional sound." },
  save_point = { { sprite = { type = "string", optional = true } }, "Opens the save screen." },
  symbol = { { troop = "string", sprite = "string", sight = { type = "number", default = 4 } }, "A visible enemy." },
  hidden_item = { {
    item = { type = "string", optional = true }, count = { type = "integer", default = 1 },
    money = { type = "integer", optional = true },
    flag = { type = "string", optional = true, doc = "Defaults to hidden:<map>:<id>" },
    sprite = { type = "string", optional = true, doc = "Usually none: the spot is invisible" },
  }, "An item found by searching a spot, once." },
}

--- Attach a placement's event pages (`props.pages`, or a single page from
--- `props.script` and `props.trigger`) to its entity; `defaults` describe
--- the entity without an active page (see fe.event.attach).
---@param entity Entity
---@param placement table
---@param defaults table
function placements.apply_pages(entity, placement, defaults)
  local props = placement.props
  local pages = props.pages
  if not pages and props.script then
    pages = { { script = props.script, trigger = props.trigger or "action" } }
  end
  if pages then
    event.attach(entity, pages, defaults)
  end
end

--- Spawn a world's placements by type (`field.types`) into the field's
--- entity list (`field.entities()`, cleared in place first), skipping ones
--- that are following the party; a spawner that raises is reported and
--- skipped.
---@param world World
function placements.spawn(world)
  local f = field()
  local entities = f.entities()
  for key in pairs(entities) do entities[key] = nil end
  local following = require("fe.follower")
  for _, p in ipairs(world:placements()) do
    local spawner = f.types[p.type]
    if spawner and not following.has(p.id) then
      local ok, entity = pcall(spawner, world, p)
      if not ok then
        fe.log.error("Placement '" .. p.id .. "' (" .. p.type .. "): " .. tostring(entity))
      elseif entity then
        entities[#entities + 1] = entity
      end
    elseif not spawner and p.type ~= "marker" then
      fe.log.warn("No field type '" .. p.type .. "' for placement " .. p.id)
    end
  end
end

--- Register every built-in type with `field.define` (fe.field does this
--- when it loads; a type the game defined already is not replaced) and
--- give `field` the `spawn_placements` and `apply_pages` functions it does
--- not have yet (or had from this module).
---@param field table the fe.field module
function placements.install(field)
  for name, spec in pairs(placements.fields) do
    if not field.types[name] or field.types[name] == placements.installed[name] then
      field.define(name, placements[name], spec[1], spec[2])
      placements.installed[name] = placements[name]
    end
  end
  local functions = { spawn_placements = placements.spawn, apply_pages = placements.apply_pages }
  for name, fn in pairs(functions) do
    if field[name] == nil or field[name] == placements.installed[name] then
      field[name] = fn
      placements.installed[name] = fn
    end
  end
end

--- The spawners this module installed, by type, and its functions on
--- fe.field, by name (so a reload replaces its own registrations and
--- leaves a game's alone).
placements.installed = {}

--- Register the fresh spawners after a hot reload of this file.
function placements.__reload(live)
  live.install(require("fe.field"))
end

return placements
