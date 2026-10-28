-- Game rules for Vale's battles (hooks into fe.battle.core). Everything
-- specific to this game lives here, not in the engine.
local rules = {}

-- Characters and enemies escape more easily when faster than the foes.
function rules.escape_chance(battle, user)
  local fastest = 0
  for _, foe in ipairs(battle:opponents(user)) do fastest = math.max(fastest, foe:stat("speed")) end
  return math.min(0.9, 0.4 + (user:stat("speed") - fastest) * 0.02)
end

-- The guardian fight has a mid-battle line when the golem is hurt.
function rules.after_action(battle, action, results)
  if not battle.troop or battle.troop.id ~= "guardian" or battle.data.taunted then return end
  for _, b in ipairs(battle:side("enemy")) do
    if b:resource("hp") < b:max_resource("hp") * 0.5 then
      battle.data.taunted = true
      require("fe.dialogue").say("Guardian", "...THE LIGHT... MUST NOT LEAVE...")
      require("fe.dialogue").close()
    end
  end
end

return rules
