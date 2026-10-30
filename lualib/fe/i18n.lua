--- Localization. String tables live in lang/<code>.json as flat objects or
--- nested objects addressed with dots ("menu.items"). `{name}` placeholders
--- (letters, digits and underscores) are filled from an arguments table.
--- Markup can use {t:key} once `i18n.install()` has registered the text
--- resolver (fe.starter does this).
---
--- Plural and select forms: `{count|one:item|other:items}` picks a form by
--- the argument. For a number the form is `=n` (an exact value, `=0`),
--- else the language's plural category from `i18n.plural_rules` ("one" or
--- "other" for English and the default; "one" covers 0 and 1 in French),
--- else `other`; for any other value (a string such as a gender) the form
--- with that name, else `other`. Inside a form `#` stands for the value:
--- `"{count|one:# item|other:# items}"`.
---
--- Lookup order: the current language, the fallback language, then default
--- strings registered with `i18n.defaults` (the kits register their English
--- UI text under "fe.*" keys), then the key itself. A game translates or
--- rewords kit text by adding those keys to its lang files.
local i18n = {
  language = nil,
  fallback = "en",
  --- Plural category of a number per language: fn(n) -> "zero" | "one" |
  --- "two" | "few" | "many" | "other". Languages not listed use `default`.
  plural_rules = {
    default = function(n) return math.abs(n) == 1 and "one" or "other" end,
    fr = function(n) return math.abs(n) < 2 and "one" or "other" end,
    pt = function(n) return math.abs(n) < 2 and "one" or "other" end,
    ja = function() return "other" end,
    zh = function() return "other" end,
    ko = function() return "other" end,
  },
}

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

--- The plural category of `n` in the current language (see `plural_rules`).
---@param n number
---@return string
function i18n.plural(n)
  local rules = i18n.plural_rules
  local code = i18n.language or i18n.fallback
  local rule = rules[code] or rules[code:match("^(%a+)") or code] or rules.default
  return rule(n)
end

-- The form of `{name|form:text|...}` for a value (see the header).
local function select_form(value, forms)
  local options = {}
  for form in forms:gmatch("[^|]+") do
    local name, text = form:match("^%s*([^:]+):(.*)$")
    if name then options[name] = text end
  end
  local chosen
  if type(value) == "number" then
    chosen = options["=" .. tostring(value)] or options[i18n.plural(value)]
  elseif value ~= nil then
    chosen = options[tostring(value)]
  end
  chosen = chosen or options.other
  if not chosen then return nil end
  return (chosen:gsub("#", value ~= nil and tostring(value) or "#"))
end

--- Translate a key; missing keys fall back as described above, so
--- untranslated text is visible during development. `args` fill `{name}`
--- placeholders and `{name|form:text|...}` plural and select forms.
---@param key string
---@param args? table<string, any>
---@return string
function i18n.t(key, args)
  local current = i18n.language and load_table(i18n.language)
  local text = (current and current[key]) or (load_table(i18n.fallback) or {})[key] or defaults[key] or key
  if args then
    text = text:gsub("{([%w_]+)|([^{}]*)}", function(name, forms)
      return select_form(args[name], forms) or ("{" .. name .. "}")
    end)
    text = text:gsub("{([%w_]+)}", function(name)
      local value = args[name]
      return value ~= nil and tostring(value) or "{" .. name .. "}"
    end)
  end
  return text
end

--- Format a number with the current language's decimal and digit-group
--- separators (`fe.app.format_number`): `i18n.number(1234.5)` is "1,234.50"
--- in English and "1.234,50" in German. `options.decimals` fixes the
--- fraction digits (default 0 for whole numbers, else 2); `options.locale`
--- overrides the language (default the current language, else the system's).
---@param n number
---@param options? {decimals?: integer, locale?: string}
---@return string
function i18n.number(n, options)
  local locale = options and options.locale or i18n.language
  if locale == nil then
    return fe.app.format_number(n, options)
  end
  return fe.app.format_number(n, { decimals = options and options.decimals, locale = locale })
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
