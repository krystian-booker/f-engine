--- Cutscene helpers: actors that walk, face, emote, animate and talk, with
--- every call waiting until it finishes. Use inside event scripts:
---
--- ```lua
--- local mira, guard = cutscene.actor("mira"), cutscene.actor("guard")
--- cutscene.parallel{
---   function() mira:walk_to({x = 8.5, z = 4.5}) end,
---   function() guard:emote("exclaim") end,
--- }
--- guard:say("Halt! Who goes there?")
--- ```
local dialogue = require("fe.dialogue")
local task = require("fe.task")
local transition = require("fe.transition")

local cutscene = {}

local Actor = {}
Actor.__index = Actor

local function resolve_world(world)
  return world or require("fe.field").world
end

--- Wrap an entity (or an entity id in the field world).
---@param entity Entity|string
---@param world? World
function cutscene.actor(entity, world)
  if type(entity) == "string" then
    local w = resolve_world(world)
    local found = w and w:find(entity)
    assert(found, "no entity '" .. entity .. "' in the current world")
    entity = found
  end
  return setmetatable({ entity = entity, name = (entity:props().name or entity.id) }, Actor)
end

local function point_of(target)
  if type(target) == "userdata" or (type(target) == "table" and target.entity) then
    local e = target.entity or target
    return e.position
  end
  if target.layer ~= nil then -- a grid cell: walk to its centre
    return { x = target.x + 0.5, z = target.z + 0.5 }
  end
  return target
end

--- Walk (pathfinding) to a position {x, z}, a cell {x, z, layer} (its
--- centre) or an entity, and wait.
function Actor:walk_to(target, options)
  options = options or {}
  local goal = point_of(target)
  if options.speed then self.entity:mover({ speed = options.speed }) end
  self.entity:move_to(goal, { path = options.path ~= false })
  task.wait_until(function() return not self.entity:moving() end, options.timeout or 30)
end

--- Walk relative to the current position.
function Actor:walk(dx, dz, options)
  local p = self.entity.position
  self:walk_to({ x = p.x + dx, z = p.z + dz }, options)
end

function Actor:face(target)
  if type(target) == "table" and target.entity then target = target.entity end
  self.entity:face(target)
end

--- Play a clip; with wait=true (default for non-looping clips) wait for it.
function Actor:anim(clip, options)
  options = options or {}
  self.entity:clip(clip)
  if options.wait ~= false then
    local info = fe.app.sprite_clips(self.entity:sprite().animation)[clip]
    if info and not info.loop then
      task.wait(info.length)
    end
  end
  if options.then_clip then self.entity:clip(options.then_clip) end
end

--- Show an emote balloon above the actor ("exclaim", "question", "music",
--- "sweat", "dots", "anger", "heart", "idea") and wait.
function Actor:emote(name, duration)
  local world = self.entity:world()
  local balloon = world:spawn({
    position = self.entity.position, keep_height = true,
    sprite = { animation = "@fe/assets/ui/emotes.anim.json", clip = name, unlit = true, cast_shadow = false, mode = "facing" },
  })
  balloon:attach(self.entity, { x = 0, y = 1.75, z = 0 })
  task.wait(duration or 1.0)
  balloon:destroy()
end

function Actor:jump_to(target, height, duration)
  local goal = point_of(target)
  self.entity:jump(goal, { height = height or 1, duration = duration or 0.5 })
  task.wait_until(function() return not self.entity:moving() end, 5)
end

--- Say a line with this actor as speaker.
function Actor:say(text, options)
  dialogue.say(self.name, text, options)
end

--- A speech bubble that does not wait.
function Actor:bark(text, duration)
  dialogue.bubble(self.entity, text, { duration = duration })
end

function Actor:hide() self.entity:sprite({ visible = false }) end
function Actor:show() self.entity:sprite({ visible = true }) end

--- Run functions at the same time and wait for all of them.
function cutscene.parallel(fns)
  task.all(fns)
end

cutscene.wait = task.wait

--- Wait for the player to press Confirm.
function cutscene.wait_input(action)
  task.wait_until(function() return fe.input.pressed(action or "confirm") end)
end

--- Run a cinematic: letterbox in, run `fn`, letterbox out. Field control is
--- disabled meanwhile because event scripts count as blocking.
function cutscene.play(fn, options)
  options = options or {}
  if options.letterbox ~= false then transition.letterbox(true, 0.4) end
  local ok, err = pcall(fn)
  dialogue.close()
  if options.letterbox ~= false then transition.letterbox(false, 0.4) end
  if not ok then error(err, 0) end
end

return cutscene
