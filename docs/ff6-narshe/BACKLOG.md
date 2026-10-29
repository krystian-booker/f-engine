# Narshe opening implementation backlog

The scope is the complete opening through the Narshe departure boundary. Effort ranges below are person-days for an engineer or technical artist already familiar with f-engine. They are planning estimates; they do not include review turnaround or promise a calendar delivery date. Asset extraction and this specification are completed groundwork. Runtime conversion, content production and playable validation remain.

## Work packages and acceptance

| ID | Work | Depends on | Estimate | Completion evidence |
| --- | --- | --- | --- | --- |
| P01 | Capture original full route plus optional local interactions; annotate shots, choices, music and timing. | Existing ROM/export | 1–2 | Reference capture and shot ledger cover every scene in `scenes.json`. |
| P02 | Compile sprites/metasprites, source fonts/UI, map tiles and battle backgrounds into engine asset formats. | Export | 2–4 | Import harness has correct palettes, priority, glyphs and directions. |
| P03 | Import required actor/item/skill/equipment data, commands and gameplay tables beyond the native rip groups. | Export, P01 | 2–3 | Data validated against source; no placeholder stats or silently missing skill records. |
| P04 | Compile map collision, triggers, NPCs, short/long exits, layers, rails, stairs and stage flags. | P02, P01 | 2–4 | Automated route checks and map inspection agree; later routes stay inaccessible. |
| P05 | Render a march music cue, ordinary battle cue and representative SFX; establish the complete audio converter. | Export | 1–3 | Listening comparison, loop check and source-ID cue ledger. |
| P06 | Build one Narshe street with three mounted riders, snow, dialogue and a fixed Guard battle. | P02–P05 | 2–3 | Playable vertical slice returns to the same field state without leaks. |
| P07 | Prototype three-party switching and pursuer contacts on the cave defense map. | P03, P04 | 2–3 | Three persistent groups, resources and field positions survive two battle returns. |
| P08 | Decide any generic renderer/audio service additions after the slice experiments. | P05–P07 | 0–2 | Recorded need/result; services stay generic and story remains Lua. |
| P09 | Implement source-specific combat formulas, commands, enemy AI and item use for the scoped encounters. | P03, P06 | 2–4 | Magitek, normal combat, target selection and rewards match required behavior. |
| P10 | Implement linked Whelk head/shell state and encounter presentation. | P09 | 1–2 | Head hidden/exposed, shell counter, queued targets, outcomes and rewards tested. |
| P11 | Implement scripted battle presenter for Esper contact and flashback destruction. | P09 | 1–2 | Story owns commands/outcomes; disappearance order and party mutations are correct. |
| P12 | Complete moogle defense routes, switching, group loss, failure/retry and Marshal victory. | P07, P09 | 2–4 | All six pursuits, commander, retries and cleanup pass deterministic tests. |
| P13 | Build story director, state schema and canonical checkpoint fixtures. | P04, P06 | 1–2 | All checkpoint states reconstruct names, inventory, actors, maps and music. |
| P14 | Author title, four-card prologue, overlook and snow march with credits. | P01, P02, P05, P13 | 3–5 | Reference shot order and measured gait/music/camera timing. |
| P15 | Author arrival, route-dependent town ambushes, mine approach, save point and barrier. | P04, P09, P13 | 2–3 | Every reachable trigger works once; barrier state persists. |
| P16 | Author Whelk approach, Esper reveal, awakening, naming and Arvis escape. | P10, P11, P13 | 2–3 | Continuous handoff from raid party to solo Terra, no stale armor or soldiers. |
| P17 | Author pursuit, collapse, Imperial flashback, Locke introduction and naming. | P11, P13 | 2–3 | Correct shot/party order; cinematic battle returns to story. |
| P18 | Author rescue/defense setup, carry sequence, switch, amnesia conversation and town return. | P12, P13 | 2–3 | Temporary parties removed, final lineup restored, secret entrance closes. |
| P19 | Implement optional school/local NPCs, chests, accessible menus, north-route rejection and departure stop. | P03, P04, P18 | 1–2 | Optional content valid for stage; south exit never loads world map. |
| P20 | Complete saves/resume, pause/focus handling, controller prompts and restart cleanup. | P12, P13, P19 | 1–2 | Supported saves round-trip; repeated retry/skip never duplicates state. |
| P21 | Add game tests and full-route/windowed replays. | P14–P20 | 2–3 | `--check`, Lua tests and windowed full-route replay pass; screenshots reviewed. |
| P22 | Tune performance and stability; produce pitch build, measured report and guided checkpoint tour. | P21, A01–A06 | 2–3 | Full acceptance matrix passed on named hardware with reproducible build settings. |

