--- Everything a save file holds: the party, what it carries and knows, flags
--- and where it stands. One plain table (`state.data`), written as it is by
--- fe.save.
local data = require("game.data")

local state = { data = nil, version = 1 }

local function hero_record(id, level)
  local hero = data.get("heroes", id)
  return { level = level or 1, exp = 0, hp = 0, mp = 0, echo = hero.echo, surge = 0 }
end

--- A fresh game: the starting party, purse and items.
function state.new_game()
  state.data = {
    version = state.version,
    party = { "aster", "mira", "bram", "sel" },
    heroes = {},
    echoes = {},          -- owned echoes: id -> { points = n }
    items = { potion = 3, ether = 1, tonic = 1 },
    gold = 120,
    flags = {},
    bestiary = {},        -- enemy id -> { element = "weak" | "resist" | "null" | "absorb" | "normal" }
    opened = {},          -- chest ids already opened: "<map>/<id>"
    vehicles = {},        -- kind -> { x, z } on the overworld, once owned
    riding = nil,         -- vehicle kind the party is on
    map = "town", point = "entry", position = nil,
    danger = 0, steps = 0, time = 0, battles = 0,
  }
  for _, id in ipairs(state.data.party) do
    state.data.heroes[id] = hero_record(id)
    state.grant_echo(data.get("heroes", id).echo)
    local stats = state.stats(id)
    state.data.heroes[id].hp, state.data.heroes[id].mp = stats.hp, stats.mp
  end
  return state.data
end

function state.flag(name)
  return state.data.flags[name] == true
end

function state.set_flag(name, value)
  state.data.flags[name] = value ~= false or nil
end

function state.grant_echo(id)
  if id and not state.data.echoes[id] then
    data.get("echoes", id)
    state.data.echoes[id] = { points = 0 }
    return true
  end
  return false
end

--- Mastery rank of an owned echo: 1, then one more per `rank_points`.
function state.echo_rank(id)
  local owned = state.data.echoes[id]
  if not owned then return 0 end
  local echo = data.get("echoes", id)
  return math.min(echo.max_rank or 3, 1 + owned.points // (echo.rank_points or 40))
end

--- A hero's stats at their level, with their echo's bonus.
function state.stats(id)
  local hero = data.get("heroes", id)
  local record = state.data.heroes[id]
  local stats = {}
  for name, base in pairs(hero.stats) do
    stats[name] = math.floor(base + (hero.growth[name] or 0) * (record.level - 1))
  end
  local echo = record.echo and data.find("echoes", record.echo)
  if echo and echo.bonus then
    for name, amount in pairs(echo.bonus) do stats[name] = (stats[name] or 0) + amount end
  end
  return stats
end

function state.exp_to_next(level)
  return 24 * level + 6 * level * level
end

--- Give a hero experience; returns the levels gained.
function state.gain_exp(id, amount)
  local record = state.data.heroes[id]
  record.exp = record.exp + amount
  local gained = 0
  while record.exp >= state.exp_to_next(record.level) and record.level < 50 do
    record.exp = record.exp - state.exp_to_next(record.level)
    record.level = record.level + 1
    gained = gained + 1
  end
  if gained > 0 then
    local stats = state.stats(id)
    record.hp, record.mp = stats.hp, stats.mp
  end
  return gained
end

function state.item_count(id)
  return state.data.items[id] or 0
end

function state.add_item(id, count)
  data.get("items", id)
  local total = state.item_count(id) + (count or 1)
  state.data.items[id] = total > 0 and total or nil
end

function state.heal_party()
  for _, id in ipairs(state.data.party) do
    local stats = state.stats(id)
    state.data.heroes[id].hp, state.data.heroes[id].mp = stats.hp, stats.mp
  end
end

--- A line for the save screen.
function state.summary()
  local lead = state.data.party[1]
  return { level = state.data.heroes[lead].level, map = state.data.map, time = math.floor(state.data.time),
    gold = state.data.gold, saved = fe.time.wall() }
end

function state.save(slot)
  fe.save.write(slot, state.data, state.summary())
end

--- Load a slot; false when it is empty or from a version this game cannot read.
function state.load(slot)
  local loaded = fe.save.read(slot)
  if not loaded or loaded.version ~= state.version then return false end
  state.data = loaded
  return true
end

return state
