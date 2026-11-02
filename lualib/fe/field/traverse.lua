--- Contextual traversal on the field: fixed-path moves (a ledge, a
--- ladder, a gap in a fence), crouch zones and paired actions (pushing a
--- cart, turning a wheel together). fe.field installs this module
--- (`traverse.install(field)`): the `traverse` placement type and verb and
--- the `crouch` zone type; replace any of them on fe.field, or this whole
--- module with your own `fe/field/traverse.lua`.
---
--- `traverse` placement props: `points` (a list of `[x, z]`, `[x, y, z]`,
--- `{x, z}` or point names of the map; the path starts where the player
--- stands), `duration` (seconds; default the path length at walking
--- speed), `clip` (played on the leader for the whole move), `label` (the
--- verb's label, text or an i18n key; default "Traverse"), `range` (how
--- close the player must be, default 1), `facing` (true: the player must
--- face it), `once` (usable once, remembered by `flag`, default
--- `traverse:<map>:<id>`). Using it takes a field lock, moves the leader
--- along the path over `duration` (segments between points with a height
--- keep that height, so ladders climb; others follow the ground), hides
--- the followers meanwhile and then places them behind the leader on the
--- far side, out of sight of the move (an unseen montage).
---
--- `crouch` zones: entering swaps the walk clip of the leader and the
--- followers to `props.clip` (default "crouch_walk", when their sprite has
--- it; `props.idle_clip`, default "crouch_idle", likewise) and caps the
--- player's speed at `props.speed` (default `traverse.settings.crouch_speed`);
--- leaving restores both.
---
--- `traverse.paired{actors, sockets, clips?, clip?, duration, face?}` walks
--- each actor (an Entity, an id or a cutscene actor) to its socket (a
--- point name or position), turns it (to `face`, or the socket point's
--- facing), plays its clip (`clips[i]`, else `clip`) on every actor at
--- once for `duration` seconds and restores their clips: a cart push, a
--- handrail lift, a wheel turned together. Followers among the actors are
--- held off the trail meanwhile (`followers.hold`). Call from a task.
local i18n = require("fe.i18n")
local state = require("fe.state")
local task = require("fe.task")
local vec = require("fe.vec")

local traverse = {
  settings = {
    crouch_speed = 1.6,   -- speed cap inside crouch zones
    walk_timeout = 8,     -- seconds paired actors may take to reach their sockets
  },
}

-- fe.field requires this module while it loads, so it is fetched when needed.
local function field()
  return require("fe.field")
end

i18n.defaults({ fe = { field = { traverse = "Traverse" } } })

-- Kept across hot reload: entities crouching and their saved clips.
local live = { crouching = {} }

--- A map started: crouch caps and saved clips are dropped (fe.field calls
--- this from `field.start`).
function traverse.reset()
  local caps = field().speed_caps
  for key in pairs(caps) do
    if type(key) == "string" and key:sub(1, 7) == "crouch:" then caps[key] = nil end
  end
  live.crouching = {}
end

--- Whether an entity's sprite has a clip.
---@param entity Entity
---@param clip string?
---@return boolean
function traverse.has_clip(entity, clip)
  if not clip or not entity:valid() then return false end
  local sprite = entity:sprite()
  if not (sprite and sprite.animation and sprite.animation ~= "") then return false end
  local ok, clips = pcall(fe.app.sprite_clips, sprite.animation)
  return ok and clips[clip] ~= nil
end

--- The path of a traversal: `points` resolved on `world` to {x, y?, z,
--- layer?} (named points carry their height and layer).
---@param world World
---@param points table[]
---@return table[]
function traverse.resolve(world, points)
  local out = {}
  local named = world:points()
  for i, point in ipairs(points or {}) do
    if type(point) == "string" then
      local spot = named[point]
      if not spot then error("traverse: map '" .. world.map_id .. "' has no point '" .. point .. "'", 2) end
      out[i] = { x = spot.position.x, y = spot.position.y, z = spot.position.z, layer = spot.layer }
    elseif point.x then
      out[i] = { x = point.x, y = point.y, z = point.z, layer = point.layer }
    elseif #point >= 3 then
      out[i] = { x = point[1], y = point[2], z = point[3] }
    else
      out[i] = { x = point[1], z = point[2] }
    end
  end
  return out
