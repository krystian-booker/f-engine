# animkit: game-ready animations

This folder is the process for every skeletal or node animation that goes into
a game on this engine (the animations stored in a glTF model: rigged
characters, bosses, animated props), written for the AI agents (and people)
who generate them: how to animate, the scripts that help, and the gate the
result has to pass. `tools/modelkit` judges the mesh, the rig and the textures
of the same file; animkit judges the motion.

A model's animations are **ready** when this exits 0:

```sh
python3 tools/animkit/validate.py --manifest tools/<game>/animations.json --engine build/fengine_headless
```

That one command runs six checks on every animated model the game ships:

| # | Check | What it answers |
| --- | --- | --- |
| 1 | Keyframe integrity | Are the keys sound? NaN, times out of order, quaternions that are not unit length, interpolation the engine does not play as authored, channels on nothing |
| 2 | Motion quality | Does it move well? Single-frame pops, jitter, half-turns between two keys, nothing moving, sparse keys with no easing, keys that say nothing |
| 3 | Loops and hand-overs | Does it join up? A loop's last pose and speed against its first; each one-shot against the animation it comes from and the one it hands over to |
| 4 | Deformation | What does the motion do to the mesh? Skinned at every frame: flying apart, stretching, collapsing joints, feet through the floor or off it, the body leaving its origin |
| 5 | Game readiness | Does it fit the game? Names, required animations, lengths, the moments the game acts on, budgets of keys and joints, and whether the engine reads the same animations |
| 6 | Visual consistency | Does it look right? A review of its sheets against the game's art direction, signed off against the file's hash |

Checks 1 to 5 are measured, on the poses the engine would show (`animkit/rig.py`
evaluates an animation exactly as `engine/assets/Model.cpp` does). Check 6 is
judged: `review.py` draws the motion, someone looks, and the verdict is
recorded. `validate.py` then refuses any model whose review is missing,
failed, or older than the file.

## Setup

Python 3.11+ with `numpy` and `pillow` (`pip install numpy pillow`). Authoring
needs Blender as a Python module (`pip install bpy`; the helpers are written
for 4.4 and later and tested only with 5.3) or Blender itself (`blender --background --python
script.py`). The engine checks need a build: `build/fengine_headless` to load
models, `build/fengine` for engine shots (on a machine without a display it
runs under `xvfb-run` by itself).

`python3 tools/animkit/selftest.py` proves the validator: it builds one
flawed animation per check and expects each to be caught, and clean ones
(among them every false alarm found so far) to pass. `--engine
build/fengine_headless` also proves the engine comparison, `--blender` the
Blender helpers.

## The process

Follow these steps in order. Do not skip to export.

1. **Read the brief.** The game's art direction (for Aetherbound,
   `tools/aetherbound/SPEC.md`): what the character is, how it should move,
   and the names, lengths and moments the game code relies on (`impact` times
   in its data, the `entity:play(name, loop, fade)` calls in its scripts).
