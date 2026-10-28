return {
  formulas = {
    physical = function(a, target, power)
      return math.max(1, a.attack * 2 + power - target.defense)
    end,
    magical = function(a, target, power)
      return math.max(1, a.magic * 2 + power - target.resistance)
    end,
    flat = function(_, _, power) return power end
  },
  effects = {
    drain = function(a, target, power)
      local amount = math.min(target.hp, math.max(1, a.magic * 2 + power - target.resistance))
      return {
        {type = "damage", formula = "flat", power = amount},
        {type = "heal", recipient = "user", power = math.floor(amount / 2)}
      }
    end
  },
  ai = {
    default = function(self, battlers)
      for target, b in ipairs(battlers) do
        if b.hp > 0 and b.enemy ~= battlers[self].enemy then
          return {skill = "attack", target = target}
        end
      end
    end,
    slime = function(self, battlers)
      local actor = battlers[self]
      local skill = actor.mp >= 3 and actor.hp < actor.max_hp * 0.65 and "venom" or "attack"
      local weakest, hp = nil, math.huge
      for target, b in ipairs(battlers) do
        if b.hp > 0 and b.enemy ~= actor.enemy and b.hp < hp then
          weakest, hp = target, b.hp
        end
      end
      return {skill = skill, target = weakest}
    end
  }
}
