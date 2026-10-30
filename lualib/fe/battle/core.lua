--- Battle rules engine, independent of how a battle looks.
---
--- A Battle holds battlers and building blocks: deciding actions, paying
--- costs, resolving effects through fe.effects, turn upkeep, outcome checks
--- and rewards. A *scheduler* (fe.battle.schedulers.*) owns the turn loop
--- (ATB, round order), and a *presenter* (side-view, or a game's own)
--- shows what happens; both are plain Lua tables, so games
--- can write their own. The core never reads presentation state (entities,
--- sprites): presenters keep themselves in sync from events.
---
--- Rules are tables of optional hooks. A battle has several *rule layers*,
--- from general to specific: fe.battle.rules (the game's), the caller's
--- `rules`, the troop's script, layers added by kits (`battle:add_rules`,
--- e.g. fe.battle.limit) and finally `battle.rules`, the battle's own
--- table. A layer's `layers` list holds more layers, each added right after
--- it (more specific), so kits combine without overwriting each other's
--- hooks. Every layer's hooks run:
---   * notifications run in every layer, general to specific:
---     setup(battle) (before anything is shown), on_start(battle),
---     on_round(battle, n), order(battle, entries) (round_order: reorder the
---     round's {battler, action} entries in place), on_turn_start(battle, b),
---     on_turn_end(battle, b), before_action(battle, action) (may set
---     action.cancelled), on_perform(battle, action) (the action goes ahead:
---     not cancelled and its cost paid; it is shown and resolved next),
---     after_action(battle, action, results), on_knocked_out(battle, b),
---     on_revived(battle, b), on_battler_added(battle, b) (before presenters
---     see the newcomer), on_battler_removed(battle, b),
---     on_swap(battle, front, back), on_end(battle, outcome, rewards),
---     on_abort(battle) (the battle stopped without finishing, see
---     Battle:abort; it must not wait)
---   * outcome(battle) -> nil|false|"victory"|"defeat"|"escaped"|"draw" is
---     asked specific to general; the first non-nil answer wins, and false
---     means the battle goes on (the default checks are skipped)
---   * usable(battle, b, skill) -> false vetoes a skill in any layer
---     (Battle:usable; menus, AI and Battle:perform respect it)
---   * skip_turn(battle, b) -> true in any layer makes the battler's next
---     turn upkeep-only (Battle:skips_turn; every scheduler honours it; the
---     ambush/pre-emptive opening uses it)
---   * single-answer hooks use the most specific layer defining them:
---     player_controller(battle, b) -> action (default: presenter:command),
---     escape_chance(battle, b) -> 0..1,
---     rewards(battle) -> table, reaction(battle, b, action, results) ->
---     action|nil (a counter or other response by a battler the action
---     affected; reactions do not react; fe.battle.reactions has ready-made
---     ones), inventory(battle) -> store (see Battle.settings.inventory),
---     actions[kind](battle, action) -> results (custom commands)
---   * effect_hooks = {{phase, fn, priority?}, ...} become per-battle
---     fe.effects hooks (active only while this battle resolves effects)
---
--- Events presented (presenter methods, also emitted on battle.events):
--- start, opening (an ambush or pre-emptive strike), action, results,
--- state_tick, states_expired, knocked_out, revived, battler_added,
--- battler_removed, dismissed, reaction, defend, flee, escaped,
--- escape_failed, exp (experience gained per action, see
--- settings.exp_per_action) and finish come from the core; schedulers add
--- round and order (round_order); kits and games add their own.
---
--- Line-ups change mid-battle three ways: `battle:add` (reinforcements),
--- `battle:summon` (temporary battlers that leave by themselves) and
--- `battle:swap` with a living back-row battler from `battle:add_reserve`.
--- An enemy whose data gives `actions` (a number, or a function path
--- "module.fn" returning one per turn) acts that many times in each of its
--- turns (Octopath Traveler's bosses) through Battle:turn; every action is
--- presented. The core needs nothing from fe.rpg: items and rewards go through
--- Battle.settings.inventory and Battle.settings.party, which fe.battle
--- points at the RPG kit.
local Battler = require("fe.battle.battler")
local ai = require("fe.battle.ai")
local class = require("fe.class")
local data = require("fe.data")
local effects = require("fe.effects")
local signal = require("fe.signal")
local tablex = require("fe.tablex")

---@class Battle
local Battle = class("Battle")

Battle.settings = {
  attack_skill = "attack", -- skill id behind the basic Attack command, AI fallback and follow-ups
  mutual_ko = "defeat",    -- outcome when both sides are down at once: "defeat", "victory" or "draw"
  --- Item store for item actions and item/money rewards:
  --- {has(id, count?), remove(id, count) -> boolean, add(id, count), add_money(amount), list?(filter),
  --- settings?: {currency}}. nil: battles have no items (an `inventory` rule hook can still supply one).
  inventory = nil,
  --- Receiver of experience and job points: {gain_exp(amount, actors) -> levels,
  --- gain_jp?(amount, actors, secondary)}; `actors` are the party members who
  --- fought (Battle:participants). nil: no experience.
  party = nil,
  --- Job points go to the secondary class as well as the primary one.
  jp_secondary = true,
  --- fn(battle, action, results) -> experience the user gains for an action
  --- it performed (Triangle Strategy). Presented as "exp" {battler, amount,
  --- gains} after the after_action hooks. nil: experience only at victory.
  exp_per_action = nil,
  --- Pacing multiplier for presenters (timelines and waits divide by it);
  --- nil reads the player's `battle_speed` setting (fe.config). See Battle.speed.
  speed = nil,
  --- Scheduler constants (the schedulers' own `settings` override them):
  --- ATB gauges fill at (speed + rate_offset) / rate_divisor per second and
  --- start between 0 and start_fill full; round order multiplies a skill's
  --- `priority` by priority_scale before adding the speed key.
  atb = { rate_offset = 20, rate_divisor = 100, start_fill = 0.3 },
  priority_scale = 10000,
}

--- The battle's pacing multiplier: settings.speed, else the player's
--- battle_speed setting, else 1. Never below 0.1.
function Battle.speed()
  local speed = Battle.settings.speed
  if speed == nil then
    local ok, config = pcall(require, "fe.config")
    speed = ok and config.get("battle_speed") or 1
  end
  return math.max(0.1, tonumber(speed) or 1)
end

--- Damage to a guarding battler is halved (games can remove this hook).
effects.hook("modify", function(ctx)
  if ctx.battle and ctx.effect.type == "damage" and ctx.target.data and ctx.target.data.guarding then
    ctx.multiplier = ctx.multiplier * 0.5
  end
end, 50, "fe.battle.guard")

local phases = { "before", "hit", "calculate", "modify", "apply", "after" }

--- The side an opening disadvantages: an ambush catches the party, a
--- pre-emptive strike the enemy.
local opening_sides = { ambush = "party", preemptive = "enemy" }

--- The rule layer behind `opening`: every battler of the disadvantaged
--- side that is present when the battle starts skips its first turn
--- (upkeep only), whatever the scheduler. Presents "opening" {kind, side}
--- right after "start".
function Battle.opening_rules(kind)
  local side = opening_sides[kind]
  if not side then error("unknown opening '" .. tostring(kind) .. "' (ambush or preemptive)", 2) end
  return {
    on_start = function(battle)
      for _, b in ipairs(battle.battlers) do
        if b.side == side and b:takes_turns() then b.data.opening_skip = true end
      end
      battle:present("opening", { kind = kind, side = side })
    end,
    skip_turn = function(_, b) return b.data.opening_skip == true end,
    on_turn_end = function(_, b) b.data.opening_skip = nil end,
  }
end

---@param options {rules?: table, layers?: table[], scheduler: table, presenter?: table, random?: RandomStream, troop?: string, escapable?: boolean, opening?: "ambush"|"preemptive"}
function Battle:init(options)
  self.rules = options.rules or {}
  self.rule_layers = {}
  self.effect_hooks = {}
  for _, phase in ipairs(phases) do self.effect_hooks[phase] = {} end
  for _, layer in ipairs(options.layers or {}) do self:add_rules(layer) end
  self:add_rules(self.rules)
  self.scheduler = options.scheduler
  self.presenter = options.presenter
  self.random = options.random or fe.random.new()
  self.battlers = {}
  self.reserve = {} -- back-row battlers waiting outside the battle (see Battle:swap)
  self.fought = {}  -- party actors that took part, in order of arrival (Battle:participants)
  self.events = signal.new() -- (name, payload) for every presented event
  self.round = 0
  self.escapable = options.escapable ~= false
  self.troop = options.troop and data.get("troops", options.troop)
  self.opening = options.opening
  if self.opening then self:add_rules(Battle.opening_rules(self.opening)) end
  self.log = {}
  self.data = {}
end

-- Insert a layer and its `layers` at `at`; returns the position after them.
local function insert_layer(self, layer, at)
  if tablex.contains(self.rule_layers, layer) then return at end
  table.insert(self.rule_layers, at, layer)
  for _, h in ipairs(layer.effect_hooks or {}) do
    self:effect_hook(h.phase or h[1], h.fn or h[2], h.priority or h[3])
  end
  at = at + 1
  for _, child in ipairs(layer.layers or {}) do at = insert_layer(self, child, at) end
  return at
end

--- Add a rule layer (and the layers in its `layers` list, right after
--- it), more specific than the existing ones but still before the
--- battle's own `battle.rules`. Its `effect_hooks` become per-battle.
function Battle:add_rules(layer)
  local own = tablex.index_of(self.rule_layers, self.rules)
  insert_layer(self, layer, own or #self.rule_layers + 1)
  return layer
end

--- Run a notification hook in every rule layer (general to specific).
function Battle:notify(name, ...)
  for _, layer in ipairs(tablex.copy(self.rule_layers)) do
    local fn = layer[name]
    if fn then fn(self, ...) end
  end
end

--- The most specific rule layer's function for a single-answer hook.
function Battle:rule(name)
  for i = #self.rule_layers, 1, -1 do
    local fn = self.rule_layers[i][name]
    if fn then return fn end
  end
end

--- Custom action handler for an action kind (most specific layer).
function Battle:action_handler(kind)
  for i = #self.rule_layers, 1, -1 do
    local actions = self.rule_layers[i].actions
    if actions and actions[kind] then return actions[kind] end
  end
end

--- An fe.effects phase hook active only for this battle's effects.
function Battle:effect_hook(phase, fn, priority, id)
  local list = self.effect_hooks[phase]
  assert(list, "unknown effect phase '" .. tostring(phase) .. "'")
  return effects.insert_hook(list, fn, priority, id)
end

-- Remember a party actor as having fought (rewards go to them).
local function fought(self, battler)
  if battler.actor and battler:is_party() and not tablex.contains(self.fought, battler.actor) then
    self.fought[#self.fought + 1] = battler.actor
  end
end

---@param battler Battler|table spec for Battler()
function Battle:add(battler)
  if not Battler.is(battler, Battler) then battler = Battler(battler) end
  self.battlers[#self.battlers + 1] = battler
  battler.battle = self
  battler.data.down = not battler:alive() or nil
  fought(self, battler)
  if self.started and not self.finished then
    battler:reset_battle_resources()
    self:notify("on_battler_added", battler)
    self:present("battler_added", battler)
  end
  return battler
end

--- Take a battler out of the battle (deployment changes, retreats). One
--- removed before the battle starts did not take part.
function Battle:remove(battler)
  if not tablex.remove_value(self.battlers, battler) then return false end
  if self.started and not self.finished then
    self:notify("on_battler_removed", battler)
    self:present("battler_removed", battler)
  elseif not self.started and battler.actor then
    tablex.remove_value(self.fought, battler.actor)
  end
  battler.battle = nil
  return true
end

--- The party actors that fought: active members and reserves that took
--- part (swapped in, deployed, added mid-battle), in order of arrival.
--- Rewards go to them; everyone else in the roster counts as benched.
function Battle:participants()
  return tablex.copy(self.fought)
end

--- Add a temporary battler mid-battle: a summon, companion or guest. It is
--- computer-controlled unless `options.controlled`, and leaves the battle
--- (events "dismissed", then "battler_removed") after `options.turns` of
--- its own turns, when it is knocked out, or when `options.owner` falls.
---@param spec Battler|table spec for Battler()
---@param options? {turns?: integer, owner?: Battler, controlled?: boolean}
function Battle:summon(spec, options)
  options = options or {}
  local battler = Battler.is(spec, Battler) and spec or Battler(spec)
  battler.summon = { turns = options.turns, owner = options.owner }
  battler.auto = not options.controlled
  return self:add(battler)
end

--- Send a summoned battler away now.
function Battle:dismiss(battler)
  if battler.battle ~= self then return false end
  self:present("dismissed", battler)
  return self:remove(battler)
end

--- Add a back-row battler (Octopath Traveler II): it waits outside the
--- battle, keeping its state, until `battle:swap` brings it in.
---@param battler Battler|table spec for Battler()
function Battle:add_reserve(battler)
  if not Battler.is(battler, Battler) then battler = Battler(battler) end
  self.reserve[#self.reserve + 1] = battler
  return battler
end

--- The living back-row battler sharing a front-row battler's formation
--- slot (a knocked-out one cannot swap in).
function Battle:partner(battler)
  local slot = battler.position and battler.position.slot
  if not slot then return nil end
  return tablex.find(self.reserve, function(b)
    return b.side == battler.side and b.position and b.position.slot == slot and b:alive()
  end)
end

--- Trade a front-row battler for a living back-row one. The newcomer takes the
--- leaver's place in the line-up, formation slot and turn order (a swap
--- during the leaver's turn hands the turn over); the leaver waits in the
--- reserve with its state (buffs, per-battle data) intact. Presenters get
--- "battler_removed" for the leaver and "battler_added" for the newcomer;
--- rule layers get on_swap(battle, front, back).
function Battle:swap(front, back)
  local i = tablex.index_of(self.battlers, front)
  local j = tablex.index_of(self.reserve, back)
  assert(i and j, "swap needs a battler in the battle and one in its reserve")
  assert(back:alive(), "a knocked-out back-row battler cannot swap in")
  self.battlers[i], self.reserve[j] = back, front
  front.position, back.position = back.position, front.position
  back.battle, front.battle = self, nil
  back.data.down = not back:alive() or nil
  fought(self, back)
  if self.current == front then self.current = back end
  if self.scheduler and self.scheduler.replace then self.scheduler.replace(self, front, back) end
  if self.started and not self.finished then
    self:present("battler_removed", front)
    self:present("battler_added", back)
    self:notify("on_swap", front, back)
  end
  return back
end

--- Add the active party, a troop's members and optionally back-row
--- battlers (`reserve_members`, sharing slots with the front row in order).
function Battle:populate(party_members, troop_id, reserve_members)
  for i, actor in ipairs(party_members) do
    self:add({ actor = actor, side = "party", position = { slot = i } })
  end
  for i, actor in ipairs(reserve_members or {}) do
    self:add_reserve({ actor = actor, side = "party", position = { slot = i } })
  end
  if troop_id then
    for i, member in ipairs(data.get("troops", troop_id).members) do
      self:add({ enemy = member.enemy, side = "enemy", position = member.position or { slot = i } })
    end
  end
end

--- The item store this battle uses (the `inventory` rule hook, else
--- Battle.settings.inventory), or nil.
function Battle:inventory()
  local hook = self:rule("inventory")
  if hook then return hook(self) end
  return Battle.settings.inventory
end

--- Show an event: the presenter may animate (and wait); hooks observe it.
function Battle:present(name, payload)
  if self.presenter and self.presenter[name] then
    self.presenter[name](self.presenter, payload, self)
  end
  self.events:emit(name, payload)
end

--- Present every knock-out and revival since the last call exactly once,
--- whatever caused it (skills, states, terrain, pushes, scripts).
--- Summons that are knocked out, or whose owner is, leave the battle.
function Battle:settle()
  for _, b in ipairs(tablex.copy(self.battlers)) do
    local down = not b:alive()
    if down and not b.data.down then
      b.data.down = true
      self:notify("on_knocked_out", b)
      self:present("knocked_out", b)
    elseif not down and b.data.down then
      b.data.down = nil
      self:notify("on_revived", b)
      self:present("revived", b)
    end
  end
  for _, b in ipairs(tablex.copy(self.battlers)) do
    local summon = b.summon
    if summon and (not b:alive() or (summon.owner and not summon.owner:alive())) then self:dismiss(b) end
  end
end

function Battle:side(side, include_dead)
  return tablex.filter(self.battlers, function(b)
    return b.side == side and (include_dead or b:alive())
  end)
end

--- Battlers on other sides.
function Battle:opponents(battler, include_dead)
  return tablex.filter(self.battlers, function(b)
    return b.side ~= battler.side and (include_dead or b:alive())
  end)
end

function Battle:allies(battler, include_dead)
  return self:side(battler.side, include_dead)
end

function Battle:living()
  return tablex.filter(self.battlers, function(b) return b:alive() end)
end

--- Candidate targets for a skill target kind.
function Battle:targets_for(user, kind)
  if kind == "enemy" or kind == "all_enemies" or kind == "random_enemy" then
    return self:opponents(user)
  elseif kind == "ally" or kind == "all_allies" then
    return self:allies(user)
  elseif kind == "dead_ally" then
    return tablex.filter(self:allies(user, true), function(b) return not b:alive() end)
  elseif kind == "self" then
    return { user }
  elseif kind == "any" or kind == "everyone" then
    return self:living()
  end
  return {}
end

--- Whether `battler` may use `skill` now: it can pay the cost and no rule
--- layer's `usable(battle, battler, skill)` hook answers false. When not,
--- the second value says why: "unaffordable" or "vetoed".
---@return boolean, string?
function Battle:usable(battler, skill)
  if not battler:can_pay(skill.cost) then return false, "unaffordable" end
  for _, layer in ipairs(self.rule_layers) do
    if layer.usable and layer.usable(self, battler, skill) == false then return false, "vetoed" end
  end
  return true
end

--- Whether a rule layer's `skip_turn(battle, battler)` hook makes the
--- battler's coming turn upkeep-only (no decision, no action).
function Battle:skips_turn(battler)
  for _, layer in ipairs(self.rule_layers) do
    if layer.skip_turn and layer.skip_turn(self, battler) == true then return true end
  end
  return false
end

--- Targets for a skill whose chosen targets are all gone by the time it
--- resolves, keeping the target kind: every living opponent or ally for
--- the `all_*` and `everyone` kinds, the user for `self`, else one random
--- candidate (the only case that draws from the battle's random stream).
function Battle:retarget(user, kind)
  if kind == "self" then return { user } end
  local pool = self:targets_for(user, kind)
  if #pool == 0 then return {} end
  if kind == "all_enemies" or kind == "all_allies" or kind == "everyone" then return pool end
  return { pool[self.random:int(1, #pool)] }
end

--- Ask the battler's controller for an action (player input or AI).
function Battle:decide(battler)
  if battler:is_party() and not battler.auto then
    local controller = self:rule("player_controller")
    if controller then return controller(self, battler) end
    if self.presenter and self.presenter.command then
      return self.presenter:command(battler, self)
    end
  end
  return ai.for_battler(battler)(self, battler)
end

--- The skill behind the basic Attack command.
function Battle.attack_skill()
  return data.find("skills", Battle.settings.attack_skill)
end

local function source_of(action)
  if action.skill then return data.get("skills", action.skill) end
  if action.item then
    local item = data.get("items", action.item)
    local use = item.use or {}
    return { id = item.id, name = item.name, effects = use.effects or {}, target = use.target,
      timeline = use.timeline, item = true, always_hit = true, consume = use.consume ~= false }
  end
end

-- Pay for an action; false and the reason ("no_inventory", "unaffordable"
-- or "vetoed") when it cannot be paid or a rule layer vetoes the skill
-- (nothing changes).
local function pay(battle, user, action, source)
  if action.kind == "item" then
    local inventory = battle:inventory()
    if not inventory then return false, "no_inventory" end
    if not inventory.has(action.item) then return false, "unaffordable" end
    if source.consume then inventory.remove(action.item, 1) end
    return true
  end
  local ok, reason = battle:usable(user, source)
  if not ok then return false, reason end
  user:pay(source.cost)
  return true
end

-- Resolve a random-target skill one hit at a time, each against a random
-- living foe at that moment.
local function resolve_random(self, action, source)
  local results, hit = {}, {}
  for _ = 1, action.hits or source.hits do
    local pool = self:opponents(action.user)
    if #pool == 0 then break end
    local target = pool[self.random:int(1, #pool)]
    if not tablex.contains(hit, target) then hit[#hit + 1] = target end
    for _, r in ipairs(effects.resolve({ user = action.user, targets = { target }, source = source, battle = self,
      random = self.random, hits = 1, multiplier = action.multiplier })) do
      results[#results + 1] = r
    end
  end
  action.targets = hit
  return results
end

-- Responses (counters...) from battlers the action affected.
local function reactions(self, action, results)
  local respond = self:rule("reaction")
  if not respond or action.reaction then return end
  local seen = {}
  for _, r in ipairs(results or {}) do
    local b = r.target
    if b and not seen[b] and b ~= action.user and b:can_act() and not self:outcome() then
      seen[b] = true
      local response = respond(self, b, action, results)
      if response then
        response.reaction = true
        response.user = response.user or b
        self:present("reaction", { battler = b, action = response, cause = action })
        self:perform(response)
      end
    end
  end
end

--- Execute an action table:
---   {kind = "skill", user = b, skill = id, targets = {...}, hits?, multiplier?}
---   {kind = "item", user = b, item = id, targets = {...}}
---   {kind = "defend" | "flee" | <custom>, user = b}
--- Returns nil (and changes nothing) and the reason when the user cannot
--- act ("cannot_act"), a before_action hook cancels it ("cancelled"), the
--- battle has no item store ("no_inventory"), its cost cannot be paid
--- ("unaffordable") or a `usable` hook vetoes the skill ("vetoed");
--- otherwise on_perform hooks run and it returns the results, with
--- `action.performed = true`.
---@return table[]|nil results, string? refusal
function Battle:perform(action)
  local user = action.user
  if not user:can_act() then return nil, "cannot_act" end
  action.kind = action.kind or (action.item and "item" or "skill")
  self:notify("before_action", action)
  if action.cancelled then return nil, "cancelled" end
  local own_turn = self.current == user
  local serial_before = own_turn and (user.state_serial or 0)
  if own_turn then user.acting = true end
  local results
  local custom = self:action_handler(action.kind)
  local source = not custom and action.kind ~= "defend" and action.kind ~= "flee" and source_of(action)
  if source then
    local paid, reason = pay(self, user, action, source)
    if not paid then
      user.acting = nil
      return nil, reason
    end
    action.source = source
  end
  self:notify("on_perform", action)
  if custom then
    results = custom(self, action)
  elseif action.kind == "defend" then
    user.data.guarding = true
    self:present("defend", action)
  elseif action.kind == "flee" then
    self:present("flee", action)
    local chance_fn = self:rule("escape_chance")
    local chance = chance_fn and chance_fn(self, user) or 0.5
    if self.escapable and self.random:float() < chance then
      self.escaped = true
      self:present("escaped", action)
    else
      self:present("escape_failed", action)
    end
  else
    if source.target == "random_enemy" then
      action.targets = self:opponents(user)
      self:present("action", action)
      results = resolve_random(self, action, source)
    else
      local targets = tablex.filter(action.targets or {}, function(t)
        return t:alive() or source.target == "dead_ally"
      end)
      if #targets == 0 and source.target ~= "dead_ally" then
        targets = self:retarget(user, source.target)
      end
      action.targets = targets
      self:present("action", action)
      results = effects.resolve({ user = user, targets = targets, source = source, battle = self, random = self.random,
        hits = action.hits, multiplier = action.multiplier })
    end
    self:present("results", { action = action, results = results })
  end
  action.performed = true
  if own_turn then
    user.acting = nil
    -- States the user gained during its own action (by the add_state
    -- serial, so a state re-applied with the same duration counts too) are
    -- not counted down at the end of this turn.
    local fresh = user.data.fresh_states or {}
    for id, serial in pairs(user.state_added or {}) do
      if serial > serial_before and user.state_turns[id] ~= nil then fresh[id] = true end
    end
    user.data.fresh_states = fresh
  end
  self:settle()
  self.log[#self.log + 1] = action
  self:notify("after_action", action, results)
  local per_action = Battle.settings.exp_per_action
  if per_action and user.actor and user:is_party() then
    local amount = per_action(self, action, results)
    if amount and amount > 0 then
      local gains = user.actor:gain_exp(amount)
      self:present("exp", { battler = user, amount = amount, gains = gains })
    end
  end
  reactions(self, action, results)
  return results
end

--- Start-of-turn upkeep: state effects (poison...) and on_turn_start. Runs
--- for every battler whose turn comes up, including ones that cannot act.
function Battle:turn_start(battler)
  self.current = battler
  battler.data.guarding = nil
  for _, id in ipairs(battler:states()) do
    local state = data.find("states", id)
    if state and #state.on_turn > 0 and battler:alive() then
      local results = effects.resolve({ user = battler, targets = { battler }, source = state,
        effects = state.on_turn, battle = self, random = self.random, environmental = true })
      self:present("state_tick", { battler = battler, state = state, results = results })
    end
  end
  self:settle()
  self:notify("on_turn_start", battler)
end

--- End-of-turn upkeep: states and buffs count down (except those gained
--- during this battler's own action), then on_turn_end. A summon with a
--- turn limit leaves once it has used them up.
function Battle:turn_end(battler)
  local expired = battler:tick_states(battler.data.fresh_states)
  battler.data.fresh_states = nil
  battler:tick_buffs()
  if #expired > 0 then self:present("states_expired", { battler = battler, states = expired }) end
  self:notify("on_turn_end", battler)
  self:settle()
  if self.current == battler then self.current = nil end
  local summon = battler.summon
  if summon and summon.turns and battler.battle == self then
    summon.turns = summon.turns - 1
    if summon.turns <= 0 then self:dismiss(battler) end
  end
end

--- A whole turn for one battler: upkeep, decision, actions, upkeep. Pass
--- `action = false` for a turn without an action (upkeep only); nil asks
--- the battler's controller. A turn a rule layer skips (Battle:skips_turn)
--- is upkeep only too. A battler with several actions per turn
--- (Battler:actions_per_turn) uses `action` first and decides the others
--- one at a time, while it can still act and the battle goes on. When the
--- controller swaps the battler for its back-row partner, the partner acts
--- and ends the turn. Returns the last action's results and the last
--- action taken.
function Battle:turn(battler, action)
  self:turn_start(battler)
  local results, taken
  if action ~= false and not self:skips_turn(battler) then
    for i = 1, battler:actions_per_turn(self) do
      local actor = self.current or battler
      if not actor:can_act() or actor.battle ~= self or self:outcome() then break end
      local chosen = action
      if i > 1 or chosen == nil then chosen = self:decide(actor) end
      if chosen then
        results = self:perform(chosen)
        taken = chosen
      end
    end
  end
  self:turn_end(self.current or battler)
  return results, taken
end

--- Current outcome or nil while the battle continues.
function Battle:outcome()
  for i = #self.rule_layers, 1, -1 do
    local fn = self.rule_layers[i].outcome
    local result = fn and fn(self)
    if result ~= nil then return result or nil end
  end
  if self.escaped then return "escaped" end
  local party_down, enemies_down = #self:side("party") == 0, #self:side("enemy") == 0
  if party_down and enemies_down then return Battle.settings.mutual_ko end
  if party_down then return "defeat" end
  if enemies_down then return "victory" end
  return nil
end

--- Default rewards: the fallen opponents' exp, money, jp and drops.
function Battle:default_rewards()
  local rewards = { exp = 0, money = 0, jp = 0, items = {} }
  for _, b in ipairs(self.battlers) do
    if b.def and b.side ~= "party" and not b:alive() then
      local r = b.def.rewards
      rewards.exp = rewards.exp + (r.exp or 0)
      rewards.money = rewards.money + (r.money or 0)
      rewards.jp = rewards.jp + (r.jp or 0)
      for _, drop in ipairs(r.drops or {}) do
        if self.random:float() < drop.chance then rewards.items[#rewards.items + 1] = drop.item end
      end
    end
  end
  return rewards
end

--- Grant rewards through Battle.settings.party (experience and job points
--- to the party members who fought, Battle:participants; the party gives
--- the rest of the roster its reserve share) and the battle's inventory
--- (money, items); returns a summary for the victory screen.
function Battle:grant(rewards)
  local party, inventory = Battle.settings.party, self:inventory()
  if party then
    local fighters = self:participants()
    rewards.levels = party.gain_exp(rewards.exp, fighters)
    if rewards.jp > 0 then
      if party.gain_jp then
        party.gain_jp(rewards.jp, fighters, Battle.settings.jp_secondary)
      else
        for _, actor in ipairs(fighters) do
          if actor:alive() then actor:gain_jp(rewards.jp) end
        end
      end
    end
  end
  if inventory then
    inventory.add_money(rewards.money)
    for _, id in ipairs(rewards.items) do inventory.add(id, 1) end
  end
  return rewards
end

--- Run the battle in the current task; returns the outcome and rewards.
--- Battle-only resources start at their start value and are reset again
--- at the end; per-battle effect hooks stop applying once it is over.
function Battle:run()
  for _, b in ipairs(self.battlers) do b:reset_battle_resources() end
  for _, b in ipairs(self.reserve) do b:reset_battle_resources() end
  self:notify("setup")
  self.started = true
  self:present("start", self)
  self:notify("on_start")
  self.scheduler.run(self)
  local outcome = self:outcome() or "draw"
  local rewards
  if outcome == "victory" then
    local custom = self:rule("rewards")
    rewards = custom and custom(self) or self:grant(self:default_rewards())
  end
  for _, list in ipairs({ self.battlers, self.reserve }) do
    for _, b in ipairs(list) do
      if b.actor then
        b:clear_states("battle_end")
        b:reset_battle_resources()
      end
    end
  end
  self:notify("on_end", outcome, rewards)
  self:present("finish", { outcome = outcome, rewards = rewards })
  self.finished = true
  for _, phase in ipairs(phases) do self.effect_hooks[phase] = {} end
  return outcome, rewards
end

--- End a battle that stopped without finishing (its task raised or was
--- cancelled; fe.battle.start calls this): per-battle effect hooks stop,
--- actors lose their battle-only resources and "battle_end" states as at a
--- normal end, and every rule layer's `on_abort(battle)` runs. There is no
--- outcome, no rewards, no
--- `on_end` and no `finish` event. Does nothing once the battle finished
--- or was aborted. Must not wait.
function Battle:abort()
  if self.finished or self.aborted then return end
  self.aborted = true
  for _, phase in ipairs(phases) do self.effect_hooks[phase] = {} end
  for _, list in ipairs({ self.battlers, self.reserve }) do
    for _, b in ipairs(list) do
      if b.actor then
        b:clear_states("battle_end")
        b:reset_battle_resources()
      end
    end
  end
  self:notify("on_abort")
end

return Battle
