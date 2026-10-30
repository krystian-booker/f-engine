--- Grid tactics battles fought on a map: deployment, move/act/wait turns
--- ordered by initiative, ranges and areas of effect, height and facing
--- bonuses, follow-up (pincer) attacks, zones of control, knockback, terrain
--- reactions (fire burns and spreads through grass, ice freezes water,
--- lightning runs through water), destructible obstacles, reinforcements
--- and objectives.
---
--- ```lua
--- local outcome = tactical.start{
---   map = "ridge_pass",                         -- or the current field map
---   deploy = {tag = "deploy", max = 6},         -- players choose and place units first
---   enemies = {{enemy = "bandit", cell = {x = 9, z = 3}}, ...},
---   obstacles = {{enemy = "barricade", cell = {x = 6, z = 5}}},
---   reinforcements = {{enemy = "bandit", cell = {x = 0, z = 0}, turn = 3}},
---   win = {tactical.objectives.defeat_all()},
---   lose = {tactical.objectives.defeat_all("party"), tactical.objectives.protect("leader")},
--- }
--- ```
--- `tactical.start` is `fe.battle.start` with this presentation, so the
--- game's battle rules, signals and on_defeat apply; setting
--- `battle.settings.presentation = "fe.battle.tactical"` works too (troop
--- members then need a `position` cell, and may give `id` and `facing`;
--- the party starts on the settings.deploy_tag cells unless `units` or
--- `deploy` say otherwise).
---
--- Skills use `range` and `area` ({shape, min, max, height}) from their data;
--- the attack skill (fe.battle.core settings.attack_skill) falls back to
--- settings.attack_range. Every rule is a setting, data or a hook, so a game
--- can turn mechanics on or off: `settings.step_rules(battle, unit, rules)`
--- changes one unit's movement rules (flyers, mounts), a `range(battle,
--- unit, skill, cells, from)` rule hook changes a skill's reach (a bow from
--- high ground), `cell_entered(battle, unit, cell) -> "stop"|nil` reacts to
--- each cell a moving unit enters (a trap: "stop" ends the move there),
--- `plan(battle, unit)` replaces the AI and
--- `Battle.settings.exp_per_action` grants experience per action. Modules:
--- grid (board, reach, ranges), rules (turns, damage bonuses, knockback,
--- terrain reactions), terrain, objectives, ai, presenter, deploy.
local ai = require("fe.battle.tactical.ai")
local Combatant = require("fe.rpg.combatant")
local common = require("fe.battle.presenter_common")
local config = require("fe.battle.tactical.settings")
local deploy = require("fe.battle.tactical.deploy")
local grid = require("fe.battle.tactical.grid")
local input = require("fe.input")
local objectives = require("fe.battle.tactical.objectives")
local party = require("fe.rpg.party")
local Presenter = require("fe.battle.tactical.presenter")
local rules = require("fe.battle.tactical.rules")
local stats = require("fe.rpg.stats")
local tablex = require("fe.tablex")
local terrain = require("fe.battle.tactical.terrain")
local i18n = require("fe.i18n")
local transition = require("fe.transition")
local ui = require("fe.ui")

local tactical = {
  settings = config.settings,
  reactions = config.reactions,
  hazards = config.hazards,
  objectives = objectives,
  Presenter = Presenter,
  --- The battle in progress, if any.
  active = nil,
  reachable = grid.reachable,
  path_to = grid.path_to,
  range_cells = grid.range_cells,
  affected = grid.affected,
  place = grid.place,
  unit_at = grid.unit_at,
  corpse_at = grid.corpse_at,
  corpses_at = grid.corpses_at,
  face = grid.face,
  walk = grid.walk,
  react = terrain.react,
  set_cell = terrain.set_cell,
  plan = ai.plan,
  board = rules.board,
  ct_cost = rules.ct_cost,
  reinforce = rules.reinforce,
  follow_ups = rules.follow_ups,
  positional_info = rules.positional_info,
  add_unit = rules.add_unit,
  deploy = deploy,
}

i18n.defaults({ fe = { tactics = {
  counter_chance = "Counter {percent}%", from_behind = "Back", from_side = "Side",
  high_ground = "High ground", low_ground = "Low ground",
} } })

--- The battler with this unit id (or battler id), if any.
function tactical.unit(battle, id)
  return tablex.find(battle.battlers, function(b) return b.unit_id == id or b.id == id end)
end

--- Put an obstacle on the board: spec {enemy, cell, id?, name?}. The
--- enemy data should say `"obstacle": true` (the spec may instead) and
--- can give a `destroyed` terrain reaction. Obstacles belong to no side:
--- a spec with `side` is an error.
function tactical.add_obstacle(battle, spec)
  if spec.side ~= nil then error("obstacles belong to no side; remove `side` from the " .. spec.enemy .. " spec", 2) end
  return rules.add_unit(battle, { enemy = spec.enemy, obstacle = true, name = spec.name, cell = spec.cell,
    id = spec.id or spec.enemy })
end

--- Default scheduler: initiative with tactical turns.
function tactical.scheduler()
  return rules.scheduler()
end

--- fe.battle presentation entry: set up the board (loading `options.map`,
--- or using the field world) and return the presenter.
function tactical.new(battle, options)
  local world
  if options.map then
    transition.out("fade", 0.3)
    world = fe.world.load(config.settings.world_name, options.map)
  else
    world = require("fe.field").world
  end
  rules.board(battle, world, options)
  battle.escapable = options.escapable == true
  tactical.active = battle
  return Presenter.new(battle, options)
end

local function add_enemies(battle, options)
  local specs = options.enemies
  if not specs and battle.troop then
    specs = tablex.map(battle.troop.members, function(m)
      assert(m.position and m.position.x, "tactical troops need a position {x, z} cell for every member")
      return { enemy = m.enemy, id = m.id, facing = m.facing, name = m.name,
        cell = { x = m.position.x, z = m.position.z, layer = m.position.layer or 0 } }
    end)
  end
  for i, spec in ipairs(specs or {}) do
    rules.add_unit(battle, { enemy = spec.enemy, side = "enemy", name = spec.name, cell = spec.cell,
      facing = spec.facing or { x = 0, z = 1 }, id = spec.id or (spec.enemy .. i) })
  end
end

local function add_unit(battle, spec)
  local u = rules.add_unit(battle, { actor = party.actor(spec.actor), side = "party", cell = spec.cell,
    facing = spec.facing or { x = 0, z = -1 }, id = spec.actor })
  u.auto = spec.auto
end

--- fe.battle populate entry: units from `options.units`, a deployment
--- phase (`options.deploy`), or the active party on the deploy cells;
--- enemies from `options.enemies` or the troop; `options.obstacles` and
--- scheduled `options.reinforcements`. `options.auto` lets the AI play
--- every party unit.
function tactical.populate(battle, options)
  for _, spec in ipairs(options.obstacles or {}) do tactical.add_obstacle(battle, spec) end
  for _, spec in ipairs(options.reinforcements or {}) do rules.reinforce(battle, spec) end
  if options.units then
    for _, spec in ipairs(options.units) do add_unit(battle, spec) end
    add_enemies(battle, options)
  elseif options.deploy then
    add_enemies(battle, options)
    deploy.run(battle.presenter, battle, options.deploy)
  else
    local cells = battle.tactical.world:cells_with_tag(config.settings.deploy_tag)
    for i, actor in ipairs(party.members()) do
      if cells[i] then add_unit(battle, { actor = actor.id, cell = cells[i] }) end
    end
    add_enemies(battle, options)
  end
  if options.auto then
    for _, u in ipairs(battle:side("party", true)) do u.auto = true end
  end
end

--- Run a tactics battle (inside a task) through fe.battle.start. See the
--- module comment. Party units with `auto` (or every party unit with
--- `options.auto`) are planned like enemies: guests, or unattended tests
--- and replays.
---@param options {map?: string, units?: table[], enemies?: table[], obstacles?: table[], reinforcements?: table[], deploy?: {tag?: string, max?: integer, required?: string[]}, win?: table[], lose?: table[], music?: string, rules?: table, auto?: boolean, troop?: string, allow_defeat?: boolean, opening?: string}
function tactical.start(options)
  local start = tablex.copy(options)
  start.presentation = "fe.battle.tactical"
  return require("fe.battle").start(start)
end

--- The forecast panel's lines: per damage or heal result the amount and
--- hit chance, then (indented) the positional bonuses and counter chance.
function tactical.forecast_lines(forecast)
  local lines = { "{color:heading}" .. forecast.skill.name .. "{color:reset}" }
  for _, r in ipairs(forecast.results) do
    if r.type == "damage" or r.type == "heal" then
      local unit = r.target
      lines[#lines + 1] = string.format("%s: %s%d  %d%%", unit:name(), r.type == "heal" and "+" or "-",
        r.amount or 0, math.floor((r.hit_chance or 1) * 100 + 0.5))
      local notes = {}
      local info = r.type == "damage" and forecast.positional and forecast.positional[unit]
      if info then
        if info.height_bonus > 0 then notes[#notes + 1] = i18n.t("fe.tactics.high_ground") end
        if info.height_bonus < 0 then notes[#notes + 1] = i18n.t("fe.tactics.low_ground") end
        if info.from_behind then notes[#notes + 1] = i18n.t("fe.tactics.from_behind") end
        if info.from_side then notes[#notes + 1] = i18n.t("fe.tactics.from_side") end
      end
      local counter = forecast.counter and forecast.counter[unit]
      if counter and counter > 0 then
        notes[#notes + 1] = i18n.t("fe.tactics.counter_chance", { percent = math.floor(counter * 100 + 0.5) })
      end
      if #notes > 0 then lines[#lines + 1] = "{color:muted}  " .. table.concat(notes, ", ") .. "{color:reset}" end
    end
  end
  return lines
end

--- HUD: turn order (while choosing, the order that follows the current
--- choice's CT cost, the acting unit's next turns highlighted, and the
--- cost), active/hover unit panels (life and battle-only resources such as
--- TP), forecast (damage, hit chance, height and facing bonuses, counter
--- chance), objectives, rewards at the end.
function tactical.draw_hud(p)
  local cw, ch = fe.canvas.size()
  local life = Combatant.life
  local bx = 8
  for i, b in ipairs(p.order or {}) do
    if i > 10 then break end
    ui.window(bx, 6, 30, 14, { alpha = b == p.active and 1 or 0.85 })
    local color = b == p.active and ui.theme.highlight or (b:is_party() and ui.theme.good or ui.theme.bad)
    ui.text(b:name():sub(1, 4), bx + 4, 9, { color = color })
    bx = bx + 32
  end
  if p.ct_cost then
    ui.text(i18n.t("fe.tactics.ct", { cost = p.ct_cost }), bx + 2, 9, { color = ui.theme.muted })
  end
  local function panel(b, x, y)
    ui.window(x, y, 128, 38)
    ui.text(b:name(), x + 6, y + 5, { color = b:is_party() and ui.theme.good or ui.theme.bad })
    ui.text(string.format("%s %d/%d", life:upper(), b:resource(life), b:max_resource(life)), x + 6, y + 16)
    local extra = {}
    for _, def in ipairs(stats.resource_list) do
      if def.battle_only and b:max_resource(def.id) > 0 then extra[#extra + 1] = def.name .. " " .. b:resource(def.id) end
    end
    if #extra > 0 then ui.text(table.concat(extra, " "), x + 6, y + 5, { width = 116, align = "right", color = ui.theme.heading }) end
    ui.gauge(x + 6, y + 27, 116, 4, b:resource(life), b:max_resource(life), ui.theme.gauges.hp)
  end
  if p.active then panel(p.active, 8, 30) end -- clear of the command menu at the bottom left
  if p.hover and p.hover ~= p.active then panel(p.hover, cw - 136, ch - 44) end
  if p.forecast and #p.forecast.results > 0 then
    local text = table.concat(tactical.forecast_lines(p.forecast), "\n")
    local _, h = ui.measure(text, { width = 150 })
    ui.window(cw - 168, 28, 160, h + 12)
    ui.text(text, cw - 162, 34, { width = 150 })
  end
  if p.battle.objectives_text then
    ui.text(p.battle.objectives_text, cw - 8 - ui.measure(p.battle.objectives_text), 9, { color = ui.theme.muted })
  end
  if p.choosing_facing then
    ui.text(i18n.t("fe.tactics.facing", { button = input.prompt("confirm") }), 0, ch - 58,
      { width = cw, align = "center" })
  end
  if p.banner_text then common.draw_banner(p.banner_text, 26) end
  common.draw_popups(p)
  if p.rewards_text then common.draw_text_window(p.rewards_text) end
end

return tactical
