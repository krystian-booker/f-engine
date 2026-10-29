-- The Bandit Chief, Wayfarers' boss: like Octopath Traveler's bosses he
-- acts more than once a turn (data/enemies.json "actions"), twice at
-- first and three times once he is down to half his HP. A broken chief
-- loses all of them.
local chief = {}

function chief.actions(_, me)
  return me:resource("hp") * 2 <= me:max_resource("hp") and 3 or 2
end

return chief
