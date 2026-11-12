# modelkit: game-ready 3D models

This folder is the process for every 3D model that goes into a game on this
engine, written for the AI agents (and people) who generate them: how to
build a model, the scripts that help, and the gate it has to pass.

A model is **ready** when this exits 0:

```sh
python3 tools/modelkit/validate.py --manifest tools/<game>/models.json --engine build/fengine_headless
```

That one command runs six checks on every model the game ships:

| # | Check | What it answers |
| --- | --- | --- |
| 1 | Mesh integrity | Is the mesh sound? Non-manifold edges, loose geometry, zero-area faces, duplicate vertices and faces |
| 2 | Normals and shading | Does light fall on it correctly? Face orientation, degenerate normals, smoothing across hard edges |
| 3 | Topology | Is it built well? Sliver and needle triangles, large n-gons, T-junctions, the leftovers of booleans |
| 4 | UV mapping | Will textures sit on it properly? Missing UVs, overlaps, tiny islands, stretch, texel density |
| 5 | Game readiness | Does it fit the game? Budgets, scale, origin, transforms, materials, textures, rig, and whether the engine loads it |
| 6 | Visual consistency | Does it look right? A review of its sheet against the game's art direction, signed off against the file's hash |

Checks 1 to 5 are measured. Check 6 is judged: `review.py` draws the model,
someone looks, and the verdict is recorded. `validate.py` then refuses any
model whose review is missing, failed, or older than the file.

## Setup

Python 3.11+ with `numpy` and `pillow` (`pip install numpy pillow`). Building
models needs Blender as a Python module (`pip install bpy`) or Blender itself
(`blender --background --python script.py`). The engine checks need a build:
`build/fengine_headless` to load models, `build/fengine` for engine shots (on a
machine without a display it runs under `xvfb-run` by itself).

`python3 tools/modelkit/selftest.py` proves the validator: it builds one
flawed model per check and expects each to be caught (`--blender` also runs
the Blender helpers).

## The process

Follow these steps in order. Do not skip to export.

1. **Read the brief.** The game's art direction (for Aetherbound,
   `tools/aetherbound/SPEC.md`): the look, the palette, the world's scale, and
   the names the game code relies on (animations, materials, points). Pick the
   model's **profile** (below); it sets the budgets you are building to.
2. **Block out the silhouette first.** Big shapes only, at final size, next to
   a 1.5-unit hero. If it does not read as a black shape from the game camera,
   detail will not save it. Decide where the eye goes; that is where the
   triangles and the texture detail go.
