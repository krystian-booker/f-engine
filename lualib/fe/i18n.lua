--- Localization. String tables live in lang/<code>.json as flat objects or
--- nested objects addressed with dots ("menu.items"). `{name}` placeholders
--- are filled from an arguments table. Markup can use {t:key} once
--- `i18n.install()` has registered the text resolver.
local i18n = { language = nil, fallback = "en" }

local tables = {}

local function flatten(prefix, source, out)
  for key, value in pairs(source) do
    local full = prefix == "" and key or prefix .. "." .. key
    if type(value) == "table" then
      flatten(full, value, out)
    else
      out[full] = tostring(value)
    end
  end
  return out
end

local function load_table(code)
  if tables[code] == nil then
    local path = "lang/" .. code .. ".json"
    tables[code] = fe.fs.exists(path) and flatten("", fe.json.read(path), {}) or false
  end
  return tables[code] or nil
end

--- Switch language (loads lang/<code>.json).
function i18n.set_language(code)
  i18n.language = code
  load_table(code)
end

--- Translate a key; missing keys fall back to the fallback language, then
--- to the key itself so untranslated text is visible during development.
---@param key string
---@param args? table<string, any>
---@return string
function i18n.t(key, args)
  local current = i18n.language and load_table(i18n.language)
  local text = (current and current[key]) or (load_table(i18n.fallback) or {})[key] or key
  if args then
    text = text:gsub("{(%w+)}", function(name)
      local value = args[name]
      return value ~= nil and tostring(value) or "{" .. name .. "}"
    end)
  end
  return text
end

--- Forget cached tables (call after lang files change).
function i18n.reload()
  tables = {}
end

local resolvers = {}

--- Add a markup tag resolver: fn(tag) returns text or nil. All resolvers
--- share the single engine hook; {t:key} is built in.
function i18n.add_resolver(fn)
  resolvers[#resolvers + 1] = fn
end

--- Install the markup resolver chain (fe.game does this).
function i18n.install()
  fe.text.resolver(function(tag)
    local key = tag:match("^t:(.+)$")
    if key then return i18n.t(key) end
    for _, resolve in ipairs(resolvers) do
      local value = resolve(tag)
      if value ~= nil then return tostring(value) end
    end
    return nil
  end)
end

return i18n
