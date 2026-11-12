--- Characters standing in a world: a sprite entity with a soft shadow at
--- its feet, a mover so it can be told to walk somewhere, and a clip that
--- follows what it is doing (idle, walk, run) unless a pose is set.
---
--- ```lua
--- local cael = actors.spawn(world, "cael", "sprites/cael.anim.json", { x = 16, z = 64 }, { x = 0, z = -1 })
--- actors.pose(cael, "kneel")     -- hold a clip
--- actors.pose(cael, nil)         -- back to idle/walk by itself
--- ```
local actors = { list = {} }

local RUN_SPEED = 3.4 -- faster than this and the walk becomes a run

--- options: `collide` (the player's character), `speed`, `shadow = false`,
--- `scale`, `clip` (a starting pose).
function actors.spawn(world, id, sprite, position, facing, options)
  options = options or {}
  local entity = world:spawn { id = id, position = position, facing = facing or { x = 0, z = 1 },
    sprite = { animation = sprite, clip = options.clip or "idle", mode = "standing", scale = options.scale,
      cast_shadow = true },
    mover = { speed = options.speed or 2.4, radius = 0.28, collide = options.collide or false, max_step = 0.6 },
    collider = options.collide and { radius = 0.28, height = 1.5, solid = true } or nil }
  local actor = { entity = entity, world = world, id = id, pose = options.clip, clip = options.clip or "idle",
    last = entity.position, speed = 0 }
  if options.shadow ~= false then
    actor.shadow = world:spawn { position = position,
      sprite = { animation = "fx/battle/shadow.anim.json", mode = "decal", blend = "multiply", unlit = true,
        depth_bias = 0.02 } }
    actor.shadow:attach(entity, { x = 0, y = 0, z = 0 }, { ground = true })
  end
  actors.list[entity.key] = actor
  return entity
end

function actors.of(entity)
  return actors.list[entity.key]
end

--- Hold `clip` (restarting it) until actors.pose(entity, nil).
function actors.pose(entity, clip, restart)
  local actor = actors.list[entity.key]
  if not actor then return end
  actor.pose = clip
  if clip then
    actor.clip = clip
    entity:clip(clip, restart ~= false)
  end
end

--- The clips a moving and a resting actor shows when it has no pose: the
--- battle scene swaps in "ready" for "idle".
function actors.rest(entity, clip)
  local actor = actors.list[entity.key]
  if actor then actor.rest = clip end
end

function actors.remove(entity)
  local actor = actors.list[entity.key]
  if not actor then return end
  if actor.shadow and actor.shadow:valid() then actor.shadow:destroy() end
  if entity:valid() then entity:destroy() end
  actors.list[entity.key] = nil
end

--- Forget every actor of a world that is being unloaded.
function actors.clear(world)
  for key, actor in pairs(actors.list) do
    if not world or actor.world == world or not actor.entity:valid() then actors.list[key] = nil end
  end
end

function actors.update(dt)
  for key, actor in pairs(actors.list) do
    local entity = actor.entity
    if not entity:valid() then
      actors.list[key] = nil
    else
      local p = entity.position
      local dx, dz = p.x - actor.last.x, p.z - actor.last.z
      actor.speed = dt > 0 and math.sqrt(dx * dx + dz * dz) / dt or 0
      actor.last = p
      if not actor.pose then
        local clip = actor.rest or "idle"
        if actor.speed > RUN_SPEED then clip = "run" elseif actor.speed > 0.2 then clip = "walk" end
        if clip ~= actor.clip then
          actor.clip = clip
          entity:clip(clip, false)
        end
      end
    end
  end
end

return actors
