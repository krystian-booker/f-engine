--- Name entry: an on-screen letter grid (works with a gamepad) plus typing
--- with the keyboard and the system IME (`fe.input.text_begin`).
--- `screen.open(default, max_length)` waits and returns the name, or nil
--- when the player presses Cancel with an empty name (Cancel otherwise
--- deletes a letter).
---
--- Typed text and IME candidates go into the name
--- (the composition is shown underlined after it), Backspace deletes, Enter
--- finishes and Escape cancels; the grid still works with a gamepad or the
--- arrow keys. `max_length` counts characters, not bytes.
local i18n = require("fe.i18n")
local task = require("fe.task")
local ui = require("fe.ui")

local screen = {
  letters = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-' .",
  columns = 13,
}

i18n.defaults({ fe = { name_entry = { name = "Name: ", delete = "Del", done = "OK" } } })

local function length(text)
  return utf8.len(text) or #text
end

local function drop_last(text)
  if text == "" then return text end
  return text:sub(1, (utf8.offset(text, -1) or #text) - 1)
end

local function contains(list, value)
  for _, item in ipairs(list) do
    if item == value then return true end
  end
  return false
end

local function layout()
  local w, h = screen.columns * 18 + 30, 6 * 13 + 44
  local x, y = ui.anchor(w, h, "center")
  return x, y, w, h
end

function screen.open(default, max_length)
  max_length = max_length or 8
  local name = default or ""
  local composing = ""
  local function add(text)
    for _, code in utf8.codes(text) do
      if length(name) >= max_length then return end
      name = name .. utf8.char(code)
    end
  end
  local entries = {}
  for i = 1, #screen.letters do entries[i] = { label = screen.letters:sub(i, i) } end
  entries[#entries + 1] = { label = ui.t("fe.name_entry.delete"), value = "delete" }
  entries[#entries + 1] = { label = ui.t("fe.name_entry.done"), value = "done" }
  local view = { name = "name_entry", blocks_update = true }
  local function finish()
    if length(name) > 0 then ui.close(view, name) end
  end
  local list = ui.List({ items = entries, columns = screen.columns, rows = 6, row_height = 13,
    on_confirm = function(item)
      if item.value == "delete" then
        name = drop_last(name)
      elseif item.value == "done" then
        finish()
      else
        add(item.label)
      end
    end,
    on_cancel = function()
      if name == "" then ui.close(view) else name = drop_last(name) end
    end })
  -- Keyboard text and IME; true when it used this tick's input.
  local function type_text()
    local typed = fe.input.text()
    composing = fe.input.composition() or ""
    if typed ~= "" then
      add(typed)
      return true
    end
    if composing ~= "" then return true end
    local pressed = fe.input.pressed_inputs()
    if contains(pressed, "key:Backspace") then
      name = drop_last(name)
      return true
    elseif contains(pressed, "key:Return") then
      finish()
      return true
    elseif contains(pressed, "key:Escape") then
      ui.close(view)
      return true
    end
    return false
  end
  function view:update()
    if type_text() then return end
    list:update()
  end
  function view:draw()
    local x, y, w = layout()
    ui.window(x, y, w, 6 * 13 + 44)
    local shown = ui.t("fe.name_entry.name") .. "{color:highlight}" .. name
    local tw = ui.text(shown, x + 10, y + 8)
    if composing ~= "" then
      local cw = ui.text(composing, x + 10 + tw, y + 8, { color = ui.theme.muted })
      fe.canvas.rect(x + 10 + tw, y + 17, cw, 1, ui.theme.muted)
    else
      ui.text("_", x + 10 + tw, y + 8, { color = ui.theme.highlight })
    end
    list:draw(x + 20, y + 26, w - 26)
  end
  local x, y, w = layout()
  fe.input.text_begin({ x = x + 10, y = y + 6, w = w - 20, h = 14 })
  return task.ensure(function() return ui.run_screen(view) end, function() fe.input.text_end() end)
end

return screen