3. **Build it as a generator script**, not by hand: a Python file under
   `tools/<game>/` that builds the model from nothing, so it can be
   regenerated and fixed. Follow [the modelling rules](#modelling-rules).
4. **Prepare** each mesh object before export with the Blender helpers:
   `mk.prepare(objects, sharp_angle=40, origin="base_centre")` inspects it,
   applies transforms, welds, removes degenerate and loose geometry,
   recalculates normals, splits them at hard edges and triangulates. Read what
   it reports: an error it could not clean is yours to fix in the generator.
5. **Unwrap and texture** at the profile's texel density
   (`mk.box_unwrap(obj, pixels_per_unit=32, texture_size=512)` for tiling
   pixel-art surfaces; a real unwrap for anything painted or baked).
6. **Export** with `mk.export_glb(path, objects)`: it sets the options the
   engine needs and nothing else.
7. **Validate** while iterating:
   `python3 tools/modelkit/validate.py model.glb --profile prop --engine build/fengine_headless`.
   Every finding names the mesh and material it is in and how to fix it. Fix
   the generator, not the file.
8. **Review**: `python3 tools/modelkit/review.py --manifest ... model.glb --engine build/fengine`
   writes the review sheet. **Open the image and look at it** against the
   [review questions](#6-visual-consistency). Then record the verdict with
   `--sign pass|fail --by ... --notes ...`.
9. **Register** the model in the game's manifest with its profile, run the
   manifest validation, and commit the model, its generator and the manifest
   together.

A model that fails is not shipped with a waiver to make the run green.
Waivers are for flaws that are deliberate, or known and scheduled; see
[the manifest](#the-manifest).

## What the engine expects

| | |
| --- | --- |
| File | glTF 2.0 binary (`.glb`), textures embedded as PNG or JPEG. No Draco or meshopt compression, no morph targets (shape keys), triangles only |
| Units and axes | One unit is one grid cell; a hero stands about 1.5 units tall. Y is up in the file (Blender's Z up is converted by the exporter). Say which way the model faces in the game's brief; `yaw` turns it when it is spawned |
| Transforms | Applied: the mesh's size in the file is its size in the game. No scaled or mirrored nodes |
| Origin | Props, characters and bosses: the centre of the base (between the feet). Pickups: the centre. A map's geometry: where cell (0, 0) begins |
| Normals | Exported, unit length, facing out. The engine culls back faces unless the material is double sided, so a face wound the wrong way is a hole |
| UVs | One set (`TEXCOORD_0`). Tiling textures may run outside 0..1; baked or painted textures need unique, non-overlapping UVs (profile `"uv": "unique"`) |
| Materials | Base colour (factor and texture), normal map, emissive (colour, texture, `KHR_materials_emissive_strength`; above 1 it glows), vertex colours, alpha opaque, mask or blend. Every material named; one draw call per material per mesh, so few materials |
| Textures | Power-of-two sizes within the profile's limit; pixel-art textures use nearest filtering |
| Rig | One armature, at most four bone influences per vertex, weights that sum to 1, every vertex weighted. Skinning runs on the CPU every frame: keep skinned vertex counts low |
| Animations | Every action exported in the one file, each with the name the game plays (`idle`, `attack`...); `fe.app.model_animations(path)` lists them |
| Extensions | Only `KHR_materials_emissive_strength` and `KHR_lights_punctual`; anything else the file *requires* is an error |

`docs/authoring.md` ("Models and skeletons") is the engine's own account.

## Profiles

`profiles.json` holds the budgets and conventions per kind of model. A game's
manifest may override a value or add a profile under `"profiles"`.

| Profile | For | Triangles | Materials | Texture | Texel density | Origin |
| --- | --- | --- | --- | --- | --- | --- |
| `pickup` | shard, key, coin | 400 | 1 | 128 | 32 px/unit | centre |
| `prop` | crate, brazier, banner, pillar | 3,000 | 3 | 512 | 32 | base centre |
| `set_piece` | statue, great crystal, siege engine | 8,000 | 3 | 1024 | 32 | any |
| `character` | rigged figure of hero height (1.0 to 2.5 units) | 5,000 | 3 | 512 | 64 | base centre |
| `boss` | rigged creature or machine (3 to 12 units) | 20,000 | 5 | 1024 | 32 | base centre |
| `map` | a map's `geometry.glb` | 150,000 | 8 | 1024 | 12 to 40 | corner |
| `default` | anything else | 30,000 | 6 | 2048 | not checked | any |

Texel density is pixels of the base colour texture per world unit; a model may
sit within half to one and a half times its profile's number, and its own
parts may not differ by more than 2.5 times. Budgets are ceilings, not
targets: spend what the silhouette and the camera distance need.

## Modelling rules

These are what a professional would do without being asked, and what the
checks then find nothing to say about.

**Form**
- Silhouette before surface. Exaggerate the two or three shapes that make the
  thing recognisable; drop detail smaller than two texels at game distance.
- Triangles go where the outline curves or the camera looks. A cylinder seen
  from four units away needs 8 to 12 sides, not 32. Flat areas need none.
- Model at final size in world units. Never fix size with a node scale.

**Mesh**
- Separate parts may simply pass through each other (a rivet in a plate, a
  horn in a skull); that is normal game art and cheaper than welding. What is
  not allowed is *half* joining them: faces left inside a join, three faces
  on one edge, two faces in the same plane.
- Do not leave a boolean as it fell. Either avoid it (build the shape from
  loops, or let parts intersect) or apply it and clean the cut: weld, remove
  the slivers, re-cut the n-gon it leaves into quads. `mk.inspect` reports an
  unapplied boolean as an error.
- Quads while modelling; triangulate last, on purpose (`mk.clean` does it,
  quads along the shorter diagonal). Never export an n-gon of more than a few
  corners: a disc is a fan or rings of quads around a centre, not one face.
- Where an edge of one face ends in the middle of another's edge
  (a T-junction), cut the long edge there. Otherwise the renderer can leave
  pinhole cracks along it.
- Delete faces nothing can ever see (the underside of a floor, the back of a
  wall against the map edge), but check the claim from the game camera: the
  review sheet draws every one-sided face seen from behind in red.
- No loose vertices, edges or stray single triangles. No zero-area faces.

**Shading**
- Decide every edge: smooth or hard. Hard edges (about 40 degrees and
  sharper on hard-surface work) get split normals (`mk.clean(obj,
  sharp_angle=40)`); smoothing across a right angle makes the dark gradient
  the shading check reports.
- Thin plates and rims are the usual offenders: their side faces take the big
  face's normal unless the edge is marked sharp.
- Recalculate normals outward after any mirror, flip or negative scale.

**UVs and textures**
- One texel density across the model, the profile's. Bigger where the camera
  dwells is fine within the spread limit; random variation is not.
- No face may collapse to a line in UV space (a face edge-on to a box
  projection): it smears one row of texels across itself.
- Islands smaller than a few texels are wasted texture and shimmer; merge
  them into a neighbour or give the part a flat colour swatch.
- Baked or painted textures: unique UVs, no overlaps except deliberate
  mirroring, a few texels of padding between islands.
- Put seams where the form already breaks (hard edges, undersides, joints).

**Materials**
- Few and named (`colossus_body`, not `Material.003`). Colours, values and
  glow come from the game's palette; emissive is for things that are lit from
  within, not for making a texture brighter.
- Blend is expensive and sorts badly: use opaque or mask unless the thing is
  really translucent.

**Rigs and animation**
- Only bones that deform are in the skin; four influences per vertex at most.
- Name every action; hold the first and last frame of a looping action equal.
- Look at every animation halfway through (the review sheet shows them): bad
  weights show there, not in the rest pose.

## The Blender helpers

`blender/modelkit_bpy.py`, used inside a generator:

```python
import sys; sys.path.insert(0, "tools/modelkit/blender")
import modelkit_bpy as mk

for severity, message in mk.inspect(obj):       # what is wrong while it still has real topology
    print(severity, message)
ok = mk.prepare([body, lid], sharp_angle=40, origin="base_centre")   # inspect, apply transforms, clean, set origin
mk.box_unwrap(body, pixels_per_unit=32, texture_size=512)            # tiling UVs at a fixed density
print(mk.texel_density(body, 512))                                   # what a real unwrap came to
mk.export_glb("samples/<game>/models/crate.glb", [body, lid])
```

| Function | Does |
| --- | --- |
| `inspect(obj)` | Lists n-gons (and concave ones), edges with three faces, loose vertices and edges, zero-area faces, coincident vertices, unapplied modifiers and scale, missing UVs and materials. It sees what the exported triangles no longer show |
| `clean(obj, merge_distance, sharp_angle, triangulate, recalculate_normals)` | Welds, dissolves degenerate geometry, deletes loose geometry, faces normals outward, marks edges sharper than `sharp_angle` as hard, triangulates |
| `apply_transforms(obj)` | Bakes rotation and scale into the mesh |
| `set_origin(obj, "base_centre" \| "centre" \| "keep")` | Moves the mesh so the origin is where the profile wants it |
| `box_unwrap(obj, pixels_per_unit, texture_size)` | Projects each face along its dominant axis at a fixed texel density |
| `texel_density(obj, texture_size)` | The median density of the object's UVs |
| `export_glb(path, objects, animations=True)` | Exports with the options the engine reads |
| `prepare(objects, sharp_angle, origin, report)` | All of the above in order; returns False when an error is left |

`clean` cannot fix design: it will not remove an internal face, cut a
T-junction or re-cut an n-gon well. Those are fixed in how the generator
builds the shape.

## The checks

Every finding has an id (`group.name`), a severity, the mesh and material it
is in, and a fix. **Errors** always fail. **Warnings** fail too, unless the
manifest waives them (or `--lenient` while iterating). `--info` also prints
observations that are not flaws (parts that touch, open borders on a mesh
that is not meant to be closed).

### 1. Mesh integrity

| Id | Means | Fix |
| --- | --- | --- |
| `integrity.nan` | A coordinate that is not a number | The generator divided by zero somewhere; find it |
| `integrity.degenerate_faces` | Zero-area faces: collapsed, or needle-thin | Weld and dissolve degenerate geometry (`mk.clean`) |
| `integrity.duplicate_faces` | The same face twice | Remove doubles; a two-sided sheet is a double-sided material, not two faces wound alike |
| `integrity.non_manifold_edges` | An edge with three or more faces that are not two clean shells touching | Remove the internal face, or separate the parts |
| `integrity.open_edges` | Borders, on a profile that asks for a closed mesh (`"watertight": true`) | Cap or close it |
| `integrity.loose_vertices`, `integrity.loose_parts` | Vertices no face uses; stray single triangles | Delete them |
| `integrity.duplicate_vertices` | Vertices that coincide and carry the same normal and UV | Merge by distance |

### 2. Normals and shading

| Id | Means | Fix |
| --- | --- | --- |
| `normals.missing`, `normals.degenerate`, `normals.not_unit` | No normals, zero-length ones, ones that are not unit length | Export with normals; recalculate |
| `normals.flipped` | Vertex normals pointing into the surface: lit from the wrong side | Recalculate outside |
| `normals.inconsistent_winding` | Neighbouring faces wound opposite ways: one of them is culled | Recalculate outside; check the generator's vertex order |
| `normals.inside_out` | A closed shell wound inwards: invisible from outside | Flip it |
| `normals.shading` | More than 1% of the surface shaded by normals over 80 degrees off the face: smoothing across hard edges | Mark sharp edges (`mk.clean(obj, sharp_angle=40)`) |
| `normals.tangents` | Broken tangents on a normal-mapped mesh | Export without tangents (the engine derives them) or fix the UVs |

### 3. Topology

| Id | Means | Fix |
| --- | --- | --- |
| `topology.slivers` | Over 2% of triangles longer than 40 times their height | Re-cut: move the diagonal, add a loop, merge the thin strip |
| `topology.needles` | Triangles longer than 400 times their height | Almost always a boolean or bevel leftover; dissolve them |
| `topology.ngons` | A flat polygon of more than 16 corners (24 on maps) that the exporter triangulated as it fell | Build it from quads or a fan around a centre |
| `topology.t_junctions` | A vertex in the middle of another face's border edge, in the same surface | Cut the long edge at that vertex |
| `topology.coplanar_overlap` | Faces lying on other faces in one plane, facing the same way: z-fighting | Remove the hidden one, or offset the layer |

### 4. UV mapping

| Id | Means | Fix |
| --- | --- | --- |
| `uv.missing` | A textured material on a mesh with no UVs | Unwrap it |
| `uv.invalid` | UVs that are not numbers | Find the division by zero |
| `uv.collapsed` | Faces with no area in UV space | Unwrap those faces along another axis |
| `uv.texel_density` | The median density is outside the profile's range | Scale the UVs, or change the texture size |
| `uv.texel_density_spread` | Parts of one model differ by more than the limit | Even them out |
| `uv.tiny_islands` | Islands of under a few texels | Merge them or use a swatch |
| `uv.stretch` | Faces whose UVs are stretched more than 3 to 1 | Relax or re-unwrap |
| `uv.out_of_range`, `uv.overlap` | On a `"uv": "unique"` profile: UVs outside 0..1, or islands over each other | Pack them |

### 5. Game readiness

| Id | Means |
| --- | --- |
| `ready.structure`, `ready.engine_load` | Not a readable glTF; or the engine's own loader refuses it (with its message). Never waivable |
| `ready.max_triangles`, `ready.max_vertices`, `ready.max_skinned_vertices`, `ready.max_materials`, `ready.max_draws`, `ready.max_file_megabytes` | Over the profile's budget of that name |
| `ready.scale`, `ready.origin`, `ready.transforms` | Wrong size for the profile; origin not where the profile wants it; scale or rotation left on a node |
| `ready.primitive_mode`, `ready.unindexed`, `ready.morph_targets`, `ready.extensions` | Not triangle lists; no index buffer; shape keys; an extension the engine does not read |
| `ready.no_material`, `ready.material_names`, `ready.unused_material`, `ready.blend`, `ready.emissive` | A mesh without a material; unnamed or default-named materials; materials nothing uses; blend where mask would do; emissive set but black |
| `ready.missing_texture`, `ready.texture_format`, `ready.texture_size`, `ready.texture_pot` | A texture that is not there, not PNG or JPEG, over the profile's size, or not a power of two |
| `ready.skin_attributes`, `ready.unweighted`, `ready.weights`, `ready.influences`, `ready.max_joints` | A skin without joints or weights; vertices no bone moves; weights that do not sum to 1; more than four influences; too many bones |
| `ready.animation_names`, `ready.animation_length`, `ready.animations_missing` | Unnamed or duplicate animation names; a zero-length animation; an animation the profile's `"animations"` list requires and the file lacks |

### 6. Visual consistency

`review.py` writes one image, the review sheet:

- **form** from six sides, shaded by the file's own normals, with every
  one-sided face seen from behind in **red** (a hole or a flipped face);
- **textures** as they land on the surface;
- **texel density** (green on target, blue too low, red too high);
- **silhouette**, large and at the two sizes the game shows it, with a red bar
  the height of a hero;
- the **UV layout** over each texture;
- with `--engine`, **shots from the real renderer** on a neutral stage, lit,
  next to a hero-sized figure, and every animation held halfway;
- every finding of checks 1 to 5.

For a map (profile `map`) the sheet shows the built part of the map from the
game's side and then cut into six parts, each as form and as textures. A map
is also judged in the game itself: look at its replay captures or run it.

Open the image. Judge it against the game's art direction document, answering
each question; any "no" is a `fail`:

1. **Silhouette**: is it recognisable in black at game size, and distinct from its neighbours?
2. **Form**: do the big shapes read first, with detail where the eye should go and rest elsewhere?
3. **Scale**: is it the right size next to a hero, and are its parts in proportion?
4. **Shading**: is the clay view clean: no dark blotches, no faceting where it should be smooth, no red?
5. **Textures**: is the texel density even, with no smears, no seams in plain sight?
6. **Materials**: do the colours, values and glow sit in the game's palette and lighting?
7. **Style**: would it look made by the same hand as the rest of the game?

Then record it:

```sh
python3 tools/modelkit/review.py --manifest tools/<game>/models.json models/crate.glb \
    --sign pass --by "who" --notes "what was looked at and what was judged, including any flaw accepted"
```

Rules for the reviewer, agent or person:

- You must have looked at the sheet for *this* file. The verdict is stored
  with the file's SHA-256; any change to the model voids it.
- The notes say what you saw, not that you looked. Name the flaws you
  accepted and why. "Looks good" is not a review.
- When in doubt, `fail` it, say why in the notes, and fix the generator. A
  failed review costs a rebuild; a wrong pass ships.
- The review cannot be waived.

## The manifest

One JSON file per game lists every model it ships. Aetherbound's is
`tools/aetherbound/models.json`.

```json
{
  "root": "../../samples/aetherbound",
  "art_direction": "tools/aetherbound/SPEC.md",
  "reviews": "model_reviews",
  "folders": ["models", "maps"],
  "profiles": { "boss": { "max_triangles": 12000 } },
  "models": {
    "models/crate.glb": {
      "profile": "prop",
      "waive": {
        "uv.texel_density": { "reason": "By design: only ever seen from across the courtyard.", "max": 1 }
      },
      "review": { "verdict": "pass", "sha256": "...", "by": "...", "notes": "...", "date": "..." }
    }
  }
}
```

- `root`: where the model paths start, relative to the manifest.
- `folders`: searched for `.glb`/`.gltf` files; one the manifest does not
  list fails the run, so nothing ships unvalidated.
- `profiles`: the game's own budgets, over `profiles.json`.
- `waive`: check id to a reason, or to `{"reason", "max"}`. With `max` the
  waiver holds only while the count of that finding stays at or under it, so
  a known flaw cannot quietly grow. Write the reason for the next reader:
  either why it is right for this model ("By design: ..."), or what is wrong,
  where to fix it, and that it is debt ("Known debt: ..."). Remove the waiver
  when the flaw is fixed.
- `review`: written by `review.py --sign`.

Waive a flaw only when it is deliberate, or when fixing it is out of the
change at hand and it is recorded as debt with its fix. Never waive to get a
new model through: a new model starts with no waivers.

## Commands

```sh
# one model, while iterating
python3 tools/modelkit/validate.py out/crate.glb --profile prop --engine build/fengine_headless

# everything a game ships (the gate; also the CTest `models_<game>`)
python3 tools/modelkit/validate.py --manifest tools/<game>/models.json --engine build/fengine_headless

# options: --lenient (warnings pass), --info, --json report.json, --no-review (skip check 6 while iterating)

# the review sheet, then the verdict
python3 tools/modelkit/review.py --manifest tools/<game>/models.json models/crate.glb --engine build/fengine
python3 tools/modelkit/review.py --manifest tools/<game>/models.json models/crate.glb --sign pass --by "..." --notes "..."

# prove the validator itself
python3 tools/modelkit/selftest.py [--blender]
```

## What this does not see

- The automated checks read the exported triangles. They cannot know intent:
  a hole may be a window. That is what the red in the review sheet, and the
  reviewer, are for.
- The review sheet is drawn by a small software renderer (flat faces, no
  shadows); the engine shots are the real renderer but on a neutral stage,
  not in the game's lighting. Judge a model in its scene before calling the
  scene done.
- Animation quality (timing, weight, arcs) is only sampled as one pose per
  animation. Watch the animations in the game.
- Texture *content* (is the painting good) is judged by eye in the sheet, not
  measured.
