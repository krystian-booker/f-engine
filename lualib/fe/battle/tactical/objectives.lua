--- Tactical objectives: {text, check(battle) -> "victory" | "defeat" | nil}.
--- `objectives.rules(win, lose)` turns lists of them into a rule layer
--- whose `outcome` hook checks lose conditions first. Texts are fe.tactics.*
--- i18n keys.
local i18n = require("fe.i18n")
local tablex = require("fe.tablex")

local objectives = {}

i18n.defaults({ fe = { tactics = { objective = {
  defeat_all = "Defeat all enemies", all_fall = "All allies fall", defeat = "Defeat {name}",
  protect = "{name} must survive", reach = "Reach the marked area", survive = "Survive {rounds} rounds",
} } } })

-- Reinforcements of `side` still to come that must be defeated too.
local function required_pending(battle, side)
  for _, spec in ipairs(battle.tactical and battle.tactical.reinforcements or {}) do
    if spec.required and (spec.side or "enemy") == side then return true end
  end
  return false
end

local function is_unit(b, id)
  return b.id == id or b.unit_id == id
end

--- All units of a side defeated ("enemy" by default), including
--- reinforcements scheduled with `required = true`.
function objectives.defeat_all(side)
  side = side or "enemy"
  local text = i18n.t(side == "enemy" and "fe.tactics.objective.defeat_all" or "fe.tactics.objective.all_fall")
  return { text = text, check = function(battle)
    if #battle:side(side) == 0 and not required_pending(battle, side) then
      return side == "enemy" and "victory" or "defeat"
    end
  end }
end

--- A specific unit (by id) is defeated.
function objectives.defeat(id, name)
  return { text = i18n.t("fe.tactics.objective.defeat", { name = name or id }), check = function(battle)
    for _, b in ipairs(battle.battlers) do
      if is_unit(b, id) and not b:alive() then return "victory" end
    end
  end }
end

--- Lose if this unit falls.
function objectives.protect(id, name)
  return { text = i18n.t("fe.tactics.objective.protect", { name = name or id }), check = function(battle)
    for _, b in ipairs(battle.battlers) do
      if is_unit(b, id) and not b:alive() then return "defeat" end
    end
  end }
end

--- Win when any unit of `side` stands on a cell with `tag`.
function objectives.reach(tag, side)
  side = side or "party"
  return { text = i18n.t("fe.tactics.objective.reach"), check = function(battle)
    for _, b in ipairs(battle:side(side)) do
      local c = b.cell and battle.tactical.world:cell(b.cell)
      if c and tablex.contains(c.tags, tag) then return "victory" end
    end
  end }
end

--- Win once `rounds` full rounds have passed (a round ends when every
--- living unit has taken a turn).
function objectives.survive(rounds)
  return { text = i18n.t("fe.tactics.objective.survive", { rounds = rounds }), check = function(battle)
    if battle.round > rounds then return "victory" end
  end }
end

--- A rule layer checking `lose` (first) and `win` objectives. While a
--- side is wiped out but required reinforcements for it are still to
--- come, the battle goes on.
function objectives.rules(win, lose)
  return {
    outcome = function(battle)
      for _, o in ipairs(lose) do
        if o.check(battle) == "defeat" then return "defeat" end
      end
      for _, o in ipairs(win) do
        if o.check(battle) == "victory" then return "victory" end
      end
      for _, side in ipairs({ "party", "enemy" }) do
        if #battle:side(side) == 0 and required_pending(battle, side) then return false end
      end
    end,
  }
end

--- One line describing objectives (HUD).
function objectives.describe(list)
  return table.concat(tablex.map(list, function(o) return o.text end), " / ")
end

return objectives
