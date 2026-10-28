--- Default action maps and button prompts. The engine has no built-in
--- bindings; call `input.use_defaults()` (fe.game does) or define your own
--- with fe.input.define. Actions used by the library kits:
---   up, down, left, right   menu navigation
---   confirm, cancel, menu   menus, dialogue, interaction
---   page_left, page_right   switch characters/tabs, battle modifiers
---   run, camera_left, camera_right, camera_reset, zoom_in, zoom_out, info
--- Axes: move_x, move_y (field movement), camera_x.
local input = {}

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
  },
  axes = {
    move_x = { negative = { "key:Left", "key:A", "pad:dpleft" }, positive = { "key:Right", "key:D", "pad:dpright" }, analog = { "pad:leftx" } },
    move_y = { negative = { "key:Up", "key:W", "pad:dpup" }, positive = { "key:Down", "key:S", "pad:dpdown" }, analog = { "pad:lefty" } },
    camera_x = { negative = {}, positive = {}, analog = { "pad:rightx" } },
  },
}

--- Define the default actions, then apply saved rebinding overrides.
---@param overrides? table<string, string[]>
function input.use_defaults(overrides)
  for name, inputs in pairs(input.defaults.actions) do
    fe.input.define(name, inputs)
  end
  for name, axis in pairs(input.defaults.axes) do
    fe.input.define_axis(name, axis)
  end
  for name, inputs in pairs(overrides or {}) do
    fe.input.bind(name, inputs)
  end
end

local glyphs = {
  keyboard = function(raw) return raw:gsub("^key:", "") end,
  gamepad = function(raw)
    local names = { a = "A", b = "B", x = "X", y = "Y", leftshoulder = "LB", rightshoulder = "RB",
      lefttrigger = "LT", righttrigger = "RT", start = "Start", back = "Back", dpup = "Up", dpdown = "Down",
      dpleft = "Left", dpright = "Right", rightstick = "R3", leftstick = "L3" }
    local button = raw:gsub("^pad:", "")
    return names[button] or button
  end,
}
glyphs.mouse = glyphs.keyboard

--- Text naming the first binding of `action` for the last used device,
--- e.g. "Z" or "A". Games can replace `input.prompt` to draw icons instead.
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
  return first and first:gsub("^%w+:", "") or "?"
end

--- Menu navigation with key repeat: returns -1, 0 or 1 for an axis pair.
function input.navigate(negative, positive)
  if fe.input.repeating(negative) then return -1 end
  if fe.input.repeating(positive) then return 1 end
  return 0
end

return input
