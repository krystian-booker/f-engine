--- Player settings stored outside save slots (fe.save config file):
--- volumes, text speed, language, window mode and rebound controls.
local input = require("fe.input")
local signal = require("fe.signal")

local config = {
  values = {
    master = 1, music = 0.7, sfx = 0.8,
    text_speed = 45,        -- characters per second (0 = instant)
    battle_speed = 1,       -- multiplier games can apply to battle pacing
    battle_wait = true,     -- ATB "wait" mode
    language = "en",
    fullscreen = false,
    bindings = {},          -- action -> raw inputs (only changed actions)
  },
  changed = signal.new(),
}

--- Load the settings file and apply it (fe.game calls this at start).
function config.load()
  local saved = fe.save.read_config()
  for key, value in pairs(saved) do
    config.values[key] = value
  end
  config.apply()
end

--- Push settings to the engine.
function config.apply()
  local v = config.values
  fe.audio.bus_volume("master", v.master)
  fe.audio.bus_volume("music", v.music)
  fe.audio.bus_volume("sfx", v.sfx)
  fe.app.fullscreen(v.fullscreen)
  input.use_defaults(v.bindings)
  config.changed:emit()
end

function config.save()
  fe.save.write_config(config.values)
end

--- Change one setting, apply and persist it.
function config.set(key, value)
  config.values[key] = value
  config.apply()
  config.save()
end

function config.get(key)
  return config.values[key]
end

--- Rebind an action and remember the override.
function config.bind(action, inputs)
  config.values.bindings[action] = inputs
  fe.input.bind(action, inputs)
  config.save()
end

--- Restore default controls.
function config.reset_bindings()
  config.values.bindings = {}
  config.apply()
  config.save()
end

return config
