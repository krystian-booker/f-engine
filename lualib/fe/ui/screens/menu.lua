--- The field main menu. Pages are entries in `menu.pages`; remove, reorder
--- or add your own ({id, label, run = function() ... end}, run in a task).
---
--- ```lua
--- table.insert(menu.pages, 3, {id = "crafting", label = "Craft", run = require("ui.crafting").open})
--- menu.allow_save = function() return false end   -- save only at save points
--- ```
local data = require("fe.data")
local effects = require("fe.effects")
local inventory = require("fe.rpg.inventory")
local party = require("fe.rpg.party")
local scene = require("fe.scene")
local stats = require("fe.rpg.stats")
local tablex = require("fe.tablex")
local task = require("fe.task")
local ui = require("fe.ui")

local menu = {
  allow_save = function() return true end,
}

local life = function() return require("fe.rpg.combatant").life end

local function member_panel(actor, x, y, w, selected)
  ui.text(actor:name(), x, y, { color = selected and ui.theme.highlight or ui.theme.heading })
  ui.text("Lv " .. actor:level(), x, y, { width = w - 6, align = "right", color = ui.theme.muted })
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
  local entries = tablex.map(party.members(), function(actor)
    return { label = actor:name(), value = actor, enabled = not filter or filter(actor) }
  end)
  local _, item = ui.choose(entries, { title = title, anchor = "right", width = 140, rows = #entries, cancel = true,
    render = nil })
  return item and item.value
end

local function use_on_members(source, user, pay)
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

local function items_page()
  while true do
    local entries = tablex.map(inventory.list(function(item) return item.kind ~= "equipment" end), function(entry)
      local usable = entry.item.use and entry.item.use.scope ~= "battle"
      return { label = entry.item.name, right = "x" .. entry.count, value = entry.item, enabled = usable == true,
        icon = entry.item.icon }
    end)
    if #entries == 0 then
      entries = { { label = "(No items)", enabled = false } }
    end
    local _, item = ui.choose(entries, { title = "Items", anchor = "left", width = 200, rows = 10, cancel = true })
    if not item then return end
    local it = item.value
    use_on_members(it, nil, function()
      if it.use.consume ~= false then inventory.remove(it.id, 1) end
    end)
  end
end

