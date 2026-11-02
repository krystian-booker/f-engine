--- The field's followers: party members (`field.settings.followers` of
--- them, sprites from `field.follower_sprites`) and then NPC followers from
--- fe.follower walk the leader's trail. fe.field installs this module
--- (`followers.install(field)`), which gives the field its
--- `spawn_followers`, `refresh_followers`, `followers` and
--- `update_followers` functions; replace any of them on fe.field, or this
--- whole module with your own `fe/field/followers.lua`.
---
--- The trail is sampled every `settings.trail_step` of leader movement and
--- keeps `settings.trail_length` points; follower i walks to the point
--- `i * settings.follower_spacing` behind the leader, at
--- `settings.follower_speed`, once it is more than `settings.follower_settle`
--- away from it.
---
--- Loose formation, catch-up and separation (`followers.settings`, all off
--- by default):
---   * `band = {min, max}`: follower i (from 1) stands still while it is
---     within `max + (i - 1) * follower_spacing` of the leader and only
---     then walks, trailing at that distance: a loose 1.5 to 3 m formation
---     instead of a fixed spacing;
---   * `catchup = true`: a follower stuck for `catchup_stuck` seconds (5),
---     or farther than `catchup_distance` (12) from the leader, is moved to
---     the nearest walkable trail point that is off camera, and only while
---     it is itself off camera: it is never teleported on screen;
---   * `separation = pixels`: a follower whose goal would show it within
---     that many canvas pixels of the leader or another follower is
---     nudged further back along the trail.
--- `followers.gather(options)` brings every follower next to the leader
--- before a cinematic or a choice (unseen when they are off screen),
--- `followers.reset_trail(points)` restarts the trail along given points
--- and places the followers on it, `followers.set_visible(on)` hides or
--- shows them (fe.field.traverse does both for its unseen montage), and
--- `followers.on_screen(position)` is the visibility test (replaceable).
---
--- Party changes (the Formation page, a tavern): a party module handed to
--- `followers.watch_party(party)` (fe.rpg's `install` does this) has its
--- `changed` signal re-read the leader's sprite and respawn the followers.
--- fe.follower's `joined` and `left` do the same.
local tablex = require("fe.tablex")
local vec = require("fe.vec")

local followers = {
  settings = {
    band = nil,             -- {min, max}: loose formation distances (nil: fixed spacing)
    catchup = false,        -- move stuck or distant followers off camera
    catchup_stuck = 5,      -- seconds a follower may be stuck
    catchup_distance = 12,  -- farthest a follower may be from the leader
    separation = nil,       -- canvas pixels kept between follower sprites (nil: off)
    offscreen_margin = 12,  -- canvas pixels beyond the edge that still count as on screen
    lift = 0.8,             -- height above the feet used for screen tests
  },
}

--- The file's settings, for `__reload`.
followers.file_defaults = { settings = tablex.deep_copy(followers.settings) }

-- fe.field requires this module while it loads, so it is fetched when needed.
local function field()
  return require("fe.field")
end

-- Lists kept across hot reload are cleared in place, never replaced.
local entities = {}
local trail = {}
-- Per follower (by index): seconds stuck and the last position seen.
local status = {}
-- Followers scripts hold in place (entity key -> count).
local held = {}
local clock = { last = nil }

local function clear(t)
  for key in pairs(t) do t[key] = nil end
end

--- Sprites for party followers (the RPG kit replaces this on fe.field).
---@return string[]
function followers.sprites()
  return {}
end

--- Spawn the followers at `position` on `world`, behind the player: the
--- party's first, then NPC followers. Followers already spawned are
--- removed and the trail starts over.
---@param world World
---@param position table
function followers.spawn(world, position)
  local f = field()
  for _, entity in ipairs(entities) do
    if entity:valid() then entity:destroy() end
  end
  clear(entities)
  clear(trail)
  clear(status)
  clock.last = nil
  local sprites = {}
  local party = f.follower_sprites()
  for i = 1, math.min(f.settings.followers, #party) do sprites[#sprites + 1] = party[i] end
  for _, entry in ipairs(require("fe.follower").list()) do sprites[#sprites + 1] = entry.sprite end
  for i, sprite in ipairs(sprites) do
    entities[i] = world:spawn({ position = position, layer = f.player.layer, tags = { "follower" },
      sprite = { animation = sprite, move_clip = "walk", idle_clip = "idle" },
      mover = { collide = false, speed = f.settings.follower_speed } })
  end
end

--- Respawn the followers behind the player (after the party or the NPC
--- followers change; fe.follower and fe.rpg.party changes do this by
--- themselves).
function followers.refresh()
  local f = field()
  if f.world and f.player and f.player:valid() then
    f.spawn_followers(f.world, f.player.position)
  end
end

--- The field's followers (party, then NPCs), nearest the leader first.
---@return Entity[]
function followers.list()
  return entities
end

--- Whether a world position shows on the canvas (with
--- `settings.offscreen_margin` pixels of slack), seen by the field camera.
--- Replace for another camera or test.
---@param position table
---@return boolean
function followers.on_screen(position)
  local f = field()
  if not f.world then return false end
  local cw, ch = fe.canvas.size()
  local margin = followers.settings.offscreen_margin
  local x, y, visible = f.world:camera():project({ x = position.x, y = (position.y or 0) + followers.settings.lift,
    z = position.z })
  return visible and x >= -margin and x <= cw + margin and y >= -margin and y <= ch + margin
end

-- The trail index `distance` behind the leader, counted in samples (the
-- fixed spacing's rule).
local function index_at(distance)
  local step = field().settings.trail_step
  return math.min(#trail, math.floor(distance / step) + 1)
end

-- The trail index whose distance along the trail is closest to `distance`
-- without passing it (the band's rule: measured, not counted).
local function index_along(distance)
  local world = field().world
  local walked = 0
  for i = 2, #trail do
    walked = walked + vec.flat_distance_in(world, trail[i - 1], trail[i])
    if walked > distance then return i - 1 end
  end
  return #trail
end

--- The trail index follower `i` heads for, or nil to stand still (a
--- loose formation within its band).
---@param i integer
---@param follower Entity
---@return integer?
function followers.goal(i, follower)
  local f = field()
  local settings = f.settings
  local band = followers.settings.band
  if not band then return index_at(i * settings.follower_spacing) end
  local reach = band.max + (i - 1) * settings.follower_spacing
  if vec.flat_distance_in(f.world, f.player.position, follower.position) <= reach then return nil end
  return index_along(reach)
end

-- Canvas position of a world point (nil when it does not show).
local function project(position)
  local x, y, visible = field().world:camera():project({ x = position.x,
    y = (position.y or 0) + followers.settings.lift, z = position.z })
  if not visible then return nil end
  return x, y
end

-- Push goals back along the trail while a follower would show too close
-- to the leader or a follower before it.
local function separate(goals)
  local pixels = followers.settings.separation
  local f = field()
  local taken = {}
  local lx, ly = project(f.player.position)
  if lx then taken[1] = { lx, ly } end
  local nudge = math.max(1, math.floor(0.3 / f.settings.trail_step + 0.5))
  local limit = math.floor(2 / f.settings.trail_step)
  for i, follower in ipairs(entities) do
    local index = goals[i]
    local spot = index and trail[index] or follower.position
    local x, y = project(spot)
    if index and x then
      local moved = 0
      local function crowded()
        for _, other in ipairs(taken) do
          if math.abs(other[1] - x) < pixels and math.abs(other[2] - y) < pixels * 0.75 then return true end
        end
        return false
      end
      while crowded() and moved < limit and index < #trail do
        index = math.min(#trail, index + nudge)
        moved = moved + nudge
        x, y = project(trail[index])
        if not x then break end
      end
      goals[i] = index
    end
    if x then taken[#taken + 1] = { x, y } end
  end
end

--- Move a follower to the nearest walkable trail point (from its goal
--- backwards) that is off camera, if it is off camera itself. Returns
--- whether it moved.
---@param i integer
---@param follower Entity
---@return boolean
function followers.relocate(i, follower)
  local f = field()
  if followers.on_screen(follower.position) then return false end
  local start = index_at(i * f.settings.follower_spacing)
  for index = start, #trail do
    local spot = trail[index]
    if not followers.on_screen(spot) and follower:can_occupy(spot, spot.layer) then
      follower:stop()
      follower:place({ x = spot.x, z = spot.z }, spot.layer)
      status[i] = nil
      return true
    end
  end
  return false
end

local function check_catchup(i, follower, spot, dt)
  local settings = followers.settings
  local f = field()
  local st = status[i] or { stuck = 0 }
  status[i] = st
  local p = follower.position
  local wants = spot and vec.flat_distance_in(f.world, spot, p) > math.max(f.settings.follower_settle, 0.3)
  if wants and st.last and vec.flat_distance_in(f.world, st.last, p) < 0.02 then
    st.stuck = st.stuck + dt
  else
    st.stuck = 0
  end
  st.last = { x = p.x, z = p.z }
  local far = vec.flat_distance_in(f.world, f.player.position, p) > settings.catchup_distance
  if far or st.stuck >= settings.catchup_stuck then followers.relocate(i, follower) end
end

--- Move followers along the leader's trail (fe.field calls this after the
--- world moved).
function followers.update()
  if #entities == 0 then return end
  local f = field()
  local settings = f.settings
  local p = f.player.position
  local last = trail[1]
  if not last or vec.flat_distance_in(f.world, last, p) > settings.trail_step then
    table.insert(trail, 1, { x = p.x, y = p.y, z = p.z, layer = f.player.layer })
    while #trail > settings.trail_length do table.remove(trail) end
  end
  local now = f.world.time
  local dt = clock.last and now - clock.last or 0
  clock.last = now
  local goals = {}
  for i, follower in ipairs(entities) do
    if follower:valid() then goals[i] = f.follower_goal(i, follower) end
  end
  if followers.settings.separation then separate(goals) end
  for i, follower in ipairs(entities) do
    if follower:valid() and held[follower.key] then
      status[i] = nil
    elseif follower:valid() then
      local spot = goals[i] and trail[goals[i]]
      if spot and vec.flat_distance_in(f.world, spot, follower.position) > settings.follower_settle then
        follower:move_to(spot, { path = false })
      end
      local sprite = follower:sprite()
      if followers.settings.catchup and not (sprite and sprite.visible == false) then
        check_catchup(i, follower, spot, dt)
      end
    end
  end
end

--- Keep a follower where scripts put it (a paired action, a cutscene
--- mark) until the returned function is called or the calling task ends:
--- the trail does not move it meanwhile.
---@param follower Entity
---@return fun()
function followers.hold(follower)
  local key = follower.key
  held[key] = (held[key] or 0) + 1
  return require("fe.task").cleanup(function()
    held[key] = (held[key] or 1) - 1
    if held[key] <= 0 then held[key] = nil end
  end)
end

--- Whether a follower is held in place.
---@param follower Entity
---@return boolean
function followers.held(follower)
  return held[follower.key] ~= nil
end

--- Hide or show every follower (an unseen montage).
---@param visible boolean
function followers.set_visible(visible)
  for _, follower in ipairs(entities) do
    if follower:valid() and follower:sprite() then follower:sprite({ visible = visible }) end
  end
end

-- Where follower i stands next to the leader: its trail spot, or behind
-- the leader's facing on a short trail.
local function gather_spot(i)
  local f = field()
  local band = followers.settings.band
  local distance = band and (band.min + (i - 1) * f.settings.follower_spacing) or i * f.settings.follower_spacing
  local index = band and index_along(distance) or math.floor(distance / f.settings.trail_step) + 1
  if trail[index] and (index > 1 or #trail == 1) then return trail[index] end
  local p, facing = f.player.position, f.player.facing
  local spot = { x = p.x - facing.x * distance, z = p.z - facing.z * distance, layer = f.player.layer }
  if f.player:can_occupy(spot, spot.layer) then return spot end
  return { x = p.x, z = p.z, layer = f.player.layer }
end

--- Restart the trail along `points` (nearest the leader first, {x, z,
--- layer?}; default the leader's position) and place every follower on
--- its spot at once.
---@param points? table[]
function followers.reset_trail(points)
  local f = field()
  clear(trail)
  clear(status)
  local layer = f.player.layer
  local list = { { x = f.player.position.x, z = f.player.position.z } }
  for _, point in ipairs(points or {}) do list[#list + 1] = point end
  local step = f.settings.trail_step
  trail[1] = { x = list[1].x, z = list[1].z, y = f.world:height_at(list[1], layer), layer = layer }
  for i = 2, #list do
    local a, b = list[i - 1], list[i]
    local length = vec.flat_distance(a, b)
    local count = math.max(1, math.floor(length / step))
    for k = 1, count do
      if #trail >= f.settings.trail_length then break end
      local t = k / count
      local q = { x = a.x + (b.x - a.x) * t, z = a.z + (b.z - a.z) * t, layer = b.layer or layer }
      q.y = f.world:height_at(q, q.layer)
      trail[#trail + 1] = q
    end
  end
  for i, follower in ipairs(entities) do
    if follower:valid() then
      local spot = gather_spot(i)
      -- A spot the follower cannot stand on (inside the obstacle crossed)
      -- gives way to the next one towards the leader.
      local index = tablex.index_of(trail, spot)
      while index and index > 1 and not follower:can_occupy(spot, spot.layer or layer) do
        index = index - 1
        spot = trail[index]
      end
      follower:stop()
      follower:place({ x = spot.x, z = spot.z }, spot.layer or layer)
    end
  end
end

--- Bring every follower next to the leader (before a cinematic or a
--- choice). With `instant` they are placed at once (cover the screen
--- first). Otherwise followers off camera are first moved unseen to the
--- nearest off-camera trail point, then all walk to their spots; inside a
--- task this waits until they arrive (at most `timeout` seconds, default
--- 4) unless `wait = false`, and places any still missing.
---@param options? {instant?: boolean, wait?: boolean, timeout?: number}
function followers.gather(options)
  options = options or {}
  local f = field()
  if #entities == 0 or not (f.player and f.player:valid()) then return end
  local spots, holds = {}, {}
  for i, follower in ipairs(entities) do
    if follower:valid() then
      local spot = gather_spot(i)
      spots[i] = spot
      if not options.instant then holds[#holds + 1] = followers.hold(follower) end
      if options.instant then
        follower:stop()
        follower:place({ x = spot.x, z = spot.z }, spot.layer or f.player.layer)
      else
        if not followers.on_screen(follower.position) and
            vec.flat_distance_in(f.world, f.player.position, follower.position) > followers.settings.catchup_distance then
          followers.relocate(i, follower)
        end
        follower:move_to({ x = spot.x, z = spot.z }, { path = true })
      end
    end
  end
  local function release()
    for _, hold in ipairs(holds) do hold() end
  end
  if options.instant then return end
  if options.wait == false or not require("fe.task").current() then
    -- Not waiting: let the trail take over again once they have arrived.
    require("fe.task").detached("followers:gather", function()
      require("fe.task").wait_until(function()
        for i, follower in ipairs(entities) do
          if follower:valid() and spots[i] and follower:moving() then return false end
        end
        return true
      end, options.timeout or 4)
      release()
    end)
    return
  end
  local function arrived()
    for i, follower in ipairs(entities) do
      if follower:valid() and spots[i] and vec.flat_distance_in(f.world, spots[i], follower.position) > 0.2 then
        return false
      end
    end
    return true
  end
  require("fe.task").ensure(function()
    require("fe.task").wait_until(arrived, options.timeout or 4)
    for i, follower in ipairs(entities) do
      if follower:valid() and spots[i] and vec.flat_distance_in(f.world, spots[i], follower.position) > 0.2 then
        follower:stop()
        follower:place({ x = spots[i].x, z = spots[i].z }, spots[i].layer or f.player.layer)
      end
    end
  end, release)
end

--- The party changed: give the player the leader's sprite when it differs
--- and respawn the followers.
function followers.on_party_changed()
  local f = field()
  if not (f.world and f.player and f.player:valid()) then return end
  local sprite = f.player_sprite()
  local current = f.player:sprite()
  if sprite and (not current or current.animation ~= sprite) then
    f.player:sprite({ animation = sprite, move_clip = "walk", idle_clip = "idle" })
  end
  f.refresh_followers()
end

--- The party module being watched (set by `watch_party`). The field never
--- loads the RPG kit by itself: `rpg.install` hands its party module over.
followers.party = nil

--- Listen to `party.changed` (a module with a `changed` signal, normally
--- `fe.rpg.party`; keyed, so calling this again changes nothing). Without an
--- argument the last watched party is used. Returns whether one is watched.
---@param party? table
---@return boolean
function followers.watch_party(party)
  party = party or followers.party
  if not party or not party.changed then return false end
  followers.party = party
  party.changed:connect(function() followers.on_party_changed() end, 0, "fe.field.followers")
  return true
end

-- What this module installed on fe.field, so a reload replaces its own
-- functions and leaves a game's replacements alone.
local installed = {}

--- Give `field` the follower functions it does not have yet (or had from
--- this module) and connect fe.follower's signals.
---@param field table the fe.field module
function followers.install(field)
  local functions = { spawn_followers = followers.spawn, refresh_followers = followers.refresh,
    followers = followers.list, update_followers = followers.update, follower_sprites = followers.sprites,
    follower_goal = followers.goal, gather_followers = followers.gather }
  for name, fn in pairs(functions) do
    if field[name] == nil or field[name] == installed[name] then
      field[name] = fn
      installed[name] = fn
    end
  end
  local following = require("fe.follower")
  following.joined:connect(function() followers.refresh() end, 0, "fe.field.followers")
  following.left:connect(function() followers.refresh() end, 0, "fe.field.followers")
  followers.watch_party()
end

--- Install the fresh functions after a hot reload of this file.
function followers.__reload(live, fresh)
  live.file_defaults.settings = tablex.adopt(live.settings, fresh.settings, live.file_defaults.settings)
  live.install(require("fe.field"))
end

return followers
