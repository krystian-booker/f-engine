--- What the player does on the field: input while in control
--- (`field.control`: movement relative to the camera, verbs, commands and
--- the menu), verbs on the entity in front of them (`field.verb`,
--- `field.target`, `field.interact`), commands that need no target
--- (`field.command`, `field.run_command`) and touches (`field.touch`,
--- `field.retouch`). fe.field installs this module
--- (`interaction.install(field)`), which gives the field those functions
--- and the built-in "talk" verb; replace any of them on fe.field, or this
--- whole module with your own `fe/field/interaction.lua`.
---
--- Touch triggers only fire while `field.can_trigger()` (no locks, no
--- blocking event). A touch that happened under a lock fires once the lock
--- lifts if the player still stands on the entity (within
--- `settings.touch_range`); touches on the tick a map starts never do, so
--- arriving on a door does not warp back.
local event = require("fe.event")
local tablex = require("fe.tablex")
local task = require("fe.task")
local vec = require("fe.vec")

local interaction = {}

-- fe.field requires this module while it loads, so it is fetched when needed.
local function field()
  return require("fe.field")
end

-- Kept across hot reload: whether the map's first tick has passed and the
-- touches suppressed under a lock.
local live = { settled = false, pending_touches = {} }

--- A map started: forget the previous map's touches (fe.field calls this
--- from `field.start`).
function interaction.reset()
  live.settled = false
  live.pending_touches = {}
end

--- Register (or replace) a verb: something the player does to the entity
--- in front of them by pressing `spec.input` (default "confirm").
---   when(entity, player) -> boolean   whether it applies to an entity
---   run(entity, player)               runs in the interaction task
---   label                             string/i18n key or fn(entity), shown
---                                     when several verbs share an input
---   ask                               offer the choice even when this is
---                                     the only verb (to show its label)
---   range, facing                     reach (default settings ranges) and
---                                     whether the player must face it
---@param id string
---@param spec {input?: string, when?: fun(e: Entity, p: Entity): boolean, run: fun(e: Entity, p: Entity), label?: string|fun(e: Entity): string, ask?: boolean, range?: number, facing?: boolean}
function interaction.verb(id, spec)
  return interaction.register(field().verbs, id, spec)
end

