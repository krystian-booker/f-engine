-- fe.vehicle on the arena: a canoe that only moves over the water cells at
-- (2..3, 7..8); the party's own mover is kept off the water.
local encounter = require("fe.encounter")
local field = require("fe.field")
local follower = require("fe.follower")
local input = require("fe.input")
local save = require("fe.save")
local scene = require("fe.scene")
local state = require("fe.state")
local ui = require("fe.ui")
local vec = require("fe.vec")
local vehicle = require("fe.vehicle")

local SPARK = "@fe/assets/ui/spark.anim.json"
local placement = { id = "boat", type = "vehicle", position = { x = 2.5, z = 7.5 }, layer = 0, props = { vehicle = "canoe" } }

local function fresh()
  input.use_defaults()
  state.reset()
  scene.clear()
  field.reset()
  ui.clear_toasts()
  vehicle.define("canoe", { sprite = SPARK, required_tags = { "water" }, speed = 3 })
end

local function press(t, key)
  fe.input.feed(key, 1)
  t.tick(1)
  fe.input.feed(key, 0)
  t.tick(2)
end

local function hold(t, key, ticks)
  fe.input.feed(key, 1)
  t.tick(ticks)
  fe.input.feed(key, 0)
  t.tick(2)
end

-- The field on the arena with the party kept off the water, the player on
-- the shore west of the lake and the canoe spawned from its placement.
local function launch(t)
  field.settings.player_mover = { blocked_tags = { "water" } }
  field.settings.autosave = false
  field.start("arena", "start")
  field.player:place({ x = 1.5, z = 7.5 }, 0)
  field.player:face("east")
  local boat = vehicle.placement(field.world, placement)
  table.insert(field.entities(), boat)
  t.tick(2)
  return boat
end

local function done()
  field.settings.player_mover = nil
  field.settings.autosave = true
  scene.clear()
end

local function is_water(world, position, layer)
  local cell = world:cell(vec.cell_of(position, layer))
  return cell ~= nil and require("fe.tablex").contains(cell.tags, "water")
end

