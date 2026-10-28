# Authoring a game

The samples are the reference for every format: `samples/vale`, `samples/wayfarers` and `samples/ridgeline`. Files are UTF-8 JSON or Lua, and paths are relative to the game folder.

## Game folder

```text
game.json                 title, window {width,height}, virtual_resolution {width,height}, pixels_per_unit
main.lua                  entry point (see scripting.md)
data/*.json               data tables (loaded and validated by Lua schemas)
data/audio.json           music and sound ids -> files, loop points, volumes (fe.sound)
maps/<id>/map.json        grid, points, placements, zones, lights, emitters, environment, props
maps/<id>/geometry.glb    static diorama exported from Blender (optional per map)
sprites/*.anim.json       directional sprite animations + atlas PNGs
portraits/ ui/ audio/     images, LUTs (.cube), backdrops, fonts, music, sounds
lang/<code>.json          localised strings for {t:key} and fe.i18n.t
events/ battle/ ai/ ...   any Lua modules, required by dotted name ("events.elder")
tests/*.lua               headless tests (fengine_headless --test)
replays/*.json            windowed input replays (CTest when FENGINE_RUN_REPLAYS=ON)
```

Any file in the game folder with the same path as a library module overrides it. For example, `fe/ui/screens/title.lua` replaces the default title screen.

## Data tables

`fe.rpg.database.load()` (called by `fe.starter`) loads the standard tables when they exist: `classes`, `actors`, `skills`, `items`, `states`, `enemies`, `troops`, `shops`, `encounters` and `quests`. A file is an array of records with an `id`, or an object keyed by id. Schemas live in Lua (`lualib/fe/rpg/database.lua`). They check types and cross-table references, and they drive the editor's Data tab. Schemas are open: extra fields such as Wayfarers' `shield` and `weak` are kept, and `schema.extend(name, fields)` adds validation for them. Your own tables use `data.load(name, path, schema)`.

| Table | Main fields |
| --- | --- |
| classes | `name`, `growth` (stat per level), `curves`, `skills [{skill, level, jp}]`, `equip` (types), `commands`, `passives`, `modifiers` |
| actors | `name`, `class`, `level`, `stats`, `sprite`, `battle_sprite`, `portrait`, `skills`, `equipment`, `slots` |
| skills | `name`, `type`, `target`, `scope`, `cost {resource: n}`, `hits`, `effects [...]`, `element`, `damage_type`, `timeline`, `range`, `area`, `passive`, `modifiers`, `priority`, `tags` (tactics also reads `los`) |
| items | `name`, `kind`, `price`, `stack`, `use {target, effects}`, `equip {slot, type, modifiers}` |
| states | `name`, `turns`, `modifiers [{stat, add/mul/set}]`, `prevents_action`, `on_turn` effects, `removed_by`, `stacking` |
| enemies | `name`, `sprite`, `scale`, `level`, `stats`, `skills`, `ai`, `resist {element: multiplier}`, `rewards {exp, money, drops}` |
| troops | `name`, `members [{enemy, ...}]`, `stage`, `music`, `escapable`, `script` |
| encounters | per-area tables with steps, weights and danger levels (fe.encounter) |
| shops, quests | stock and prices; quest stages, chapters and journal text |

Effects are tables with a `type` handled by `fe.effects` (`damage`, `heal`, `add_state`, `remove_state`, `revive`, `push`, or your own handlers) plus fields such as `formula`, `power`, `element` and `chance`. Tactical skills add `range` and `area` as `{shape, min, max, height}`.

## Maps

`map.json` uses format 2. The engine stores and queries a map's contents but does not interpret them.

- `grid`: `width`, `depth`, a `tags` name list and `layers`. Each layer holds rows of text: `height` (space-separated numbers), `walk` (`1`/`0`), `ramp` (`.` or the rising direction `n e s w`) and `tags` (space-separated hex bitmasks indexing the tag list). Use extra layers for bridges and overpasses, and `links` (`{x, z, from, to, dir}`) to connect them.
- `points`: named positions (`{position, facing, layer}`) for spawns, battle stage slots and cutscene marks.
- `placements`: `{id, type, position, facing, layer, props}`. `fe.field` spawns them by `type`; props are type-specific and include event `pages`.
- `zones`: named rectangles with a type and props, which fire `zone_enter` and `zone_exit`.
- `lights` (point or spot, with flicker), `emitters` (particles), `colliders` (extra solid boxes).
- `environment`: `ambient`, `sun_color`, `sun_direction`, `fog_color`, `fog_density`, `sky` (gradient colours, clouds, `backdrops`), `weather` (`rain`, `snow`, `leaves`, `embers`, `dust`), and `post` (`bloom_strength`, `focus`, `focus_range`, `vignette`, `exposure`, `lut`, `lut_blend_target`, `lut_blend`, `pixel_scale` (1–8)).
- `props`: free data read by Lua, such as `music`, `camera`, `battle_stage`, `encounters` and `daynight`.

Scripts can change the environment at run time with `world:set_environment{...}`. Those changes are not written back into the map file.

### Blender

Model at one unit per grid cell, apply transforms and export glTF 2.0 (`.glb`) with normals. Textures, vertex colours, alpha (opaque, mask or blend) and node TRS animations are supported. Skinning and Draco compression are not. Custom properties on a node, either directly or under a `fengine` key, act as markers:

```json
{"spawn": "entry"}
{"collision": true}
{"collision": {"walkable": true, "height": 1}}
{"material": "water"}
```

`KHR_lights_punctual` point and spot lights are imported as lights. In the editor, **Import markers** bakes spawn, collision and light markers into the grid, points and lights. Save the map afterwards.

## Sprites

An `.anim.json` file names an `atlas` image, an optional `pixels_per_unit`, and `clips`. Each clip has `loop` and `directions` (1, 4 or 8). Each direction is a list of frames `{rect: [x, y, w, h], duration, event?}`. The 4-direction order is south, west, north, east. Frame `event` strings appear as `anim_event` world events, which is useful for footstep sounds and hit timing. The kits use the clip names `idle`, `walk`, `attack`, `cast`, `hurt`, `ko` and `victory` where they exist.

- `fengine_assetc sheet sheet.json out.anim.json` converts a uniform grid sheet.
- `fengine_assetc atlas manifest.json outdir` packs loose frames into an atlas with padding.

## Editor (F1, dev builds)

| Tab | Purpose |
| --- | --- |
| World | Pick worlds and entities and inspect transform, sprite, light and props |
| Map | Paint height, walkability, ramps and tags per layer; edit placements with schema-driven prop forms; edit zones, lights, points and environment; import Blender markers; undo, redo and save |
| Data | Form editing of data tables from their schemas, with validation |
| Console | Run Lua in the live game |
| Profiler | Frame, Lua and draw statistics |
| Launch | Buttons registered by the game with `fe.devtools.register` (battles, warps, cheats) |

Lua errors open an overlay with the traceback. Fix the file and it reloads. Keep authored data in version control; map saves are atomic but overwrite the file.
