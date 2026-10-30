--- Declarative schemas for data tables and authored objects.
---
--- Types: "string", "number", "integer", "boolean", "table", "any",
--- {list = T}, {map = T}, {enum = {...}}, {ref = "table_name"},
--- {record = {field = spec, ...}} or the name of another schema. Strings
--- that name files or things get a picker in the editor and an existence
--- check in fe.data.validate: {asset = "sprite"|"image"|"model"|"audio"|
--- "lut"|"shader"} (a game-relative path; audio also takes a data/audio.json
--- id), {module = "events"} (a dotted Lua module under that folder, "" for
--- any), {map_id = true} (a map folder id) and {point = "map"} (a point of
--- the map the sibling field names).
--- A field spec is a type or {type = T, optional = true, default = v,
--- min = n, max = n, doc = "..."}; table types may carry the field keys
--- directly ({enum = {...}, optional = true}). Lists must have no holes.
---
--- ```lua
--- schema.define("item", {
---   id = "string", name = "string",
---   price = {type = "integer", min = 0, default = 0},
---   kind = {enum = {"consumable", "equipment", "key"}},
--- })
--- local ok, errors = schema.validate("item", entry)
--- ```
--- `schema.export()` describes every schema, placement type and zone type
--- for the editor's form inspectors.
local schema = {}

local definitions = {}
local entity_types = {}
local zone_types = {}

local field_keys = { optional = true, default = true, min = true, max = true, doc = true }

local function field_spec(spec)
  if type(spec) ~= "table" then
    return { type = spec }
  end
  if spec.type ~= nil then
    return spec
  end
  local field, kind = {}, {}
  for key, value in pairs(spec) do
    if field_keys[key] then field[key] = value else kind[key] = value end
  end
  field.type = kind
  return field
end

--- Define (or replace) a named record schema.
---@param name string
---@param fields table<string, any>
---@param options? {doc?: string, open?: boolean} open records allow unknown fields
function schema.define(name, fields, options)
  definitions[name] = { fields = fields, doc = options and options.doc, open = options and options.open ~= false }
  return definitions[name]
end

