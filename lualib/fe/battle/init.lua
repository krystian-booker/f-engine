--- Starting battles. A battle combines rules (fe.battle.core), a turn
--- scheduler and a presentation. Defaults: side-view presentation with ATB;
--- games pick others:
---
--- ```lua
--- battle.settings.scheduler = function() return require("fe.battle.schedulers.round_order").new() end
--- battle.settings.presentation = "fe.battle.sideview"      -- or "fe.battle.tactical", or a game module
--- battle.rules.on_start = function(b) ... end              -- game rules
--- local outcome = battle.start{troop = "slimes"}           -- inside a task
--- ```
local Battle = require("fe.battle.core")
local config = require("fe.config")
local party = require("fe.rpg.party")
local signal = require("fe.signal")
local data = require("fe.data")

local battle = {
  Battle = Battle,
  settings = {
    presentation = "fe.battle.sideview",
    scheduler = function()
      return require("fe.battle.schedulers.atb").new({ mode = config.get("battle_wait") and "wait" or "active",
        speed = config.get("battle_speed") or 1 })
    end,
  },
  --- Rules copied into every battle (see fe.battle.core for hook names).
  rules = {},
  started = signal.new(),  -- (battle)
  finished = signal.new(), -- (battle, outcome, rewards)
  active = nil,
}

--- Called after a defeat that the caller did not handle; default shows the
--- game-over screen if the game provides scenes.game_over, else reloads the
--- title through fe.game.reset when available.
function battle.on_defeat()
  local ok, screen = pcall(require, "scenes.game_over")
  if ok and screen.open then screen.open() end
end

--- Stage map when neither the caller nor the troop names one: the current
--- field map's "battle_stage" prop, else a map called "battle".
function battle.default_stage()
  local world = require("fe.field").world
  return world and world:props().battle_stage or "battle"
end

--- Run a battle in the current task; returns outcome, rewards.
---@param options {troop?: string, stage?: string, music?: string, escapable?: boolean, scheduler?: table, rules?: table, presentation?: string, setup?: fun(b: table), allow_defeat?: boolean}
function battle.start(options)
  local troop = options.troop and data.get("troops", options.troop)
  local rules = {}
  for k, v in pairs(battle.rules) do rules[k] = v end
  for k, v in pairs(options.rules or {}) do rules[k] = v end
  if troop and troop.script then
    for k, v in pairs(require(troop.script)) do rules[k] = v end
  end
  local presentation = require(options.presentation or battle.settings.presentation)
  local b = Battle({
    rules = rules,
    scheduler = options.scheduler or (presentation.scheduler and presentation.scheduler()) or battle.settings.scheduler(),
    troop = options.troop,
    escapable = options.escapable ~= false and (not troop or troop.escapable),
  })
  b.presenter = presentation.new(b, {
    stage = options.stage or (troop and troop.stage) or battle.default_stage(),
    music = options.music or (troop and troop.music),
  })
  if options.setup then
    options.setup(b)
  else
    b:populate(party.members(), options.troop)
  end
  battle.active = b
  battle.started:emit(b)
  local outcome, rewards = b:run()
  battle.active = nil
  battle.finished:emit(b, outcome, rewards)
  if outcome == "defeat" and not options.allow_defeat then
    battle.on_defeat()
  end
  return outcome, rewards
end

--- A random or scripted encounter with a troop.
function battle.encounter(troop, options)
  options = options or {}
  options.troop = troop
  return battle.start(options)
end

return battle
