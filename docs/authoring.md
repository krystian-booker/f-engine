# Authoring a game

The sandbox is the executable reference for every format. JSON files are UTF-8 and paths are relative to the game directory unless specified otherwise. IDs should be stable; saves refer to actor, item, skill, state and map IDs.

## Game folder

```text
game.json                 title, window, virtual_resolution, pixels_per_unit,
                          start_map, start_spawn, battle_effect_sprite
initial_state.json        party IDs, gold, inventory counts
database.json             RPG tables
timelines.json            battle presentation sequences
maps/<id>/map.json        navigation, events, lights, cameras, encounters
maps/<id>/geometry.glb    static diorama
sprites/*.anim.json       directional animation descriptions
sprites/*.png            sprite sheets / atlases
events/*.lua              event functions
scripts/rules.lua         formulas, effects and AI
audio/                    music and sounds
audio/events.json         sprite-animation event → sound path
ui/layout.json            window rectangles, skin and optional BMFont
ui/skin.png               nine-slice skin
ui/icons.json             optional icon name → image path
```

`game.json` uses `window: {width,height}`, `virtual_resolution: {width,height}` and `pixels_per_unit`. The sample uses 1280×720, 480×270 and 32 PPU. `initial_state.json` recruits the listed actors and selects them as the active party (one to four members).

## Blender and maps

Model at one unit per tile, apply transforms and export a static glTF 2.0 `.glb` with normals. Use uncompressed triangles; skins and Draco compression are unsupported. The renderer reads base color, optional normal maps, material alpha and vertex color. It supports embedded GLB images and external image files. Use GLB for portable map exports. Blender's glTF exporter converts its coordinates into Y-up automatically.

Enable custom properties in the exporter. Node extras may be directly on the node or inside `fengine`:

```json
{"spawn":"entry"}
{"collision":true}
{"collision":{"walkable":true,"height":1}}
{"light":{"color":[1,0.6,0.2],"radius":6,"intensity":2}}
```

The editor's **Import Blender markers** action bakes these into the map. Collision markers rasterize transformed mesh bounds into nav cells. A walkable marker supplies a flat height. Spawn markers use the node's X/Z location; point and directional `KHR_lights_punctual` are imported too. Save the map after import. Author detailed ramps and boundaries with the nav painter.

`nav` contains `width`, `depth` and row-major `cells` (index `z*width+x`). Each cell has `height`, `walkable`, `ramp` (`none/north/east/south/west`), numeric `flags` and `region`. Ramp names indicate the direction of rising height. Flags are water=1, counter=2 and damage-floor=4; combine them with addition. Region 0 has no encounter unless configured. Map `encounters` maps region-number strings to database encounter IDs. `spawns` maps names to `[x,y,z]`; height is resolved from navigation.

Entities have unique `id`, `name`, `[x,y,z] position`, sprite path, clip, `[x,z] facing`, speed, radius, solid and routine. Routines are `stationary`, `wander` or `patrol`; patrol `route` contains `[x,z]` points. A `pages` array selects its **last matching page**:

```json
{
  "conditions":{"flags":{"chest_open":false},
    "variables":[{"id":"chapter","op":">=","value":1}]},
  "trigger":"action", "script":"events/chest.lua", "solid":true
}
```

Conditions also support `item` and `actor` IDs. Variable comparisons are `==`, `!=`, `>=`, `<=`, `>` and `<`. Triggers are action, touch, autorun and parallel. A page may override the sprite and solidity. A touch event runs on entry; autorun pages should change a condition when finished to avoid restarting forever. Counters let interaction reach an NPC across a flagged cell.

Map cameras use pitch, yaw and FOV in degrees, distance and `[x,z] min/max` target bounds. Camera zones provide min/max rectangles and partial camera overrides; the last containing zone wins. Maps also define point lights, ambient/sun colors and direction, fog and BGM. Post keys are `bloom`, `depth_of_field`, `vignette`, `color_grade`, `bloom_strength`, `focus` (vertical tilt band center), `focus_range`, `focus_distance` (0 follows the camera distance), `aperture`, `exposure` and `lut`. LUT paths accept text `.cube` 3D LUTs and are resampled to 16³.

## Sprites and asset tools

