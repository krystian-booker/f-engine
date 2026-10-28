--- The exploration scene: walk around a map, talk to people, open chests,
--- use doors, trigger events and random encounters.
---
--- ```lua
--- field.start("town", "entry")                 -- immediately
--- field.warp("cave", "entrance")               -- in a task, with a fade
--- field.define("statue", function(world, placement) ... return entity end)
--- field.zone("music", function(zone, entered) sound.music(zone.props.track) end)
--- ```
--- Map placements are spawned by type. Built-in types: npc, chest, door,
--- sign, light, emitter, prop, sound, save_point, symbol. Any of them can be
--- replaced, and games add their own. Placement props are documented with
--- fe.schema.entity so the editor shows the right fields.
local camera = require("fe.camera")
local dialogue = require("fe.dialogue")
local encounter = require("fe.encounter")
local event = require("fe.event")
local save = require("fe.save")
local scene = require("fe.scene")
local schema = require("fe.schema")
local signal = require("fe.signal")
local sound = require("fe.sound")
local state = require("fe.state")
local task = require("fe.task")
local transition = require("fe.transition")

local field = {
  world = nil,
  player = nil,
  rig = nil,
  settings = {
    world_name = "field",
    walk_speed = 3.2,
    run_speed = 5.6,
    interact_range = 1.4,
    counter_range = 2.3,
    player_radius = 0.22,
    footstep_sound = "step",
    followers = 0,          -- party members trailing the leader
    follower_spacing = 0.85,
    autosave = true,        -- write the autosave slot after each map transition (field.warp)
    player_sprite = nil,    -- string or function() -> sprite path
  },
  types = {},
  zone_types = {},
  entered = signal.new(),       -- (map_id, point_name)
  interacted = signal.new(),    -- (entity)
  hud = signal.new(),           -- draw overlays above the field
  menu_requested = signal.new(),
  locks = 0,
}

local location = state.section("field", function() return {} end)

local function remember_location()
  if not field.player or not field.player:valid() then return end
  local p = field.player.position
  location.x, location.z, location.layer = p.x, p.z, field.player.layer
  location.facing = field.player.facing
end

--- Placement spawner: fn(world, placement) -> entity or nil.
---@param type_name string
---@param spawner fun(world: World, placement: table): Entity?
---@param fields? table<string, any> props schema for the editor
---@param doc? string
function field.define(type_name, spawner, fields, doc)
  field.types[type_name] = spawner
  if fields then schema.entity(type_name, fields, doc) end
end

--- Zone handler: fn(zone, entered: boolean, entity) for the player.
function field.zone(type_name, handler)
  field.zone_types[type_name] = handler
end

--- Disable player control (counts nested calls).
function field.lock() field.locks = field.locks + 1 end
function field.unlock() field.locks = math.max(0, field.locks - 1) end

function field.can_control()
  return field.locks == 0 and not event.busy() and scene.top() == field.scene
end

local function player_sprite()
  local s = field.settings.player_sprite
  if type(s) == "function" then return s() end
  return s
end

local function apply_pages(entity, placement, defaults)
  local props = placement.props
  local pages = props.pages
  if not pages and props.script then
    pages = { { script = props.script, trigger = props.trigger or "action" } }
  end
  if pages then
    event.attach(entity, pages, defaults)
  end
end

-------------------------------------------------------------------------------
-- Built-in placement types

field.define("npc", function(world, p)
  local props = p.props
  local entity = world:spawn({
    id = p.id, position = p.position, layer = p.layer, facing = p.facing, tags = { "npc" },
    sprite = { animation = props.sprite, move_clip = "walk", idle_clip = "idle" },
    collider = { radius = props.radius or 0.3, solid = props.solid ~= false },
    mover = { speed = props.speed or 1.6 },
    props = props,
  })
  apply_pages(entity, p, { sprite = props.sprite, solid = props.solid })
  local d = entity:data()
  d.routine = props.routine
  d.home = p.position
  d.route = props.route
  return entity
end, {
  name = { type = "string", optional = true, doc = "Speaker name" },
  sprite = { type = "string", doc = "Sprite .anim.json" },
  script = { type = "string", optional = true, doc = "Event module for a single action page" },
  pages = { type = { list = "table" }, optional = true, doc = "Event pages" },
  routine = { type = { enum = { "still", "wander", "patrol" } }, default = "still" },
  route = { type = { list = "table" }, optional = true, doc = "Patrol points [{x, z}]" },
  speed = { type = "number", optional = true },
  solid = { type = "boolean", optional = true },
}, "A person or creature with event pages.")

