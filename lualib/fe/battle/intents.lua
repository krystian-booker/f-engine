--- Enemy intents (telegraphs) as an optional rule layer: a battler whose
--- data names a planner (`"plan": "module.fn"`) decides its NEXT action as
--- soon as its previous one resolves (and when the battle starts, when it
--- joins or is revived), shows it, and carries it out when its turn comes.
---
--- ```lua
--- -- enemies.json: {"id": "hound", ..., "plan": "ai.hound.plan", "ai": "fe.battle.intents.execute"}
--- battle.rules = { layers = { intents.rules() } }   -- or intents.rules(extra)
--- -- ai/hound.lua
--- function M.plan(battle, me, previous)
---   local foe = battle:targets_for(me, "enemy")[1]
---   return { skill = "pounce", targets = { foe }, label = "Pounce", charge = 1 }
--- end
--- ```
--- plan(battle, me, previous) -> intent|nil, where intent is
--- {skill = id, targets? = {...}, kind? = "skill"|"defend", label?, marker?,
--- charge? = n, data?}; `previous` is the intent it replaces (nil at
--- first). Targets left out resolve when the action runs (all targets of an
--- `all_*` skill; a random one otherwise). The intent is stored at
--- `me.data.intent` with `charge_left` (counting down) and presented as
--- "intent" {battler, intent, previous, reason} (presenter:intent(payload,
--- battle)), reason being "planned", "replanned", "charging" (a charge turn
--- went by) or "cancelled" (intent nil).
---
--- `"ai": "fe.battle.intents.execute"` makes the battler carry out its
--- stored intent (planning one first if it has none, and re-planning if the
--- skill can no longer be used). `fengine_headless --check` only resolves
--- game modules in data, so a game usually names it through a one-line
--- module: ai/intents.lua `return require("fe.battle.intents")`, then
--- `"ai": "ai.intents.execute"`. `charge = n` turns the battler's next n
--- turns into upkeep-only charging turns (Battle:skips_turn), so the
--- countdown spans n + 1 turns of its clock. A single-target intent whose
--- target is knocked out, removed or swapped out before it runs is planned
--- again and presented again; it is never silently retargeted (an already
--- queued action follows the new intent). Any other action the battler
--- takes in its own turn (a forced one) also uses its intent up.
--- A battler that is knocked out loses its intent. Add the layer before
--- the battle runs.
local ai = require("fe.battle.ai")
local tablex = require("fe.tablex")

local intents = {}

--- Whether the battler plans its actions.
function intents.planner(b)
  return b.def ~= nil and b.def.plan ~= nil and b:takes_turns()
end

local function present(battle, me, previous, reason)
  battle:present("intent", { battler = me, intent = me.data.intent, previous = previous, reason = reason })
end

local function plan(battle, me, reason)
  local previous = me.data.intent
  local intent = ai.resolve(me.def.plan)(battle, me, previous)
  if intent then
    intent.charge_left = intent.charge or 0
    intent.charging = nil
  end
  me.data.intent = intent
  present(battle, me, previous, reason)
  return intent
end

--- Plan the battler's next action again now and present it ("replanned").
--- Returns the new intent (nil when the planner gives none or the battler
--- does not plan).
function intents.replan(battle, me)
  if not intents.planner(me) then return nil end
  return plan(battle, me, "replanned")
end

--- Drop the battler's intent (presented as "cancelled"). A queued action
--- for it is cancelled when it would start.
function intents.cancel(battle, me)
  local previous = me.data.intent
  if previous == nil then return end
  me.data.intent = nil
  present(battle, me, previous, "cancelled")
end

--- Turns of the battler's clock until its intent runs (1: its next turn).
function intents.turns_left(me)
  local intent = me.data.intent
  return intent and intent.charge_left + 1 or nil
end

-- The action carrying out an intent.
local function action_for(me, intent)
  if intent.kind and intent.kind ~= "skill" then
    return { kind = intent.kind, user = me, intent = intent }
  end
  return { kind = "skill", user = me, skill = intent.skill,
    targets = intent.targets and tablex.copy(intent.targets) or {}, intent = intent }
end

local function usable(battle, me, intent)
  if intent.kind and intent.kind ~= "skill" then return true end
  local skill = require("fe.data").find("skills", intent.skill)
  return skill ~= nil and battle:usable(me, skill)
end

--- AI entry ("ai": "fe.battle.intents.execute"): the battler's stored
--- intent as an action.
function intents.execute(battle, me)
  if not intents.planner(me) then return ai.default(battle, me) end
  local intent = me.data.intent or plan(battle, me, "planned")
  if intent and not usable(battle, me, intent) then intent = plan(battle, me, "replanned") end
  if not intent or not usable(battle, me, intent) then return ai.default(battle, me) end
  return action_for(me, intent)
end

-- Single-target intents aimed at `gone` are planned again.
local function target_gone(battle, gone)
  for _, b in ipairs(tablex.copy(battle.battlers)) do
    local intent = b.data.intent
    if b ~= gone and intent and intent.targets and #intent.targets == 1 and intent.targets[1] == gone
      and intents.planner(b) and not battle:outcome() then
      plan(battle, b, "replanned")
    end
  end
end

--- The rule layer. `extra` (a rule layer) is added right after it.
function intents.rules(extra)
  local layer = { layers = { extra } }

  function layer.on_start(battle)
    -- Presenters have seen these events by the time listeners run, so a
    -- newcomer's or a fallen target's telegraph follows its animation.
    battle.events:connect(function(name, payload)
      if battle.finished or battle.aborted then return end
      if name == "knocked_out" then
        if payload.data.intent then intents.cancel(battle, payload) end
        target_gone(battle, payload)
      elseif name == "battler_removed" then
        target_gone(battle, payload)
      elseif (name == "battler_added" or name == "revived") and intents.planner(payload)
        and not payload.data.intent and not battle:outcome() then
        plan(battle, payload, "planned")
      end
    end)
    for _, b in ipairs(tablex.copy(battle.battlers)) do
      if intents.planner(b) then plan(battle, b, "planned") end
    end
  end

  function layer.skip_turn(_, b)
    local intent = b.data.intent
    return intent ~= nil and intent.charge_left > 0 and intents.planner(b)
  end

  function layer.on_turn_start(_, b)
    local intent = b.data.intent
    if intent and intent.charge_left > 0 then intent.charging = true end
  end

  function layer.on_turn_end(battle, b)
    local intent = b.data.intent
    if intent and intent.charging then
      intent.charging = nil
      intent.charge_left = intent.charge_left - 1
      present(battle, b, intent, "charging")
    end
  end

  -- A queued action follows its battler's current intent: re-planned
  -- (new skill and targets), cancelled, or charging again.
  function layer.before_action(_, action)
    local planned = action.intent
    if not planned or action.reaction then return end
    local current = action.user.data.intent
    if current == planned then return end
    if not current or current.charge_left > 0 then
      action.cancelled = true
      return
    end
    local fresh = action_for(action.user, current)
    action.kind, action.skill, action.targets, action.intent = fresh.kind, fresh.skill, fresh.targets, current
  end

  function layer.on_perform(battle, action)
    local me = action.user
    if action.reaction or battle.current ~= me or not intents.planner(me) then return end
    me.data.intent = nil
    action.plans_next = true
  end

  function layer.after_action(battle, action)
    local me = action.user
    if action.plans_next and me.battle == battle and intents.planner(me) and me:alive() and not battle:outcome() then
      plan(battle, me, "planned")
    end
  end

  function layer.on_swap(battle, front)
    target_gone(battle, front)
  end

  return layer
end

return intents