An animation file references an `atlas` relative to itself, optional `pixels_per_unit`, and named clips. Each clip has `loop` and `directions`; each direction contains frames with `rect:[x,y,w,h]`, `duration` seconds and optional `event` string. Direction counts are 1, 4 or 8. Four-direction order is south, west, north, east; eight adds diagonals between these in that order. Frames are bottom-centered in the world. Idle and walk are conventional field clip names.

```json
{"atlas":"hero.png","pixels_per_unit":32,"clips":{"idle":{
  "loop":true,"directions":[[{"rect":[0,0,24,32],"duration":0.2}]]
}}}
```

`fengine_assetc anim sheet.json hero.anim.json` converts uniform sheets. Its manifest includes `atlas`, `frame_width`, `frame_height`, `directions` and `clips`; each clip supplies frame column indices, duration and optional row/column offsets. The atlas is copied beside the output when needed.

`fengine_assetc atlas manifest.json output-dir` shelf-packs separate images with duplicated edge padding. The manifest uses width, height, padding, directions, pixels_per_unit and `frames:[{file,id,clip,direction,duration,event}]`. Every declared direction of a clip must contain frames. Output is `atlas.png`, `atlas.anim.json` and `regions.json`.

## RPG database

Tables are arrays of objects with unique `id`. Names are display strings; references use IDs. The loader validates references, target scopes, stats, rewards, probabilities and prices.

| Table | Main fields |
| --- | --- |
| actors | name, job, sprite, level, stats, skills, elements |
| jobs | name, growth, learn:[{level,skills}], curves |
| skills | name, target, mp_cost, timeline, effects |
| items | name, type (consumable/equipment/key), price, slot, bonus, effects |
| states | name, duration, damage_fraction, speed_multiplier, prevent_action |
| elements | id, name |
| enemies | name, sprite, stats, level, skills, ai, elements, exp, gold, drops |
| troops | enemies, stage (map ID), escapable |
| encounters | distance, troops:[{troop,weight}] |
| shops | items, buy_rate, sell_rate |

Stats are hp, mp, attack, defense, magic, resistance and speed. Job growth adds bonuses per level beyond level 1. Optional job `curves:{hp:[0,12,28],...}` supplies cumulative bonuses indexed by level and overrides linear growth for that stat; short curves hold their final value. Top-level `experience_curve` supplies cumulative EXP thresholds starting with 0, for 2–99 levels. The default is `(level-1)²*50`. Learning occurs as levels are crossed.

Element multipliers on actors/enemies use `{fire:0.5,ice:2}`: zero is immune, one normal, two double damage. Declaring the optional elements table enables reference checking. Effects use types `damage`, `heal`, `restore_mp`, `state`, `remove_state`, `revive` or `script`; other fields include power, formula, element, state, chance, script and recipient (`target` or `user`). Skills target `enemy`, `ally`, `all_enemies`, `all_allies`, `self` or `dead_ally`. Consumables' field effects cover healing, MP, revive and state removal. Script effects and damage formulas execute in battles.

Battle timeline files map names to arrays of `{time,command,...parameters}` events (see the sample). Commands are step, animate, effect, return, flash, sfx and shake. Each timeline must contain exactly one effect event. Step moves the acting sprite until return. Skills select timelines by name.

## UI and editing

`ui/layout.json` names rectangles with x/y/w/h in virtual pixels. Existing sample keys cover dialogue, choices, menus, actor panels, shops and battle windows. The skin is a nine-slice image with `skin_border`. Optional `font` selects an AngelCode text `.fnt` and its referenced page images; otherwise ImGui's embedded TrueType font is used. Include glyphs for the language you ship. The layout's default font does not provide every writing system.

Messages word-wrap and paginate, retain inline codes across pages, reveal with a typewriter effect and optionally show a portrait. First confirm reveals a page; the next advances it. Lists retain their selection and scroll automatically.

F1 toggles developer tools. The map editor can paint navigation, set regions/flags, select/place/edit/delete entities and pages, edit lights, spawns, camera zones and post effects, import markers and play from a picked position. Inspect the ECS and flags/variables, execute Lua snippets, inspect/edit database JSON, or select a troop in the battle sandbox. Map Save writes atomically. Database editing and external data changes are validated before use; inspect the log after a rejected edit. Keep backups/version control for authored data.
