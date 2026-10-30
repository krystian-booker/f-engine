--- Shields, weaknesses, Break and Boost (the Octopath Traveler battle
--- loop) as an optional rule layer for fe.battle.
---
--- ```lua
--- local shields = require("fe.battle.break")
--- battle.rules = shields.rules({ on_round = ... })  -- extra layers keep their own hooks
--- shields.install_sideview()         -- boost input, shield/weakness markers and the break banner
--- ```
---
--- Enemy data: `"shield": 2, "weak": ["bow", "fire"]` (elements or damage
--- types; see the enemy schema). The game's states.json defines the break
--- state (settings.state, default "broken"); it should set
--- `prevents_action` and last 2 turns.
---
--- * Every hit of a damage effect whose element or damage type is a
---   weakness reveals that weakness and removes one shield point (one point
---   per matching key), also when the hit knocks the enemy out (a fallen
---   enemy does not break). Revealed weaknesses are remembered per enemy type
---   in the "bestiary" state section (saved with the game), so the next
---   battle against that enemy shows them from the start
---   (settings.remember; `shields.known(enemy_id)` lists them).
--- * At zero the enemy breaks: it gains the break state, takes
---   `damage_bonus` times damage, and the round order sends it to the back
---   of this round's remaining turns and of the next round. An enemy that
---   already took its turn this round breaks for one turn less, so it
---   loses exactly its next turn plus the rest of this round. Shields
---   refill at the end of the turn in which the state wears off.
--- * Boost: battlers with the boost resource (settings.resource, a
---   battle-only resource) gain `per_round` at the start of every round
---   after the first, except in a round they boosted. Choosing a command,
---   they may commit up to `max_spend`. The points are paid when the action
---   actually goes ahead (on_perform: after every before_action hook and the
---   cost check), at most what the battler has then, and nothing is paid
---   for a cancelled or unaffordable action. Each point paid repeats the
---   attack skill's hits once more (a 2-hit attack boosted by 2 hits 6
---   times); other actions gain `power_per_point` power per point.
---
--- * A revived enemy returns with full shields (on_revived), so a boss
---   script's revival makes it breakable again at once.
--- * `shields.reveal(battle, b, key)` reveals a weakness without a hit
---   (an Analyze skill); `shields.set_weaknesses(b, list)` changes a
---   battler's weaknesses mid-battle (a phase change) and
---   `shields.weaknesses(b)` reads them (b.data.weak, else the data's weak).
---
--- Events: "break" (battler), "shield_restored" (battler),
--- "weakness_revealed" ({battler, key}). Notification hook:
--- on_break(battle, battler, breaker).
local Battle = require("fe.battle.core")
local data = require("fe.data")
local i18n = require("fe.i18n")
local state = require("fe.state")
local stats = require("fe.rpg.stats")
local tablex = require("fe.tablex")

local shields = {
  settings = {
    state = "broken",
    damage_bonus = 2,
    default_shield = 2,
    resource = "bp",
    per_round = 1,
    max_spend = 3,
    power_per_point = 0.6,
    remember = true, -- keep revealed weaknesses per enemy type across battles
  },
}

local settings = shields.settings

i18n.defaults({ fe = { battle = {
  boost = "Boost", broken_marker = "BREAK", broken = "{name} is broken!",
} } })

local bestiary = state.section("bestiary", function() return { weaknesses = {} } end)

--- Weaknesses (element and damage type ids) of a battler: the list set
--- with shields.set_weaknesses for this battle, else its enemy data's `weak`.
function shields.weaknesses(b)
  return b.data.weak or (b.def and b.def.weak) or {}
end

--- Change a battler's weaknesses for the rest of the battle (a boss phase
--- that shifts its element); nil goes back to the data. Reveals stay.
---@param list string[]|nil
function shields.set_weaknesses(b, list)
  b.data.weak = list
end

function shields.is_broken(b)
  return b:has_state(settings.state)
end