field.define("chest", function(world, p)
  local props = p.props
  local flag = props.flag or ("chest:" .. world.map_id .. ":" .. p.id)
  local opened = state.flag(flag)
  local entity = world:spawn({
    id = p.id, position = p.position, layer = p.layer, facing = p.facing, tags = { "chest" },
    sprite = { animation = props.sprite or "@fe/assets/ui/spark.anim.json", clip = opened and "open" or "closed" },
    collider = { radius = 0.35 }, props = props,
  })
  entity:data().interact = function()
    if state.flag(flag) then
      dialogue.say(nil, props.empty_text or "It's empty.")
      return
    end
    state.set_flag(flag)
    entity:clip("open")
    sound.play(props.sound or "chest")
    field.give(props.item, props.count or 1, props.money)
  end
  return entity
end, {
  item = { type = "string", optional = true }, count = { type = "integer", default = 1 },
  money = { type = "integer", optional = true }, sprite = { type = "string", optional = true },
  flag = { type = "string", optional = true, doc = "Defaults to chest:<map>:<id>" },
}, "A treasure chest opened once.")

field.define("door", function(world, p)
  local props = p.props
  local entity = world:spawn({
    id = p.id, position = p.position, layer = p.layer, tags = { "door" },
    collider = { radius = props.radius or 0.45, solid = false },
    sprite = props.sprite and { animation = props.sprite } or nil, props = props,
  })
  local function go()
    task.named("warp", field.warp, props.map, props.point, { transition = props.transition })
  end
  if (props.trigger or "touch") == "touch" then
    entity:data().on_touch = go
  else
    entity:data().interact = go
  end
  return entity
end, {
  map = { type = "string" }, point = { type = "string" },
  trigger = { type = { enum = { "touch", "action" } }, default = "touch" },
  transition = { type = "string", optional = true }, sprite = { type = "string", optional = true },
}, "Moves the player to another map.")

field.define("sign", function(world, p)
  local entity = world:spawn({ id = p.id, position = p.position, layer = p.layer, collider = { radius = 0.3 },
    sprite = p.props.sprite and { animation = p.props.sprite } or nil, props = p.props })
  entity:data().interact = function() dialogue.say(nil, p.props.text) end
  return entity
end, { text = { type = "string" }, sprite = { type = "string", optional = true } }, "Shows a message.")

field.define("light", function(world, p)
  local light = {}
  for k, v in pairs(p.props) do light[k] = v end
  light.position = { 0, p.props.height or 0, 0 }
  return world:spawn({ id = p.id, position = p.position, layer = p.layer, keep_height = true, light = light })
end, { color = "table", radius = "number", intensity = "number", flicker = { type = "number", optional = true } },
  "A dynamic light (static lights belong in the map's light list).")

field.define("emitter", function(world, p)
  return world:spawn({ id = p.id, position = p.position, layer = p.layer, keep_height = true, emitter = p.props })
end, { sprite = { type = "string", optional = true }, rate = "number" }, "Particles (fire, smoke, sparkles).")

field.define("prop", function(world, p)
  local props = p.props
  local entity = world:spawn({
    id = p.id, position = p.position, layer = p.layer, yaw = math.rad(props.yaw or 0), scale = props.scale,
    mesh = { model = props.model, animation = props.animation, loop = props.loop ~= false, playing = props.animation ~= nil },
    collider = props.solid and { radius = props.radius or 0.45 } or nil, props = props,
  })
  apply_pages(entity, p, {})
  return entity
end, {
  model = { type = "string", doc = ".glb file" }, animation = { type = "string", optional = true },
  yaw = { type = "number", optional = true }, solid = { type = "boolean", optional = true },
}, "A 3D object (doors, windmills, crates).")

