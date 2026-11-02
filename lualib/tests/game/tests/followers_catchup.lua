-- fe.field.followers: loose formation band, off-camera catch-up, gather
-- and screen-space separation.
local field = require("fe.field")
local followers = require("fe.field.followers")
local input = require("fe.input")
local scene = require("fe.scene")
local state = require("fe.state")
local task = require("fe.task")
local vec = require("fe.vec")

local walker = "sprites/walker.anim.json"
local saved

local function start(count)
  input.use_defaults()
  state.reset()
  scene.clear()
  field.reset()
  saved = { field.follower_sprites, field.settings.followers, field.settings.player_sprite, field.settings.autosave,
    require("fe.tablex").deep_copy(followers.settings) }
  field.follower_sprites = function() return { walker, walker, walker } end
  field.settings.followers = count
  field.settings.player_sprite = walker
  field.settings.autosave = false
  field.start("trail", "start")
end

local function finish()
  field.follower_sprites, field.settings.followers, field.settings.player_sprite, field.settings.autosave =
    saved[1], saved[2], saved[3], saved[4]
  for key in pairs(followers.settings) do followers.settings[key] = nil end
  for key, value in pairs(saved[5]) do followers.settings[key] = value end
  scene.clear()
end

-- Put the leader at x on the middle row with a straight trail back to
-- `back_to`, followers on it, and let the camera settle there.
local function stand(t, x, back_to)
  field.player:place({ x = x, z = 6.5 }, 0)
  followers.reset_trail({ { x = back_to, z = 6.5 } })
  field.rig:snap()
  t.tick(2)
end

local function distance(a, b) return vec.flat_distance(a.position, b.position) end

return {
  ["followers in a band stand still until the leader is far enough"] = function(t)
    start(2)
    followers.settings.band = { min = 1.5, max = 3 }
    field.player:place({ x = 4.5, z = 6.5 }, 0)
    t.tick(2)
    fe.input.feed("key:Right", 1)
    t.wait_until(function() return field.player.position.x > 9.5 end, 400)
    fe.input.feed("key:Right", 0)
    t.tick(60)
    local first, second = field.followers()[1], field.followers()[2]
    local d1, d2 = distance(first, field.player), distance(second, field.player)
    t.ok(d1 >= 1.5 and d1 <= 3.2, "the first follower trails within the band (" .. d1 .. ")")
    t.ok(d2 > d1 and d2 <= 3.2 + field.settings.follower_spacing, "the next one a spacing further (" .. d2 .. ")")
    local before = first.position
    fe.input.feed("key:Left", 1)
    t.tick(12)
    fe.input.feed("key:Left", 0)
    t.tick(30)
    t.near(first.position.x, before.x, 0.05, "a step towards it does not move the formation")
    finish()
  end,

  ["a distant follower is moved off camera to the trail, never on screen"] = function(t)
    start(1)
    followers.settings.catchup = true
    stand(t, 40.5, 28.5)
    local follower = field.followers()[1]
    t.ok(followers.on_screen(field.player.position), "the leader is on screen")
    follower:place({ x = 4.5, z = 6.5 }, 0)
    t.tick(1)
    local d = distance(follower, field.player)
    t.ok(d < field.settings.follower_spacing + 12, "it was brought back towards the leader (" .. d .. ")")
    t.ok(d > 3, "to the trail behind, not next to the leader")
    t.ok(not followers.on_screen(follower.position), "where the camera does not see it")
    t.wait_until(function() return distance(follower, field.player) < 1.2 end, 300, "then it walks in")
    finish()
  end,

  ["a stuck follower catches up after the stuck time; one in view waits"] = function(t)
    start(1)
    followers.settings.catchup = true
    stand(t, 21.5, 12.5)
    local follower = field.followers()[1]
    follower:mover({ collide = true }) -- field followers pass through walls; a game's may not
    follower:place({ x = 10.5, z = 6.5 }, 0)
    t.tick(1)
    t.ok(not followers.on_screen(follower.position))
    t.tick(200)
    t.ok(follower.position.x < 11, "blocked by the wall, it waits for the stuck time")
    t.wait_until(function() return follower.position.x > 11 end, 400, "after five seconds stuck it is moved")
    t.ok(distance(follower, field.player) < 11)
    -- In view: a stuck follower is left alone.
    stand(t, 14.5, 12.5)
    follower:place({ x = 10.5, z = 6.5 }, 0)
    t.tick(1)
    t.ok(followers.on_screen(follower.position))
    t.tick(400)
    t.ok(follower.position.x < 11, "never teleported on screen")
    finish()
  end,

  ["gather brings every follower next to the leader"] = function(t)
    start(2)
    stand(t, 30.5, 20.5)
    for _, follower in ipairs(field.followers()) do follower:place({ x = 21.5, z = 2.5 }, 0) end
    local job = task.spawn(function() followers.gather({ timeout = 6 }) end)
    t.wait_until(function() return job.status ~= "running" end, 600)
    for _, follower in ipairs(field.followers()) do
      t.ok(distance(follower, field.player) < 2, "gathered behind the leader")
    end
    for _, follower in ipairs(field.followers()) do follower:place({ x = 12.5, z = 9.5 }, 0) end
    followers.gather({ instant = true })
    for _, follower in ipairs(field.followers()) do t.ok(distance(follower, field.player) < 2) end
    finish()
  end,

  ["separation nudges followers whose sprites would overlap"] = function(t)
    start(1)
    field.player:place({ x = 4.5, z = 6.5 }, 0)
    t.tick(2)
    fe.input.feed("key:Right", 1)
    t.wait_until(function() return field.player.position.x > 9.5 end, 400)
    fe.input.feed("key:Right", 0)
    t.tick(60)
    local plain = distance(field.followers()[1], field.player)
    followers.settings.separation = 40
    field.player:place({ x = 4.5, z = 6.5 }, 0)
    followers.reset_trail()
    t.tick(2)
    fe.input.feed("key:Right", 1)
    t.wait_until(function() return field.player.position.x > 9.5 end, 400)
    fe.input.feed("key:Right", 0)
    t.tick(60)
    local spaced = distance(field.followers()[1], field.player)
    t.ok(spaced > plain + 0.2, "the follower keeps further back (" .. plain .. " -> " .. spaced .. ")")
    finish()
  end,
}
