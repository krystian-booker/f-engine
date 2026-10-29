local cutscene = require("fe.cutscene")
local data = require("fe.data")
local dialogue = require("fe.dialogue")
local event = require("fe.event")
local field = require("fe.field")
local game = require("fe.game")
local input = require("fe.input")
local pages = require("tests.support.pages")
local record = require("tests.support.record")
local save = require("fe.save")
local scene = require("fe.scene")
local state = require("fe.state")
local task = require("fe.task")

local function fresh()
  input.use_defaults()
  state.reset()
  scene.clear()
  field.reset()
end

local function press(t, key)
  fe.input.feed(key, 1)
  t.tick(1)
  fe.input.feed(key, 0)
  t.tick(2)
end

local function expect_errors(t, pattern, fn)
  local before = #fe.dev.errors()
  fn()
  local errors = fe.dev.errors()
  local found = 0
  for i = before + 1, #errors do
    if errors[i].message:find(pattern, 1, true) then found = found + 1 end
  end
  fe.dev.clear_errors()
  return found
end

-- An entity on the field map with event pages, registered as a field entity.
local function placed(id, x, page_list)
  local e = field.world:spawn({ id = id, position = { x = x, z = 7.5 } })
  event.attach(e, page_list)
  table.insert(field.entities(), e)
  return e
end

-- Skip the running skippable cutscene: Menu asks, Confirm agrees.
local function skip(t)
  press(t, "key:Tab")
  press(t, "key:Z")
end

-- An entity just east of the player, registered as a field entity.
local function neighbour(id)
  local p = field.player.position
  local e = field.world:spawn({ id = id, position = { x = p.x + 0.9, z = p.z }, collider = { radius = 0.3 } })
  table.insert(field.entities(), e)
  field.player:face("east")
  return e
end

