--- Asset preview harness (tools/aetherbound/shot.py): loads a map, spawns
--- what shot.json lists, moves the camera and writes screenshots.
local main = {}
local shot, world, tick = nil, nil, 0

local function vec(t) return { x = t[1], y = t[2] or 0, z = t[3] or t[2] or 0 } end

function main.init()
  shot = fe.json.read("shot.json")
  world = fe.world.load("preview", shot.map)
  if shot.environment then world:set_environment(shot.environment) end
  local camera = world:camera()
  local c = shot.camera or {}
  camera.target = vec(c.target or { 8, 1, 8 })
  camera.yaw, camera.pitch = c.yaw or 0, c.pitch or 25
  camera.distance, camera.fov = c.distance or 14, c.fov or 30
  if c.far then camera.far = c.far end
  camera:snap()
  for _, e in ipairs(shot.entities or {}) do
    local spec = e.spawn or {}
    spec.position = vec(e.position or { 8, 0, 8 })
    if e.facing then spec.facing = { x = e.facing[1], z = e.facing[2] } end
    spec.keep_height = e.keep_height
    local entity = world:spawn(spec)
    if e.clip then entity:clip(e.clip, true) end
    if e.play then entity:play(e.play, e.loop ~= false) end
  end
end

function main.update()
  tick = tick + 1
  for index, at in ipairs(shot.frames or { 30 }) do
    if tick == at then fe.app.capture(string.format("shot_%d.png", index)) end
  end
  local last = (shot.frames or { 30 })[#(shot.frames or { 30 })]
  if tick > last + 6 then fe.app.quit() end
end

function main.draw()
  world:draw()
  if shot.label then fe.canvas.text(shot.label, 8, 8) end
end

return main
