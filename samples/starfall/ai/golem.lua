-- The guardian alternates single blows with a quake every third turn.
local ai = require("fe.battle.ai")

return {
  think = function(battle, me)
    me.data.turns = (me.data.turns or 0) + 1
    local want = me.data.turns % 3 == 0 and "quake" or "attack"
    return ai.choose(battle, me, {
      score = function(skill) return skill.id == want and 10 or 1 end,
      jitter = 0,
    })
  end,
}
