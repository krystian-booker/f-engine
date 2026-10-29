-- Stands in for the battle kit's companion module in follower tests.
local probe = { calls = {} }

function probe.join(id, spec)
  probe.calls[#probe.calls + 1] = { "join", id, spec.enemy, spec.skill, spec.name }
end

function probe.leave(id)
  probe.calls[#probe.calls + 1] = { "leave", id }
end

return probe
