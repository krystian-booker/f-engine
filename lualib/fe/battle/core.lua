--- Battle rules engine, independent of how a battle looks.
---
--- A Battle holds battlers and building blocks: deciding actions, paying
--- costs, resolving effects through fe.effects, turn upkeep, outcome checks
--- and rewards. A *scheduler* (fe.battle.schedulers.*) owns the turn loop
--- (ATB, round order, initiative), and a *presenter* (side-view, tactical,
--- or a game's own) shows what happens; both are plain Lua tables, so games
--- can write their own.
---
--- Rules are hooks on `battle.rules`, all optional:
---   player_controller(battle, b) -> action (default: presenter:command),
---   setup(battle) (before anything is shown), on_start(battle), on_round(battle, n), on_turn_start(battle, b),
---   on_turn_end(battle, b), before_action(battle, action),
---   after_action(battle, action, results), on_end(battle, outcome),
---   outcome(battle) -> nil|"victory"|"defeat"|"escaped",
---   escape_chance(battle, b) -> 0..1, rewards(battle) -> table,
---   actions[kind](battle, action) -> results  (custom commands)
local Battler = require("fe.battle.battler")
local ai = require("fe.battle.ai")
local class = require("fe.class")
local data = require("fe.data")
local effects = require("fe.effects")
local signal = require("fe.signal")
local tablex = require("fe.tablex")

---@class Battle
local Battle = class("Battle")

--- Damage to a guarding battler is halved (games can remove this hook).
effects.hook("modify", function(ctx)
  if ctx.battle and ctx.effect.type == "damage" and ctx.target.data and ctx.target.data.guarding then
    ctx.multiplier = ctx.multiplier * 0.5
  end
end, 50)

---@param options {rules?: table, scheduler: table, presenter?: table, random?: RandomStream, troop?: string, escapable?: boolean}
function Battle:init(options)
  self.rules = options.rules or {}
  self.scheduler = options.scheduler
  self.presenter = options.presenter
  self.random = options.random or fe.random.new()
  self.battlers = {}
  self.events = signal.new() -- (name, payload) for every presented event
  self.round = 0
  self.escapable = options.escapable ~= false
  self.troop = options.troop and data.get("troops", options.troop)
  self.log = {}
  self.data = {}
end

---@param battler Battler|table spec for Battler()
function Battle:add(battler)
  if not Battler.is(battler, Battler) then battler = Battler(battler) end
  self.battlers[#self.battlers + 1] = battler
  battler.battle = self
  return battler
end

--- Add the active party and a troop's members.
function Battle:populate(party_members, troop_id)
  for i, actor in ipairs(party_members) do
    self:add({ actor = actor, side = "party", position = { slot = i } })
  end
  if troop_id then
    for i, member in ipairs(data.get("troops", troop_id).members) do
      self:add({ enemy = member.enemy, side = "enemy", position = member.position or { slot = i } })
    end
  end
end

--- Show an event: the presenter may animate (and wait); hooks observe it.
function Battle:present(name, payload)
  if self.presenter and self.presenter[name] then
    self.presenter[name](self.presenter, payload, self)
  end
  self.events:emit(name, payload)
end

function Battle:side(side, include_dead)
  return tablex.filter(self.battlers, function(b)
    return b.side == side and (include_dead or b:alive())
  end)
end

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

--- Ask the battler's controller for an action (player input or AI).
function Battle:decide(battler)
  if battler:is_party() and not battler.auto then
    if self.rules.player_controller then
      return self.rules.player_controller(self, battler)
    end
    if self.presenter and self.presenter.command then
      return self.presenter:command(battler, self)
    end
  end
  return ai.for_battler(battler)(self, battler)
end

local function source_of(action)
  if action.skill then return data.get("skills", action.skill) end
  if action.item then
    local item = data.get("items", action.item)
    local use = item.use or {}
    return { id = item.id, name = item.name, effects = use.effects or {}, target = use.target,
      timeline = use.timeline, item = true, always_hit = true }
  end
end

--- Execute an action table:
---   {kind = "skill", user = b, skill = id, targets = {...}, hits?, multiplier?}
---   {kind = "item", user = b, item = id, targets = {...}}
---   {kind = "defend" | "flee" | <custom>, user = b}
---@return table[]|nil results
function Battle:perform(action)
  local user = action.user
  if not user:can_act() then return nil end
  action.kind = action.kind or (action.item and "item" or "skill")
  if self.rules.before_action then self.rules.before_action(self, action) end
  if action.cancelled then return nil end
  local results
  local custom = self.rules.actions and self.rules.actions[action.kind]
  if custom then
    results = custom(self, action)
  elseif action.kind == "defend" then
    user.data.guarding = true
    self:present("defend", action)
  elseif action.kind == "flee" then
    self:present("flee", action)
    local chance = self.rules.escape_chance and self.rules.escape_chance(self, user) or 0.5
    if self.escapable and self.random:float() < chance then
      self.escaped = true
      self:present("escaped", action)
    else
      self:present("escape_failed", action)
    end
  else
    local source = source_of(action)
    if action.kind == "item" then
      if not require("fe.rpg.inventory").remove(action.item, 1) then return nil end
    else
      if not user:can_pay(source.cost) then return nil end
      user:pay(source.cost)
    end
    action.source = source
    local targets = tablex.filter(action.targets or {}, function(t)
      return t:alive() or source.target == "dead_ally"
    end)
    if #targets == 0 and source.target ~= "dead_ally" and not action.cell then
      local pool = self:targets_for(user, source.target)
      targets = #pool > 0 and { pool[self.random:int(1, #pool)] } or {}
    end
    if source.target == "random_enemy" then
      local pool = self:opponents(user)
      targets = {}
      for _ = 1, source.hits do targets[#targets + 1] = pool[self.random:int(1, math.max(1, #pool))] end
    end
    action.targets = targets
    self:present("action", action)
    results = effects.resolve({ user = user, targets = targets, source = source, battle = self, random = self.random,
      hits = source.target == "random_enemy" and 1 or action.hits, multiplier = action.multiplier })
    self:present("results", { action = action, results = results })
    for _, r in ipairs(results) do
      if r.knocked_out and r.type == "damage" then
        self:present("knocked_out", r.target)
      end
    end
  end
  self.log[#self.log + 1] = action
  if self.rules.after_action then self.rules.after_action(self, action, results) end
  return results
end

--- Start-of-turn upkeep: state effects (poison...) and expiry.
function Battle:turn_start(battler)
  battler.data.guarding = nil
  for _, id in ipairs(battler:states()) do
    local state = data.find("states", id)
    if state and #state.on_turn > 0 and battler:alive() then
      local results = effects.resolve({ user = battler, targets = { battler }, source = state,
        effects = state.on_turn, battle = self, random = self.random })
      self:present("state_tick", { battler = battler, state = state, results = results })
    end
  end
  if self.rules.on_turn_start then self.rules.on_turn_start(self, battler) end
end

function Battle:turn_end(battler)
  local expired = battler:tick_states()
  battler:tick_buffs()
  if #expired > 0 then self:present("states_expired", { battler = battler, states = expired }) end
  if self.rules.on_turn_end then self.rules.on_turn_end(self, battler) end
end

--- A whole turn for one battler: upkeep, decision, action, upkeep.
function Battle:turn(battler, action)
  self:turn_start(battler)
  local results
  if battler:can_act() and not self:outcome() then
    action = action or self:decide(battler)
    if action then results = self:perform(action) end
  end
  self:turn_end(battler)
  return results
end

--- Current outcome or nil while the battle continues.
function Battle:outcome()
  if self.rules.outcome then
    local result = self.rules.outcome(self)
    if result then return result end
  end
  if self.escaped then return "escaped" end
  if #self:side("party") == 0 then return "defeat" end
  if #self:side("enemy") == 0 then return "victory" end
  return nil
end

--- Default rewards: the troop's enemies' exp, money, jp and drops.
function Battle:default_rewards()
  local rewards = { exp = 0, money = 0, jp = 0, items = {} }
  for _, b in ipairs(self.battlers) do
    if b.def and not b:alive() then
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

--- Grant rewards to the party; returns a summary for the victory screen.
function Battle:grant(rewards)
  local party = require("fe.rpg.party")
  local inventory = require("fe.rpg.inventory")
  rewards.levels = party.gain_exp(rewards.exp)
  inventory.add_money(rewards.money)
  for _, id in ipairs(rewards.items) do inventory.add(id, 1) end
  if rewards.jp > 0 then
    for _, actor in ipairs(party.alive()) do actor:gain_jp(rewards.jp) end
  end
  return rewards
end

--- Run the battle in the current task; returns the outcome and rewards.
function Battle:run()
  if self.rules.setup then self.rules.setup(self) end
  self:present("start", self)
  if self.rules.on_start then self.rules.on_start(self) end
  self.scheduler.run(self)
  local outcome = self:outcome() or "draw"
  local rewards
  if outcome == "victory" then
    rewards = self.rules.rewards and self.rules.rewards(self) or self:grant(self:default_rewards())
  end
  for _, b in ipairs(self.battlers) do
    if b.actor then b:clear_states("battle_end") end
  end
  if self.rules.on_end then self.rules.on_end(self, outcome, rewards) end
  self:present("finish", { outcome = outcome, rewards = rewards })
  return outcome, rewards
end

return Battle
