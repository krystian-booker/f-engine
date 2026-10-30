--- What happens around the player on the field, each piece a function a
--- game replaces on fe.field: this tick's world events (touches, zones and
--- footsteps: `field.handle_events`), NPC routines (`field.update_routines`
--- with the built-in `wander` and `patrol`), encounter symbols and random
--- encounters (`field.update_encounters`, `field.start_encounter`,
--- `field.on_encounter`), the camera rig (`field.make_rig`), pickups
--- (`field.give`, `field.announce`, the default `treasure` listener) and
--- save points (`field.on_save_point`). fe.field installs this module
--- (`hooks.install(field)`); replace any function on fe.field, or this
--- whole module with your own `fe/field/hooks.lua`.
---
--- Random encounters (fe.encounter, loaded on first use) are on while
--- `settings.encounters` is true; movement is measured between field
--- updates, so it does not depend on when the engine moves entities.
---
--- Pickups: `field.give` announces treasure with `field.announce`, a toast
--- (`ui.toast`) unless `settings.pickup_toasts` is false, then a message.
local camera = require("fe.camera")
local dialogue = require("fe.dialogue")
local event = require("fe.event")
local i18n = require("fe.i18n")
local sound = require("fe.sound")
local task = require("fe.task")
local vec = require("fe.vec")

local hooks = {}

-- fe.field requires this module while it loads, so it is fetched when needed.
local function field()
  return require("fe.field")
end

i18n.defaults({ fe = { field = {
  found_item = "Found {item}.", found_items = "Found {item} x{count}.", found_money = "Found {money} money.",
} } })

-- Kept across hot reload: the player's position at the previous update
-- (random encounters measure the distance walked from it).
local live = { last_position = nil }

--- A map started: the distance walked starts over (fe.field calls this
--- from `field.start`).
function hooks.reset()
  live.last_position = nil
end

-------------------------------------------------------------------------------
-- Routines

--- Built-in NPC routines, installed under their names (`field.routine`).
hooks.routines = {}

--- Wander: every 1.5 to 4 seconds, walk to a free spot within two cells of
--- home.
function hooks.routines.wander(entity, d, dt, rng)
  if entity:moving() then return end
  d.wait = (d.wait or rng:range(1, 3)) - dt
  if d.wait <= 0 then
    d.wait = rng:range(1.5, 4)
    local target = { x = d.home.x + rng:range(-2, 2), z = d.home.z + rng:range(-2, 2) }
    if entity:can_occupy(target) then entity:move_to(target) end
  end
end

--- Patrol: walk `data().route`'s points in turn, around and around.
function hooks.routines.patrol(entity, d)
  if not d.route or entity:moving() then return end
  d.leg = (d.leg or 0) % #d.route + 1
  local point = d.route[d.leg]
  entity:move_to({ x = point.x or point[1], z = point.z or point[2] })
end

--- Run `field.routines` for idle entities (no script, interaction or
--- parallel page running).
function hooks.update_routines(dt)
  local f = field()
  local rng = f.random or fe.random.new()
  f.random = rng
  for _, entity in ipairs(f.entities()) do
    if entity:valid() then
      local d = entity:data()
      local routine = d.routine and f.routines[d.routine]
      if routine and not event.running(entity) and not (d.parallel and d.parallel:running()) then
        routine(entity, d, dt, rng)
      end
    end
  end
end

-------------------------------------------------------------------------------
-- World events, encounters and the camera

--- Handle this tick's world events: contacts (`field.touch`), zones (their
--- `field.zone` handlers) and footsteps (`settings.footstep_sound`).
function hooks.handle_events(world)
  local f = field()
  local player = f.player
  for _, e in ipairs(world:events()) do
    if e.type == "contact_begin" and (e.a == player or e.b == player) then
      f.touch(e.a == player and e.b or e.a)
    elseif (e.type == "zone_enter" or e.type == "zone_exit") and e.a == player then
      local zone = f.map_zone(e.name)
      local handler = zone and f.zone_types[zone.type]
      if handler then handler(zone, e.type == "zone_enter", player) end
    elseif e.type == "anim_event" and e.name == "footstep" and e.a == player then
      sound.play(f.settings.footstep_sound, { volume = f.settings.footstep_volume })
    end
  end
