--- Settings and terrain data shared by the tactical modules (exposed as
--- tactical.settings, tactical.reactions and tactical.hazards).
local tactical_settings = {
  settings = {
    world_name = "tactical",
    height_bonus = 0.12,       -- damage per unit of height above the target (capped at +/- 3)
    back_bonus = 0.3,          -- extra damage from behind
    side_bonus = 0.15,         -- extra damage from the side
    follow_up = true,          -- allies opposite a melee target attack too
    follow_up_diagonal = false, -- allies diagonally behind the target join too
    follow_up_ranged = false,  -- allies behind the target whose attack reaches it from afar join too
    follow_up_max = nil,       -- most allies joining one attack (nil: every one that qualifies)
    zone_of_control = true,    -- entering a cell next to a foe ends movement
    fall_damage = 0.08,        -- fraction of max HP per unit fallen beyond 2 when pushed
    collision_damage = 0.06,   -- fraction of max HP dealt to both units when a push slams one into another
    attack_range = { shape = "diamond", min = 1, max = 1 },
    default_move = 4,
    default_jump = 2,          -- climb height for units without a jump stat (one more going down)
    gap_jump = false,          -- {distance, diagonal?}: units may jump gaps up to `distance` cells wide
    ladders = true,            -- units may climb the map's ladders
    sink_damage = 0.1,         -- fraction of max HP when the cell under a unit stops being walkable (melting ice)
    blocked_tags = { "blocked" },            -- cells nobody can enter (or be pushed into)
    tag_costs = { mud = 1, water = 1 },      -- extra movement cost per tag
    --- fn(battle, unit, rules): change the world:reachable rules of one
    --- unit before they are used (a flyer's tag costs, a mount's climb).
    --- The default changes nothing.
    step_rules = function() end,
    deploy_tag = "deploy",                   -- cells party units start on without a deploy phase
    camera = { pitch = 48, distance = 19, fov = 32 },
    ct_cost = { base = 100, no_move = -20, no_act = -20 },
    eye_height = 1.2,          -- line of sight starts this far above the shooter's cell
    target_height = 0.8,       -- ... and aims this far above the target cell
    colors = { move = 0x4aa3ff88, attack = 0xff6a5a88, area = 0xffd04a99, deploy = 0x6aff9a66, path = 0xffffffcc },
    --- Sprite clips the presenter plays on a unit's jump and climb events.
    motion_clips = { jump_start = "jump", landed = "idle", climb_start = "climb", climb_end = "idle" },
    --- Presenter pauses in seconds (divided by the battle speed, Battle.speed).
    waits = { turn = 0.3, action = 0.35, results = 0.4, reinforcements = 0.6, opening = 0.8, level_up = 0.8,
      finish = 1.5, rewards = 0.5, banner = 1 },
    --- Weights of the tactical AI's expected-value scoring (fe.battle.tactical.ai).
    ai = {
      ko = 0.6,            -- bonus for a hit likely to knock the target out
      ally_damage = 1.5,   -- hurting an ally counts this many times against
      enemy_heal = 0.5,    -- healing a foe counts this much against
      danger = 0.05,       -- per foe adjacent to the destination
      buff = 0.25,         -- a buff on an ally or a debuff on a foe (per stat change)
      state = 0.3,         -- a state added to a foe or removed from an ally
      disable = 0.6,       -- extra for a state that prevents action
      revive = 1,          -- reviving an ally
      --- Most action previews (`effects.resolve` in preview mode) one plan
      --- may run; past it the best plan found so far is used. Previews are
      --- memoised per (skill, target, height bucket, side attacked from)
      --- and only destinations within a skill's range of a target are tried.
      max_previews = 400,
    },
  },
  --- Terrain reactions: when an element hits a cell with `tag`.
  ---   add/remove: tags changed on the cell
  ---   walkable: true/false, or "restore" (the walkability the cell had
  ---     before an earlier reaction changed it this battle; false if none)
  ---   turns: rounds until the added tags wear off
  ---   spread_chance: each round, chance (battle RNG) to spread to each
  ---     orthogonally adjacent cell that has `tag`
  ---   conduct: hurt every unit standing in the connected `tag` region
  ---     with `effects` (each unit once per reaction)
  reactions = {
    { element = "fire", tag = "grass", add = { "burning" }, remove = { "grass" }, turns = 3, spread_chance = 0.25 },
    { element = "ice", tag = "water", add = { "ice" }, remove = { "water" }, walkable = true },
    { element = "fire", tag = "ice", add = { "water" }, remove = { "ice" }, walkable = "restore" },
    { element = "thunder", tag = "water", conduct = true, name = "Shock",
      effects = { { type = "damage", formula = "percent", power = 12, variance = 0 } } },
  },
  --- Cell hazards: a unit starting its turn on a cell with `tag` suffers
  --- `effects` (through fe.effects, so resistances apply).
  hazards = {
    { tag = "burning", name = "Burning", element = "fire",
      effects = { { type = "damage", formula = "percent", power = 10, variance = 0 } } },
  },
}

return tactical_settings
