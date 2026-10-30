--- Player input for side-view battles: the command menu (a ui.Menu shown
--- with ui.run_screen, with the modifier gauge on page left/right), skill
--- and item lists, target picking and the Swap command. `fe.battle.sideview`
--- wraps `commands.command` as `Presenter:command(user, battle)`; every
--- function takes the sideview module so its settings, custom commands
--- and modifier are read live. The menu's task is cancelled by the ATB
--- scheduler when the chooser is knocked out, which closes the menu.
local Battle = require("fe.battle.core")
local i18n = require("fe.i18n")
local tablex = require("fe.tablex")
local task = require("fe.task")
local ui = require("fe.ui")

local commands = {}

local function choose_targets(presenter, battle, user, kind)
  local candidates = battle:targets_for(user, kind)
  if #candidates == 0 then return nil end
  if kind:find("^all") or kind == "everyone" or kind == "self" then
    presenter.targeting = candidates
    task.frame()
    while true do
      if fe.input.pressed("confirm") then presenter.targeting = nil return candidates end
      if fe.input.pressed("cancel") then presenter.targeting = nil return nil end
      task.frame()
    end
  end
  local index = 1
  -- Start on the side the skill is meant for.
  presenter.targeting = { candidates[index] }
  task.frame()
  while true do
    local step = (fe.input.repeating("down") or fe.input.repeating("right")) and 1
      or (fe.input.repeating("up") or fe.input.repeating("left")) and -1 or 0
    if step ~= 0 then
      index = (index - 1 + step) % #candidates + 1
      presenter.targeting = { candidates[index] }
      ui.sound("move")
    end
    if fe.input.pressed("confirm") then
      ui.sound("confirm")
      presenter.targeting = nil
      return { candidates[index] }
    end
    if fe.input.pressed("cancel") then
      ui.sound("cancel")
      presenter.targeting = nil
      return nil
    end
    task.frame()
  end
end

local function pick_from(title, entries)
  local cw, ch = fe.canvas.size()
  local index, item = ui.choose(entries, { title = title, x = 8, y = ch - 112, width = math.floor(cw * 0.55),
    rows = 6, columns = 2, cancel = true })
  return index and item
end

local function skill_entries(battle, user, skill_type)
  return tablex.map(user:skills(skill_type), function(skill)
    local cost = {}
    for resource, amount in pairs(skill.cost) do cost[#cost + 1] = amount .. " " .. resource:upper() end
    table.sort(cost)
    return { label = skill.name, right = table.concat(cost, " "), value = skill,
      enabled = battle:usable(user, skill) and skill.scope ~= "field" }
  end)
end

local function item_entries(battle)
  local inventory = battle:inventory()
  if not inventory or not inventory.list then return {} end
  return tablex.map(inventory.list(function(item) return item.use and item.use.scope ~= "field" end), function(entry)
    return { label = entry.item.name, right = "x" .. entry.count, value = entry.item, icon = entry.item.icon }
  end)
end

-- The command menu: a ui.Menu shown with ui.run_screen. While it is open
-- page left/right change the modifier (boost).
local function choose_command(sideview, presenter, user, battle)
  local entries = tablex.map(user:commands(), function(name)
    return { label = sideview.command_label(name, user), value = name, enabled = name ~= "flee" or battle.escapable }
  end)
  local partner = battle:partner(user)
  if partner then
    entries[#entries + 1] = { label = i18n.t("fe.battle.swap_with", { name = partner:name() }), value = "swap" }
  end
  local _, ch = fe.canvas.size()
  local menu
  menu = ui.Menu({ items = entries, x = 8, y = ch - 98, width = 92, rows = 6,
    on_confirm = function(item) ui.close(menu, item.value) end })
  function menu.update(view, dt)
    ui.Menu.update(view, dt)
    if sideview.modifier then
      local max = sideview.modifier.max(user)
      if fe.input.pressed("page_right") then presenter.modifier_value = math.min(max, presenter.modifier_value + 1) end
      if fe.input.pressed("page_left") then presenter.modifier_value = math.max(0, presenter.modifier_value - 1) end
    end
  end
  return ui.run_screen(menu)
end

--- Command input for a party battler; returns an action or nil. Choosing
--- Swap trades places with the back-row partner, who then chooses.
function commands.command(sideview, presenter, user, battle)
  presenter.active = user
  presenter.modifier_value = 0
  -- The battle can end (or the chooser fall) while this player is choosing.
  local forget = task.current():finally(function()
    presenter.active, presenter.targeting = nil, nil
  end)
  while true do
    local choice = choose_command(sideview, presenter, user, battle)
    local action
    if choice == "swap" then
      local partner = battle:partner(user)
      forget()
      battle:swap(user, partner)
      return commands.command(sideview, presenter, partner, battle)
    elseif choice == "attack" then
      local targets = choose_targets(presenter, battle, user, "enemy")
      if targets then action = { kind = "skill", user = user, skill = Battle.settings.attack_skill, targets = targets } end
    elseif choice == "defend" then
      action = { kind = "defend", user = user }
    elseif choice == "flee" then
      action = { kind = "flee", user = user }
    elseif choice == "item" then
      local item = pick_from(sideview.command_label("item", user), item_entries(battle))
      if item then
        local targets = choose_targets(presenter, battle, user, item.value.use.target)
        if targets then action = { kind = "item", user = user, item = item.value.id, targets = targets } end
      end
    elseif sideview.commands[choice] then
      action = sideview.commands[choice].run(presenter, user, battle)
    else
      local skill_type = choice ~= "skill" and choice or nil
      local picked = pick_from(sideview.command_label(choice, user), skill_entries(battle, user, skill_type))
      if picked then
        local targets = choose_targets(presenter, battle, user, picked.value.target)
        if targets then action = { kind = "skill", user = user, skill = picked.value.id, targets = targets } end
      end
    end
    if action then
      if sideview.modifier and presenter.modifier_value > 0 then
        sideview.modifier.apply(action, presenter.modifier_value)
      end
      presenter.active = nil
      forget()
      return action
    end
  end
end

return commands
