-- fe.battle.schedulers.atb: true Wait (hold_queue), holds, the ready list
-- (ready = "hold"), initial gauges, delays, gauge edits and refunds.
local Battle = require("fe.battle.core")
local atb = require("fe.battle.schedulers.atb")
local data = require("fe.data")
local inventory = require("fe.rpg.inventory")
local party = require("fe.rpg.party")
local state = require("fe.state")
local task = require("fe.task")

local function setup()
  if not data.has_table("skills") then require("fe.rpg.database").load() end
  Battle.settings.inventory, Battle.settings.party = inventory, party
  state.reset()
  party.reset()
  inventory.reset()
  party.join("hero")
  party.join("sage")
end

-- A battle of the hero against two slimes where nobody hurts anybody:
-- every action is a logged "wait". Enemies always wait (forced action);
-- the hero's menu is `menu(b)` (default: wait at once). It runs until
-- `b.data.done`.
local function arena(settings, options)
  options = options or {}
  local log = {}
  local rules = {
    actions = { wait = function(battle, action)
      log[#log + 1] = { user = action.user, time = task.time(), action = action }
      if options.on_wait then options.on_wait(battle, action) end
      return {}
    end },
    forced_action = function(_, b)
      if not b:is_party() then return { kind = "wait", user = b } end
    end,
    player_controller = function(_, b)
      if options.menu then return options.menu(b) end
      return { kind = "wait", user = b }
    end,
    outcome = function(battle) if battle.data.done then return "draw" end end,
  }
  local b = Battle({ scheduler = atb.new(settings), random = fe.random.new(4), rules = rules })
  local hero = b:add({ actor = party.actor("hero"), side = "party", position = { slot = 1 } })
  local slime_a = b:add({ enemy = "slime", side = "enemy", position = { slot = 1 } })
  local slime_b = b:add({ enemy = "slime", side = "enemy", position = { slot = 2 } })
  local job = task.spawn(function() b:run() end)
  return b, log, job, hero, slime_a, slime_b
end

local function acted(log, who)
  local n = 0
  for _, entry in ipairs(log) do if entry.user == who then n = n + 1 end end
  return n
end

local function finish(t, b, job)
  b.data.done = true
  t.wait_until(function() return job.status ~= "running" end, 600)
  t.eq(job.status, "done", tostring(job.error))
end

-- Gauges: everyone full at once except slime B, half full. 1 gauge/second.
local function starts(full)
  return function(b) return full[b:name()] or 0 end
end

return {
  ["atb hold_queue: an open menu freezes enemy gauges and queued enemy actions"] = function(t)
    setup()
    local release = false
    local b, log, job, hero, slime_a, slime_b = arena({ mode = "wait", hold_queue = true, rate = function() return 1 end,
      initial = function(x) return x:is_party() and 1 or (x.position.slot == 1 and 1 or 0.5) end }, {
      menu = function(me)
        task.wait_until(function() return release end)
        return { kind = "wait", user = me }
      end })
    t.tick(2)
    t.eq(#b.scheduler.queue, 1, "slime A decided and waits in the queue")
    t.ok(slime_a.data.queued)
    local gauge_b = slime_b.data.gauge
    t.ok(b.scheduler.frozen())
    t.tick(90)
    t.eq(acted(log, slime_a), 0, "the queued action does not start while the menu is open")
    t.eq(slime_b.data.gauge, gauge_b, "enemy gauges hold still")
    release = true
    t.wait_until(function() return acted(log, hero) == 1 end, 60)
    t.eq(log[1].user, slime_a, "the earlier queued action goes first once the menu closes")
    t.eq(log[2].user, hero)
    t.ok(slime_b.data.gauge > gauge_b, "time runs again")
    finish(t, b, job)
  end,

  ["atb without hold_queue: queued actions still play while a wait-mode menu is open"] = function(t)
    setup()
    local release = false
    local b, log, job, _, slime_a, slime_b = arena({ mode = "wait", rate = function() return 1 end,
      initial = function(x) return x:is_party() and 1 or (x.position.slot == 1 and 1 or 0.5) end }, {
      menu = function(me)
        task.wait_until(function() return release end)
        return { kind = "wait", user = me }
      end })
    local gauge_b
    t.wait_until(function() return acted(log, slime_a) == 1 end, 30, "the queued action plays")
    gauge_b = slime_b.data.gauge
    t.tick(30)
    t.eq(slime_b.data.gauge, gauge_b, "gauges still wait for the menu")
    release = true
    finish(t, b, job)
  end,

  ["atb hold: a hold freezes gauges and the queue in any mode, and ends with its task"] = function(t)
    setup()
    local b, log, job, hero, slime_a = arena({ mode = "active", rate = function() return 1 end,
      initial = starts({ Hero = 0.2 }) })
    local scheduler = b.scheduler
    t.tick(2)
    local release = scheduler.hold("tutorial")
    t.ok(scheduler.held("tutorial") and scheduler.held() and not scheduler.held("inspect"))
    local before = hero.data.gauge
    t.tick(120)
    t.eq(hero.data.gauge, before, "no gauge fills while held")
    t.eq(#log, 0)
    -- A queued action waits for the hold too.
    slime_a.data.gauge = 1
    scheduler.set_gauge(slime_a, 1)
    local inner = task.spawn(function()
      local r = scheduler.hold("inspect")
      task.wait(0.2)
      -- ends without releasing: the task's end releases it
      return r
    end)
    release()
    release() -- twice is harmless
    t.ok(scheduler.held("inspect"), "the inner hold is still held")
    t.tick(5)
    t.eq(hero.data.gauge, before, "still frozen by the inner hold")
    t.wait_until(function() return inner.status ~= "running" end, 60)
    t.ok(not scheduler.held(), "the task's hold went with it")
    t.wait_until(function() return acted(log, slime_a) >= 1 end, 120)
    t.ok(hero.data.gauge > before, "time runs again")
    finish(t, b, job)
  end,

  ["atb ready = hold: full player gauges wait in the ready list; a cancelled choice costs nothing"] = function(t)
    setup()
    local menu_calls, answer = 0, nil
    local b, log, job, hero = arena({ mode = "wait", hold_queue = true, ready = "hold",
      rate = function(x) return x:is_party() and 2 or 0 end, initial = starts({}) }, {
      menu = function(me)
        menu_calls = menu_calls + 1
        task.frame()
        if answer then return { kind = "wait", user = me } end
        return nil
      end })
    local sage = b:add({ actor = party.actor("sage"), side = "party", position = { slot = 2 } })
    local scheduler = b.scheduler
    local changes = 0
    scheduler.ready_changed:connect(function() changes = changes + 1 end)
    t.wait_until(function() return #scheduler.ready == 2 end, 120)
    t.eq(scheduler.ready, { hero, sage }, "in the order they filled")
    t.ok(changes >= 2, "ready_changed fires")
    t.tick(300)
    t.eq(menu_calls, 0, "no menu opens by itself")
    t.eq(#log, 0, "a ready battler waits without acting")
    t.eq(hero.data.gauge, 1)
    local mp = hero:resource("mp")
    local result = "pending"
    task.spawn(function() result = scheduler.choose(hero) end)
    t.wait_until(function() return result ~= "pending" end, 60)
    t.eq(result, nil, "the decision was cancelled")
    t.eq(menu_calls, 1)
    t.eq(scheduler.ready, { hero, sage }, "back in the list, in its old place")
    t.eq(hero.data.gauge, 1, "still full: no turn spent")
    t.eq(hero:resource("mp"), mp)
    t.eq(#log, 0)
    -- Choose the second one first, then the first.
    answer = true
    task.spawn(function() result = scheduler.choose(sage) end)
    t.wait_until(function() return acted(log, sage) == 1 end, 60)
    t.eq(result.kind, "wait")
    t.eq(scheduler.ready, { hero })
    t.eq(scheduler.choose(b.battlers[2]), nil, "an enemy is never ready")
    finish(t, b, job)
  end,

  ["atb ready = hold: a forced action does not wait for the player"] = function(t)
    setup()
    local b, log, job, hero = arena({ mode = "wait", ready = "hold",
      rate = function(x) return x:is_party() and 2 or 0 end, initial = starts({}) })
    b:add_rules({ forced_action = function(_, me)
      if me == hero then return { kind = "wait", user = me, forced = true } end
    end })
    t.wait_until(function() return acted(log, hero) >= 1 end, 120)
    t.ok(log[1].action.forced)
    t.eq(#b.scheduler.ready, 0)
    finish(t, b, job)
  end,

  ["atb delay pushes an action back by exactly its seconds; initial may be negative"] = function(t)
    setup()
    local b, log, job, hero, slime_a, slime_b = arena({ mode = "active",
      rate = function(x) return x:is_party() and 0 or 1 end,
      initial = function(x) return x:is_party() and 0 or (x.position.slot == 1 and 0 or -0.25) end })
    local scheduler = b.scheduler
    t.tick(1)
    local start = task.time()
    t.near(scheduler.time_left(slime_b) - scheduler.time_left(slime_a), 0.25, 1e-6, "a negative start delays")
    scheduler.delay(slime_a, 0.5)
    t.near(scheduler.time_left(slime_a) - scheduler.time_left(slime_b), 0.25, 1e-6)
    t.eq(scheduler.time_left(hero), math.huge, "a gauge that does not fill")
    t.ok(scheduler.gauge(slime_a) == 0, "a negative gauge shows as empty")
    t.wait_until(function() return acted(log, slime_a) == 1 end, 200)
    t.eq(log[1].user, slime_b)
    local a_time = log[2].time - start
    t.near(a_time, 1.5, 2 / 60 + 1e-6, "A acts half a second late: " .. a_time)
    t.near(log[2].time - log[1].time, 0.25, 1 / 60 + 1e-6)
    finish(t, b, job)
  end,

  ["atb speed may be a function read every frame; delay follows the current speed"] = function(t)
    setup()
    local speed = 1
    local b, _, job, _, slime_a = arena({ mode = "active", speed = function() return speed end,
      rate = function(x) return x:is_party() and 0 or 1 end, initial = starts({}) })
    local scheduler = b.scheduler
    t.tick(1)
    local g0 = slime_a.data.gauge
    t.tick(6)
    local normal = slime_a.data.gauge - g0
    speed = 0.5
    local g1 = slime_a.data.gauge
    t.tick(6)
    t.near(slime_a.data.gauge - g1, normal / 2, 1e-6, "slow battle applies at once")
    local left = scheduler.time_left(slime_a)
    scheduler.delay(slime_a, 1)
    t.near(scheduler.time_left(slime_a) - left, 1, 1e-6)
    finish(t, b, job)
  end,

  ["atb cancel_queued drops matching queued actions and refills their users"] = function(t)
    setup()
    local release = false
    local b, log, job, _, slime_a, slime_b = arena({ mode = "wait", hold_queue = true, rate = function() return 1 end,
      initial = function() return 1 end }, {
      menu = function(me)
        task.wait_until(function() return release end)
        return { kind = "wait", user = me }
      end })
    local scheduler = b.scheduler
    t.tick(2)
    t.eq(#scheduler.queue, 2, "both slimes queued behind the open menu")
    local first, second = scheduler.queue[1], scheduler.queue[2]
    local removed = scheduler.cancel_queued(function(action) return action.user == slime_a end, 1)
    t.eq(removed, { first })
    t.ok(not slime_a.data.queued)
    t.eq(slime_a.data.gauge, 1, "refunded: full again")
    t.eq(scheduler.queue, { second })
    t.eq(scheduler.cancel_queued(function() return false end), {})
    t.eq(scheduler.cancel_queued(), { second }, "no predicate: every queued action")
    t.eq(slime_b.data.gauge, 0, "the default refill empties the gauge")
    t.tick(30)
    t.eq(#log, 0, "nothing ran while the menu was open")
    release = true
    t.wait_until(function() return acted(log, slime_a) >= 1 end, 60, "the refunded slime decides again")
    for _, entry in ipairs(log) do
      t.ok(entry.action ~= first and entry.action ~= second, "cancelled actions never run")
    end
    finish(t, b, job)
  end,

  ["atb set_gauge below full takes back a queued turn"] = function(t)
    setup()
    local release = false
    local b, log, job, _, slime_a, slime_b = arena({ mode = "wait", hold_queue = true, rate = function() return 1 end,
      initial = function() return 1 end }, {
      menu = function(me)
        task.wait_until(function() return release end)
        return { kind = "wait", user = me }
      end })
    local scheduler = b.scheduler
    t.tick(2)
    t.eq(#scheduler.queue, 2)
    scheduler.set_gauge(slime_a, 0.5)
    t.ok(not slime_a.data.queued)
    t.eq(#scheduler.queue, 1)
    t.eq(slime_a.data.gauge, 0.5)
    scheduler.set_gauge(slime_b, 1)
    t.ok(slime_b.data.queued, "a full gauge keeps its queued action")
    t.near(scheduler.time_left(slime_a), 0.5)
    t.eq(scheduler.time_left(slime_b), 0)
    release = true
    t.wait_until(function() return acted(log, slime_b) == 1 end, 30)
    t.eq(acted(log, slime_a), 0)
    t.wait_until(function() return acted(log, slime_a) == 1 end, 60)
    finish(t, b, job)
  end,

  ["atb drops a queued action whose user was knocked out, without paying for it"] = function(t)
    setup()
    local release = false
    local b = Battle({ scheduler = atb.new({ mode = "wait", hold_queue = true, rate = function() return 1 end,
      initial = function() return 1 end }), random = fe.random.new(2), rules = {
        forced_action = function(battle, me)
          if me.actor and me.actor.id == "sage" then
            return { kind = "skill", user = me, skill = "fire", targets = { battle:side("enemy")[1] } }
          elseif not me:is_party() then
            return { kind = "defend", user = me }
          end
        end,
        player_controller = function(_, me)
          task.wait_until(function() return release end)
          return { kind = "defend", user = me }
        end,
        outcome = function(battle) if battle.data.done then return "draw" end end,
      } })
    b:add({ actor = party.actor("hero"), side = "party", position = { slot = 1 } })
    local sage = b:add({ actor = party.actor("sage"), side = "party", position = { slot = 2 } })
    sage.auto = true
    b:add({ enemy = "slime", side = "enemy", position = { slot = 1 } })
    local actions = {}
    b.events:connect(function(name, payload)
      if name == "action" or name == "defend" then actions[#actions + 1] = payload end
    end)
    local job = task.spawn(function() b:run() end)
    t.tick(2)
    t.ok(sage.data.queued, "the sage's spell waits behind the hero's menu")
    local mp = sage:resource("mp")
    sage:set_resource("hp", 0)
    b:settle()
    release = true
    t.wait_until(function() return #actions >= 3 end, 300, "the slime and the hero act")
    for _, action in ipairs(actions) do t.ok(action.user ~= sage, "the fallen battler's action never plays") end
    t.eq(sage:resource("mp"), mp, "nothing was paid")
    t.ok(not sage.data.queued)
    b.data.done = true
    t.wait_until(function() return job.status ~= "running" end, 600)
    t.eq(job.status, "done", tostring(job.error))
  end,
}
