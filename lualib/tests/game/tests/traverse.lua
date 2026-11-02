-- fe.field.traverse: fixed-path traversals with an unseen follower
-- montage, crouch zones and paired actions; also toggled running and
-- game-defined settings.
local config = require("fe.config")
local field = require("fe.field")
local input = require("fe.input")
local scene = require("fe.scene")
local state = require("fe.state")
local task = require("fe.task")
local traverse = require("fe.field.traverse")

local walker = "sprites/walker.anim.json"

local function press(t, key)
  fe.input.feed(key, 1)
  t.tick(1)
  fe.input.feed(key, 0)
  t.tick(2)
end

local saved
local function start(followers)
  input.use_defaults()
  state.reset()
  scene.clear()
  field.reset()
  saved = { field.follower_sprites, field.settings.followers, field.settings.player_sprite, field.settings.autosave }
  field.follower_sprites = function() return { walker, walker } end
  field.settings.followers = followers or 0
  field.settings.player_sprite = walker
  field.settings.autosave = false
  field.start("trail", "start")
end

local function finish()
  field.follower_sprites, field.settings.followers, field.settings.player_sprite, field.settings.autosave =
    saved[1], saved[2], saved[3], saved[4]
  scene.clear()
end

return {
  ["a traversal moves the leader along its path; followers catch up unseen on the far side"] = function(t)
    start(2)
    field.player:place({ x = 9.9, z = 6.5 }, 0)
    field.player:face("east")
    t.tick(2)
    local target, verbs = field.target("confirm")
    t.ok(target and target.id == "gap", "the traversal is in reach")
    t.eq(verbs[1].id, "traverse")
    t.eq(verbs[1].label(target), "Climb over")
    field.player:place({ x = 8.5, z = 6.5 }, 0)
    t.eq(field.target("confirm"), nil, "out of its range")
    field.player:place({ x = 9.9, z = 6.5 }, 0)
    t.tick(1)
    local visible_during, locked_during, clip_during
    press(t, "key:Z")
    t.tick(10)
    locked_during = field.locks > 0
    visible_during = field.followers()[1]:sprite().visible
    clip_during = field.player:sprite().idle_clip
    t.wait_until(function() return field.locks == 0 and field.can_control() end, 200, "the traversal ends")
    t.ok(locked_during, "control is locked during the move")
    t.eq(visible_during, false, "followers are hidden during the montage")
    t.eq(clip_during, "climb", "the leader plays the traversal clip")
    t.near(field.player.position.x, 13.5, 0.01)
    t.near(field.player.position.z, 6.5, 0.01)
    t.eq(field.player:sprite().idle_clip, "idle", "clips are restored")
    for _, follower in ipairs(field.followers()) do
      t.eq(follower:sprite().visible, true)
      t.ok(follower.position.x >= 12 and follower.position.x < 13.5, "placed behind the leader on the far side")
    end
    finish()
  end,

  ["a once-only traversal is used up"] = function(t)
    start(0)
    field.player:place({ x = 9.9, z = 2.5 }, 0)
    field.player:face("east")
    t.tick(1)
    t.eq(field.target("confirm").id, "once_gap")
    press(t, "key:Z")
    t.wait_until(function() return field.locks == 0 and field.can_control() end, 200)
    t.near(field.player.position.x, 12.5, 0.01)
    t.ok(state.flag("traverse:trail:once_gap"))
    field.player:place({ x = 9.9, z = 2.5 }, 0)
    t.tick(1)
    t.eq(field.target("confirm"), nil, "it cannot be used again")
    finish()
  end,

  ["crouch zones swap walk clips and cap the speed until the party leaves"] = function(t)
    start(1)
    field.player:place({ x = 15.2, z = 6.5 }, 0)
    t.tick(2)
    t.eq(field.move_speed(false), field.settings.walk_speed)
    fe.input.feed("key:Right", 1)
    t.wait_until(function() return field.player.position.x > 16.6 end, 300, "walked into the zone")
    t.ok(traverse.crouching())
    t.eq(field.player:sprite().move_clip, "crouch_walk")
    t.eq(field.player:sprite().idle_clip, "crouch_idle")
    t.eq(field.followers()[1]:sprite().move_clip, "crouch_walk", "followers crouch too")
    t.eq(field.move_speed(true), 1.0, "even running is capped")
    t.wait_until(function() return field.player.position.x > 19.4 end, 600, "walked out")
    fe.input.feed("key:Right", 0)
    t.tick(2)
    t.ok(not traverse.crouching())
    t.eq(field.player:sprite().move_clip, "walk")
    t.eq(field.followers()[1]:sprite().move_clip, "walk")
    t.eq(field.move_speed(false), field.settings.walk_speed)
    finish()
  end,

  ["paired actions walk actors to sockets and play their clips together"] = function(t)
    start(1)
    field.player:place({ x = 28.5, z = 6.5 }, 0)
    t.tick(2)
    local partner = field.followers()[1]
    local during
    local job = task.spawn(function()
      traverse.paired({ actors = { field.player, partner }, sockets = { "cart_left", "cart_right" }, clip = "push",
        duration = 0.5 })
    end)
    t.wait_until(function()
      if not during and field.player:sprite().idle_clip == "push" and partner:sprite().idle_clip == "push" then
        t.tick(10)
        during = { player = field.player.position, partner = partner.position, facing = field.player.facing,
          held = require("fe.field.followers").held(partner) }
      end
      return job.status ~= "running"
    end, 900)
    t.eq(job.status, "done")
    t.ok(during, "both actors played the clip at once")
    t.near(during.player.x, 30.5, 0.01)
    t.near(during.player.z, 5.5, 0.01)
    t.near(during.partner.x, 30.5, 0.01, "the follower stays at its socket, off the trail")
    t.near(during.partner.z, 7.5, 0.01)
    t.ok(during.held)
    t.near(during.facing.x, 1, 0.01, "actors face the socket's facing")
    t.ok(not require("fe.field.followers").held(partner), "released afterwards")
    t.eq(field.player:sprite().idle_clip, "idle")
    t.eq(field.locks, 0)
    finish()
  end,

  ["run toggles with the run_toggle setting"] = function(t)
    start(0)
    t.eq(config.get("run_toggle"), false)
    fe.input.feed("key:Left Shift", 1)
    t.tick(1)
    t.ok(field.running(), "held")
    fe.input.feed("key:Left Shift", 0)
    t.tick(1)
    t.ok(not field.running(), "released")
    config.set("run_toggle", true)
    press(t, "key:Left Shift")
    t.ok(field.running(), "a press turns running on")
    t.tick(5)
    t.ok(field.running(), "and it stays on")
    t.eq(field.move_speed(field.running()), field.settings.run_speed)
    press(t, "key:Left Shift")
    t.ok(not field.running(), "the next press turns it off")
    config.set("run_toggle", false)
    config.dirty = false
    finish()
  end,

  ["games define settings with defaults, appliers and settings rows"] = function(t)
    input.use_defaults()
    local applied = {}
    config.define("test_difficulty", 2, function(values) applied[#applied + 1] = values.test_difficulty end,
      { label = "Difficulty", choices = { 1, 2, 3 } })
    t.eq(config.get("test_difficulty"), 2)
    config.set("test_difficulty", 3)
    t.eq(applied, { 3 })
    config.define("test_difficulty", 1)
    t.eq(config.get("test_difficulty"), 3, "a value already set is kept")
    config.define("test_shake", true, nil, { label = "Screen shake" })
    local screen = require("fe.ui.screens.config")
    local job = task.spawn(screen.open)
    t.tick(1)
    local view = scene.top()
    t.eq(view.name, "config")
    press(t, "key:Escape")
    t.eq(job.status, "done")
    local found = {}
    for _, entry in ipairs(config.defined) do found[entry.key] = true end
    t.ok(found.test_shake and found.test_difficulty)
    for i = #config.defined, 1, -1 do
      local key = config.defined[i].key
      if key == "test_shake" or key == "test_difficulty" then
        table.remove(config.defined, i)
        config.values[key] = nil
      end
    end
    config.dirty = false
  end,
}
