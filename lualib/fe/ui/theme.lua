--- The UI theme: window skin, colours, font and sounds used by fe.ui and
--- the default screens. Games change it with `theme.set{...}`.
---
--- Presets: `theme.use("high_contrast")` switches to plain black windows
--- with white borders and brighter text (fe.ui applies it for the
--- `high_contrast` setting); `theme.use("default")` restores the values
--- the theme had before the first switch, including the game's own
--- `theme.set` changes. Add presets to `theme.presets`.
local theme = {
  skin = "@fe/assets/ui/window.png",   -- false: plain windows (window_fill, window_border)
  skin_border = 4,
  cursor = "@fe/assets/ui/cursor.png",
  font = "default",
  text_size = 10,          -- the font's size; text scaling multiplies it
  padding = 6,
  row_height = 12,
  text = 0xe8ecf4ff,
  muted = 0x9aa6bdff,
  highlight = 0xffe08aff,
  disabled = 0x6d7488ff,
  selection = 0x5577aa66,
  shadow = 0x00000099,
  heading = 0xffd27aff,
  good = 0x8ff0a4ff,
  bad = 0xff8a7aff,
  window_fill = 0x101a34ee,
  window_border = 0x101a34ee,
  gauge_back = 0x0a1024ff,
  gauges = { hp = 0x6fd49fff, mp = 0x7fa6f6ff, sp = 0xf0c46cff, tp = 0xd99cf2ff, exp = 0xf2e36cff },
  sounds = { move = "cursor", confirm = "confirm", cancel = "cancel", buzzer = "buzzer" },
  open_time = 0.12,
  current = "default",
  presets = {
    high_contrast = {
      skin = false, window_fill = 0x000000ff, window_border = 0xffffffff, text = 0xffffffff, muted = 0xd8d8d8ff,
      highlight = 0xffff40ff, heading = 0xffff40ff, disabled = 0xa0a0a0ff, shadow = 0x000000ff,
      good = 0x60ff80ff, bad = 0xff6060ff, gauge_back = 0x000000ff,
    },
  },
}

--- Theme colours usable in markup as {color:name}.
theme.markup_colors = { "highlight", "muted", "good", "bad", "heading" }

--- Register the markup colours with fe.text (theme.set does this).
function theme.apply()
  for _, name in ipairs(theme.markup_colors) do
    fe.text.color(name, theme[name])
  end
end

--- Override theme values and re-register the markup colours.
function theme.set(values)
  for key, value in pairs(values) do
    theme[key] = value
  end
  theme.apply()
end

--- Switch to a preset by name ("default" restores the values replaced by
--- the first switch away from it).
---@param name string
function theme.use(name)
  if name == theme.current then return end
  local presets = theme.presets
  if theme.current == "default" then
    local saved = {}
    for preset, values in pairs(presets) do
      if preset ~= "default" then
        for key in pairs(values) do saved[key] = theme[key] end
      end
    end
    presets.default = saved
  end
  local values = presets[name]
  if not values then error("unknown theme preset '" .. tostring(name) .. "'", 2) end
  if theme.current ~= "default" and name ~= "default" then theme.set(presets.default) end
  theme.set(values)
  theme.current = name
end

-- The values a reload may adopt: every scalar field but `current`, and the
-- entries of these tables.
local adoptable_tables = { "gauges", "sounds" }

local function snapshot(source)
  local out = {}
  for key, value in pairs(source) do
    if type(value) ~= "function" and type(value) ~= "table" and key ~= "current" then out[key] = value end
  end
  for _, name in ipairs(adoptable_tables) do
    out[name] = {}
    for key, value in pairs(source[name]) do out[name][key] = value end
  end
  out.presets = {}
  for name, values in pairs(source.presets) do
    if name ~= "default" then
      out.presets[name] = {}
      for key, value in pairs(values) do out.presets[name][key] = value end
    end
  end
  return out
end

--- The file's values, for `__reload`.
theme.file_defaults = snapshot(theme)

--- Adopt edited colours, skin, font and sounds on hot reload: a value the
--- game changed at runtime (`theme.set`) is kept, every other one takes the
--- file's. While a preset such as high contrast is in use, the adopted
--- base values go into the saved "default" preset and the preset's own
--- edited values apply at once, so the override stays on top.
function theme.__reload(live, fresh)
  local baseline = live.file_defaults
  local fresh_values = snapshot(fresh)
  local overridden = live.current ~= "default" and live.presets.default or nil
  for key, value in pairs(fresh_values) do
    if key ~= "presets" and key ~= "gauges" and key ~= "sounds" then
      local target = overridden and overridden[key] ~= nil and overridden or live
      if target[key] == baseline[key] then target[key] = value end
    end
  end
  for _, name in ipairs(adoptable_tables) do
    for key, value in pairs(fresh_values[name]) do
      if live[name][key] == (baseline[name] or {})[key] then live[name][key] = value end
    end
  end
  for name, values in pairs(fresh_values.presets) do
    live.presets[name] = live.presets[name] or {}
    for key, value in pairs(values) do
      if live.presets[name][key] == ((baseline.presets or {})[name] or {})[key] then live.presets[name][key] = value end
    end
  end
  live.file_defaults = fresh_values
  if overridden then
    for key, value in pairs(live.presets[live.current]) do live[key] = value end
  end
  theme.apply()
end

theme.apply()

return theme
