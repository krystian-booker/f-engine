--- The party: everyone recruited (the roster) and the active members.
local Actor = require("fe.rpg.actor")
local signal = require("fe.signal")
local state = require("fe.state")
local tablex = require("fe.tablex")

local party = {
  settings = { max_active = 4 },
  changed = signal.new(),
}

local record = state.section("party", function() return { roster = {}, active = {} } end)
local cache = {}

state.reloaded:connect(function() cache = {} end)

--- The Actor object for an id.
function party.actor(id)
  local actor = cache[id]
  if not actor then
    actor = Actor(id)
    cache[id] = actor
  end
  return actor
end

--- Recruit an actor; they become active when there is room.
function party.join(id, options)
  options = options or {}
  local actor = party.actor(id)
  if not tablex.contains(record.roster, id) then
    record.roster[#record.roster + 1] = id
  end
  if options.active ~= false and #record.active < party.settings.max_active and not tablex.contains(record.active, id) then
    record.active[#record.active + 1] = id
  end
  party.changed:emit()
  return actor
end

function party.leave(id)
  tablex.remove_value(record.roster, id)
  tablex.remove_value(record.active, id)
  party.changed:emit()
end

--- Active members in formation order.
function party.members()
  return tablex.map(record.active, party.actor)
end

--- Everyone recruited.
function party.roster()
  return tablex.map(record.roster, party.actor)
end

--- Recruited but not active.
function party.reserve()
  local out = {}
  for _, id in ipairs(record.roster) do
    if not tablex.contains(record.active, id) then out[#out + 1] = party.actor(id) end
  end
  return out
end

function party.is_active(id)
  return tablex.contains(record.active, id)
end

--- Replace the active line-up.
function party.set_active(ids)
  assert(#ids >= 1 and #ids <= party.settings.max_active, "active party size out of range")
  for _, id in ipairs(ids) do
    assert(tablex.contains(record.roster, id), "'" .. id .. "' has not joined")
  end
  record.active = tablex.copy(ids)
  party.changed:emit()
end

--- Swap two formation positions.
function party.swap(a, b)
  record.active[a], record.active[b] = record.active[b], record.active[a]
  party.changed:emit()
end

function party.leader()
  return record.active[1] and party.actor(record.active[1])
end

function party.alive()
  return tablex.filter(party.members(), function(a) return a:alive() end)
end

function party.defeated()
  return #party.alive() == 0
end

function party.recover()
  for _, actor in ipairs(party.roster()) do actor:recover() end
end

--- How experience is shared: active members get `amount`; reserve members
--- get `reserve_rate` of it (0.5 by default, 1 for "everyone levels").
party.settings.reserve_rate = 0.5
party.settings.exp_for_knocked_out = false

--- Award experience; returns {actor = Actor, gains = {...}}[].
function party.gain_exp(amount)
  local results = {}
  for _, actor in ipairs(party.members()) do
    if actor:alive() or party.settings.exp_for_knocked_out then
      results[#results + 1] = { actor = actor, gains = actor:gain_exp(amount) }
    end
  end
  if party.settings.reserve_rate > 0 then
    for _, actor in ipairs(party.reserve()) do
      results[#results + 1] = { actor = actor, gains = actor:gain_exp(math.floor(amount * party.settings.reserve_rate)), reserve = true }
    end
  end
  return results
end

--- New game: forget everyone.
function party.reset()
  for _, id in ipairs(record.roster) do Actor.reset(id) end
  record.roster, record.active = {}, {}
  cache = {}
end

return party