Engineering and technical integration total **36–65 person-days** by summing the listed ranges. This includes scene scripting and integration but excludes the separate environment/art production packages below. Work can overlap between people; elapsed time depends on staffing and the risk gates, and is not the sum divided blindly by headcount.

## Environment and art production

| ID | Work | Depends on | Estimate | Completion evidence |
| --- | --- | --- | --- | --- |
| A01 | Establish source-pixel material style, scale, palette/lighting treatment and one approved street. | P02, P06 | 1–2 | Approved sprite scale, warm/cold contrast, nearest sampling and camera. |
| A02 | Produce Narshe exterior, overlook, vista and secret-entrance states. | A01, P04 | 3–5 | All story marks, doors and occlusion work in the diorama. |
| A03 | Produce north/south mines, Esper chamber, collapse and defense spaces. | A01, P04 | 3–5 | Route and height/layer tests pass with final geometry. |
| A04 | Produce Arvis/interiors, tutorial rooms, Imperial flashback and battle dioramas. | A01, P04 | 2–4 | Every mandatory and eligible optional room has complete dressing/camera. |
| A05 | Finish Magitek compositions, character clips, battle VFX, snow/steam, UI and font treatment. | P02, P09–P11 | 2–4 | No placeholder art; source poses and effect timing verified. |
| A06 | Finish music/SFX renders, loops, scene mix and cinematic polish. | P05, P14–P19 | 2–3 | Full playthrough listening and music continuity review. |

Art/audio/environment production total **13–23 person-days**. Combined planning allowance is **49–88 person-days**, before contingency. Reassess after P06 and P07: those experiments determine whether import fidelity, audio, renderer features or party-state integration expand the work. A flat source-layer prototype can be produced earlier than the HD-2D pitch build, but does not close the final environment acceptance gates.

## Risk gates and implementation decisions

| Gate | Resolve | Decision criterion |
| --- | --- | --- |
| After P02/P04 | Layer priority, transparent tiles and collision conversion | Compare full map crops and traverse doors/stairs; fix the importer before authoring every map. |
| After P05 | Source audio fidelity and timing | Correct instruments, effects and loops; measure march drift before choosing a new audio-clock API. |
| After P06 | Mount composition, sprite lighting, perspective and battle handoff | Approve a street shot and repeated enter/exit battle test. |
| After P07 | Multiple parties on a single field | Preserve inactive parties and avoid contact/switch races; settle the game-level controller before final defense scripting. |
| After P10/P11 | Boss/cinematic outcome ownership | Shell surviving does not prevent Whelk victory; cinematic enemies do not invoke normal rewards/game over. |
| Before P22 | Entire story and optional-route completeness | Every manifest node has a passing test/capture or an explicitly recorded blocked dependency. |

## Pitch build handoff

Deliver the complete playable game folder and packaged runtime; an asset provenance manifest; source/reference and diorama screenshots for the key shots; a full-run replay; a short guided checkpoint replay/video; the test results; a named-machine performance report; and a concise list of any remaining visual differences. The final build must not depend on the temporary reference checkout or open the source ROM at runtime.