end

--- Start a battle from the field in its own task, with control locked
--- until it ends.
---@return Task
function hooks.start_encounter(troop, entry)
  local f = field()
  return task.detached("encounter", function()
    f.lock()
    f.on_encounter(troop, entry)
    if f.settings.encounters then require("fe.encounter").reset() end
  end)
end

--- Encounter symbols and random encounters. Distance walked is measured
--- from the player's position at the previous field update.
function hooks.update_encounters(world, player)
  local f = field()
  if not f.settings.encounters then return end
  local encounter = require("fe.encounter")
  encounter.update_symbols(world, player)
  local p = player.position
  local previous = live.last_position
  live.last_position = { x = p.x, z = p.z, layer = player.layer }
  if not previous or previous.layer ~= player.layer or not f.can_trigger() then return end
  local moved = vec.flat_distance_in(world, previous, p)
  if moved > 0 then
    local troop, entry = encounter.step(world, p, moved, player.layer)
    if troop then f.start_encounter(troop, entry) end
  end
end

--- A random encounter fired; default starts a side-view battle.
function hooks.on_encounter(troop, entry)
  local battle = require("fe.battle")
  battle.encounter(troop, { stage = entry and entry.stage or field().world:props().battle_stage })
end

--- The camera rig for a map (`field.rig`, updated after the world moved):
--- a rig following the player. Replace it for another camera.
---@param world World
---@param player Entity
---@return table
function hooks.make_rig(world, player)
  return camera.rig(world, { follow = player })
end

-------------------------------------------------------------------------------
-- Pickups and save points

--- Give treasure: emits `field.treasure`; the default listener announces
--- what was found (the RPG kit adds the items to the inventory).
function hooks.give(item, count, money)
  field().treasure:emit(item, count or 1, money)
end

--- Tell the player about a pickup: a toast (`ui.toast`, with `options.icon`)
--- when `settings.pickup_toasts` is on, else a message (waits).
---@param text string markup
---@param options? {icon?: string, sound?: string}
function hooks.announce(text, options)
  if field().settings.pickup_toasts then
    require("fe.ui").toast(text, options)
  else
    dialogue.say(nil, text)
  end
end

--- The default `treasure` listener (lowest priority): announce the item and
--- the money.
function hooks.on_treasure(item, count, money)
  local f = field()
  if item then
    local key = count > 1 and "fe.field.found_items" or "fe.field.found_item"
    f.announce(i18n.t(key, { item = item, count = count }))
  end
  if money then f.announce(i18n.t("fe.field.found_money", { money = money })) end
  return true
end

--- A save point was used; default opens the save screen.
function hooks.on_save_point()
  require("fe.ui.screens.save").open("save")
end

-- What this module installed on fe.field, so a reload replaces its own
-- functions and routines and leaves a game's replacements alone.
local installed = {}

--- Give `field` the hook functions it does not have yet (or had from this
--- module), the built-in routines and the treasure listener.
---@param field table the fe.field module
function hooks.install(field)
  local functions = { update_routines = hooks.update_routines, handle_events = hooks.handle_events,
    start_encounter = hooks.start_encounter, update_encounters = hooks.update_encounters,
    on_encounter = hooks.on_encounter, make_rig = hooks.make_rig, give = hooks.give, announce = hooks.announce,
    on_save_point = hooks.on_save_point }
  for name, fn in pairs(functions) do
    if field[name] == nil or field[name] == installed[name] then
      field[name] = fn
      installed[name] = fn
    end
  end
  for name, fn in pairs(hooks.routines) do
    if field.routines[name] == nil or field.routines[name] == installed[name] then
      field.routine(name, fn)
      installed[name] = fn
    end
  end
  field.treasure:connect(function(...) return hooks.on_treasure(...) end, -1000, "fe.field.treasure")
end

--- Install the fresh functions after a hot reload of this file.
function hooks.__reload(live_module)
  live_module.install(require("fe.field"))
end

return hooks
