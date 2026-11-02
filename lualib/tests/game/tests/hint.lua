-- fe.field.waypoint and fe.hint: objective markers across maps, compass
-- bearings, idle and boundary hints and the route line.
local config = require("fe.config")
local data = require("fe.data")
local dialogue = require("fe.dialogue")
local field = require("fe.field")
local hint = require("fe.hint")
local input = require("fe.input")
local quest = require("fe.quest")
local scene = require("fe.scene")
local state = require("fe.state")
local task = require("fe.task")
local waypoint = require("fe.field.waypoint")

local function start()
  input.use_defaults()
  state.reset()
  scene.clear()
  field.reset()
  hint.reset()
  field.settings.autosave = false
  data.load("quests", "data/quests.json", "quest")
  field.start("trail", "start")
end

local function finish()
  hint.reset()
  field.settings.autosave = true
  scene.clear()
end

return {
  ["objective markers resolve on this map or through its exits"] = function(t)
    start()
    t.eq(waypoint.current(), nil, "no active quest, no marker")
    quest.start("beacon")
    local current = waypoint.current()
    t.eq(current.map, "annex")
    t.eq(current.point, "door")
    t.eq(current.objective, "annex")
    t.eq(current.via, "east", "a marker on another map points at this map's exit towards it")
    t.near(current.position.x, 45.5, 1e-4)
    quest.complete_objective("beacon", "annex")
    current = waypoint.current()
    t.eq(current.map, "trail", "the next unfinished objective has no marker: the stage's")
    t.near(current.position.x, 40.5, 1e-4)
    t.eq(current.hint, "Head east along the trail.")
    t.eq(waypoint.route("trail", "depot"), "east", "exits of the maps between are followed")
    t.eq(waypoint.route("trail", "nowhere"), nil)
    finish()
  end,

  ["bearings follow the camera's yaw"] = function(t)
    local o = { x = 0, z = 0 }
    t.near(waypoint.bearing(o, { x = 0, z = -1 }, 0), 0, 1e-6, "up the screen")
    t.near(waypoint.bearing(o, { x = 1, z = 0 }, 0), 90, 1e-6, "right")
    t.near(waypoint.bearing(o, { x = 0, z = 1 }, 0), 180, 1e-6, "down")
    t.near(waypoint.bearing(o, { x = -1, z = 0 }, 0), 270, 1e-6, "left")
    t.near(waypoint.bearing(o, { x = -1, z = 0 }, 90), 0, 1e-6, "a turned camera turns the compass")
    local _, distance = waypoint.bearing(o, { x = 3, z = 4 }, 0)
    t.near(distance, 5, 1e-6)
  end,

  ["idle hints wait for no progress, pause with dialogue and follow the setting"] = function(t)
    start()
    quest.start("beacon")
    hint.settings.idle = 2
    local shown = {}
    local disconnect = hint.shown:connect(function(text) shown[#shown + 1] = text end)
    t.tick(60)
    quest.progress("beacon", "annex")
    t.eq(hint.idle_time(), 0, "quest progress starts the idle timer over")
    local talk = task.spawn(function() dialogue.say("Guide", "Wait.", { speed = 0 }) end)
    t.tick(150)
    t.eq(#shown, 0, "the timer stands still while a message box is open")
    dialogue.finish_line(talk)
    t.tick(130)
    t.eq(shown[1], "Head east along the trail.", "the next-step hint after idling")
    t.ok(hint.current() ~= nil)
    hint.draw()
    config.set("hints", false)
    local idle = hint.idle_time()
    t.tick(60)
    t.eq(hint.idle_time(), idle, "hints off: no timers")
    config.set("hints", true)
    config.dirty = false
    disconnect()
    hint.settings.idle = hint.file_defaults.settings.idle
    finish()
  end,

  ["lingering in a hint zone or near a boundary shows its hint once"] = function(t)
    start()
    local shown = {}
    local disconnect = hint.shown:connect(function(text) shown[#shown + 1] = text end)
    field.player:place({ x = 44.5, z = 6.5 }, 0)
    t.tick(60)
    t.eq(#shown, 0)
    t.tick(90)
    t.eq(shown[1], "The road is blocked.", "after the zone's two seconds")
    t.tick(200)
    t.eq(#shown, 1, "once per visit")
    local stop = hint.boundary("barrier_test", "Use the side path.", 1)
    t.tick(30)
    stop()
    t.tick(60)
    t.eq(#shown, 1, "leaving stops the timer")
    hint.boundary("barrier_test", "Use the side path.", 0.5)
    t.tick(40)
    t.eq(shown[2], "Use the side path.")
    disconnect()
    finish()
  end,

  ["the new kits hot-reload in place without adding listeners"] = function(t)
    start()
    require("fe.bark")
    require("fe.grant")
    require("fe.codex")
    local game = require("fe.game")
    local before = { updated = game.updated:count(), drawn = game.drawn:count(), shown = dialogue.line_shown:count(),
      changed = quest.changed:count(), verbs = #field.verbs }
    for _, file in ipairs({ "bark.lua", "grant.lua", "codex.lua", "hint.lua", "field/traverse.lua", "field/anchors.lua",
      "field/waypoint.lua", "field/followers.lua", "field/interaction.lua", "camera.lua", "cutscene.lua", "config.lua",
      "field.lua" }) do
      t.ok(fe.harness.reload("@fe/" .. file), file)
    end
    t.eq(#fe.dev.errors(), 0, "no reload errors")
    t.eq({ updated = game.updated:count(), drawn = game.drawn:count(), shown = dialogue.line_shown:count(),
      changed = quest.changed:count(), verbs = #field.verbs }, before)
    t.eq(field.remember_location, require("fe.field.anchors").remember_location,
      "anchors still decide the saved location")
    t.ok(field.types.traverse and field.zone_types.crouch and field.zone_types.no_save and field.zone_types.hint)
    finish()
  end,

  ["show_route draws a path line for a while and never moves the player"] = function(t)
    start()
    quest.start("beacon")
    quest.complete_objective("beacon", "annex")
    field.player:place({ x = 14.5, z = 6.5 }, 0)
    t.tick(1)
    local before = field.player.position
    local cells = hint.show_route()
    t.ok(cells and #cells > 10, "a route to the current waypoint")
    local last = cells[#cells]
    t.eq({ last.x, last.z }, { 40, 6 })
    t.ok(hint.route() ~= nil)
    hint.draw_route()
    t.tick(240)
    t.ok(hint.route() ~= nil)
    t.tick(80)
    t.eq(hint.route(), nil, "gone after five seconds")
    t.near(field.player.position.x, before.x, 1e-4, "the player did not move")
    t.eq(hint.show_route({ x = 11.5, z = 6.5 }), nil, "no route onto a wall")
    finish()
  end,
}