2. **Write the spec first.** Add the model to the game's manifest with its
   profile and, under `"overrides"`, [what each animation is meant to
   be](#the-spec): a loop or a one-shot, its length, what it comes from and
   hands over to, the moments the game acts on. Stating it once is what lets
   the checks hold the file to it.
3. **Block the key poses.** For each animation: the start, the extreme, the
   end. Each must read as a silhouette from the game's camera. An action has
   an anticipation before it and a follow-through after it; decide their
   lengths now ([the rules](#animation-rules)).
4. **Animate it in a generator script**, not by hand: a Python file under
   `tools/<game>/` that keys the actions from nothing, so they can be
   regenerated and fixed. Either compute a pose per frame and key it with
   `ak.key_action`, or key a few Bezier keys and `ak.bake` them.
5. **Repair and inspect** each action with the Blender helpers while it still
   has its real curves: `ak.fix_quaternion_flips`, `ak.make_loop` for loops,
   `ak.bake(action, 30)`, then `ak.check_action(action, spec)` and read what
   it says.
6. **Export** with `ak.push_to_nla(armature)` and `ak.export_glb(path,
   objects)`: every action becomes one animation under its own name, sampled,
   starting at time 0.
7. **Validate** while iterating:
   `python3 tools/animkit/validate.py --manifest tools/<game>/animations.json models/golem.glb --no-review --engine build/fengine_headless`.
   Every finding names the animation, the joint and the time, and how to fix
   it. Fix the generator, not the file.
8. **Review**: `python3 tools/animkit/review.py --manifest ... models/golem.glb --engine build/fengine`
   writes the overview and a page per animation. **Open every page and look**
   against the [review questions](#6-visual-consistency), then watch the
   animations in the game. Record the verdict with `--sign pass|fail --by ...
   --notes ...`.
9. **Run the whole manifest**, then commit the model, its generator and the
   manifest together. The same file is also in the game's `models.json`: its
   modelkit review is void too, because the file changed.

An animation that fails is not shipped with a waiver to make the run green.
Waivers are for flaws that are deliberate, or known and scheduled; see
[the manifest](#the-manifest).

## What the engine does with an animation

All of this is `engine/assets/Model.cpp`, `engine/world/Animation.cpp`,
`engine/world/World.cpp` (`playMesh`) and `engine/render/WorldRenderer.cpp`;
`docs/authoring.md` ("Models and skeletons") is the engine's own account.

| | |
| --- | --- |
| What is animated | The translation, rotation and scale of nodes (bones are nodes). Morph-weight channels and channels without a target node are skipped |
| Names | `entity:play(name, loop, fade)` plays an animation by its name in the file. An unnamed one is `animation0`, `animation1`... by position; of two with one name only the first can be played; a name the model lacks shows the rest pose |
| Interpolation | `LINEAR` and `STEP` play as authored. `CUBICSPLINE` plays through its key values with **linear** interpolation: the tangents are ignored. Rotations are slerped the short way round and are **not** renormalised: a quaternion that is not unit length scales and skews the part |
| Time | The clock starts at 0 whatever the first key's time is; before a channel's first key its first value holds, after its last key its last. The animation's length is the latest key of any channel |
| Rate | Animations advance once per tick, 60 times a second, and the model is posed at that time: keys closer together than 1/60 s are never all shown |
| Loops | A looping animation's time wraps (`fmod`) from its length to 0. Its last key must therefore equal its first; the last key itself is never shown, so the copy is not a stalled frame |
| One-shots | Hold their last pose and emit `anim_finished`. An animation of length 0 never finishes |
| Cross-fade | `play(name, loop, fade)` starts the new animation at time 0 and, over `fade` seconds, blends from the old one (which keeps running: a loop keeps turning, a one-shot holds its end) per node: translation and scale mixed, rotation slerped the short way, weight eased in and out. Without `fade` it cuts |
| Root motion | None. An animation moves nodes inside the model; the entity stays where the game put it. A walk cycle is animated on the spot; a body that travels in an animation is back at its origin the moment the next one starts |
| Nodes it does not key | Keep their rest transform, in every animation. Of two channels on one node and property the later one in the file wins |
| Cost | Every node's matrix is composed for every animated entity every tick, and every skinned vertex is skinned on the CPU. An entity animates at most 16 skinned parts (one per material of each skinned mesh); further ones are drawn unposed. A skin has at most 1024 joints, a vertex four influences |

## The spec

What an animation is *meant* to be cannot be read from its keys, so it is
stated in the manifest, once, under the model's `"overrides"` (kitcommon
merges `overrides` into the profile, which is all a check sees):

```json
"models/golem.glb": {
  "profile": "character",
  "overrides": {
    "game_view": [270, 22],
    "required": ["idle", "attack", "hit", "die"],
    "animations": {
      "idle":   {"loop": true,  "seconds": [2, 8], "grounded": true, "in_place": true},
      "attack": {"loop": false, "seconds": 1.2, "from": "idle", "to": "idle", "impacts": [0.5], "grounded": true, "in_place": true},
      "hit":    {"loop": false, "seconds": 0.5, "from": "idle", "to": "idle", "tremble": ["plume_*"]},
      "die":    {"loop": false, "from": {"animation": "idle", "fade": 0}, "in_place": false}
    }
  }
}
```

| Key | Means | What it switches on |
| --- | --- | --- |
| `loop` | `true`: the game plays it looping. Required for every animation | `loops.seam_pose`, `loops.seam_velocity` |
| `seconds` | Its length: a number (within `duration_tolerance`, 0.05 s) or `[least, most]` | `ready.duration` |
| `from`, `to` | The animation it starts in the pose of, and the one that starts where it ends: a name (the game cross-fades) or `{"animation": name, "fade": 0}` (the game cuts) | `loops.handover` |
| `impacts` | Seconds at which the game acts on it (the blow lands, the spell is released): the times in the game's data | `ready.impact`; marked in the speed graph |
| `grounded` | `true`: it stands on the ground throughout (`deform.floating`). `false`: it leaves or passes the ground on purpose (no `deform.underground` either). Without the key a model of a `ground` profile is still checked for sinking | `deform.floating`, `deform.underground` |
| `in_place` | `true`: the body is over its origin when the animation starts and ends (and, in a loop, throughout) | `deform.in_place` |
| `tremble` | `true`, or a list of joint-name patterns (`"shard_*"`): parts that shake on purpose | exempts them from `motion.jitter` |
| `optional` | `true`: the file need not have it | `ready.missing` |

`"required"` lists names the file must have (every name under `"animations"`
is required too, unless `optional`). An animation in the file with no spec
gets `ready.undeclared`: it was only checked for what needs no intent. A key
animkit does not know (`"looping"`) is an error, not a silent nothing.

Two more keys in `overrides` set how the review sheet looks at the model:
`"game_view": [yaw, pitch]` (degrees; yaw 0 looks at the model's front, 270 at
the side it shows when it faces east under the default camera) and
`"ground_view"` (default `[90, 0]`).

## Profiles

`profiles.json` holds the budgets, conventions and thresholds per kind of
animated model. A game's manifest may override a value or add a profile under
`"profiles"`; one model's own values go in its `"overrides"`.

| Profile | For | Animations | Keys | Key data | Nodes | Joints | Stands on y = 0 |
| --- | --- | --- | --- | --- | --- | --- | --- |
| `character` | rigged figure of hero height | 24 | 40,000 | 600 kB | 96 | 60 | yes |
| `boss` | large rigged creature or machine | 32 | 120,000 | 2,000 kB | 128 | 80 | yes |
| `prop` | a prop whose nodes move: door, wheel, lever | 8 | 4,000 | 64 kB | 32 | 16 | no |
| `set_piece` | a large one-off that moves by itself: floating crystal, windmill | 8 | 12,000 | 192 kB | 64 | 32 | no |
| `default` | anything else | 32 | 150,000 | 2,500 kB | 256 | 128 | no |

Budgets are ceilings, not targets. Distances in the thresholds are fractions
of the model's **size** (the largest side of its bounds at rest), so one
number suits a 1.5-unit hero and an 8-unit boss; `profiles.json` names each
one and what it does.

## Animation rules

These are what a professional would do without being asked, and what the
checks then find nothing to say about.

**Poses and timing**
- Pose first. Every key pose must read in silhouette from the game's camera:
  the limb that acts is clear of the body, the line of action is one curve.
- Anticipate, act, follow through. A blow winds up the other way first
  (a quarter to a third of the animation), lands fast, and takes time to
  settle. An action that starts and stops like a switch has no weight.
- Slow in and slow out. Heavy things start slowly and stop hard; nothing but
  a machine part moves at one speed. The engine does not ease: the easing
  must be *in the keys*, so sample the curves (`ak.bake`).
- Limbs move in arcs, not in straight lines between poses.
- Land the blow on the game's time. The impact is one moment: build speed
  into it, stop dead (or pass through), and tell the spec its time. Sounds
  and effects are timed to it.
- Hold what must be read. A pose the player has to see lasts at least four
  frames at 30 a second.

**Loops and hand-overs**
- A loop's last key equals its first, and the motion runs *through* the seam:
  use whole cycles of every periodic layer (breathing, sway, tremble), never
  a curve that happens to stop near where it started.
- A one-shot that returns to `idle` starts and ends in the idle's first pose,
  exactly. The game's cross-fade hides a hair's difference, not a different
  stance. An animation the game cuts into must match the pose it cuts from.
- No root motion: animate on the spot. If the body must travel (a stagger
  back), say `"in_place": false` and chain the animations that continue from
  there with `"from"` and `"to"`; the entity itself does not move.
- A bone an animation does not key is at rest for as long as it plays. An
  attack that leaves the wings out folds them to their rest pose the moment
  it starts: key every bone that the pose you start from holds away from
  rest.

**Keys**
- Key at 30 a second (a pose per frame); 60 only for very fast motion. The
  engine shows 60 poses a second and interpolates linearly between keys.
- Rotations are quaternions, unit length, with consecutive keys on the same
  side of the sphere, and less than 140 degrees apart. A turn of half a
  circle between two keys can go either way round.
- Never scale through zero or below it. Hide a part with 0.0001.
- Deliberate holds and steps are fine. A step the *file* should state is a
  `STEP` channel; a snap should ease over two or three frames.
- A deliberate shake (a tremble, a shiver) is declared in the spec; an
  undeclared one is jitter.

**The mesh under the motion**
- Feet, knees and fists stay on or above the ground in every pose, including
  the kneeling and fallen ones.
- Look at every joint at its extreme. A twist of more than about 90 degrees
  on one bone collapses the mesh around it (the candy-wrapper): spread it
  over two bones.
- A vertex on the wrong bone only shows when that bone moves: the stretch
  check, and the magenta in the review sheet, are how it is found.

## The Blender helpers

`blender/animkit_bpy.py`, used inside a generator:

```python
import sys; sys.path.insert(0, "tools/animkit/blender")
import animkit_bpy as ak

idle = ak.key_action(armature, "idle", frames)      # frames[i] = {bone: {"rotation": (w, x, y, z), "location": ..., "scale": ...}}
wave = ak.new_action(armature, "wave")              # ... or key Bezier curves yourself on a new action
ak.fix_quaternion_flips(wave)                       # no key on the far side of the sphere
ak.make_loop(idle)                                  # last key = first key, tangents matched
ak.bake(wave, 30)                                   # the curves, sampled: one LINEAR key per frame
for severity, message in ak.check_action(idle, {"loop": True, "seconds": 3}):
    print(severity, message)
ak.push_to_nla(armature)                            # one track per action, named after it
ak.export_glb("samples/<game>/models/golem.glb", [armature, mesh])
```

| Function | Does |
| --- | --- |
| `fcurves(action)`, `groups(action)`, `frame_range(action)` | An action's F-curves, however Blender stores them: since 4.4 they live in a channelbag per layer, strip and slot, and in 5.3 `action.fcurves` no longer exists |
| `new_action(obj, name)` | A new action assigned to the object, with its own slot |
| `key_action(obj, name, frames, start=0)` | Keys an action from a pose per frame: LINEAR keys, quaternions normalised and kept on one side of the sphere, bones in quaternion mode |
| `fix_quaternion_flips(action)` | Negates quaternion keys that sit across the sphere from the key before (Blender would interpolate between them the long way round, and a bake samples that) |
| `make_loop(action)` | The last key takes the first key's value on every curve; Bezier tangents at both ends get the slope of the curve running through the seam |
| `bake(action, fps=None)` | Replaces every curve's keys with LINEAR samples at `fps` (default: the scene's), renormalising quaternions. Samples the curves themselves; a rig driven by constraints or IK needs Blender's visual bake first |
| `clean_curves(action, tolerance, degrees)` | Drops the keys linear interpolation reproduces anyway; the components of one property keep the same frames. For baked curves |
| `slide_to_zero(action)` | Moves the keys so the action starts at frame 0 |
| `check_action(action, spec=None, fps=None)` | Lists what is wrong while the action still has its curves: no keys, a default name, NaN, a late start, the wrong length, quaternions not unit or flipping sign or turning over 140 degrees, negative or zero scale, a loop that does not close or changes speed at its seam, curves that are still Bezier |
| `push_to_nla(obj, actions=None)` | Parks each action on its own NLA track named after it and leaves none assigned, so each exports as one animation |
| `reset_pose(obj)` | Bones back to rest, so the exported rest pose is not the last key |
| `export_glb(path, objects, sampled=True)` | Exports with the options the engine needs. `sampled=True`: one animation per NLA track, a key per frame (Bezier curves, constraints and IK arrive as they look), starting at 0. `sampled=False`: one animation per action with its keys as they are: smaller, but only right for baked, cleaned actions that start at frame 0, and only with one armature |

The helpers cannot fix design: they will not add an anticipation, put a foot
on the ground or stop an IK solve from flipping. Those are fixed in how the
generator poses the rig.

## The checks

Every finding has an id (`group.name`), a severity, the animation it is in
(with the joint and the time) and a fix. **Errors** always fail. **Warnings**
fail too, unless the manifest waives them (or `--lenient` while iterating).
`--info` also prints observations that are not flaws.

### 1. Keyframe integrity

| Id | Means | Fix |
| --- | --- | --- |
| `keys.nan` | Key times or values that are not numbers | The generator divided by zero or normalised a zero vector |
| `keys.times` | Key times that are negative or do not increase | Remove doubled keys; do not key before frame 0 |
| `keys.start` | The first key is later than time 0: the engine holds the first pose until then, at every turn of a loop | Start the action at frame 0 (`ak.export_glb` slides it) |
| `keys.quaternion` | Rotation keys that are not unit length (error above ten times `quaternion_tolerance`, or zero length): the part swells or collapses | Normalise before keying (`ak.key_action` and `ak.bake` do) |
| `keys.scale` | Negative scale keys (error: the part is mirrored and its faces culled) or keys of exactly 0 | Stay positive; hide with 0.0001 |
| `keys.scale_nonuniform` | Observation: squash and stretch, which the engine shades with slightly skewed normals | |
| `keys.interpolation` | Cubic-spline channels (played as straight lines between keys, tangents ignored; the finding says how far the pose is off), or an unknown mode | Export sampled keys: `ak.bake`, `ak.export_glb` |
| `keys.target` | A channel on no node, on morph weights, or on a property that is not translation, rotation or scale: the engine skips it | Remove it; no shape-key animation |
| `keys.sampler` | A sampler that cannot be read: the wrong accessor types, fewer values than key times | Export again |
| `keys.duplicate_channels` | Two channels on one node and property: only the later plays | One NLA track per action |
| `keys.empty` | An animation with no playable keys: length 0, never finishes | Key it, or delete it |

### 2. Motion quality

Measured on points of the skinned mesh (one per joint: the vertex it carries
that is farthest from it), in world space, at the rate the animation is keyed
at: what the player sees move.

| Id | Means | Fix |
| --- | --- | --- |
| `motion.pops` | In one frame a point jumps out and straight back (more than 1% of the model's size, four times the calmer frame beside it), or leaps and stays (more than 3%, four times the frames on both sides). Counted in pops | An IK solve crossing a singularity, an angle wrapping past 180 degrees: fix the generator. A cut that is meant eases over two or three frames |
| `motion.jitter` | Joints whose direction reverses six times a second or faster, by more than 0.3% of the size a frame, for a third of a second or more | Smooth the noise at its source; a shake that is meant goes in the spec as `tremble` |
| `motion.flips` | Rotations of more than 140 degrees from one key to the next: the short way round may be the wrong way | Key the turn in smaller steps; `ak.fix_quaternion_flips` |
| `motion.static` | Nothing moves | Give it life, or waive a still pose as by design |
| `motion.no_easing` | Three or more keys at which a channel changes speed or direction at once between stretches of constant speed a tenth of a second or longer: sparse keys under linear interpolation | Author the eases and `ak.bake` them |
| `motion.redundant_keys` | Observation: the share of keys linear interpolation reproduces within 0.05 degrees and 0.02% of the size. A warning at 90% of 3,000 keys or more | `ak.clean_curves`, then `export_glb(sampled=False)` |
| `motion.constant_channels` | Observation: channels that never change | |

An authored impact is not a pop: its speed builds over several frames and
stops, and nothing comes back. A tremble is not a pop either: every frame is
like the next. A step written as a `STEP` channel is taken as meant.

### 3. Loops and hand-overs

| Id | Means | Fix |
| --- | --- | --- |
| `loops.seam_pose` | A loop whose last pose is not its first: some vertex is more than 0.3% of the size away and more than half of what it moves in one of the loop's own frames (error above 2%) | `ak.make_loop`; in a generator, key the last frame from the first |
| `loops.seam_velocity` | The poses meet but the motion does not: a joint's velocity changes at the seam three times more sharply than anywhere inside the loop | Whole cycles of every periodic layer; `ak.make_loop` matches tangents |
| `loops.handover` | The first pose is not the pose of its `from` (a loop's first pose, a one-shot's last), or the last pose is not the first pose of its `to`: more than 5% of the size apart where the game cross-fades, 0.5% where it cuts (error above 2%). Also: a cut out of a *loop* that moves more than 0.5% from that pose, since the game cuts wherever the loop is. And a `from` or `to` the file does not have | Key the end from the other animation's frame; cross-fade instead of cutting |

The engine's cross-fade bridges any two poses smoothly, so the fade threshold
is not about a visible jump: it is about a different stance, which the fade
can only cross by sliding.

### 4. Deformation

The mesh is skinned in numpy at every frame (thinned for very large models),
as the engine skins it.

| Id | Means | Fix |
| --- | --- | --- |
| `deform.bounds` | The mesh reaches more than three times its rest reach from its centre: a bone has flown off | Keys in the wrong units or space |
| `deform.stretch` | Triangles pulled to more than 2.5 times their length at rest and longer than 3% of the model. Counted in triangles | A vertex on the wrong bone. A part made to stretch between two bones is waived as by design |
| `deform.collapse` | Eight or more triangles under 5% of their area at rest: the candy-wrapper | Spread the twist over two bones |
| `deform.underground` | In a `ground` profile: the lowest point sinks more than 1.5% of the size below y = 0. Counted in frames | Raise the IK targets or the hips |
| `deform.floating` | An animation declared `grounded` whose lowest point is more than 2% of the size above the ground. Counted in frames | Plant the feet, or do not declare a jump grounded |
| `deform.in_place` | An animation declared `in_place` whose hips (the joint nearest the root that carries vertices) start or end more than 5% of the size from where they stand at rest, or wander more than 25% in a loop | Animate on the spot; the game moves the entity |

Triangles under a joint whose scale the animation keys (an eye scaled shut)
are left out of stretch and collapse.

### 5. Game readiness

| Id | Means |
| --- | --- |
| `ready.structure`, `ready.engine_load` | Not a readable glTF; or, with `--engine`, the engine's own loader refuses it or `fe.app.model_animations` reports other names or lengths than were read here. Never waivable |
| `ready.no_animations` | A file in the animations manifest that has none |
| `ready.names` | An unnamed animation, two with one name (errors), or a name outside the profile's `name_pattern` (lower case, digits, underscores) |
| `ready.missing`, `ready.undeclared`, `ready.spec` | An animation the spec asks for is not in the file; one in the file has no spec; the spec has an unknown key or does not say `loop` |
| `ready.duration` | Not the length the spec says |
| `ready.impact` | At a moment the game acts on, the animation is nearly still (under 20% of its top speed from a tenth of a second before it to a twentieth after): the blow and its effect will not meet. Or the moment is after the animation ends |
| `ready.key_rate` | Keyed more than 60 times a second |
| `ready.max_animations`, `ready.max_keys`, `ready.max_animation_kilobytes`, `ready.max_nodes`, `ready.max_joints`, `ready.max_skinned_parts` | Over the profile's budget of that name (`max_skinned_parts` is the engine's own 16) |

### 6. Visual consistency

`review.py` writes an **overview** (per animation: six poses as the game shows
it and the speed over time; then every finding and the questions) and, beside
it, **one page per animation** (`<sheet>_<animation>.png`):

- a **filmstrip** of eight evenly spaced poses from the game's side, and
  level with the ground: the ground is a green line, whatever is under it is
  **red**, triangles stretched or collapsed past the profile's limits are
  **magenta**;
- **frame by frame**: eight consecutive frames around the first moment the
  game acts on (or, without one, the animation's fastest moment), where a
  blow must read and a glitch would hide;
- the **seam pair** of a loop, or the **hand-overs** of a one-shot: the two
  poses that should be one, drawn over each other in blue and orange (grey
  where they agree), with the largest gap in units;
- the **trails** of the extremities: a dot per frame, so an arc shows as a
  curve and speed as the spacing of the dots;
- the **speed graph**: the fastest point (white) and the typical point (blue)
  over time, the moments the game acts on (yellow), pops (red), and the
  lowest point of the mesh against the ground;
- with `--engine`, **shots from the real renderer** at four of the
  filmstrip's moments, with a hero-sized figure for scale (`stage/` is the
  small game that plays each animation to that moment and holds it; the
  caption gives the time the engine really reached);
- the animation's spec and its findings.

Open every page. Judge them against the game's art direction document,
answering each question; any "no" is a `fail`:

1. **Poses**: does every key pose read as a silhouette (what it is doing, and to whom), with nothing passing through the body or the ground (red)?
2. **Arcs**: do the trails of the hands, feet and head run in clean curves, without kinks, zigzags or straight-line cuts?
3. **Anticipation and follow-through**: does each action wind up before it and settle after it, or does it start and stop like a switch?
4. **Weight**: do heavy parts start slowly, land hard and take time to stop? Does the speed graph rise and fall, or is it flat?
5. **Timing**: do the blows land at the moments the game acts on (the yellow lines), and are the holds long enough to read?
6. **Loops and hand-overs**: is the seam pair one figure, not two? Do the spikes in the graph belong to the action, not to a glitch?
7. **Style**: does the motion suit the character and the game's art direction (a machine moves like a machine), and do all its animations look made by the same hand?

Then record it:

```sh
python3 tools/animkit/review.py --manifest tools/<game>/animations.json models/golem.glb \
    --sign pass --by "who" --notes "what was looked at and what was judged, including any flaw accepted"
```

Rules for the reviewer, agent or person:

- You must have looked at the overview **and every page** for *this* file.
  The verdict is stored with the file's SHA-256; any change to the model
  voids it.
- A sheet is stills. It shows poses, arcs, spacing and speed; it does not
  show the animation playing. If you can watch it play (in the game, or
  frames captured with `tools/<game>/shot.py`), do so before a `pass`; if you
  cannot, say in the notes that the review was made from the sheets alone.
- The notes say what you saw, animation by animation, not that you looked.
  Name the flaws you accepted and why. "Looks good" is not a review.
- When in doubt, `fail` it, say why in the notes, and fix the generator. A
  failed review costs a rebuild; a wrong pass ships.
- The review cannot be waived.

## The manifest

One JSON file per game lists every animated model it ships. Aetherbound's is
`tools/aetherbound/animations.json`.

```json
{
  "root": "../../samples/aetherbound",
  "art_direction": "tools/aetherbound/SPEC.md",
  "reviews": "animation_reviews",
  "folders": ["models", "maps", "fx"],
  "profiles": { "boss": { "max_keys": 80000 } },
  "models": {
    "models/golem.glb": {
      "profile": "character",
      "overrides": { "required": ["idle"], "animations": { "idle": {"loop": true, "seconds": 3} } },
      "waive": {
        "deform.stretch": { "reason": "By design: the bellows between hip and thigh are made to stretch.", "max": 24 }
      },
      "review": { "verdict": "pass", "sha256": "...", "by": "...", "notes": "...", "date": "..." }
    }
  }
}
```

- `root`: where the model paths start, relative to the manifest.
- `folders`: searched for `.glb`/`.gltf` files **that contain animations**;
  one the manifest does not list fails the run, so no motion ships
  unvalidated. Models without animations are modelkit's alone and need no
  entry here. `"ignore"` takes glob patterns for files to leave out.
- `profiles`: the game's own budgets and thresholds, over `profiles.json`.
- `overrides`: this model's own values over its profile: above all
  [its spec](#the-spec).
- `waive`: check id to a reason, or to `{"reason", "max"}`. With `max` the
  waiver holds only while the count of that finding (pops, frames,
  triangles, joints) stays at or under it, so a known flaw cannot quietly
  grow. Write the reason for the next reader: either why it is right for
  this model (`"By design: ..."`), or what is wrong, where to fix it, and
  that it is debt (`"Known debt from before animkit: ... Fix in <generator>:
  ..."`). Remove the waiver when the flaw is fixed.
- `review`: written by `review.py --sign`.

Waive a flaw only when it is deliberate, or when fixing it is out of the
change at hand and it is recorded as debt with its fix. Never waive to get a
new animation through: a new model starts with no waivers. Intent belongs in
the spec, not in a waiver: a tremble that is meant is `"tremble"`, a stagger
that travels is `"in_place": false`.

## Commands

```sh
# one model, while iterating (no spec: integrity, motion, deformation and budgets only)
python3 tools/animkit/validate.py out/golem.glb --profile character --engine build/fengine_headless

# one model of a manifest, with its spec, before its review exists
python3 tools/animkit/validate.py --manifest tools/<game>/animations.json models/golem.glb --no-review

# everything a game ships (the gate)
python3 tools/animkit/validate.py --manifest tools/<game>/animations.json --engine build/fengine_headless

# options: --lenient (warnings pass), --info, --json report.json, --no-review

# the review sheets (an overview and a page per animation), then the verdict
python3 tools/animkit/review.py --manifest tools/<game>/animations.json models/golem.glb --engine build/fengine
python3 tools/animkit/review.py --manifest tools/<game>/animations.json --unreviewed --engine build/fengine
python3 tools/animkit/review.py --manifest tools/<game>/animations.json models/golem.glb --sign pass --by "..." --notes "..."

# prove the validator itself
python3 tools/animkit/selftest.py [--engine build/fengine_headless] [--blender]
```

## What this does not see

- **Whether the motion is good.** The checks find what is broken: a pop, a
  seam, a foot in the floor. Appeal, weight and timing are judged, from
  stills, by the reviewer; a sheet cannot show an animation playing. Watch
  it in the game.
- **Intent it was not told.** A pop is told from an authored snap only by its
  shape: one frame out and back, or one isolated leap. A deliberate cut of a
  single frame is reported and has to be waived; a glitch inside a fast swing
  is not reported, because it is not seen either. A tremble is told from
  jitter only by the spec.
- **The engine's pose, in numbers.** `rig.py` reimplements the engine's
  sampling and skinning; no Lua binding returns a posed joint, so the two are
  compared by eye (the engine shots beside the filmstrip) and by name and
  length (`ready.engine_load`), not vertex by vertex.
- **How the game uses it.** A hand-over is checked where the spec declares
  one. If the game also plays an attack out of some other stance with a
  cross-fade, that blend is the game's to judge. Collisions with the scene,
  with other characters, and the sync of sound and effects beyond the declared
  `impacts` are not measured.
- **Self-intersection.** A hand passing through the chest is seen in the
  filmstrip or not at all.
- **Cameras, lights, materials and morph targets** animated in the file: the
  engine does not play them, and beyond a warning for morph weights neither
  does this.
