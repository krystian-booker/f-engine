--- Deployment: the player picks which roster members fight and where.
---
--- The first `max` living roster members (required units first) start on
--- the deploy cells. Confirm on a unit selects it; Confirm on another cell
--- moves it there (swapping with a unit on that cell); Confirm on the
--- selected unit again opens the roster to replace it or send it back
--- (required units cannot leave). Confirm on an empty cell adds a unit
--- from the roster. The Menu button starts the battle.
local grid = require("fe.battle.tactical.grid")
local i18n = require("fe.i18n")
local party = require("fe.rpg.party")
local config = require("fe.battle.tactical.settings")
local tablex = require("fe.tablex")
local ui = require("fe.ui")

local deploy = {}

i18n.defaults({ fe = { tactics = { deploy = {
  help = "Deploy your units - Confirm to place or swap, Menu to begin", title = "Deploy",
  replace = "Replace {name}", remove = "Remove", level = "Lv {level}",
} } } })

local function deployed_ids(battle)
  local ids = {}
  for _, b in ipairs(battle:side("party", true)) do
    if b.actor then ids[b.actor.id] = true end
  end
  return ids
end

--- Living roster members not on the field.
function deploy.bench(battle)
  local ids = deployed_ids(battle)
  return tablex.filter(party.roster(), function(actor) return actor:alive() and not ids[actor.id] end)
end

--- Put an actor on a deploy cell.
function deploy.add(battle, p, actor, cell)
  local b = battle:add({ actor = actor, side = "party", cell = cell, facing = { x = 0, z = -1 } })
  b.unit_id = actor.id
  grid.place(battle, b, cell)
  if p and p.scene then p:spawn(b) end
  return b
end

--- Send a unit back to the roster.
function deploy.remove(battle, p, b)
  grid.unplace(battle, b)
  if p then p:despawn(b) end
  battle:remove(b)
end

local function choose_actor(battle, title, allow_remove)
  local entries = tablex.map(deploy.bench(battle), function(actor)
    return { label = actor:name(), right = i18n.t("fe.tactics.deploy.level", { level = actor:level() }), value = actor }
  end)
  if allow_remove then entries[#entries + 1] = { label = i18n.t("fe.tactics.deploy.remove"), value = "remove" } end
  if #entries == 0 then return nil end
  local _, item = ui.choose(entries, { title = title, anchor = "bottom-left", width = 140, rows = 6, cancel = true })
  return item and item.value
end

--- Run the deployment phase (shows the board). Returns the deployed units.
---@param options {tag?: string, max?: integer, required?: string[]}
function deploy.run(p, battle, options)
  local world = battle.tactical.world
  local cells = world:cells_with_tag(options.tag or config.settings.deploy_tag)
  local max = math.min(options.max or 6, #cells)
  local required = options.required or {}
  local roster = tablex.filter(party.roster(), function(actor) return actor:alive() end)
  tablex.sort_by(roster, function(actor) return tablex.contains(required, actor.id) and 0 or 1 end)
  for i = 1, math.min(max, #roster) do deploy.add(battle, nil, roster[i], cells[i]) end
  p:start(battle)
  world:overlay("deploy", cells, { color = config.settings.colors.deploy, pulse = 1 })
  p:banner(i18n.t("fe.tactics.deploy.help"), 3)
  local selected
  while true do
    local cell, done = p:pick_cell(cells, cells[1], nil, "menu")
    if done then
      if #battle:side("party", true) > 0 then break end
    elseif not cell then
      selected = nil
    else
      local u = grid.unit_at(battle, cell)
      if selected and u == selected then
        if not tablex.contains(required, selected.unit_id) then
          local choice = choose_actor(battle, i18n.t("fe.tactics.deploy.replace", { name = selected:name() }),
            #battle:side("party", true) > 1)
          if choice then deploy.remove(battle, p, selected) end
          if choice and choice ~= "remove" then deploy.add(battle, p, choice, cell) end
        end
        selected = nil
      elseif selected then
        local from = selected.cell
        if u then
          grid.place(battle, u, from)
          p:sync(u)
        end
        grid.place(battle, selected, cell)
        p:sync(selected)
        selected = nil
      elseif u then
        selected = u
      elseif #battle:side("party", true) < max then
        local actor = choose_actor(battle, i18n.t("fe.tactics.deploy.title"), false)
        if actor then deploy.add(battle, p, actor, cell) end
      end
    end
  end
  world:clear_overlay("deploy")
  return battle:side("party", true)
end

return deploy
