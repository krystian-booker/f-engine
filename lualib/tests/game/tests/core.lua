local config = require("fe.config")
local cutscene = require("fe.cutscene")
local data = require("fe.data")
local daynight = require("fe.daynight")
local dialogue = require("fe.dialogue")
local event = require("fe.event")
local field = require("fe.field")
local game = require("fe.game")
local i18n = require("fe.i18n")
local input = require("fe.input")
local quest = require("fe.quest")
local save = require("fe.save")
local scene = require("fe.scene")
local schema = require("fe.schema")
local signal = require("fe.signal")
local state = require("fe.state")
local task = require("fe.task")
local tween = require("fe.tween")
local ui = require("fe.ui")
local worldmap = require("fe.worldmap")

-- Runs `fn`, which must report at least one script error matching
-- `pattern`, then forgets the errors so the test itself passes.
local function expect_errors(t, pattern, fn)
  local before = #fe.dev.errors()
  fn()
  local errors = fe.dev.errors()
  t.ok(#errors > before, "expected a reported error")
  local found = false
  for i = before + 1, #errors do
    if errors[i].message:find(pattern, 1, true) then found = true end
  end
  t.ok(found, "no reported error mentions '" .. pattern .. "'")
  fe.dev.clear_errors()
end

-- Press and release a key; the extra tick lets tasks woken by the press
-- (which run after the test task in a tick) finish.
local function press(t, key)
  fe.input.feed(key, 1)
  t.tick(1)
  fe.input.feed(key, 0)
  t.tick(2)
end

return {
  ["tasks wait for time and conditions"] = function(t)
    local log = {}
    local flag = false
    local child = task.spawn(function()
      task.wait(0.1)
      log[#log + 1] = "waited"
      task.wait_until(function() return flag end)
      log[#log + 1] = "flag"
    end)
    t.tick(3)
    t.eq(#log, 0)
    t.tick(5)
    t.eq(log, { "waited" })
    flag = true
    t.tick(1)
    t.eq(log, { "waited", "flag" })
    t.eq(child.status, "done")
  end,

  ["task.race cancels the losers; cancelling a parent stops children"] = function(t)
    local slow_finished = false
    local winner = task.race({
      function() task.wait(0.05) end,
      function() task.wait(1); slow_finished = true end,
    })
    t.eq(winner, 1)
    t.tick(80)
    t.ok(not slow_finished)
    local inner
    local outer = task.spawn(function()
      inner = task.spawn(function() task.wait(10) end)
      task.wait(10)
    end)
    outer:cancel()
    t.eq(inner.status, "cancelled")
  end,

  ["finally runs on done, cancel and failure, newest first"] = function(t)
    local log = {}
    local done = task.spawn(function()
      task.finally(function() log[#log + 1] = "first" end)
      task.finally(function() log[#log + 1] = "second" end)
    end)
    t.eq(done.status, "done")
    t.eq(log, { "second", "first" })
    local cancelled = task.spawn(function()
      task.finally(function() log[#log + 1] = "cancelled" end)
      task.wait(10)
    end)
    cancelled:cancel()
    t.eq(log[3], "cancelled")
    local removed = false
    task.spawn(function()
      local forget = task.finally(function() removed = true end)
      forget()
    end)
    t.ok(not removed, "an unregistered cleanup does not run")
    expect_errors(t, "Task 'crash' failed", function()
      local failed = task.named("crash", function()
        task.finally(function() log[#log + 1] = "failed" end)
        error("boom")
      end)
      t.eq(failed.status, "failed")
    end)
    t.eq(log[4], "failed")
  end,

  ["a failing wait predicate fails the task, cancels its children and cleans up"] = function(t)
    local cleaned, child = false, nil
    local job
    expect_errors(t, "predicate broke", function()
      job = task.named("waiter", function()
        child = task.spawn(function() task.wait(10) end)
        task.finally(function() cleaned = true end)
        task.wait_until(function() error("predicate broke") end, 5)
      end)
      t.tick(2)
    end)
    t.eq(job.status, "failed")
    t.ok(tostring(job.error):find("predicate broke"))
    t.eq(child.status, "cancelled")
    t.ok(cleaned)
  end,

  ["children outlive a parent that finishes; detached tasks outlive cancellation"] = function(t)
    local child
    local parent = task.spawn(function()
      child = task.spawn(function() task.wait(0.1) end)
    end)
    t.eq(parent.status, "done")
    t.eq(child.status, "running")
    t.eq(child.parent, task.current(), "handed to the grandparent")
    t.tick(10)
    t.eq(child.status, "done")
    local detached, attached
    local owner = task.spawn(function()
      detached = task.detached("free", function() task.wait(0.1) end)
      attached = task.spawn(function() task.wait(0.1) end)
      task.wait(10)
    end)
    owner:cancel()
    t.eq(attached.status, "cancelled")
    t.eq(detached.status, "running")
    t.eq(detached.parent, nil)
    t.tick(10)
    t.eq(detached.status, "done")
  end,

  ["children handed on keep their parent's world binding, speed and skipping"] = function(t)
    local world = fe.world.load("bound", "arena")
    local child
    local ticks = 0
    task.detached("parent", function()
      task.bind(world)
      task.current().speed = 2
      child = task.spawn(function()
        while true do
          task.wait(0.05)
          ticks = ticks + 1
        end
      end)
    end)
    t.eq(child.parent, nil)
    t.eq(child:bound_world(), "bound")
    local release = game.pause(world)
    local before = ticks
    t.tick(30)
    t.eq(ticks, before, "frozen while the bound world is paused")
    release()
    t.tick(2)
    local clock = child.now
    t.tick(30)
    t.near(child.now - clock, 1, 0.001, "its clock still runs twice as fast")
    child:cancel()
    local skipped
    task.detached("skipper", function()
      task.current().skipping = true
      skipped = task.spawn(function()
        task.frame()
        task.wait(5)
      end)
    end)
    t.ok(task.skipping(skipped))
    t.tick(2)
    t.eq(skipped.status, "done", "its waits still end at once")
    fe.world.unload("bound")
  end,

  ["tasks start under another task and move between parents"] = function(t)
    local world = fe.world.load("moved", "arena")
    local holder = task.detached("holder", function() task.wait_until(function() return false end) end)
    local bound = task.detached("bound", function()
      task.bind(world)
      task.wait(10)
    end)
    local inner = bound:spawn("inner", function() task.wait(10) end)
    t.eq(inner.parent, bound)
    inner:set_parent(holder)
    t.eq(inner.parent, holder)
    t.eq(#bound.children, 0)
    t.eq(inner:bound_world(), "moved", "it keeps the binding it inherited")
    t.error(function() holder:set_parent(inner) end, "under itself")
    bound:cancel()
    t.eq(inner.status, "running")
    holder:cancel()
    t.eq(inner.status, "cancelled", "it ends with its new parent")
    t.error(function() holder:spawn("late", function() end) end, "has ended")
    fe.world.unload("moved")
  end,

  ["a task can cancel itself; cleanup helpers run once"] = function(t)
    local after = false
    local runs = 0
    local job = task.spawn(function()
      task.cleanup(function() runs = runs + 1 end)
      task.current():cancel()
      after = true
    end)
    t.eq(job.status, "cancelled")
    t.ok(not after, "code after a self-cancel never runs")
    t.eq(runs, 1)
    local count = 0
    local run = task.cleanup(function() count = count + 1 end)
    run()
    run()
    t.eq(count, 1)
    local cleaned = 0
    t.error(function()
      task.ensure(function() error("inside") end, function() cleaned = cleaned + 1 end)
    end, "inside")
    t.eq(cleaned, 1)
    local value = task.ensure(function() return 42 end, function() cleaned = cleaned + 1 end)
    t.eq(value, 42)
    t.eq(cleaned, 2)
    local guarded = task.spawn(function()
      task.ensure(function() task.wait(10) end, function() cleaned = cleaned + 1 end)
    end)
    guarded:cancel()
    t.eq(cleaned, 3)
  end,

  ["wait_signal disconnects when cancelled"] = function(t)
    local s = signal.new()
    local waiter = task.spawn(function() task.wait_signal(s) end)
    t.eq(s:count(), 1)
    waiter:cancel()
    t.eq(s:count(), 0)
    task.spawn(function() task.wait(0.05); s:emit(7, 8) end)
    local a, b = task.wait_signal(s)
    t.eq({ a, b }, { 7, 8 })
    t.eq(s:count(), 0)
  end,

  ["task.all and task.race raise a child's failure and cancel the rest"] = function(t)
    local survivor
    t.error(function()
      task.all({
        function() task.wait(0.05); error("all failed") end,
        function() survivor = task.current(); task.wait(10) end,
      })
    end, "all failed")
    t.eq(survivor.status, "cancelled")
    t.error(function()
      task.race({
        function() error("race failed") end,
        function() task.wait(10) end,
      })
    end, "race failed")
    local winner = task.race({
      function() task.wait(0.2) end,
      function() task.wait(0.05) end,
    })
    t.eq(winner, 2)
    local results = {}
    task.all({ function() results[1] = "a" end, function() task.wait(0.05); results[2] = "b" end })
    t.eq(results, { "a", "b" })
  end,

  ["helpers of task.all belong to their caller"] = function(t)
    local me = task.current()
    local owners = {}
    task.all({ function() owners[1] = task.current():owner() end, function() owners[2] = task.current():owner() end })
    t.eq(owners[1], me)
    t.eq(owners[2], me)
    local spawned = task.spawn(function() task.wait(1) end)
    t.eq(spawned:owner(), spawned)
  end,

  ["signals pass values and can stop propagation"] = function(t)
    local s = signal.new()
    local seen = {}
    s:connect(function(v) seen[#seen + 1] = "low" .. v end)
    s:connect(function(v) seen[#seen + 1] = "high" .. v; return v == 2 end, 10)
    s:emit(1)
    t.ok(s:emit(2))
    t.eq(seen, { "high1", "low1", "high2" })
    task.spawn(function() task.wait(0.05); s:emit(7) end)
    t.eq(task.wait_signal(s), 7)
  end,

  ["tweens reach their goal"] = function(t)
    local box = { x = 0 }
    task.join(tween.to(box, { x = 10 }, 0.2, "out_cubic"))
    t.eq(box.x, 10)
    t.near(tween.approach(0, 10, 1e9, 1), 10)
  end,

  ["state sections snapshot and restore in place"] = function(t)
    local inventory = state.section("test_inventory", function() return { potion = 1 } end)
    state.set_flag("met_elder")
    state.set_var("chapter", 2)
    local snapshot = state.snapshot()
    inventory.potion = 5
    state.set_flag("met_elder", false)
    state.restore(snapshot)
    t.eq(inventory.potion, 1)
    t.ok(state.flag("met_elder"))
    t.eq(state.var("chapter"), 2)
  end,

  ["state rejects values that cannot be saved, when they are set"] = function(t)
    t.error(function() state.set_var("callback", function() end) end, "vars.callback: function")
    t.error(function() state.set_var("sparse", { [1] = "a", [3] = "c" }) end, "holes")
    t.error(function() state.set_var("mixed", { 1, name = "x" }) end, "mix")
    t.error(function() state.set_var("inf", { value = math.huge }) end, "vars.inf.value")
    t.error(function() state.set_flag("odd", "yes") end, "boolean")
    state.set_var("fine", { list = { 1, 2 }, name = "ok", nested = { deep = true } })
    t.eq(state.var("fine").list[2], 2)
    local section = state.section("test_bad")
    section.handler = function() end
    t.error(function() save.write("test_bad") end, "state.test_bad.handler")
    section.handler = nil
  end,

  ["flags only announce real changes; restore and reset announce a reload"] = function(t)
    state.reset()
    local seen = {}
    local disconnect = state.changed:connect(function(kind, name) seen[#seen + 1] = kind .. ":" .. tostring(name) end)
    state.set_flag("never_set", false)
    state.set_flag("door_open")
    state.set_flag("door_open")
    state.restore(state.snapshot())
    state.reset()
    disconnect()
    t.eq(seen, { "flag:door_open", "reload:nil", "reload:nil" })
  end,

  ["unknown save sections never become new-game defaults"] = function(t)
    state.reset()
    local snapshot = state.snapshot()
    snapshot.from_the_future = { secret = 7 }
    state.restore(snapshot)
    t.eq(state.snapshot().from_the_future, { secret = 7 }, "kept for the next save")
    state.reset()
    t.eq(state.snapshot().from_the_future, nil, "dropped by a new game")
    state.restore(snapshot)
    local adopted = state.section("from_the_future", function() return { secret = 0 } end)
    t.eq(adopted.secret, 7, "a kit registering it later gets the loaded data")
    state.reset()
    t.eq(adopted.secret, 0)
  end,

  ["playtime is saved without announcing state changes"] = function(t)
    local entry = game.main({ start = function() end })
    local before = game.playtime()
    local changes = 0
    local disconnect = state.changed:connect(function() changes = changes + 1 end)
    entry.update(0.5)
    disconnect()
    t.near(game.playtime() - before, 0.5, 1e-9)
    t.eq(changes, 0)
    t.ok(state.snapshot().clock.playtime >= 0.5)
  end,

  ["saves migrate older versions"] = function(t)
    state.set_var("gold_coins", 3)
    save.version = 1
    save.write("test_slot")
    save.version = 2
    save.migrations[1] = function(d) d.state.vars.migrated = true; return d end
    state.reset()
    t.ok(save.load("test_slot"))
    t.eq(state.var("gold_coins"), 3)
    t.eq(state.var("migrated"), true)
    save.version = 1
    save.migrations = {}
    save.delete("test_slot")
  end,

  ["schemas validate, fill defaults and collect references"] = function(t)
    schema.define("thing", {
      id = "string",
      count = { type = "integer", min = 0, default = 1 },
      kind = { enum = { "a", "b" } },
      mood = { enum = { "calm", "angry" }, optional = true },
      friend = { type = { ref = "things" }, optional = true },
      tags = { type = { list = "string" }, default = {} },
    })
    local good = { id = "x", kind = "a", friend = "y" }
    local ok, errors, refs = schema.validate("thing", good)
    t.ok(ok, table.concat(errors, "; "))
    t.eq(good.count, 1)
    t.eq(refs[1].id, "y")
    local bad_ok, bad_errors = schema.validate("thing", { id = 3, kind = "z", count = -1, mood = "sleepy" })
    t.ok(not bad_ok)
    t.eq(#bad_errors, 4)
    t.ok(schema.export().schemas.thing.fields.kind.options[2] == "b")
    t.eq(schema.export().schemas.thing.fields.mood.optional, true)
    local _, hole_errors = schema.validate("thing", { id = "h", kind = "a", tags = { [1] = "a", [3] = "c" } })
    t.eq(#hole_errors, 1)
    t.ok(hole_errors[1]:find("thing.tags%[2%]"), hole_errors[1])
  end,

  ["i18n.number formats with the current language's separators"] = function(t)
    local language = i18n.language
    i18n.language = nil
    t.eq(i18n.number(1234567), "1,234,567") -- the system locale, en-US in tests
    t.eq(i18n.number(1234.5), "1,234.50")
    t.eq(i18n.number(1234.5, { decimals = 1 }), "1,234.5")
    i18n.set_language("de")
    t.eq(i18n.number(1234567.891, { decimals = 2 }), "1.234.567,89")
    t.eq(i18n.number(1234567.891, { decimals = 2, locale = "en" }), "1,234,567.89")
    i18n.set_language("fr-CA")
    t.eq(i18n.number(1000), "1\u{a0}000")
    i18n.language = language
    t.eq(fe.app.format_number(0.5, { locale = "ja" }), "0.50")
  end,
  ["i18n fills underscore placeholders and falls back to kit defaults"] = function(t)
    require("fe.ui.screens.title")
    i18n.defaults({ test = { greeting = "Hello {first_name}, {unknown_key}" } })
    t.eq(i18n.t("test.greeting", { first_name = "Ada" }), "Hello Ada, {unknown_key}")
    t.eq(i18n.t("fe.title.new"), "New Game")
    t.eq(i18n.t("Plain label"), "Plain label")
  end,

  ["saved bindings for unknown actions are skipped; movement follows direction bindings"] = function(t)
    input.use_defaults()
    local failed = input.apply_bindings({ renamed_action = { "key:K" }, confirm = { "key:K" } })
    t.eq(#failed, 1)
    t.eq(failed[1].action, "renamed_action")
    t.eq(fe.input.bindings("confirm"), { "key:K" })
    config.bind("left", { "key:J" })
    fe.input.feed("key:J", 1)
    t.tick(1)
    t.eq(fe.input.axis("move_x"), -1)
    fe.input.feed("key:J", 0)
    fe.input.feed("key:Left", 1)
    t.tick(1)
    t.eq(fe.input.axis("move_x"), 0, "the old key no longer moves")
    fe.input.feed("key:Left", 0)
    config.reset_bindings()
    config.dirty = false
    t.eq(fe.input.bindings("left")[1], "key:Left")
  end,

  ["settings are written once, not on every change"] = function(t)
    local before = fe.save.read_config().music
    config.set("music", 0.25)
    config.set("music", 0.3)
    t.eq(fe.save.read_config().music, before)
    config.save()
    t.eq(fe.save.read_config().music, 0.3)
    config.set("music", before or 0.7)
    config.save()
  end,

  ["screens are removed when their task is cancelled"] = function(t)
    scene.clear()
    local job = task.spawn(function() ui.choose({ "a", "b" }, { cancel = true }) end)
    t.tick(1)
    t.ok(scene.find("menu") ~= nil)
    job:cancel()
    t.tick(1)
    t.eq(scene.find("menu"), nil)
    input.use_defaults()
    local chosen
    job = task.spawn(function() chosen = { ui.choose({ "a", "b" }, { cancel = 2 }) } end)
    t.tick(1)
    press(t, "key:Escape")
    t.eq(chosen[1], 2)
    t.eq(scene.size(), 0)
  end,

  ["dialogue lines from different tasks queue; boxes close with their task"] = function(t)
    input.use_defaults()
    scene.clear()
    local first = task.spawn(function() dialogue.say("A", "one", { speed = 0 }) end)
    local second = task.spawn(function() dialogue.say("B", "two", { speed = 0 }) end)
    t.tick(2)
    t.eq(dialogue.current().line.text, "one")
    press(t, "key:Z")
    t.eq(first.status, "done")
    t.eq(dialogue.current().line.text, "two")
    press(t, "key:Z")
    t.tick(1)
    t.eq(second.status, "done")
    t.eq(dialogue.current(), nil)
    t.eq(scene.size(), 0)
    t.eq(dialogue.backlog[#dialogue.backlog].text, "two")
  end,

  ["a text speed of 0 reveals the rest of a line at once"] = function(t)
    local box = setmetatable({ line = {}, units = { count = 12, wait = {}, speed = { [2] = 0 } }, visible = 0, timer = 0,
      speed = 30 }, dialogue.Box)
    box:reveal(1 / 60)
    t.eq(box.visible, 0)
    box:reveal(1 / 60)
    t.eq(box.visible, 1)
    box:reveal(1 / 30)
    t.eq(box.visible, 12, "everything after the speed-0 point is visible")
    t.ok(box.finished)
    scene.clear()
    local job = task.spawn(function() dialogue.say(nil, "instant line", { speed = 0 }) end)
    t.tick(1)
    t.ok(dialogue.current().finished)
    job:cancel()
    t.eq(dialogue.current(), nil, "the box closes with its task")
    t.tick(1)
    t.eq(scene.size(), 0)
  end,

  ["the backlog screen opens and closes"] = function(t)
    input.use_defaults()
    scene.clear()
    local job = task.spawn(require("fe.ui.screens.backlog").open)
    t.tick(1)
    t.ok(scene.find("backlog") ~= nil)
    press(t, "key:Escape")
    t.eq(job.status, "done")
    t.eq(scene.find("backlog"), nil)
  end,

  ["cutscenes lock control and clean up when cancelled"] = function(t)
    input.use_defaults()
    field.reset()
    local job = task.spawn(function()
      cutscene.play(function() task.wait(10) end)
    end)
    t.tick(30)
    t.ok(field.locks > 0, "locked during the cutscene")
    t.ok(fe.screen.get().letterbox > 0.9)
    job:cancel()
    t.eq(field.locks, 0)
    t.eq(fe.screen.get().letterbox, 0)
    local world = fe.world.load("emotes", "arena")
    local actor = cutscene.actor(world:spawn({ id = "talker", position = { x = 3.5, z = 5.5 } }), world)
    local count = #world:query({ near = { x = 3.5, z = 5.5 }, radius = 5 })
    job = task.spawn(function() actor:emote("exclaim", 5) end)
    t.eq(#world:query({ near = { x = 3.5, z = 5.5 }, radius = 5 }), count + 1)
    job:cancel()
    t.eq(#world:query({ near = { x = 3.5, z = 5.5 }, radius = 5 }), count)
    fe.world.unload("emotes")
  end,

  ["camera pans turn the short way and end with their task"] = function(t)
    local world = fe.world.load("pan", "arena")
    local rig = require("fe.camera").rig(world, { point = { x = 3, z = 3 }, yaw = 350 })
    local lowest, highest = 1e9, -1e9
    local job = task.spawn(function() rig:pan_to({ x = 5, z = 5 }, 0.2, { yaw = 10 }) end)
    for _ = 1, 15 do
      t.tick(1)
      lowest, highest = math.min(lowest, rig.cam.yaw), math.max(highest, rig.cam.yaw)
    end
    t.eq(job.status, "done")
    t.ok(lowest >= 349 and highest <= 371, "yaw stayed between 350 and 370")
    t.near(rig.cam.yaw % 360, 10, 1e-3)
    t.ok(rig.override == nil or not rig.override:running())
    rig:update(1 / 60)
    t.eq(rig.override, nil, "the hold ended with its task")
    fe.world.unload("pan")
  end,

  ["quest objectives track progress and announce changes"] = function(t)
    state.reset()
    data.load("quests", "data/quests.json", "quest")
    local seen = {}
    local disconnect = quest.changed:connect(function(id, what, detail) seen[#seen + 1] = what .. ":" .. tostring(detail) end)
    local changes = 0
    local disconnect_state = state.changed:connect(function(kind) if kind == "quest" then changes = changes + 1 end end)
    quest.start("relics")
    t.ok(not quest.stage_done("relics"))
    t.ok(not quest.progress("relics", "shards"))
    t.ok(not quest.progress("relics", "shards"))
    t.ok(quest.progress("relics", "shards", 5), "capped at the count and complete")
    t.eq(quest.objectives("relics")[1].progress, 3)
    t.ok(event.matches({ when = { objective = { relics = "shards" } } }, {}))
    t.ok(not event.matches({ when = { objective = { relics = "key" } } }, {}))
    quest.complete_objective("relics", "key")
    t.ok(quest.stage_done("relics"))
    quest.advance("relics")
    t.ok(event.matches({ when = { quest_stage = { relics = "return" }, quest = { relics = "active" } } }, {}))
    t.eq(#quest.objectives("relics"), 0)
    t.error(function() quest.progress("relics", "shards") end, "no objective")
    disconnect()
    disconnect_state()
    t.eq(seen, { "active:nil", "objective:shards", "objective:shards", "objective:shards", "objective:key", "stage:return" })
    t.eq(changes, #seen)
    local snapshot = state.snapshot()
    t.eq(snapshot.quests.quests.relics.stage, 2)
    state.reset()
  end,

  ["unknown quests and stages raise instead of changing progress"] = function(t)
    state.reset()
    data.load("quests", "data/quests.json", "quest")
    quest.start("relics")
    t.error(function() quest.advance("relics", "no_such_stage") end, "no stage 'no_such_stage'")
    t.eq(state.snapshot().quests.quests.relics.stage, 1, "the stage is unchanged")
    t.error(function() quest.advance("not_a_quest") end, "not_a_quest")
    t.error(function() quest.complete("not_a_quest") end, "not_a_quest")
    t.error(function() quest.fail("not_a_quest") end, "not_a_quest")
    t.eq(quest.status("not_a_quest"), "unknown")
    t.eq(quest.advance("relics", "return"), true)
    t.eq(quest.stage("relics").id, "return")
    state.reset()
  end,

  ["day and night: hours, LUT blending and re-applying on map entry"] = function(t)
    state.reset()
    local hours = {}
    local disconnect = state.changed:connect(function(kind, _, value) if kind == "hour" then hours[#hours + 1] = value end end)
    daynight.set_hour(22)
    t.eq(daynight.hour(), 22)
    t.ok(event.matches({ when = { hours = { 20, 4 } } }, {}))
    t.ok(not event.matches({ when = { hours = { 6, 18 } } }, {}))
    disconnect()
    t.eq(hours, { 22 })
    t.near(daynight.sample(12).lut_blend, 0, 1e-6)
    t.near(daynight.sample(3).lut_blend, 0.75, 1e-6)
    local applied
    local stub = {
      props = function() return { daynight = true } end,
      set_environment = function(_, environment) applied = environment end,
    }
    local real = field.world
    field.world = stub
    daynight.luts = { day = "ui/day.cube", night = "ui/night.cube" }
    field.entered:emit("stub", "start")
    daynight.luts = nil
    field.world = real
    t.ok(applied ~= nil, "applied on map entry with the clock stopped")
    t.near(applied.post.lut_blend, daynight.sample(22).lut_blend, 1e-6)
    t.eq(applied.post.lut_blend_target, "ui/night.cube")
    state.reset()
  end,

  ["worldmap.discover works before the map is opened"] = function(t)
    state.reset()
    worldmap.definition = nil
    worldmap.discover("arena")
    t.ok(state.flag("found_arena"))
    t.eq(worldmap.places()[1].id, "arena")
    state.reset()
  end,

  ["scenes stack, defer changes and block updates"] = function(t)
    local calls = {}
    local base = { update = function() calls[#calls + 1] = "base" end }
    local overlay = { blocks_update = false, update = function() calls[#calls + 1] = "overlay" end }
    local modal = {
      update = function(self)
        calls[#calls + 1] = "modal"
        scene.pop()
      end,
    }
    scene.switch(base)
    scene.push(overlay)
    scene.update(0)
    t.eq(calls, { "base", "overlay" })
    calls = {}
    scene.push(modal)
    scene.update(0)
    t.eq(calls, { "modal" })
    t.eq(scene.top(), overlay)
    scene.clear()
  end,

  ["every core kit module loads"] = function(t)
    for _, name in ipairs({ "fe.camera", "fe.class", "fe.config", "fe.cutscene", "fe.daynight", "fe.data", "fe.devtools",
      "fe.dialogue", "fe.event", "fe.field", "fe.field.placements", "fe.field.followers", "fe.follower", "fe.game", "fe.i18n", "fe.input", "fe.quest", "fe.save", "fe.scene",
      "fe.schema", "fe.signal", "fe.sound", "fe.starter", "fe.state", "fe.story", "fe.tablex", "fe.task", "fe.test", "fe.timeline",
      "fe.transition", "fe.tween", "fe.vec", "fe.worldmap", "fe.ui", "fe.ui.theme", "fe.ui.screens.backlog",
      "fe.ui.screens.config", "fe.ui.screens.menu", "fe.ui.screens.name_entry", "fe.ui.screens.save",
      "fe.ui.screens.shop", "fe.ui.screens.title" }) do
      t.ok(require(name) ~= nil, name)
    end
  end,

  ["name entry returns the name, or nil when backed out"] = function(t)
    input.use_defaults()
    scene.clear()
    local screen = require("fe.ui.screens.name_entry")
    local name
    local job = task.spawn(function() name = screen.open("Al", 4) end)
    t.tick(1)
    press(t, "key:Z")   -- the cursor starts on "A"
    t.ok(job:running())
    press(t, "key:Left") -- wraps to "OK", the last entry
    press(t, "key:Z")
    t.eq(job.status, "done")
    t.eq(name, "AlA")
    job = task.spawn(function() name = screen.open("") end)
    t.tick(1)
    press(t, "key:Escape")
    t.eq(job.status, "done")
    t.eq(name, nil)
  end,

  ["data tables load from JSON"] = function(t)
    t.ok(not data.has_table("nothing"))
    t.error(function() data.get("nothing", "x") end, "not loaded")
  end,

  ["task.ensure keeps the traceback of where an error was raised and passes table errors on"] = function(t)
    local job
    expect_errors(t, "Task 'ensured' failed", function()
      job = task.named("ensured", function()
        task.ensure(function()
          task.ensure(function()
            error("deep trouble")       -- the location this line reports
          end, function() end)
        end, function() end)
      end)
    end)
    t.eq(job.status, "failed")
    t.ok(tostring(job.error):find("deep trouble", 1, true) ~= nil)
    local first = job.traceback:match("tests/core%.lua:(%d+)")
    t.ok(first ~= nil, "the traceback names this file: " .. job.traceback)
    local error_line = tonumber(first)
    local ensure_line = job.traceback:match("task%.lua:(%d+): in function <lualib/fe/task%.lua:%d+>")
    t.ok(not job.traceback:find("in function 'ensure'", 1, true) or
      tonumber(job.traceback:match("tests/core%.lua:(%d+)")) == error_line, "the first frame of this file is the raise")
    local marker = { code = 42 }
    local ok, err = pcall(task.ensure, function() error(marker) end, function() end)
    t.ok(not ok)
    t.eq(err, marker, "a table error value travels on unchanged")
    t.eq(task.ensure(function() return 1, nil, 3 end, function() end), 1)
    t.eq(select("#", task.ensure(function() return 1, nil, 3 end, function() end)), 3)
  end,

  ["moving a task under an ancestor does not count the shared speed twice"] = function(t)
    local root = task.detached("root", function() task.wait_until(function() return false end) end)
    root.speed = 2
    local middle = root:spawn("middle", function() task.wait_until(function() return false end) end)
    local leaf = middle:spawn("leaf", function() task.wait_until(function() return false end) end)
    leaf:set_parent(root)
    t.eq(leaf.parent, root)
    local before = leaf.now
    t.tick(30)
    t.near(leaf.now - before, 1, 0.05, "twice game time, not four times")
    middle.speed = 3
    local other = middle:spawn("other", function() task.wait_until(function() return false end) end)
    other:set_parent(root)
    before = other.now
    t.tick(30)
    t.near(other.now - before, 3, 0.11, "it carries the speed of the ancestor it left and still inherits the root's")
    root:cancel()
  end,

  ["movement tuning is read and set per world"] = function(t)
    local world = fe.world.load("tuning", "arena")
    local other = fe.world.load("tuning_other", "arena")
    local tuning = world:movement_tuning()
    t.near(tuning.arrive_distance, 0.05, 1e-6)
    t.near(tuning.jump_arc_per_rise, 0.5, 1e-6)
    world:set_movement_tuning({ arrive_distance = 0.1, stuck_seconds = 2 })
    tuning = world:movement_tuning()
    t.near(tuning.arrive_distance, 0.1, 1e-6)
    t.eq(tuning.stuck_seconds, 2)
    t.near(tuning.snap_distance, 0.02, 1e-6, "omitted fields keep their value")
    t.near(other:movement_tuning().arrive_distance, 0.05, 1e-6, "another world keeps the defaults")
    t.error(function() world:set_movement_tuning({ stuck_share = 2 }) end, "stuck_share must be from 0 to 1")
    t.error(function() world:set_movement_tuning({ arrive_distance = 0 }) end, "arrive_distance must be from")
    t.near(world:movement_tuning().stuck_share, 0.1, 1e-6, "a refused change leaves everything as it was")
    fe.world.unload("tuning")
    fe.world.unload("tuning_other")
  end,

  ["config.set pushes only the changed setting and ignores equal values"] = function(t)
    local volumes, displays = {}, 0
    local saved_volume, saved_display = fe.audio.set_bus_volume, fe.app.set_display
    fe.audio.set_bus_volume = function(bus, v) volumes[#volumes + 1] = bus .. "=" .. v end
    fe.app.set_display = function() displays = displays + 1 end
    local before = config.get("music")
    local changes = 0
    local disconnect = config.changed:connect(function() changes = changes + 1 end)
    local ok, err = pcall(function()
      config.dirty = false
      config.set("music", 0.42)
      t.eq(volumes, { "music=0.42" }, "only the music bus")
      t.eq(displays, 0, "the window is left alone")
      t.ok(config.dirty)
      config.dirty = false
      config.set("music", 0.42)
      t.ok(not config.dirty, "an equal value changes nothing")
      t.eq(changes, 1)
      config.set("display", {})
      t.eq(displays, 0, "an equal table changes nothing either")
      config.set("fullscreen", config.get("fullscreen"))
      t.eq(displays, 0)
    end)
    fe.audio.set_bus_volume, fe.app.set_display = saved_volume, saved_display
    disconnect()
    config.set("music", before)
    config.dirty = false
    if not ok then error(err, 0) end
  end,

  ["quest.advance past the last stage is a no-op"] = function(t)
    state.reset()
    data.load("quests", "data/quests.json", "quest")
    quest.start("relics")
    quest.progress("relics", "shards", 3)
    local last = #data.get("quests", "relics").stages
    quest.advance("relics", data.get("quests", "relics").stages[last].id)
    local seen = {}
    local disconnect = quest.changed:connect(function(_, what) seen[#seen + 1] = what end)
    t.eq(quest.advance("relics"), false)
    disconnect()
    t.eq(seen, {}, "no stage change announced")
    t.eq(quest.stage("relics").id, data.get("quests", "relics").stages[last].id)
    state.reset()
  end,

  ["camera rotate snaps to the next step in either direction"] = function(t)
    local world = fe.world.load("rotate", "arena")
    local rig = require("fe.camera").rig(world, { point = { x = 3, z = 3 } })
    rig:rotate(90, true)
    t.eq(rig.yaw_offset, 90)
    rig:rotate(-90, true)
    t.eq(rig.yaw_offset, 0)
    rig:rotate(-90, true)
    t.eq(rig.yaw_offset, -90)
    rig:rotate(-90, true)
    t.eq(rig.yaw_offset, -180, "a negative step keeps going down")
    rig.yaw_offset = 10
    rig:rotate(-45, true)
    t.eq(rig.yaw_offset, -45, "from off the grid it snaps to the next multiple")
    rig:rotate(30)
    t.eq(rig.yaw_offset, -15)
    fe.world.unload("rotate")
  end,

  ["camera near and far planes are set from Lua and keep their order"] = function(t)
    local world = fe.world.load("planes", "arena")
    local cam = world:camera()
    t.near(cam.near, 0.1)
    t.near(cam.far, 300)
    cam.far = 1000
    t.near(cam.far, 1000, 1e-6, "far round-trips")
    t.error(function() cam.near = 2000 end, "from 0.001 to 100")
    cam.far = 10
    t.error(function() cam.near = 20 end, "below far")
    t.near(cam.near, 0.1, 1e-6, "a refused value changes nothing")
    cam.near = 5
    t.error(function() cam.far = 2 end, "above near")
    t.error(function() cam.far = 200000 end, "from 1 to 100000")
    t.near(cam.far, 10)
    cam.near = 0.5
    t.near(cam.near, 0.5)
    fe.world.unload("planes")
  end,

  ["i18n picks plural and select forms"] = function(t)
    i18n.defaults({ test = {
      items = "{count} {count|one:item|other:items}",
      found = "Found {count|=0:nothing|one:# item|other:# items}.",
      pronoun = "{gender|female:She|male:He|other:They} left.",
    } })
    t.eq(i18n.t("test.items", { count = 1 }), "1 item")
    t.eq(i18n.t("test.items", { count = 3 }), "3 items")
    t.eq(i18n.t("test.found", { count = 0 }), "Found nothing.")
    t.eq(i18n.t("test.found", { count = 1 }), "Found 1 item.")
    t.eq(i18n.t("test.found", { count = 12 }), "Found 12 items.")
    t.eq(i18n.t("test.pronoun", { gender = "female" }), "She left.")
    t.eq(i18n.t("test.pronoun", { gender = "x" }), "They left.")
    t.eq(i18n.t("test.pronoun", {}), "They left.")
    t.eq(i18n.t("test.items", {}), "{count} items", "a missing argument shows the placeholder, or the other form")
    local before = i18n.language
    i18n.set_language("fr")
    t.eq(i18n.plural(0), "one", "French: 0 and 1 are singular")
    t.eq(i18n.plural(2), "other")
    i18n.set_language(before or "en")
    t.eq(i18n.plural(0), "other")
  end,

  ["dialogue keeps a portrait on each side when two_portraits is on"] = function(t)
    local box = { line = { portrait = "a.png", speaker = "A" } }
    local sides = dialogue.portraits(box)
    t.eq(sides.left.image, "a.png")
    t.eq(sides.right, nil)
    box.line = { portrait = "b.png", speaker = "B", side = "right" }
    sides = dialogue.portraits(box)
    t.eq(sides.left, nil, "one portrait at a time by default")
    t.eq(sides.right.image, "b.png")
    dialogue.style.two_portraits = true
    box = { line = { portrait = "a.png", speaker = "A" } }
    dialogue.portraits(box)
    box.line = { portrait = "b.png", speaker = "B", side = "right" }
    sides = dialogue.portraits(box)
    t.eq(sides.left.image, "a.png")
    t.eq(sides.left.speaking, false, "the other side is dimmed")
    t.eq(sides.right.speaking, true)
    box.line = { speaker = "Narrator" }
    sides = dialogue.portraits(box)
    t.eq(sides.right.speaking, false, "a line without a portrait keeps both, neither speaking")
    dialogue.style.two_portraits = false
    t.eq(dialogue.Box.blocks_update, false)
  end,
}
