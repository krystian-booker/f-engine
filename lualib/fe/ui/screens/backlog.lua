--- Dialogue backlog: the most recent lines of `fe.dialogue.backlog`, newest
--- at the bottom. Up/Down scroll a line, Left/Right a page; Confirm or
--- Cancel closes. `screen.open()` waits until closed (returns nothing).
--- The menu has a "Log" page for it and `dialogue.style.backlog_input`
--- opens it while a message box is shown.
local i18n = require("fe.i18n")
local input = require("fe.input")
local tablex = require("fe.tablex")
local ui = require("fe.ui")

local screen = { width = 300 }

--- The file's values, for `__reload`.
screen.file_defaults = { width = screen.width }

--- Adopt an edited width on hot reload unless the game changed it.
function screen.__reload(live, fresh)
  live.file_defaults = tablex.adopt(live, { width = fresh.width }, live.file_defaults)
end

i18n.defaults({ fe = { backlog = { title = "Log", empty = "Nothing has been said yet." } } })

local function entry_text(line)
  if line.speaker and line.speaker ~= "" then
    return "{color:heading}" .. line.speaker .. "{color:reset}\n" .. line.text
  end
  return line.text
end

function screen.open()
  local log = require("fe.dialogue").backlog
  local view = { name = "backlog", blocks_update = true, last = #log }
  local function body_size()
    local cw, ch = fe.canvas.size()
    return math.min(cw - 16, screen.width), ch - 16
  end
  function view:update()
    local _, h = body_size()
    local page = math.max(1, math.floor(h / 40))
    local step = input.navigate("up", "down") + input.navigate("left", "right") * page
    if step ~= 0 then
      self.last = tablex.clamp(self.last + step, math.min(1, #log), #log)
      ui.sound("move")
    end
    if fe.input.pressed("confirm") or fe.input.pressed("cancel") then
      ui.sound("cancel")
      ui.close(self)
    end
  end
  function view:draw()
    local w, h = body_size()
    local x, y = ui.anchor(w, h, "center")
    local inner = w - ui.theme.padding * 2 - 4
    ui.window(x, y, w, h)
    ui.text(ui.t("fe.backlog.title"), x + ui.theme.padding, y + ui.theme.padding, { color = ui.theme.heading })
    local top = y + ui.theme.padding + 14
    if #log == 0 then
      ui.text(ui.t("fe.backlog.empty"), x + ui.theme.padding + 2, top, { color = ui.theme.muted })
      return
    end
    local bottom = y + h - ui.theme.padding
    for i = self.last, 1, -1 do
      local text = entry_text(log[i])
      local _, th = ui.measure(text, { width = inner })
      if bottom - th < top then break end
      bottom = bottom - th
      ui.text(text, x + ui.theme.padding + 2, bottom, { width = inner })
      bottom = bottom - 6
    end
    if self.last < #log then
      ui.text("▼", x + w - 14, y + h - 12, { color = ui.theme.muted })
    end
  end
  ui.run_screen(view)
end

return screen