end

-- Set the clips a sprite plays while moving and idle, returning a function
-- that puts the previous ones back.
local function swap_clips(entity, move_clip, idle_clip)
  local sprite = entity:valid() and entity:sprite()
  if not sprite then return function() end end
  local saved = { move_clip = sprite.move_clip, idle_clip = sprite.idle_clip }
  local changes = {}
  if move_clip then changes.move_clip = move_clip end
  if idle_clip then changes.idle_clip = idle_clip end
  entity:sprite(changes)
  if idle_clip or move_clip then entity:clip(idle_clip or move_clip) end
  return function()
    if entity:valid() and entity:sprite() then
      entity:sprite({ move_clip = saved.move_clip or "walk", idle_clip = saved.idle_clip or "idle" })
      if saved.idle_clip then entity:clip(saved.idle_clip) end
    end
  end
end

--- Move an entity along `path` (from `traverse.resolve`, starting where it
--- stands) over `duration` seconds playing `clip`, ignoring walkability (a
--- fixed path). Waits; in a skipping task it arrives at once.
---@param entity Entity
---@param path table[]
---@param options? {duration?: number, clip?: string}
function traverse.move(entity, path, options)
  options = options or {}
  local p = entity.position
  local legs = { { x = p.x, y = p.y, z = p.z, layer = entity.layer } }
  for _, point in ipairs(path) do legs[#legs + 1] = point end
  local lengths, total = {}, 0
  for i = 2, #legs do
    lengths[i] = vec.flat_distance(legs[i - 1], legs[i]) + math.abs((legs[i].y or 0) - (legs[i - 1].y or 0)) * 0.5
    total = total + lengths[i]
  end
  local duration = options.duration or total / field().settings.walk_speed
  local clip = traverse.has_clip(entity, options.clip) and options.clip or nil
  local restore = swap_clips(entity, clip, clip)
  local function put(t)
    local along = t * total
    for i = 2, #legs do
      local a, b = legs[i - 1], legs[i]
      if along <= lengths[i] or i == #legs then
        local u = lengths[i] > 0 and math.min(1, along / lengths[i]) or 1
        local layer = b.layer or a.layer or entity.layer
        local spot = { x = a.x + (b.x - a.x) * u, z = a.z + (b.z - a.z) * u }
        if b.x ~= a.x or b.z ~= a.z then entity:face({ x = b.x - a.x, z = b.z - a.z }) end
        if a.y and b.y then
          spot.y = a.y + (b.y - a.y) * u
          entity:place(spot, layer, true)
        else
          entity:place(spot, layer)
        end
        return
      end
      along = along - lengths[i]
    end
  end
  task.ensure(function()
    entity:stop()
    local started = task.clock()
    while not task.skipping() and duration > 0 do
      local t = math.min(1, (task.clock() - started) / duration)
      put(t)
      if t >= 1 then break end
      task.frame()
    end
  end, function()
    if entity:valid() then put(1) end
    restore()
  end)
end

local function flag_of(world, placement_id, props)
  return props.flag or ("traverse:" .. world.map_id .. ":" .. placement_id)
end

--- The traversal spawner: an invisible marker entity the verb acts on.
function traverse.spawn(world, p)
  local entity = world:spawn({ id = p.id, position = p.position, layer = p.layer, tags = { "traverse" }, props = p.props })
  entity:data().traverse = { flag = flag_of(world, p.id, p.props) }
  return entity
end

--- Whether the player can use a traversal now: in range (`props.range`,
--- default 1), facing it when `props.facing`, and not used up (`once`).
---@param entity Entity
---@param player Entity
---@return boolean
function traverse.usable(entity, player)
  local d = entity:data().traverse
  if not d then return false end
  local props = entity:props()
  if props.once and state.flag(d.flag) then return false end
  local f = field()
  local delta = f.world:wrap_delta(player.position, entity.position)
  local distance = math.sqrt(delta.x * delta.x + delta.z * delta.z)
  if distance > (props.range or 1) then return false end
  if props.facing and distance > 0.01 then
    local fc = player.facing
    if (delta.x * fc.x + delta.z * fc.z) / distance <= f.settings.facing_dot then return false end
  end
  return true
end

--- Use a traversal (the verb's `run`, in the interaction task): see the
--- header.
---@param entity Entity
---@param player Entity
function traverse.run(entity, player)
  local f = field()
  local props = entity:props()
  local d = entity:data().traverse
  local world = f.world
  local path = traverse.resolve(world, props.points)
  if #path == 0 then return end
  local followers = require("fe.field.followers")
  local unlock = f.lock()
  local start = { x = player.position.x, z = player.position.z }
  followers.set_visible(false)
  task.ensure(function()
    traverse.move(player, path, { duration = props.duration, clip = props.clip })
  end, function()
    if player:valid() and world == f.world then
      local back = {}
      for i = #path - 1, 1, -1 do back[#back + 1] = path[i] end
      back[#back + 1] = start
      followers.reset_trail(back)
    end
    followers.set_visible(true)
    unlock()
  end)
  if props.once then state.set_flag(d.flag) end
end

--- The "traverse" verb (Confirm near a traversal).
traverse.verb = {
  input = "confirm",
  range = 1000, -- `usable` measures each placement's own range
  facing = false,
  when = function(entity, player) return traverse.usable(entity, player) end,
  label = function(entity) return entity:props().label or "fe.field.traverse" end,
  run = function(entity, player) traverse.run(entity, player) end,
}

-- Entities a crouch affects: the player and the followers.
local function crouchers()
  local f = field()
  local list = { f.player }
  for _, follower in ipairs(f.followers()) do list[#list + 1] = follower end
  return list
end

--- The crouch zone handler: see the header.
---@param zone table
---@param entered boolean
function traverse.crouch(zone, entered)
  local f = field()
  local props = zone.props or {}
  local key = "crouch:" .. zone.id
  if entered then
    f.speed_caps[key] = props.speed or traverse.settings.crouch_speed
    if live.crouching[key] then return end
    local restores = {}
    for _, entity in ipairs(crouchers()) do
      if entity and entity:valid() then
        local move = props.clip or "crouch_walk"
        local idle = props.idle_clip or "crouch_idle"
        move = traverse.has_clip(entity, move) and move or nil
        idle = traverse.has_clip(entity, idle) and idle or nil
        if move or idle then restores[#restores + 1] = swap_clips(entity, move, idle) end
      end
    end
    live.crouching[key] = restores
  else
    f.speed_caps[key] = nil
    for _, restore in ipairs(live.crouching[key] or {}) do restore() end
    live.crouching[key] = nil
  end
end

--- Whether the player is inside a crouch zone.
---@return boolean
function traverse.crouching()
  return next(live.crouching) ~= nil
end

local function entity_of(actor, world)
  if type(actor) == "string" then
    local found = world:find(actor)
    if not found then error("traverse.paired: no entity '" .. actor .. "'", 3) end
    return found
  end
  if type(actor) == "table" and actor.entity then return actor.entity end
  return actor
end

--- A paired action (see the header). Waits; call from a task. Holds a
--- field lock while it runs.
---@param spec {actors: table[], sockets: table[], clips?: string[], clip?: string, duration: number, face?: any, world?: World}
function traverse.paired(spec)
  local f = field()
  local world = spec.world or f.world
  local unlock = f.lock()
  local actors, goals, facings = {}, {}, {}
  local named = world:points()
  for i, actor in ipairs(spec.actors) do
    actors[i] = entity_of(actor, world)
    local socket = spec.sockets[i]
    if type(socket) == "string" then
      local point = named[socket]
      if not point then error("traverse.paired: map '" .. world.map_id .. "' has no point '" .. socket .. "'", 2) end
      goals[i] = { x = point.position.x, z = point.position.z, layer = point.layer }
      facings[i] = point.facing
    else
      goals[i] = { x = socket.x or socket[1], z = socket.z or socket[2], layer = socket.layer }
    end
  end
  local restores = {}
  local followers = require("fe.field.followers")
  for _, entity in ipairs(actors) do
    for _, follower in ipairs(followers.list()) do
      if follower == entity then restores[#restores + 1] = followers.hold(follower) end
    end
  end
  task.ensure(function()
    if not task.skipping() then
      for i, entity in ipairs(actors) do entity:move_to({ x = goals[i].x, z = goals[i].z }, { path = true }) end
      local current = task.current()
      task.wait_until(function()
        if task.skipping(current) then return true end
        for _, entity in ipairs(actors) do
          if entity:valid() and entity:moving() then return false end
        end
        return true
      end, traverse.settings.walk_timeout)
    end
    for i, entity in ipairs(actors) do
      entity:stop()
      entity:place({ x = goals[i].x, z = goals[i].z }, goals[i].layer or entity.layer)
      local face = spec.face or facings[i]
      if type(face) == "string" and named[face] then face = named[face].position end
      if type(face) == "table" and face.entity then face = face.entity end
      if face then entity:face(face) end
      local clip = spec.clips and spec.clips[i] or spec.clip
      if traverse.has_clip(entity, clip) then restores[#restores + 1] = swap_clips(entity, clip, clip) end
    end
    task.wait(spec.duration or 1)
  end, function()
    for _, restore in ipairs(restores) do restore() end
    unlock()
  end)
end

-- What this module installed on fe.field, so a reload replaces its own
-- registrations and leaves a game's alone.
local installed = {}

--- Register the `traverse` placement type, the "traverse" verb and the
--- `crouch` zone type on `field` (each unless the game replaced it).
---@param field table the fe.field module
function traverse.install(field)
  if not field.types.traverse or field.types.traverse == installed.spawn then
    field.define("traverse", traverse.spawn, {
      points = { type = { list = "any" }, doc = "Path: [x, z], [x, y, z] or point names, from the player onwards" },
      duration = { type = "number", optional = true, min = 0, doc = "Seconds (default: the length at walking speed)" },
      clip = { type = "string", optional = true, doc = "Clip the leader plays while moving" },
      label = { type = "string", optional = true, doc = "Verb label (default Traverse)" },
      range = { type = "number", default = 1, min = 0, doc = "How close the player must stand" },
      facing = { type = "boolean", optional = true, doc = "The player must face it" },
      once = { type = "boolean", optional = true, doc = "Usable once" },
      flag = { type = "string", optional = true, doc = "Defaults to traverse:<map>:<id>" },
    }, "A fixed-path move (ledge, ladder, gap) the player triggers with Confirm.")
    installed.spawn = traverse.spawn
  end
  local verbs = field.verbs
  local existing
  for _, verb in ipairs(verbs) do
    if verb.id == "traverse" then existing = verb end
  end
  if existing == nil or existing == installed.verb then
    installed.verb = require("fe.field.interaction").register(verbs, "traverse", traverse.verb)
  end
  if not field.zone_types.crouch or field.zone_types.crouch == installed.crouch then
    installed.crouch = function(zone, entered) traverse.crouch(zone, entered) end
    field.zone("crouch", installed.crouch, {
      clip = { type = "string", optional = true, doc = "Walk clip inside (default crouch_walk)" },
      idle_clip = { type = "string", optional = true, doc = "Idle clip inside (default crouch_idle)" },
      speed = { type = "number", optional = true, min = 0, doc = "Speed cap inside" },
    }, "Crouch: the party swaps to crouching clips and moves slower inside.")
  end
end

--- Install the fresh registrations after a hot reload of this file.
function traverse.__reload(live_module)
  live_module.install(require("fe.field"))
end

return traverse
