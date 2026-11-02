-- fe.field.anchors: saves resume at the nearest valid anchor; no_save
-- zones refuse saving with a reason the menu shows.
local anchors = require("fe.field.anchors")
local field = require("fe.field")
local input = require("fe.input")
local menu = require("fe.ui.screens.menu")
local scene = require("fe.scene")
local state = require("fe.state")
local task = require("fe.task")

local location = state.section("field")

local function start(map)
  input.use_defaults()
  state.reset()
  scene.clear()
  field.reset()
  field.settings.autosave = false
  field.start(map or "trail", "start")
end

local function at(t, x, z)
  field.player:place({ x = x, z = z }, 0)
  t.tick(1)
end

return {
  ["the saved location is the nearest valid anchor on maps that have them"] = function(t)
    start()
    t.ok(anchors.has_any(field.world))
    at(t, 7.5, 8.5)
    t.eq(location.anchor, "safe_west")
    t.near(location.x, 4.5, 1e-4)
    t.near(location.z, 6.5, 1e-4)
    at(t, 33.5, 6.5)
    t.eq(location.anchor, "safe_west", "an anchor whose condition fails is skipped")
    state.set_flag("east_open")
    t.tick(1)
    t.eq(location.anchor, "safe_east", "and used once it holds")
    t.near(location.facing.x, -1, 1e-4, "with the anchor's facing")
    at(t, 21.5, 4.5)
    t.eq(location.anchor, "safe_east", "map props anchors need their condition too")
    state.set_flag("rest_known")
    t.tick(1)
    t.eq(location.anchor, "rest", "a point listed in the map's props.anchors")
    local snapshot = state.snapshot()
    start("arena")
    state.restore(snapshot)
    t.ok(field.resume())
    t.near(field.player.position.x, 20.5, 1e-3, "resuming puts the player on the anchor")
    t.near(field.player.position.z, 3.5, 1e-3)
    start("arena")
    t.ok(not anchors.has_any(field.world))
    at(t, 4.25, 6.75)
    t.near(location.x, 4.25, 1e-4, "a map without anchors stores the exact position")
    t.eq(field.remember_location, anchors.remember_location)
    scene.clear()
  end,

  ["no_save zones refuse saving with a reason the menu shows"] = function(t)
    start()
    at(t, 8.5, 6.5)
    t.eq(anchors.can_save(), true)
    t.eq(menu.allow_save(), true)
    at(t, 24.5, 6.5)
    local ok, reason = anchors.can_save()
    t.eq(ok, false)
    t.eq(reason, "Not while the bridge moves.")
    local job = task.spawn(menu.open)
    t.tick(1)
    local save_item
    for _, item in ipairs(menu.view.list.items) do
      if item.value.id == "save" then save_item = item end
    end
    t.eq(save_item.enabled, false, "the Save page is disabled")
    t.eq(save_item.reason, "Not while the bridge moves.", "with the zone's reason")
    fe.input.feed("key:Escape", 1)
    t.tick(1)
    fe.input.feed("key:Escape", 0)
    t.tick(2)
    t.wait_until(function() return job.status ~= "running" end, 30)
    t.eq(job.status, "done")
    local saved = menu.allow_save
    menu.allow_save = function() return false end
    t.eq(menu.page("save").enabled(), false, "a plain false still works")
    menu.allow_save = saved
    scene.clear()
  end,
}
