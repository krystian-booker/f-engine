--- The stage review.py photographs a model on: a grey floor under even
--- light, the model at its centre, and the shots listed in shots.json.
local main = {}
local plan, world, model, tick = nil, nil, nil, 0

function main.init()
  -- review.py writes shots.json; without it the stage is just the stage.
  plan = fe.fs.exists("shots.json") and fe.json.read("shots.json") or { height = 1.5, distance = 8, shots = {} }
  world = fe.world.load("stage", "stage")
  if plan.model then
    model = world:spawn { position = { x = 20, y = 0, z = 20 }, mesh = { model = plan.model } }
  end
  -- A hero's height beside it, for scale.
  world:spawn { position = { x = 20 + math.max(1.2, plan.height * 0.55), y = 0, z = 21 },
    sprite = { animation = "hero.anim.json", mode = "standing" } }
end

local function aim(shot)
  local camera = world:camera()
  camera.target = { x = 20, y = plan.height * 0.5, z = 20 }
  camera.yaw, camera.pitch, camera.distance, camera.fov = shot.yaw, shot.pitch, plan.distance, 30
  camera.far = 2000
  camera:snap()
end

function main.update()
  tick = tick + 1
  local last = 0
  for index, shot in ipairs(plan.shots) do
    last = math.max(last, shot.tick)
    -- Each shot is set up a few ticks before it is taken.
    if tick == shot.tick - 8 then
      aim(shot)
      if shot.animation then
        model:play(shot.animation, false)
        model:mesh({ speed = 0 })
      end
    end
    if shot.animation and tick == shot.tick - 7 then
      -- Half-way through the animation, held still.
      model:mesh({ speed = (shot.at or 0) * 60 })
    end
    if shot.animation and tick == shot.tick - 6 then model:mesh({ speed = 0 }) end
    if tick == shot.tick then fe.app.capture(string.format("shot_%d.png", index)) end
  end
  if #plan.shots > 0 and tick > last + 6 then fe.app.quit() end
end

function main.draw()
  world:draw()
end

return main
