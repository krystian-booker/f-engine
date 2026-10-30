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
    id = p.id, position = p.position, layer = p.layer,
    -- The placement's own yaw and scale (the editor's gizmo); `props.yaw`
    -- and `props.scale` are older maps' way of saying the same thing.
    yaw = (p.yaw ~= 0 and p.yaw) or props.yaw, scale = (not placements.unit_scale(p.scale) and p.scale) or props.scale,
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
--- An event page as a placement's `pages` list holds it (fe.event): the
--- editor shows these fields; a page may carry more.
placements.page_schema = { record = {
  script = { type = { module = "events" }, doc = "Event module run when the page triggers" },
  trigger = { type = { enum = { "action", "touch", "auto", "parallel" } }, default = "action",
    doc = "action: Confirm facing it; touch: walk into it; auto: once when active; parallel: every tick" },
  when = { type = "table", optional = true, doc = "Conditions: {flag = ..., not_flag = ..., var = {...}, check = ...}" },
  sprite = { type = { asset = "sprite" }, optional = true, doc = "Look while this page is active" },
  clip = { type = "string", optional = true },
  solid = { type = "boolean", optional = true },
  hidden = { type = "boolean", optional = true },
} }

--- The editor's props schema and description of every built-in type.
placements.fields = {
  npc = { {
    name = { type = "string", optional = true, doc = "Speaker name" },
    sprite = { type = { asset = "sprite" }, doc = "Sprite .anim.json" },
    script = { type = { module = "events" }, optional = true, doc = "Event module for a single action page" },
    trigger = { type = { enum = { "action", "touch", "auto", "parallel" } }, optional = true,
      doc = "How `script` fires (default action)" },
    pages = { type = { list = placements.page_schema }, optional = true, doc = "Event pages" },
    routine = { type = "string", default = "still", doc = "still, wander, patrol or a field.routine name" },
    route = { type = { list = { record = { x = "number", z = "number" } } }, optional = true, doc = "Patrol points" },
    speed = { type = "number", optional = true },
    solid = { type = "boolean", optional = true },
  }, "A person or creature with event pages." },
  chest = { {
    item = { type = { ref = "items" }, optional = true }, count = { type = "integer", default = 1 },
    money = { type = "integer", optional = true }, sprite = { type = { asset = "sprite" }, optional = true },
    flag = { type = "string", optional = true, doc = "Defaults to chest:<map>:<id>" },
    empty_text = { type = "string", optional = true },
    locked = { type = "boolean", optional = true, doc = "Shut until its unlock flag is set or the key is held" },
    key = { type = { ref = "items" }, optional = true, doc = "Item that unlocks it (the RPG kit's `item` condition)" },
    unlock_flag = { type = "string", optional = true, doc = "Defaults to unlocked:<map>:<id>" },
    locked_text = { type = "string", optional = true },
    sound = { type = { asset = "audio" }, optional = true, doc = "Opening sound (default chest)" },
  }, "A treasure chest opened once." },
  door = { {
    map = { type = { map_id = true } }, point = { type = { point = "map" }, doc = "Arrival point on that map" },
    trigger = { type = { enum = { "touch", "action" } }, default = "touch" },
    transition = { type = "string", optional = true }, sprite = { type = { asset = "sprite" }, optional = true },
  }, "Moves the player to another map." },
  sign = { { text = { type = "string" }, sprite = { type = { asset = "sprite" }, optional = true } }, "Shows a message." },
  light = { { color = "table", radius = "number", intensity = "number", flicker = { type = "number", optional = true } },
    "A dynamic light (static lights belong in the map's light list)." },
  emitter = { { sprite = { type = { asset = "sprite" }, optional = true }, rate = "number" }, "Particles (fire, smoke, sparkles)." },
  prop = { {
    model = { type = { asset = "model" }, doc = ".glb file" }, animation = { type = "string", optional = true },
    solid = { type = "boolean", optional = true },
    pages = { type = { list = placements.page_schema }, optional = true, doc = "Event pages (show and hide the mesh)" },
  }, "A 3D object (doors, windmills, crates)." },
  sound = { { sound = { type = { asset = "audio" } }, radius = { type = "number", default = 8 } }, "Looping positional sound." },
  save_point = { { sprite = { type = { asset = "sprite" }, optional = true } }, "Opens the save screen." },
  symbol = { { troop = { type = { ref = "troops" } }, sprite = { type = { asset = "sprite" } },
    sight = { type = "number", default = 4 } }, "A visible enemy." },
  hidden_item = { {
    item = { type = { ref = "items" }, optional = true }, count = { type = "integer", default = 1 },
    money = { type = "integer", optional = true },
    flag = { type = "string", optional = true, doc = "Defaults to hidden:<map>:<id>" },
    sprite = { type = { asset = "sprite" }, optional = true, doc = "Usually none: the spot is invisible" },
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

--- Whether a placement scale is the default (1 on every axis).
---@param scale table?
---@return boolean
function placements.unit_scale(scale)
  return scale == nil or (scale.x == 1 and scale.y == 1 and scale.z == 1)
end

--- Spawn one placement through its type's spawner and apply the
--- placement's yaw and scale to the entity it made (so game-defined types
--- honour the editor's gizmo without reading them). Returns the entity, or
--- nil (no spawner, the spawner made nothing, or it raised: reported).
---@param world World
---@param p table a placement (world:placements() entry)
---@return Entity?
function placements.spawn_one(world, p)
  local spawner = field().types[p.type]
  if not spawner then
    if p.type ~= "marker" then fe.log.warn("No field type '" .. p.type .. "' for placement " .. p.id) end
    return nil
  end
  local ok, entity = pcall(spawner, world, p)
  if not ok then
    fe.log.error("Placement '" .. p.id .. "' (" .. p.type .. "): " .. tostring(entity))
    return nil
  end
  if entity and entity:valid() then
    -- Mesh entities turn and scale; sprites face the camera but still scale.
    if p.yaw and p.yaw ~= 0 and entity:mesh() then entity.yaw = p.yaw end
    if not placements.unit_scale(p.scale) then
      if entity:mesh() then
        entity.scale = p.scale
      elseif entity:sprite() and p.scale.x ~= 0 then
        entity:sprite({ scale = (entity:sprite().scale or 1) * p.scale.x })
      end
    end
  end
  return entity
end

--- Spawn a world's placements by type (`field.types`) into the field's
--- entity list (`field.entities()`, cleared in place first), skipping ones
--- that are following the party; a spawner that raises is reported and
--- skipped.
---@param world World
function placements.spawn(world)
  local entities = field().entities()
  for key in pairs(entities) do entities[key] = nil end
  local following = require("fe.follower")
  for _, p in ipairs(world:placements()) do
    if not following.has(p.id) then
      local entity = placements.spawn_one(world, p)
      if entity then entities[#entities + 1] = entity end
    end
  end
end

--- Respawn one placement after the editor changed it: the entity named by
--- the id is destroyed (and dropped from `field.entities()`), then the
--- placement as the live map now has it is spawned again; with no such
--- placement (deleted) the entity is only removed. Returns the new entity.
---@param world World
---@param id string
---@return Entity?
function placements.respawn(world, id)
  local entities = field().entities()
  local existing = world:find(id)
  for i = #entities, 1, -1 do
    local e = entities[i]
    if not e:valid() or e == existing then table.remove(entities, i) end
  end
  if existing and existing:valid() then existing:destroy() end
  local p = world:placement(id)
  if not p then return nil end
  local entity = placements.spawn_one(world, p)
  if entity then entities[#entities + 1] = entity end
  return entity
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
  local functions = { spawn_placements = placements.spawn, apply_pages = placements.apply_pages,
                      respawn_placement = placements.respawn }
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
