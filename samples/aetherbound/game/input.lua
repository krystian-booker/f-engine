--- The game's actions and its button prompts.
---
--- `input.prompt(action)` is markup for the button bound to an action on the
--- device in use right now: a keycap after a key press, the gamepad's own
--- face button after a button press or after a controller is plugged in. The
--- engine tracks the device (`fe.input.device_type()`); prompts are looked up
--- every time they are drawn, so the whole UI follows a switch at once.
local input = {}

input.defaults = {
  up = { "key:Up", "key:W", "pad:dpup", "pad:lefty-" },
  down = { "key:Down", "key:S", "pad:dpdown", "pad:lefty+" },
  left = { "key:Left", "key:A", "pad:dpleft", "pad:leftx-" },
  right = { "key:Right", "key:D", "pad:dpright", "pad:leftx+" },
  confirm = { "key:Return", "key:Z", "key:Space", "pad:a" },
  cancel = { "key:Escape", "key:X", "key:Backspace", "pad:b" },
  run = { "key:Left Shift", "pad:x" },
  skip = { "key:Tab", "pad:start" },
}

--- ui/glyphs.png (tools/aetherbound/ui/generate.py): 64 px cells, 8 per row.
local CELL, PER_ROW = 64, 8
local sheet = { keyboard = {}, xbox = {}, playstation = {}, switch = {} }
local layout = {
  keyboard = { "Return", "Escape", "Space", "Z", "X", "Left Shift", "Tab", "Backspace", "Up", "Down", "Left", "Right",
    "W", "A", "S", "D" },
  xbox = { "a", "b", "x", "y", "start", "dpup", "dpdown", "dpleft", "dpright" },
  playstation = { "a", "b", "x", "y", "start", "dpup", "dpdown", "dpleft", "dpright" },
  switch = { "a", "b", "x", "y", "start", "dpup", "dpdown", "dpleft", "dpright" },
}
local rows = { keyboard = 0, xbox = 2, playstation = 4, switch = 6 }
local sheet_of = { keyboard = "keyboard", xbox = "xbox", playstation = "playstation", switch = "switch",
  steamdeck = "xbox", generic = "xbox" }

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
  for device, names in pairs(layout) do
    for i, name in ipairs(names) do
      local id = "glyph." .. device .. "." .. name:lower():gsub("[^%w]+", "_")
      fe.text.icon(id, { image = "ui/glyphs.png", linear = true,
        rect = { ((i - 1) % PER_ROW) * CELL, (rows[device] + (i - 1) // PER_ROW) * CELL, CELL, CELL } })
      sheet[device][name] = id
    end
  end
end

function input.device()
  return sheet_of[fe.input.device_type()] or "xbox"
end

--- Markup for the button bound to `action` on the device in use.
function input.prompt(action)
  local device = input.device()
  local pad = device ~= "keyboard"
  local fallback
  for _, raw in ipairs(fe.input.bindings(action)) do
    local kind, name = raw:match("^(%a+)%d*:(.+)$")
    local analog = name and #name > 1 and name:find("[%+%-]$")
    if kind and not analog and ((pad and kind == "pad") or (not pad and kind == "key")) then
      local id = sheet[device][name]
      if id then return "{icon:" .. id .. "}" end
      fallback = fallback or "[" .. fe.input.label(raw) .. "]"
    end
  end
  return fallback or "[" .. action .. "]"
end

--- "glyph Label   glyph Label" for a row of hints: {{"confirm", "Next"}, ...}.
function input.hints(list)
  local parts = {}
  for _, hint in ipairs(list) do
    parts[#parts + 1] = input.prompt(hint[1]) .. " " .. hint[2]
  end
  return table.concat(parts, "     ")
end

--- Menu navigation: -1, 0 or 1 with key repeat.
function input.step(negative, positive)
  if fe.input.repeating(negative, 0.35, 0.1) then return -1 end
  if fe.input.repeating(positive, 0.35, 0.1) then return 1 end
  return 0
end

return input
