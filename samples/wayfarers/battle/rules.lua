-- Wayfarers' battle rules: the kit's shields / break / boost layer
-- (fe.battle.break) with this game's numbers.
--  * Shields: enemies carry shield points and hidden weaknesses (damage
--    types and elements, data/enemies.json). Each weakness hit removes a
--    point and reveals the weakness; at zero the enemy is Broken
--    (data/states.json): it loses its turns, takes double damage, acts last,
--    then its shield recovers.
--  * Boost: party members gain 1 BP per round (max 5, battle-only) and
--    commit up to 3 when choosing a command (Q/E or shoulders) for extra
--    hits or power; BP is paid when the action happens.
--  * Divine skills (fe.battle.limit): an LP gauge (max 3, battle-only) fills
--    when a traveller is hurt, breaks a foe or sees an ally fall; a full
--    gauge pays for the class's divine skill (the Divine command).
--  * Turns come in Octopath order (main.lua): everyone acts once a round
--    by speed, choosing when their turn comes; the HUD shows this round and
--    the next. The Bandit Chief (battle/chief.lua) takes two or three
--    actions in each of his turns.
--  * shields.rules(limit.rules()) adds the limit layer as a layer of its
--    own, so both kits' hooks run.
local limit = require("fe.battle.limit")
local shields = require("fe.battle.break")

shields.settings.damage_bonus = 2
shields.settings.max_spend = 3
shields.install_sideview()

return shields.rules(limit.rules())