field.define("sound", function(world, p)
  return world:spawn({ id = p.id, position = p.position, layer = p.layer, audio = p.props })
end, { sound = "string", radius = { type = "number", default = 8 } }, "Looping positional sound.")

field.define("save_point", function(world, p)
  local entity = world:spawn({ id = p.id, position = p.position, layer = p.layer, collider = { radius = 0.4 },
    sprite = { animation = p.props.sprite or "@fe/assets/ui/spark.anim.json", unlit = true }, props = p.props })
  entity:data().interact = function() field.on_save_point(entity) end
  return entity
end, { sprite = { type = "string", optional = true } }, "Opens the save screen.")

field.define("symbol", function(world, p)
  return encounter.symbol(world, { troop = p.props.troop, position = p.position, sprite = p.props.sprite,
    sight = p.props.sight, speed = p.props.speed })
end, { troop = "string", sprite = "string", sight = { type = "number", default = 4 } }, "A visible enemy.")

-------------------------------------------------------------------------------
-- Hooks games replace

--- Give treasure; the rpg kit replaces this to use the inventory.
function field.give(item, count, money)
  if item then dialogue.say(nil, "Found " .. item .. (count > 1 and " x" .. count or "") .. ".") end
  if money then dialogue.say(nil, "Found " .. money .. " money.") end
end

--- A save point was used; default opens the save screen if one exists.
function field.on_save_point()
  local ok, screen = pcall(require, "fe.ui.screens.save")
  if ok then screen.open("save") end
end

--- A random encounter fired; default starts a side-view battle.
function field.on_encounter(troop, entry)
  local battle = require("fe.battle")
  battle.encounter(troop, { stage = entry and entry.stage or field.world:props().battle_stage })
end

-------------------------------------------------------------------------------
-- Scene

local FieldScene = { name = "field", opaque = true }
FieldScene.__index = FieldScene
field.scene = setmetatable({}, FieldScene)

local entities = {}
local trail = {}
local followers = {}

