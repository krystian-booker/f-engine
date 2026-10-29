# Authoring a game

The samples are the reference for every format: `samples/vale`, `samples/wayfarers` and `samples/ridgeline`. Files are UTF-8 JSON or Lua, and paths are relative to the game folder.

## Game folder

```text
game.json                 title, window {width,height}, virtual_resolution {width,height}, pixels_per_unit,
                          pause_on_focus_loss (default true)
main.lua                  entry point (see scripting.md)
data/*.json               data tables (loaded and validated by Lua schemas)
data/audio.json           music, sfx and voice ids -> files, loop points, volumes (fe.sound)
maps/<id>/map.json        grid, points, placements, zones, lights, emitters, environment, props
maps/<id>/geometry.glb    static diorama exported from Blender (optional per map)
sprites/*.anim.json       directional sprite animations + atlas PNGs
portraits/ ui/ audio/     images, LUTs (.cube), backdrops, fonts, music, sounds
shaders/fs_*.sc           game shaders: post passes, mesh materials, sprites (see Post shaders, Game shaders); shaders/<backend>/fs_*.bin compiled for shipping
lang/<code>.json          localised strings for {t:key} and fe.i18n.t
events/ battle/ ai/ ...   any Lua modules, required by dotted name ("events.elder")
tests/*.lua               headless tests (fengine_headless --test)
replays/*.json            windowed input replays (CTest when FENGINE_RUN_REPLAYS=ON)
```

With `pause_on_focus_loss` (the default), the windowed runtime stops ticking the game and pauses audio while its window is minimised or in the background, and resumes where it left off. Replays and headless runs never pause.

Any file in the game folder with the same path as a library module overrides it. For example, `fe/ui/screens/title.lua` replaces the default title screen.

