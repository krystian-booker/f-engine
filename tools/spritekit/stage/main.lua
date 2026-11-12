--- The stage review.py photographs a sprite on: nothing but the sprite, a
--- dim ambient light and one point light that the shots move, so a normal
--- map shows what it does. shots.json (written by spritekit/draw.py) lists
--- the shots; without it the stage is just the stage.
local main = {}
local plan, world, sprite, lamp, bar, tick = nil, nil, nil, nil, nil, 0
local turned = {}
local CENTRE = { x = 8, z = 8 }
local FACINGS = { { 0, 1 }, { -1, 0 }, { 0, -1 }, { 1, 0 }, { -1, 1 }, { -1, -1 }, { 1, -1 }, { 1, 1 } }

local function spawn(x, facing)
  local settings = {}
  for key, value in pairs(plan.settings or {}) do settings[key] = value end
  settings.animation, settings.clip, settings.cast_shadow, settings.visible = plan.sprite, "idle", false, false
  return world:spawn { position = { x = x, y = plan.lift or 0, z = CENTRE.z }, keep_height = true, facing = { x = facing[1], z = facing[2] },
    sprite = settings }
end

function main.init()
  plan = fe.fs.exists("shots.json") and fe.json.read("shots.json") or { height = 1.5, directions = 1, shots = {} }
  world = fe.world.load("stage", "stage")
  lamp = world:spawn { position = { x = CENTRE.x, y = 1, z = CENTRE.z + 1 }, keep_height = true,
    light = { color = { 1.0, 0.8, 0.55 }, radius = 8, intensity = 5 } }
  if not plan.sprite then return end
  sprite = spawn(CENTRE.x, { 0, 1 })
  -- One copy per direction for the shot that shows them all, and a bar the height of a hero.
  local count = math.min(plan.directions or 1, 8)
  local step = math.max(1.1, (plan.height or 1.5) * 0.9)
  for index = 1, count > 1 and count or 0 do
    turned[index] = spawn(CENTRE.x + (index - (count + 1) / 2) * step, FACINGS[count == 4 and index or index] or { 0, 1 })
  end
  bar = world:spawn { position = { x = CENTRE.x - (count > 1 and (count - 1) / 2 * step + 0.6 or math.max(0.6, (plan.height or 1.5) * 0.5)), y = 0, z = CENTRE.z },
    sprite = { animation = "bar.anim.json", mode = "upright", unlit = true, cast_shadow = false, visible = false } }
end

local function prepare(shot)
  local camera = world:camera()
  local centre_y = shot.bar and math.max(0.9, (plan.height or 1.5) * 0.45) or (plan.height or 1.5) * 0.5
  camera.target = { x = CENTRE.x, y = centre_y, z = CENTRE.z }
  camera.yaw, camera.pitch, camera.distance, camera.fov = 0, shot.bar and 27 or 12, shot.distance or 6, 30
  camera:snap()
  local light = shot.light or { 0, 2, 2 }
  lamp:place({ x = CENTRE.x + light[1], y = light[2], z = CENTRE.z + light[3] }, 0, true)
  sprite:sprite({ visible = not shot.turn, outline = shot.outline and { 1.0, 0.85, 0.2, 1 } or { 0, 0, 0, 0 }, outline_width = 1 })
  if shot.facing then sprite:face({ x = shot.facing[1], z = shot.facing[2] }) end
  for _, copy in ipairs(turned) do copy:sprite({ visible = shot.turn == true }) end
  bar:sprite({ visible = shot.bar == true })
end

function main.update()
  if not sprite then return end
  tick = tick + 1
  local last = 0
  for index, shot in ipairs(plan.shots) do
    last = math.max(last, shot.tick)
    local lead = math.min(20, math.floor((shot.at or 0) * 60 + 0.5))
    if tick == shot.tick - 30 then prepare(shot) end
    -- The clip restarts `lead` ticks before the shot, so the shot shows the frame at that time.
    if tick == shot.tick - lead then
      sprite:clip(shot.clip or "idle", true)
      for _, copy in ipairs(turned) do copy:clip(shot.clip or "idle", true) end
    end
    if tick == shot.tick then fe.app.capture(string.format("shot_%d.png", index)) end
  end
  if #plan.shots > 0 and tick > last + 6 then fe.app.quit() end
end

function main.draw()
  if world then world:draw() end
end

return main