--- Put a verb in `verbs` (fe.field's list) under `id`, replacing one with
--- that id; what `field.verb` does.
---@param verbs table[]
---@param id string
---@param spec table
---@return table spec
function interaction.register(verbs, id, spec)
  spec.id = id
  spec.input = spec.input or "confirm"
  for i, verb in ipairs(verbs) do
    if verb.id == id then
      verbs[i] = spec
      return spec
    end
  end
  verbs[#verbs + 1] = spec
  return spec
end

--- Remove a verb by id.
function interaction.remove_verb(id)
  local verbs = field().verbs
  for i, verb in ipairs(verbs) do
    if verb.id == id then
      table.remove(verbs, i)
      return
    end
  end
end

--- Register (or replace) a command: something the player does by pressing
--- `spec.input` while in control, with no target (a day/night toggle, a
--- skill like Octopath's path actions on the field). `run()` runs in a task
--- that holds a field lock; `when()` may refuse.
---@param id string
---@param spec {input: string, run: fun(), when?: fun(): boolean}
function interaction.command(id, spec)
  spec.id = id
  field().commands[id] = spec
  return spec
end

--- Talk: an entity's `data().interact` function, or its active "action"
--- event page (the entity turns to the player unless props.face_player is
--- false).
interaction.talk = {
  input = "confirm",
  when = function(entity)
    if entity:data().interact then return true end
    local page = event.page(entity)
    return page ~= nil and (page.trigger or "action") == "action"
  end,
  run = function(entity, player)
    local d = entity:data()
    if d.interact then
      d.interact(entity)
      return
    end
    if entity:props().face_player ~= false then entity:face(player) end
    event.execute(entity, event.page(entity), { player = player })
  end,
}

--- The verbs of `input` that apply to an entity (their `when`), in
--- registration order.
---@return table[]
function interaction.verbs_for(entity, input, player)
  local out = {}
  for _, verb in ipairs(field().verbs) do
    if verb.input == input and (not verb.when or verb.when(entity, player)) then out[#out + 1] = verb end
  end
  return out
end

--- The entity the player would act on with `input` (default "confirm"),
--- and the verbs that apply to it; nil when there is none. The nearest
--- entity in reach wins; reach is the verb's range (default
--- settings.interact_range, or counter_range across a "counter" cell) and,
--- unless the verb sets facing = false, the player must face it
--- (`settings.facing_dot`).
---@return Entity?, table[]?
function interaction.target(input)
  input = input or "confirm"
  local f = field()
  local player = f.player
  if not player or not player:valid() then return nil end
  local p, fc = player.position, player.facing
  local ahead = f.world:cell({ x = math.floor(p.x + fc.x), z = math.floor(p.z + fc.z), layer = player.layer })
  local default_range = f.settings.interact_range
  if ahead and tablex.contains(ahead.tags, "counter") then default_range = f.settings.counter_range end
  local best, best_distance, best_verbs
  for _, entity in ipairs(f.entities()) do
    if entity:valid() and entity ~= player and not event.running(entity) then
      local q = entity.position
      local distance = vec.flat_distance(p, q)
      if math.abs(q.y - p.y) < 1 and (not best or distance < best_distance) then
        local facing_dot = distance > 0.01 and ((q.x - p.x) * fc.x + (q.z - p.z) * fc.z) / distance or 1
        local reached = {}
        for _, verb in ipairs(f.verbs_for(entity, input, player)) do
          if distance <= (verb.range or default_range) and (verb.facing == false or facing_dot > f.settings.facing_dot) then
            reached[#reached + 1] = verb
          end
        end
        if #reached > 0 then best, best_distance, best_verbs = entity, distance, reached end
      end
    end
  end
  return best, best_verbs
end

--- Pick one of several verbs for an entity (default: a menu titled with
--- the entity's name). Returns the verb or nil when cancelled.
function interaction.choose_verb(entity, verbs)
  local ui = require("fe.ui")
  local entries = {}
  for i, verb in ipairs(verbs) do
    local label = verb.label
    if type(label) == "function" then label = label(entity) end
    entries[i] = { label = ui.t(label or verb.id), value = verb }
  end
  local _, chosen = ui.choose(entries, { title = entity:props().name, cancel = true, width = 140 })
  return chosen and chosen.value
end

--- Act on the target for `input`: runs the verb (asking with
--- `field.choose_verb` when several apply or the verb sets `ask`) in a task
--- of the map's scripts that holds a blocking event, so control returns
--- when it finishes. An entity running a script (other than a parallel
--- page) is not a target. Returns the task, or nil.
---@param input? string
---@return Task?
function interaction.interact(input)
  input = input or "confirm"
  local f = field()
  local target, verbs = f.target(input)
  if not target then return nil end
  f.interacted:emit(target, input)
  local player = f.player
  local job = f.map_scripts():spawn("interact:" .. target.id, function()
    task.bind(f.world)
    event.block()
    local verb = verbs[1]
    if #verbs > 1 or verb.ask then verb = f.choose_verb(target, verbs) end
    if verb then verb.run(target, player) end
  end)
  if job:running() then target:data().running = job end
  return job
end

--- The player touched another entity: doors, touch pages and encounter
--- symbols react (only while `field.can_trigger()`; otherwise the touch is
--- kept for `field.retouch`, unless the map just started).
function interaction.touch(other)
  local f = field()
  if not f.can_trigger() then
    if live.settled and not tablex.contains(live.pending_touches, other) then
      live.pending_touches[#live.pending_touches + 1] = other
    end
    return
  end
  local d = other:data()
  if d.on_touch then
    d.on_touch(other)
    return
  end
  local page = event.page(other)
  if page and page.trigger == "touch" then
    event.run(other, page, { player = f.player })
  end
  if other:has_tag("encounter_symbol") and f.can_trigger() then
    local troop = other:props().troop
    other:destroy()
    f.start_encounter(troop, nil)
  end
end

--- Fire the touches suppressed under a lock once triggers are allowed
--- again, for entities the player still stands on (`settings.touch_range`,
--- same layer).
function interaction.retouch()
  local f = field()
  if not f.can_trigger() then return end
  if not live.settled then
    live.settled = true
    return
  end
  if #live.pending_touches == 0 then return end
  local pending = live.pending_touches
  live.pending_touches = {}
  local player = f.player
  for _, other in ipairs(pending) do
    if not f.can_trigger() then return end
    if other:valid() and other.layer == player.layer
        and vec.flat_distance(other.position, player.position) <= f.settings.touch_range then
      f.touch(other)
    end
  end
end

--- Run the first command whose input was pressed (and whose `when`
--- allows it) in a task holding a field lock. Returns the task, or nil.
---@return Task?
function interaction.run_command()
  local f = field()
  local ids = {}
  for id in pairs(f.commands) do ids[#ids + 1] = id end
  table.sort(ids)
  for _, id in ipairs(ids) do
    local command = f.commands[id]
    if fe.input.pressed(command.input) and (not command.when or command.when()) then
      return task.named("command:" .. id, function()
        f.lock()
        command.run()
      end)
    end
  end
  return nil
end

--- Player input while `field.can_control()`: movement relative to the
--- camera, verbs (each registered input action) and the menu.
function interaction.control()
  local f = field()
  local player = f.player
  local mx, my = fe.input.axis("move_x"), fe.input.axis("move_y")
  local length = math.sqrt(mx * mx + my * my)
  if length > 1 then mx, my = mx / length, my / length end
  if length > 0.05 then
    local yaw = math.rad(f.rig:yaw())
    local c, s = math.cos(yaw), math.sin(yaw)
    local speed = fe.input.held("run") and f.settings.run_speed or f.settings.walk_speed
    player:drive({ x = (c * mx + s * my) * speed, z = (-s * mx + c * my) * speed })
  end
  local tried = {}
  for _, verb in ipairs(f.verbs) do
    if not tried[verb.input] then
      tried[verb.input] = true
      if fe.input.pressed(verb.input) and f.interact(verb.input) then return end
    end
  end
  if f.run_command() then return end
  if fe.input.pressed("menu") then f.menu_requested:emit() end
end

-- What this module installed on fe.field, so a reload replaces its own
-- functions and verbs and leaves a game's replacements alone.
local installed = {}

--- Give `field` the interaction functions it does not have yet (or had from
--- this module) and the built-in verbs.
---@param field table the fe.field module
function interaction.install(field)
  local functions = { verb = interaction.verb, remove_verb = interaction.remove_verb,
    command = interaction.command, verbs_for = interaction.verbs_for, target = interaction.target,
    choose_verb = interaction.choose_verb, interact = interaction.interact, touch = interaction.touch,
    retouch = interaction.retouch, run_command = interaction.run_command, control = interaction.control }
  for name, fn in pairs(functions) do
    if field[name] == nil or field[name] == installed[name] then
      field[name] = fn
      installed[name] = fn
    end
  end
  local talk = tablex.find(field.verbs, function(verb) return verb.id == "talk" end)
  if talk == nil or talk == installed.talk then
    installed.talk = interaction.register(field.verbs, "talk", interaction.talk)
  end
end

--- Install the fresh functions after a hot reload of this file.
function interaction.__reload(live_module)
  live_module.install(require("fe.field"))
end

return interaction
