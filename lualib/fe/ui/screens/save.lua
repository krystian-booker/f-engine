--- Save and load screen. `screen.open("save" | "load")` waits and returns
--- the slot used (or nil). Slots are `screen.slots`; the load list also
--- shows the autosave.
local save = require("fe.save")
local scene = require("fe.scene")
local task = require("fe.task")
local ui = require("fe.ui")

local screen = { slots = { "slot1", "slot2", "slot3", "slot4" } }

local function clock(seconds)
  seconds = math.floor(seconds or 0)
  return string.format("%d:%02d", seconds // 3600, (seconds // 60) % 60)
end

local function describe(meta)
  if not meta then return "Empty", "" end
  local names = {}
  for _, member in ipairs(meta.party or {}) do names[#names + 1] = member.name .. " Lv" .. member.level end
  return (meta.location or "Unknown") .. "  " .. clock(meta.playtime), table.concat(names, ", ")
end

function screen.open(mode)
  local metas = {}
  for _, info in ipairs(save.list()) do metas[info.slot] = info.meta end
  local slots = {}
  if mode == "load" and metas[save.autosave_slot] then slots[#slots + 1] = save.autosave_slot end
  for _, slot in ipairs(screen.slots) do slots[#slots + 1] = slot end
  local entries = {}
  for i, slot in ipairs(slots) do
    local line, party_line = describe(metas[slot])
    local label = slot == save.autosave_slot and "Auto" or ("File " .. i)
    entries[#entries + 1] = { label = label, value = slot, line = line, party = party_line,
      enabled = mode == "save" or metas[slot] ~= nil }
  end
  local cw, ch = fe.canvas.size()
  local done, chosen = false, nil
  local list = ui.List({
    items = entries, rows = math.min(#entries, 6), row_height = 28,
    on_confirm = function(item) chosen, done = item.value, true end,
    on_cancel = function() done = true end,
    render = function(item, x, y, w, selected)
      ui.text(item.label, x, y, { color = selected and ui.theme.highlight or (item.enabled and ui.theme.text or ui.theme.disabled) })
      ui.text(item.line, x + 44, y, { color = item.enabled and ui.theme.text or ui.theme.disabled })
      ui.text(item.party, x + 44, y + 11, { color = ui.theme.muted })
    end,
  })
  local view = { name = "save_screen", blocks_update = true }
  function view:update() list:update() end
  function view:draw()
    local w, h = cw - 40, list.rows * 28 + 30
    local x, y = ui.anchor(w, h, "center")
    ui.window(x, y, w, h)
    ui.text(mode == "save" and "Save" or "Load", x + 10, y + 6, { color = ui.theme.heading })
    list:draw(x + 22, y + 22, w - 30)
  end
  scene.push(view)
  task.wait_until(function() return done end)
  scene.remove(view)
  if not chosen then return nil end
  if mode == "save" then
    save.write(chosen, { thumbnail = true })
    ui.sound("save")
    return chosen
  end
  local meta, reason = save.load(chosen)
  if not meta then
    fe.log.warn("Could not load " .. chosen .. ": " .. tostring(reason))
    return nil
  end
  return chosen
end

return screen
