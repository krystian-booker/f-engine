--- The four heroes between battles: who they are, their HP and MP, Rowan's
--- corruption, and the items they carry. A ten-minute prologue has no
--- levels, equipment or shops, so this is all the state there is.
local party = { order = { "cael", "rowan", "lyra", "darius" }, state = {}, items = {} }

function party.reset()
  local heroes = fe.json.read("data/heroes.json")
  party.heroes = heroes
  party.state, party.items = {}, {}
  for _, id in ipairs(party.order) do
    party.state[id] = { hp = heroes[id].hp, mp = heroes[id].mp, corruption = heroes[id].corruption and 0 or nil }
  end
  for id, item in pairs(fe.json.read("data/items.json")) do party.items[id] = item.count end
end

function party.record(id)
  return party.heroes[id]
end

return party
