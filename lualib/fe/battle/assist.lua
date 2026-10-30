--- Companions who help in battle without being party members (Octopath
--- Traveler's NPC followers), as an optional rule layer.
---
--- ```lua
--- local assist = require("fe.battle.assist")
--- assist.join("old_knight", { enemy = "old_knight", skill = "shield_bash", chance = 0.5 })
--- battle.rules = assist.rules()       -- or inside another kit's layer: shields.rules(assist.rules())
--- ```
--- A companion is a stat block from data/enemies.json (name, stats,
--- sprite) and one skill. At the end of every round, in id order, each
--- companion acts with its `chance` (battle RNG; settings.chance by
--- default): it appears on the party's side (presenters get
--- "battler_added" and an "assist" event), uses its skill on a random
--- target the skill allows, and leaves again ("battler_removed"). It
--- cannot be targeted between its appearances. Companions are kept in the
--- "assists" state section, so saves remember them; a game connects its
--- field followers with `assist.join` / `assist.leave`.
local data = require("fe.data")
local state = require("fe.state")
local tablex = require("fe.tablex")

local assist = {
  settings = { chance = 0.5 },
}

local record = state.section("assists", function() return {} end)

--- Add (or update) a companion: {enemy, skill, chance?, name?}.
---@param spec {enemy: string, skill: string, chance?: number, name?: string}
function assist.join(id, spec)
  data.get("enemies", spec.enemy)
  data.get("skills", spec.skill)
  record[id] = { enemy = spec.enemy, skill = spec.skill, chance = spec.chance, name = spec.name }
end

function assist.leave(id)
  record[id] = nil
end

--- Companions as {id, enemy, skill, chance, name}, sorted by id.
function assist.list()
  local out = {}
  for _, id in ipairs(tablex.keys(record)) do
    local entry = tablex.copy(record[id])
    entry.id = id
    out[#out + 1] = entry
  end
  table.sort(out, function(a, b) return a.id < b.id end)
  return out
end

--- One companion's turn now: appear, use the skill, leave. Returns the
--- results, or nil when the skill has no target.
function assist.act(battle, entry)
  local skill = data.get("skills", entry.skill)
  local helper = battle:summon({ enemy = entry.enemy, side = "party", name = entry.name })
  local pool = battle:targets_for(helper, skill.target)
  local results
  if #pool > 0 then
    local targets = pool
    if not (skill.target:find("^all") or skill.target == "everyone") then
      targets = { pool[battle.random:int(1, #pool)] }
    end
    battle:present("assist", { battler = helper, id = entry.id })
    results = battle:perform({ kind = "skill", user = helper, skill = skill.id, targets = targets, assist = true })
  end
  battle:remove(helper)
  return results
end

--- The rule layer. `extra` (a rule layer) is added right after it as a
--- layer of its own, so its hooks run too instead of replacing these.
function assist.rules(extra)
  local layer = { layers = { extra } }

  -- on_round for round n > 1 runs after round n - 1's last turn.
  function layer.on_round(battle, round)
    if round <= 1 then return end
    for _, entry in ipairs(assist.list()) do
      if battle:outcome() then return end
      if battle.random:float() < (entry.chance or assist.settings.chance) then assist.act(battle, entry) end
    end
  end

  return layer
end

return assist
