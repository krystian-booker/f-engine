# Lua gameplay API

Event files return a function. Commands that display UI, move an entity or perform a transition yield the event coroutine; write the event in ordinary top-to-bottom order.

```lua
return function(ev)
  ev:face_player()
  say("Elder", "Welcome, {player}.{wait:20} Will you help?")
  if choice({"Yes", "Later"}) == 1 then
    set_flag("quest_started")
    fade_out(0.4)
    warp("grotto", "entry")
    fade_in(0.4)
  end
end
```

`ev.self` is the triggering entity ID. The player is `"player"`. Event globals and library tables are isolated. Base, math, string, table, coroutine and utf8 are available; io, os, package, debug, dynamic loading and bytecode dumping are absent. Persistent values belong in flags/variables or the party/inventory. Coroutines are not saved.

## Commands that yield

| Command | Behavior |
| --- | --- |
| `wait(seconds)` | Suspend for game time |
| `say(speaker,text,portrait?)` | Paginated message; portrait is an image path |
| `choice({labels...})` | Return 1-based selection; Cancel selects the final option |
| `menu({title,options,layout?})` | Custom selectable screen using a named layout rectangle |
| `wait_input(action?)` | Wait for an action edge; default Confirm |
| `move(id,{x,z})` | Path to an absolute cell-space position |
| `move(id,{dx,dz})` | Relative movement; dy is accepted as a legacy alias for dz |
| `face(id,direction)` | north/east/south/west/player or an `[x,z]` vector |
| `animate(id,clip)` | Change the sprite clip |
| `camera(settings)` | Set camera override; optional duration waits for that interval |
| `fade_out(seconds)`, `fade_in(seconds)` | Full-screen transition |
| `warp(map,spawn)` | Change maps; cancels other map events |
| `battle(troop)` | Return `victory`, `defeat` or `escaped` after presentation |
| `shop(id)` | Open a buy/sell shop until closed |
| `inn(price)` | Offer rest, deduct gold and heal; return boolean |
| `play_bgm(path,options?)` | Streaming music with crossfade and loop points |
| `stop_bgm(seconds?)` | Fade out music |
| `play_sfx(path)` | Play a sound |
| `screen_fx(settings)` | Post toggles, flash, shake or particle burst |
| `save(slot)` | Save persistent state; unavailable during battle |
| `load_game(slot)` | Load state; replaces the current event/map/scene stack |

Camera settings accept pitch, yaw, distance, fov, target (entity ID or `[x,y,z]`), respect_bounds and duration. `{reset=true}` restores map camera tracking. A duration waits while smoothing; it does not interpolate a preauthored camera path.

`screen_fx({flash=0.7,shake={duration=0.3,amplitude=0.1}})` applies a flash and shake. `particles={entity="player",sprite="sprites/spark.anim.json",count=12,speed=1.5,size=0.25,lifetime=0.7}` emits noncolliding particles. It can toggle bloom, depth_of_field and vignette for the current map.

BGM options use seconds: `{loop_start=1,loop_end=10,fade=0.75}`. Omit loop_end to loop through the file's end. Music/SFX paths are inside the game directory. `audio/events.json` maps animation event strings to SFX paths.

Message codes are `{player}`, `{var:chapter}`, `{color:red}`, `{color:#ff8800}`, `{color:reset}`, `{wait:20}` (60 Hz frames) and `{icon:potion}`. Named colors include white, red, green, blue, gold and yellow. Icons reference `ui/icons.json`.

## Immediate state functions

- `get_flag(id)`, `set_flag(id, bool=true)`, `get_var(id)`, `set_var(id,number)`.
- `gold()`, `add_gold(amount)`; the latter returns false if the result is invalid.
- `item_count(id)`, `give_item(id,count=1)`, `use_item(item,actor)`, `equip(actor,slot,item)`; mutation functions return success booleans. Equip with an empty item ID to remove equipment.
- `item_name(id)`, `item_price(id)`.
- `recruit(actor)`, `party()`, `set_party({actorIDs...})`, `change_job(actor,job)`, `learn_skill(actor,skill)`.
- `actor_hp(actor)`, `actor_mp(actor)`, `actor_level(actor)`, `actor_stat(actor,stat)`.
- `change_hp(actor,amount,allowKO=false)`, `change_mp(actor,amount)`, `gain_experience(amount)`, `rest_party()`.
- `player_name()`, `print(text)`.
- `run_event(file,eventID="",blocking=true)` starts another task and returns its ID. It does not wait for that task. Use flags and `wait` for coordination. Nesting is limited to 16 and concurrent tasks to 256.

## Battle rules

`scripts/rules.lua` returns tables named formulas, ai and effects:

```lua
return {
  formulas = {
    physical = function(a,t,power)
      return math.max(1, a.attack*2 + power - t.defense)
    end
  },
  ai = {
    default = function(self,battlers)
      for target,b in ipairs(battlers) do
        if b.hp > 0 and b.enemy ~= battlers[self].enemy then
          return {skill="attack",target=target}
        end
      end
    end
  },
  effects = {
    drain = function(a,t,power)
      return {{type="damage",formula="physical",power=power},
              {type="heal",recipient="user",power=10}}
    end
  }
}
```

Battler tables expose id, hp, mp, max_hp, max_mp, attack, defense, magic, resistance, speed, level, enemy, gauge, skills and states. AI receives a **1-based** self index and battler list; return a skill ID and 1-based target, or `{kind="guard"}`. A custom effect function receives user, target and power and returns one typed effect object or an array. Expansion is limited to eight nesting levels. Effects use the same validated format as database effects. Rules are synchronous and cannot yield. Formulas must return finite numbers. Rules reload transactionally, keeping the previous rules on failure.

An event or rules error is logged with context; an instruction budget stops infinite loops. Modal dialogue/shops/camera commands serialize ownership among tasks. Prefer a short parallel event that waits between checks, and a blocking event for a cutscene.
