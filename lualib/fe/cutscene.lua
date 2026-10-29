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
--- Everything an actor call creates (emote balloons) is removed if the
--- task is cancelled. `cutscene.play` adds letterboxing and locks field
--- control; both are undone however the cutscene ends.
---
--- Inside `cutscene.play`:
---   * the cutscene follows the field world: it freezes while the world is
---     paused and speeds up with it;
---   * holding the "skip" action (`dialogue.style.fast_forward_input`)
---     fast-forwards: the world runs `settings.fast_forward` times faster
---     and the cutscene's waits with it (they run on the world's time, so
---     the factor applies once; with no world the cutscene task's own clock
---     is sped up instead), and lines advance on their own;
---   * with `skippable = true` (or `settings.skippable`), pressing
---     `settings.skip_input` ("menu") asks whether to skip. Skipping fades
---     out and runs the rest of the cutscene at once: waits end, walks and
---     jumps arrive, lines go only to the backlog, so every state change
---     the cutscene makes still happens. The screen stays covered until the
---     cutscene ends (`field.warp` leaves it covered). A `dialogue.choice`
---     ends the skip: the screen fades back in, the choice is asked and the
---     scene plays on from there.
local dialogue = require("fe.dialogue")
local game = require("fe.game")
local i18n = require("fe.i18n")
local task = require("fe.task")
local transition = require("fe.transition")

local cutscene = {
  settings = {
    fast_forward = 3,       -- speed while fast-forwarding
    skippable = false,      -- default for cutscene.play's `skippable`
    skip_input = "menu",    -- asks to skip a skippable cutscene
  },
}

i18n.defaults({ fe = { cutscene = { skip = "Skip this scene?", yes = "Skip", no = "Continue" } } })

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
-- Arrive at once when the cutscene is skipped.
local function settle(entity, goal, current)
  if task.skipping(current) and entity:valid() then
    entity:stop()
    entity:place(goal)
  end
end

local function wait_still(entity, timeout)
  local current = task.current()
  task.wait_until(function() return not entity:moving() or task.skipping(current) end, timeout)
end

function Actor:walk_to(target, options)
  options = options or {}
  local goal = point_of(target)
  if task.skipping() then
    settle(self.entity, goal)
    return
  end
  if options.speed then self.entity:mover({ speed = options.speed }) end
  self.entity:move_to(goal, { path = options.path ~= false })
  wait_still(self.entity, options.timeout or 30)
  settle(self.entity, goal)
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
  task.ensure(function() task.wait(duration or 1.0) end, function()
    if balloon:valid() then balloon:destroy() end
  end)
end

function Actor:jump_to(target, height, duration)
  local goal = point_of(target)
  if not task.skipping() then
    self.entity:jump(goal, { height = height or 1, duration = duration or 0.5 })
    wait_still(self.entity, 5)
  end
  settle(self.entity, goal)
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

--- Run functions at the same time and wait for all of them (an error in
--- one cancels the others and is raised here).
function cutscene.parallel(fns)
  task.all(fns)
end

cutscene.wait = task.wait

--- Wait for the player to press Confirm.
function cutscene.wait_input(action)
  task.wait_until(function() return fe.input.pressed(action or "confirm") end)
end

--- Ask whether to skip; true when the player agrees. The cutscene's world
--- is paused meanwhile. Replace to restyle.
---@param world? World
---@return boolean
function cutscene.confirm_skip(world)
  local resume = world and game.pause(world) or function() end
  local ui = require("fe.ui")
  local index = ui.choose({ i18n.t("fe.cutscene.yes"), i18n.t("fe.cutscene.no") },
    { title = i18n.t("fe.cutscene.skip"), cancel = 2, width = 130 })
  resume()
  return index == 1
end

-- `skip.covered` is true from the moment a skip starts covering the screen
-- until something uncovers it.
local function skip_scene(owner, world, skip)
  local resume = world and game.pause(world) or function() end
  skip.covered = true
  transition.out("fade", 0.25)
  owner.skipping = true
  owner.on_end_skip = function()
    transition.into("fade", 0.25)
    skip.covered = false
  end
  dialogue.finish_line(owner)
  resume()
end

-- Fast-forward and the skip prompt, for the cutscene task `owner`. With a
-- world, fast-forward speeds the world up and the cutscene (bound to it)
-- follows; without one it speeds up the owner's clock. Never both: a bound
-- task already runs on its world's time.
local function watch(owner, world, skippable, skip)
  task.bind(false)
  local key = "fe.cutscene.fast_forward"
  task.finally(function()
    owner.speed = nil
    game.scale_time(key, nil)
  end)
  while true do
    local fast = not owner.skipping and dialogue.fast_forwarding()
    local scale = fast and cutscene.settings.fast_forward or nil
    if world then
      game.scale_time(key, scale, world)
    else
      owner.speed = scale
    end
    if skippable and not owner.skipping and fe.input.pressed(cutscene.settings.skip_input) then
      owner.speed = nil
      game.scale_time(key, nil)
      if cutscene.confirm_skip(world) then
        skip_scene(owner, world, skip)
      end
    end
    task.frame()
  end
end

--- Run a cinematic in the current task: lock field control, letterbox in,
--- run `fn`, close its message box, letterbox out, unlock. If `fn` raises
--- or the task is cancelled, the bars are removed at once and the lock is
--- released (a camera held with `rig:pan_to` is released when the task
--- ends). The cutscene follows `options.world` (default the field's) and
--- can be fast-forwarded or, when skippable, skipped (see the header).
---@param fn fun()
---@param options? {letterbox?: boolean, skippable?: boolean, world?: World}
function cutscene.play(fn, options)
  options = options or {}
  local letterbox = options.letterbox ~= false
  local skippable = options.skippable
  if skippable == nil then skippable = cutscene.settings.skippable end
  local field = require("fe.field")
  local world = options.world or field.world
  local owner = task.current():owner()
  local unlock = field.lock()
  local unbind = world and task.bind(world) or function() end
  local skip = { covered = false }
  local watcher = task.named("cutscene:watch", watch, owner, world, skippable, skip)
  local finished = false
  task.ensure(function()
    if letterbox then transition.letterbox(true, 0.4) end
    fn()
    dialogue.close()
    watcher:cancel()
    owner.skipping, owner.on_end_skip = nil, nil
    if skip.covered then
      if letterbox then fe.screen.set({ letterbox = 0 }) end
      transition.into("fade", 0.25)
      skip.covered = false
    elseif letterbox then
      transition.letterbox(false, 0.4)
    end
    finished = true
  end, function()
    watcher:cancel()
    owner.skipping, owner.on_end_skip = nil, nil
    if skip.covered then transition.clear() end
    unbind()
    unlock()
    if letterbox and not finished then fe.screen.set({ letterbox = 0 }) end
  end)
end

--- Whether the current cutscene is being skipped.
---@return boolean
function cutscene.skipping()
  return task.skipping()
end

return cutscene
