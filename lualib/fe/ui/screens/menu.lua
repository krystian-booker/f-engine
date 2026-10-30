--- The field main menu (needs the RPG kit). Pages are entries in
--- `menu.pages`, a list identified by `id`; remove, reorder or add your own
--- ({id, label, run, enabled?, closes?}). `label` is an i18n key or plain
--- text. A page's `run` is a function, or the name of a module function
--- (`run = "items_page"`), looked up on this module when the page is
--- chosen, so replacing `menu.items_page` changes the Items page; the
--- built-in pages are `menu.items_page`, `skills_page`, `equip_page`,
--- `status_page`, `formation_page`, `quests_page`, `log_page`,
--- `config_page` and `save_page`. It is called in the menu's task; if it
--- raises, the error is reported and the menu returns. While the menu is
--- open the field world is paused (`fe.game.pause`), so sprites, particles
--- and field scripts stand still.
---
--- A page that starts gameplay (the world map's travel, a chapter select
--- that warps) must not run under that pause: give it `closes = true`, and
--- the menu closes first and runs the page in a detached task once the
--- pause is released; or call `menu.continue_with(fn)` from inside a page
--- for the same effect.
---
--- ```lua
--- table.insert(menu.pages, 3, {id = "crafting", label = "Craft", run = require("ui.crafting").open})
--- menu.pages[#menu.pages + 1] = {id = "travel", label = "Travel", run = worldmap.open, closes = true}
--- menu.allow_save = function() return false end   -- save only at save points
--- menu.formation_page()                            -- open one page directly (in a task)
--- menu.equip_page = function() ... end             -- replace a built-in page body
--- ```
--- Hot reload: the file's pages are merged into `menu.pages` by id (a
--- game's own pages keep their place and functions) and `menu.dim`, the
--- colour drawn over the field, is adopted unless the game changed it.
---
--- The RPG kit (`fe.rpg.*`) and `fe.effects` are required inside the pages
--- that use them, so this file (and `fe.ui`) loads without the kit.
local data = require("fe.data")
local game = require("fe.game")
local i18n = require("fe.i18n")
local tablex = require("fe.tablex")
local task = require("fe.task")
local ui = require("fe.ui")

local menu = {
  allow_save = function() return true end,
  --- Colour drawn over the field while the menu is open.
  dim = 0x050a1888,
}

--- The file's values, for `__reload`.
menu.file_defaults = { dim = menu.dim }

i18n.defaults({ fe = { menu = {
  items = "Items", skills = "Skills", equip = "Equip", status = "Status", formation = "Formation", journal = "Journal",
  settings = "Settings", save = "Save", log = "Log",
  no_items = "(No items)", no_skills = "(No skills)", no_quests = "(No active quests)", who = "Who?",
  skills_of = "{name}'s skills", remove = "(Remove)", equip_slot = "Equip {slot}", level = "Lv {level}",
  level_line = "Level {level}   {class}", exp_line = "EXP {exp}   Next {next}   JP {jp}",
  previous = "Prev", next = "Next", back = "Back", pick_member = "Formation: pick a member", swap_with = "Swap with",
  reserve = "{name} (reserve)",
}, slot = { weapon = "Weapon", shield = "Shield", head = "Head", body = "Body", armor = "Armor",
  accessory = "Accessory" } } })

local t = ui.t

--- An equipment slot's name: the `fe.slot.<id>` string, else the id
--- capitalised.
function menu.slot_name(slot)
  local key = "fe.slot." .. slot
  local name = t(key)
  return name == key and (slot:gsub("^%l", string.upper)) or name
end

