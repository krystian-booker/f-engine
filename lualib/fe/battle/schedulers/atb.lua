--- Active Time Battle: each battler's gauge fills with speed; full gauges
--- act. Player menus open as party gauges fill while time keeps running
--- ("active") or pauses during menus ("wait"). Actions queue and resolve one
--- at a time so animations never overlap, and gauges hold still while an
--- action (or a turn's upkeep) plays out. A battler whose gauge fills while
--- it cannot act (asleep, broken), or whose turn a rule layer skips
--- (Battle:skips_turn), takes an upkeep-only turn. A battler knocked out
--- or disabled while still choosing has its decision cancelled (the
--- side-view command menu closes with it) and its gauge emptied. Battlers
--- added mid-battle get a gauge on the next frame; a battler swapped in
--- (Battle:swap) takes over the leaver's gauge. Obstacles have no gauge. A
--- queued action whose user was knocked out (or left) before it started is
--- dropped: nothing is paid.
---
--- Gauges fill at (speed + rate_offset) / rate_divisor per second times
--- `speed` and start between 0 and `start_fill` full; the defaults come
--- from Battle.settings.atb. Settings (all optional):
---   mode = "wait"|"active"; rate(b) -> fill per second; speed (a number, or
---   fn(battle) -> number read every frame, so a "slow battle" option
---   applies at once); start_fill, rate_offset, rate_divisor;
---   initial(b, battle) -> start fraction (nil: random), may be negative to
---     delay a first action;
---   hold_queue = true: true Wait. While a player decision is open in wait
---     mode no queued action starts either (an action already playing
---     finishes), so being quick in a menu never matters;
---   ready = "hold": a player battler whose gauge fills joins
---     `scheduler.ready` (in the order they filled) instead of opening its
---     menu, and waits there, full, without acting until the presenter calls
---     `scheduler.choose(b)`. A decision that returns nil (cancelled, or the
---     player switched character) puts it back in the list, still full.
---     A battler with a forced action (Battle:forced_action) when its gauge
---     fills acts at once instead. The default ("auto") opens the menu.
---
--- `scheduler.hold(key) -> release` stops the clock (no gauge fills, no
--- queued action starts) until every hold is released, in any mode:
--- inspect screens, tutorials and cinematics. A hold taken in a task is
--- released when that task ends; releasing twice is harmless.
local Battle = require("fe.battle.core")
local signal = require("fe.signal")
local tablex = require("fe.tablex")
local task = require("fe.task")

local atb = {}

---@param settings? {mode?: "wait"|"active", rate?: fun(b: table): number, speed?: number|fun(battle: table): number, start_fill?: number, rate_offset?: number, rate_divisor?: number, initial?: fun(b: table, battle: table): number?, hold_queue?: boolean, ready?: "auto"|"hold"}
function atb.new(settings)
  settings = settings or {}
  local scheduler = {
    settings = settings,
    --- Player battlers waiting with a full gauge (ready = "hold"), in the
    --- order they filled. Changed in place; read it, do not edit it.
    ready = {},
    --- (ready_list) whenever `ready` changes.
    ready_changed = signal.new(),
    --- Decided actions waiting to start, in order (read-only).
    queue = {},
  }
  local holds = {}
  local choosing = 0      -- open player decisions (wait mode pauses for them)
  local deciding = {}     -- battler -> decision record
  local decisions = {}    -- running decision records, in start order
  local ready_serial = 0

  local function rate(b)
    if settings.rate then return settings.rate(b) end
    local defaults = Battle.settings.atb
    return (b:stat("speed") + (settings.rate_offset or defaults.rate_offset)) / (settings.rate_divisor or defaults.rate_divisor)
  end

  local function speed()
    local s = settings.speed
    if type(s) == "function" then s = s(scheduler.battle) end
    return s or 1
  end

  -- Gauge fill per second for a battler at the current speed.
  local function fill_rate(b)
    return rate(b) * speed()
  end

  local function player(b) return b:is_party() and not b.auto end

  local function ensure_gauge(b)
    if b.data.gauge == nil then
      local start = settings.initial and settings.initial(b, b.battle)
      if start == nil then
        start = (settings.start_fill or Battle.settings.atb.start_fill) * b.battle.random:float()
      end
      b.data.gauge = start
    end
  end

  local function ready_changed()
    scheduler.ready_changed:emit(scheduler.ready)
  end

  local function add_ready(b)
    if tablex.contains(scheduler.ready, b) then return end
    if not b.data.ready_serial then
      ready_serial = ready_serial + 1
      b.data.ready_serial = ready_serial
    end
    local list, at = scheduler.ready, #scheduler.ready + 1
    for i, other in ipairs(list) do
      if (other.data.ready_serial or 0) > b.data.ready_serial then at = i break end
    end
    table.insert(list, at, b)
    ready_changed()
  end

  local function remove_ready(b)
    if tablex.remove_value(scheduler.ready, b) then
      ready_changed()
      return true
    end
    return false
  end

  -- A decision ended (decided, cancelled or its task stopped).
  local function close(d)
    if d.closed then return end
    d.closed = true
    if d.counted then choosing = choosing - 1 end
    if deciding[d.battler] == d then deciding[d.battler] = nil end
  end

  -- Ask a battler for its action in a task of its own (a child of the
  -- current task). `chosen`: opened by scheduler.choose (a nil decision
  -- returns the battler to the ready list).
  local function open(battle, b, counted, chosen)
    local d = { battler = b, counted = counted }
    deciding[b] = d
    if counted then choosing = choosing + 1 end
    d.task = task.spawn(function()
      task.finally(function() close(d) end)
      local action = battle:decide(d.battler)
      local who = d.battler -- a swap during the decision hands it over
      close(d)
      if action then
        deciding[action.user] = nil
        action.user.data.queued = true
        scheduler.queue[#scheduler.queue + 1] = action
        d.action = action
      elseif chosen and who:can_act() and who.battle == battle then
        who.data.gauge = 1
        add_ready(who)
      else
        who.data.gauge = 0
      end
    end)
    -- AI decisions finish immediately; player menus keep running.
    if d.task.status == "running" then decisions[#decisions + 1] = d end
    return d
  end

  local function unqueue(b)
    for i = #scheduler.queue, 1, -1 do
      if scheduler.queue[i].user == b then table.remove(scheduler.queue, i) end
    end
    b.data.queued = nil
  end

  --- Whether a hold is held (with this key, or any).
  function scheduler.held(key)
    for _, entry in ipairs(holds) do
      if key == nil or entry.key == key then return true end
    end
    return false
  end

  --- Whether battle time stands still now: a hold is held, or a player is
  --- choosing in wait mode. (Queued actions also wait for menus only with
  --- hold_queue.) Presenters pause countdowns with it.
  function scheduler.frozen()
    return scheduler.held() or ((settings.mode or "wait") == "wait" and choosing > 0)
  end

  --- Stop the clock until the returned function is called (or the task
  --- that took the hold ends). `key` names it for `held(key)`.
  ---@param key? string
  ---@return fun()
  function scheduler.hold(key)
    local entry = { key = key or "hold" }
    holds[#holds + 1] = entry
    return task.cleanup(function() tablex.remove_value(holds, entry) end)
  end

  function scheduler.run(battle)
    scheduler.last = nil
    scheduler.battle = battle
    scheduler.queue = {}
    deciding, decisions, choosing = {}, {}, 0
    scheduler.deciding = deciding
    if #scheduler.ready > 0 then
      for i = #scheduler.ready, 1, -1 do scheduler.ready[i] = nil end
      ready_changed()
    end
    local hold_mode = settings.ready == "hold"
    for _, b in ipairs(battle.battlers) do ensure_gauge(b) end
    while not battle:outcome() do
      local wait_mode = (settings.mode or "wait") == "wait"
      local now = task.time()
      local dt = now - (scheduler.last or now)
      scheduler.last = now
      -- A battler that can no longer act while it is choosing stops choosing.
      for _, d in ipairs(decisions) do
        local b = d.battler
        if d.task.status == "running" and deciding[b] == d and not b:can_act() then
          d.task:cancel()
          close(d)
          b.data.gauge = 0
        end
      end
      decisions = tablex.filter(decisions, function(d) return d.task.status == "running" end)
      -- Ready battlers that fell, left or went under AI control leave the list.
      for i = #scheduler.ready, 1, -1 do
        local b = scheduler.ready[i]
        if b.battle ~= battle or not b:can_act() or not b:takes_turns() then
          remove_ready(b)
          b.data.gauge = 0
        elseif not player(b) then
          remove_ready(b)
        end
      end
      if not (scheduler.held() or (wait_mode and choosing > 0)) then
        for _, b in ipairs({ table.unpack(battle.battlers) }) do
          ensure_gauge(b)
          if b:takes_turns() and not deciding[b] and not b.data.queued and not battle:outcome()
            and not tablex.contains(scheduler.ready, b) then
            b.data.gauge = math.min(1, b.data.gauge + dt * fill_rate(b))
            if b.data.gauge >= 1 then
              if not b:can_act() or battle:skips_turn(b) then
                b.data.gauge = 0
                battle:turn(b, false)
                scheduler.last = task.time()
              elseif hold_mode and player(b) then
                local forced = battle:forced_action(b)
                if forced then
                  b.data.queued = true
                  scheduler.queue[#scheduler.queue + 1] = forced
                else
                  b.data.ready_serial = nil
                  add_ready(b)
                end
              else
                open(battle, b, player(b), false)
              end
            end
          end
        end
      end
      local queue = scheduler.queue
      local paused = scheduler.held() or (settings.hold_queue and wait_mode and choosing > 0)
      if #queue > 0 and not paused then
        local action = table.remove(queue, 1)
        local b = action.user
        b.data.queued = nil
        b.data.gauge = 0
        if b:alive() and b.battle == battle then
          battle:turn(b, action)
          scheduler.last = task.time()
        end
      end
      task.frame()
    end
    -- Anyone still choosing when the battle ends stops choosing.
    for _, d in ipairs(decisions) do
      if d.task.status == "running" then d.task:cancel() end
    end
    if #scheduler.ready > 0 then
      for i = #scheduler.ready, 1, -1 do scheduler.ready[i] = nil end
      ready_changed()
    end
  end

  --- Open the decision of a ready battler (ready = "hold") and wait for it;
  --- call from a task (the presenter's). Returns the action it queued, or
  --- nil: the decision returned nil (the battler is back in the ready list,
  --- still full), it was cancelled (the battler fell), or `b` was not ready.
  --- Battle time stands still from the call until it returns in wait mode,
  --- so switching straight to another ready battler loses no time.
  function scheduler.choose(b)
    local battle = scheduler.battle
    if not battle or not tablex.contains(scheduler.ready, b) then return nil end
    remove_ready(b)
    choosing = choosing + 1
    local release = task.cleanup(function() choosing = choosing - 1 end)
    local d = open(battle, b, false, true)
    if d.task.status == "running" then task.join(d.task) end
    release()
    return d.action
  end

  --- Set a battler's gauge (a fraction; 1 is full, below 0 delays it). A
  --- value below 1 takes back its turn: a queued action is dropped and a
  --- ready battler leaves the ready list. An open decision is not affected.
  function scheduler.set_gauge(b, fraction)
    if fraction < 1 then
      if b.data.queued then unqueue(b) end
      remove_ready(b)
    end
    b.data.gauge = fraction
  end

  --- Push a battler's next action back by `seconds` of its own fill time at
  --- the current speed (its time_left grows by exactly that much; the gauge
  --- may go negative). A battler with a queued action or waiting ready
  --- counts as full: the action is dropped and it decides again when the
  --- gauge refills. Returns the new gauge.
  function scheduler.delay(b, seconds)
    local gauge = b.data.gauge or 0
    if b.data.queued or tablex.contains(scheduler.ready, b) then gauge = 1 end
    gauge = gauge - seconds * fill_rate(b)
    scheduler.set_gauge(b, gauge)
    return gauge
  end

  --- Seconds until the battler's gauge is full at the current rate (0 when
  --- full; math.huge when it does not fill). Battle time stands still while
  --- frozen, so this does too.
  function scheduler.time_left(b)
    local gauge = b.data.gauge or 0
    if gauge >= 1 or b.data.queued then return 0 end
    local per_second = fill_rate(b)
    if per_second <= 0 then return math.huge end
    return (1 - gauge) / per_second
  end

  --- Remove queued actions that have not started and match `pred(action)`
  --- (every one when nil); their users' gauges become `refill` (default 0;
  --- 1 refunds the turn). Returns the removed actions.
  ---@param pred? fun(action: table): boolean
  ---@param refill? number
  function scheduler.cancel_queued(pred, refill)
    local removed = {}
    local queue = scheduler.queue
    for i = #queue, 1, -1 do
      local action = queue[i]
      if not pred or pred(action) then
        table.remove(queue, i)
        table.insert(removed, 1, action)
      end
    end
    for _, action in ipairs(removed) do
      action.user.data.queued = nil
      action.user.data.gauge = refill or 0
    end
    return removed
  end

  function scheduler.replace(_, old, new)
    new.data.gauge = old.data.gauge or 0
    new.data.queued = nil
    old.data.queued = nil
    local d = deciding[old]
    if d then
      deciding[old] = nil
      deciding[new] = d
      d.battler = new
    end
    local i = tablex.index_of(scheduler.ready, old)
    if i then
      new.data.ready_serial = old.data.ready_serial
      scheduler.ready[i] = new
      ready_changed()
    end
  end

  --- Gauge fill 0..1 for UI (a delayed, negative gauge shows as empty).
  function scheduler.gauge(b)
    return tablex.clamp(b.data.gauge or 0, 0, 1)
  end

  return scheduler
end

return atb
