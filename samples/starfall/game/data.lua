--- The game's data tables: every data/<name>.json listed here is an array of
--- records with a string `id`, loaded once and looked up by id. The editor's
--- Data tab edits the same files (see game/editor.lua for their forms).
local data = { tables = {}, names = { "heroes", "echoes", "skills", "enemies", "troops", "encounters", "items",
  "shops", "statuses" } }

function data.load(name)
  local list = fe.json.read("data/" .. name .. ".json")
  local by_id = {}
  for index, record in ipairs(list) do
    if type(record.id) ~= "string" then
      error("data/" .. name .. ".json: record " .. index .. " has no string id")
    end
    if by_id[record.id] then error("data/" .. name .. ".json: duplicate id '" .. record.id .. "'") end
    by_id[record.id] = record
  end
  data.tables[name] = { list = list, by_id = by_id }
  return data.tables[name]
end

function data.reload()
  for _, name in ipairs(data.names) do data.load(name) end
end

local function table_of(name)
  return data.tables[name] or data.load(name)
end

--- The record, or an error naming the table and id.
function data.get(name, id)
  local record = table_of(name).by_id[id]
  if not record then error("no '" .. tostring(id) .. "' in data/" .. name .. ".json", 2) end
  return record
end

function data.find(name, id)
  return table_of(name).by_id[id]
end

function data.list(name)
  return table_of(name).list
end

--- Every reference between tables must resolve (`fengine_headless --check`).
function data.validate()
  data.reload()
  local problems = {}
  local function need(name, id, where)
    if id ~= nil and not data.find(name, id) then
      problems[#problems + 1] = where .. " names the unknown " .. name .. " id '" .. tostring(id) .. "'"
    end
  end
  for _, hero in ipairs(data.list("heroes")) do
    need("skills", hero.attack, "hero " .. hero.id)
    need("skills", hero.surge, "hero " .. hero.id)
    need("echoes", hero.echo, "hero " .. hero.id)
    for _, skill in ipairs(hero.skills or {}) do need("skills", skill.id or skill, "hero " .. hero.id) end
  end
  for _, echo in ipairs(data.list("echoes")) do
    need("skills", echo.unison, "echo " .. echo.id)
    for _, entry in ipairs(echo.skills or {}) do need("skills", entry.id, "echo " .. echo.id) end
  end
  for _, enemy in ipairs(data.list("enemies")) do
    for _, entry in ipairs(enemy.skills or {}) do need("skills", entry.id, "enemy " .. enemy.id) end
  end
  for _, troop in ipairs(data.list("troops")) do
    for _, id in ipairs(troop.enemies) do need("enemies", id, "troop " .. troop.id) end
  end
  for _, zone in ipairs(data.list("encounters")) do
    for _, entry in ipairs(zone.troops) do need("troops", entry.troop, "encounter zone " .. zone.id) end
  end
  for _, shop in ipairs(data.list("shops")) do
    for _, id in ipairs(shop.items) do need("items", id, "shop " .. shop.id) end
  end
  for _, skill in ipairs(data.list("skills")) do
    if skill.status then need("statuses", skill.status.id, "skill " .. skill.id) end
    if skill.summon then need("enemies", skill.summon, "skill " .. skill.id) end
  end
  if #problems > 0 then error(table.concat(problems, "\n")) end
end

return data
