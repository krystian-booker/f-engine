--- Settings screen: volumes (master, music, effects, voice), text speed,
--- auto-advance, battle mode, accessibility (text size, high contrast),
--- language, display and key rebinding. `screen.open()` waits until closed
--- (returns nothing) and writes the settings file once, when it closes.
---
--- Display rows: fullscreen, vsync, window size (when the monitor lists
--- fullscreen modes) and monitor (when there are several). Languages
--- are those `fe.config.languages()` finds, named by the `fe.language.<code>`
--- strings (the code itself when there is none).
local config = require("fe.config")
local i18n = require("fe.i18n")
local input = require("fe.input")
local tablex = require("fe.tablex")
local task = require("fe.task")
local ui = require("fe.ui")

local screen = {
  --- Actions offered for rebinding.
  rebindable = { "confirm", "cancel", "menu", "info", "page_left", "page_right", "run", "skip", "up", "down", "left",
    "right" },
  --- Text sizes offered.
  text_scales = { 1, 1.25, 1.5 },
}

i18n.defaults({ fe = {
  settings = {
    title = "Settings", master = "Master volume", music = "Music volume", sfx = "Effects volume",
    voice = "Voice volume", text_speed = "Text speed", instant = "Instant", auto_advance = "Auto-advance",
    battle_mode = "Battle mode", wait = "Wait", active = "Active", text_scale = "Text size",
    high_contrast = "High contrast", language = "Language", fullscreen = "Fullscreen", window_size = "Window size",
    vsync = "VSync", monitor = "Monitor", on = "On", off = "Off", key = "Key: {action}", reset = "Reset controls",
    press = "Press a key or button for {action}",
  },
  action = {
    up = "Up", down = "Down", left = "Left", right = "Right", confirm = "Confirm", cancel = "Cancel", menu = "Menu",
    info = "Info", page_left = "Page left", page_right = "Page right", run = "Run", skip = "Fast-forward",
  },
  language = { en = "English", fr = "Français", de = "Deutsch", es = "Español", it = "Italiano", pt = "Português" },
} })

local function percent(v) return math.floor(v * 100 + 0.5) .. "%" end

local function action_name(action)
  return ui.t("fe.action." .. action)
end

--- A language's display name.
---@param code string
---@return string
function screen.language_name(code)
  local key = "fe.language." .. code
  local name = ui.t(key)
  return name == key and code or name
end

local function on_off(value)
  return value and ui.t("fe.settings.on") or ui.t("fe.settings.off")
end

local function cycle(list, current, step)
  local index = tablex.index_of(list, current) or 1
  return list[(index - 1 + step) % #list + 1]
end

local function mode_size(mode)
  return mode.width or mode[1], mode.height or mode[2]
end

local function display_rows(list)
  local t = ui.t
  local v = config.values
  list[#list + 1] = { label = t("fe.settings.fullscreen"), right = on_off(v.fullscreen), key = "fullscreen", toggle = true }
  local current = fe.app.display()
  local modes = current.modes or {}
  if #modes > 0 then
    list[#list + 1] = { label = t("fe.settings.window_size"), right = current.width .. "x" .. current.height,
      cycle = function(step)
        local index = 1
        for i, mode in ipairs(modes) do
          local w, h = mode_size(mode)
          if w == current.width and h == current.height then index = i end
        end
        local w, h = mode_size(modes[(index - 1 + step) % #modes + 1])
        return { width = w, height = h }
      end }
  end
  list[#list + 1] = { label = t("fe.settings.vsync"), right = on_off(current.vsync),
    cycle = function() return { vsync = not current.vsync } end }
  local monitors = current.monitors or {}
  if #monitors > 1 then
    list[#list + 1] = { label = t("fe.settings.monitor"), right = tostring(current.monitor),
      cycle = function(step) return { monitor = (current.monitor - 1 + step) % #monitors + 1 } end }
  end
end

local function entries()
  local v = config.values
  local t = ui.t
  local list = {
    { label = t("fe.settings.master"), right = percent(v.master), key = "master", step = 0.05 },
    { label = t("fe.settings.music"), right = percent(v.music), key = "music", step = 0.05 },
    { label = t("fe.settings.sfx"), right = percent(v.sfx), key = "sfx", step = 0.05 },
    { label = t("fe.settings.voice"), right = percent(v.voice), key = "voice", step = 0.05 },
    { label = t("fe.settings.text_speed"), right = v.text_speed == 0 and t("fe.settings.instant") or tostring(v.text_speed),
      key = "text_speed", step = 15, max = 120 },
    { label = t("fe.settings.auto_advance"), right = on_off(v.auto_advance), key = "auto_advance", toggle = true },
    { label = t("fe.settings.battle_mode"), right = v.battle_wait and t("fe.settings.wait") or t("fe.settings.active"),
      key = "battle_wait", toggle = true },
    { label = t("fe.settings.text_scale"), right = percent(v.text_scale or 1), key = "text_scale",
      choices = screen.text_scales },
    { label = t("fe.settings.high_contrast"), right = on_off(v.high_contrast), key = "high_contrast", toggle = true },
    { label = t("fe.settings.language"), right = screen.language_name(v.language or "en"), key = "language",
      choices = config.languages() },
  }
  display_rows(list)
  for _, action in ipairs(screen.rebindable) do
    list[#list + 1] = { label = t("fe.settings.key", { action = action_name(action) }), right = ui.prompt(action),
      rebind = action }
  end
  list[#list + 1] = { label = t("fe.settings.reset"), reset = true }
  return list
end

-- Change the selected row by `step` (left/right, or +1 on confirm).
local function change(item, step)
  if item.toggle then
    config.set(item.key, not config.values[item.key])
  elseif item.choices then
    config.set(item.key, cycle(item.choices, config.values[item.key], step))
  elseif item.cycle then
    local changes = item.cycle(step)
    for key, value in pairs(changes) do
      if key ~= "fullscreen" then config.values.display[key] = value end
    end
    config.set("display", config.values.display)
  elseif item.key then
    config.set(item.key, tablex.clamp(config.values[item.key] + step * item.step, 0, item.max or 1))
  end
end

function screen.open()
  local rebinding
  local view = { name = "config", blocks_update = true }
  local list
  list = ui.List({ items = entries(), rows = 12, on_cancel = function() ui.close(view) end,
    on_confirm = function(item)
      if item.rebind then
        rebinding = item.rebind
      elseif item.reset then
        config.reset_bindings()
      elseif item.toggle or item.choices or item.cycle then
        change(item, 1)
      end
      list:set_items(entries())
    end })
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
    if item and step ~= 0 and (item.key or item.cycle) then
      change(item, step)
      list:set_items(entries())
    end
    list:update()
  end
  function view:draw()
    local cw = fe.canvas.size()
    local w, h = math.min(cw - 30, 260), 12 * ui.row_height() + 30
    local x, y = ui.anchor(w, h, "center")
    ui.window(x, y, w, h)
    ui.text(ui.t("fe.settings.title"), x + 10, y + 6, { color = ui.theme.heading })
    list:draw(x + 22, y + 22, w - 32)
    if rebinding then
      ui.window(x + 20, y + h / 2 - 12, w - 40, 24)
      ui.text(ui.t("fe.settings.press", { action = action_name(rebinding) }), x + 28, y + h / 2 - 5)
    end
  end
  task.ensure(function() ui.run_screen(view) end, config.save)
end

return screen
