-- Records what an event script was given (field tests read `record.calls`).
local record = { calls = {} }

setmetatable(record, {
  __call = function(_, ev)
    record.calls[#record.calls + 1] = { self = ev.self, player = ev.player, trigger = ev.page.trigger }
    require("fe.task").frame()
  end,
})

return record
