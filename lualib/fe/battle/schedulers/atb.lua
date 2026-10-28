--- Active Time Battle: each battler's gauge fills with speed; full gauges
--- act. Player menus open as party gauges fill while time keeps running
--- ("active") or pauses during menus ("wait"). Actions queue and resolve one
--- at a time so animations never overlap.
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
    local queue, deciding, choosing = {}, {}, 0
    for _, b in ipairs(battle.battlers) do
      b.data.gauge = (settings.start_fill or 0.3) * battle.random:float()
    end
    local executing = false
    while not battle:outcome() do
      local wait_mode = (settings.mode or "wait") == "wait"
      local paused = executing or (wait_mode and choosing > 0)
      local dt = task.time() - (scheduler.last or task.time())
      scheduler.last = task.time()
      if not paused then
        for _, b in ipairs(battle.battlers) do
          if b:alive() and not deciding[b] and not b.data.queued then
            b.data.gauge = math.min(1, b.data.gauge + dt * rate(b) * (settings.speed or 1))
            if b.data.gauge >= 1 then
              if not b:can_act() then
                battle:turn_start(b)
                battle:turn_end(b)
                b.data.gauge = 0
              else
                if b:is_party() and not b.auto then choosing = choosing + 1 end
                deciding[b] = true
                local decision = task.spawn(function()
                  local action = battle:decide(b)
                  if b:is_party() and not b.auto then choosing = choosing - 1 end
                  deciding[b] = nil
                  if action then
                    b.data.queued = true
                    queue[#queue + 1] = action
                  else
                    b.data.gauge = 0
                  end
                end)
                -- AI decisions finish immediately; player menus keep running.
                if deciding[b] then deciding[b] = decision end
              end
            end
          end
        end
      end
      if not executing and #queue > 0 then
        local action = table.remove(queue, 1)
        local b = action.user
        b.data.queued = nil
        b.data.gauge = 0
        if b:alive() then
          executing = true
          battle:turn(b, action)
          executing = false
        end
      end
      task.frame()
    end
    -- Anyone still choosing when the battle ends stops choosing.
    for _, decision in pairs(deciding) do
      if type(decision) == "table" then decision:cancel() end
    end
  end

  --- Gauge fill 0..1 for UI.
  function scheduler.gauge(b)
    return b.data.gauge or 0
  end

  return scheduler
end

return atb
