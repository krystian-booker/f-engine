--- Settings screen: volumes, text speed, battle mode, fullscreen and key
--- rebinding. `screen.open()` waits until closed.
local config = require("fe.config")
local input = require("fe.input")
local scene = require("fe.scene")
local task = require("fe.task")
local ui = require("fe.ui")

local screen = {
  --- Actions offered for rebinding.
  rebindable = { "confirm", "cancel", "menu", "info", "page_left", "page_right", "run", "up", "down", "left", "right" },
}

local function percent(v) return math.floor(v * 100 + 0.5) .. "%" end

local function entries()
  local v = config.values
  local list = {
    { label = "Master volume", right = percent(v.master), key = "master", step = 0.05 },
    { label = "Music volume", right = percent(v.music), key = "music", step = 0.05 },
    { label = "Effects volume", right = percent(v.sfx), key = "sfx", step = 0.05 },
    { label = "Text speed", right = v.text_speed == 0 and "Instant" or tostring(v.text_speed), key = "text_speed", step = 15, max = 120 },
    { label = "Battle mode", right = v.battle_wait and "Wait" or "Active", key = "battle_wait", toggle = true },
    { label = "Fullscreen", right = v.fullscreen and "On" or "Off", key = "fullscreen", toggle = true },
  }
  for _, action in ipairs(screen.rebindable) do
    list[#list + 1] = { label = "Key: " .. action, right = input.prompt(action), rebind = action }
  end
  list[#list + 1] = { label = "Reset controls", reset = true }
  return list
end

function screen.open()
  local done, rebinding = false, nil
  local list
  list = ui.List({ items = entries(), rows = 12, on_cancel = function() done = true end,
    on_confirm = function(item)
      if item.toggle then
        config.set(item.key, not config.values[item.key])
      elseif item.rebind then
        rebinding = item.rebind
      elseif item.reset then
        config.reset_bindings()
      end
      list:set_items(entries())
    end })
  local view = { name = "config", blocks_update = true }
  function view:update()
    if rebinding then
      local pressed = fe.input.pressed_inputs()
      if #pressed > 0 then
        if pressed[1] ~= "key:Escape" then
          local current = fe.input.bindings(rebinding)
          local kept = {}
          local prefix = pressed[1]:match("^(%w+:)")
          for _, raw in ipairs(current) do
            if raw:sub(1, #prefix) ~= prefix then kept[#kept + 1] = raw end
          end
          table.insert(kept, 1, pressed[1])
          config.bind(rebinding, kept)
        end
        rebinding = nil
        list:set_items(entries())
      end
      return
    end
    local item = list:selected()
    local step = input.navigate("left", "right")
    if item and item.key and not item.toggle and step ~= 0 then
      local max = item.max or 1
      local value = math.max(0, math.min(max, config.values[item.key] + step * item.step))
      config.set(item.key, value)
      list:set_items(entries())
    end
    list:update()
  end
  function view:draw()
    local cw, ch = fe.canvas.size()
    local w, h = math.min(cw - 30, 260), 12 * ui.theme.row_height + 30
    local x, y = ui.anchor(w, h, "center")
    ui.window(x, y, w, h)
    ui.text("Settings", x + 10, y + 6, { color = ui.theme.heading })
    list:draw(x + 22, y + 22, w - 32)
    if rebinding then
      ui.window(x + 20, y + h / 2 - 12, w - 40, 24)
      ui.text("Press a key or button for " .. rebinding, x + 28, y + h / 2 - 5)
    end
  end
  scene.push(view)
  task.wait_until(function() return done end)
  scene.remove(view)
end

return screen