--- Add fields to an existing schema (games extend the kits' schemas).
function schema.extend(name, fields)
  local definition = assert(definitions[name], "unknown schema '" .. name .. "'")
  for key, spec in pairs(fields) do
    definition.fields[key] = spec
  end
end

function schema.get(name)
  return definitions[name]
end

--- Describe the props of a placement type (map editor inspector).
---@param type_name string
---@param fields table<string, any>
---@param doc? string
function schema.entity(type_name, fields, doc)
  entity_types[type_name] = { fields = fields, doc = doc }
end

--- Describe the props of a zone type (map editor inspector).
---@param type_name string
---@param fields table<string, any>
---@param doc? string
function schema.zone(type_name, fields, doc)
  zone_types[type_name] = { fields = fields, doc = doc }
end

local check

local function check_record(fields, value, path, errors, refs, open)
  if type(value) ~= "table" then
    errors[#errors + 1] = path .. ": expected a table"
    return
  end
  for key, spec in pairs(fields) do
    local f = field_spec(spec)
    local child = value[key]
    if child == nil then
      if f.default ~= nil then
        value[key] = type(f.default) == "table" and require("fe.tablex").deep_copy(f.default) or f.default
      elseif not f.optional then
        errors[#errors + 1] = path .. "." .. key .. ": missing"
      end
    else
      check(f, child, path .. "." .. key, errors, refs)
    end
  end
  if open == false then
    for key in pairs(value) do
      if fields[key] == nil then
        errors[#errors + 1] = path .. "." .. tostring(key) .. ": unknown field"
      end
    end
  end
end

check = function(f, value, path, errors, refs)
  local t = f.type
  if type(t) == "string" then
    if t == "any" then
      return
    elseif t == "integer" then
      if math.type(value) ~= "integer" and not (type(value) == "number" and value == math.floor(value)) then
        errors[#errors + 1] = path .. ": expected an integer"
        return
      end
    elseif t == "string" or t == "number" or t == "boolean" or t == "table" then
      if type(value) ~= t then
        errors[#errors + 1] = path .. ": expected " .. t .. ", got " .. type(value)
        return
      end
    elseif definitions[t] then
      local d = definitions[t]
      check_record(d.fields, value, path, errors, refs, d.open)
      return
    else
      errors[#errors + 1] = path .. ": unknown type '" .. t .. "'"
      return
    end
    if type(value) == "number" then
      if f.min and value < f.min then errors[#errors + 1] = path .. ": below minimum " .. f.min end
      if f.max and value > f.max then errors[#errors + 1] = path .. ": above maximum " .. f.max end
    end
    return
  end
  if type(t) ~= "table" then
    errors[#errors + 1] = path .. ": invalid schema"
    return
  end
  if t.list then
    if type(value) ~= "table" then
      errors[#errors + 1] = path .. ": expected a list"
      return
    end
    local count = 0
    for key in pairs(value) do
      count = count + 1
      if math.type(key) ~= "integer" or key < 1 then
        errors[#errors + 1] = path .. ": expected a list, found key '" .. tostring(key) .. "'"
        return
      end
    end
    for i = 1, count do
      if value[i] == nil then
        errors[#errors + 1] = path .. "[" .. i .. "]: missing (lists cannot have holes)"
        return
      end
      check(field_spec(t.list), value[i], path .. "[" .. i .. "]", errors, refs)
    end
  elseif t.map then
    if type(value) ~= "table" then
      errors[#errors + 1] = path .. ": expected a table"
      return
    end
    for key, item in pairs(value) do
      check(field_spec(t.map), item, path .. "." .. tostring(key), errors, refs)
    end
  elseif t.enum then
    for _, option in ipairs(t.enum) do
      if option == value then return end
    end
    errors[#errors + 1] = path .. ": '" .. tostring(value) .. "' is not one of " .. table.concat(t.enum, ", ")
  elseif t.ref then
    if type(value) ~= "string" then
      errors[#errors + 1] = path .. ": expected an id of " .. t.ref
    elseif refs then
      refs[#refs + 1] = { table = t.ref, id = value, path = path }
    end
  elseif t.asset or t.module or t.map_id or t.point then
    if type(value) ~= "string" then
      errors[#errors + 1] = path .. ": expected a string (" ..
        (t.asset and (t.asset .. " path") or t.module and "module name" or t.map_id and "map id" or "point name") .. ")"
    elseif refs then
      refs[#refs + 1] = { asset = t.asset, module = t.module, map = t.map_id and true or nil,
        point = t.point, id = value, path = path }
    end
  elseif t.record then
    check_record(t.record, value, path, errors, refs, t.open)
  end
end

--- Validate a value; fills defaults in place. Returns ok, errors, refs
--- (references to other data tables, checked by fe.data).
---@return boolean, string[], table[]
function schema.validate(name_or_spec, value, path)
  local errors, refs = {}, {}
  local spec = type(name_or_spec) == "string" and { type = name_or_spec } or field_spec(name_or_spec)
  check(spec, value, path or (type(name_or_spec) == "string" and name_or_spec or "value"), errors, refs)
  return #errors == 0, errors, refs
end

local function describe(spec)
  local f = field_spec(spec)
  local t = f.type
  local out = { optional = f.optional or nil, default = f.default, min = f.min, max = f.max, doc = f.doc }
  if type(t) == "string" then
    out.type = t
  elseif t.list then
    out.type, out.of = "list", describe(t.list)
  elseif t.map_id then
    out.type = "map_id"
  elseif t.map then
    out.type, out.of = "map", describe(t.map)
  elseif t.enum then
    out.type, out.options = "enum", t.enum
  elseif t.ref then
    out.type, out.table = "ref", t.ref
  elseif t.asset then
    out.type, out.asset = "asset", t.asset
  elseif t.module then
    out.type, out.folder = "module", t.module
  elseif t.point then
    out.type, out.map_field = "point", t.point
  elseif t.record then
    out.type, out.fields = "record", {}
    for key, child in pairs(t.record) do out.fields[key] = describe(child) end
  end
  return out
end

local function describe_types(types)
  local out = {}
  for name, t in pairs(types) do
    local fields = {}
    for key, spec in pairs(t.fields) do fields[key] = describe(spec) end
    out[name] = { doc = t.doc, fields = fields }
  end
  return out
end

--- JSON-friendly description of every schema, entity type and zone type.
function schema.export()
  return { schemas = describe_types(definitions), entities = describe_types(entity_types),
    zones = describe_types(zone_types) }
end

function schema.entity_type(name)
  return entity_types[name]
end

function schema.zone_type(name)
  return zone_types[name]
end

return schema
