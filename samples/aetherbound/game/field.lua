--- Exploration: the player walks Cael through a map and the other three
--- follow in his footsteps. The story (story/prologue.lua) says which map,
--- what happens when the party crosses a zone, and when control is taken
--- away for a scene (`field.lock`).
---
--- ```lua
--- field.enter("fortress", { cael = "cael", rowan = "rowan", lyra = "lyra", darius = "darius" })
--- field.on_zone.enc1 = function() ... end     -- runs as a task when Cael enters the zone
--- field.lock()  ...cutscene...  field.unlock()
--- ```
local actors = require("game.actors")
local audio = require("game.audio")
local dialogue = require("game.dialogue")
local fx = require("game.fx")
local party = require("game.party")
local scenes = require("game.scenes")
local task = require("game.task")

local field = { world = nil, actors = {}, on_zone = {}, locked = 0, follow = true, camera_follow = true,
  trail = {}, hud = true, scene = {} }

local WALK, RUN = 2.3, 3.8
local SPACING = 0.95 -- between followers, along the leader's trail

local function heroes()
  return party.order
end

function field.leader()
  return field.actors[heroes()[1]]
end

--- Load `map` (in the background, so call it behind a fade) and stand the
--- heroes at the named points: `places` maps hero id -> point name, or is
--- one point name for the leader with the others lined up behind him.
function field.enter(map, places)
  if field.world then
    actors.clear(field.world)
    fx.world_changed()
    fe.world.unload(field.world.name)
  end
  local ticket = fe.world.load_async("field", map)
  local preload = {}
  for _, id in ipairs(heroes()) do preload[#preload + 1] = party.record(id).sprite end
  local assets = fe.app.preload(preload)
  task.wait_until(function() return fe.world.load_progress(ticket) >= 1 and fe.app.progress(assets) >= 1 end)
  local world = fe.world.finish_load(ticket)
  field.world, field.actors, field.on_zone = world, {}, {}
  local points = world:points()
  for index, id in ipairs(heroes()) do
    local point
    if type(places) == "table" then
      point = points[places[id]] or error("map '" .. map .. "' has no point '" .. tostring(places[id]) .. "'")
    else
      point = points[places] or error("map '" .. map .. "' has no point '" .. tostring(places) .. "'")
      -- Lined up behind the leader, away from where he faces.
      local back = (index - 1) * SPACING
      point = { position = { x = point.position.x - point.facing.x * back, y = point.position.y,
        z = point.position.z - point.facing.z * back }, facing = point.facing }
    end
    field.actors[id] = actors.spawn(world, id, party.record(id).sprite, point.position, point.facing,
      { collide = index == 1, speed = WALK })
  end
  field.regroup()
  field.place_camera(true)
  if not scenes.contains(field.scene) then scenes.push(field.scene) end
  return world
end

--- Leave the field (the title, the end of the slice).
function field.leave()
  scenes.pop(field.scene)
  if field.world then
    actors.clear(field.world)
    fx.world_changed()
    fe.world.unload(field.world.name)
  end
  field.world, field.actors, field.on_zone, field.locked = nil, {}, {}, 0
end

--- Take control from the player (a scene, a battle); calls nest.
function field.lock()
  field.locked = field.locked + 1
  local leader = field.leader()
  if leader and leader:valid() then leader:stop() end
end

function field.unlock()
  field.locked = math.max(0, field.locked - 1)
  if field.locked == 0 then field.regroup() end
end

--- Rebuild the trail the followers walk along from where everyone stands
--- now, so that after a scene they fall in behind the leader from there.
function field.regroup()
  field.trail = {}
  for _, id in ipairs(heroes()) do
    local actor = field.actors[id]
    if actor and actor:valid() then
      local p = actor.position
      field.trail[#field.trail + 1] = { x = p.x, z = p.z }
    end
  end
end

local function camera_settings()
  return field.world:props().camera or {}
end

--- Point the camera at the leader (smoothly, or at once with `snap`).
function field.place_camera(snap)
  local leader = field.leader()
  if not leader then return end
  local camera = field.world:camera()
  local base = camera_settings()
  local p = leader.position
  local target = { x = p.x, y = p.y + 1.0, z = p.z - 1.2 }
  local blend = snap and 1 or 0.1
  local current = camera.target
  camera.target = { x = current.x + (target.x - current.x) * blend, y = current.y + (target.y - current.y) * blend,
    z = current.z + (target.z - current.z) * blend }
  camera.distance = camera.distance + ((base.distance or 13) - camera.distance) * blend
  camera.pitch = camera.pitch + ((base.pitch or 27) - camera.pitch) * blend
  camera.yaw = camera.yaw + ((base.yaw or 0) - camera.yaw) * blend
  camera.fov = base.fov or 30
  if snap then camera:snap() end
end

local function follow()
  local leader = field.leader()
  local p = leader.position
  local head = field.trail[1]
  if not head or (p.x - head.x) ^ 2 + (p.z - head.z) ^ 2 > 0.05 * 0.05 then
    table.insert(field.trail, 1, { x = p.x, z = p.z })
    while #field.trail > 160 do table.remove(field.trail) end
  end
  -- Each follower stands SPACING further back along the trail.
  for index = 2, #heroes() do
    local follower = field.actors[heroes()[index]]
    local wanted = (index - 1) * SPACING
    local walked = 0
    for i = 1, #field.trail - 1 do
      local a, b = field.trail[i], field.trail[i + 1]
      local length = math.sqrt((a.x - b.x) ^ 2 + (a.z - b.z) ^ 2)
      if walked + length >= wanted then
        local t = length > 0 and (wanted - walked) / length or 0
        local x, z = a.x + (b.x - a.x) * t, a.z + (b.z - a.z) * t
        local now = follower.position
        local dx, dz = x - now.x, z - now.z
        if dx * dx + dz * dz > 1e-5 then
          follower:place({ x = x, z = z }, leader.layer)
          follower:face({ x = dx, z = dz })
        end
        break
      end
      walked = walked + length
    end
  end
end

function field.scene:update(dt)
  local world = field.world
  if not world then return end
  local leader = field.leader()
  local free = field.locked == 0 and not dialogue.active()
  if free then
    local x, z = fe.input.axis("move_x"), fe.input.axis("move_z")
    local length = math.sqrt(x * x + z * z)
    if length > 0.01 then
      local speed = fe.input.held("run") and RUN or WALK
      local scale = speed / math.max(1, length)
      leader:drive({ x = x * scale, z = z * scale })
      leader:face({ x = x, z = z })
    end
    if field.follow then follow() end
  end
  if field.camera_follow and field.locked == 0 then field.place_camera(false) end
end

function field.scene:post_update()
  local world = field.world
  if not world then return end
  local leader = field.leader()
  for _, event in ipairs(world:events()) do
    if event.type == "zone_enter" and event.a == leader and field.locked == 0 then
      local handler = field.on_zone[event.name]
      if handler then
        field.on_zone[event.name] = nil -- every story zone fires once
        task.spawn(handler, "zone " .. event.name)
      end
    elseif event.type == "anim_event" and event.name == "step" and event.a == leader then
      audio.sfx("step", { volume = 0.22, pitch = 0.92 + (fe.time.ticks() % 5) * 0.04 })
    end
  end
end

function field.scene:draw()
  if field.world then field.world:draw() end
end

return field
