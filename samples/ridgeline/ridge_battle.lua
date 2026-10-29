-- The battle on Hollow Ridge. The council's route decides the objective and
-- where the raiders wait. Barricades guard the approach (archers cannot
-- shoot through them; broken ones leave rubble that slows movement), more
-- raiders arrive while the fight goes on, and Cora and the captain riposte
-- melee attacks that do not come from behind. Arts cost TP (fe.battle.tp,
-- as in Triangle Strategy): everyone starts a battle with 1 TP, gains 1 at
-- the start of each turn (up to 5), and Cora's quietus, Steadfast Charge,
-- can be used once per battle.
local reactions = require("fe.battle.reactions")
local state = require("fe.state")
local tactical = require("fe.battle.tactical")
local tp = require("fe.battle.tp")

local ridge_battle = {}

tactical.settings.tag_costs.rubble = 1
tp.declare({ max = 5, start = 1 })

local barricades = { { x = 7, z = 9 }, { x = 11, z = 9 } }

local layouts = {
  hold = {
    enemies = {
      { enemy = "raider", cell = { x = 5, z = 7 } }, { enemy = "raider", cell = { x = 12, z = 8 } },
      { enemy = "raider", cell = { x = 7, z = 5 } }, { enemy = "raider_archer", cell = { x = 3, z = 2 } },
      { enemy = "raider_archer", cell = { x = 13, z = 2 } }, { enemy = "captain", id = "captain", cell = { x = 12, z = 1 } },
    },
    -- Holding the pass: a second wave comes over the ridge in round 3.
    reinforcements = {
      { enemy = "raider", cell = { x = 3, z = 0 }, turn = 3 }, { enemy = "raider", cell = { x = 14, z = 0 }, turn = 3 },
    },
    win = function() return { tactical.objectives.survive(5), tactical.objectives.defeat_all("enemy") } end,
  },
  flank = {
    enemies = {
      { enemy = "raider", cell = { x = 4, z = 7 } }, { enemy = "raider", cell = { x = 6, z = 8 } },
      { enemy = "raider_archer", cell = { x = 3, z = 2 } }, { enemy = "raider_archer", cell = { x = 11, z = 3 } },
      { enemy = "captain", id = "captain", cell = { x = 12, z = 1 } },
    },
    -- The captain calls his guard once he is badly hurt.
    reinforcements = {
      { enemy = "raider", id = "guard", cell = { x = 15, z = 1 }, when = function(battle)
        local captain = tactical.unit(battle, "captain")
        return captain ~= nil and captain:resource("hp") < captain:max_resource("hp") / 2
      end },
    },
    win = function() return { tactical.objectives.defeat("captain", "Captain Vey") } end,
  },
}

--- Fight on the ridge. `options.units` skips deployment; `options.auto`
--- lets the AI play the party (tests and replays).
function ridge_battle.fight(route, options)
  options = options or {}
  local layout = layouts[route] or layouts.hold
  local function on_ground(cell) return { x = cell.x, z = cell.z, layer = 0 } end
  local enemies = {}
  for _, spec in ipairs(layout.enemies) do
    enemies[#enemies + 1] = { enemy = spec.enemy, id = spec.id, cell = on_ground(spec.cell), facing = { x = 0, z = 1 } }
  end
  local obstacles = {}
  for _, cell in ipairs(barricades) do obstacles[#obstacles + 1] = { enemy = "barricade", cell = on_ground(cell) } end
  local reinforcements = {}
  for _, spec in ipairs(layout.reinforcements) do
    reinforcements[#reinforcements + 1] = { enemy = spec.enemy, id = spec.id, cell = on_ground(spec.cell), turn = spec.turn,
      when = spec.when, facing = { x = 0, z = 1 } }
  end
  local outcome = tactical.start({
    map = "ridge",
    music = "battle",
    deploy = not options.units and { tag = "deploy", max = 4 } or nil,
    units = options.units,
    auto = options.auto,
    enemies = enemies,
    obstacles = obstacles,
    reinforcements = reinforcements,
    rules = tp.rules({ reaction = reactions.counter({ requires = "riposte", facing = "not_behind" }) }),
    win = layout.win(),
    lose = { tactical.objectives.defeat_all("party"), tactical.objectives.protect("cora", "Cora") },
    allow_defeat = true, -- the story continues either way
  })
  if outcome == "victory" then state.set_flag("ridge_won") end
  return outcome
end

return ridge_battle