A game can ship its own shaders as `shaders/vs_*.sc` and `fs_*.sc` (with an optional `shaders/varying.def.sc`; the engine's is used otherwise). `tools/package_game.py --shaderc <shaderc> --shader-include <build>/shader-include` compiles them for every backend of the package into `game/shaders/<backend>/<name>.bin`; a game without sources but with precompiled `shaders/<backend>/*.bin` ships those.

Every engine program accepts a `.pak` archive wherever it takes a game folder (`fengine_headless --check game.pak`), and plays `game.pak` when it lies where the folder would. `fengine_assetc pack <folder> <out.pak>` writes one.

## Data tables

`fe.rpg.database.load()` (called by `fe.starter`) loads the standard tables when they exist: `classes`, `actors`, `skills`, `items`, `states`, `enemies`, `troops`, `shops` and `encounters`; the starter also loads `quests` through `fe.quest`. A file is an array of records with an `id`, or an object keyed by id. Schemas live in Lua (`lualib/fe/rpg/database.lua`). They check types and cross-table references, and they drive the editor's Data tab. Schemas are open: extra fields are kept, and `schema.extend(name, fields)` adds validation for them. Your own tables use `data.load(name, path, schema)`.

| Table | Main fields |
| --- | --- |
| classes | `name`, `growth` (stat per level), `curves`, `skills [{skill, level, jp}]`, `equip` (types), `commands`, `passives`, `support_slots`, `modifiers` |
| actors | `name`, `class`, `level`, `stats`, `sprite`, `battle_sprite`, `portrait`, `skills`, `equipment`, `slots`, `support_slots` |
| skills | `name`, `type`, `target`, `scope`, `cost {resource: n}`, `hits`, `effects [...]`, `element`, `damage_type`, `timeline`, `range`, `area`, `passive`, `support`, `modifiers`, `priority`, `tags` (tactics also reads `los`) |
| items | `name`, `kind`, `price`, `stack`, `use {target, effects, consume}`, `equip {slot, type, modifiers}` |
| states | `name`, `turns`, `modifiers [{stat, add/mul/set, priority}]`, `prevents_action`, `on_turn` effects, `removed_by`, `stacking` |
| enemies | `name`, `sprite`, `scale`, `level`, `stats`, `skills`, `ai` (function path `"ai.goblin.think"`), `actions` (actions per turn: an integer, or a function path returning one each turn), `resist {element: multiplier}`, `rewards {exp, money, drops}`, `shield` and `weak` (fe.battle.break), `obstacle` and `destroyed {add, remove, walkable}` (tactics destructibles) |
| troops | `name`, `members [{enemy, position}]` (tactical battles: a `{x, z}` cell), `stage`, `music`, `escapable`, `script` |
| encounters | per-area tables with steps, weights and danger levels (fe.encounter) |
| shops, quests | stock and prices; quest stages with objectives, chapters and journal text |

Effects are tables with a `type` handled by `fe.effects` (`damage`, `heal`, `resource`, `add_state`, `remove_state`, `buff`, `revive`, `push`, `script`, or your own handlers) plus fields such as `formula`, `power`, `element` and `chance`. Tactical skills add `range` and `area` as `{shape, min, max, height}`.

## Maps

`map.json` uses format 2. The engine stores and queries a map's contents but does not interpret them.

- `grid`: `width`, `depth`, a `tags` name list and `layers`. Each layer holds rows of text: `height` (space-separated numbers), `walk` (`1`/`0`), `ramp` (`.` or the rising direction `n e s w`), `rise` (space-separated numbers: how far each ramp climbs across its cell, default 1) and `tags` (space-separated hex bitmasks indexing the tag list). Use extra layers for bridges and overpasses, and `links` (`{x, z, from, to, dir, kind}`) to connect them or to place ladders (see below).
- `points`: named positions (`{position, facing, layer}`) for spawns, battle stage slots and cutscene marks.
- `placements`: `{id, type, position, facing, layer, props}`. `fe.field` spawns them by `type`; props are type-specific and include event `pages`. `layer` must exist in the grid.
- `zones`: named rectangles with a type, props and a `layer` (`-1` = every layer), which fire `zone_enter` and `zone_exit` for entities with a mover or a collider.
- `lights`: point or spot (`inner_cone`, `outer_cone` in degrees from the axis, 0 ≤ inner < outer ≤ 90, `direction`), with `radius` (above 0, at most 1000), `intensity`, and flicker. `flicker_curve` is a list of `[seconds, scale]` keyframes that loops; `flicker` and `flicker_speed` add a noise wobble on top. `shadows: true` makes the light cast shadows; per world up to 16 spot lights, or two point lights and four spot lights, cast at once, chosen by brightness and closeness to the camera focus (see [Rendering](architecture.md#rendering)). `emitters` (particles; `max` caps live particles, `world_space: false` makes them travel with the emitter, `blend` is `alpha` (default), `additive`, `multiply`, `premultiplied` or `opaque`; additive particles with colours above 1 glow through bloom; `distort` > 0 turns them into heat haze that bends the scene behind them instead of drawing), `colliders` (extra solid boxes).
- `environment`: `ambient`, `sun_color`, `sun_direction`, `fog_color`, `fog_density`, `fog_height`, `fog_height_falloff`, `fog_max` (strongest fog blend), `shadows`, `shadow_distance` (how far past the camera focus sun shadows reach), `shadow_strength`, `shadow_bias` (world units), `sprite_light_height` (where sprites sample point lights), `anisotropic` (texture filtering), `pixel_snap` (sprites move in whole scene pixels), `water` (`wave_scale`, `wave_speed`, `fresnel`, `specular`, `deep_color`, `depth_falloff`, `reflection` 0–1 for screen-space reflections of the scene, `refraction` for how far waves bend the scene seen through the surface), `sky` (`mode` `none`/`color`/`gradient`/`panorama`, gradient colours, clouds, `backdrops`; with `none` the background is transparent, so worlds drawn earlier show through), `weather` (`kind`: `rain`, `snow`, `leaves`, `embers`, `dust` or a kind from the game's `data/weather.json`; `intensity`, `wind`, `color`, `sprite`), and `post`:
  - bloom: `bloom`, `bloom_strength`, `bloom_threshold`, `bloom_knee` (soft threshold, 0–1 of the threshold), `bloom_radius` (0–1, how much the wider levels add), `bloom_levels` (1–8);
  - depth of field: `depth_of_field`, `dof_mode` (`depth`, `tilt` or `both`), `focus_distance` (0 = the camera distance), `aperture`, `focus` and `focus_range` (the tilt-shift band, 0–1 down the screen), `dof_radius` (widest blur in screen pixels, 0–32);
  - ambient occlusion: `ssao`, `ssao_radius` (world units), `ssao_strength`, `ssao_bias`;
  - `antialias` (`auto`: FXAA unless `pixel_scale` > 1; `fxaa`; `none`), `chromatic_aberration` (this world only, before tone mapping; the same shift as `fe.screen.set{chromatic}`: with amount a, red and blue split by a × 3% of the distance from the screen centre), `distortion` (scales distort sprites and particles);
  - `vignette`, `vignette_strength`, `exposure`, `tonemap` (`aces` or `none`), `color_grade`, `lut`, `lut_blend_target`, `lut_blend`, `pixel_scale` (1–8);
  - `custom`: game post shaders, `[{"shader": "sepia", "params": [1, 1, 0.82, 0.6]}]`, at most 8 (see [Post shaders](#post-shaders)).

  Environment colours are linear values.
- `props`: free data read by Lua, such as `music`, `camera`, `battle_stage`, `encounters` and `daynight`.

Scripts can change the environment at run time with `world:set_environment{...}`. Those changes are not written back into the map file. Every post field can be animated that way (for example a `fe.tween` on `aperture` or `bloom_strength`); `custom = {}` removes the game's post shaders.

### Post shaders

A game adds full-screen passes with bgfx fragment shaders in `shaders/`. `post.custom = [{"shader": "sepia", "params": [...]}]` runs `shaders/fs_sepia.sc` after tone mapping and grading (before FXAA), on the world's image at output resolution:

```glsl
$input v_texcoord0
#include <bgfx_shader.sh>
SAMPLER2D(s_input, 0);    // the graded image so far; alpha is the world's coverage
SAMPLER2D(s_depth, 1);    // the scene's depth buffer
uniform vec4 u_params[4]; // `params`, up to 16 numbers in order: u_params[0].x is the first
uniform vec4 u_info;      // 1 / width, 1 / height, seconds, texture origin bottom-left (0/1)
void main() {
    vec4 color = texture2D(s_input, v_texcoord0);
    gl_FragColor = vec4(mix(color.rgb, vec3_splat(dot(color.rgb, vec3(0.3, 0.59, 0.11))), u_params[0].x), color.a);
}
```

Names use `a-z`, `0-9`, `_` and `-`. Keep the input alpha unless the pass should change what shows through. Development builds compile a source that is newer than its binary with the build's `shaderc` (into a cache in the temp folder) on first use and again when it changes; compile errors appear in the log and the pass is skipped. Every build loads `shaders/<backend>/fs_<name>.bin` (a loose file or an entry of `game.pak`) when there is no newer source, and ship builds only that: `metal` (macOS), `dx11` (Windows), `spirv` and `glsl` (Linux). `tools/package_game.py` compiles `shaders/fs_*.sc` for the packaged backends; to compile by hand, use the engine's shader settings:

```sh
shaderc -f shaders/fs_sepia.sc -o shaders/metal/fs_sepia.bin --type fragment --platform osx --profile metal \
  --varyingdef <engine>/shaders/varying.def.sc -i <bgfx>/src -i <engine>/shaders
# dx11: --platform windows --profile s_5_0   spirv: --platform linux --profile spirv   glsl: --platform linux --profile 330
```

### Game shaders

Mesh materials and sprites can replace the engine's fragment shader with the game's own, compiled and loaded like post shaders (`shaders/fs_<name>.sc`, same names, same development compile and `shaders/<backend>/fs_<name>.bin` for shipping; the engine's shader folder is on the include path, so `lighting.sh` is available):

- a glTF material names one in its custom properties (`extras`, directly or under `fengine`): `{"shader": "glint"}`, or a node's custom property `shader` applies to the node's materials; a mesh component's `shader` (`mesh = {model = ..., shader = "glint"}`) replaces it for every part of that entity;
- a sprite component takes `shader` (`sprite = {..., shader = "hologram"}`; `""` is the engine's).

The shader replaces only the colour pass: shadows, silhouettes and distortion keep the engine's. A mesh shader reads what `vs_world` (or, for instanced draws, `vs_world_instanced`) writes and gets what `fs_world.sc` gets (`fs_water.sc`'s for water materials):

```glsl
$input v_texcoord0, v_color0, v_worldPos, v_normal, v_viewDir, v_tangent
#include <bgfx_shader.sh>
SAMPLER2D(s_albedo, 0);
SAMPLER2D(s_normal, 1);
#include "lighting.sh"            // samplers 2-4 and 7; shadowVisibility, pointLights, fogAmount, blendOutput, u_env
SAMPLER2D(s_emissive, 5);
uniform vec4 u_material[3];       // colour; alpha cutoff, has normal map, unlit, normal scale; emissive rgb, blend output
uniform vec4 u_info;              // 1 / width, 1 / height, seconds, texture origin bottom-left (0/1)
```

`v_color0` already includes the entity's `tint` for instanced draws and `u_material[0]` does for single ones; multiply both, as `fs_world.sc` does. A sprite shader reads what `vs_sprite` writes, with the samplers and uniforms of `fs_sprite.sc`:

```glsl
$input v_texcoord0, v_color0, v_worldPos, v_normal, v_flash, v_outline, v_probe, v_spriteParams, v_tangent
#include <bgfx_shader.sh>
SAMPLER2D(s_albedo, 0);           // the atlas (palette indices with a palette)
SAMPLER2D(s_palette, 1);
#include "lighting.sh"
SAMPLER2D(s_spriteNormal, 5);
SAMPLER2D(s_spriteEmissive, 6);
uniform vec4 u_sprite[2];         // atlas texel w, h, palette (0/1), alpha cutoff; blend output, has normal map, has emissive
uniform vec4 u_info;              // as above
```

Discard below the alpha cutoff (`u_sprite[0].w`, `u_material[1].x`) so opaque draws keep their cutout, and end with `blendOutput(color, alpha, v_worldPos, mode)` so blend modes and fog work. Names use `a-z`, `0-9`, `_` and `-` (at most 64); a script naming another is an error, and a glTF name that is not valid is logged and ignored. A shader that does not compile, cannot be loaded, or whose `$input` does not match the vertex shader's outputs is reported once in the log, with the error, and the engine's shader is used. `samples/wayfarers/shaders` has one of each (`fs_glint.sc`, `fs_hologram.sc`).

### Links between layers

A link joins two layers at one cell, usually the foot of a ramp on the upper layer. Path finding and walking use the same rule, so a planned path can always be walked:

- With `dir` (`n`, `e`, `s` or `w`), moving along `dir` inside the cell switches from `from` to `to`, and moving against it switches back.
- Without `dir`, the switch happens only where the current layer ends (the next cell is not walkable on it).
- The other layer's cell must be walkable, and the two surfaces must be within 0.25 of each other (and within the walker's step limit) where the switch happens. Shape the ramp so its low edge meets the lower surface on the side walkers enter from.

A link with `"kind": "ladder"` is a ladder up a face of any height: its foot is the cell `(x, z)` on layer `from` and its top the neighbouring cell in `dir` (required) on layer `to`, which may be the same layer, as for a ladder up a cliff. Path finding takes the ladder from its foot heading along `dir` or from its top heading against it, and marks that step `climb`; movers climb it at their `climb_speed` (emitting `climb_start` and `climb_end`), on a planned route or when steered into it.

`entity:move_to(target, {layer = 1})` targets a layer explicitly; without `layer` the walker stays on its layer when it can, else picks the walkable layer whose surface is nearest the target's height.

### Weather

`weather.kind` names a preset in `@fe/assets/weather.json`: `rain`, `snow`, `leaves`, `embers`, `dust` and `custom`. A preset is an emitter (`rate`, `max`, `area`, `velocity`, `lifetime`, `size`, `color`, `color_end`, ...) plus an `origin` offset from the camera target. Put a `data/weather.json` in the game folder to add kinds or replace presets; it is read when a world first shows weather. `wind` is added to the preset's velocity, a non-white `color` replaces the preset's colours (keeping its fade), and `sprite` replaces its particle image.

### Blender

Model at one unit per grid cell, apply transforms and export glTF 2.0 (`.glb`) with normals. Textures (with their sampler's wrap and filter modes; mip chains are generated), normal maps (with tangents, or tangents are generated from the UVs), emissive textures and colours (`KHR_materials_emissive_strength` multiplies them; above 1 they glow), vertex colours (values above 1 glow), alpha (opaque, mask or blend) and node TRS animations are supported. Only the nodes of the file's scene are shown. Skinning and Draco compression are not. Custom properties on a node, either directly or under a `fengine` key, act as markers:

```json
{"spawn": "entry"}
{"collision": true}
{"collision": {"walkable": true, "height": 1}}
{"material": "water"}
{"shader": "glint"}
```

`KHR_lights_punctual` point and spot lights, with spot cones (held within 90 degrees; equal inner and outer angles get a half-degree edge) and direction, are imported as lights; directional lights are ignored (use the environment's sun). In the editor, **Import markers** bakes spawn, collision and light markers into the grid, points and lights. Save the map afterwards.

### Compressed textures

`fengine_assetc cook <game> [--force]` converts the PNG textures of world geometry and meshes (images that `.gltf`/`.glb` materials reference by file; images embedded in a `.glb` are not) and of map backdrops and panoramas to BC7 `.ktx` files with full mip chains, written next to each PNG. Colour textures are sRGB, normal maps linear. The runtime uses a `.ktx` when the GPU supports BC7 and falls back to the PNG otherwise; sprites and UI images stay PNG. Only PNGs newer than their `.ktx` are cooked again unless `--force` is given. BC7 encoding is slow (several seconds per 64×64 texture in a debug build), so cook with a release build of `fengine_assetc`.

## Sprites

An `.anim.json` file names an `atlas` image, an optional `pixels_per_unit`, and `clips`. Particles that use a sprite play its `idle` clip (or its first clip) over each particle's life. Each clip has `loop` and `directions` (1, 4 or 8). Each direction is a list of frames `{rect: [x, y, w, h], duration, event?}`. Rows run clockwise relative to the camera, starting with the row that faces the viewer: 4 directions are south, west, north, east; 8 directions are south, south-west, west, north-west, north, north-east, east, south-east. A fixed `sprite.direction` beyond a clip's rows uses the nearest row. Frame `event` strings appear as `anim_event` world events, which is useful for footstep sounds and hit timing; an event on the first frame fires as the clip starts. A clip name the sheet lacks plays its `idle` clip (or its first clip): frames, events and `anim_finished` (with the name that was asked for) all follow that clip. The kits use the clip names `idle`, `walk`, `attack`, `cast`, `hurt`, `ko` and `victory` where they exist.

An `.anim.json` may also name a `normal` image (tangent-space: red points right, green up, blue towards the viewer) and an `emissive` image, both laid out exactly like the atlas. With a normal map the sprite is lit per pixel, so point and spot lights shade it as they pass; the emissive image's colour, times its alpha, is added unlit (eyes, lanterns, runes).

A sprite component (`world:spawn{sprite = {...}}`, `entity:sprite{...}`) takes `animation`, `clip`, `move_clip`, `idle_clip`, `speed` (-1000 to 1000), `direction`, `visible`, `unlit`, `cast_shadow` (only opaque sprites cast; blended ones and opaque ones with a tint alpha below 1 cast nothing), `shader` (see [Game shaders](#game-shaders)), `scale` (0 to 1000), `depth_bias`, `offset` (pixels), `tint`, `flash`, `outline`, `palette`, `palette_row`, and:

- `mode`: `upright` (default, turns about the vertical axis), `facing` (full billboard), `flat` (lies at the feet's height) or `decal` (projected onto the grid surface of the entity's layer, following heights and ramps; for shadows under characters, magic circles and target markers).
- `blend`: `opaque` (default; an alpha cutout), `alpha`, `additive`, `multiply` or `premultiplied`; `true` and `false` mean `alpha` and `opaque`. Additive sprites glow (fire, light shafts); multiply darkens what is behind.
- `silhouette`: a colour drawn where map geometry or meshes hide the sprite (alpha 0 or `false` turns it off; `true` is translucent white).
- `pixel_snap`: `true` or `false` overrides the environment's `pixel_snap` for this sprite.
- `distort`: a strength > 0 draws no colour; the sprite bends the scene behind it by its image's alpha gradient plus a heat wobble, so a ring image makes a shock wave and a soft dot a lens. Walls in front of it hide the effect.

Meshes (`mesh = {...}`) take `blend` as well, and an animation `speed` of -1000 to 1000 like sprites; without it each glTF material's alpha mode applies. Their `shader` replaces every part's fragment shader (see [Game shaders](#game-shaders)).

- `fengine_assetc sheet sheet.json out.anim.json` converts a uniform grid sheet.
- `fengine_assetc atlas manifest.json outdir` packs loose frames into an atlas with padding.
- `fengine_assetc pack folder out.pak` archives a folder (entries are compressed unless the format already is, or zlib saves less than a tenth); `fengine_assetc list archive.pak` prints each entry's name, size, stored size and method.

## Input names

Scripts bind actions to raw input names (`fe.input.bind`, `fe.input` defaults):

| Name | Input |
| --- | --- |
| `key:<name>` | A key by SDL scancode name: `key:Return`, `key:Z`, `key:Left Shift`, `key:F5` |
| `mouse:left`, `mouse:right`, `mouse:middle`, `mouse:x1`, `mouse:x2` | Mouse buttons (x1/x2 are the side buttons) |
| `pad:<button>` / `pad:<axis>` | Any connected gamepad: `pad:a`, `pad:dpup`, `pad:leftshoulder`, `pad:leftx`, `pad:righttrigger`. With several pads the strongest value wins. |
| `pad1:<...>`, `pad2:<...>`, ... | One gamepad, numbered from 1 in connection order (a free number is reused after a disconnect) |

Axes take values from -1 to 1; append `+` or `-` (`pad:leftx-`) to bind one direction of an axis to an action. While the developer editor has the keyboard or mouse, presses go to the editor but releases still reach the game, so no key stays held. Losing window focus releases every input.

## Editor (F1, dev builds)

| Tab | Purpose |
| --- | --- |
| World | Pick worlds and entities and inspect transform, sprite, mover, light, emitter, collider and props; list the active lights. These are run-time changes; a collider edit blocks and touches at its new size at once, also while the game is paused. |
| Map | Paint height, walkability, ramps and tags per layer; edit layer links (step or ladder, with the ladder's way up), placements (schema-driven prop forms), zones (typed, with their props form), points (position, facing, layer, **Play from here**), lights (shadows, flicker curve keys), emitters (blend, distort, max, world space), colliders, every environment field (water, sky and backdrops, weather, post including custom passes) and map props; import Blender markers; undo, redo and save |
| Data | Form editing of data tables from their schemas; add and delete records; **Validate and save** |
| Console | Run Lua in the live game; Up and Down recall earlier commands |
| Profiler | Frame, Lua, task, memory and draw statistics |
| Launch | Buttons registered by the game with `fe.devtools.register` (battles, warps, cheats) |

A world holds two copies of its map: the **authored** map (the file plus editor edits) and the **live** map that scripts read and change (`world:set_environment`, `world:set_cell`). Editor edits go to both, so you see them at once; **Save map** writes only the authored map, so run-time changes never reach `map.json`. Environment edits replace the live environment. Light, emitter and collider edits take effect in the running world at once (changed emitters restart; unchanged ones keep running).

Undo and redo are kept separately for each world and map, and survive the world being reloaded after the map was saved. A reload that brings back a different map (unsaved edits discarded, or `map.json` changed outside the editor) clears that world's undo and redo, so they never restore discarded edits. A drag, a typed field or a paint stroke is one undo step. Problems (an invalid layer, a full 32-tag palette, a map that fails validation) appear in the editor's status line and never stop the game. Link, light, emitter and environment edits are checked like the map loader checks them (a ladder needs a way up and both ends inside the grid, a step link two different layers, flicker keys increasing times, post values their ranges) and are not applied while they would make the map unloadable. Sky backdrops and custom post passes are edited as JSON. In prop forms, the **x** beside an optional field removes it, so its default applies.

Prop forms come from the schemas scripts declare, so a placement's sprite look is editable when its type declares the fields and passes them to the sprite. The kit's built-in types take only a sprite path; a game type can add the rest:

```lua
field.define("glow", function(world, p)
  local props = p.props
  return world:spawn({ id = p.id, position = p.position, layer = p.layer,
    sprite = { animation = props.sprite, blend = props.blend, mode = props.mode,
               silhouette = props.silhouette, pixel_snap = props.pixel_snap } })
end, {
  sprite = { type = "string", doc = "Sprite .anim.json" },
  blend = { enum = { "opaque", "alpha", "additive", "multiply", "premultiplied" }, optional = true },
  mode = { enum = { "upright", "facing", "flat", "decal" }, optional = true },
  silhouette = { type = "any", optional = true, doc = "true, false or a colour {r, g, b, a}" },
  pixel_snap = { type = "boolean", optional = true },
}, "A sprite with an authored look.")
```

Enums become drop-downs and booleans check boxes; a left-out `pixel_snap` keeps the environment's. Encounter zones (`fe.encounter`) describe `table` and `rate` the same way.

The editor calls these `fe.devtools` functions when the game or kit defines them:

| Function | Used for |
| --- | --- |
| `describe()` | JSON text with `schemas`, `entities` (placement types), `zones` (zone types), `tables` and optional `defaults = {entities = type, zones = type}` for new placements and zones; otherwise the first type listed is used. The kit's lists zone types described with `fe.schema.zone` or `field.zone(type, handler, fields)` (the camera kit describes `camera` zones) and `fe.devtools.defaults` (`npc` and `camera`) |
| `play_from(map, point)` | **Play from here**: cancel running scripts and menus and start the field on that map at that point, keeping the game in progress or, from the title screen, starting a new game first (`fe.devtools.new_game`, set by fe.starter); disabled when undefined |
| `validate_table(name, records)` | Data tab validation before saving; returns `true` or `false, message`. The kit's checks ids, duplicates, the table's schema and references to other loaded tables (`fe.data.validate`). Without it, each record is checked with `fe.schema.validate` against the table's schema. |
| `list()`, `launch(name)` | The Launch tab |

Lua errors open an overlay with the traceback. Fix the file and it reloads. Keep authored data in version control; map saves are atomic but overwrite the file.