local function spawn_placements(world)
  entities = {}
  for _, p in ipairs(world:placements()) do
    local spawner = field.types[p.type]
    if spawner then
      local ok, entity = pcall(spawner, world, p)
      if not ok then
        fe.log.error("Placement '" .. p.id .. "' (" .. p.type .. "): " .. tostring(entity))
      elseif entity then
        entities[#entities + 1] = entity
      end
    elseif p.type ~= "marker" then
      fe.log.warn("No field type '" .. p.type .. "' for placement " .. p.id)
    end
  end
end

local function spawn_followers(world, position)
  followers = {}
  trail = {}
  local count = field.settings.followers
  local sprites = field.follower_sprites and field.follower_sprites() or {}
  for i = 1, math.min(count, #sprites) do
    followers[i] = world:spawn({ position = position, tags = { "follower" },
      sprite = { animation = sprites[i], move_clip = "walk", idle_clip = "idle" }, mover = { collide = false, speed = 9 } })
  end
end

--- Load a map and place the player at a named point (or position).
---@param map_id string
---@param point string|table point name or {x, z, layer?, facing?}
function field.start(map_id, point, options)
  options = options or {}
  dialogue.clear_bubbles()
  local world = fe.world.load(field.settings.world_name, map_id)
  field.world = world
  local spot
  if type(point) == "table" then
    spot = { position = point, layer = point.layer or 0, facing = point.facing }
  else
    spot = world:points()[point or "start"]
    if not spot then error("map '" .. map_id .. "' has no point '" .. tostring(point) .. "'") end
  end
  field.player = world:spawn({
    id = "player", position = spot.position, layer = spot.layer, facing = spot.facing, tags = { "player" },
    sprite = { animation = player_sprite(), move_clip = "walk", idle_clip = "idle" },
    mover = { speed = field.settings.walk_speed, radius = field.settings.player_radius },
    collider = { radius = field.settings.player_radius, solid = false },
  })
  spawn_placements(world)
  spawn_followers(world, field.player.position)
  field.rig = camera.rig(world, { follow = field.player })
  location.map = map_id
  location.map_name = world.map_name
  local music = world:props().music
  if music and options.music ~= false then sound.music(music) end
  encounter.reset()
  if not scene.contains(field.scene) then scene.push(field.scene) end
  field.entered:emit(map_id, point)
  return world
end

--- Change maps with a transition, then autosave (unless settings.autosave
--- or options.autosave is false). Call from a task.
function field.warp(map_id, point, options)
  options = options or {}
  local kind = options.transition or "fade"
  field.lock()
  transition.out(kind, options.duration or 0.35)
  field.start(map_id, point, options)
  transition.into(kind, options.duration or 0.35)
  field.unlock()
  if field.settings.autosave and options.autosave ~= false then
    remember_location()
    save.autosave()
  end
end

--- Reload the current map from the saved location (after loading a save).
function field.resume()
  if not location.map then return false end
  field.start(location.map, { x = location.x, z = location.z, layer = location.layer,
    facing = location.facing and { x = location.facing.x, z = location.facing.z } })
  return true
end

save.add_meta(function(meta)
  meta.location = location.map_name
end)

local function interact()
  local player = field.player
  local p, f = player.position, player.facing
  local ahead = field.world:cell(math.floor(p.x + f.x), math.floor(p.z + f.z), player.layer)
  local range = field.settings.interact_range
  if ahead and require("fe.tablex").contains(ahead.tags, "counter") then
    range = field.settings.counter_range
  end
  local best, best_distance
  for _, entity in ipairs(entities) do
    if entity:valid() and entity ~= player then
      local d = entity:data()
      local page = event.page(entity)
      if d.interact or (page and (page.trigger or "action") == "action") then
        local q = entity.position
        local dx, dz = q.x - p.x, q.z - p.z
        local distance = math.sqrt(dx * dx + dz * dz)
        local facing_dot = distance > 0.01 and (dx * f.x + dz * f.z) / distance or 1
        if distance <= range and facing_dot > 0.45 and (not best or distance < best_distance)
          and math.abs(q.y - p.y) < 1 then
          best, best_distance = entity, distance
        end
      end
    end
  end
  if not best then return end
  field.interacted:emit(best)
  local d = best:data()
  if d.interact then
    task.named("interact:" .. best.id, function()
      event.blocking = event.blocking + 1
      local ok, err = pcall(d.interact, best)
      event.blocking = event.blocking - 1
      dialogue.close()
      if not ok then error(err, 0) end
    end)
  else
    if best:props().face_player ~= false then best:face(player) end
    event.run(best, nil, { player = player })
  end
end

local function touch(other)
  local d = other:data()
  if d.on_touch then
    d.on_touch(other)
    return
  end
  local page = event.page(other)
  if page and page.trigger == "touch" and not event.busy() then
    event.run(other, page, { player = field.player })
  end
  if other:has_tag("encounter_symbol") and not event.busy() then
    local troop = other:props().troop
    other:destroy()
    task.named("symbol battle", field.on_encounter, troop, nil)
  end
end

local zone_lookup = {}

local function handle_events(world)
  local player = field.player
  for _, e in ipairs(world:events()) do
    if e.type == "contact_begin" and (e.a == player or e.b == player) then
      touch(e.a == player and e.b or e.a)
    elseif (e.type == "zone_enter" or e.type == "zone_exit") and e.a == player then
      local zone = zone_lookup[e.name]
      local handler = zone and field.zone_types[zone.type]
      if handler then handler(zone, e.type == "zone_enter", player) end
    elseif e.type == "anim_event" and e.name == "footstep" and e.a == player then
      sound.play(field.settings.footstep_sound, { volume = 0.35 })
    end
  end
end

local function update_routines(dt)
  local rng = field.random or fe.random.new()
  field.random = rng
  for _, entity in ipairs(entities) do
    if entity:valid() then
      local d = entity:data()
      local busy = d.running and d.running:running()
      if d.routine == "wander" and not busy and not entity:moving() then
        d.wait = (d.wait or rng:range(1, 3)) - dt
        if d.wait <= 0 then
          d.wait = rng:range(1.5, 4)
          local target = { x = d.home.x + rng:range(-2, 2), z = d.home.z + rng:range(-2, 2) }
          if entity:can_occupy(target) then entity:move_to(target) end
        end
      elseif d.routine == "patrol" and d.route and not busy and not entity:moving() then
        d.leg = (d.leg or 0) % #d.route + 1
        local point = d.route[d.leg]
        entity:move_to({ x = point.x or point[1], z = point.z or point[2] })
      end
    end
  end
end

local function update_followers()
  if #followers == 0 then return end
  local p = field.player.position
  local last = trail[1]
  if not last or (last.x - p.x) ^ 2 + (last.z - p.z) ^ 2 > 0.01 then
    table.insert(trail, 1, { x = p.x, y = p.y, z = p.z, layer = field.player.layer })
    while #trail > 200 do table.remove(trail) end
  end
  local spacing = field.settings.follower_spacing
  for i, follower in ipairs(followers) do
    local index = math.min(#trail, math.floor(i * spacing / 0.1) + 1)
    local spot = trail[index]
    if spot then
      local q = follower.position
      local dx, dz = spot.x - q.x, spot.z - q.z
      if dx * dx + dz * dz > 0.0004 then
        follower:move_to(spot, { path = false })
      end
    end
  end
end

function FieldScene:enter()
  zone_lookup = {}
end

function FieldScene:update(dt)
  local world, player = field.world, field.player
  if not world or not player or not player:valid() then return end
  if next(zone_lookup) == nil then
    for _, zone in ipairs(world:zones()) do zone_lookup[zone.id] = zone end
  end
  local before = player.position
  if field.can_control() then
    local mx, my = fe.input.axis("move_x"), fe.input.axis("move_y")
    local length = math.sqrt(mx * mx + my * my)
    if length > 1 then mx, my = mx / length, my / length end
    if length > 0.05 then
      local yaw = math.rad(field.rig:yaw())
      local c, s = math.cos(yaw), math.sin(yaw)
      local speed = fe.input.held("run") and field.settings.run_speed or field.settings.walk_speed
      player:drive((c * mx + s * my) * speed, (-s * mx + c * my) * speed)
    end
    if fe.input.pressed("confirm") then
      interact()
    elseif fe.input.pressed("menu") then
      field.menu_requested:emit()
    end
  end
  handle_events(world)
  event.tick(entities)
  update_routines(dt)
  update_followers()
  encounter.update_symbols(world, player)
  local after = player.position
  local moved = math.sqrt((after.x - before.x) ^ 2 + (after.z - before.z) ^ 2)
  if moved > 0 and not event.busy() then
    local troop, entry = encounter.step(world, after, moved, player.layer)
    if troop then task.named("encounter", field.on_encounter, troop, entry) end
  end
  remember_location()
  field.rig:update(dt)
end

function FieldScene:draw()
  if not field.world then return end
  field.world:draw()
  dialogue.draw_bubbles()
  field.hud:emit()
end

-- Keep event pages current as flags and variables change.
state.changed:connect(function()
  for _, entity in ipairs(entities) do event.refresh(entity) end
end)

-- Development: saving the current map in the editor reloads it in place.
require("fe.game").file_changed:connect(function(path)
  if field.world and location.map and path == "maps/" .. location.map .. "/map.json" then
    local p = field.player and field.player:valid() and field.player.position
    local ok, err = pcall(field.start, location.map, p and { x = p.x, z = p.z, layer = field.player.layer } or "start",
      { music = false })
    if not ok then fe.log.error("Map reload failed: " .. tostring(err)) end
  end
end)

--- Entities spawned from placements on the current map.
function field.entities()
  return entities
end

return field
