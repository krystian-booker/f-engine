-- Slimes poison healthy targets and bully the weakest one.
local ai = require("fe.battle.ai")

return {
  think = function(battle, me)
    return ai.choose(battle, me, {
      score = function(skill, targets)
        local target = targets[1]
        if skill.id == "venom" then
          return target:has_state("poison") and 0.2 or 1.5
        end
        return 1 + (1 - target:resource("hp") / target:max_resource("hp"))
      end,
    })
  end,
}
