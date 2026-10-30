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
--- Party changes (the Formation page, a tavern): a party module handed to
--- `followers.watch_party(party)` (fe.rpg's `install` does this) has its
--- `changed` signal re-read the leader's sprite and respawn the followers.
--- fe.follower's `joined` and `left` do the same.
local vec = require("fe.vec")

local followers = {}

-- fe.field requires this module while it loads, so it is fetched when needed.
local function field()
  return require("fe.field")
end

-- Lists kept across hot reload are cleared in place, never replaced.
local entities = {}
local trail = {}

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
  local spacing = settings.follower_spacing
  for i, follower in ipairs(entities) do
    local index = math.min(#trail, math.floor(i * spacing / settings.trail_step) + 1)
    local spot = trail[index]
    if spot and vec.flat_distance_in(f.world, spot, follower.position) > settings.follower_settle then
      follower:move_to(spot, { path = false })
    end
  end
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
    followers = followers.list, update_followers = followers.update, follower_sprites = followers.sprites }
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
function followers.__reload(live)
  live.install(require("fe.field"))
end

return followers