return {
  ["event pages follow flags and run scripts"] = function(t)
    fresh()
    local world = fe.world.load("pages", "arena")
    local npc = world:spawn({ id = "npc", position = { x = 3.5, z = 5.5 } })
    event.attach(npc, {
      { script = "tests.support.greet", trigger = "action" },
      { when = { flag = "met" }, script = "tests.support.again", trigger = "action", hidden = true },
    })
    t.eq(event.page(npc).script, "tests.support.greet")
    local job = event.run(npc)
    t.wait_until(function() return job.status ~= "running" end)
    t.ok(state.flag("met"))
    event.refresh(npc)
    t.eq(event.page(npc).script, "tests.support.again")
    t.ok(not npc:sprite().visible)
    t.eq(event.blocking, 0, "the event's hold was released")
    fe.world.unload("pages")
  end,

  ["the player walks with input and talks to entities"] = function(t)
    fresh()
    local talked = false
    field.start("arena", "start")
    local before = field.player.position
    fe.input.feed("key:Right", 1)
    t.tick(30)
    fe.input.feed("key:Right", 0)
    t.tick(2)
    local after = field.player.position
    t.ok(after.x > before.x + 0.5, "moved right")
    neighbour("friend"):data().interact = function() talked = true end
    press(t, "key:Z")
    t.ok(talked)
    t.ok(field.can_control(), "control returns after the interaction")
    scene.clear()
  end,

  ["walking in an encounter area starts a battle"] = function(t)
    fresh()
    data.load("encounters", "data/encounters.json", "encounter_table")
    field.start("arena", "start")
    local world = field.world
    local width, depth = world:grid_size()
    for x = 0, width - 1 do
      for z = 0, depth - 1 do world:set_cell({ x = x, z = z, layer = 0 }, { add_tags = { "encounter:meadow" } }) end
    end
    local fought
    local original = field.on_encounter
    field.on_encounter = function(troop)
      fought = troop
      t.ok(not field.can_control(), "control is locked during the battle")
    end
    field.player:place({ x = 1.5, z = 6.5 }, 0)
    local key = "key:Right"
    for _ = 1, 12 do
      fe.input.feed(key, 1)
      t.wait_until(function()
        local x = field.player.position.x
        return fought ~= nil or (key == "key:Right" and x > 9.2) or (key == "key:Left" and x < 1.8)
      end, 400)
      fe.input.feed(key, 0)
      if fought then break end
      key = key == "key:Right" and "key:Left" or "key:Right"
    end
    field.on_encounter = original
    t.ok(fought == "slimes" or fought == "golem", "an encounter fired")
    t.tick(2)
    t.eq(field.locks, 0, "control returns after the battle")
    scene.clear()
  end,

  ["an action door warps from its interaction and returns control"] = function(t)
    fresh()
    field.settings.autosave = false
    field.start("arena", "start")
    local p = field.player.position
    local door = field.types.door(field.world, { id = "door", position = { x = p.x + 0.9, z = p.z }, layer = 0,
      props = { map = "arena", point = "east", trigger = "action" } })
    table.insert(field.entities(), door)
    field.player:face("east")
    press(t, "key:Z")
    t.wait_until(function() return field.player.position.x > 9 and field.locks == 0 end, 200,
      "the warp finished and unlocked")
    t.ok(field.can_control())
    field.settings.autosave = true
    scene.clear()
  end,

  ["touch triggers wait while control is locked"] = function(t)
    fresh()
    field.start("arena", "start")
    local touched = 0
    local plate = field.world:spawn({ id = "plate", position = { x = 5.5, z = 7.5 }, collider = { radius = 0.3, solid = false } })
    plate:data().on_touch = function() touched = touched + 1 end
    local unlock = field.lock()
    field.touch(plate)
    t.eq(touched, 0)
    local release = event.block()
    unlock()
    field.touch(plate)
    t.eq(touched, 0, "a running event also blocks touches")
    release()
    field.touch(plate)
    t.eq(touched, 1)
    scene.clear()
  end,

  ["locks and holds end with their task; reset clears them"] = function(t)
    fresh()
    local job = task.spawn(function()
      field.lock()
      event.block()
      task.wait(10)
    end)
    t.eq(field.locks, 1)
    t.ok(event.busy())
    job:cancel()
    t.eq(field.locks, 0)
    t.ok(not event.busy())
    local release = field.lock()
    field.reset()
    t.eq(field.locks, 0)
    release()
    t.eq(field.locks, 0, "a stale release after reset changes nothing")
  end,

  ["games add verbs on their own input"] = function(t)
    fresh()
    field.start("arena", "start")
    local poked
    field.verb("test_poke", {
      input = "info",
      when = function(e) return e.id == "pokeable" end,
      run = function(e, player) poked = e.id .. ">" .. player.id end,
    })
    neighbour("pokeable")
    local target, verbs = field.target("info")
    t.eq(target and target.id, "pokeable")
    t.eq(verbs[1].id, "test_poke")
    t.eq(field.target("confirm"), nil, "talk does not apply")
    press(t, "key:I")
    field.remove_verb("test_poke")
    t.eq(poked, "pokeable>player")
    scene.clear()
  end,

  ["custom routines run for idle entities"] = function(t)
    fresh()
    field.start("arena", "start")
    local ticks = 0
    field.routine("count", function(entity, d) ticks = ticks + 1 end)
    local npc = neighbour("counter")
    npc:data().routine = "count"
    t.tick(3)
    t.ok(ticks >= 2)
    field.routines.count = nil
    scene.clear()
  end,

  ["auto and parallel pages get the player; a new map cancels old scripts"] = function(t)
    fresh()
    record.calls = {}
    field.start("arena", "start")
    local watcher = field.world:spawn({ id = "watcher", position = { x = 4.5, z = 7.5 } })
    event.attach(watcher, { { script = "tests.support.record", trigger = "parallel" } })
    table.insert(field.entities(), watcher)
    local greeter = field.world:spawn({ id = "greeter", position = { x = 6.5, z = 7.5 } })
    event.attach(greeter, { { script = "tests.support.record", trigger = "auto" } })
    table.insert(field.entities(), greeter)
    t.tick(4)
    local triggers = {}
    for _, call in ipairs(record.calls) do
      t.eq(call.player and call.player.id, "player")
      triggers[call.trigger] = true
    end
    t.ok(triggers.parallel and triggers.auto)
    local running = watcher:data().parallel
    t.ok(running:running())
    field.start("arena", "start")
    t.eq(running.status, "cancelled")
    scene.clear()
  end,

  ["a failing page condition is reported once and does not break flags"] = function(t)
    fresh()
    field.start("arena", "start")
    event.condition("explodes", function() error("kaboom") end)
    local found = expect_errors(t, "Event page 2 of 'fragile'", function()
      local npc = field.world:spawn({ id = "fragile", position = { x = 4.5, z = 7.5 } })
      event.attach(npc, { { script = "tests.support.greet" }, { when = { explodes = true }, script = "tests.support.again" } })
      table.insert(field.entities(), npc)
      state.set_flag("anything")
      state.set_flag("anything_else")
      t.eq(event.page(npc).script, "tests.support.greet")
      npc:destroy()
    end)
    t.eq(found, 1)
    event.conditions.explodes = nil
    scene.clear()
  end,

  ["warping writes the autosave once scripts have finished"] = function(t)
    fresh()
    save.delete(save.autosave_slot)
    field.start("arena", "start")
    local job = task.spawn(function() field.warp("arena", "east") end)
    t.wait_until(function() return job.status ~= "running" end, 120)
    t.tick(1)
    t.ok(save.exists(save.autosave_slot), "autosave written")
    local _, meta = fe.save.read(save.autosave_slot)
    t.ok(meta.autosave)
    t.eq(meta.location, "Test Arena")
    save.delete(save.autosave_slot)
    local saved_during
    job = task.spawn(function()
      event.block()
      field.warp("arena", "start")
      t.tick(5)
      saved_during = save.exists(save.autosave_slot)
    end)
    t.wait_until(function() return job.status ~= "running" end, 120)
    t.tick(2)
    t.eq(saved_during, false, "no autosave in the middle of an event")
    t.ok(save.exists(save.autosave_slot), "saved after the event")
    field.settings.autosave = false
    save.delete(save.autosave_slot)
    job = task.spawn(function() field.warp("arena", "start") end)
    t.wait_until(function() return job.status ~= "running" end, 120)
    t.tick(2)
    t.ok(not save.exists(save.autosave_slot), "autosave can be turned off")
    field.settings.autosave = function(map_id, point) return point == "east" end
    job = task.spawn(function() field.warp("arena", "east") end)
    t.wait_until(function() return job.status ~= "running" end, 120)
    t.tick(2)
    t.ok(save.exists(save.autosave_slot), "a function decides per warp")
    field.settings.autosave = true
    save.delete(save.autosave_slot)
    scene.clear()
  end,

  ["a cancelled warp releases control and uncovers the screen"] = function(t)
    fresh()
    field.start("arena", "start")
    local job = task.spawn(function() field.warp("arena", "east", { autosave = false }) end)
    t.tick(5)
    t.ok(field.locks > 0)
    job:cancel()
    t.eq(field.locks, 0)
    t.eq(fe.screen.get().fade[4], 0)
    scene.clear()
  end,

  ["a parallel page that turns itself off stops after the change it made"] = function(t)
    fresh()
    pages.reset()
    field.start("arena", "start")
    local now = placed("now", 4.5, { { when = { not_flag = "off_now" }, script = "tests.support.switch_now", trigger = "parallel" } })
    t.tick(10)
    t.eq(pages.runs, 1, "the page turned off during its first run, before event.run returned")
    t.eq(event.page(now), nil)
    t.eq(now:data().parallel.status, "done")
    pages.reset()
    local later = placed("later", 5.5, { { when = { not_flag = "off_later" }, script = "tests.support.switch_later",
      trigger = "parallel" } })
    local other = placed("other", 6.5, { { script = "tests.support.greet" },
      { when = { flag = "off_later" }, script = "tests.support.again" } })
    local heard = 0
    local disconnect = state.changed:connect(function(kind) if kind == "flag" then heard = heard + 1 end end, -10)
    t.wait_until(function() return state.flag("off_later") end, 60)
    disconnect()
    t.eq(heard, 1, "listeners after the field's still hear the change")
    t.eq(event.page(other).script, "tests.support.again", "other entities' pages follow it")
    t.tick(1)
    t.eq(later:data().parallel.status, "cancelled", "the rest of the run is cancelled on the next tick")
    t.tick(80)
    t.eq(pages.late, nil)
    t.eq(pages.runs, 1)
    scene.clear()
  end,

  ["an entity running a parallel page can still be talked to"] = function(t)
    fresh()
    record.calls = {}
    field.start("arena", "start")
    local npc = neighbour("busy")
    event.attach(npc, { { script = "tests.support.record", trigger = "parallel" } })
    local talked = false
    npc:data().interact = function() talked = true end
    t.tick(2)
    t.ok(npc:data().parallel:running())
    t.eq(field.target(), npc)
    press(t, "key:Z")
    t.ok(talked)
    t.ok(npc:data().parallel:running(), "the parallel page runs on")
    scene.clear()
  end,

  ["tasks a script starts pause with the field and end with its map"] = function(t)
    fresh()
    pages.reset()
    field.start("arena", "start")
    local npc = placed("spawner", 4.5, { { script = "tests.support.spawner", trigger = "auto" } })
    t.tick(3)
    t.eq(npc:data().running.status, "done", "the script returned and left its task running")
    local release = game.pause(field.world)
    local before = pages.count
    t.tick(30)
    t.eq(pages.count, before, "the task keeps the script's world binding")
    release()
    t.tick(10)
    t.ok(pages.count > before)
    field.start("arena", "start")
    before = pages.count
    t.tick(10)
    t.eq(pages.count, before, "a new map cancels it")
    scene.clear()
  end,

  ["a script that changes maps carries on as a script of the new map"] = function(t)
    fresh()
    pages.reset()
    field.start("arena", "start")
    placed("traveller", 4.5, { { script = "tests.support.traveller", trigger = "auto" } })
    t.wait_until(function() return pages.arrived end, 200)
    t.tick(10)
    t.ok(pages.count > 0, "the script went on after the warp")
    field.start("arena", "start")
    local before = pages.count
    t.tick(10)
    t.eq(pages.count, before, "what it started on the new map ends with that map")
    scene.clear()
  end,

  ["a skipped cutscene uncovers the screen for a choice and at its end"] = function(t)
    fresh()
    field.start("arena", "start")
    local answer
    local job = task.spawn(function()
      cutscene.play(function()
        dialogue.say("Guide", "First line.", { speed = 0 })
        task.wait(2)
        answer = dialogue.choice({ "Yes", "No" })
        dialogue.say("Guide", "After the choice.", { speed = 0 })
      end, { skippable = true, letterbox = false })
    end)
    t.tick(5)
    skip(t)
    t.wait_until(function() return scene.top() and scene.top().name == "menu" end, 120, "the choice is asked")
    t.eq(fe.screen.get().fade[4], 0, "the choice is asked on an uncovered screen")
    press(t, "key:Z")
    t.tick(2)
    t.eq(dialogue.current().line.text, "After the choice.")
    press(t, "key:Z")
    t.wait_until(function() return job.status ~= "running" end, 120)
    t.eq(answer, 1)
    t.eq(fe.screen.get().fade[4], 0)
    t.eq(field.locks, 0)
    scene.clear()
  end,

  ["a warp inside a skipped cutscene keeps the screen covered until it ends"] = function(t)
    fresh()
    field.start("arena", "start")
    local fade_after_warp
    local job = task.spawn(function()
      cutscene.play(function()
        dialogue.say("Guide", "First line.", { speed = 0 })
        field.warp("arena", "east", { autosave = false })
        fade_after_warp = fe.screen.get().fade[4]
        dialogue.say("Guide", "Arrived.", { speed = 0 })
      end, { skippable = true, letterbox = false })
    end)
    t.tick(5)
    skip(t)
    t.wait_until(function() return job.status ~= "running" end, 200)
    t.eq(job.status, "done")
    t.eq(fade_after_warp, 1)
    t.near(field.player.position.x, 9.5, 0.01)
    t.eq(fe.screen.get().fade[4], 0)
    t.eq(field.locks, 0)
    scene.clear()
  end,

  ["a menu page that fails returns to the menu"] = function(t)
    fresh()
    local menu = require("fe.ui.screens.menu")
    table.insert(menu.pages, 1, { id = "broken", label = "Broken", run = function() error("page broke") end })
    local job = task.spawn(menu.open)
    t.tick(1)
    local found = expect_errors(t, "page broke", function() press(t, "key:Z") end)
    table.remove(menu.pages, 1)
    t.eq(found, 1)
    t.eq(job.status, "running")
    t.ok(scene.find("menu") ~= nil)
    press(t, "key:Escape")
    t.tick(1)
    t.eq(job.status, "done")
    t.eq(scene.find("menu"), nil)
  end,
}
