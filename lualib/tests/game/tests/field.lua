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

  ["without an editor the game may always leave a map"] = function(t)
    t.eq(fe.dev.leave_map("field"), "ok")
    t.eq(fe.dev.leave_map_result(), nil)
    fresh()
    field.start("arena", "start")
    t.ok(require("fe.field.warp").editor_allows_leaving())
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

  ["the field keeps running while a line is on screen: camera, events and parallel pages"] = function(t)
    fresh()
    record.calls = {}
    field.start("arena", "start")
    placed("watcher", 4.5, { { script = "tests.support.record", trigger = "parallel" } })
    local touched = 0
    local plate = field.world:spawn({ id = "plate", position = { x = 4.5, z = 7.5 }, collider = { radius = 0.3, solid = false } })
    plate:data().on_touch = function() touched = touched + 1 end
    t.tick(2)
    local job = task.spawn(function() dialogue.say("Guide", "Walk with me.", { speed = 0 }) end)
    t.tick(1)
    t.eq(dialogue.current().line.text, "Walk with me.")
    t.eq(dialogue.current().blocks_update, false, "the box does not block the field")
    t.ok(not field.can_control(), "field input is gated while the box is on top")
    local calls_before = #record.calls
    local camera_before = field.rig.cam.target.x
    field.player:move_to({ x = 4.5, z = 7.5 })
    t.wait_until(function() return not field.player:moving() end, 300, "the player walks while the line shows")
    t.tick(2)
    t.eq(job.status, "running", "the line is still on screen")
    t.ok(#record.calls > calls_before, "the parallel page kept running")
    t.ok(field.rig.cam.target.x > camera_before + 1, "the camera kept following the player")
    t.eq(touched, 1, "world events were drained: the plate was touched")
    local chosen
    job:cancel()
    job = task.spawn(function() chosen = dialogue.choice({ "Yes", "No" }) end)
    t.tick(2)
    t.eq(scene.top().name, "menu")
    t.eq(scene.top().blocks_update, false, "the choice menu does not block the field either")
    press(t, "key:Z")
    t.eq(chosen, 1)
    scene.clear()
  end,

  ["auto pages wait for a field lock and start when it lifts"] = function(t)
    fresh()
    record.calls = {}
    field.start("arena", "start")
    local unlock = field.lock()
    placed("greeter", 6.5, { { script = "tests.support.record", trigger = "auto" } })
    t.tick(4)
    t.eq(#record.calls, 0, "no auto page starts under a lock")
    unlock()
    t.tick(2)
    t.eq(#record.calls, 1)
    t.eq(record.calls[1].trigger, "auto")
    local release = event.block()
    placed("second", 7.5, { { script = "tests.support.record", trigger = "auto" } })
    t.tick(3)
    t.eq(#record.calls, 1, "a blocking event holds auto pages too")
    release()
    t.tick(2)
    t.eq(#record.calls, 2)
    scene.clear()
  end,

  ["pages attached with no active page hide the entity at once; props hide their mesh"] = function(t)
    fresh()
    local world = fe.world.load("attach", "arena")
    local npc = world:spawn({ id = "later", position = { x = 3.5, z = 5.5 }, collider = { radius = 0.3 },
      sprite = { animation = "@fe/assets/ui/spark.anim.json" } })
    event.attach(npc, { { when = { flag = "appears" }, script = "tests.support.greet" } })
    t.eq(event.page(npc), nil)
    t.ok(not npc:sprite().visible, "hidden from the start")
    t.ok(npc:can_occupy(npc.position), "not solid: another entity may stand there")
    state.set_flag("appears")
    event.refresh(npc)
    t.ok(npc:sprite().visible)
    local calls = {}
    local fake = {
      mesh = function(_, settings) calls[#calls + 1] = { "mesh", settings.visible } end,
      sprite = function(_, settings) calls[#calls + 1] = { "sprite", settings.visible } end,
      collider = function(_, settings) calls[#calls + 1] = { "collider", settings.solid } end,
    }
    event.apply(fake, nil, { mesh = true, solid = true })
    t.eq(calls, { { "mesh", false }, { "collider", false } }, "a mesh prop hides its mesh and adds no sprite")
    calls = {}
    event.apply(fake, { script = "x" }, { mesh = true })
    t.eq(calls, { { "mesh", true }, { "collider", true } })
    fe.world.unload("attach")
  end,

  ["a touch suppressed under a lock fires when the lock lifts, if the player still stands there"] = function(t)
    fresh()
    field.start("arena", "start")
    t.tick(2)
    local touched = 0
    local p = field.player.position
    local plate = field.world:spawn({ id = "plate", position = { x = p.x + 0.3, z = p.z }, collider = { radius = 0.3, solid = false } })
    plate:data().on_touch = function() touched = touched + 1 end
    local unlock = field.lock()
    field.touch(plate)
    t.eq(touched, 0)
    t.tick(3)
    t.eq(touched, 0, "still locked")
    unlock()
    t.tick(1)
    t.eq(touched, 1, "fired when the lock lifted")
    local far = field.world:spawn({ id = "far", position = { x = 8.5, z = 7.5 }, collider = { radius = 0.3, solid = false } })
    far:data().on_touch = function() touched = touched + 10 end
    unlock = field.lock()
    field.touch(far)
    unlock()
    t.tick(1)
    t.eq(touched, 1, "a touch the player walked away from is dropped")
    scene.clear()
  end,

  ["a script warp cancels a pending detached door warp"] = function(t)
    fresh()
    field.settings.autosave = false
    field.start("arena", "start")
    local door = task.detached("warp", field.warp, "arena", "east", { duration = 1 })
    t.tick(5)
    t.ok(door:running())
    local script = task.spawn(function() field.warp("arena", "start", { duration = 0.1 }) end)
    t.eq(door.status, "cancelled", "one warp at a time")
    t.wait_until(function() return script.status ~= "running" end, 200)
    t.eq(script.status, "done")
    t.near(field.player.position.x, 2.5, 0.01, "the script's destination won")
    t.eq(field.locks, 0)
    t.eq(fe.screen.get().fade[4], 0)
    field.settings.autosave = true
    scene.clear()
  end,

  ["hidden items are found once; locked chests need their key or flag"] = function(t)
    fresh()
    field.start("arena", "start")
    local given = {}
    local disconnect = field.treasure:connect(function(item, count, money) given[#given + 1] = { item, count, money } return true end, 10)
    local spot = field.types.hidden_item(field.world, { id = "spot", position = { x = 4.5, z = 5.5 }, layer = 0,
      props = { item = "herb", count = 2 } })
    t.ok(spot ~= nil and spot:valid())
    spot:data().interact(spot)
    t.eq(given, { { "herb", 2, nil } })
    t.ok(state.flag("hidden:arena:spot"))
    t.ok(not spot:valid(), "the spot is gone once searched")
    t.eq(field.types.hidden_item(field.world, { id = "spot", position = { x = 4.5, z = 5.5 }, layer = 0, props = { item = "herb" } }), nil,
      "and is not spawned again")
    local has_key = false
    event.condition("item", function(id) return id == "iron_key" and has_key end)
    local chest = field.types.chest(field.world, { id = "box", position = { x = 5.5, z = 5.5 }, layer = 0,
      props = { item = "gem", locked = true, key = "iron_key" } })
    local function finished() return dialogue.current() and dialogue.current().finished end
    local job = task.spawn(function() chest:data().interact(chest) end)
    t.tick(1)
    t.eq(dialogue.current().line.text, "It's locked.")
    t.wait_until(finished, 60)
    press(t, "key:Z")
    t.eq(job.status, "done")
    t.eq(#given, 1, "nothing given")
    has_key = true
    job = task.spawn(function() chest:data().interact(chest) end)
    t.tick(1)
    t.eq(dialogue.current().line.text, "Unlocked with iron_key.")
    t.wait_until(finished, 60)
    press(t, "key:Z")
    t.eq(job.status, "done")
    t.eq(given[2], { "gem", 1, nil })
    t.ok(state.flag("unlocked:arena:box"))
    local flagged = field.types.chest(field.world, { id = "crate", position = { x = 6.5, z = 5.5 }, layer = 0,
      props = { item = "coin", locked = true } })
    state.set_flag("unlocked:arena:crate")
    job = task.spawn(function() flagged:data().interact(flagged) end)
    t.tick(1)
    t.eq(job.status, "done", "a chest unlocked by its flag opens without a word")
    t.eq(given[3], { "coin", 1, nil })
    event.conditions.item = nil
    disconnect()
    scene.clear()
  end,

  ["field.start builds the player through field.make_player"] = function(t)
    fresh()
    local original = field.make_player
    local got
    field.make_player = function(world, point, sprite)
      got = { map = world.map_id, point = point, sprite = sprite }
      local player = original(world, point, sprite)
      player:tag("custom")
      return player
    end
    field.start("arena", "start")
    field.make_player = original
    t.eq(got.map, "arena")
    t.near(got.point.position.x, 2.5, 0.01)
    t.eq(got.sprite, field.player_sprite())
    t.ok(field.player:has_tag("custom"), "the entity it returns is the player")
    t.near(field.player:mover().speed, field.settings.walk_speed, 0.01, "the default mover walks at walk_speed")
    scene.clear()
  end,

  ["menu pages may name a module function to run"] = function(t)
    fresh()
    local menu = require("fe.ui.screens.menu")
    local ran = 0
    menu.test_page = function() ran = ran + 1 end
    table.insert(menu.pages, 1, { id = "named", label = "Named", run = "test_page" })
    t.eq(menu.page_run(menu.page("named")), menu.test_page)
    t.eq(menu.page_run(menu.page("items")), menu.items_page, "the built-in pages name theirs")
    local job = task.spawn(menu.open)
    t.tick(1)
    press(t, "key:Z")
    t.eq(ran, 1, "resolved on the module when chosen")
    menu.test_page = function() ran = ran + 10 end
    press(t, "key:Z")
    t.eq(ran, 11, "a replaced module function is what runs")
    table.remove(menu.pages, 1)
    menu.test_page = nil
    t.ok(not pcall(menu.page_run, { id = "x", run = "no_such_page" }), "an unknown name raises")
    press(t, "key:Escape")
    t.tick(1)
    t.eq(job.status, "done")
  end,

  ["a menu page that starts gameplay runs after the menu's pause is released"] = function(t)
    fresh()
    field.start("arena", "start")
    local menu = require("fe.ui.screens.menu")
    local ran_paused, ran_in
    table.insert(menu.pages, 1, { id = "travel_test", label = "Travel", closes = true, run = function()
      ran_paused = field.world.paused
      ran_in = task.current().parent
    end })
    local job = task.spawn(menu.open)
    t.tick(2)
    t.ok(field.world.paused)
    press(t, "key:Z")
    t.tick(2)
    table.remove(menu.pages, 1)
    t.eq(job.status, "done", "the menu closed")
    t.eq(scene.find("menu"), nil)
    t.eq(ran_paused, false, "the page ran with the field world running")
    t.eq(ran_in, nil, "in a detached task")
    scene.clear()
  end,

  ["the bridge layer carries the player over the river"] = function(t)
    fresh()
    field.start("arena", "start")
    field.player:place({ x = 5.5, z = 7.5 }, 0)
    fe.input.feed("key:Right", 1)
    t.wait_until(function() return field.player.position.x > 7.6 end, 300)
    t.eq(field.player.layer, 1)
    t.ok(field.player.position.y > 0.9)
    fe.input.feed("key:Right", 0)
    t.tick(2)
    scene.clear()
  end,

  ["encounter symbols wander near home and chase at their own speed"] = function(t)
    local encounter = require("fe.encounter")
    local world = fe.world.load("symbol_test", "arena")
    local symbol = encounter.symbol(world, { troop = "slimes", position = { x = 8.5, z = 7.5 }, sprite = "", speed = 3,
      wander = 1.5 })
    local player = { position = { x = 1.5, z = 1.5 } }
    local moved = false
    for _ = 1, 300 do
      encounter.update_symbols(world, player)
      t.tick(1)
      local p = symbol.position
      if math.abs(p.x - 8.5) + math.abs(p.z - 7.5) > 0.2 then moved = true end
      t.ok(math.sqrt((p.x - 8.5) ^ 2 + (p.z - 7.5) ^ 2) < 2.2, "stays near home")
    end
    t.ok(moved, "it wanders")
    player.position = { x = symbol.position.x + 3, z = symbol.position.z }
    local before = symbol.position.x
    for _ = 1, 30 do
      encounter.update_symbols(world, player)
      t.tick(1)
    end
    t.ok(symbol.position.x - before > 1.2, "chases at speed 3 (" .. (symbol.position.x - before) .. ")")
    fe.world.unload("symbol_test")
  end,
}
