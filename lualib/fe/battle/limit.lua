--- A limit gauge as an optional rule layer: a resource that fills during
--- battle and pays for special skills (Octopath Traveler's divine skills
--- and latent powers, limit breaks).
---
--- Declare the resource in the stat block and give skills a cost in it:
--- ```lua
--- stats.resources({ ..., lp = { max = 3, start = 0, name = "LP", battle_only = true } })
--- -- skills.json: {"id": "starfall", "type": "divine", "cost": {"lp": 3}, ...}
--- battle.rules = limit.rules()        -- or inside another kit's layer: shields.rules(limit.rules())
--- ```
--- A class command naming the skill type ("divine") lists those skills in
--- the side-view menu; they are usable once the gauge holds their cost.
---
--- Gains (settings, 0 turns one off), for battlers on `settings.sides`
--- whose maximum of the resource is above 0:
---   damaged: per action that damages the battler
---   breaks: per enemy the battler breaks (fe.battle.break's on_break)
---   ally_down: per ally knocked out
--- Events: "limit_ready" (battler) when the gauge becomes full.
local stats = require("fe.rpg.stats")

local limit = {
  settings = {
    resource = "lp",
    damaged = 1,
    breaks = 1,
    ally_down = 1,
    sides = { party = true },
  },
}

local settings = limit.settings

local function has_gauge(b)
  return settings.sides[b.side] == true and stats.resource(settings.resource) ~= nil
    and b:max_resource(settings.resource) > 0
end

--- Whether the battler's gauge is full.
function limit.ready(b)
  return has_gauge(b) and b:resource(settings.resource) >= b:max_resource(settings.resource)
end

--- Fill a battler's gauge by `amount`; returns the change.
function limit.gain(battle, b, amount)
  if amount <= 0 or not has_gauge(b) or not b:alive() then return 0 end
  local was_ready = limit.ready(b)
  local change = b:change_resource(settings.resource, amount)
  if not was_ready and limit.ready(b) then battle:present("limit_ready", b) end
  return change
end

--- The rule layer. `extra` (a rule layer) is added right after it as a
--- layer of its own, so its hooks run too instead of replacing these.
function limit.rules(extra)
  local layer = { layers = { extra } }

  function layer.after_action(battle, action, results)
    local seen = {}
    for _, r in ipairs(results or {}) do
      local b = r.target
      if b and not seen[b] and r.type == "damage" and not r.missed and (r.amount or 0) > 0 and b.side ~= action.user.side then
        seen[b] = true
        limit.gain(battle, b, settings.damaged)
      end
    end
  end

  function layer.on_break(battle, _, breaker)
    if breaker then limit.gain(battle, breaker, settings.breaks) end
  end

  function layer.on_knocked_out(battle, fallen)
    for _, ally in ipairs(battle:allies(fallen)) do limit.gain(battle, ally, settings.ally_down) end
  end

  return layer
end

return limit
