# Authoring a game

The sample is the reference for every format: `samples/vale`. Files are UTF-8 JSON or Lua, and paths are relative to the game folder.

## Game folder

```text
game.json                 title, id, window {width,height}, virtual_resolution {width,height}, pixels_per_unit,
                          pause_on_focus_loss (default true), max_fps (default 0), canvas_fit (default "letterbox"),
                          hdr_output (default false), hdr_paper_white (nits, default 200)
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

`id` names the game's per-user folder (saves, settings, `platform.json`, `log.txt`): 1 to 64 of `a-z`, `0-9`, `_` and `-`. Leave it out when the title is plain words (`"Crystal of the Vale"` derives `crystal-of-the-vale`: lower-cased, spaces to `-`); a title with any other character (punctuation, non-ASCII) needs an explicit `id`. Pick it once and never change it: renaming or translating the title keeps the players' saves, changing the id orphans them.

With `pause_on_focus_loss` (the default), the windowed runtime stops ticking the game and pauses audio while its window is minimised or in the background, and resumes where it left off. Replays and headless runs never pause.

`max_fps` caps the frame rate while vsync is off (0, the default, is uncapped; `fengine --max-fps n` overrides it). `canvas_fit` says what the virtual canvas does in a window of another aspect ratio: `"letterbox"` (default) keeps `virtual_resolution` and centres it with bars; `"expand"` widens the canvas to the window's aspect ratio (never below the configured width), so UI anchored with `fe.canvas.width()` reaches the edges of an ultrawide screen. The 3D world fills the window either way.

`hdr_output` (or `fengine --hdr`) presents to a high dynamic range back buffer when the renderer has one: RGBA16F in scRGB (Metal, Direct3D, Vulkan), else RGB10A2 HDR10, else the 8-bit buffer as before. The frame is still composed in sRGB (the post pass grades and encodes each world; the canvas draws on top); the screen pass decodes it to linear light and places its white at `hdr_paper_white` nits (80 to 10000; on macOS the system's own SDR white, which the player sets with the display brightness, stands in for it), so an SDR-authored game is shown without clipping or dimming and the OS does not re-tone-map it. `fe.app.display().hdr` says whether the back buffer is HDR; with `hdr_output` off every pixel is what it was before, so replays and their reference captures are unaffected.

Any file in the game folder with the same path as a library module overrides it. For example, `fe/ui/screens/title.lua` replaces the default title screen.

A game can ship its own shaders as `shaders/vs_*.sc` and `fs_*.sc` (with an optional `shaders/varying.def.sc`; the engine's is used otherwise). `tools/package_game.py --shaderc <shaderc> --shader-include <build>/shader-include` compiles them for every backend of the package into `game/shaders/<backend>/<name>.bin`; a game without sources but with precompiled `shaders/<backend>/*.bin` ships those.

Every engine program accepts a `.pak` archive wherever it takes a game folder (`fengine_headless --check game.pak`), and plays `game.pak` when it lies where the folder would. `fengine_assetc pack <folder> <out.pak>` writes one.

## Data tables

`fe.rpg.database.load()` (called by `fe.starter`) loads the standard tables when they exist: `classes`, `actors`, `skills`, `items`, `states`, `enemies`, `troops`, `shops` and `encounters`; the starter also loads `quests` through `fe.quest`. A file is an array of records with an `id`, or an object keyed by id. Schemas live in Lua (`lualib/fe/rpg/database.lua`). They check types and cross-table references, and they drive the editor's Data tab. Schemas are open: extra fields are kept, and `schema.extend(name, fields)` adds validation for them. Your own tables use `data.load(name, path, schema)`.

| Table | Main fields |
| --- | --- |
| classes | `name`, `growth` (stat per level), `curves`, `skills [{skill, level, jp}]`, `equip` (types), `commands`, `passives`, `support_slots`, `modifiers` |
| actors | `name`, `class`, `level`, `stats`, `sprite`, `battle_sprite`, `portrait`, `skills`, `equipment`, `slots`, `support_slots` |
| skills | `name`, `type`, `target`, `scope`, `cost {resource: n}`, `hits`, `effects [...]`, `element`, `damage_type`, `uses_weapon` (take them from the weapon), `always_hit`, `accuracy` (percent), `power` (default for the effects), `timeline`, `passive`, `support`, `modifiers`, `resist` (a passive's), `priority`, `tags` |
| items | `name`, `kind`, `price`, `stack`, `use {target, effects, consume}`, `equip {slot, type, modifiers, skills, element, damage_type, resist}` |
| states | `name`, `turns`, `modifiers [{stat, add/mul/set, priority}]`, `prevents_action`, `on_turn` effects, `removed_by`, `stacking` |
| enemies | `name`, `sprite`, `scale`, `level`, `stats`, `skills`, `ai` (function path `"ai.goblin.think"`), `actions` (actions per turn: an integer, or a function path returning one each turn), `resist {key: multiplier}`, `rewards {exp, money, drops}` |
| troops | `name`, `members [{enemy, position {slot}, name}]`, `stage`, `music`, `escapable`, `script` |
| encounters | per-area tables with steps, weights and danger levels (fe.encounter) |
| shops, quests | stock and prices; quest stages with objectives, chapters and journal text |

`resist` tables (enemies, equipment and passives) map a key to a multiplier: an element or damage type scales damage of that kind (`{"fire": 2, "sword": 0.5}`), and `"state:<id>"` scales the chance that the state lands (`{"state:poison": 0}` makes it immune, `0.5` halves the chance); several sources multiply.

Effects are tables with a `type` handled by `fe.effects` (`damage`, `heal`, `resource`, `add_state`, `remove_state`, `buff`, `revive`, `script`, or your own handlers) plus the fields of that type: `damage` and `heal` take `formula`, `power`, `resource`, `variance`, `element`, `damage_type`, `accuracy`, `always_hit` (and `damage` also `no_critical`, `drain`); `resource` takes `resource`, `amount` or `fraction`; `add_state` takes `state`, `chance`, `turns`; `remove_state` a `state` or a `tag`; `buff` a `stat` with `add` or `mul`, `turns`, `id`, `cap`; `revive` a `fraction`; `script` an `fn`. Every effect record is validated when the tables load: its type must have a handler and its fields must fit the handler's schema (`effects.schema(type, fields)` registers one for your own handler; `effects.provider(type, module)` names the module to load for a type a kit provides).

## Maps

`map.json` uses format 2. The engine stores and queries a map's contents but does not interpret them.

- `grid`: `width`, `depth`, a `tags` name list and `layers`. On a map with a `bake` section the rows are derived output: the editor (or `fengine_headless --bake <game>`) writes them from the geometry and the placed models, so edit them through the editor's overrides rather than by hand. Each layer holds rows of text: `height` (space-separated numbers), `walk` (`1` walkable, `0` or `#` not; any other character is an error), `ramp` (`.` or the rising direction `n e s w`), `rise` (space-separated numbers: how far each ramp climbs across its cell, default 1) and `tags` (space-separated hex bitmasks indexing the tag list; a bit beyond the list is an error). Use extra layers for bridges and overpasses, and `links` (`{x, z, from, to, dir, kind}`) to connect them or to place ladders (see below). `"wrap": true` makes the map toroidal, for an overworld: the east edge joins the west and the south the north, so heights, walkability, tags, links, path finding, walking, jumps, collisions, contacts, `world:query` and the camera continue across the seam, and the renderer draws the map, its meshes, sprites and lights again under each mirror the camera sees. Positions stay in `[0, width) × [0, depth)` (`world:wrap_point(p)` folds one in; `world:wraps()` says whether the map wraps), so take differences between two positions with `world:wrap_delta(a, b)` (the kits do: followers, encounters, interaction range, the camera rig) instead of subtracting them. Rules of the seam: a ladder may not straddle it (both ends inside the rectangle, an error otherwise); `points` lie inside the rectangle; zones, platforms and collider boxes are measured in plain coordinates, so keep them inside the map too; rays (`world:raycast`, line of sight), decals, particles, overlays and distortion do not wrap; and a body is assumed to move less than half the map in one tick (speeds are capped at 1000 units/second, so only a map under 34 cells could break that). The camera rig does not clamp to `min`/`max` on a wrapping map.
- `points`: named positions (`{position, facing, layer}`) for spawns, battle stage slots and cutscene marks.
- `placements`: `{id, type, position, facing, layer, yaw, scale, props}`. `fe.field` spawns them by `type`; props are type-specific and include event `pages`. `layer` must exist in the grid. `yaw` (degrees) and `scale` (a number or `[x, y, z]`) are optional and apply to the spawned entity: meshes turn and scale, sprites scale.
- `zones`: named rectangles with a type, props and a `layer` (`-1` = every layer), which fire `zone_enter` and `zone_exit` for entities with a mover or a collider. Ids must be unique and non-empty (presence inside a zone is tracked by id). For per-cell triggers (traps, event tiles) listen for `cell_enter` instead, which a mover emits whenever its cell or layer changes.
- `lights`: point or spot (`inner_cone`, `outer_cone` in degrees from the axis, 0 ≤ inner < outer ≤ 90, `direction`), with `radius` (above 0, at most 1000), `intensity`, and flicker. `flicker_curve` is a list of `[seconds, scale]` keyframes that loops; `flicker` and `flicker_speed` add a noise wobble on top. `shadows: true` makes the light cast shadows; per world up to 16 spot lights, or two point lights and four spot lights, cast at once, chosen by brightness and closeness to the camera focus (see [Rendering](architecture.md#rendering)). `emitters` (particles; `max` caps live particles, `world_space: false` makes them travel with the emitter, `blend` is `alpha` (default), `additive`, `multiply`, `premultiplied` or `opaque`; additive particles with colours above 1 glow through bloom; `distort` > 0 turns them into heat haze that bends the scene behind them instead of drawing), `colliders` (extra solid boxes).
- `bake`: derived walkability. `auto` (the editor re-bakes as models are placed and moved), `fingerprint` (what the bake read; `fengine_headless --check` warns when it is out of date and `--bake <game> --check-only` fails), `options` (the bake's settings: `layer`, `samples` per cell side, `max_slope`, `clearance`, `step`, `min_coverage`, `height_step`, `prune_islands`, `block_edges`, `box_drops`), `colliders` (the quarter-cell boxes the bake made for walls, posts and drops; the runtime adds them to the map's colliders) and `overrides`: rectangles `{x, z, layer, w, d}` with any of `walkable`, `height`, `ramp` (`n e s w`, with `rise`), `tags` and `untags`, applied in order after every bake, which is how an author's corrections survive re-bakes (a door on the blocked edge ring: `{"x": 9, "z": 0, "w": 2, "d": 1, "walkable": true}`). A map without the section is hand-painted and left alone.
- `environment`: `ambient`, `sun_color`, `sun_direction`, `fog_color`, `fog_density`, `fog_height`, `fog_height_falloff`, `fog_max` (strongest fog blend), `shadows`, `shadow_distance` (how far past the camera focus sun shadows reach), `shadow_strength`, `shadow_bias` (world units), `sprite_light_height` (where sprites sample point lights), `anisotropic` (texture filtering), `pixel_snap` (sprites move in whole scene pixels), `water` (`wave_scale`, `wave_speed`, `fresnel`, `specular`, `deep_color`, `depth_falloff`, `reflection` 0–1 for screen-space reflections of the scene, `refraction` for how far waves bend the scene seen through the surface), `sky` (`mode` `none`/`color`/`gradient`/`panorama`, gradient colours, clouds, `backdrops`; with `none` the background is transparent, so worlds drawn earlier show through), `weather` (`kind`: `rain`, `snow`, `leaves`, `embers`, `dust` or a kind from the game's `data/weather.json`; `intensity` (0 or more), `wind`, `color`, `sprite`), and `post`:
  - bloom: `bloom`, `bloom_strength`, `bloom_threshold`, `bloom_knee` (soft threshold, 0–1 of the threshold), `bloom_radius` (0–1, how much the wider levels add), `bloom_levels` (1–8);
  - depth of field: `depth_of_field`, `dof_mode` (`depth`, `tilt` or `both`), `focus_distance` (0 = the camera distance), `aperture`, `focus` and `focus_range` (the tilt-shift band, each 0–1 down the screen), `dof_radius` (widest blur in screen pixels, 0–32);
  - ambient occlusion: `ssao`, `ssao_radius` (world units), `ssao_strength`, `ssao_bias`;
  - `antialias` (`auto`: FXAA unless `pixel_scale` > 1; `fxaa`; `none`), `chromatic_aberration` (this world only, before tone mapping; the same shift as `fe.screen.set{chromatic}`: with amount a, red and blue split by a × 3% of the distance from the screen centre), `distortion` (scales distort sprites and particles);
  - `vignette`, `vignette_strength`, `exposure`, `tonemap` (`aces` or `none`), `color_grade`, `lut`, `lut_blend_target`, `lut_blend`, `pixel_scale` (a number from 1 to 8: the 3D scene renders at the output size divided by it and is scaled up in the composite; a whole number upsamples with nearest filtering, so every scene pixel is a crisp block for pixel art, and a fraction such as 1.5 bilinearly, which is the dynamic-resolution hook: `world:set_environment{post = {pixel_scale = 1.5}}` from a quality setting or when `fe.dev.stats().frame_ms` climbs);
  - `custom`: game post shaders, `[{"shader": "sepia", "params": [1, 1, 0.82, 0.6]}]`, at most 8 (see [Post shaders](#post-shaders)).

  Environment colours are linear values.
- `props`: free data read by Lua, such as `music`, `camera`, `battle_stage`, `encounters` and `daynight`.

Scripts can change the environment at run time with `world:set_environment{...}`. Those changes are not written back into the map file. Every post field can be animated that way (for example a `fe.tween` on `aperture` or `bloom_strength`); `custom = {}` removes the game's post shaders.

### Post shaders

A game adds full-screen passes with bgfx fragment shaders in `shaders/`. `post.custom = [{"shader": "sepia", "params": [...]}]` runs `shaders/fs_sepia.sc` after tone mapping and grading (before FXAA), on the world's image at output resolution:

```glsl
$input v_texcoord0
#include <bgfx_shader.sh>
SAMPLER2D(s_input, 0);    // the graded image so far, premultiplied: rgb already times alpha, the world's coverage
SAMPLER2D(s_depth, 1);    // the scene's depth buffer
uniform vec4 u_params[4]; // `params`, up to 16 numbers in order: u_params[0].x is the first
uniform vec4 u_info;      // 1 / width, 1 / height, seconds, texture origin bottom-left (0/1)
void main() {
    vec4 color = texture2D(s_input, v_texcoord0);
    gl_FragColor = vec4(mix(color.rgb, vec3_splat(dot(color.rgb, vec3(0.3, 0.59, 0.11))), u_params[0].x), color.a);
}
```

Names use `a-z`, `0-9`, `_` and `-`. The input is premultiplied (where the world is transparent its colour is black, and bloom that reaches past the geometry is colour with no alpha), so scale `rgb` and `a` together and keep the input alpha unless the pass should change what shows through; the last pass is blended ONE/INV_SRC_ALPHA over what is underneath. Development builds compile a source that is newer than its binary with the build's `shaderc` (into a cache in the temp folder) on first use and again when it changes; compile errors appear in the log and the pass is skipped. Every build loads `shaders/<backend>/fs_<name>.bin` (a loose file or an entry of `game.pak`) when there is no newer source, and ship builds only that: `metal` (macOS), `dx11` (Windows), `spirv` and `glsl` (Linux). `tools/package_game.py` compiles `shaders/fs_*.sc` for the packaged backends; to compile by hand, use the engine's shader settings:

```sh
shaderc -f shaders/fs_sepia.sc -o shaders/metal/fs_sepia.bin --type fragment --platform osx --profile metal \
  --varyingdef <engine>/shaders/varying.def.sc -i <bgfx>/src -i <engine>/shaders
# dx11: --platform windows --profile s_5_0   spirv: --platform linux --profile spirv   glsl: --platform linux --profile 330
```

### Game shaders

Mesh materials and sprites can replace the engine's fragment shader with the game's own, compiled and loaded like post shaders (`shaders/fs_<name>.sc`, same names, same development compile and `shaders/<backend>/fs_<name>.bin` for shipping; the engine's shader folder is on the include path, so `lighting.sh` is available):

- a glTF material names one in its custom properties (`extras`, directly or under `fengine`): `{"shader": "glint"}`, or a node's custom property `shader` applies to the node's materials; a mesh component's `shader` (`mesh = {model = ..., shader = "glint"}`) replaces it for every part of that entity;
- a sprite component takes `shader` (`sprite = {..., shader = "hologram"}`; `""` is the engine's).

Every such draw gets `uniform vec4 u_params[4]`, 16 numbers set per entity from a script (`entity:shader_params{...}`; zero until set), for dissolve amounts, hit timers and the like, and `u_info` as below. The shader replaces the colour pass. Shadows and silhouettes keep the engine's depth-only shader unless the game also ships `shaders/fs_<name>_shadow.sc`: that variant pairs with the same vertex shader and reads the same inputs, samplers and uniforms as `fs_<name>.sc`, and the sun and light shadow passes and the sprite silhouette pass use it in place of the engine's for that shader's draws. It should discard exactly what the colour shader discards; for sprites it should return `v_outline` (the silhouette colour; in the shadow passes its colour is ignored, only the discards count). Without it a dissolving sprite keeps a full shadow and silhouette. In the same way a sprite shader's `distort` draws use `shaders/fs_<name>_distort.sc` when the game ships it, and the engine's `fs_distort.sc` otherwise: the variant takes `fs_distort.sc`'s inputs (`vs_sprite`'s outputs, `s_albedo` at 0, `s_sceneDepth` at 1, `uniform vec4 u_distort[2]`: time and the atlas texel size, then 1 / scene width and height) plus `u_info` and `u_params`, and writes screen offsets (fractions of the screen, `v_spriteParams.y` is the sprite's `distort` strength) into the RG distortion buffer, as `fs_distort.sc` does; copy it to start. A dissolving shock wave can then thin out its lens with the same parameter as its colour. A mesh shader reads what `vs_world` (or, for instanced draws, `vs_world_instanced`) writes and gets what `fs_world.sc` gets (`fs_water.sc`'s for water materials):

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
uniform vec4 u_sprite[2];         // atlas texel w, h, palette (0/1), alpha cutoff; blend output, has normal map, has emissive, outline width (texels)
uniform vec4 u_info;              // as above
```

Discard below the alpha cutoff (`u_sprite[0].w`, `u_material[1].x`) so opaque draws keep their cutout, and end with `blendOutput(color, alpha, v_worldPos, mode)` so blend modes and fog work. Names use `a-z`, `0-9`, `_` and `-` (at most 64); a script naming another is an error, and a glTF name that is not valid is logged and ignored. A shader that does not compile, cannot be loaded, or whose `$input` does not match the vertex shader's outputs is reported once in the log, with the error, and the engine's shader is used. `samples/vale/shaders` has one of each (`fs_glint.sc`, `fs_hologram.sc`).

### Links between layers

A link joins two layers at one cell, usually the foot of a ramp on the upper layer. Path finding and walking use the same rule, so a planned path can always be walked:

- With `dir` (`n`, `e`, `s` or `w`), moving along `dir` inside the cell switches from `from` to `to`, and moving against it switches back.
- Without `dir`, the switch happens only where the current layer ends (the next cell is not walkable on it).
- The other layer's cell must be walkable, and the two surfaces must be within 0.25 of each other (and within the walker's step limit) where the switch happens. Shape the ramp so its low edge meets the lower surface on the side walkers enter from: for a directed link that edge (the one against `dir`) is the junction, and while a walker is inside the link cell it may stand on either layer as long as the surfaces meet there, so a planned path is walkable for any body radius up to 0.49.

A link with `"kind": "ladder"` is a ladder up a face of any height: its foot is the cell `(x, z)` on layer `from` and its top the neighbouring cell in `dir` (required) on layer `to`, which may be the same layer, as for a ladder up a cliff. Path finding takes the ladder from its foot heading along `dir` or from its top heading against it, and marks that step `climb`; movers climb it at their `climb_speed` (emitting `climb_start` and `climb_end`), on a planned route or when steered into it.

`entity:move_to(target, {layer = 1})` targets a layer explicitly; without `layer` the walker stays on its layer when it can, else picks the walkable layer whose surface is nearest the target's height. By default the plan uses the mover's step limit and gap jumps, diagonal steps, ladders and its tag class; path rules passed with the move (the table `world:path` takes: `diagonal`, `blocked_tags`, `required_tags`, `blocked`, `cost`, ...) plan the route instead, their tags replacing the mover's for that plan. A mover's `footprint` (cells per side, anchored at its cell towards +x and +z) and `fly` (crosses and stands on unwalkable cells and height steps; grid bounds and blocked tags still hold) apply to planning and standing alike. The footprint is grid occupancy, for planning and standing; collisions between bodies use the collider's radius (up to 64), not the footprint.

A mover's **passability class** is its `blocked_tags` and `required_tags` (`e:mover{blocked_tags = {"lava"}}`, `e:mover{fly = true, required_tags = {"water"}}`), names from the map's tag list (an unknown name is an error; `e:mover()` reads them back as name lists). It never stands on, steps onto, is carried onto, lands on or paths through a cell carrying a blocked tag or, when `required_tags` is not empty, a cell carrying none of them, flying or not. That is how vehicles work: a boat is a flying mover that requires `water`, so it floats over unwalkable water cells, plans along the river and cannot be steered onto the bank; a ground unit that may not enter lava blocks `lava`; a swimmer is a walker whose class allows water. Path rules (`world:path`, `move_to` rules) take `required_tags` the same way. An airship adds `altitude`: `e:mover{altitude = 3, altitude_speed = 4}` lifts the body that far above the grid surface (its feet rest at surface + `lift`, which `e:mover().lift` reads and which approaches `altitude` at `altitude_speed` units per second; 0 applies it at once). The lifted body passes over collider boxes and bodies whose tops lie below its feet and keeps its cell, zones and tags (still bounded by its class and the grid), flies past ladders, keeps its lift through `place` and up and down ramps, and comes down for a `launch` (the lift eases back up afterwards). For a blob shadow under it, attach a flat sprite with `shadow:attach(ship, nil, {ground = true})`: a ground attachment adds its offset to the grid surface under the parent instead of the parent's position.

A command given during a jump, a launch or a climb ends it first: the jump lands on the ground under the body (over a void, at the nearer end of its arc) with `landed`, the launch on the highest standable surface under the body, the climb steps off at the nearer end of the ladder with `climb_end`. A jump whose landing lies inside a collider box or a solid entity lands on the nearest free spot within one unit, or is refused with `blocked`. `entity:launch(velocity)` throws the body: it flies a ballistic arc under the world's gravity (`world:set_movement_tuning{gravity = 20}` units per second squared), keeps its horizontal speed until a solid entity, a collider box, a cliff face or the grid's edge stops it (it then drops straight down), and lands on the first surface it falls onto that its class lets it stand on, on any layer (a fall from above a bridge lands on the bridge), or over nothing standable on the lowest layer's surface; a landing inside a box or body moves to the nearest free spot within one unit, with none it lands anyway and `blocked` follows `landed`. It emits `jump_start` and `landed` like a jump; a zero velocity from a height is a fall.

### Weather

`weather.kind` names a preset in `@fe/assets/weather.json`: `rain`, `snow`, `leaves`, `embers`, `dust` and `custom`. A preset is an emitter (`rate`, `max`, `area`, `velocity`, `lifetime`, `size`, `color`, `color_end`, ...) plus an `origin` offset from the camera target. Put a `data/weather.json` in the game folder to add kinds or replace presets; it is read when a world first shows weather. `wind` is added to the preset's velocity, a non-white `color` replaces the preset's colours (keeping its fade), and `sprite` replaces its particle image.

### Blender

A map's own geometry is its terrain: ground, paths, cliffs, water and stairs. Model at one unit per grid cell with the origin where cell (0, 0) begins, apply transforms and export glTF 2.0 (`.glb`) with normals. Buildings, trees and fences are better placed as props (the editor's Assets panel, `fx/`): models with their origin at the centre of the base and a footprint of whole cells, which the walkability bake reads as placed. The bake reads a model's visible parts, or, when any node carries the custom property `collision` (truthy), the parts under those nodes alone, which can be hidden with `"visible": false`: a box over a house's walls so its roof overhang does not block, a trunk under a tree's crown, a slope under stairs. Collision pieces should be at least a quarter of a cell thick (`tools/generate_samples.py` writes `samples/vale/fx` this way). Textures (with their sampler's wrap and filter modes; mip chains are generated), normal maps (with tangents, or tangents are generated from the UVs), emissive textures and colours (`KHR_materials_emissive_strength` multiplies them; above 1 they glow), vertex colours (values above 1 glow), alpha (opaque, mask or blend) and node TRS animations are supported. Only the nodes of the file's scene are shown. Skinning and Draco compression are not. Custom properties on a node, either directly or under a `fengine` key, act as markers:

```json
{"spawn": "entry"}
{"collision": true, "visible": false}
{"material": "water"}
{"shader": "glint"}
```

`KHR_lights_punctual` point and spot lights, with spot cones (held within 90 degrees; equal inner and outer angles get a half-degree edge) and direction, are imported as lights; directional lights are ignored (use the environment's sun). In the editor, **Import markers** turns spawn and light markers into points and lights (collision is the bake's business). Save the map afterwards.

### Compressed textures

`fengine_assetc cook <game> [--force]` converts the PNG textures of world geometry and meshes (images that `.gltf`/`.glb` materials reference by file; images embedded in a `.glb` are not) and of map backdrops and panoramas to BC7 `.ktx` files with full mip chains, written next to each PNG. Colour textures are sRGB, normal maps linear. The runtime uses a `.ktx` when the GPU supports BC7 and falls back to the PNG otherwise; sprites and UI images stay PNG. Only PNGs newer than their `.ktx` are cooked again unless `--force` is given. BC7 encoding is slow (several seconds per 64×64 texture in a debug build), so cook with a release build of `fengine_assetc`.

## Sprites

An `.anim.json` file names an `atlas` image, an optional `pixels_per_unit`, and `clips`. Particles that use a sprite play its `idle` clip (or its first clip) over each particle's life. Each clip has `loop` and `directions` (1, 4 or 8). Each direction is a list of frames `{rect: [x, y, w, h], duration, event?}`. Rows run clockwise relative to the camera, starting with the row that faces the viewer: 4 directions are south, west, north, east; 8 directions are south, south-west, west, north-west, north, north-east, east, south-east. A fixed `sprite.direction` beyond a clip's rows uses the nearest row. Frame `event` strings appear as `anim_event` world events, which is useful for footstep sounds and hit timing; an event on the first frame fires as the clip starts. A clip name the sheet lacks plays its `idle` clip (or its first clip): frames, events and `anim_finished` (with the name that was asked for) all follow that clip. The kits use the clip names `idle`, `walk`, `attack`, `cast`, `hurt`, `ko` and `victory` where they exist.

An `.anim.json` may also name a `normal` image (tangent-space: red points right, green up, blue towards the viewer) and an `emissive` image, both laid out exactly like the atlas. With a normal map the sprite is lit per pixel, so point and spot lights shade it as they pass; the emissive image's colour, times its alpha, is added unlit (eyes, lanterns, runes).

A sprite component (`world:spawn{sprite = {...}}`, `entity:sprite{...}`) takes `animation`, `clip`, `move_clip`, `idle_clip`, `speed` (-1000 to 1000), `direction`, `visible`, `unlit`, `cast_shadow` (only opaque sprites cast; blended ones and opaque ones with a tint alpha below 1 cast nothing), `shader` (see [Game shaders](#game-shaders)), `scale` (0 to 1000), `depth_bias`, `offset` (pixels), `tint`, `flash`, `outline` (a colour drawn around the opaque pixels; `outline_width` is how far it reaches in atlas texels, 0.5 to 4, default 1: the shader looks that far along each axis, so a width above 1 skips the texels between and reads as a thicker line on sprites whose edges are at least that thick), `palette`, `palette_row`, and:

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

Axes take values from -1 to 1; append `+` or `-` (`pad:leftx-`) to bind one direction of an axis to an action. The suffix is read only on analog inputs (gamepad names, or inputs an axis lists as `analog`), so keys whose scancode names end that way (`key:-`, `key:Keypad -`, `key:Keypad +`) bind as they are. While the developer editor has the keyboard or mouse, presses go to the editor but releases still reach the game, so no key stays held. Losing window focus releases every input.

## Editor (F1, dev builds)

F1 opens the editor; `fengine <game> --tools` starts with it open. It is a dockable workspace around a **Scene** viewport that shows the running game through the editor's own camera (right-drag orbits, middle-drag or Shift+right-drag pans, the wheel zooms, F frames the selection, Home the map, O toggles orthographic). **Play (F5)** hides the panels and gives the window back to the game; **Edit (F5)** brings them back and pauses the game (a toolbar button runs it in place while you edit). The layout is saved to `<user data>/editor.ini`; View > Reset layout restores the default. The [editor guide](editor.md) walks through making a map from scratch.

| Panel | Purpose |
| --- | --- |
| Scene | Click to select; drag a selected placement, point or entity along the ground (snapped to cell centres, Ctrl for free); W/E/R switch to the move, rotate and scale gizmos (Snap toggles 0.5-unit, 15-degree and 0.25 steps); Ctrl+click adds to the selection, which then moves, duplicates (Ctrl+D) and deletes (Del) together. Zones, points, lights, emitters, collider boxes, trigger colliders and placements without an entity are drawn as handles; View toggles each kind, the labels and the script badges (talk, touch, auto, loop). Drag a sprite or model from Assets onto the ground to place it. |
| Hierarchy | Points, placements by type, zones, lights, emitters, colliders and the live entities no placement made, with a filter, per-group visibility, Add buttons, and a context menu (Focus, Duplicate, Delete, Play from here on points). |
| Inspector | The selection's form: a placement's id, type, position, facing, yaw and scale, its props from the type's schema (pickers for sprites, models, sounds, scripts, maps, points and table ids; lists and records as rows), its live event pages with the active one marked and a Run button, and its live entity; zones, points, lights, emitters and colliders likewise. Props are edited in the authored map and the running game at once (a placement respawns through `fe.devtools.placement_changed`). |
| Map | **Walkability** (Auto bake, Bake now, options, the bake's status and the overlays; see below), the override brush (walkable, height, ramp, tags; brush, rectangle or flood fill; Clear override), layer links, environment (sky, backdrops, weather, post, custom passes as forms), map props (from the `map_props` schema), import of Blender markers, undo, redo and save. |
| Assets | The game folder: folder tree, thumbnails or a list, search and kind filters, details (clips of a sprite, nodes and markers of a model, who uses the file), **Import...** (or drop files onto the panel) copies files in, a loose image in `sprites/` opens the sprite sheet wizard that writes its `.anim.json`, and a **missing** button lists references no file supplies. |
| Scripts | What triggers what on the map (every scripted placement and zone, its trigger and module, the active page and whether it runs), the running tasks (with Cancel), and the last world events the field handled. |
| Data | Form editing of data tables from their schemas; add and delete records; **Validate and save** |
| Console | Run Lua in the live game; Up and Down recall earlier commands |
| Profiler | Frame, Lua, task, memory and draw statistics |
| Launch | Buttons registered by the game with `fe.devtools.register` (battles, warps, cheats) |

**Maps** (File menu): New map (flat ground the size of the grid with a `start` point and Auto bake on, optionally with a Blender export as its geometry), Open map, Save (Ctrl+S), Save as (writes the map as you see it under a new id and opens it), Rename, Delete, Import geometry (copies a `.glb` in as `maps/<id>/geometry.glb` and imports its markers) and Import assets. Opening or making another map while the current one has edits asks to save or discard them, and so does a map change the game makes while playing (a door or a warp zone): `fe.field.warp` asks the editor through `fe.dev.leave_map` and waits for **Save, then continue**, **Discard edits and continue** or **Cancel**, which drops that warp and keeps the map as edited.

**Walkability** is derived. One cell is one world unit and the grid stays the unit of path planning; the bake (`engine/editor/WalkBake.h`, UI-free, also `fengine_headless --bake <game>`) samples each cell of the terrain plus every placed model, as placed, with rays from above: a cell is walkable when at least `min_coverage` of it is level ground with head room, and every quarter of a walkable cell that is not ground (a wall, however thin, a post, a drop) becomes a **collider box** (`bake.colliders`), so bodies stop at the geometry itself while planned routes avoid the cells a box stands on (shown in orange, "cells routes avoid"). It finds heights, ramps, water (tagged `water`), ground nothing leads to (roofs, ledges without a ramp) and blocks the outer ring. With **Auto bake** on, placing, moving, turning, scaling or removing a model re-bakes the cells it covered and covers, joined to the same undo step; **Bake now** does the whole map, **Options...** sets the limits and a height band for a bridge deck. A prop with `solid = false` is left out (decoration), and a prop with event `pages` keeps a round collider instead (it shows and hides itself). The brush records **overrides** (`bake.overrides`) that reapply after every bake; **Clear override** hands a cell back. The status line says when the bake is out of date (its fingerprint covers the geometry, the baked placements, their model files and the options); `fengine_headless --check` warns too, and the `bake_check_vale` test keeps the sample's committed grid equal to its bake.

A world holds two copies of its map: the **authored** map (the file plus editor edits) and the **live** map that scripts read and change (`world:set_environment`, `world:set_cell`). Editor edits go to both, so you see them at once; **Save map** writes only the authored map, so run-time changes never reach `map.json`. Environment edits replace the live environment. Light, emitter and collider edits take effect in the running world at once (changed emitters restart; unchanged ones keep running); a moved placement moves its entity, and any other placement edit respawns it.

Undo and redo are kept separately for each world and map, and survive the world being reloaded after the map was saved. A reload that brings back a different map (unsaved edits discarded, or `map.json` changed outside the editor) clears that world's undo and redo, so they never restore discarded edits. A drag, a typed field, a gizmo move or a paint stroke is one undo step; the map is serialised when the step starts and once more when the mouse is released, not on every frame of the stroke (a world replaced in the middle of a stroke, by **Play from here** or a warp, loses its history: the safe side). Problems (an invalid layer, a full 32-tag palette, a map that fails validation) appear in the status line and never stop the game. Link, light, emitter and environment edits are checked like the map loader checks them (a ladder needs a way up and both ends inside the grid, a step link two different layers, flicker keys increasing times, post values their ranges) and are not applied while they would make the map unloadable. In prop forms, the **x** beside an optional field removes it, so its default applies, and **+** sets one; fields the schema does not know stay in an "Other fields (JSON)" box.

Prop forms come from the schemas scripts declare, so a placement's sprite look is editable when its type declares the fields and passes them to the sprite. The kit's built-in types take a sprite path; a game type can add the rest:

```lua
field.define("glow", function(world, p)
  local props = p.props
  return world:spawn({ id = p.id, position = p.position, layer = p.layer,
    sprite = { animation = props.sprite, blend = props.blend, mode = props.mode,
               silhouette = props.silhouette, pixel_snap = props.pixel_snap } })
end, {
  sprite = { type = { asset = "sprite" }, doc = "Sprite .anim.json" },
  blend = { enum = { "opaque", "alpha", "additive", "multiply", "premultiplied" }, optional = true },
  mode = { enum = { "upright", "facing", "flat", "decal" }, optional = true },
  silhouette = { type = "any", optional = true, doc = "true, false or a colour {r, g, b, a}" },
  pixel_snap = { type = "boolean", optional = true },
}, "A sprite with an authored look.")
```

Enums become drop-downs, booleans check boxes, `{asset = "sprite"|"image"|"model"|"audio"|"lut"|"shader"}` an asset picker with thumbnails (audio also offers the ids of `data/audio.json`), `{module = "events"}` a picker of the Lua modules under that folder, `{map_id = true}` a map picker, `{point = "map"}` a picker of the points of the map the sibling field names, `{ref = "items"}` a picker of that table's ids, and lists, records and maps rows and nested groups; every picker accepts a drop from the Assets panel and a right-click to type a value. `fe.data.validate` (the Data tab's check, `fengine_headless --check`) reports targets that do not exist. A left-out `pixel_snap` keeps the environment's. Encounter zones (`fe.encounter`) describe `table` and `rate` the same way. A value that does not match its schema type (a number where the schema says string, after a hand edit of the file) is shown in a JSON editor for that field instead of breaking the form, and a data record whose `id` is not a string is listed by its index. The editor holds the world it shows for the whole frame, so **Play from here** replacing it cannot pull it from under the other panels.

The editor calls these `fe.devtools` functions when the game or kit defines them:

| Function | Used for |
| --- | --- |
| `describe()` | JSON text with `schemas`, `entities` (placement types), `zones` (zone types), `tables` and optional `defaults = {entities = type, zones = type}` for new placements and zones; otherwise the first type listed is used. The kit's lists zone types described with `fe.schema.zone` or `field.zone(type, handler, fields)` (the camera kit describes `camera` zones), the `map_props` schema, and `fe.devtools.defaults` (`npc` and `camera`) |
| `play_from(map, point)` | **Play from here**, Open map and New map: cancel running scripts and menus and start the field on that map at that point, keeping the game in progress or, from the title screen, starting a new game first (`fe.devtools.new_game`, set by fe.starter); disabled when undefined |
| `placement_changed(world, id)` | After a placement was added, deleted, retyped or had its props edited: respawn that one entity from the live map (`fe.field.respawn_placement`); a plain move needs no respawn |
| `validate_table(name, records)` | Data tab validation before saving; returns `true` or `false, message`. The kit's checks ids, duplicates, the table's schema, references to other loaded tables and the files, modules and maps asset fields name (`fe.data.validate`). Without it, each record is checked with `fe.schema.validate` against the table's schema. |
| `tasks()`, `cancel_task(index)` | The Scripts panel's running tasks |
| `inspect_entity(id, world)`, `inspect_entities(world)`, `run_page(id, index)`, `recent_events()` | The Scripts panel's wiring and events, and the Inspector's live pages |
| `list()`, `launch(name)` | The Launch tab |

Lua errors open an overlay with the traceback. Fix the file and it reloads. Keep authored data in version control; map saves are atomic but overwrite the file.