local function member_panel(actor, x, y, w, selected)
  local stats = require("fe.rpg.stats")
  ui.text(actor:name(), x, y, { color = selected and ui.theme.highlight or ui.theme.heading })
  ui.text(t("fe.menu.level", { level = actor:level() }), x, y, { width = w - 6, align = "right", color = ui.theme.muted })
  local row = y + 12
  for _, def in ipairs(stats.resource_list) do
    if not def.battle_only then
      local max = actor:max_resource(def.id)
      if max > 0 then
        ui.text(def.name .. " " .. actor:resource(def.id) .. "/" .. max, x, row)
        ui.gauge(x, row + 9, w - 10, 3, actor:resource(def.id), max, ui.theme.gauges[def.id])
        row = row + 13
      end
    end
  end
end

--- Pick a party member; returns the Actor or nil.
function menu.choose_member(title, filter)
  local party = require("fe.rpg.party")
  local entries = tablex.map(party.members(), function(actor)
    return { label = actor:name(), value = actor, enabled = not filter or filter(actor) }
  end)
  local _, item = ui.choose(entries, { title = title, anchor = "right", width = 140, rows = #entries, cancel = true })
  return item and item.value
end

local function use_on_members(source, user, pay)
  local effects = require("fe.effects")
  local party = require("fe.rpg.party")
  local target_kind = source.target or (source.use and source.use.target) or "ally"
  local targets
  if target_kind == "all_allies" then
    targets = party.members()
  elseif target_kind == "self" then
    targets = { user }
  else
    local member = menu.choose_member(source.name, function(a) return effects.useful(source, a) end)
    if not member then return false end
    targets = { member }
  end
  if not tablex.find(targets, function(t) return effects.useful(source, t) end) then
    ui.sound("buzzer")
    return false
  end
  if pay then pay() end
  local spec = { user = user or targets[1], targets = targets, source = source.use and {
    id = source.id, name = source.name, effects = source.use.effects, target = source.use.target, always_hit = true,
  } or source, random = fe.random.new() }
  effects.resolve(spec)
  ui.sound("heal")
  return true
end

function menu.items_page()
  local inventory = require("fe.rpg.inventory")
  while true do
    local entries = tablex.map(inventory.list(function(item) return item.kind ~= "equipment" end), function(entry)
      local usable = entry.item.use and entry.item.use.scope ~= "battle"
      return { label = entry.item.name, right = "x" .. entry.count, value = entry.item, enabled = usable == true,
        icon = entry.item.icon }
    end)
    if #entries == 0 then
      entries = { { label = t("fe.menu.no_items"), enabled = false } }
    end
    local _, item = ui.choose(entries, { title = t("fe.menu.items"), anchor = "left", width = 200, rows = 10, cancel = true })
    if not item then return end
    local it = item.value
    use_on_members(it, nil, function()
      if it.use.consume ~= false then inventory.remove(it.id, 1) end
    end)
  end
end

function menu.skills_page()
  local user = menu.choose_member(t("fe.menu.who"))
  if not user then return end
  while true do
    local entries = tablex.map(user:skills(), function(skill)
      local cost = {}
      for r, n in pairs(skill.cost) do cost[#cost + 1] = n .. " " .. r:upper() end
      local enabled = skill.scope ~= "battle" and user:alive()
      for r, n in pairs(skill.cost) do if user:resource(r) < n then enabled = false end end
      return { label = skill.name, right = table.concat(cost, " "), value = skill, enabled = enabled }
    end)
    if #entries == 0 then entries = { { label = t("fe.menu.no_skills"), enabled = false } } end
    local _, item = ui.choose(entries, { title = t("fe.menu.skills_of", { name = user:name() }), anchor = "left", width = 220,
      rows = 10, cancel = true })
    if not item then return end
    local skill = item.value
    use_on_members(skill, user, function()
      for r, n in pairs(skill.cost) do user:change_resource(r, -n) end
    end)
  end
end

local function stat_lines(actor, preview)
  local stats = require("fe.rpg.stats")
  local now = actor:stats()
  local lines = {}
  for _, def in ipairs(stats.list) do
    local value = now[def.id]
    local line = string.format("%-8s %4d", def.short or def.name, value)
    if preview and preview[def.id] ~= value then
      local better = preview[def.id] > value
      line = line .. string.format(" {color:%s}→ %d{color:reset}", better and "good" or "bad", preview[def.id])
    end
    lines[#lines + 1] = line
  end
  return table.concat(lines, "\n")
end

function menu.equip_page()
  local inventory = require("fe.rpg.inventory")
  local actor = menu.choose_member(t("fe.menu.equip"))
  if not actor then return end
  while true do
    local slots = tablex.map(actor:slots(), function(slot)
      local id = actor:equipped(slot)
      return { label = menu.slot_name(slot), right = id and data.get("items", id).name or "-", value = slot }
    end)
    local _, chosen = ui.choose(slots, { title = actor:name(), anchor = "left", width = 200, rows = #slots, cancel = true })
    if not chosen then return end
    local slot = chosen.value
    local options = { { label = t("fe.menu.remove"), value = false } }
    for _, entry in ipairs(inventory.list("equipment")) do
      if actor:can_equip(entry.item.id, slot) then
        options[#options + 1] = { label = entry.item.name, right = "x" .. entry.count, value = entry.item.id }
      end
    end
    local preview_view = { name = "equip_preview", blocks_update = false }
    local hovered = options[1]
    function preview_view:draw()
      local cw = fe.canvas.size()
      local text = stat_lines(actor, hovered and actor:preview_equip(slot, hovered.value or nil))
      ui.window(cw - 150, 30, 142, 150)
      ui.text(text, cw - 142, 38)
    end
    local _, item = ui.with_view(preview_view, ui.choose, options, { title = t("fe.menu.equip_slot", { slot = menu.slot_name(slot) }),
      anchor = "left", width = 200, rows = 8, cancel = true, on_change = function(entry) hovered = entry end })
    if item then
      actor:equip(slot, item.value or nil)
      ui.sound("equip")
    end
  end
end

function menu.status_page()
  local party = require("fe.rpg.party")
  local actor = menu.choose_member(t("fe.menu.status"))
  if not actor then return end
  local view = { name = "status", blocks_update = true }
  function view:update()
    if fe.input.pressed("cancel") or fe.input.pressed("confirm") then ui.close(view) end
    local members = party.members()
    local index = tablex.index_of(tablex.map(members, function(a) return a.id end), actor.id) or 1
    if fe.input.pressed("page_right") then actor = members[index % #members + 1] end
    if fe.input.pressed("page_left") then actor = members[(index - 2) % #members + 1] end
  end
  function view:draw()
    local cw, ch = fe.canvas.size()
    ui.window(8, 8, cw - 16, ch - 16)
    if actor:portrait() then fe.canvas.image(actor:portrait(), 16, 16, { w = 48, h = 48 }) end
    ui.text(actor:name(), 72, 18, { color = ui.theme.heading })
    local cls = actor:class()
    local secondary = actor:secondary()
    ui.text(t("fe.menu.level_line", { level = actor:level(), class = cls.name .. (secondary and (" / " .. secondary.name) or "") }),
      72, 30)
    ui.text(t("fe.menu.exp_line", { exp = actor:exp(), next = actor:exp_to_next(), jp = actor:jp() }), 72, 42,
      { color = ui.theme.muted })
    member_panel(actor, 16, 70, 120, false)
    ui.text(stat_lines(actor), 150, 70)
    local lines = {}
    for _, slot in ipairs(actor:slots()) do
      local id = actor:equipped(slot)
      lines[#lines + 1] = menu.slot_name(slot) .. ": " .. (id and data.get("items", id).name or "-")
    end
    for _, skill in ipairs(actor:passives()) do lines[#lines + 1] = "{color:good}" .. skill.name .. "{color:reset}" end
    for _, id in ipairs(actor:states()) do lines[#lines + 1] = "{color:bad}" .. data.get("states", id).name .. "{color:reset}" end
    ui.text(table.concat(lines, "\n"), 270, 70, { width = cw - 290 })
    ui.text(ui.prompts({ { "page_left", t("fe.menu.previous") }, { "page_right", t("fe.menu.next") },
      { "cancel", t("fe.menu.back") } }), 16, ch - 24, { color = ui.theme.muted })
  end
  ui.run_screen(view)
end

function menu.formation_page()
  local party = require("fe.rpg.party")
  while true do
    local entries = {}
    for i, actor in ipairs(party.members()) do
      entries[#entries + 1] = { label = actor:name(), right = t("fe.menu.level", { level = actor:level() }),
        value = { kind = "active", index = i } }
    end
    for _, actor in ipairs(party.reserve()) do
      entries[#entries + 1] = { label = t("fe.menu.reserve", { name = actor:name() }),
        right = t("fe.menu.level", { level = actor:level() }), value = { kind = "reserve", id = actor.id } }
    end
    local _, first = ui.choose(entries, { title = t("fe.menu.pick_member"), anchor = "left", width = 200, rows = 10,
      cancel = true })
    if not first then return end
    local _, second = ui.choose(entries, { title = t("fe.menu.swap_with"), anchor = "left", width = 200, rows = 10,
      cancel = true })
    if second then
      local ids = tablex.map(party.members(), function(a) return a.id end)
      local a, b = first.value, second.value
      if a.kind == "active" and b.kind == "active" then
        party.swap(a.index, b.index)
      elseif a.kind ~= b.kind then
        local active = a.kind == "active" and a or b
        local reserve = a.kind == "reserve" and a or b
        ids[active.index] = reserve.id
        party.set_active(ids)
      end
    end
  end
end

function menu.quests_page()
  local quest = require("fe.quest")
  local entries = tablex.map(quest.list("active"), function(q)
    local stage = quest.stage(q.def.id)
    local detail = stage and stage.text or q.def.description
    for _, objective in ipairs(quest.objectives(q.def.id)) do
      local mark = objective.done and "{color:good}" or "{color:muted}"
      local count = objective.count > 1 and string.format(" %d/%d", objective.progress, objective.count) or ""
      detail = detail .. "\n" .. mark .. objective.text .. count .. "{color:reset}"
    end
    return { label = q.def.name, right = q.def.kind, value = q, detail = detail }
  end)
  if #entries == 0 then entries = { { label = t("fe.menu.no_quests"), enabled = false } } end
  local hovered = entries[1]
  local view = { name = "quest_detail", blocks_update = false }
  function view:draw()
    if not hovered or not hovered.detail then return end
    local cw, ch = fe.canvas.size()
    ui.window(cw - 170, 30, 162, ch - 60)
    ui.text(hovered.detail, cw - 162, 38, { width = 146 })
  end
  ui.with_view(view, ui.choose, entries, { title = t("fe.menu.journal"), anchor = "left", width = 190, rows = 10,
    cancel = true, on_change = function(item) hovered = item end })
end

function menu.log_page()
  require("fe.ui.screens.backlog").open()
end

function menu.config_page()
  require("fe.ui.screens.config").open()
end

function menu.save_page()
  require("fe.ui.screens.save").open("save")
end

menu.pages = {
  { id = "items", label = "fe.menu.items", run = "items_page" },
  { id = "skills", label = "fe.menu.skills", run = "skills_page" },
  { id = "equip", label = "fe.menu.equip", run = "equip_page" },
  { id = "status", label = "fe.menu.status", run = "status_page" },
  { id = "formation", label = "fe.menu.formation", run = "formation_page" },
  { id = "quests", label = "fe.menu.journal", run = "quests_page" },
  { id = "log", label = "fe.menu.log", run = "log_page" },
  { id = "config", label = "fe.menu.settings", run = "config_page" },
  { id = "save", label = "fe.menu.save", run = "save_page", enabled = function() return menu.allow_save() end },
}

--- A page by id, or nil.
function menu.page(id)
  return tablex.find(menu.pages, function(page) return page.id == id end)
end

--- The function a page runs: its `run`, or the module function it names
--- (raises for a name this module lacks).
---@param page table
---@return fun()
function menu.page_run(page)
  local run = page.run
  if type(run) == "string" then
    run = menu[run]
    if type(run) ~= "function" then error("menu page '" .. tostring(page.id) .. "': no menu." .. page.run .. " function", 2) end
  end
  return run
end

--- Adopt the file's pages by id after a hot reload: a live page with the
--- same id takes the fresh page's `label`, `run` and `enabled`; fresh
--- pages the live list lacks are added after the page that precedes them
--- in the file; a game's own pages stay where they are.
function menu.__reload(live, fresh)
  live.file_defaults = tablex.adopt(live, { dim = fresh.dim }, live.file_defaults)
  local previous
  for _, page in ipairs(fresh.pages) do
    local existing = live.page(page.id)
    if existing then
      existing.label, existing.run, existing.enabled = page.label, page.run, page.enabled
    else
      local index = previous and (tablex.index_of(live.pages, previous) or #live.pages) + 1 or 1
      table.insert(live.pages, index, page)
      existing = page
    end
    previous = existing
  end
end

--- Close the menu and run `fn` in a detached task once the menu's pause
--- is released (for pages that start gameplay). Outside the menu, runs it
--- in a detached task at once.
---@param fn fun()
function menu.continue_with(fn)
  local view = menu.view
  if view and not view.closed then
    view.continuation = fn
    ui.close(view)
  else
    task.detached("menu:continue", fn)
  end
end

local function page_entries()
  return tablex.map(menu.pages, function(page)
    return { label = t(page.label), value = page, enabled = not page.enabled or page.enabled() }
  end)
end

--- Open the menu (call in a task); returns when the player closes it, or
--- when a page with `closes` is chosen (that page then runs on its own).
function menu.open()
  local inventory, party = require("fe.rpg.inventory"), require("fe.rpg.party")
  local world = require("fe.field").world
  local resume = world and game.pause(world) or function() end
  local view = { name = "menu", opaque = false, blocks_update = true }
  menu.view = view
  local list = ui.List({ items = page_entries(), rows = #menu.pages,
    on_confirm = function(item) view.chosen = item.value end,
    on_cancel = function() ui.close(view) end })
  local function run_pages()
    while true do
      task.wait_until(function() return view.chosen ~= nil end)
      local page = view.chosen
      local ok, run = pcall(menu.page_run, page)
      if ok and page.closes then
        menu.continue_with(run)
        return
      end
      local err
      if ok then ok, err = pcall(run) else err = run end
      view.chosen = nil
      if not ok then fe.log.error("Menu page '" .. page.id .. "' failed: " .. tostring(err)) end
      list:set_items(page_entries())
    end
  end
  function view:update()
    if self.chosen then return end
    if fe.input.pressed("menu") then
      ui.close(self)
    else
      list:update()
    end
  end
  function view:draw()
    local cw, ch = fe.canvas.size()
    fe.canvas.rect(0, 0, cw, ch, menu.dim)
    ui.window(8, 8, 90, #list.items * ui.row_height() + 12)
    list.active = not self.chosen
    list:draw(26, 14, 66)
    local members = party.members()
    local panel_w = math.min(cw - 112, 300)
    ui.window(104, 8, panel_w, #members * 40 + 10)
    for i, actor in ipairs(members) do
      member_panel(actor, 112, 14 + (i - 1) * 40, panel_w - 20, false)
    end
    ui.window(8, ch - 26, 150, 18)
    ui.text(inventory.money() .. " " .. inventory.settings.currency, 14, ch - 21)
    local playtime = math.floor(game.playtime())
    ui.text(string.format("%d:%02d", playtime // 3600, (playtime // 60) % 60), 14, ch - 21,
      { width = 138, align = "right", color = ui.theme.muted })
  end
  task.ensure(function() task.race({ function() ui.run_screen(view) end, run_pages }) end, function()
    resume()
    if menu.view == view then menu.view = nil end
  end)
  if view.continuation then task.detached("menu:continue", view.continuation) end
end

return menu