local function skills_page()
  local user = menu.choose_member("Who?")
  if not user then return end
  while true do
    local entries = tablex.map(user:skills(), function(skill)
      local cost = {}
      for r, n in pairs(skill.cost) do cost[#cost + 1] = n .. " " .. r:upper() end
      local enabled = skill.scope ~= "battle" and user:alive()
      for r, n in pairs(skill.cost) do if user:resource(r) < n then enabled = false end end
      return { label = skill.name, right = table.concat(cost, " "), value = skill, enabled = enabled }
    end)
    if #entries == 0 then entries = { { label = "(No skills)", enabled = false } } end
    local _, item = ui.choose(entries, { title = user:name() .. "'s skills", anchor = "left", width = 220, rows = 10,
      cancel = true })
    if not item then return end
    local skill = item.value
    use_on_members(skill, user, function()
      for r, n in pairs(skill.cost) do user:change_resource(r, -n) end
    end)
  end
end

local function stat_lines(actor, preview)
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

local function equip_page()
  local actor = menu.choose_member("Equip")
  if not actor then return end
  while true do
    local slots = tablex.map(actor:slots(), function(slot)
      local id = actor:equipped(slot)
      return { label = slot:gsub("^%l", string.upper), right = id and data.get("items", id).name or "-", value = slot }
    end)
    local _, chosen = ui.choose(slots, { title = actor:name(), anchor = "left", width = 200, rows = #slots, cancel = true })
    if not chosen then return end
    local slot = chosen.value
    local options = { { label = "(Remove)", value = false } }
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
    scene.push(preview_view)
    local _, item = ui.choose(options, { title = "Equip " .. slot, anchor = "left", width = 200, rows = 8, cancel = true,
      on_change = function(entry) hovered = entry end })
    scene.remove(preview_view)
    if item then
      actor:equip(slot, item.value or nil)
      ui.sound("equip")
    end
  end
end

local function status_page()
  local actor = menu.choose_member("Status")
  if not actor then return end
  local done = false
  local view = { name = "status", blocks_update = true }
  function view:update()
    if fe.input.pressed("cancel") or fe.input.pressed("confirm") then done = true end
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
    ui.text("Level " .. actor:level() .. "   " .. cls.name .. (secondary and (" / " .. secondary.name) or ""), 72, 30)
    ui.text("EXP " .. actor:exp() .. "   Next " .. actor:exp_to_next() .. "   JP " .. actor:jp(), 72, 42, { color = ui.theme.muted })
    member_panel(actor, 16, 70, 120, false)
    ui.text(stat_lines(actor), 150, 70)
    local lines = {}
    for _, slot in ipairs(actor:slots()) do
      local id = actor:equipped(slot)
      lines[#lines + 1] = slot .. ": " .. (id and data.get("items", id).name or "-")
    end
    for _, skill in ipairs(actor:passives()) do lines[#lines + 1] = "{color:good}" .. skill.name .. "{color:reset}" end
    for _, id in ipairs(actor:states()) do lines[#lines + 1] = "{color:bad}" .. data.get("states", id).name .. "{color:reset}" end
    ui.text(table.concat(lines, "\n"), 270, 70, { width = cw - 290 })
    ui.text(ui.prompts({ { "page_left", "Prev" }, { "page_right", "Next" }, { "cancel", "Back" } }), 16, ch - 24,
      { color = ui.theme.muted })
  end
  scene.push(view)
  task.wait_until(function() return done end)
  scene.remove(view)
end

local function formation_page()
  while true do
    local entries = {}
    for i, actor in ipairs(party.members()) do
      entries[#entries + 1] = { label = actor:name(), right = "Lv " .. actor:level(), value = { kind = "active", index = i } }
    end
    for _, actor in ipairs(party.reserve()) do
      entries[#entries + 1] = { label = actor:name() .. " (reserve)", right = "Lv " .. actor:level(),
        value = { kind = "reserve", id = actor.id } }
    end
    local _, first = ui.choose(entries, { title = "Formation: pick a member", anchor = "left", width = 200, rows = 10, cancel = true })
    if not first then return end
    local _, second = ui.choose(entries, { title = "Swap with", anchor = "left", width = 200, rows = 10, cancel = true })
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

local function quests_page()
  local quest = require("fe.quest")
  local entries = tablex.map(quest.list("active"), function(q)
    local stage = quest.stage(q.def.id)
    return { label = q.def.name, right = q.def.kind, value = q, detail = stage and stage.text or q.def.description }
  end)
  if #entries == 0 then entries = { { label = "(No active quests)", enabled = false } } end
  local hovered = entries[1]
  local view = { name = "quest_detail", blocks_update = false }
  function view:draw()
    if not hovered or not hovered.detail then return end
    local cw, ch = fe.canvas.size()
    ui.window(cw - 170, 30, 162, ch - 60)
    ui.text(hovered.detail, cw - 162, 38, { width = 146 })
  end
  scene.push(view)
  ui.choose(entries, { title = "Journal", anchor = "left", width = 190, rows = 10, cancel = true,
    on_change = function(item) hovered = item end })
  scene.remove(view)
end

menu.pages = {
  { id = "items", label = "Items", run = items_page },
  { id = "skills", label = "Skills", run = skills_page },
  { id = "equip", label = "Equip", run = equip_page },
  { id = "status", label = "Status", run = status_page },
  { id = "formation", label = "Formation", run = formation_page },
  { id = "quests", label = "Journal", run = quests_page },
  { id = "config", label = "Settings", run = function() require("fe.ui.screens.config").open() end },
  { id = "save", label = "Save", run = function() require("fe.ui.screens.save").open("save") end,
    enabled = function() return menu.allow_save() end },
}

--- Open the menu (call in a task); returns when closed.
function menu.open()
  local done, running = false, false
  local entries = tablex.map(menu.pages, function(page)
    return { label = page.label, value = page, enabled = not page.enabled or page.enabled() }
  end)
  local list = ui.List({ items = entries, rows = #entries,
    on_confirm = function(item)
      running = true
      task.named("menu:" .. item.value.id, function()
        item.value.run()
        running = false
      end)
    end,
    on_cancel = function() done = true end })
  local view = { name = "menu", opaque = false, blocks_update = true }
  function view:update()
    if not running then
      if fe.input.pressed("menu") then done = true end
      list:update()
    end
  end
  function view:draw()
    local cw, ch = fe.canvas.size()
    fe.canvas.rect(0, 0, cw, ch, 0x050a1888)
    ui.window(8, 8, 90, #entries * ui.theme.row_height + 12)
    list.active = not running
    list:draw(26, 14, 66)
    local members = party.members()
    local panel_w = math.min(cw - 112, 300)
    ui.window(104, 8, panel_w, #members * 40 + 10)
    for i, actor in ipairs(members) do
      member_panel(actor, 112, 14 + (i - 1) * 40, panel_w - 20, false)
    end
    ui.window(8, ch - 26, 150, 18)
    ui.text(inventory.money() .. " " .. inventory.settings.currency, 14, ch - 21)
    local playtime = math.floor(require("fe.state").var("playtime", 0))
    ui.text(string.format("%d:%02d", playtime // 3600, (playtime // 60) % 60), 14, ch - 21,
      { width = 138, align = "right", color = ui.theme.muted })
  end
  scene.push(view)
  task.wait_until(function() return done end)
  scene.remove(view)
end

return menu
