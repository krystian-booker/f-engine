--- The game's actions and its button prompts.
---
--- Bindings are the defaults below, replaced by what the player rebound
--- (kept in the settings file). `input.prompt(action)` is markup for the
--- button bound to an action on the device in use right now: a keyboard key
--- glyph after a key press, the gamepad's own face button after a button
--- press or after a controller is plugged in. The engine tracks the device
--- (`fe.input.device_type()`); prompts are looked up every time they are
--- drawn, so the whole UI follows a switch at once.
local input = {}

input.defaults = {
  up = { "key:Up", "key:W", "pad:dpup", "pad:lefty-" },
  down = { "key:Down", "key:S", "pad:dpdown", "pad:lefty+" },
  left = { "key:Left", "key:A", "pad:dpleft", "pad:leftx-" },
  right = { "key:Right", "key:D", "pad:dpright", "pad:leftx+" },
  confirm = { "key:Return", "key:Z", "key:Space", "pad:a" },
  cancel = { "key:Escape", "key:X", "key:Backspace", "pad:b" },
  menu = { "key:Tab", "key:C", "pad:y" },
  run = { "key:Left Shift", "pad:x" },
  board = { "key:V", "pad:rightshoulder" },
}

--- Glyph sheets shipped with the engine: 10x10 cells, 8 per row.
local pad_buttons = { "a", "b", "x", "y", "leftshoulder", "rightshoulder", "lefttrigger", "righttrigger", "start",
  "back", "dpup", "dpdown", "dpleft", "dpright", "leftstick", "rightstick" }
local sheets = {
  keyboard = { image = "@fe/assets/ui/glyphs_keyboard.png", names = {
    "A", "B", "C", "D", "E", "F", "G", "H", "I", "J", "K", "L", "M", "N", "O", "P", "Q", "R", "S", "T", "U", "V",
    "W", "X", "Y", "Z", "0", "1", "2", "3", "4", "5", "6", "7", "8", "9", "Return", "Space", "Escape", "Tab",
    "Left Shift", "Right Shift", "Left Ctrl", "Right Ctrl", "Backspace", "Up", "Down", "Left", "Right", "PageUp",
    "PageDown", "mouse:left", "mouse:right", "=", "-" } },
  xbox = { image = "@fe/assets/ui/glyphs_xbox.png", names = pad_buttons },
  playstation = { image = "@fe/assets/ui/glyphs_playstation.png", names = pad_buttons },
  switch = { image = "@fe/assets/ui/glyphs_switch.png", names = pad_buttons },
}
local sheet_of = { keyboard = "keyboard", xbox = "xbox", playstation = "playstation", switch = "switch",
  steamdeck = "xbox", generic = "xbox" }
local icons = {}

local function icon_id(sheet, name)
  return "glyph." .. sheet .. "." .. name:lower():gsub("[^%w]+", "_")
end

--- Define the actions (with the player's rebindings) and register the glyphs.
function input.setup(bindings)
  fe.input.clear()
  for action, inputs in pairs(input.defaults) do
    fe.input.define(action, bindings and bindings[action] or inputs)
  end
  fe.input.define_axis("move_x", { negative = { "key:Left", "key:A", "pad:dpleft" },
    positive = { "key:Right", "key:D", "pad:dpright" }, analog = { "pad:leftx" }, deadzone = 0.25 })
  fe.input.define_axis("move_z", { negative = { "key:Up", "key:W", "pad:dpup" },
    positive = { "key:Down", "key:S", "pad:dpdown" }, analog = { "pad:lefty" }, deadzone = 0.25 })
  for sheet, spec in pairs(sheets) do
    icons[sheet] = {}
    for i, name in ipairs(spec.names) do
      local id = icon_id(sheet, name)
      fe.text.icon(id, { image = spec.image, rect = { ((i - 1) % 8) * 10, ((i - 1) // 8) * 10, 10, 10 } })
      icons[sheet][name] = id
    end
  end
end

--- The glyph sheet for the device the player is using.
function input.device()
  return sheet_of[fe.input.device_type()] or "xbox"
end

--- Markup for the button bound to `action` on the device in use: its glyph,
--- or the key's label under the player's keyboard layout when the sheet has
--- no picture of it.
function input.prompt(action)
  local sheet = input.device()
  local pad = sheet ~= "keyboard"
  local fallback
  for _, raw in ipairs(fe.input.bindings(action)) do
    local kind, name = raw:match("^(%a+)%d*:(.+)$")
    local analog = name and #name > 1 and name:find("[%+%-]$")
    if kind and not analog and ((pad and kind == "pad") or (not pad and (kind == "key" or kind == "mouse"))) then
      local id = icons[sheet][kind == "mouse" and raw or name]
      if id then return "{icon:" .. id .. "}" end
      fallback = fallback or "[" .. fe.input.label(raw) .. "]"
    end
  end
  return fallback or "[" .. action .. "]"
end

--- "glyph Label   glyph Label" for a row of hints: {{"confirm", "Talk"}, ...}.
function input.hints(list)
  local parts = {}
  for _, hint in ipairs(list) do
    parts[#parts + 1] = input.prompt(hint[1]) .. " " .. hint[2]
  end
  return table.concat(parts, "   ")
end

--- Menu navigation: -1, 0 or 1 with key repeat.
function input.step(negative, positive)
  if fe.input.repeating(negative, 0.35, 0.09) then return -1 end
  if fe.input.repeating(positive, 0.35, 0.09) then return 1 end
  return 0
end

return input