return {
  ["the party boards from the shore, drives on water only and lands facing away"] = function(t)
    fresh()
    follower.join("pal", { name = "Pal", sprite = SPARK })
    local boat = launch(t)
    t.eq(field.player:mover().blocked_tags, { "water" }, "settings.player_mover reaches the player's mover")
    t.eq(boat:mover().required_tags, { "water" }, "the vehicle's mover carries its tags")
    t.near(boat.position.x, 2.5, 0.01, "spawned at its placement the first time")
    local target, verbs = field.target("confirm")
    t.eq(target, boat)
    t.eq(verbs and verbs[1].id, "board")
    press(t, "key:Z")
    local current, id = vehicle.current()
    t.eq(current, boat)
    t.eq(id, "canoe")
    t.ok(not field.player:sprite().visible, "the player rides hidden")
    t.ok(not field.followers()[1]:sprite().visible, "followers hide")
    t.near(vec.flat_distance(field.player.position, boat.position), 0, 0.01, "and the player follows the vehicle")
    t.ok(field.can_control())
    hold(t, "key:Right", 60)
    t.ok(boat.position.x > 3.0, "the canoe moved east over the water: x = " .. boat.position.x)
    t.ok(boat.position.x < 4.0, "but not onto the land at x = 4: x = " .. boat.position.x)
    t.near(vec.flat_distance(field.player.position, boat.position), 0, 0.01)
    hold(t, "key:Down", 40)
    t.ok(boat.position.z > 8.0, "south across the lake: z = " .. boat.position.z)
    hold(t, "key:Up", 60)
    t.ok(boat.position.z >= 7.0, "the north shore stops it: z = " .. boat.position.z)
    t.ok(is_water(field.world, boat.position, boat.layer), "still on water")
    local before = { x = boat.position.x, z = boat.position.z }
    target, verbs = field.target("confirm")
    t.eq(target, boat)
    t.eq(verbs and verbs[1].id, "leave_vehicle", "Confirm offers Leave while aboard")
    local lefts = {}
    local disconnect = vehicle.left:connect(function(vid, _, cell) lefts[#lefts + 1] = { vid, cell } end)
    press(t, "key:X")
    disconnect()
    t.eq(vehicle.current(), nil)
    t.eq(#lefts, 1)
    t.eq(lefts[1][1], "canoe")
    local p = field.player.position
    t.ok(not is_water(field.world, p, field.player.layer), "the player stands on land")
    t.ok(vec.flat_distance(p, before) <= 2, "next to the canoe")
    local away = { x = p.x - before.x, z = p.z - before.z }
    t.ok(field.player.facing.x * away.x + field.player.facing.z * away.z > 0, "facing away from it")
    t.ok(field.player:sprite().visible)
    t.ok(field.followers()[1]:sprite().visible, "followers are back")
    t.near(boat.position.x, before.x, 0.01, "the canoe stays where it was left")
    local saved = vehicle.saved("canoe")
    t.eq(saved.map, "arena")
    t.near(saved.x, before.x, 0.01)
    t.ok(field.can_control())
    follower.leave("pal")
    done()
  end,

  ["leaving is refused with no shore in reach, and tells the player"] = function(t)
    fresh()
    local boat = launch(t)
    t.ok(vehicle.board(boat))
    vehicle.defs.canoe.leave_search = 0.9
    t.eq(vehicle.leave(), false)
    t.eq(vehicle.current(), boat, "still aboard")
    t.eq(ui.toasts()[1] and ui.toasts()[1].text, "There's nowhere to land here.")
    t.ok(not field.player:sprite().visible)
    vehicle.defs.canoe.leave_search = nil
    t.ok(vehicle.leave())
    t.eq(vehicle.current(), nil)
    ui.clear_toasts()
    done()
  end,

  ["a save made aboard resumes aboard; the vehicle keeps its spot across map starts"] = function(t)
    fresh()
    save.delete("vehicle_test")
    local boat = launch(t)
    t.ok(vehicle.board(boat))
    hold(t, "key:Down", 40)
    local at = { x = boat.position.x, z = boat.position.z }
    t.ok(at.z > 8, "drove south first")
    save.write("vehicle_test")
    fresh()
    t.eq(vehicle.current(), nil)
    t.ok(save.load("vehicle_test"))
    field.settings.player_mover = { blocked_tags = { "water" } }
    t.ok(field.resume())
    t.tick(2)
    local current, id = vehicle.current()
    t.ok(current ~= nil, "aboard again")
    t.eq(id, "canoe")
    t.near(current.position.x, at.x, 0.05)
    t.near(current.position.z, at.z, 0.05)
    t.near(vec.flat_distance(field.player.position, current.position), 0, 0.01, "the player rides it")
    t.ok(not field.player:sprite().visible)
    hold(t, "key:Up", 30)
    t.ok(current.position.z < at.z, "and can drive on")
    t.ok(vehicle.leave())
    local left_at = { x = current.position.x, z = current.position.z }
    -- Away and back: the placement spawns the canoe where it was left.
    field.start("arena", "east")
    t.eq(vehicle.entity("canoe"), nil, "no placement on this start: nothing spawned")
    local again = vehicle.placement(field.world, placement)
    t.ok(again ~= nil)
    t.near(again.position.x, left_at.x, 0.01)
    t.near(again.position.z, left_at.z, 0.01)
    t.eq(vehicle.current(), nil, "on foot after leaving")
    -- Recorded on another map: not spawned here.
    vehicle.saved("canoe").map = "elsewhere"
    t.eq(vehicle.placement(field.world, placement), nil)
    -- Aboard a vehicle of another map: the party arrives on foot.
    vehicle.saved("canoe").map = "arena"
    again:destroy()
    local back = vehicle.placement(field.world, placement)
    table.insert(field.entities(), back)
    t.ok(vehicle.board(back))
    vehicle.saved("canoe").map = "elsewhere"
    field.start("arena", "start")
    t.eq(vehicle.current(), nil)
    t.eq(vehicle.placement(field.world, placement), nil, "the vehicle stayed on the other map")
    t.ok(field.player:sprite().visible)
    save.delete("vehicle_test")
    done()
  end,

  ["encounters at sea use the map's encounters_sea table; hooks survive a reload"] = function(t)
    fresh()
    local boat = launch(t)
    local calls = 0
    local base = function() calls = calls + 1 return "land", 1, {} end
    local sea_world = { props = function() return { encounters_sea = "sea" } end }
    local land_world = { props = function() return {} end }
    t.eq(vehicle.hooked.table_at(base, sea_world, boat.position, 0), "land", "ashore: the usual lookup")
    t.ok(vehicle.board(boat))
    t.eq(vehicle.hooked.table_at(base, sea_world, boat.position, 0), "sea")
    t.eq(vehicle.hooked.table_at(base, land_world, boat.position, 0), "land", "a map without a sea table")
    t.eq(calls, 2)
    t.eq(encounter.table_at, vehicle.hooks.table_at.wrapper, "installed on fe.encounter")
    t.eq(field.control, vehicle.hooks.control.wrapper)
    local wrappers = { field.control, field.update_followers, encounter.table_at }
    t.ok(fe.harness.reload("@fe/vehicle.lua"))
    t.ok(fe.harness.reload("@fe/field.lua"))
    vehicle.install()
    t.eq(encounter.table_at, wrappers[3], "a reload wraps nothing twice")
    t.eq(field.control, vehicle.hooks.control.wrapper)
    t.eq(field.update_followers, vehicle.hooks.update_followers.wrapper)
    t.ok(vehicle.hooks.control.base ~= wrappers[1] and vehicle.hooks.update_followers.base ~= wrappers[2],
      "no wrapper wraps a wrapper")
    t.eq(vehicle.current(), boat, "still aboard")
    hold(t, "key:Right", 40)
    t.ok(boat.position.x > 3.0, "the reloaded control still drives: x = " .. boat.position.x)
    press(t, "key:X")
    t.eq(vehicle.current(), nil, "and the reloaded verbs still leave")
    t.ok(field.can_control())
    t.eq(#fe.dev.errors(), 0)
    done()
  end,
}
