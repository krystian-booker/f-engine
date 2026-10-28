--- Building blocks for enemy (and auto-battle) decisions. An AI function
--- receives (battle, battler) and returns an action table. Enemies name one
--- in data ("ai": "ai.goblin.think"); without one, `ai.default` is used.
---
--- `ai.choose` scores candidate actions so custom AIs can stay short:
--- ```lua
--- return { think = function(battle, me)
---   return ai.choose(battle, me, {
---     score = function(skill, targets) return skill.id == "heal" and ai.missing_hp(targets) or 1 end,
---   })
--- end }
--- ```
local ai = {}

--- Fraction of life missing across targets (0..n).
function ai.missing_hp(targets)
  local life = require("fe.rpg.combatant").life
  local total = 0
  for _, t in ipairs(targets) do
    total = total + 1 - t:resource(life) / math.max(1, t:max_resource(life))
  end
  return total
end

--- Every (skill, targets) pair a battler could use now.
function ai.options(battle, battler)
  local out = {}
  local skills = battler:skills()
  if #skills == 0 then
    skills = { require("fe.data").find("skills", "attack") }
  end
  for _, skill in ipairs(skills) do
    if skill and battler:can_pay(skill.cost) then
      local candidates = battle:targets_for(battler, skill.target)
      if #candidates > 0 then
        if skill.target:find("^all") or skill.target == "everyone" then
          out[#out + 1] = { skill = skill, targets = candidates }
        else
          for _, target in ipairs(candidates) do
            out[#out + 1] = { skill = skill, targets = { target } }
          end
        end
      end
    end
  end
  return out
end

--- Pick the best-scored option (ties broken randomly); weights can be
--- randomised with `jitter` (0..1).
---@param options {score?: fun(skill: table, targets: table[], battle: table, me: table): number, jitter?: number}
function ai.choose(battle, battler, options)
  options = options or {}
  local candidates = ai.options(battle, battler)
  if #candidates == 0 then
    return { kind = "defend", user = battler }
  end
  local best, best_score
  for _, c in ipairs(candidates) do
    local score = options.score and options.score(c.skill, c.targets, battle, battler) or 1
    score = score * (1 + (options.jitter or 0.5) * battle.random:float())
    if not best or score > best_score then best, best_score = c, score end
  end
  return { kind = "skill", user = battler, skill = best.skill.id, targets = best.targets }
end

--- Default: random usable option, preferring hurting the weakest foe.
function ai.default(battle, battler)
  local life = require("fe.rpg.combatant").life
  return ai.choose(battle, battler, {
    score = function(_, targets)
      local hurt = 0
      for _, t in ipairs(targets) do hurt = hurt + 1 / math.max(1, t:resource(life)) end
      return 1 + hurt * 10
    end,
  })
end

--- Resolve a battler's AI function.
function ai.for_battler(battler)
  local name = battler.def and battler.def.ai
  if not name then return ai.default end
  local module_name, fn = name:match("^(.*)%.([%w_]+)$")
  local f = module_name and require(module_name)[fn]
  assert(f, "AI function '" .. name .. "' not found")
  return f
end

return ai
