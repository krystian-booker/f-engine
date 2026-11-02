-- fe.camera: orthographic views from rig options, map props and zones.
local camera = require("fe.camera")
local field = require("fe.field")
local input = require("fe.input")
local scene = require("fe.scene")
local state = require("fe.state")

return {
  ["the rig takes orthographic views from options and map props and blends zone heights"] = function(t)
    input.use_defaults()
    state.reset()
    scene.clear()
    field.reset()
    field.settings.autosave = false
    field.start("trail", "start")
    local cam = field.world:camera()
    t.eq(cam.orthographic, true, "the map's camera props make the view orthographic")
    t.near(cam.ortho_height, 9, 1e-3)
    field.player:place({ x = 40.5, z = 6.5 }, 0)
    for _ = 1, 120 do field.rig:update(1 / 60) end
    t.near(cam.ortho_height, 5, 0.05, "a camera zone's ortho_height blends in")
    local mid
    field.player:place({ x = 30.5, z = 6.5 }, 0)
    field.rig:update(1 / 60)
    mid = cam.ortho_height
    t.ok(mid > 5 and mid < 9, "the height blends rather than jumps")
    for _ = 1, 120 do field.rig:update(1 / 60) end
    t.near(cam.ortho_height, 9, 0.05)
    field.rig:set_zoom(2)
    for _ = 1, 200 do field.rig:update(1 / 60) end
    t.near(cam.ortho_height, 18, 0.1, "zoom scales the height like the distance")
    local rig = camera.rig(field.world, { follow = field.player, orthographic = false })
    rig:update(1 / 60)
    t.eq(cam.orthographic, false, "rig options win over the map")
    scene.clear()
    field.settings.autosave = true
  end,

  ["a rig on a map without projection settings leaves the camera alone"] = function(t)
    input.use_defaults()
    state.reset()
    scene.clear()
    field.reset()
    field.start("arena", "start")
    local cam = field.world:camera()
    cam.orthographic = true
    cam.ortho_height = 7
    for _ = 1, 10 do field.rig:update(1 / 60) end
    t.eq(cam.orthographic, true)
    t.near(cam.ortho_height, 7, 1e-3)
    local rig = camera.rig(field.world, { follow = field.player, ortho_height = 4 })
    for _ = 1, 200 do rig:update(1 / 60) end
    t.near(cam.ortho_height, 4, 0.05, "an option alone sets the height")
    scene.clear()
  end,
}
