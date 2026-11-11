--- The party menu (items, echoes, status, save) and the save and load screens.
local audio = require("game.audio")
local data = require("game.data")
local scenes = require("game.scenes")
local state = require("game.state")
local task = require("game.task")
local ui = require("game.ui")

local menu = { slots = { "slot1", "slot2", "slot3" } }

local function hero_rows()
  local rows = {}
  for _, id in ipairs(state.data.party) do
    local record, stats = state.data.heroes[id], state.stats(id)
    rows[#rows + 1] = { data.get("heroes", id).name, right = record.hp .. "/" .. stats.hp, id = id }
  end
  return rows
end

--- Use an item outside battle; true when it was spent.
local function use_item(item, hero_id)
  local effect = item.effect
  if effect.kind == "tent" then
    state.heal_party()
    return true
  end
  local record, stats = state.data.heroes[hero_id], state.stats(hero_id)
  if effect.kind == "heal" and record.hp > 0 and record.hp < stats.hp then
    record.hp = math.min(stats.hp, record.hp + effect.amount)
    return true
  elseif effect.kind == "mp" and record.mp < stats.mp then
    record.mp = math.min(stats.mp, record.mp + effect.amount)
    return true
  elseif effect.kind == "revive" and record.hp <= 0 then
    record.hp = math.max(1, math.floor(stats.hp * effect.fraction))
    return true
  end
  return false
end

-- ------------------------------------------------------------ the menu

local Menu = {}
Menu.__index = Menu

local pages = { { "Items", id = "items" }, { "Echoes", id = "echoes" }, { "Status", id = "status" },
  { "Save", id = "save" }, { "Title", id = "title" } }

local function item_rows()
  local rows = {}
  for _, item in ipairs(data.list("items")) do
    local count = state.item_count(item.id)
    if count > 0 then rows[#rows + 1] = { item.name, right = "x" .. count, id = item.id, about = item.about } end
  end
  return rows
end

local function echo_rows(hero_id)
  local rows = {}
  for _, echo in ipairs(data.list("echoes")) do
    if state.data.echoes[echo.id] then
      local wearer
      for _, id in ipairs(state.data.party) do
        if state.data.heroes[id].echo == echo.id then wearer = data.get("heroes", id).name end
      end
      rows[#rows + 1] = { echo.name .. "  R" .. state.echo_rank(echo.id), right = wearer or "",
        id = echo.id, about = echo.about, worn = state.data.heroes[hero_id].echo == echo.id }
    end
  end
  return rows
end

function Menu:update()
  if self.level == "pages" then
    ui.list_input(self.page, #pages)
    if fe.input.pressed("cancel") or fe.input.pressed("menu") then
      audio.sfx("cancel", { volume = 0.5 })
      scenes.pop(self)
    elseif fe.input.pressed("confirm") then
      audio.sfx("confirm", { volume = 0.6 })
      local id = pages[self.page.index].id
      if id == "save" then
        menu.save_screen()
      elseif id == "title" then
        scenes.pop(self)
        require("game.title").open()
      else
        self.level, self.sub, self.hero = id, ui.cursor(), ui.cursor()
        self.picking = id ~= "items" -- echoes and status start by choosing a hero
      end
    end
    return
  end
  if fe.input.pressed("cancel") then
    audio.sfx("cancel", { volume = 0.5 })
    if self.level == "items" and self.picking then
      self.picking = false
    elseif self.level == "echoes" and not self.picking then
      self.picking = true
    else
      self.level = "pages"
    end
    return
  end
  if self.level == "items" then
    local rows = item_rows()
    if self.picking then
      ui.list_input(self.hero, #state.data.party)
      if fe.input.pressed("confirm") then
        local item = data.get("items", self.item)
        if use_item(item, state.data.party[self.hero.index]) then
          state.add_item(item.id, -1)
          audio.sfx("heal")
          if state.item_count(item.id) == 0 then self.picking = false end
        else
          audio.sfx("buzzer")
        end
      end
    else
      ui.list_input(self.sub, #rows)
      if fe.input.pressed("confirm") and rows[self.sub.index] then
        local item = data.get("items", rows[self.sub.index].id)
        if item.effect.kind == "tent" then
          if use_item(item) then
            state.add_item(item.id, -1)
            audio.sfx("heal")
            ui.toast("The party rests.")
          end
        else
          self.item, self.picking = item.id, true
        end
      end
    end
  elseif self.level == "echoes" then
    if self.picking then
      ui.list_input(self.hero, #state.data.party)
      if fe.input.pressed("confirm") then self.picking, self.sub = false, ui.cursor() end
    else
      local hero_id = state.data.party[self.hero.index]
      local rows = echo_rows(hero_id)
      ui.list_input(self.sub, #rows)
      if fe.input.pressed("confirm") and rows[self.sub.index] then
        -- Wearing an echo another hero wears swaps the two.
        local wanted, mine = rows[self.sub.index].id, state.data.heroes[hero_id].echo
        for _, id in ipairs(state.data.party) do
          if state.data.heroes[id].echo == wanted then state.data.heroes[id].echo = mine end
        end
        state.data.heroes[hero_id].echo = wanted
        audio.sfx("equip")
      end
    end
  elseif self.level == "status" then
    ui.list_input(self.hero, #state.data.party)
  end
end

local function draw_status(x, y, hero_id)
  local hero, record, stats = data.get("heroes", hero_id), state.data.heroes[hero_id], state.stats(hero_id)
  ui.window(x, y, 250, 150)
  if hero.portrait then fe.canvas.image(hero.portrait, x + 8, y + 8, { w = 40, h = 40 }) end
  ui.text(hero.name .. "   Lv " .. record.level, x + 56, y + 8, { color = ui.colors.accent })
  ui.text("EXP " .. record.exp .. " / " .. state.exp_to_next(record.level), x + 56, y + 20, { color = ui.colors.dim })
  ui.text("HP " .. record.hp .. "/" .. stats.hp .. "   MP " .. record.mp .. "/" .. stats.mp, x + 56, y + 32)
  local line = y + 54
  for _, name in ipairs({ "atk", "def", "mag", "res", "spd" }) do
    ui.text(name:upper(), x + 10, line, { color = ui.colors.dim })
    ui.text(tostring(stats[name]), x + 40, line)
    line = line + 12
  end
  local echo = record.echo and data.get("echoes", record.echo)
  ui.text("Echo", x + 90, y + 54, { color = ui.colors.dim })
  ui.text(echo and (echo.name .. "  R" .. state.echo_rank(echo.id)) or "none", x + 124, y + 54)
  local names = {}
  for _, id in ipairs(hero.skills) do names[#names + 1] = data.get("skills", id).name end
  for _, entry in ipairs(echo and echo.skills or {}) do
    if state.echo_rank(echo.id) >= entry.rank then names[#names + 1] = data.get("skills", entry.id).name end
  end
  ui.text(table.concat(names, ", "), x + 90, y + 68, { width = 152, color = ui.colors.text })
  ui.text("Surge: " .. data.get("skills", hero.surge).name, x + 90, y + 122, { color = ui.colors.weak })
end

function Menu:draw()
  local w, h = ui.size()
  fe.canvas.rect(0, 0, w, h, 0x0a0e1ac0)
  ui.list(8, 8, 84, pages, self.page)
  ui.window(8, h - 46, 84, 30)
  ui.text("Gil " .. state.data.gold, 16, h - 40)
  ui.text(string.format("%d:%02d", state.data.time // 3600, state.data.time // 60 % 60), 16, h - 28, { color = ui.colors.dim })
  local x, y = 100, 8
  if self.level == "items" then
    local rows = item_rows()
    if #rows == 0 then
      ui.window(x, y, 160, 24)
      ui.text("The bag is empty.", x + 8, y + 7, { color = ui.colors.dim })
    else
      ui.list(x, y, 160, rows, self.sub, 12)
      ui.text(rows[math.min(#rows, self.sub.index)].about or "", x + 4, h - 30, { color = ui.colors.dim })
    end
    if self.picking then ui.list(x + 166, y, 150, hero_rows(), self.hero) end
  elseif self.level == "echoes" then
    ui.list(x, y, 130, hero_rows(), self.hero)
    if not self.picking then
      local rows = echo_rows(state.data.party[self.hero.index])
      ui.list(x + 136, y, 170, rows, self.sub, 10)
      ui.text(rows[math.min(#rows, self.sub.index)].about or "", x + 4, h - 30, { width = 360, color = ui.colors.dim })
    end
  elseif self.level == "status" then
    ui.list(x, y, 110, hero_rows(), self.hero)
    draw_status(x + 116, y, state.data.party[self.hero.index])
  end
  ui.hints({ { "confirm", "Choose" }, { "cancel", "Back" } })
end

function menu.open()
  audio.sfx("confirm", { volume = 0.5 })
  return scenes.push(setmetatable({ level = "pages", page = ui.cursor() }, Menu))
end

-- ------------------------------------------------------------ save and load

local Slots = {}
Slots.__index = Slots

local function slot_rows()
  local saved = {}
  for _, entry in ipairs(fe.save.list()) do saved[entry.slot] = entry.meta end
  local rows = {}
  for i, slot in ipairs(menu.slots) do
    local meta = saved[slot]
    if meta and not meta.corrupt then
      rows[i] = { "File " .. i .. "   Lv " .. (meta.level or "?") .. "   " .. (meta.map or ""),
        right = string.format("%d:%02d", (meta.time or 0) // 3600, (meta.time or 0) // 60 % 60), slot = slot }
    else
      rows[i] = { "File " .. i .. "   " .. (meta and "unreadable" or "empty"), slot = slot, disabled = true, empty = true }
    end
  end
  return rows
end

function Slots:update()
  ui.list_input(self.cursor, #self.rows)
  if fe.input.pressed("cancel") then
    self.closed = true
    scenes.pop(self)
  elseif fe.input.pressed("confirm") then
    local row = self.rows[self.cursor.index]
    if self.mode == "save" then
      local field = require("game.field")
      local p = field.player.position
      state.data.position = { x = p.x, z = p.z }
      state.save(row.slot)
      audio.sfx("save")
      ui.toast("Saved.")
      self.rows = slot_rows()
    elseif row.empty then
      audio.sfx("buzzer")
    else
      self.chosen, self.closed = row.slot, true
      scenes.pop(self)
    end
  end
end

function Slots:draw()
  local w, h = ui.size()
  fe.canvas.rect(0, 0, w, h, 0x0a0e1ac0)
  ui.window(w / 2 - 130, 30, 260, 18)
  ui.text(self.mode == "save" and "Save to which file?" or "Load which file?", w / 2 - 120, 35, { color = ui.colors.accent })
  ui.list(w / 2 - 130, 52, 260, self.rows, self.cursor)
  ui.hints({ { "confirm", self.mode == "save" and "Save" or "Load" }, { "cancel", "Back" } })
end

local function slots(mode)
  local screen = scenes.push(setmetatable({ mode = mode, rows = slot_rows(), cursor = ui.cursor() }, Slots))
  if coroutine.isyieldable() then task.wait_until(function() return screen.closed end) end
  return screen
end

--- The save screen. Called from a task (a save point) it waits until closed.
function menu.save_screen()
  return slots("save")
end

--- The load screen; `on_load(slot)` runs when a file is chosen.
function menu.load_screen(on_load)
  local screen = slots("load")
  task.spawn(function()
    task.wait_until(function() return screen.closed end)
    if screen.chosen then on_load(screen.chosen) end
  end, "load screen")
end

function menu.has_saves()
  for _, row in ipairs(slot_rows()) do
    if not row.empty then return true end
  end
  return false
end

return menu
