--- Tactical points and quietuses (Triangle Strategy) as an optional rule
--- layer: a battle-only resource that every unit starts each battle with,
--- regains turn by turn and spends on skills, and quietus skills that each
--- unit can use once per battle.
---
--- ```lua
--- local tp = require("fe.battle.tp")
--- tp.declare({ max = 5, start = 1 })              -- adds "tp" to the game's resources
--- tactical.start({ ..., rules = tp.rules({ reaction = ... }) })
--- -- skills.json: {"id": "shove", "cost": {"tp": 1}, ...}
--- --              {"id": "last_stand", "tags": ["quietus"], "cost": {"tp": 3}, ...}
--- ```
--- * Skills cost TP like any resource (`cost`), so menus, the AI and
---   Battle:perform only allow what a unit can pay.
--- * Every battler whose maximum TP is above 0 gains `settings.per_turn`
---   at the start of each of its turns (its first included), up to the
---   maximum. Battles start everyone at the resource's `start` value.
--- * A skill tagged `settings.quietus_tag` is a quietus: each battler can
---   use each one once per battle (Battle:usable says no afterwards; the
---   menus grey it out and the AI skips it).
---
--- Events: "tp_gained" ({battler, amount}) when a turn's gain adds TP.
local stats = require("fe.rpg.stats")
local tablex = require("fe.tablex")

local tp = {
  settings = {
    resource = "tp",
    per_turn = 1,
    quietus_tag = "quietus",
  },
}

local settings = tp.settings

--- Add the TP resource ({max = 5, start = 1, name = "TP"} by default; a
--- battle-only resource) to the game's resources, keeping the others.
---@param def? {max?: integer, start?: integer, name?: string}
function tp.declare(def)
  def = def or {}
  local map = {}
  for _, existing in ipairs(stats.resource_list) do map[existing.id] = existing end
  map[settings.resource] = { max = def.max or 5, start = def.start or 1, name = def.name or "TP", battle_only = true }
  stats.resources(map)
end

local function has_tp(b)
  return stats.resource(settings.resource) ~= nil and b:max_resource(settings.resource) > 0
end

--- Whether a skill is a quietus.
function tp.is_quietus(skill)
  return tablex.contains(skill.tags or {}, settings.quietus_tag)
end

--- Whether `b` has used the quietus `skill_id` in this battle.
function tp.quietus_used(b, skill_id)
  return b.data.quietus ~= nil and b.data.quietus[skill_id] == true
end

--- The rule layer. `extra` (a rule layer) is added right after it as a
--- layer of its own, so its hooks run too instead of replacing these.
function tp.rules(extra)
  local layer = { layers = { extra } }

  function layer.on_turn_start(battle, b)
    if not has_tp(b) or not b:alive() or settings.per_turn <= 0 then return end
    local gained = b:change_resource(settings.resource, settings.per_turn)
    if gained > 0 then battle:present("tp_gained", { battler = b, amount = gained }) end
  end

  function layer.usable(_, b, skill)
    if tp.is_quietus(skill) and tp.quietus_used(b, skill.id) then return false end
  end

  function layer.on_perform(_, action)
    local skill = action.kind == "skill" and action.source
    if skill and tp.is_quietus(skill) then
      action.user.data.quietus = action.user.data.quietus or {}
      action.user.data.quietus[skill.id] = true
    end
  end

  return layer
end

return tp
