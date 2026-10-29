--- Active Time Battle: each battler's gauge fills with speed; full gauges
--- act. Player menus open as party gauges fill while time keeps running
--- ("active") or pauses during menus ("wait"). Actions queue and resolve one
--- at a time so animations never overlap, and gauges hold still while an
--- action (or a turn's upkeep) plays out. A battler whose gauge fills while
--- it cannot act (asleep, broken) takes an upkeep-only turn. Battlers added
--- mid-battle get a gauge on the next frame; a battler swapped in
--- (Battle:swap) takes over the leaver's gauge. Obstacles have no gauge.
local tablex = require("fe.tablex")
local task = require("fe.task")

local atb = {}

---@param settings? {mode?: "wait"|"active", rate?: fun(b: table): number, speed?: number, start_fill?: number}
function atb.new(settings)
  settings = settings or {}
  local scheduler = { settings = settings }

  local function rate(b)
    if settings.rate then return settings.rate(b) end
    return (b:stat("speed") + 20) / 100
  end

  function scheduler.run(battle)
    scheduler.last = nil
    -- `decisions` lists the decision tasks in start order, so the ones still
    -- running at the end are cancelled in the same order on every run
    -- (`deciding` is keyed by battler, and pairs over it follows addresses).
    local queue, deciding, decisions, choosing = {}, {}, {}, 0
    scheduler.deciding = deciding
    local function ensure_gauge(b)
      if b.data.gauge == nil then b.data.gauge = (settings.start_fill or 0.3) * battle.random:float() end
    end
    for _, b in ipairs(battle.battlers) do ensure_gauge(b) end
    while not battle:outcome() do
      local wait_mode = (settings.mode or "wait") == "wait"
      local now = task.time()
      local dt = now - (scheduler.last or now)
      scheduler.last = now
      if not (wait_mode and choosing > 0) then
        for _, b in ipairs({ table.unpack(battle.battlers) }) do
          ensure_gauge(b)
          if b:takes_turns() and not deciding[b] and not b.data.queued and not battle:outcome() then
            b.data.gauge = math.min(1, b.data.gauge + dt * rate(b) * (settings.speed or 1))
            if b.data.gauge >= 1 then
              if not b:can_act() then
                b.data.gauge = 0
                battle:turn(b, false)
                scheduler.last = task.time()
              else
                if b:is_party() and not b.auto then choosing = choosing + 1 end
                deciding[b] = true
                local decision = task.spawn(function()
                  local action = battle:decide(b)
                  if b:is_party() and not b.auto then choosing = choosing - 1 end
                  deciding[b] = nil
                  if action then
                    deciding[action.user] = nil
                    action.user.data.queued = true
                    queue[#queue + 1] = action
                  else
                    b.data.gauge = 0
                  end
                end)
                -- AI decisions finish immediately; player menus keep running.
                if deciding[b] then deciding[b] = decision end
                if decision.status == "running" then
                  decisions = tablex.filter(decisions, function(d) return d.status == "running" end)
                  decisions[#decisions + 1] = decision
                end
              end
            end
          end
        end
      end
      if #queue > 0 then
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
    for _, decision in ipairs(decisions) do
      if decision.status == "running" then decision:cancel() end
    end
  end

  function scheduler.replace(_, old, new)
    new.data.gauge = old.data.gauge or 0
    new.data.queued = nil
    old.data.queued = nil
    if scheduler.deciding then scheduler.deciding[new] = scheduler.deciding[old] end
  end

  --- Gauge fill 0..1 for UI.
  function scheduler.gauge(b)
    return b.data.gauge or 0
  end

  return scheduler
end

return atb