--- Current and maximum shield points (nil for battlers without shields).
function shields.shield(b)
  return b.data.shield, b.data.max_shield
end

--- Weakness keys revealed for an enemy type in earlier battles, sorted.
function shields.known(enemy_id)
  local list = tablex.keys(bestiary.weaknesses[enemy_id] or {})
  table.sort(list)
  return list
end

--- Whether a weakness of `b` has been revealed (this battle, or before for
--- its enemy type when settings.remember is on).
function shields.revealed(b, key)
  if b.data.revealed ~= nil and b.data.revealed[key] == true then return true end
  local known = settings.remember and b.def and bestiary.weaknesses[b.def.id]
  return known ~= nil and known ~= false and known[key] == true
end

--- Reveal one of a battler's weaknesses (an Analyze skill): in this battle
--- and, with settings.remember, in the bestiary for its enemy type.
--- Presents "weakness_revealed" {battler, key} when it was hidden before.
--- `battle` may be nil (no event). Returns true when it was hidden.
function shields.reveal(battle, b, key)
  local was_hidden = not shields.revealed(b, key)
  b.data.revealed = b.data.revealed or {}
  b.data.revealed[key] = true
  if settings.remember and b.def then
    local known = bestiary.weaknesses[b.def.id] or {}
    known[key] = true
    bestiary.weaknesses[b.def.id] = known
  end
  if was_hidden and battle then battle:present("weakness_revealed", { battler = b, key = key }) end
  return was_hidden
end

-- Enemies get shields once; a battler swapped back in keeps its own.
local function init_shield(b)
  if b.def and b.side ~= "party" and not b.obstacle and b.data.max_shield == nil then
    b.data.max_shield = b.def.shield or settings.default_shield
    b.data.shield = b.data.max_shield
    b.data.revealed = {}
  end
end

-- Whether `b` already took its turn in the current round_order round.
local function acted_this_round(battle, b)
  local entries, index = battle.data.round_entries, battle.data.round_index
  if not entries or not index then return false end
  for i = 1, index - 1 do
    if entries[i].battler == b then return true end
  end
  return false
end

local function break_battler(battle, b, breaker)
  local turns = data.get("states", settings.state).turns
  if turns > 1 and acted_this_round(battle, b) then turns = turns - 1 end
  b:add_state(settings.state, turns)
  battle:present("break", b)
  battle:notify("on_break", b, breaker)
end

local function weakness_hit(ctx)
  local battle, target = ctx.battle, ctx.target
  if ctx.effect.type ~= "damage" or ctx.result.missed or ctx.result.cancelled or not target.data.max_shield then
    return
  end
  local weak = shields.weaknesses(target)
  for _, key in ipairs({ ctx.damage_type or false, ctx.element or false }) do
    if key and tablex.contains(weak, key) then
      shields.reveal(battle, target, key)
      if not shields.is_broken(target) and target.data.shield > 0 then
        target.data.shield = target.data.shield - 1
        if target.data.shield == 0 and target:alive() then break_battler(battle, target, ctx.user) end
      end
    end
  end
end

local function broken_bonus(ctx)
  if ctx.effect.type == "damage" and shields.is_broken(ctx.target) then
    ctx.multiplier = ctx.multiplier * settings.damage_bonus
  end
end

local function has_boost(b)
  return stats.resource(settings.resource) ~= nil and b:max_resource(settings.resource) > 0
end

--- How many points `b` may commit to a boost now.
function shields.boost_max(b)
  if not has_boost(b) then return 0 end
  return math.min(settings.max_spend, b:resource(settings.resource))
end

--- Commit `amount` boost points to an action. Nothing changes until it is
--- performed: then the points are paid and the extra hits or power apply.
function shields.boost(action, amount)
  amount = math.min(amount, shields.boost_max(action.user))
  if amount <= 0 then return end
  action.boost = amount
end

