-- Per-turn action counts for the "warlock" test enemy: one action on odd
-- turns, two on even ones.
local actions = {}

function actions.alternate(_, me)
  me.data.turns_taken = (me.data.turns_taken or 0) + 1
  return me.data.turns_taken % 2 == 0 and 2 or 1
end

return actions
