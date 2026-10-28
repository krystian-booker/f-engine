-- The battle on Hollow Ridge. The council's route decides the objective and
-- where the raiders wait.
local state = require("fe.state")
local tactical = require("fe.battle.tactical")

local ridge_battle = {}

local layouts = {
  hold = {
    enemies = {
      { enemy = "raider", cell = { x = 5, z = 7 } }, { enemy = "raider", cell = { x = 12, z = 8 } },
      { enemy = "raider", cell = { x = 7, z = 5 } }, { enemy = "raider_archer", cell = { x = 3, z = 2 } },
      { enemy = "raider_archer", cell = { x = 13, z = 2 } }, { enemy = "captain", id = "captain", cell = { x = 12, z = 1 } },
    },
    win = function() return { tactical.objectives.survive(5), tactical.objectives.defeat_all("enemy") } end,
  },
  flank = {
    enemies = {
      { enemy = "raider", cell = { x = 4, z = 7 } }, { enemy = "raider", cell = { x = 6, z = 8 } },
      { enemy = "raider_archer", cell = { x = 3, z = 2 } }, { enemy = "raider_archer", cell = { x = 11, z = 3 } },
      { enemy = "captain", id = "captain", cell = { x = 12, z = 1 } },
    },
    win = function() return { tactical.objectives.defeat("captain", "Captain Vey") } end,
  },
}

--- Fight on the ridge. `options.units` skips deployment; `options.auto`
--- lets the AI play the party (tests and replays).
function ridge_battle.fight(route, options)
  options = options or {}
  local layout = layouts[route] or layouts.hold
  local enemies = {}
  for _, spec in ipairs(layout.enemies) do
    enemies[#enemies + 1] = { enemy = spec.enemy, id = spec.id, cell = { x = spec.cell.x, z = spec.cell.z, layer = 0 },
      facing = { x = 0, z = 1 } }
  end
  local outcome = tactical.start({
    map = "ridge",
    music = "battle",
    deploy = not options.units and { tag = "deploy", max = 4 } or nil,
    units = options.units,
    auto = options.auto,
    enemies = enemies,
    win = layout.win(),
    lose = { tactical.objectives.defeat_all("party"), tactical.objectives.protect("cora", "Cora") },
  })
  if outcome == "victory" then state.set_flag("ridge_won") end
  return outcome
end

return ridge_battle
