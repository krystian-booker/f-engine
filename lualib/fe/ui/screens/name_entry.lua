--- Name entry with an on-screen letter grid (works with a gamepad).
--- `screen.open(default, max_length)` waits and returns the name.
local scene = require("fe.scene")
local task = require("fe.task")
local ui = require("fe.ui")

local screen = {
  letters = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-' .",
  columns = 13,
}

function screen.open(default, max_length)
  max_length = max_length or 8
  local name = default or ""
  local entries = {}
  for i = 1, #screen.letters do entries[i] = { label = screen.letters:sub(i, i) } end
  entries[#entries + 1] = { label = "Del", value = "delete" }
  entries[#entries + 1] = { label = "OK", value = "done" }
  local done = false
  local list = ui.List({ items = entries, columns = screen.columns, rows = 6, row_height = 13,
    on_confirm = function(item)
      if item.value == "delete" then
        name = name:sub(1, -2)
      elseif item.value == "done" then
        if #name > 0 then done = true end
      elseif #name < max_length then
        name = name .. item.label
      end
    end,
    on_cancel = function() name = name:sub(1, -2) end })
  local view = { name = "name_entry", blocks_update = true }
  function view:update() list:update() end
  function view:draw()
    local w, h = screen.columns * 18 + 30, 6 * 13 + 44
    local x, y = ui.anchor(w, h, "center")
    ui.window(x, y, w, h)
    ui.text("Name: {color:highlight}" .. name .. "_", x + 10, y + 8)
    list:draw(x + 20, y + 26, w - 26)
  end
  scene.push(view)
  task.wait_until(function() return done end)
  scene.remove(view)
  return name
end

return screen
