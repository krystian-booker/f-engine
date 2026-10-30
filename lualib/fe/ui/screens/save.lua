--- Save and load screen. `screen.open("save" | "load")` waits and returns
--- the slot used, or nil when cancelled or when saving/loading failed (the
--- player is told why). Slots are `screen.slots`; the load list also shows
--- the autosave. Each slot shows its location, playtime, party, thumbnail
--- (`fe.save.thumbnail_path`) and when it was saved in local time
--- (`fe.time.local_date`, formatted by the `fe.save.date` string).
local i18n = require("fe.i18n")
local save = require("fe.save")
local tablex = require("fe.tablex")
local ui = require("fe.ui")

local screen = { slots = { "slot1", "slot2", "slot3", "slot4" }, thumbnail_size = { 40, 23 } }

--- The file's values, for `__reload`.
screen.file_defaults = { slots = tablex.deep_copy(screen.slots), thumbnail_size = tablex.deep_copy(screen.thumbnail_size) }

--- Adopt edited slots and thumbnail size on hot reload unless the game
--- changed them.
function screen.__reload(live, fresh)
  live.file_defaults = tablex.adopt(live, { slots = fresh.slots, thumbnail_size = fresh.thumbnail_size },
    live.file_defaults)
end

i18n.defaults({ fe = { save = {
  save = "Save", load = "Load", empty = "Empty", unknown = "Unknown", auto = "Auto", file = "File {number}",
  level = "{name} Lv{level}", save_failed = "Could not save the game.", load_failed = "Could not load this save.",
  date = "{year}-{month}-{day} {hour}:{min}",
} } })

local function clock(seconds)
  seconds = math.floor(seconds or 0)
  return string.format("%d:%02d", seconds // 3600, (seconds // 60) % 60)
end

--- When a save was made, in local time ("" without a time).
---@param epoch number? seconds since the Unix epoch
---@return string
function screen.date(epoch)
  if not epoch then return "" end
  local d = fe.time.local_date(epoch)
  local function two(n) return string.format("%02d", n) end
  return ui.t("fe.save.date", { year = d.year, month = two(d.month), day = two(d.day), hour = two(d.hour),
    min = two(d.min) })
end

local function describe(meta)
  if not meta then return ui.t("fe.save.empty"), "" end
  local names = {}
  for _, member in ipairs(meta.party or {}) do
    names[#names + 1] = ui.t("fe.save.level", { name = member.name, level = member.level })
  end
  local line = (meta.location or ui.t("fe.save.unknown")) .. "  " .. clock(meta.playtime)
  local date = screen.date(meta.saved_at)
  if date ~= "" then line = line .. "  {color:muted}" .. date .. "{color:reset}" end
  return line, table.concat(names, ", ")
end

--- The slot's thumbnail image path, or nil.
---@param slot string
---@return string?
function screen.thumbnail(slot)
  return fe.save.thumbnail_path(slot)
end

local function label(slot)
  if slot == save.autosave_slot then return ui.t("fe.save.auto") end
  for i, other in ipairs(screen.slots) do
    if other == slot then return ui.t("fe.save.file", { number = i }) end
  end
  return slot
end

function screen.open(mode)
  local metas = {}
  for _, info in ipairs(save.list()) do metas[info.slot] = info.meta end
  local slots = {}
  if mode == "load" and metas[save.autosave_slot] then slots[#slots + 1] = save.autosave_slot end
  for _, slot in ipairs(screen.slots) do slots[#slots + 1] = slot end
  local entries = {}
  for _, slot in ipairs(slots) do
    local line, party_line = describe(metas[slot])
    entries[#entries + 1] = { label = label(slot), value = slot, line = line, party = party_line,
      enabled = mode == "save" or metas[slot] ~= nil, thumbnail = metas[slot] and screen.thumbnail(slot) }
  end
  local cw = fe.canvas.size()
  local view = { name = "save_screen", blocks_update = true }
  local list = ui.List({
    items = entries, rows = math.min(#entries, 6), row_height = 28,
    on_confirm = function(item) ui.close(view, item.value) end,
    on_cancel = function() ui.close(view) end,
    render = function(item, x, y, w, selected)
      ui.text(item.label, x, y, { color = selected and ui.theme.highlight or (item.enabled and ui.theme.text or ui.theme.disabled) })
      local tx = x + 44
      if item.thumbnail then
        local tw, th = screen.thumbnail_size[1], screen.thumbnail_size[2]
        fe.canvas.image(item.thumbnail, tx, y + 1, { w = tw, h = th, linear = true })
        tx = tx + tw + 6
      end
      ui.text(item.line, tx, y, { color = item.enabled and ui.theme.text or ui.theme.disabled })
      ui.text(item.party, tx, y + 11, { color = ui.theme.muted })
    end,
  })
  function view:update() list:update() end
  function view:draw()
    local w, h = cw - 40, list.rows * 28 + 30
    local x, y = ui.anchor(w, h, "center")
    ui.window(x, y, w, h)
    ui.text(ui.t(mode == "save" and "fe.save.save" or "fe.save.load"), x + 10, y + 6, { color = ui.theme.heading })
    list:draw(x + 22, y + 22, w - 30)
  end
  local chosen = ui.run_screen(view)
  if not chosen then return nil end
  if mode == "save" then
    local ok, err = pcall(save.write, chosen, { thumbnail = true })
    if not ok then
      fe.log.error("Saving to " .. chosen .. " failed: " .. tostring(err))
      ui.message(ui.t("fe.save.save_failed"))
      return nil
    end
    ui.sound("save")
    return chosen
  end
  local meta, reason = save.load(chosen)
  if not meta then
    fe.log.warn("Could not load " .. chosen .. ": " .. tostring(reason))
    ui.message(ui.t("fe.save.load_failed"))
    return nil
  end
  return chosen
end

return screen
