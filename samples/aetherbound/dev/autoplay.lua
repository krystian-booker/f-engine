--- A player made of code, for replays: it presses the buttons a person
--- would (through `fe.input.feed`, so the game's own input code runs) to
--- read dialogue, walk Cael along the map's path, and fight.
---
--- ```json
--- {"tick": 5, "lua": "require('dev.autoplay').start()"}
--- {"tick": 6, "lua": "require('dev.autoplay').walk('enc1')"}
--- ```
--- `start()` talks and fights by itself from then on; `walk(zone or point)`
--- sends Cael somewhere whenever the player has control.
local battle = require("game.battle.scene")
local dialogue = require("game.dialogue")
local field = require("game.field")
local task = require("game.task")

local autoplay = { running = nil, goal = nil, fast = false }

--- One press every eight ticks (down for two): slow enough that the game
--- has shown what the last press did before the next one is decided.
local function tap(input_name, state)
  if state.held ~= input_name then
    if state.held then fe.input.feed(state.held, 0) end
    state.held, state.phase = input_name, 2
  end
  if input_name then
    state.phase = (state.phase + 1) % 8
    fe.input.feed(input_name, state.phase < 2 and 1 or 0)
  end
end

local function goal_position(world, name)
  local point = world:points()[name]
  if point then return point.position end
  for _, zone in ipairs(world:zones()) do
    if zone.id == name then return { x = (zone.min.x + zone.max.x) / 2, z = (zone.min.z + zone.max.z) / 2 } end
  end
  return nil
end

local function steer(walk)
  local keys = { "key:Left", "key:Right", "key:Up", "key:Down" }
  local want = {}
  local world, leader = field.world, field.leader()
  local target = world and leader and autoplay.goal and goal_position(world, autoplay.goal)
  if target and field.locked == 0 and not dialogue.active() then
    local p = leader.position
    if not walk.path or walk.goal ~= autoplay.goal then
      walk.path = world:path(leader:cell(), { x = math.floor(target.x), z = math.floor(target.z), layer = 0 })
      walk.goal, walk.index = autoplay.goal, 1
    end
    local step = walk.path[walk.index]
    while step and math.abs(step.x + 0.5 - p.x) < 0.35 and math.abs(step.z + 0.5 - p.z) < 0.35 do
      walk.index = walk.index + 1
      step = walk.path[walk.index]
    end
    local aim = step and { x = step.x + 0.5, z = step.z + 0.5 } or target
    local dx, dz = aim.x - p.x, aim.z - p.z
    if math.abs(dx) < 0.2 and math.abs(dz) < 0.2 and not step then
      autoplay.goal = nil
    else
      if dx < -0.15 then want["key:Left"] = true elseif dx > 0.15 then want["key:Right"] = true end
      if dz < -0.15 then want["key:Up"] = true elseif dz > 0.15 then want["key:Down"] = true end
    end
  else
    walk.path = nil
  end
  -- The arrow keys are the walk's only while it is walking: in a menu they
  -- belong to whoever is choosing commands.
  local walking = next(want) ~= nil
  if walking or walk.walking then
    for _, key in ipairs(keys) do fe.input.feed(key, want[key] and 1 or 0) end
  end
  walk.walking = walking
end

--- Which input the battle menu needs next: down to reach the wanted
--- command, confirm once it is under the cursor.
local function battle_input(scene)
  local menu = scene.menu
  if not menu then return nil end
  if menu.target or menu.sub then return "key:Return" end
  local wanted = 1
  local unit = menu.unit
  for index, command in ipairs(menu.commands) do
    if unit.only and command.skill == unit.only then wanted = index end
    if not unit.only and command.skill == "resonance" and (unit.corruption or 0) < 75 then wanted = index end
  end
  return menu.index == wanted and "key:Return" or "key:Down"
end

--- Play by itself until `autoplay.stop()`: confirm finished lines, pick
--- battle commands, walk to `autoplay.goal`. `fast` holds the dialogue skip.
function autoplay.start(fast)
  autoplay.stop()
  autoplay.fast = fast or false
  local state, walk = {}, {}
  autoplay.running = task.spawn(function()
    while true do
      fe.input.feed("key:Tab", autoplay.fast and 1 or 0)
      local scene = battle.active
      local line = dialogue.line
      local press
      if scene and scene.hint then
        press = "key:Return"
      elseif scene and scene.menu then
        press = battle_input(scene)
      elseif line and not line.locked and line.shown >= line.total and not line.auto then
        press = "key:Return"
      end
      tap(press, state)
      steer(walk)
      task.frame()
    end
  end, "autoplay")
end

function autoplay.walk(goal)
  autoplay.goal = goal
end

function autoplay.stop()
  if autoplay.running then task.cancel(autoplay.running) end
  autoplay.running, autoplay.goal = nil, nil
  for _, key in ipairs { "key:Left", "key:Right", "key:Up", "key:Down", "key:Return", "key:Tab" } do
    fe.input.feed(key, 0)
  end
end

return autoplay
