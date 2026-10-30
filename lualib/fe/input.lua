--- Default action maps and button prompts. The engine has no built-in
--- bindings; call `input.use_defaults()` (fe.config does when it loads) or
--- define your own with fe.input.define. Actions used by the library kits:
---   up, down, left, right   menu navigation and field movement
---   confirm, cancel, menu   menus, dialogue, interaction
---   page_left, page_right   switch characters/tabs, battle modifiers
---   run, camera_left, camera_right, camera_reset, zoom_in, zoom_out, info
---   skip                    hold to fast-forward dialogue and cutscenes
--- Axes: move_x, move_y (field movement) are rebuilt from the digital
--- bindings of left/right and up/down by `input.sync_axes()`, so rebinding
--- those actions also rebinds movement; camera_x.
local i18n = require("fe.i18n")

local input = {}

i18n.defaults({ fe = { button = {
  a = "A", b = "B", x = "X", y = "Y", leftshoulder = "LB", rightshoulder = "RB", lefttrigger = "LT",
  righttrigger = "RT", start = "Start", back = "Back", dpup = "Up", dpdown = "Down", dpleft = "Left",
  dpright = "Right", rightstick = "R3", leftstick = "L3",
} } })

input.defaults = {
  actions = {
    up = { "key:Up", "key:W", "pad:dpup", "pad:lefty-" },
    down = { "key:Down", "key:S", "pad:dpdown", "pad:lefty+" },
    left = { "key:Left", "key:A", "pad:dpleft", "pad:leftx-" },
    right = { "key:Right", "key:D", "pad:dpright", "pad:leftx+" },
    confirm = { "key:Return", "key:Z", "key:Space", "pad:a", "mouse:left" },
    cancel = { "key:Escape", "key:X", "key:Backspace", "pad:b", "mouse:right" },
    menu = { "key:Tab", "key:C", "pad:y" },
    info = { "key:I", "key:V", "pad:x" },
    page_left = { "key:Q", "key:PageUp", "pad:leftshoulder" },
    page_right = { "key:E", "key:PageDown", "pad:rightshoulder" },
    run = { "key:Left Shift", "key:Right Shift", "pad:b" },
    camera_left = { "key:Q", "pad:lefttrigger" },
    camera_right = { "key:E", "pad:righttrigger" },
    camera_reset = { "key:R", "pad:rightstick" },
    zoom_in = { "key:=", "pad:rightshoulder" },
    zoom_out = { "key:-", "pad:leftshoulder" },
    skip = { "key:Left Ctrl", "key:Right Ctrl", "pad:back" },
  },
  --- Movement axes: digital inputs come from these actions; analog sticks
  --- are listed here.
  movement = {
    move_x = { negative = "left", positive = "right", analog = { "pad:leftx" } },
    move_y = { negative = "up", positive = "down", analog = { "pad:lefty" } },
  },
  axes = {
    camera_x = { negative = {}, positive = {}, analog = { "pad:rightx" } },
  },
}

--- The file's defaults, for `__reload`.
input.file_defaults = require("fe.tablex").deep_copy(input.defaults)

--- Adopt edited defaults on hot reload (actions, movement and axes the
--- game changed at runtime are kept), define them again and re-apply the
--- player's saved rebindings.
function input.__reload(live, fresh)
  local tablex = require("fe.tablex")
  local before = tablex.deep_copy(live.defaults)
  for _, group in ipairs({ "actions", "movement", "axes" }) do
    tablex.adopt(live.defaults[group], fresh.defaults[group], live.file_defaults[group])
  end
  live.file_defaults = tablex.deep_copy(fresh.defaults)
  if not tablex.deep_equal(before, live.defaults) then
    live.use_defaults()
    require("fe.config").apply_bindings(false)
  end
end

local function digital(action)
  local out = {}
  for _, raw in ipairs(fe.input.bindings(action)) do
    if not raw:find("[%+%-]$") then out[#out + 1] = raw end
  end
  return out
end

--- Rebuild the movement axes from the current direction bindings.
function input.sync_axes()
  for name, axis in pairs(input.defaults.movement) do
    fe.input.define_axis(name, { negative = digital(axis.negative), positive = digital(axis.positive), analog = axis.analog })
  end
end

--- Apply rebinding overrides {action = raw inputs}. Overrides for actions
--- that do not exist (yet), or with invalid inputs, are skipped; returns
--- them as {action, error} records.
---@param overrides? table<string, string[]>
---@return {action: string, error: string}[]
function input.apply_bindings(overrides)
  local failed = {}
  for name, inputs in pairs(overrides or {}) do
    local ok, err = pcall(fe.input.bind, name, inputs)
    if not ok then failed[#failed + 1] = { action = name, error = tostring(err) } end
  end
  table.sort(failed, function(a, b) return a.action < b.action end)
  input.sync_axes()
  return failed
end

--- Define the default actions and axes, then apply rebinding overrides
--- (see `input.apply_bindings`, whose result this returns).
---@param overrides? table<string, string[]>
function input.use_defaults(overrides)
  for name, inputs in pairs(input.defaults.actions) do
    fe.input.define(name, inputs)
  end
  for name, axis in pairs(input.defaults.axes) do
    fe.input.define_axis(name, axis)
  end
  return input.apply_bindings(overrides)
end

local glyphs = {
  keyboard = function(raw) return (raw:gsub("^key:", "")) end,
  gamepad = function(raw)
    local button = (raw:gsub("^pad:", ""))
    local key = "fe.button." .. button
    local name = i18n.t(key)
    return name == key and button or name
  end,
}
glyphs.mouse = glyphs.keyboard

--- Text naming the first binding of `action` for the last used device,
--- e.g. "Z" or "A" (gamepad names are the `fe.button.<name>` strings).
--- `fe.ui.prompt` shows button glyphs and falls back to this.
---@param action string
---@return string
function input.prompt(action)
  local device = fe.input.last_device()
  local prefix = device == "gamepad" and "pad:" or "key:"
  for _, raw in ipairs(fe.input.bindings(action)) do
    if raw:sub(1, #prefix) == prefix and not raw:find("[%+%-]$") then
      return glyphs[device](raw)
    end
  end
  local first = fe.input.bindings(action)[1]
  return first and (first:gsub("^%w+:", "")) or "?"
end

--- Menu navigation with key repeat: returns -1, 0 or 1 for an axis pair.
function input.navigate(negative, positive)
  if fe.input.repeating(negative) then return -1 end
  if fe.input.repeating(positive) then return 1 end
  return 0
end

return input
