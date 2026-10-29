--- Localization. String tables live in lang/<code>.json as flat objects or
--- nested objects addressed with dots ("menu.items"). `{name}` placeholders
--- (letters, digits and underscores) are filled from an arguments table.
--- Markup can use {t:key} once `i18n.install()` has registered the text
--- resolver (fe.starter does this).
---
--- Lookup order: the current language, the fallback language, then default
--- strings registered with `i18n.defaults` (the kits register their English
--- UI text under "fe.*" keys), then the key itself. A game translates or
--- rewords kit text by adding those keys to its lang files.
local i18n = { language = nil, fallback = "en" }

local tables = {}
local defaults = {}

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

--- Register default strings ({key = text}, nested tables allowed).
function i18n.defaults(strings)
  flatten("", strings, defaults)
end

--- Translate a key; missing keys fall back as described above, so
--- untranslated text is visible during development.
---@param key string
---@param args? table<string, any>
---@return string
function i18n.t(key, args)
  local current = i18n.language and load_table(i18n.language)
  local text = (current and current[key]) or (load_table(i18n.fallback) or {})[key] or defaults[key] or key
  if args then
    text = text:gsub("{([%w_]+)}", function(name)
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
--- share the single engine hook; {t:key} is built in. With a `key`, a
--- resolver added earlier with the same key is replaced.
---@param fn fun(tag: string): any
---@param key? string
function i18n.add_resolver(fn, key)
  if key ~= nil then
    for i, entry in ipairs(resolvers) do
      if entry.key == key then
        resolvers[i] = { fn = fn, key = key }
        return
      end
    end
  end
  resolvers[#resolvers + 1] = { fn = fn, key = key }
end

--- Install the markup resolver chain (fe.starter does this).
function i18n.install()
  fe.text.resolver(function(tag)
    local key = tag:match("^t:(.+)$")
    if key then return i18n.t(key) end
    for _, resolver in ipairs(resolvers) do
      local value = resolver.fn(tag)
      if value ~= nil then return tostring(value) end
    end
    return nil
  end)
end

return i18n
