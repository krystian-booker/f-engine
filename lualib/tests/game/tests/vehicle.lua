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

  ["an airship rises, crosses walls, lands only on walkable ground and comes down to leave"] = function(t)
    fresh()
    vehicle.define("airship", { sprite = SPARK, speed = 6, altitude = 2, altitude_speed = 8, leave_search = 1,
      camera = { zoom = 2, far = 600 } })
    field.settings.player_mover = { blocked_tags = { "water" } }
    field.settings.autosave = false
    field.start("arena", "start")
    local ship = vehicle.spawn(field.world, "airship", { x = 3.5, z = 5.5 }, 0)
    table.insert(field.entities(), ship)
    t.tick(2)
    local cam = field.world:camera()
    t.near(cam.far, 300)
    t.eq(ship:mover().lift, 0, "on the ground until boarded")
    t.ok(vehicle.board(ship))
    t.eq(ship:mover().altitude, 2)
    t.eq(field.rig.zoom, 2, "the definition's camera zoom applies")
    t.near(cam.far, 600, 1e-6, "and its far plane")
    t.tick(5)
    local lift = ship:mover().lift
    t.ok(lift > 0 and lift < 2, "rising: lift = " .. lift)
    t.near(ship.position.y, lift, 1e-4, "the body is where its lift says")
    t.wait_until(function() return ship:mover().lift >= 2 end, 60, "reaches its altitude")
    t.tick(1)
    t.near(ship.position.y, 2, 1e-4)
    t.near(field.player.position.y, 2, 1e-4, "the player rides at the ship's height")
    t.ok(vehicle.aboard())
    hold(t, "key:Right", 60)
    t.ok(ship.position.x > 6, "flew over the wall at x = 5: x = " .. ship.position.x)
    t.near(ship.position.y, 2, 1e-4)
    -- Over the middle of the lake nothing within a cell is walkable for the party.
    ship:place({ x = 3, z = 8 }, 0)
    t.tick(1)
    t.near(ship.position.y, 2, 1e-4, "place keeps the lift")
    t.eq(vehicle.leave(), false)
    t.eq(vehicle.current(), ship, "still aboard")
    t.eq(ui.toasts()[1] and ui.toasts()[1].text, "There's nowhere to land here.")
    ui.clear_toasts()
    -- Over grass it comes down first, then the party steps off.
    ship:place({ x = 2.5, z = 5.5 }, 0)
    t.tick(1)
    t.ok(vehicle.leave())
    t.eq(vehicle.current(), ship, "aboard while descending")
    t.ok(not field.can_control(), "and control is locked")
    t.eq(ship:mover().altitude, 0)
    t.tick(3)
    t.ok(vehicle.leave(), "asking again while descending changes nothing")
    lift = ship:mover().lift
    t.ok(lift > 0 and lift < 2, "descending: lift = " .. lift)
    t.wait_until(function() return vehicle.current() == nil end, 60, "landed and disembarked")
    t.near(ship.position.y, 0, 1e-4)
    t.near(field.player.position.y, 0, 1e-4, "the player stands on the ground")
    t.ok(field.player:sprite().visible)
    t.eq(field.rig.zoom, 1, "camera zoom restored")
    t.near(cam.far, 300, 1e-6, "far plane restored")
    t.tick(1)
    t.ok(field.can_control())
    t.eq(#fe.dev.errors(), 0)
    done()
  end,

  ["a mount carries a visible rider, turns encounters and touches off and slows on tagged ground"] = function(t)
    fresh()
    local def = vehicle.define("chocobo", { sprite = SPARK, fly = false, blocked_tags = { "water" }, speed = 4,
      rider = { visible = true, offset = { x = 0, y = 0.35, z = 0 } }, encounters = false,
      speed_on_tags = { deploy = 0.5 } })
    field.settings.player_mover = { blocked_tags = { "water" } }
    field.settings.autosave = false
    field.start("arena", "start")
    local bird = vehicle.spawn(field.world, "chocobo", { x = 3.5, z = 5.5 }, 0)
    table.insert(field.entities(), bird)
    t.tick(2)
    t.ok(vehicle.board(bird))
    t.tick(2)
    local player = field.player
    t.ok(player:sprite().visible, "the rider stays visible")
    t.near(player.position.y - bird.position.y, 0.35, 1e-4, "seated on its back")
    t.near(player:sprite().depth_bias, 0.02, 1e-6, "drawn over the mount")
    t.eq(bird:mover().lift, 0, "a mount does not fly")
    t.eq(field.rig.zoom, 1, "no camera settings: the rig is left alone")
    -- No encounters aboard: the base lookup is never asked.
    local calls = 0
    local base = function() calls = calls + 1 return "land", 1, {} end
    t.eq(vehicle.hooked.table_at(base, field.world, bird.position, 0), nil)
    t.eq(calls, 0)
    -- Touches and the verbs not flagged `aboard` are off while riding.
    local touched = 0
    local touch = function() touched = touched + 1 end
    vehicle.hooked.touch(touch, bird)
    t.eq(touched, 0)
    local all = { { id = "talk" }, { id = "leave_vehicle", aboard = true } }
    t.eq(vehicle.hooked.verbs_for(function() return all end, bird, "confirm", player), { all[2] })
    local target, verbs = field.target("confirm")
    t.eq(target, bird)
    t.eq(verbs and verbs[1].id, "leave_vehicle")
    -- Terrain speed: half as fast on the deploy cells at (1..2, 1..2).
    t.eq(vehicle.speed_factor(def, field.world:cell({ x = 1, z = 1, layer = 0 })), 0.5)
    t.eq(vehicle.speed_factor(def, field.world:cell({ x = 3, z = 5, layer = 0 })), 1)
    t.eq(vehicle.speed_factor(def, nil), 1)
    hold(t, "key:Left", 15)
    local on_grass = 3.5 - bird.position.x
    bird:place({ x = 2.5, z = 2.5 }, 0)
    t.tick(1)
    hold(t, "key:Left", 15)
    local on_deploy = 2.5 - bird.position.x
    t.ok(on_grass > 0.8 and on_grass < 1.1, "a second at speed 4 over 15 ticks on grass: " .. on_grass)
    t.near(on_deploy / on_grass, 0.5, 0.05, "half that on deploy ground")
    -- Kept off the water by its class.
    bird:place({ x = 4.5, z = 7.5 }, 0)
    t.tick(1)
    hold(t, "key:Left", 60)
    t.ok(bird.position.x >= 3.9, "stopped at the lake: x = " .. bird.position.x)
    -- A follower joining while aboard hides like the rest.
    follower.join("pal", { name = "Pal", sprite = SPARK })
    t.tick(1)
    t.ok(not field.followers()[1]:sprite().visible, "joined hidden")
    t.ok(vehicle.leave())
    t.eq(vehicle.current(), nil, "on foot at once: nothing to come down from")
    t.ok(field.followers()[1]:sprite().visible, "followers are back")
    t.near(player:sprite().depth_bias, 0, 1e-6, "depth bias reset")
    vehicle.hooked.touch(touch, bird)
    t.eq(touched, 1, "touches again on foot")
    t.eq(vehicle.hooked.table_at(base, field.world, bird.position, 0), "land")
    follower.leave("pal")
    t.eq(#fe.dev.errors(), 0)
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
    vehicle.defs.canoe.encounters = "lagoon"
    t.eq(vehicle.hooked.table_at(base, sea_world, boat.position, 0), "lagoon", "a definition's own table wins")
    vehicle.defs.canoe.encounters = nil
    t.eq(encounter.table_at, vehicle.hooks.table_at.wrapper, "installed on fe.encounter")
    local on_field = { "control", "update_followers", "spawn_followers", "verbs_for", "touch", "handle_events" }
    local wrappers = { table_at = encounter.table_at }
    for _, name in ipairs(on_field) do
      t.eq(field[name], vehicle.hooks[name].wrapper, name .. " installed on fe.field")
      wrappers[name] = field[name]
    end
    t.ok(fe.harness.reload("@fe/vehicle.lua"))
    t.ok(fe.harness.reload("@fe/field.lua"))
    vehicle.install()
    t.eq(encounter.table_at, wrappers.table_at, "a reload wraps nothing twice")
    for _, name in ipairs(on_field) do
      t.eq(field[name], vehicle.hooks[name].wrapper, name .. " is still the wrapper")
      t.ok(vehicle.hooks[name].base ~= wrappers[name], "no wrapper wraps a wrapper: " .. name)
    end
    t.eq(vehicle.current(), boat, "still aboard")
    hold(t, "key:Right", 40)
    t.ok(boat.position.x > 3.0, "the reloaded control still drives: x = " .. boat.position.x)
    press(t, "key:X")
    t.eq(vehicle.current(), nil, "and the reloaded verbs still leave")
    t.ok(field.can_control())
    t.eq(#fe.dev.errors(), 0)
    done()
  end,

  ["zone handlers still run while aboard, touches do not"] = function(t)
    fresh()
    vehicle.define("airship", { sprite = SPARK, altitude = 2, altitude_speed = 0, speed = 6 })
    field.settings.autosave = false
    local visits = {}
    field.zone("probe", function(zone, entered) visits[#visits + 1] = { zone.id, entered } end)
    field.start("torus", "start")
    local ship = vehicle.spawn(field.world, "airship", { x = 3.5, z = 5.5 }, 0)
    field.player:place({ x = 2.5, z = 5.5 }, 0)
    t.ok(vehicle.board(ship))
    t.tick(2)
    hold(t, "key:Right", 70) -- east through the probe zone (x 6..8) and out the other side
    t.ok(ship.position.x > 8.5, "the airship flew past the zone: x = " .. ship.position.x)
    t.eq(visits, { { "probe", true }, { "probe", false } }, "the player's zone handler saw the pass")
    t.ok(vehicle.current() == ship, "still aboard")
    vehicle.disembark(ship, "airship", vehicle.landing(ship, field.player) or ship:cell())
    field.zone("probe", nil)
    done()
  end,
}
