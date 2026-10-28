--- Title screen. Configure with callbacks; replace the module for a
--- custom title.
---
--- ```lua
--- scene.push(require("fe.ui.screens.title").new{
---   title = "Crystal of the Vale",
---   new_game = function() ... end,
---   continue = function(slot) field.resume() end,
--- })
--- ```
local save = require("fe.save")
local scene = require("fe.scene")
local task = require("fe.task")
local ui = require("fe.ui")

local title = {}

function title.new(options)
  local view = { name = "title", opaque = true, blocks_update = true, time = 0 }
  local has_save = #save.list() > 0
  local entries = {
    { label = "New Game", value = "new" },
    { label = "Continue", value = "continue", enabled = has_save },
    { label = "Settings", value = "config" },
    { label = "Quit", value = "quit" },
  }
  local busy = false
  local list = ui.List({ items = entries, rows = #entries, on_confirm = function(item)
    if busy then return end
    busy = true
    task.named("title:" .. item.value, function()
      if item.value == "new" then
        scene.remove(view)
        options.new_game()
      elseif item.value == "continue" then
        local slot = require("fe.ui.screens.save").open("load")
        if slot then
          scene.remove(view)
          options.continue(slot)
        end
      elseif item.value == "config" then
        require("fe.ui.screens.config").open()
      elseif item.value == "quit" then
        fe.app.quit()
      end
      busy = false
    end)
  end })
  list:select(has_save and 2 or 1)
  function view:update(dt)
    self.time = self.time + dt
    if not busy then list:update() end
  end
  function view:draw()
    local cw, ch = fe.canvas.size()
    if options.draw_background then
      options.draw_background()
    else
      fe.canvas.rect(0, 0, cw, ch, 0x0b1429ff)
    end
    local glow = 0.6 + 0.4 * math.sin(self.time * 1.5)
    ui.text(options.title or fe.app.title(), 0, math.floor(ch * 0.25), { width = cw, align = "center",
      color = ui.theme.heading, size = 20, outline = 0x000000ff })
    if options.subtitle then
      ui.text(options.subtitle, 0, math.floor(ch * 0.25) + 26, { width = cw, align = "center", color = ui.theme.muted })
    end
    local w, h = 110, #entries * ui.theme.row_height + 12
    local x, y = math.floor((cw - w) / 2), math.floor(ch * 0.55)
    fe.canvas.push({ alpha = 0.85 + 0.15 * glow })
    ui.window(x, y, w, h)
    fe.canvas.pop()
    list:draw(x + 22, y + 6, w - 30)
  end
  return view
end

return title
