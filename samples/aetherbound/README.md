# Aetherbound: Prologue

A ten-minute vertical slice in the HD-2D style: pixel-art characters in a lit
3D world. It is the opening of a larger story, played as if it were the end
of one: four people who have fought a war together reach its last fortress,
and one of them does not come back.

```sh
./build/fengine samples/aetherbound
```

Keyboard: arrows or WASD to move, Shift to run, Enter/Z/Space to confirm,
Esc/X to cancel, Tab to hurry dialogue. Gamepad: stick or d-pad, A (cross) to
confirm, B (circle) to cancel, X (square) to run, Start to hurry dialogue. The
prompts on screen follow whichever you touched last.

## What happens

1. **The Fallen Fortress.** A shattered bridge, a burning courtyard, the great
   crystal over the tower. A scene, then the player walks the party through
   the fortress and fights twice: the first fight explains the battle system,
   the second introduces Rowan's Resonance.
2. **The Weapon Awakens.** The chamber under the tower and the Aetherbound
   Colossus. The boss battle runs until half of its 8,000 HP is gone.
3. **The Breaking Point.** The core overloads; nothing hurts the machine any
   more. Rowan's secret comes out, and the battle resumes with one command
   that matters: Unbound Resonance, twice, and then LAST LIGHT.
4. **The Sacrifice.** A cinematic cut to the music.
5. **What Remains.** The same chamber before dawn.

## The battle system

Active-time battles, fought where the party stands. Every fighter has a gauge
that fills at a rate set by speed; a hero with a full gauge gets the command
window, and time waits while a command is chosen or an action plays ("wait"
mode). Each hero has their own commands:

| Hero | Commands |
| --- | --- |
| Cael | Attack, Aether (Ember, Mend, Ward), Guard, Item |
| Rowan | Attack, **Resonance** (immense damage; raises his corruption), Guard, Item |
| Lyra | Attack, Aether (Frostbind, Aether Surge, Mend), Guard, Item |
| Darius | Attack, Arts (Shield Bash, Bulwark), Guard, Item |

A lost battle offers to try again from its start. The last phase cannot be
lost: it is there to be lived through, not won.

## How it is built

Everything the game does is Lua in this folder; the engine provides worlds,
sprites, skinned meshes, lights, particles, input, audio, text and the canvas.

| File | What |
| --- | --- |
| `main.lua` | The entry module: wires the modules below into the engine's `init`, `update`, `draw` |
| `game/task.lua`, `game/scenes.lua` | Coroutines for everything that takes time; the scene stack |
| `game/input.lua`, `game/ui.lua` | Actions and button prompts; panels, gauges and text on a 1920 × 1080 canvas |
| `game/audio.lua`, `game/fx.lua`, `game/vfx.lua` | Music and sounds by id; fades, shakes, bars, camera moves; short-lived effects in the world |
| `game/dialogue.lua` | Speech boxes, the machine's voice, lines floating over heads |
| `game/actors.lua`, `game/director.lua` | Characters standing in a world; how scenes move and pose them |
| `game/field.lua` | Exploration: the player walks Cael, the others follow his trail, zones call the story |
| `game/battle/rules.lua` | The battle system with nothing drawn: gauges, commands, damage (tested in `tests/rules.lua`) |
| `game/battle/scene.lua` | The battle on screen: formation, camera, command window, blows and numbers |
| `story/*.lua` | The five scenes, in order, as straight-line scripts |
| `data/*.json` | Heroes, enemies, troops, skills, items, audio ids |
| `dev/autoplay.lua` | A player made of code, for the replays |

`./build/fengine_headless --test samples/aetherbound` runs the rules tests and
plays the whole story headless. `replays/*.json` (written by
`tools/aetherbound/make_replays.py`) play it in the windowed runtime through
`dev/autoplay.lua` and compare screenshots; `playthrough.json` is the whole
prologue from the title to the end card. In the F1 editor's Launch tab the
scenes can be started one by one.

## Art, music and fonts

All art, animation, music and sound in this folder was generated for it by
the scripts in `tools/aetherbound/` (Blender through Python for the maps, the
Colossus and the character sprites; numpy for effects, UI art and all audio),
and carries no third-party licence. `tools/aetherbound/README.md` says how to
regenerate each part.

The fonts in `ui/fonts/` are Cinzel, Crimson Pro and Share Tech Mono, each
under the SIL Open Font License 1.1 (the `OFL-*.txt` files beside them).

The story, its characters and their names are the author's own; the names are
working names.
