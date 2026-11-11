--- Exploration: a map loaded as a world, the party walking it, and what the
--- things placed on it do.
---
--- A map's placements (authored in the F1 editor) each have a `type` and
--- `props`. `field.types` says what a type is: how its entity is spawned and
--- what happens when the player acts on it or walks into it. Add a type by
--- adding a function to that table; the editor's forms for the types are in
--- game/editor.lua.
---
---   prop        a model: scenery (its collision is the map's bake)
---   npc         a sprite or a model; `script` names a module (events/*.lua)
---               that returns `function(context)`, run when the player talks
---   chest       gives `item` x `count` once
---   sign        shows `text`
---   save_point  rests the party and opens the save screen
---   door        walking onto it warps to `map` at `point`
---   boss        a model with a `script`, gone once `hide_flag` is set
---   symbol      an enemy in sight: touching it starts `troop`
---   vehicle     a strider or the skiff, once the party owns it
---   emitter, sound   particles and a looping positional sound
local audio = require("game.audio")
local data = require("game.data")
local dialogue = require("game.dialogue")
local scenes = require("game.scenes")
local state = require("game.state")
local task = require("game.task")
local ui = require("game.ui")

local field = {
  world = nil, player = nil, locked = 0, epoch = 0, scene = {}, types = {}, hint = nil,
  settings = { walk = 3.2, run = 5.0, reach = 1.1, danger_low = 60, danger_high = 110 },
  vehicles = {
    strider = { name = "Strider", speed = 6.5, mover = { fly = true, blocked_tags = { "water", "mountain" } },
      about = "Fords rivers. Nothing attacks a strider." },
    skiff = { name = "Skiff", speed = 8, mover = { fly = true, altitude = 2.4, altitude_speed = 3 },
      about = "Flies over everything; lands on open ground." },
  },
}

local rng

-- ------------------------------------------------------------ placements

local function remember(entity, placement, extra)
  local d = entity:data()
  d.placement, d.type, d.props = placement.id, placement.type, placement.props
  for key, value in pairs(extra or {}) do d[key] = value end
  return entity
end

local function visual(placement)
  local props = placement.props
  if props.model then return { mesh = { model = props.model } } end
  if props.sprite then return { sprite = { animation = props.sprite, clip = props.clip or "idle" } } end
  return {}
end

local function spawn(world, placement, spec)
  spec.id = placement.id
  spec.position = placement.position
  spec.layer = placement.layer
  spec.facing = placement.facing
  spec.yaw = placement.yaw
  spec.scale = placement.scale
  spec.tags = spec.tags or { placement.type }
  return world:spawn(spec)
end

--- Run an event script: a module returning `function(context)`.
local function run_script(module, entity)
  local ok, script = pcall(require, module)
  if not ok or type(script) ~= "function" then
    fe.log.error("event script '" .. tostring(module) .. "': " .. tostring(ok and "must return a function" or script))
    return
  end
  script({ entity = entity, player = field.player, world = field.world, field = field })
end

function field.types.prop(world, placement)
  return spawn(world, placement, visual(placement))
end

function field.types.npc(world, placement)
  local spec = visual(placement)
  spec.collider = { radius = 0.3, solid = true }
  if placement.props.wander then spec.mover = { speed = 1.2 } end
  if spec.sprite then spec.sprite.move_clip, spec.sprite.idle_clip = "walk", "idle" end
  local props = placement.props
  return remember(spawn(world, placement, spec), placement, {
    label = "Talk", wander = props.wander and { home = placement.position, wait = 2 } or nil,
    interact = function(entity)
      if entity:sprite() then entity:face(field.player) end
      if props.script then run_script(props.script, entity) end
    end,
  })
end

function field.types.chest(world, placement)
  local key = world.map_id .. "/" .. placement.id
  local spec = visual(placement)
  spec.collider = { radius = 0.3, solid = true }
  if spec.sprite then spec.sprite.clip = state.data.opened[key] and "open" or "closed" end
  return remember(spawn(world, placement, spec), placement, {
    label = "Open",
    interact = function(entity)
      if state.data.opened[key] then
        dialogue.say(nil, "The chest is empty.")
        return
      end
      state.data.opened[key] = true
      if entity:sprite() then entity:clip("open") end
      local item, count = data.get("items", placement.props.item), placement.props.count or 1
      state.add_item(item.id, count)
      audio.sfx("chest")
      dialogue.say(nil, "Found " .. item.name .. (count > 1 and " x" .. count or "") .. ".")
    end,
  })
end

function field.types.sign(world, placement)
  return remember(spawn(world, placement, { collider = { radius = 0.3, solid = false } }), placement, {
    label = "Read",
    interact = function() dialogue.say(nil, placement.props.text or "...") end,
  })
end

function field.types.save_point(world, placement)
  local spec = visual(placement)
  spec.collider = { radius = 0.3, solid = true }
  spec.light = { color = { 0.5, 0.8, 1 }, radius = 3, intensity = 1.5, position = { x = 0, y = 0.8, z = 0 } }
  return remember(spawn(world, placement, spec), placement, {
    label = "Save",
    interact = function()
      state.heal_party()
      audio.sfx("save")
      ui.toast("The party is restored.")
      require("game.menu").save_screen()
    end,
  })
end

function field.types.door(world, placement)
  return remember(spawn(world, placement, visual(placement)), placement, {
    enter = function() field.warp(placement.props.map, placement.props.point) end,
  })
end

function field.types.boss(world, placement)
  local props = placement.props
  if props.hide_flag and state.flag(props.hide_flag) then return nil end
  local spec = visual(placement)
  spec.collider = { radius = 0.9, height = 2, solid = true }
  local entity = remember(spawn(world, placement, spec), placement, {
    label = "Challenge",
    interact = function(e) if props.script then run_script(props.script, e) end end,
  })
  if props.model then entity:play(props.animation or "idle", true) end
  return entity
end

function field.types.symbol(world, placement)
  local spec = visual(placement)
  spec.collider = { radius = 0.35, solid = false }
  return remember(spawn(world, placement, spec), placement, {
    touch = function(entity)
      local outcome = require("game.battle.scene").start(placement.props.troop)
      if outcome == "victory" and entity:valid() then entity:destroy() end
    end,
  })
end

function field.types.vehicle(world, placement)
  local kind = placement.props.kind
  local owned = state.data.vehicles[kind]
  if not owned or state.data.riding == kind then return nil end
  local at = owned.x and { x = owned.x, z = owned.z } or placement.position
  local entity = world:spawn { id = placement.id, position = at, tags = { "vehicle" },
    sprite = { animation = placement.props.sprite, clip = "idle" }, collider = { radius = 0.3, solid = false } }
  return remember(entity, placement, { vehicle = kind, sprite = placement.props.sprite })
end

function field.types.emitter(world, placement)
  return spawn(world, placement, { emitter = placement.props, position = placement.position })
end

function field.types.sound(world, placement)
  local props = placement.props
  return spawn(world, placement, { audio = { sound = props.sound, radius = props.radius, volume = props.volume } })
end

--- Spawn (or respawn) the entity of one placement of the loaded map.
function field.spawn_placement(world, placement)
  local existing = world:find(placement.id)
  if existing then existing:destroy() end
  local make = field.types[placement.type]
  if not make then
    fe.log.warn("map " .. world.map_id .. ": placement '" .. placement.id .. "' has the unknown type '" ..
      tostring(placement.type) .. "'")
    return nil
  end
  return make(world, placement)
end

-- ------------------------------------------------------------ the player

local function lead_sprite()
  return data.get("heroes", state.data.party[1]).sprite
end

local function vehicle_sprite(kind)
  for _, placement in ipairs(field.world:placements()) do
    if placement.type == "vehicle" and placement.props.kind == kind then return placement.props.sprite end
  end
end

local function apply_mount()
  local riding = state.data.riding
  local spec = riding and field.vehicles[riding]
  local mover = { speed = field.settings.walk, radius = 0.25, fly = false, altitude = 0, altitude_speed = 0,
    blocked_tags = {}, required_tags = {} }
  if spec then
    mover.speed = spec.speed
    for key, value in pairs(spec.mover) do mover[key] = value end
  end
  field.player:mover(mover)
  field.player:sprite { animation = riding and vehicle_sprite(riding) or lead_sprite(), move_clip = "walk",
    idle_clip = "idle", silhouette = { 0.25, 0.4, 0.8, 0.55 } } -- stays visible behind houses
end

local function spawn_player(world, position, facing, layer)
  field.player = world:spawn { id = "player", position = position, layer = layer, facing = facing, tags = { "player" },
    sprite = { animation = lead_sprite(), move_clip = "walk", idle_clip = "idle" },
    collider = { radius = 0.25, solid = true }, mover = { speed = field.settings.walk, radius = 0.25 } }
  if state.data.riding and not world:props().overworld then state.data.riding = nil end
  apply_mount()
end

-- ------------------------------------------------------------ maps

local function camera_settings()
  return field.world:props().camera or {}
end

local function place_camera(snap)
  local camera = field.world:camera()
  local base = camera_settings()
  local position = field.player.position
  local distance, pitch = base.distance or 18, base.pitch or 42
  for _, zone in ipairs(field.world:zones_at(position, field.player.layer)) do
    if zone.type == "camera" then
      distance, pitch = zone.props.distance or distance, zone.props.pitch or pitch
    end
  end
  local target = { x = position.x, y = position.y + 0.5, z = position.z }
  if base.min and base.max then
    target.x = math.max(base.min[1], math.min(base.max[1], target.x))
    target.z = math.max(base.min[2], math.min(base.max[2], target.z))
  end
  local blend = snap and 1 or 0.12
  camera.distance = camera.distance + (distance - camera.distance) * blend
  camera.pitch = camera.pitch + (pitch - camera.pitch) * blend
  camera.fov = base.fov or 30
  camera.target = target
  if snap then camera:snap() end
end

--- Load `map` and stand the party at `point` (a named point) or `position`.
function field.start(map, point, position)
  rng = rng or fe.random.new()
  if field.world then fe.world.unload(field.world.name) end
  local world = fe.world.load("field", map)
  field.world = world
  local points = world:points()
  local at = point and points[point] or points.start or points.entry
  local facing = at and at.facing or { x = 0, z = 1 }
  local spot = position or (at and at.position) or { x = 1.5, z = 1.5 }
  for _, placement in ipairs(world:placements()) do field.spawn_placement(world, placement) end
  spawn_player(world, spot, facing, at and at.layer or 0)
  state.data.map, state.data.point = map, point
  place_camera(true)
  audio.music(world:props().music)
  if not scenes.contains(field.scene) then scenes.push(field.scene) end
  field.hint = nil
  ui.toast(world.map_name, 2)
  return world
end

--- Change maps behind a fade. Blocks the calling task.
function field.warp(map, point)
  -- The editor may hold unsaved edits of the map being left.
  if fe.dev.leave_map(field.world.name) == "pending" then
    task.wait_until(function() return fe.dev.leave_map_result() ~= nil end)
    if fe.dev.leave_map_result() == "cancel" then return end
  end
  if state.data.riding == "strider" and not (map == "overworld") then
    -- The strider waits where the party left the world map.
    local p = field.player.position
    state.data.vehicles.strider = { x = p.x, z = p.z }
    state.data.riding = nil
  end
  task.over(0.25, function(t) fe.screen.set { fade = { 0, 0, 0, t } } end)
  field.start(map, point)
  task.over(0.25, function(t) fe.screen.set { fade = { 0, 0, 0, 1 - t } } end)
end

--- Run `fn` as an event: the player cannot move or act until it returns.
function field.event(fn, name)
  field.locked = field.locked + 1
  local epoch = field.epoch
  return task.spawn(function()
    local ok, err = xpcall(fn, function(m) return fe.dev.traceback(nil, tostring(m)) end)
    -- A game that was reset meanwhile (back to the title) has no lock of this event's.
    if field.epoch == epoch then field.locked = field.locked - 1 end
    if not ok then fe.log.error(err) end
  end, name or "event")
end

-- ------------------------------------------------------------ vehicles

local function board(entity)
  local kind = entity:data().vehicle
  state.data.riding = kind
  field.player:place(entity.position)
  entity:destroy()
  apply_mount()
  audio.sfx("equip")
  ui.toast(field.vehicles[kind].name .. ": " .. field.vehicles[kind].about, 3)
end

local function leave_vehicle()
  local kind = state.data.riding
  local cell = field.world:cell(field.player:cell())
  local tags = {}
  for _, tag in ipairs(cell and cell.tags or {}) do tags[tag] = true end
  if not cell or not cell.walkable or tags.mountain or (kind == "skiff" and tags.forest) then
    audio.sfx("buzzer")
    ui.toast(kind == "skiff" and "The skiff needs open ground to land." or "No footing here.")
    return
  end
  local p = field.player.position
  state.data.vehicles[kind] = { x = p.x, z = p.z }
  state.data.riding = nil
  apply_mount()
  for _, placement in ipairs(field.world:placements()) do
    if placement.type == "vehicle" and placement.props.kind == kind then field.spawn_placement(field.world, placement) end
  end
  audio.sfx("equip")
end

--- Give the party a vehicle (it appears on the world map).
function field.grant_vehicle(kind)
  state.data.vehicles[kind] = state.data.vehicles[kind] or {}
end

-- ------------------------------------------------------------ encounters

local function encounter_zone(cell)
  for _, tag in ipairs(cell and cell.tags or {}) do
    local zone = tag:match("^encounter:(.+)$")
    if zone then return data.find("encounters", zone) end
  end
end

local function reset_danger()
  state.data.danger = 0
  state.data.danger_limit = rng:int(field.settings.danger_low, field.settings.danger_high)
end

--- 0..1: how close the next random battle is (the sensor's reading).
function field.danger()
  return math.min(1, state.data.danger / (state.data.danger_limit or field.settings.danger_high))
end

local function step_taken(cell)
  state.data.steps = state.data.steps + 1
  if state.data.riding then return end
  local zone = encounter_zone(field.world:cell(cell))
  field.zone = zone
  if not zone then return end
  if not state.data.danger_limit then reset_danger() end
  state.data.danger = state.data.danger + zone.rate * rng:range(0.6, 1.4)
  if state.data.danger < state.data.danger_limit then return end
  reset_danger()
  local total = 0
  for _, entry in ipairs(zone.troops) do total = total + entry.weight end
  local roll, troop = rng:range(0, total), zone.troops[#zone.troops].troop
  for _, entry in ipairs(zone.troops) do
    roll = roll - entry.weight
    if roll < 0 then
      troop = entry.troop
      break
    end
  end
  field.event(function() require("game.battle.scene").start(troop) end, "encounter")
end

-- ------------------------------------------------------------ the scene

local function facing_target()
  local player = field.player
  local p, f = player.position, player.facing
  local best, best_score
  for _, entity in ipairs(field.world:query { near = p, radius = field.settings.reach + 0.6 }) do
    local d = entity:data()
    if entity ~= player and (d.interact or d.vehicle) then
      local delta = field.world:wrap_delta(p, entity.position)
      local distance = math.sqrt(delta.x * delta.x + delta.z * delta.z)
      local ahead = distance > 0.01 and (delta.x * f.x + delta.z * f.z) / distance or 1
      local reach = field.settings.reach + (entity:collider() and entity:collider().radius or 0)
      if distance <= reach and (ahead > 0.3 or d.vehicle) and (not best or distance < best_score) then
        best, best_score = entity, distance
      end
    end
  end
  return best
end

local function wander(dt)
  for _, entity in ipairs(field.world:query { tag = "npc" }) do
    local d = entity:data()
    if d.wander and not entity:moving() then
      d.wander.wait = d.wander.wait - dt
      if d.wander.wait <= 0 then
        d.wander.wait = rng:range(1.5, 4)
        local home = d.wander.home
        local target = { x = home.x + rng:int(-2, 2), z = home.z + rng:int(-2, 2) }
        if entity:can_occupy(target) then entity:move_to(target) end
      end
    end
  end
end

function field.scene:update(dt)
  state.data.time = state.data.time + dt
  local player = field.player
  if field.locked > 0 or not player or not player:valid() then return end
  wander(dt)
  local x, z = fe.input.axis("move_x"), fe.input.axis("move_z")
  local length = math.sqrt(x * x + z * z)
  if length > 0.01 then
    local speed = player:mover().speed
    if not state.data.riding and fe.input.held("run") then speed = field.settings.run end
    local scale = speed / math.max(1, length)
    player:drive({ x = x * scale, z = z * scale })
    player:face({ x = x, z = z })
  end
  local target = facing_target()
  local d = target and target:data()
  field.hint = nil
  if state.data.riding then
    field.hint = { "board", "Leave the " .. field.vehicles[state.data.riding].name:lower() }
    if fe.input.pressed("board") then leave_vehicle() end
  elseif d and d.vehicle then
    field.hint = { "board", "Board the " .. field.vehicles[d.vehicle].name:lower() }
    if fe.input.pressed("board") or fe.input.pressed("confirm") then board(target) end
  elseif d and d.interact then
    field.hint = { "confirm", d.label or "Check" }
    if fe.input.pressed("confirm") then
      player:stop()
      field.event(function() d.interact(target) end, "interact " .. (d.placement or "?"))
    end
  end
  if fe.input.pressed("menu") and field.locked == 0 and not state.data.riding then
    player:stop()
    require("game.menu").open()
  end
end

function field.scene:post_update()
  if not field.world or not field.player or not field.player:valid() then return end
  for _, event in ipairs(field.world:events()) do
    if event.type == "cell_enter" and event.a == field.player and field.locked == 0 then
      step_taken(event.cell)
      if state.data.riding ~= "skiff" then
        for _, entity in ipairs(field.world:query { tag = "door", near = field.player.position, radius = 1.5 }) do
          local cell = entity:cell()
          if cell.x == event.cell.x and cell.z == event.cell.z and field.locked == 0 then
            field.player:stop()
            field.event(entity:data().enter, "door " .. entity.id)
          end
        end
      end
    elseif event.type == "contact_begin" and field.locked == 0 then
      local other = event.a == field.player and event.b or event.b == field.player and event.a or nil
      local touch = other and other:valid() and other:data().touch
      if touch then
        field.player:stop()
        field.event(function() touch(other) end, "touch " .. other.id)
      end
    end
  end
  place_camera(false)
end

function field.scene:draw()
  if not field.world then return end
  field.world:draw()
  if scenes.top() ~= self then return end
  local w = ui.size()
  -- The encounter sensor: it fills, and reddens, as the next battle nears.
  if field.zone and not state.data.riding and encounter_zone(field.world:cell(field.player:cell())) then
    local danger = field.danger()
    local color = danger < 0.5 and ui.colors.ally or danger < 0.85 and ui.colors.weak or ui.colors.bad
    ui.window(w - 70, 4, 66, 20)
    ui.text("Sensor", w - 64, 8, { color = ui.colors.dim })
    ui.bar(w - 30, 11, 22, 5, danger, color)
  end
  local hints = {}
  if field.hint then hints[#hints + 1] = field.hint end
  if not state.data.riding then
    hints[#hints + 1] = { "menu", "Menu" }
    hints[#hints + 1] = { "run", "Run" }
  end
  if field.locked == 0 then ui.hints(hints) end
end

return field
