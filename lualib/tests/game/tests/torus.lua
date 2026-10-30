-- The torus map: the arena with `grid.wrap` and open outer rows, so walking
-- off the east edge arrives on the west and the party, camera and queries
-- follow the short way round.
local field = require("fe.field")
local follower = require("fe.follower")
local input = require("fe.input")
local scene = require("fe.scene")
local state = require("fe.state")
local vec = require("fe.vec")

local SPARK = "@fe/assets/ui/spark.anim.json"

local function hold(t, key, ticks)
  fe.input.feed(key, 1)
  t.tick(ticks)
  fe.input.feed(key, 0)
  t.tick(2)
end

return {
  ["walking east off the torus arrives on the west with the camera and party in tow"] = function(t)
    input.use_defaults()
    state.reset()
    scene.clear()
    field.reset()
    field.settings.autosave = false
    local errors = #fe.dev.errors()
    follower.join("pal", { name = "Pal", sprite = SPARK })
    field.start("torus", "start")
    local world, player = field.world, field.player
    t.ok(world:wraps(), "the torus wraps")
    local width = world:grid_size()
    t.eq(width, 12)
    local folded = world:wrap_point({ x = -0.4, z = 12.5 })
    t.near(folded.x, 11.6, 1e-5)
    t.near(folded.z, 2.5, 1e-5)
    t.eq(world:wrap_delta({ x = 11.5, y = 0, z = 1 }, { x = 0.5, y = 2, z = 9 }), { x = 1, y = 2, z = -2 })
    player:place({ x = 11.6, z = 5.5 }, 0)
    player:face("east")
    t.tick(60) -- the followers settle behind and the camera catches up
    hold(t, "key:Right", 30)
    local p = player.position
    t.ok(p.x >= 0 and p.x < 1.5, "the player crossed the seam onto the west side: x = " .. p.x)
    t.near(p.z, 5.5, 0.05)
    t.ok(field.rig.wraps, "the rig knows the map wraps")
    t.tick(60)
    local to_camera = world:wrap_delta(player.position, field.rig.cam.target)
    t.ok(math.abs(to_camera.x) < 2 and math.abs(to_camera.z) < 2,
      "the camera followed across the seam: dx = " .. to_camera.x .. ", dz = " .. to_camera.z)
    local cam = field.rig.cam.target
    t.ok(cam.x >= 0 and cam.x < 12, "and its target is canonical: x = " .. cam.x)
    local pal = field.followers()[1]
    t.ok(pal and pal:valid())
    t.ok(vec.flat_distance_in(world, player.position, pal.position) < 2.5,
      "the follower is close behind: " .. vec.flat_distance_in(world, player.position, pal.position))
    t.eq(#world:query({ tag = "player", near = player.position, radius = 1 }), 1, "queries measure the short way")
    local plain = fe.world.load("plain", "arena")
    t.ok(not plain:wraps(), "the arena does not wrap")
    t.eq(plain:wrap_delta({ x = 11.5, z = 1 }, { x = 0.5, z = 1 }), { x = -11, y = 0, z = 0 })
    fe.world.unload("plain")
    t.eq(#fe.dev.errors(), errors, "no script errors")
    follower.leave("pal")
    field.settings.autosave = true
    scene.clear()
  end,
}
