--- Player settings stored outside save slots (fe.save config file):
--- volumes (master, music, effects, voice), text speed, auto-advance,
--- language, accessibility (text scale, high contrast), display and
--- rebound controls.
---
--- `config.set` and `config.bind` apply immediately; `config.save()` writes
--- the file when something changed (the settings screen saves when it
--- closes, so sliders do not write on every step).
---
--- The language starts as the system's (`fe.app.locale()`) if the game
--- ships that language, else "en". Display settings (`fullscreen`, and
--- `display = {width, height, vsync, monitor}`) go to `fe.app.set_display`.
local input = require("fe.input")
local signal = require("fe.signal")

local config = {
  values = {
    master = 1, music = 0.7, sfx = 0.8, voice = 1,
    text_speed = 45,        -- characters per second (0 = instant)
    auto_advance = false,   -- dialogue lines advance by themselves
    text_scale = 1,         -- UI text size multiplier
    high_contrast = false,  -- plain high-contrast windows and text
    battle_speed = 1,       -- multiplier games can apply to battle pacing
    battle_wait = true,     -- ATB "wait" mode
    language = nil,         -- nil until chosen: the system language when available
    fullscreen = false,
    display = {},           -- {width?, height?, vsync?, monitor?} for fe.app.set_display
    bindings = {},          -- action -> raw inputs (only changed actions)
  },
  changed = signal.new(),
  dirty = false,
}

--- Load the settings file, define the default controls and apply both
--- (fe.starter calls this at start). `defaults` fills values the player has
--- not saved (fe.starter passes its `language` option). Saved bindings for
--- actions that do not exist yet stay pending for `config.apply_bindings`.
---@param defaults? table
function config.load(defaults)
  local saved = fe.save.read_config()
  for key, value in pairs(defaults or {}) do
    if saved[key] == nil then config.values[key] = value end
  end
  for key, value in pairs(saved) do
    config.values[key] = value
  end
  if not config.values.language then config.values.language = config.system_language() end
  input.use_defaults()
  config.apply_bindings(false)
  config.apply()
end

--- Languages the game ships: "en" plus every lang/<code>.json, sorted.
---@return string[]
function config.languages()
  local out, seen = { "en" }, { en = true }
  if fe.fs.exists("lang") then
    for _, entry in ipairs(fe.fs.list("lang")) do
      local code = entry:match("^(.+)%.json$")
      if code and not seen[code] then
        seen[code] = true
        out[#out + 1] = code
      end
    end
  end
  table.sort(out)
  return out
end

--- The system language if the game ships it ("pt-BR" matches lang/pt-BR or
--- lang/pt), else "en".
---@return string
function config.system_language()
  local locale = fe.app.locale()
  if locale == "" then return "en" end
  local available = {}
  for _, code in ipairs(config.languages()) do available[code] = true end
  if available[locale] then return locale end
  local base = locale:match("^(%a+)")
  return base and available[base] and base or "en"
end

local function apply_display(v)
  local display = { fullscreen = v.fullscreen }
  for key, value in pairs(v.display or {}) do display[key] = value end
  fe.app.set_display(display)
end

--- Push audio, language and display settings to the engine.
function config.apply()
  local v = config.values
  fe.audio.set_bus_volume("master", v.master)
  fe.audio.set_bus_volume("music", v.music)
  fe.audio.set_bus_volume("sfx", v.sfx)
  fe.audio.set_bus_volume("voice", v.voice)
  apply_display(v)
  local i18n = require("fe.i18n")
  if v.language and i18n.language ~= v.language then i18n.set_language(v.language) end
  config.changed:emit()
end

--- Apply saved control overrides. With `report`, overrides that still
--- cannot be applied (renamed or removed actions) are logged as warnings;
--- they are kept in the file either way.
---@param report boolean
function config.apply_bindings(report)
  local failed = input.apply_bindings(config.values.bindings)
  if report then
    for _, f in ipairs(failed) do
      fe.log.warn("Ignoring the saved binding for '" .. f.action .. "': " .. f.error)
    end
  end
end

--- Write the settings file if anything changed since the last save.
function config.save()
  if not config.dirty then return end
  fe.save.write_config(config.values)
  config.dirty = false
end

--- Change one setting and apply it (call `config.save()` to persist).
function config.set(key, value)
  config.values[key] = value
  config.dirty = true
  config.apply()
end

function config.get(key)
  return config.values[key]
end

--- Rebind an action and remember the override.
function config.bind(action, inputs)
  fe.input.bind(action, inputs)
  config.values.bindings[action] = inputs
  config.dirty = true
  input.sync_axes()
end

--- Restore the default controls.
function config.reset_bindings()
  config.values.bindings = {}
  config.dirty = true
  input.use_defaults()
end

return config
