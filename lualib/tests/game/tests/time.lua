-- Pause, time scales, the post-update phase, world-bound tasks and cutscene
-- fast-forward and skipping.
local cutscene = require("fe.cutscene")
local dialogue = require("fe.dialogue")
local field = require("fe.field")
local game = require("fe.game")
local input = require("fe.input")
local scene = require("fe.scene")
local state = require("fe.state")
local task = require("fe.task")

local function fresh()
  input.use_defaults()
  state.reset()
  scene.clear()
  field.reset()
  game.reset_time()
end

local function press(t, key)
  fe.input.feed(key, 1)
  t.tick(1)
  fe.input.feed(key, 0)
  t.tick(2)
end

return {
  ["game.pause pauses every world, also ones loaded later, until resumed"] = function(t)
    fresh()
    local w = fe.world.load("pause_a", "arena")
    game.pause()
    t.ok(game.paused)
    t.ok(w.paused)
    local later = fe.world.load("pause_b", "arena")
    t.tick(1)
    t.ok(later.paused, "a world loaded while paused is paused")
    local playtime = game.playtime()
    t.tick(5)
    t.eq(game.playtime(), playtime, "playtime stops")
    game.resume()
    t.ok(not game.paused)
    t.ok(not w.paused and not later.paused)
    w.paused = true
    t.tick(2)
    t.ok(w.paused, "a world paused by hand stays paused")
    w.paused = false
    fe.world.unload("pause_a")
    fe.world.unload("pause_b")
  end,

  ["a world pause taken in a task ends with the task"] = function(t)
    fresh()
    local w = fe.world.load("pause_task", "arena")
    local job = task.spawn(function()
      game.pause(w)
      task.wait(10)
    end)
    t.tick(1)
    t.ok(w.paused)
    t.ok(not game.paused, "only that world")
    t.ok(game.world_paused("pause_task"))
    job:cancel()
    t.ok(not w.paused)
    fe.world.unload("pause_task")
  end,

  ["a paused world stops its movers"] = function(t)
    fresh()
    local w = fe.world.load("pause_move", "arena")
    local walker = w:spawn({ position = { x = 2.5, z = 5.5 }, mover = { speed = 2 } })
    walker:move_to({ x = 8.5, z = 5.5 }, { path = false })
    t.tick(10)
    local release = game.pause(w)
    local x = walker.position.x
    t.tick(20)
    t.eq(walker.position.x, x, "no movement while paused")
    release()
    t.tick(10)
    t.ok(walker.position.x > x, "moves again after resuming")
    fe.world.unload("pause_move")
  end,

  ["tasks bound to a world freeze with it and follow its time scale"] = function(t)
    fresh()
    local w = fe.world.load("bound", "arena")
    local done, child_done = false, false
    task.spawn(function()
      task.bind(w)
      task.spawn(function()
        task.wait(0.5)
        child_done = true
      end)
      task.wait(0.5)
      done = true
    end)
    local release = game.pause(w)
    t.tick(60)
    t.ok(not done and not child_done, "frozen while the world is paused")
    release()
    t.tick(25)
    t.ok(not done)
    t.tick(10)
    t.ok(done and child_done, "children inherit the binding")
    done = false
    game.scale_time("half", 0.5, w)
    t.eq(w.time_scale, 0.5)
    task.spawn(function()
      task.bind("bound")
      task.wait(0.5)
      done = true
    end)
    t.tick(45)
    t.ok(not done, "half speed")
    t.tick(25)
    t.ok(done)
    game.scale_time("half", nil)
    t.eq(w.time_scale, 1)
    fe.world.unload("bound")
  end,

  ["game.time_scale applies once: bound tasks run on world time, others on game time"] = function(t)
    fresh()
    local w = fe.world.load("scaled", "arena")
    -- Each sampler records how far its own clock, the world and game time
    -- moved over 60 ticks, measured from inside the task.
    local function sampler(bind)
      local out = {}
      local job = task.detached("sampler", function()
        if bind then task.bind(w) end
        while true do
          local c0, w0, g0 = task.clock(), w.time, task.time()
          for _ = 1, 60 do task.frame() end
          out[#out + 1] = { clock = task.clock() - c0, world = w.time - w0, game = task.time() - g0 }
        end
      end)
      return out, job
    end
    task.ensure(function()
      game.time_scale = 0.5
      t.tick(1)
      t.eq(w.time_scale, 0.5)
      local bound, bound_job = sampler(true)
      local free, free_job = sampler(false)
      t.tick(61)
      t.near(bound[1].world, 0.5, 1e-4, "the world moved half a second")
      t.near(bound[1].clock, bound[1].world, 1e-4, "a bound task's clock is its world's time")
      t.near(bound[1].clock, 0.5, 1e-4, "not 0.25: game.time_scale is not applied twice")
      t.near(free[1].clock, 0.5, 1e-4, "an unbound task runs on game time")
      t.near(free[1].clock, free[1].game, 1e-6)
      -- a world's own scale (hit-stop, fast-forward) multiplies the game's, once
      game.scale_time("slow", 0.5, w)
      t.tick(120) -- the third window lies wholly after the change
      t.near(bound[3].world, 0.25, 1e-4)
      t.near(bound[3].clock, bound[3].world, 1e-4, "still the world's time")
      t.near(free[3].clock, 0.5, 1e-4, "world scales do not reach unbound tasks")
      bound_job:cancel(); free_job:cancel()
    end, function()
      game.scale_time("slow", nil)
      game.time_scale = 1
      fe.world.unload("scaled")
    end)
  end,

  ["a fast-forwarded cutscene's waits run at the world's speed, not faster"] = function(t)
    fresh()
    field.start("arena", "start")
    local ticks, world_time = 0, 0
    local stop = game.updated:connect(function()
      ticks = ticks + 1
      world_time = world_time + (1 / 60) * field.world.time_scale
    end, 0, "test.ticks")
    local start_tick, end_tick, w0, w1
    local job = task.spawn(function()
      cutscene.play(function()
        task.wait(0.2) -- fast-forward is on from here
        start_tick, w0 = ticks, world_time
        task.wait(1.5)
        end_tick, w1 = ticks, world_time
      end, { letterbox = false })
    end)
    fe.input.feed("key:Left Ctrl", 1)
    t.wait_until(function() return job.status ~= "running" end, 400)
    fe.input.feed("key:Left Ctrl", 0)
    stop()
    t.eq(job.status, "done")
    t.ok(math.abs((end_tick - start_tick) - 30) <= 1, "1.5 s at 3x takes 30 ticks, took " .. (end_tick - start_tick))
    t.near(w1 - w0, 1.5, 0.06, "the wait lasted 1.5 s of world time")
    t.tick(1)
    t.eq(field.world.time_scale, 1)
    scene.clear()
  end,

  ["held time scales combine in the order they were set, keep their place when replaced, and expire"] = function(t)
    fresh()
    local w = fe.world.load("scales", "arena") -- world.time_scale is stored as a float
    task.ensure(function()
      game.scale_time("a", 0.1, w)
      local stop = game.hit_stop(10, 0.7, w) -- keyed by a table
      game.scale_time("c", 3, w)
      t.near(w.time_scale, ((1 * 0.1) * 0.7) * 3, 1e-6, "they multiply")
      game.scale_time("a", 0.5, w) -- replacing keeps its place
      t.near(w.time_scale, ((1 * 0.5) * 0.7) * 3, 1e-6)
      stop()
      t.near(w.time_scale, (1 * 0.5) * 3, 1e-6)
      game.hit_stop(0.05, 0.25, w)
      t.near(w.time_scale, ((1 * 0.5) * 3) * 0.25, 1e-6)
      t.tick(5)
      t.near(w.time_scale, (1 * 0.5) * 3, 1e-6, "an expired hit-stop is dropped")
      game.scale_time("a", nil)
      game.scale_time("c", nil)
      t.near(w.time_scale, 1, 1e-6)
    end, function()
      game.reset_time()
      fe.world.unload("scales")
    end)
  end,

  ["hit-stop slows a world for a moment; game.time_scale scales every world"] = function(t)
    fresh()
    local w = fe.world.load("stop", "arena")
    local other = fe.world.load("stop_other", "arena")
    game.hit_stop(0.1, 0.25, w)
    t.near(w.time_scale, 0.25)
    t.eq(other.time_scale, 1, "only the given world")
    t.tick(10)
    t.eq(w.time_scale, 1, "restored afterwards")
    local finish = game.hit_stop(5)
    t.eq(w.time_scale, 0)
    t.eq(other.time_scale, 0)
    finish()
    t.eq(w.time_scale, 1)
    game.time_scale = 0.5
    game.sync_worlds()
    t.eq(other.time_scale, 0.5)
    game.time_scale = 1
    game.sync_worlds()
    t.eq(other.time_scale, 1)
    fe.world.unload("stop")
    fe.world.unload("stop_other")
  end,

  ["post_update runs after update, for scenes and game.post_updated"] = function(t)
    fresh()
    local calls = {}
    scene.push({
      update = function() calls[#calls + 1] = "update" end,
      post_update = function() calls[#calls + 1] = "post_update" end,
    })
    local disconnect = game.post_updated:connect(function() calls[#calls + 1] = "signal" end)
    t.tick(3)
    disconnect()
    t.eq({ calls[1], calls[2], calls[3], calls[4] }, { "update", "post_update", "signal", "update" })
    scene.clear()
  end,

  ["the field menu pauses the field world and its scripts"] = function(t)
    fresh()
    field.start("arena", "start")
    local count = 0
    task.spawn(function()
      task.bind(field.world)
      while true do
        task.wait(1 / 60)
        count = count + 1
      end
    end)
    t.tick(3)
    local job = task.spawn(require("fe.ui.screens.menu").open)
    t.tick(2)
    t.ok(field.world.paused)
    local before = count
    t.tick(10)
    t.eq(count, before, "field scripts stand still")
    press(t, "key:Escape")
    t.tick(1)
    t.eq(job.status, "done")
    t.ok(not field.world.paused)
    t.tick(3)
    t.ok(count > before)
    scene.clear()
  end,

  ["holding skip fast-forwards a cutscene and its world"] = function(t)
    fresh()
    field.start("arena", "start")
    local started, ended
    local job = task.spawn(function()
      cutscene.play(function()
        started = task.time()
        task.wait(1.2)
        ended = task.time()
      end, { letterbox = false })
    end)
    fe.input.feed("key:Left Ctrl", 1)
    t.tick(4)
    t.eq(field.world.time_scale, 3, "the world runs faster")
    t.wait_until(function() return job.status ~= "running" end, 120)
    fe.input.feed("key:Left Ctrl", 0)
    t.tick(1)
    t.ok(ended - started < 0.6, "a 1.2 second wait took " .. (ended - started))
    t.eq(field.world.time_scale, 1)
    scene.clear()
  end,

  ["a skipped cutscene jumps to its end and still changes state"] = function(t)
    fresh()
    field.start("arena", "start")
    local walker = field.world:spawn({ id = "walker", position = { x = 2.5, z = 3.5 }, mover = { speed = 0.5 } })
    local job = task.spawn(function()
      cutscene.play(function()
        dialogue.say("Guide", "First line.", { speed = 0 })
        cutscene.actor(walker):walk_to({ x = 8.5, z = 3.5 }, { path = false })
        task.wait(30)
        state.set_flag("scene_done")
        dialogue.say("Guide", "Last line.")
      end, { skippable = true })
    end)
    t.tick(30)
    t.eq(dialogue.current().line.text, "First line.")
    press(t, "key:Tab")
    t.ok(field.world.paused, "the scene pauses while asking")
    press(t, "key:Z")
    t.wait_until(function() return job.status ~= "running" end, 120)
    t.eq(job.status, "done")
    t.ok(state.flag("scene_done"))
    t.near(walker.position.x, 8.5, 0.01, "the walk arrived")
    t.eq(dialogue.backlog[#dialogue.backlog].text, "Last line.")
    t.eq(field.locks, 0)
    t.wait_until(function() return fe.screen.get().fade[4] == 0 end, 60, "the fade lifts")
    t.eq(fe.screen.get().letterbox, 0)
    t.ok(not field.world.paused)
    scene.clear()
  end,

  ["timelines keep their spacing under hit-stop"] = function(t)
    fresh()
    local w = fe.world.load("timeline", "arena")
    local timeline = require("fe.timeline")
    local fired = {}
    local job = task.spawn(function()
      task.bind(w)
      timeline.play({ { time = 0, command = "mark" }, { time = 0.2, command = "mark" }, { time = 0.4, command = "mark" } },
        { mark = function() fired[#fired + 1] = task.time() end })
    end)
    game.hit_stop(5, 0.25, w)
    t.wait_until(function() return job.status ~= "running" end, 300)
    t.eq(#fired, 3)
    t.near(fired[2] - fired[1], 0.8, 0.05, "0.2 s of world time at quarter speed")
    t.near(fired[3] - fired[2], 0.8, 0.05, "and the next entry waits its turn too")
    game.reset_time()
    fe.world.unload("timeline")
  end,

  ["a nested cutscene leaves the outer skip in place"] = function(t)
    fresh()
    field.start("arena", "start")
    local inner_skipped, after_inner, walked
    local walker = field.world:spawn({ id = "walker", position = { x = 2.5, z = 3.5 }, mover = { speed = 0.5 } })
    local job = task.spawn(function()
      cutscene.play(function()
        dialogue.say("Guide", "Outer.", { speed = 0 })
        task.wait(30)
        cutscene.play(function()
          inner_skipped = task.skipping()
          dialogue.say("Guide", "Inner.")
        end, { letterbox = false })
        after_inner = task.skipping()
        cutscene.actor(walker):walk_to({ x = 8.5, z = 3.5 }, { path = false })
        walked = walker.position.x
      end, { skippable = true, letterbox = false })
    end)
    t.tick(5)
    press(t, "key:Tab")
    press(t, "key:Z")
    t.wait_until(function() return job.status ~= "running" end, 120)
    t.eq(job.status, "done")
    t.ok(inner_skipped, "the inner cutscene runs under the outer skip")
    t.ok(after_inner, "which it leaves in place")
    t.near(walked, 8.5, 0.01, "so the rest of the outer cutscene still jumps to its end")
    t.eq(fe.screen.get().fade[4], 0)
    scene.clear()
  end,

  ["game.paused is true when every loaded world is paused by any means"] = function(t)
    fresh()
    local a = fe.world.load("all_a", "arena")
    local b = fe.world.load("all_b", "arena")
    local others = {}
    for _, name in ipairs(fe.world.list()) do
      if name ~= "all_a" and name ~= "all_b" then others[#others + 1] = fe.world.get(name) end
    end
    for _, w in ipairs(others) do w.paused = true end
    local release_a = game.pause(a)
    b.paused = true
    local announced
    local disconnect = game.pause_changed:connect(function(v) announced = v end)
    t.tick(1)
    t.ok(game.paused, "a hold on one world and the others paused by hand")
    t.eq(announced, true)
    b.paused = false
    t.tick(1)
    t.ok(not game.paused)
    t.eq(announced, false)
    disconnect()
    release_a()
    for _, w in ipairs(others) do w.paused = false end
    fe.world.unload("all_a")
    fe.world.unload("all_b")
  end,

  ["declining the skip prompt plays the cutscene on"] = function(t)
    fresh()
    field.start("arena", "start")
    local job = task.spawn(function()
      cutscene.play(function() dialogue.say(nil, "Watch this.", { speed = 0 }) end, { skippable = true, letterbox = false })
    end)
    t.tick(2)
    press(t, "key:Tab")
    press(t, "key:Down")
    press(t, "key:Z")
    t.eq(dialogue.current().line.text, "Watch this.")
    t.ok(not task.skipping(job))
    press(t, "key:Z")
    t.wait_until(function() return job.status ~= "running" end, 30)
    t.eq(job.status, "done")
    scene.clear()
  end,
}
