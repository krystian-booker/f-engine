--- Drawing helpers for the game's screens: windows, lists with a cursor,
--- gauges, floating toasts. Immediate mode: a scene calls these from its
--- draw every frame.
local audio = require("game.audio")
local input = require("game.input")

local ui = {
  colors = { text = 0xf4f1e8ff, dim = 0x9aa0b4ff, accent = 0xffd27aff, good = 0x8fe3a2ff, bad = 0xff7d7dff,
    hp = 0x6fdc8cff, mp = 0x6fb4ffff, gauge = 0x1c2233ff, shade = 0x000000a0, weak = 0xffb347ff,
    ally = 0x5fa8ffff, foe = 0xff6b6bff },
  toasts = {},
}

function ui.size()
  return fe.canvas.size()
end

function ui.window(x, y, w, h)
  fe.canvas.nine_slice("@fe/assets/ui/window.png", x, y, w, h, 8)
end

function ui.text(markup, x, y, options)
  options = options or {}
  options.color = options.color or ui.colors.text
  return fe.canvas.text(markup, x, y, options)
end

--- A filled gauge with a dark back.
function ui.bar(x, y, w, h, fraction, color)
  fe.canvas.rect(x, y, w, h, ui.colors.gauge)
  local filled = math.floor(w * math.max(0, math.min(1, fraction)) + 0.5)
  if filled > 0 then fe.canvas.rect(x, y, filled, h, color) end
end

--- A new list cursor: `ui.list_input(cursor, count)` moves it with up/down.
function ui.cursor()
  return { index = 1, top = 1 }
end

--- Move a cursor through `count` rows (wrapping); true when it moved.
function ui.list_input(cursor, count)
  if count <= 0 then return false end
  local step = input.step("up", "down")
  if step ~= 0 then
    cursor.index = (cursor.index - 1 + step) % count + 1
    audio.sfx("cursor", { volume = 0.5 })
    return true
  end
  cursor.index = math.max(1, math.min(count, cursor.index))
  return false
end

--- Draw rows in a window with the cursor on the selected one. `rows` are
--- strings or {label, right = "text", disabled = bool}; at most `visible`
--- rows show and the list scrolls to keep the cursor in view.
function ui.list(x, y, w, rows, cursor, visible)
  visible = math.min(visible or #rows, math.max(1, #rows))
  local h = visible * 12 + 12
  ui.window(x, y, w, h)
  if cursor.index < cursor.top then cursor.top = cursor.index end
  if cursor.index >= cursor.top + visible then cursor.top = cursor.index - visible + 1 end
  cursor.top = math.max(1, math.min(cursor.top, math.max(1, #rows - visible + 1)))
  for line = 1, visible do
    local index = cursor.top + line - 1
    local row = rows[index]
    if not row then break end
    local label = type(row) == "table" and row[1] or row
    local disabled = type(row) == "table" and row.disabled
    local ry = y + 6 + (line - 1) * 12
    ui.text(label, x + 16, ry, { color = disabled and ui.colors.dim or ui.colors.text })
    if type(row) == "table" and row.right then
      ui.text(row.right, x + 8, ry, { width = w - 16, align = "right", color = disabled and ui.colors.dim or ui.colors.accent })
    end
    if index == cursor.index then
      fe.canvas.image("@fe/assets/ui/cursor.png", x + 6, ry + 1)
    end
  end
  if cursor.top > 1 then ui.text("^", x + w - 12, y + 1, { color = ui.colors.dim }) end
  if cursor.top + visible - 1 < #rows then ui.text("v", x + w - 12, y + h - 10, { color = ui.colors.dim }) end
  return h
end

--- The row of button hints at the bottom of the screen.
function ui.hints(list)
  local w, h = ui.size()
  fe.canvas.rect(0, h - 13, w, 13, ui.colors.shade)
  ui.text(input.hints(list), 6, h - 11, { color = ui.colors.dim })
end

--- A message that floats at the top of the screen for a while.
function ui.toast(message, seconds)
  ui.toasts[#ui.toasts + 1] = { text = message, left = seconds or 2.5 }
end

function ui.update(dt)
  for i = #ui.toasts, 1, -1 do
    ui.toasts[i].left = ui.toasts[i].left - dt
    if ui.toasts[i].left <= 0 then table.remove(ui.toasts, i) end
  end
end

function ui.draw_toasts()
  local w = ui.size()
  for i, toast in ipairs(ui.toasts) do
    local tw = fe.text.measure(toast.text)
    local x, y = (w - tw) / 2 - 8, 6 + (i - 1) * 20
    fe.canvas.push({ alpha = math.min(1, toast.left * 3) })
    ui.window(x, y, tw + 16, 18)
    ui.text(toast.text, x + 8, y + 5)
    fe.canvas.pop()
  end
end

return ui
