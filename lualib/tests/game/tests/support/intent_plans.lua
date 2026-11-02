-- Planner for the "seer" test enemy (fe.battle.intents): tests set
-- `plans.choose(battle, me, previous) -> intent` and read `plans.calls`.
-- `execute` is fe.battle.intents.execute under a game module path, the way
-- a game names it in data (fengine_headless --check resolves game modules).
local plans = { calls = 0, execute = require("fe.battle.intents").execute }

function plans.plan(battle, me, previous)
  plans.calls = plans.calls + 1
  if plans.choose then return plans.choose(battle, me, previous) end
  local foe = battle:targets_for(me, "enemy", nil)[1]
  return foe and { skill = "attack", targets = { foe }, label = "Attack" }
end

return plans
