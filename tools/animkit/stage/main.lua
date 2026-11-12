--- The stage review.py photographs a model's animations on: a grey floor
--- under even light, the model at its centre, and the shots listed in
--- shots.json. Each shot plays one animation from its start, runs it to the
--- time `at` and holds it there while the frame is captured. The times the
--- engine really reached are written to times.json (in the user files
--- folder), so the review can say which moment each picture shows.
local main = {}
local plan, world, model, tick = nil, nil, nil, 0
local reached = {}
local TICK = 1 / 60

function main.init()
  -- review.py writes shots.json; without it the stage is just the stage.
  plan = fe.fs.exists("shots.json") and fe.json.read("shots.json") or { height = 1.5, distance = 8, shots = {} }
  world = fe.world.load("stage", "stage")
  if plan.model then
    model = world:spawn { position = { x = 20, y = plan.lift or 0, z = 20 }, keep_height = true, mesh = { model = plan.model } }
  end
  -- A hero's height beside it, for scale.
  world:spawn { position = { x = 20 + (plan.hero_x or 1.5), y = 0, z = 20 + (plan.hero_z or 1) },
    sprite = { animation = "hero.anim.json", mode = "standing" } }
end

local function aim(shot)
  local camera = world:camera()
  camera.target = { x = 20, y = plan.target_y or plan.height * 0.5, z = 20 }
  camera.yaw, camera.pitch, camera.distance, camera.fov = shot.yaw, shot.pitch, plan.distance, 30
  camera.far = 2000
  camera:snap()
end

function main.update()
  tick = tick + 1
  local last = 0
  for index, shot in ipairs(plan.shots) do
    last = math.max(last, shot.tick)
    -- A shot is set up `lead` ticks before it is taken: the animation starts
    -- held at 0, then runs to `at` in steps the mesh's speed allows (at most
    -- 1000 times real time), then is held again.
    local lead = shot.steps + 4
    if tick == shot.tick - lead then
      aim(shot)
      model:play(shot.animation, false)
      model:mesh({ speed = 0 })
    elseif tick > shot.tick - lead and tick <= shot.tick - lead + shot.steps then
      model:mesh({ speed = shot.at / (shot.steps * TICK) })
    elseif tick == shot.tick - lead + shot.steps + 1 then
      model:mesh({ speed = 0, playing = true })
    end
    if tick == shot.tick then
      reached[index] = model:mesh().time
      fe.app.capture(string.format("shot_%d.png", index))
    end
  end
  if #plan.shots > 0 and tick == last + 4 then fe.fs.user_write("times.json", fe.json.encode({ times = reached })) end
  if #plan.shots > 0 and tick > last + 8 then fe.app.quit() end
end

function main.draw()
  world:draw()
end

return main
