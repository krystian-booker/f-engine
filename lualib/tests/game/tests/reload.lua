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
  "schema.lua", "data.lua", "event.lua", "camera.lua", "dialogue.lua", "cutscene.lua", "field.lua",
  "field/placements.lua", "field/followers.lua", "follower.lua", "daynight.lua", "quest.lua", "story.lua",
  "worldmap.lua", "sound.lua", "timeline.lua", "devtools.lua", "ui/theme.lua", "ui/init.lua", "ui/screens/menu.lua",
  "ui/screens/backlog.lua", "ui/screens/config.lua", "ui/screens/save.lua", "ui/screens/shop.lua", "vehicle.lua",
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
    local vehicle = require("fe.vehicle")
    vehicle.define("reload_boat", { sprite = "@fe/assets/ui/spark.anim.json", required_tags = { "water" } })
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
    -- Pretend the file used to say walk_speed = 99 (live and baseline agree)
    -- while the game changed `followers` at runtime (above): a reload adopts
    -- the first and keeps the second.
    local file_walk = field.file_defaults.settings.walk_speed
    field.settings.walk_speed, field.file_defaults.settings.walk_speed = 99, 99
    dialogue.style.auto_delay, dialogue.file_defaults.style.auto_delay = 9, 9
    dialogue.style.lines = 5
    local file_text = ui.theme.file_defaults.text
    ui.theme.text, ui.theme.file_defaults.text = 0x123456ff, 0x123456ff
    ui.theme.set({ muted = 0x111111ff })
    local menu = require("fe.ui.screens.menu")
    local order_before = {}
    for i, page in ipairs(menu.pages) do order_before[i] = page.id end
    local custom_run = function() end
    table.insert(menu.pages, 1, { id = "custom", label = "Custom", run = custom_run })
    local items_run = menu.items_page

    for _, file in ipairs(kits) do
      t.ok(fe.harness.reload("@fe/" .. file), file)
    end
    t.eq(#fe.dev.errors(), 0, "no reload errors")

    t.eq(listeners(), before, "no listener was added twice")
    t.ok(state.flag("before_reload"))
    t.eq(save.version, 3)
    t.eq(field.settings.followers, 2, "a setting the game changed is kept")
    t.eq(field.settings.walk_speed, file_walk, "an edited setting is adopted")
    t.eq(field.file_defaults.settings.walk_speed, file_walk)
    t.eq(dialogue.style.auto_delay, dialogue.file_defaults.style.auto_delay, "dialogue.style too")
    t.eq(dialogue.style.lines, 5)
    dialogue.style.lines = dialogue.file_defaults.style.lines
    t.eq(ui.theme.text, file_text, "theme colours are adopted")
    t.eq(ui.theme.muted, 0x111111ff, "unless the game set them")
    t.eq(menu.pages[1].id, "custom", "a page the game inserted keeps its place")
    t.eq(menu.pages[1].run, custom_run, "and its function")
    local order_after = {}
    for i = 2, #menu.pages do order_after[i - 1] = menu.pages[i].id end
    t.eq(order_after, order_before, "the file's pages follow in their order")
    t.ok(menu.items_page ~= items_run, "with fresh page functions")
    t.eq(menu.page("items").run, "items_page", "pages name their module function")
    t.ok(menu.page("items").run ~= menu.page("skills").run, "paired by id, not by position")
    t.eq(menu.page_run(menu.page("items")), menu.items_page, "resolved on the module when opened")
    table.remove(menu.pages, 1)
    ui.theme.set({ muted = ui.theme.file_defaults.muted })
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
    t.eq(field.control, vehicle.hooks.control.wrapper, "the vehicle kit's hook is in place once")
    t.ok(vehicle.hooks.control.base ~= vehicle.hooks.control.wrapper)
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

  ["a theme reload keeps the high-contrast override on top"] = function(t)
    input.use_defaults()
    config.set("high_contrast", true)
    t.eq(ui.theme.current, "high_contrast")
    local file_text = ui.theme.file_defaults.text
    ui.theme.presets.default.text, ui.theme.file_defaults.text = 0x123456ff, 0x123456ff
    t.ok(fe.harness.reload("@fe/ui/theme.lua"))
    t.eq(ui.theme.current, "high_contrast")
    t.eq(ui.theme.text, 0xffffffff, "the override still shows")
    t.eq(ui.theme.skin, false)
    t.eq(ui.theme.presets.default.text, file_text, "the base colour underneath was adopted")
    config.set("high_contrast", false)
    t.eq(ui.theme.text, file_text, "and shows once the override lifts")
    config.dirty = false
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
