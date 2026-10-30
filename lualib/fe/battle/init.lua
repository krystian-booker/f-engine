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
---
--- Rule layers, general to specific (see fe.battle.core for how hooks
--- combine): `battle.rules` (the game's), `options.rules`, the troop's
--- `script` module, then whatever the presentation adds (tactical
--- objectives). Every layer's notification hooks run; none overwrites
--- another.
---
--- A presentation module provides `new(battle, options) -> presenter` and
--- optionally `scheduler() -> scheduler` (its default scheduler) and
--- `populate(battle, options)` (adds battlers instead of party + troop).
--- A presenter may have `abort(battle)`, called when the battle raises or
--- is cancelled before it finished: it tears down at once (it must not
--- wait) whatever the presentation set up, so nothing outlives the battle.
---
--- This module connects battles to the RPG kit: items and rewards use the
--- party's inventory, experience goes to the party (Battle.settings
--- inventory and party), and with `settings.back_row` the reserve members
--- wait in the back row (`battle:swap`). `battle.mock(fn)` runs a battle
--- and rolls the game state it changed back afterwards (see battle.mock).
local Battle = require("fe.battle.core")
local config = require("fe.config")
local inventory = require("fe.rpg.inventory")
local party = require("fe.rpg.party")
local signal = require("fe.signal")
local data = require("fe.data")
local state = require("fe.state")
local tablex = require("fe.tablex")
local task = require("fe.task")

Battle.settings.inventory = Battle.settings.inventory or inventory
Battle.settings.party = Battle.settings.party or party

local battle = {
  Battle = Battle,
  settings = {
    presentation = "fe.battle.sideview",
    scheduler = function()
      return require("fe.battle.schedulers.atb").new({ mode = config.get("battle_wait") and "wait" or "active",
        speed = config.get("battle_speed") or 1 })
    end,
    --- Reserve party members join as back-row battlers (Octopath Traveler II
    --- front/back rows), sharing formation slots with the active members.
    back_row = false,
    --- fe.state sections a mock battle keeps as they are when it ends
    --- instead of rolling them back: the clock (playtime), the time of day
    --- and the player's field location carry on.
    mock_keep = { "clock", "daynight", "field" },
  },
  --- The game's rule layer, used by every battle (see fe.battle.core).
  rules = {},
  started = signal.new(),  -- (battle)
  finished = signal.new(), -- (battle, outcome, rewards)
  active = nil,
  --- True while battle.mock runs.
  mocking = false,
}

--- Called after a defeat that the caller did not allow (`allow_defeat`).
--- The default opens the game's `scenes.game_over` module (its `open()`)
--- when the game has one, and does nothing otherwise. Replace it for other
--- behaviour.
function battle.on_defeat()
  if fe.fs.exists("scenes/game_over.lua") then
    local screen = require("scenes.game_over")
    if screen.open then screen.open() end
  end
end

--- Stage map when neither the caller nor the troop names one: the current
--- field map's "battle_stage" prop, else a map called "battle".
function battle.default_stage()
  local world = require("fe.field").world
  return world and world:props().battle_stage or "battle"
end

-- The battle's task raised or was cancelled before it finished: undo what
-- it left behind (Battle:abort, then the presenter's `abort`). Failures here
-- are logged, so the original error is the one that propagates.
local function abort(b)
  local ok, err = pcall(b.abort, b)
  if not ok then fe.log.error("Aborting the battle failed: " .. tostring(err)) end
  if b.presenter and b.presenter.abort then
    ok, err = pcall(b.presenter.abort, b.presenter, b)
    if not ok then fe.log.error("Closing the aborted battle's presentation failed: " .. tostring(err)) end
  end
end

--- Run a battle in the current task; returns outcome, rewards. If the
--- battle raises or its task is cancelled, it is aborted however far it
--- got: `Battle:abort` (rule layers' `on_abort`) and the presenter's
--- `abort(battle)` (remove its scene, sprites and battle world at once)
--- run, `battle.active` is cleared, `finished` is not emitted and the error
--- propagates. Other
--- options are passed to the presentation (tactical: map, units, enemies,
--- deploy, win, lose, auto). `opening = "ambush"` makes the party skip its
--- first turns, `"preemptive"` the enemies (Battle.opening_rules).
--- `presentation` is a module name or the module table itself.
---@param options {troop?: string, stage?: string, music?: string, escapable?: boolean, scheduler?: table, rules?: table, presentation?: string|table, setup?: fun(b: table), allow_defeat?: boolean, random?: RandomStream, opening?: "ambush"|"preemptive"}
function battle.start(options)
  local troop = options.troop and data.get("troops", options.troop)
  local layers = { battle.rules }
  if options.rules then layers[#layers + 1] = options.rules end
  if troop and troop.script then layers[#layers + 1] = require(troop.script) end
  local presentation = options.presentation or battle.settings.presentation
  if type(presentation) == "string" then presentation = require(presentation) end
  local b = Battle({
    layers = layers,
    scheduler = options.scheduler or (presentation.scheduler and presentation.scheduler()) or battle.settings.scheduler(),
    troop = options.troop,
    escapable = options.escapable ~= false and (not troop or troop.escapable),
    random = options.random,
    opening = options.opening,
  })
  local presentation_options = tablex.copy(options)
  presentation_options.stage = options.stage or (troop and troop.stage) or (not options.map and battle.default_stage()) or nil
  presentation_options.music = options.music or (troop and troop.music)
  b.presenter = presentation.new(b, presentation_options)
  local completed = false
  local outcome, rewards = task.ensure(function()
    if options.setup then
      options.setup(b)
    elseif presentation.populate then
      presentation.populate(b, presentation_options)
    else
      b:populate(party.members(), options.troop, battle.settings.back_row and party.reserve() or nil)
    end
    battle.active = b
    battle.started:emit(b)
    local o, r = b:run()
    completed = true
    return o, r
  end, function()
    if battle.active == b then battle.active = nil end
    if not completed then abort(b) end
  end)
  battle.finished:emit(b, outcome, rewards)
  if outcome == "defeat" and not options.allow_defeat and not battle.mocking then
    battle.on_defeat()
  end
  return outcome, rewards
end

--- Run `fn(...)` (usually a battle) as a mock battle, like Triangle
--- Strategy's mental mock battles. Every fe.state section except those in
--- `settings.mock_keep` (clock, daynight, field) is snapshotted first and
--- rolled back afterwards, however `fn` ends: party and roster, actors' HP,
--- MP, states and experience, inventory and money, flags, variables,
--- quests, story, followers, companions, the bestiary and the game's own
--- sections. Playtime, the time of day and the field location go on
--- unchanged. Tactical terrain changes are undone by the battle itself.
--- The roster starts fully recovered unless `options.recover` is false, and
--- a defeat never calls on_defeat. Returns what `fn` returns.
---@param fn fun(...): ...
---@param options? {recover?: boolean}
function battle.mock(fn, options, ...)
  assert(not battle.mocking, "mock battles do not nest")
  local saved = state.snapshot()
  battle.mocking = true
  local args = table.pack(...)
  return task.ensure(function()
    if not options or options.recover ~= false then party.recover() end
    return fn(table.unpack(args, 1, args.n))
  end, function()
    battle.mocking = false
    local now = state.snapshot()
    for _, name in ipairs(battle.settings.mock_keep) do saved[name] = now[name] end
    state.restore(saved)
  end)
end

--- A random or scripted encounter with a troop.
function battle.encounter(troop, options)
  options = options or {}
  options.troop = troop
  return battle.start(options)
end

return battle