-- The action goes ahead: pay the committed points (at most what the user
-- has now) and apply them; `action.boost` becomes the amount paid.
local function apply_boost(action)
  local user = action.user
  local amount = math.min(action.boost, user:resource(settings.resource))
  action.boost = amount
  if amount <= 0 then return end
  user:change_resource(settings.resource, -amount)
  user.data.boosted = true
  if action.kind == "skill" and action.skill == Battle.settings.attack_skill then
    action.hits = (action.hits or action.source.hits) * (1 + amount)
  else
    action.multiplier = (action.multiplier or 1) * (1 + settings.power_per_point * amount)
  end
end

--- A rule layer with the break and boost rules. `extra` (a rule layer)
--- is added right after it as a layer of its own, so its hooks run too
--- instead of replacing these.
function shields.rules(extra)
  local layer = {
    layers = { extra },
    effect_hooks = {
      { phase = "after", fn = weakness_hit, priority = 0 },
      { phase = "modify", fn = broken_bonus, priority = 80 },
    },
  }

  function layer.setup(battle)
    for _, b in ipairs(battle.battlers) do init_shield(b) end
  end

  function layer.on_battler_added(_, b)
    init_shield(b)
  end

  function layer.on_round(battle, round)
    for _, b in ipairs(battle:living()) do
      if has_boost(b) then
        if round > 1 and not b.data.boosted then b:change_resource(settings.resource, settings.per_round) end
        b.data.boosted = nil
      end
    end
  end

  function layer.on_perform(_, action)
    if action.boost then apply_boost(action) end
  end

  function layer.on_turn_end(battle, b)
    if b.data.max_shield and b.data.shield == 0 and not shields.is_broken(b) and b:alive() then
      b.data.shield = b.data.max_shield
      battle:present("shield_restored", b)
    end
  end

  --- A revived enemy comes back with full shields (and no break state
  --- left over), so it can be broken again at once.
  function layer.on_revived(battle, b)
    if b.data.max_shield then
      b:remove_state(settings.state)
      if b.data.shield ~= b.data.max_shield then
        b.data.shield = b.data.max_shield
        battle:present("shield_restored", b)
      end
    end
  end

  --- Broken battlers act last (stable for everyone else).
  function layer.order(_, entries)
    local front, back = {}, {}
    for _, e in ipairs(entries) do
      local list = shields.is_broken(e.battler) and back or front
      list[#list + 1] = e
    end
    for i, e in ipairs(front) do entries[i] = e end
    for i, e in ipairs(back) do entries[#front + i] = e end
  end

  return layer
end

--- Side-view support: the boost gauge in the command menu (page left/right),
--- shield and weakness markers above enemies, and a break banner.
function shields.install_sideview()
  local sideview = require("fe.battle.sideview")
  local ui = require("fe.ui")
  sideview.modifier = {
    label = function() return i18n.t("fe.battle.boost") end,
    max = shields.boost_max,
    apply = shields.boost,
  }
  sideview.overlays.shields = function(p)
    for _, b in ipairs(p.battle:side("enemy")) do
      if b.data.max_shield and b.entity and b.entity:valid() then
        local x, y = b.entity:screen_position(1.9)
        local broken = shields.is_broken(b)
        ui.text(broken and i18n.t("fe.battle.broken_marker") or ("◆" .. b.data.shield), x - 12, y - 10,
          { color = broken and ui.theme.bad or ui.theme.heading })
        local known = {}
        for _, key in ipairs(shields.weaknesses(b)) do known[#known + 1] = shields.revealed(b, key) and key or "?" end
        ui.text(table.concat(known, " "), x - 30, y, { width = 60, align = "center", color = ui.theme.muted })
      end
    end
  end
  sideview.Presenter["break"] = function(presenter, target)
    presenter:show_banner(i18n.t("fe.battle.broken", { name = target:name() }), 0.9)
    presenter.rig:shake(0.15, 0.4)
    require("fe.task").wait(0.4)
  end
end

return shields
