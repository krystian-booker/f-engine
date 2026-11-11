--- Rendering showcase used by replays/render.json: glTF crates with normal
--- and emissive maps (two drawn as one instanced draw, one with the game mesh
--- shader shaders/fs_glint.sc), a normal-mapped sprite with a glow mask and a
--- copy with the game sprite shader shaders/fs_hologram.sc, point and spot
--- lights that cast shadows, additive sparks, a decal on the plaza path, a
--- silhouette for the player behind the houses, and reflective water next to
--- the town pond.
local daynight = require("fe.daynight")
local field = require("fe.field")

local showcase = {}

function showcase.setup()
  local world = field.world
  daynight.minutes_per_second = 0
  daynight.set_hour(0)
  world:set_environment({
    pixel_snap = true,
    water = { reflection = 1, refraction = 0.05 },
  })
  field.player:sprite({ silhouette = { 0.55, 0.85, 1, 0.75 } })
  world:spawn({ id = "fx_crate", position = { x = 8.2, y = 0, z = 10.3 }, yaw = 28.65,
                mesh = { model = "fx/crate.gltf" } })
  world:spawn({ id = "fx_crate_blue", position = { x = 8.7, y = 0, z = 8.4 }, yaw = -15, scale = 0.8,
                mesh = { model = "fx/crate.gltf", tint = { 0.45, 0.65, 1.8, 1 } } })
  world:spawn({ id = "fx_crate_glint", position = { x = 7.6, y = 0, z = 7.9 }, yaw = 10,
                mesh = { model = "fx/crate.gltf", shader = "glint" } })
  world:spawn({ id = "fx_rook", position = { x = 10.6, y = 0, z = 9.4 },
                sprite = { animation = "fx/rook_lit.anim.json" } })
  world:spawn({ id = "fx_hologram", position = { x = 11.6, y = 0, z = 8.9 },
                sprite = { animation = "fx/rook_lit.anim.json", shader = "hologram", unlit = true } })
  world:spawn({ id = "fx_lamp", position = { x = 8.7, y = 0, z = 9.3 },
                light = { position = { 0, 0.6, 0 }, color = { 1, 0.62, 0.3 }, radius = 5, intensity = 3,
                          shadows = true } })
  world:spawn({ id = "fx_spot", position = { x = 11.2, y = 0, z = 10.8 },
                light = { type = "spot", position = { 0, 2.5, 0 }, direction = { -0.6, -1, -0.5 },
                          color = { 0.6, 0.8, 1 }, radius = 7, intensity = 4, inner_cone = 22, outer_cone = 34,
                          shadows = true } })
  world:spawn({ id = "fx_sparks", position = { x = 9.2, y = 0.2, z = 10.4 },
                emitter = { rate = 60, lifetime = 1.2, velocity = { 0, 1.3, 0 }, velocity_jitter = { 0.5, 0.3, 0.5 },
                            size = 0.07, size_end = 0.02, color = { 4, 2, 0.8, 1 }, color_end = { 2, 0.5, 0.1, 0 },
                            blend = "additive" } })
  world:spawn({ id = "fx_pond", position = { x = 12.5, y = 0.03, z = 12.2 }, keep_height = true,
                mesh = { model = "fx/pond.gltf", cast_shadow = false } })
  world:spawn({ id = "fx_circle", position = { x = 9.7, y = 0, z = 7.6 },
                sprite = { animation = "fx/circle.anim.json", mode = "decal", blend = "additive", unlit = true,
                           tint = { 0.5, 1.2, 2.4, 1 }, cast_shadow = false } })
end

return showcase
