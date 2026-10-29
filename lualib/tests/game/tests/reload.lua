-- Hot reload of the stateful kits in the middle of a game, through the same
-- path a file change takes in development builds.
local camera = require("fe.camera")
local config = require("fe.config")
local daynight = require("fe.daynight")
local dialogue = require("fe.dialogue")
local event = require("fe.event")
local field = require("fe.field")
local follower = require("fe.follower")
local game = require("fe.game")
local input = require("fe.input")
local save = require("fe.save")
local scene = require("fe.scene")
local state = require("fe.state")
local task = require("fe.task")
local ui = require("fe.ui")

local kits = {
  "signal.lua", "task.lua", "scene.lua", "state.lua", "save.lua", "game.lua", "i18n.lua", "input.lua", "config.lua",
  "schema.lua", "data.lua", "event.lua", "camera.lua", "dialogue.lua", "cutscene.lua", "field.lua", "follower.lua",
  "daynight.lua", "quest.lua", "story.lua", "devtools.lua", "ui/theme.lua", "ui/init.lua", "ui/screens/menu.lua",
}

local function press(t, key)
  fe.input.feed(key, 1)
  t.tick(1)
  fe.input.feed(key, 0)
  t.tick(2)
end

local function listeners()
  return {
    treasure = field.treasure:count(), changed = state.changed:count(), updated = game.updated:count(),
    drawn = game.drawn:count(), entered = field.entered:count(), joined = follower.joined:count(),
    config = config.changed:count(), file_changed = game.file_changed:count(),
  }
end

return {
  ["reloading the kits mid-game keeps tasks, scenes, state and listeners"] = function(t)
    input.use_defaults()
    state.reset()
    scene.clear()
    field.reset()
    for _, module in ipairs({ "fe.quest", "fe.story", "fe.cutscene", "fe.ui.screens.menu" }) do require(module) end
    field.start("arena", "start")
    daynight.set_hour(8)
    follower.join("pal", { name = "Pal", sprite = "@fe/assets/ui/spark.anim.json" })
    state.set_flag("before_reload")
    save.version = 3
    field.settings.followers = 2
    ui.toast("Still here")
    local waited = false
    local waiter = task.spawn(function()
      task.wait(0.5)
      waited = true
    end)
    local talker = task.spawn(function()
      dialogue.say("Mira", "Reload me.", { speed = 0 })
      dialogue.say("Mira", "Still talking.", { speed = 0 })
    end)
    t.tick(2)
    local before = listeners()
    local edited_keys = daynight.keys
    daynight.keys = { { hour = 0, ambient = { 1, 1, 1 } } }
    camera.defaults = { pitch = 1, yaw = 0, distance = 1, fov = 1, height = 0, smoothing = 1 }

    for _, file in ipairs(kits) do
      t.ok(fe.harness.reload("@fe/" .. file), file)
    end
    t.eq(#fe.dev.errors(), 0, "no reload errors")

    t.eq(listeners(), before, "no listener was added twice")
    t.ok(state.flag("before_reload"))
    t.eq(save.version, 3)
    t.eq(field.settings.followers, 2)
    t.eq(daynight.hour(), 8)
    t.eq(#daynight.keys, #edited_keys, "__reload adopts the file's keyframes")
    t.eq(camera.defaults.pitch, 40, "__reload adopts the file's camera defaults")
    t.ok(follower.has("pal"))
    t.eq(#field.followers(), 1)
    t.eq(ui.toasts()[1].text, "Still here")
    t.ok(scene.contains(field.scene))
    t.eq(dialogue.current().line.text, "Reload me.")

    press(t, "key:Z")
    t.eq(dialogue.current().line.text, "Still talking.")
    press(t, "key:Z")
    t.tick(1)
    t.eq(talker.status, "done")
    t.eq(dialogue.current(), nil, "the box that was open during the reload closed")
    local after = task.spawn(function() dialogue.say(nil, "A new box.") end)
    t.tick(1)
    t.eq(dialogue.current().line.text, "A new box.")
    dialogue.finish_line(after)
    t.tick(2)
    t.eq(after.status, "done", "the new box is the one every function sees")
    t.wait_until(function() return waited end, 60, "a task waiting across the reload finishes")
    t.eq(waiter.status, "done")

    local x = field.player.position.x
    fe.input.feed("key:Right", 1)
    t.tick(20)
    fe.input.feed("key:Right", 0)
    t.tick(2)
    t.ok(field.player.position.x > x + 0.5, "the field still moves the player")
    local events = 0
    local disconnect = state.changed:connect(function() events = events + 1 end)
    state.set_flag("after_reload")
    disconnect()
    t.eq(events, 1)
    field.give("potion", 1)
    t.eq(#ui.toasts(), 2, "one toast per pickup")

    local release = game.pause(field.world)
    t.ok(field.world.paused)
    release()
    t.ok(not field.world.paused)
    follower.leave("pal")
    field.settings.followers = 0
    save.version = 1
    ui.clear_toasts()
    scene.clear()
  end,

  ["reloading fe.daynight shows the keyframes; a reloaded field keeps refreshing pages"] = function(t)
    input.use_defaults()
    state.reset()
    scene.clear()
    field.reset()
    field.start("arena", "start")
    daynight.set_hour(12)
    local applied = 0
    local real = field.world
    field.world = {
      props = function() return { daynight = true } end,
      set_environment = function() applied = applied + 1 end,
    }
    local ok = fe.harness.reload("@fe/daynight.lua")
    field.world = real
    t.ok(ok)
    t.eq(applied, 1, "__reload applied the time of day")
    t.ok(fe.harness.reload("@fe/field.lua"))
    field.start("arena", "start")
    local npc = field.world:spawn({ id = "npc", position = { x = 4.5, z = 7.5 } })
    event.attach(npc, { { script = "tests.support.greet" }, { when = { flag = "after_field_reload" }, script = "tests.support.again" } })
    table.insert(field.entities(), npc)
    state.set_flag("after_field_reload")
    t.eq(event.page(npc).script, "tests.support.again", "the reloaded listener sees the new map's entities")
    t.eq(#fe.dev.errors(), 0)
    scene.clear()
  end,
}
