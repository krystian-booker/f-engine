--- Data tables loaded from JSON and validated against schemas.
---
--- ```lua
--- data.load("items", "data/items.json", "item")
--- local potion = data.get("items", "potion")
--- for _, item in ipairs(data.list("items")) do ... end
--- ```
--- Files hold either an array of records with `id` fields or an object
--- keyed by id. Edits to a loaded file are reloaded automatically (with
--- validation; a broken edit keeps the previous data).
local schema = require("fe.schema")
local signal = require("fe.signal")
local tablex = require("fe.tablex")

local data = {
  --- Emitted as (table_name) after a table loads or reloads.
  changed = signal.new(),
}

local tables = {}

-- Records from an array or an object keyed by id, checked against a
-- schema. Returns list, by_id, errors and refs.
local function check_records(name, raw, schema_name, label)
  local list, by_id = {}, {}
  local entries = raw
  if raw[1] == nil and next(raw) ~= nil then
    entries = {}
    local ids = {}
    for id in pairs(raw) do ids[#ids + 1] = id end
    table.sort(ids)
    for _, id in ipairs(ids) do
      local entry = raw[id]
      if type(entry) == "table" then entry.id = entry.id or id end
      entries[#entries + 1] = entry
    end
  end
  local errors, refs = {}, {}
  for index, entry in ipairs(entries) do
    if type(entry) ~= "table" or type(entry.id) ~= "string" or entry.id == "" then
      errors[#errors + 1] = label .. "[" .. index .. "]: every record needs a string id"
    elseif by_id[entry.id] then
      errors[#errors + 1] = label .. ": duplicate id '" .. entry.id .. "'"
    else
      if schema_name then
        local _, entry_errors, entry_refs = schema.validate(schema_name, entry, name .. "." .. entry.id)
        for _, e in ipairs(entry_errors) do errors[#errors + 1] = e end
        for _, r in ipairs(entry_refs) do refs[#refs + 1] = r end
      end
      by_id[entry.id] = entry
      list[#list + 1] = entry
    end
  end
  return list, by_id, errors, refs
end

local function build(name, file, schema_name)
  local list, by_id, errors, refs = check_records(name, fe.json.read(file), schema_name, file)
  if #errors > 0 then
    error(table.concat(errors, "\n"), 0)
  end
  return { list = list, by_id = by_id, file = file, schema = schema_name, refs = refs }
end

--- Load (or replace) a table. Raises with every validation error.
---@param name string
---@param file string game-relative JSON path
---@param schema_name? string
function data.load(name, file, schema_name)
  tables[name] = build(name, file, schema_name)
  data.changed:emit(name)
  return tables[name].by_id
end

function data.has_table(name)
  return tables[name] ~= nil
end

--- A record by id; raises if it does not exist.
function data.get(name, id)
  local t = tables[name]
  if not t then error("data table '" .. name .. "' is not loaded", 2) end
  local entry = t.by_id[id]
  if not entry then error("no '" .. tostring(id) .. "' in data table '" .. name .. "'", 2) end
  return entry
end

--- A record by id, or nil.
function data.find(name, id)
  local t = tables[name]
  return t and t.by_id[id]
end

--- Records in file order.
function data.list(name)
  local t = tables[name]
  if not t then error("data table '" .. name .. "' is not loaded", 2) end
  return t.list
end

--- Check records for a table without loading them (the editor's Data tab
--- does this before saving): ids, the table's schema (or `schema_name`)
--- and references to other loaded tables. The records are not changed.
---@param name string
---@param records table array or object keyed by id
---@param schema_name? string
---@return boolean, string[]
function data.validate(name, records, schema_name)
  local loaded = tables[name]
  schema_name = schema_name or (loaded and loaded.schema)
  local copy = tablex.deep_copy(records)
  local _, by_id, errors, refs = check_records(name, copy, schema_name, name)
  for _, ref in ipairs(refs) do
    local target = ref.table == name and { by_id = by_id } or tables[ref.table]
    if not target then
      errors[#errors + 1] = ref.path .. ": table '" .. ref.table .. "' is not loaded"
    elseif not target.by_id[ref.id] then
      errors[#errors + 1] = ref.path .. ": unknown " .. ref.table .. " '" .. ref.id .. "'"
    end
  end
  return #errors == 0, errors
end

--- Check every {ref = "table"} field across loaded tables.
---@return boolean, string[]
function data.check_references()
  local problems = {}
  for _, t in pairs(tables) do
    for _, ref in ipairs(t.refs) do
      local target = tables[ref.table]
      if not target then
        problems[#problems + 1] = ref.path .. ": table '" .. ref.table .. "' is not loaded"
      elseif not target.by_id[ref.id] then
        problems[#problems + 1] = ref.path .. ": unknown " .. ref.table .. " '" .. ref.id .. "'"
      end
    end
  end
  table.sort(problems)
  return #problems == 0, problems
end

--- Loaded tables: {name, file, schema} for tools such as the data editor.
function data.describe()
  local out = {}
  for name, t in pairs(tables) do
    out[#out + 1] = { name = name, file = t.file, schema = t.schema }
  end
  table.sort(out, function(a, b) return a.name < b.name end)
  return out
end

--- Reload any table that came from `file`; returns true if one did. The
--- table is rebuilt: look records up again (data.get) rather than keeping
--- references to old ones.
function data.reload(file)
  for name, t in pairs(tables) do
    if t.file == file then
      local ok, result = pcall(build, name, file, t.schema)
      if ok then
        tables[name] = result
        data.changed:emit(name)
        fe.log.info("Reloaded data table " .. name)
      else
        fe.log.error("Reload of " .. file .. " failed; keeping the previous data:\n" .. tostring(result))
      end
      return true
    end
  end
  return false
end

return data
