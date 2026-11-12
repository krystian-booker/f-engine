#!/usr/bin/env python3
"""Proves the validator: every check is shown a small animated model with
exactly the flaw it exists for and must name it, and clean models (among them
the cases that once raised false alarms) must come back clean.

    python3 tools/animkit/selftest.py                                   # numpy and pillow
    python3 tools/animkit/selftest.py --engine build/fengine_headless   # also ready.engine_load
    python3 tools/animkit/selftest.py --blender                         # also the bpy helpers (needs the bpy module)
"""
import copy
import math
import os
import shutil
import sys
import tempfile

import numpy as np

sys.dont_write_bytecode = True
HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.dirname(HERE))

from kitcommon import kit as common  # noqa: E402
from animkit import fixtures as fx  # noqa: E402
from animkit.kit import AnimKit  # noqa: E402

KIT = AnimKit(HERE)
WORK = tempfile.mkdtemp(prefix="animkit-selftest-")
FAILURES = []
ENGINE = sys.argv[sys.argv.index("--engine") + 1] if "--engine" in sys.argv else None

FIGURE = fx.figure()
SPEC = {"idle": {"loop": True, "seconds": 2, "grounded": True, "in_place": True},
        "attack": {"loop": False, "seconds": 1, "from": "idle", "to": "idle", "impacts": [0.5], "grounded": True, "in_place": True}}


def clips():
    """The clean pair every case starts from: a looping idle and an attack that leaves and returns to it."""
    return {"idle": fx.bake(FIGURE, fx.idle, 2.0), "attack": fx.bake(FIGURE, fx.attack, 1.0)}


def profile(name="character", spec=SPEC, **overrides):
    return common.profile_named(KIT.profiles(), name, {"animations": copy.deepcopy(spec), **overrides})


def report(name, problems):
    print(f"  {name:<66} {'ok' if not problems else 'FAILED: ' + '; '.join(problems)}")
    if problems:
        FAILURES.append(name)


def case(name, clip_set, expect=(), spec=SPEC, clean=False, absent=(), model=FIGURE, profile_name="character", info=(), engine=None,
         **options):
    """Write the model, validate it, and check that every id in `expect` was reported (and those in
    `absent` were not; `clean`: nothing at all). `info` ids must be reported as observations."""
    path = os.path.join(WORK, name.replace(" ", "_").replace("/", "_").replace("'", "") + ".glb")
    writer = {key: options.pop(key) for key in ("extra", "skinned_parts", "mutate") if key in options}
    fx.write_glb(path, model, clip_set, **writer)
    findings, _, _ = KIT.validate(path, profile(profile_name, spec, **options), engine=engine)
    found = {f.check for f in findings if f.severity != "info"}
    noted = {f.check for f in findings if f.severity == "info"}
    problems = [f"missed {check}" for check in expect if check not in found]
    problems += [f"missed the observation {check}" for check in info if check not in noted]
    problems += [f"reported {check}" for check in absent if check in found]
    if clean and found:
        problems.append("reported " + ", ".join(sorted(found)))
    report(name, problems)
    return findings


def edit(clip_set, clip, joint, path, change):
    """Apply change(channel) to one channel of a fresh copy."""
    change(fx.channel(clip_set[clip], joint, path))
    return clip_set


def main():
    q = fx.quat
    print("clean models")
    case("a baked idle loop and an attack that returns to it", clips(), clean=True)
    case("the same through the engine's loader" if ENGINE else "the same, loaded again", clips(), clean=True, engine=ENGINE)
    turning = fx.prop()
    wheel = [{"joint": "wheel", "path": "rotation", "times": np.arange(121) / 30.0,
              "values": np.array([q((0, 0, 1), 3 * i) for i in range(121)]), "interpolation": "LINEAR"}]
    case("a wheel that turns at one speed is not unEased and does not pop", {"turn": wheel}, clean=True, model=turning,
         profile_name="prop", spec={"turn": {"loop": True, "seconds": 4, "in_place": True}})
    c = clips()
    shake = fx.channel(c["idle"], "head", "rotation")
    shake["values"] = np.array([fx.quat_mul(v, q((1, 0, 0), 8 * math.sin(2 * math.pi * 10 * t))) for v, t in zip(shake["values"], shake["times"])])
    spec = copy.deepcopy(SPEC)
    spec["idle"]["tremble"] = ["head"]
    case("a tremble the spec declares is not jitter", c, clean=True, spec=spec)
    c = clips()
    snap = fx.channel(c["attack"], "head", "rotation")
    ease = [0, 0, 0, 1 - (2 / 3) ** 3, 1 - (1 / 3) ** 3, 1] + [1] * 40       # a cubic ease-out over three frames, held, then back
    snap["values"] = np.array([q((0, 1, 0), 50 * (ease[min(i, 40)] if i < 20 else fx.smooth((30 - i) / 10)))
                               for i in range(len(snap["times"]))])
    case("a snap eased over three frames is not a pop", c, clean=True)
    c = clips()
    blink = fx.channel(c["attack"], "head", "rotation")
    blink["values"] = np.array([q((0, 1, 0), 0 if i < 10 or i >= 20 else 45) for i in range(len(blink["times"]))])
    blink["interpolation"] = "STEP"
    case("a step written as a STEP channel is not a pop", c, clean=True)
    c = clips()
    eye = {"joint": "head", "path": "scale", "times": np.arange(61) / 30.0,
           "values": np.array([[max(0.0001, 0.5 + 0.5 * math.cos(2 * math.pi * i / 60))] * 3 for i in range(61)]), "interpolation": "LINEAR"}
    c["idle"].append(eye)
    case("a part scaled almost to nothing on purpose is not a collapse", c, clean=True)

    print("keyframe integrity")
    case("a channel that is not a number", edit(clips(), "idle", "chest", "rotation", lambda ch: ch["values"].__setitem__((5, 0), np.nan)),
         ["keys.nan"])
    case("key times that go backwards", edit(clips(), "idle", "chest", "rotation", lambda ch: ch["times"].__setitem__(10, ch["times"][8])),
         ["keys.times"])
    c = clips()
    for entry in c["attack"]:
        entry["times"] = entry["times"] + 0.5
    case("an animation that starts half a second late", c, ["keys.start"], absent=["keys.times"])
    case("a rotation key that is not unit length",
         edit(clips(), "idle", "chest", "rotation", lambda ch: ch["values"].__setitem__(7, ch["values"][7] * 1.3)), ["keys.quaternion"])
    c = clips()
    c["idle"].append({"joint": "hand_r", "path": "scale", "times": np.array([0.0, 1.0, 2.0]),
                      "values": np.array([[1, 1, 1], [-1, 1, 1], [1, 1, 1.0]]), "interpolation": "LINEAR"})
    case("a negative scale key", c, ["keys.scale"])
    c = clips()
    c["idle"].append({"joint": "hand_r", "path": "scale", "times": np.array([0.0, 1.0, 2.0]),
                      "values": np.array([[1, 1, 1], [1, 1.6, 1], [1, 1, 1.0]]), "interpolation": "LINEAR"})
    case("a squash-and-stretch scale is only noted", c, info=["keys.scale_nonuniform"], absent=["keys.scale"])
    c = clips()
    spline = fx.channel(c["attack"], "arm_l", "rotation")
    keys = spline["values"][::10]
    spline["times"] = spline["times"][::10]
    spline["values"] = np.stack([np.zeros_like(keys), keys, np.zeros_like(keys)], axis=1).reshape(-1, 4)
    spline["interpolation"] = "CUBICSPLINE"
    case("cubic-spline keys, which the engine plays as straight lines", c, ["keys.interpolation"])
    c = clips()
    c["idle"].append({"joint": "chest", "path": "weights", "times": np.array([0.0, 2.0]), "values": np.array([[0, 0, 0], [1, 1, 1.0]])})
    case("a channel that animates morph weights", c, ["keys.target"])
    c = clips()
    fx.channel(c["idle"], "chest", "rotation")["values"] = fx.channel(c["idle"], "chest", "rotation")["values"][:-5]
    case("fewer values than key times", c, ["keys.sampler"])
    c = clips()
    c["idle"].append(copy.deepcopy(fx.channel(c["idle"], "chest", "rotation")))
    case("two channels on one bone's rotation", c, ["keys.duplicate_channels"])
    c = clips()
    c["empty"] = []
    case("an animation with no keys", c, ["keys.empty"], spec={**SPEC, "empty": {"loop": False}})

    print("motion quality")
    c = clips()
    turn = fx.channel(c["attack"], "head", "rotation")
    turn["values"] = np.array([q((0, 1, 0), 0 if i < 15 else 170) for i in range(len(turn["times"]))])
    case("a turn of 170 degrees between two keys", c, ["motion.flips"])
    still = {"idle": [{"joint": j, "path": "rotation", "times": np.array([0.0, 2.0]), "values": np.array([q((0, 0, 1), 5)] * 2),
                       "interpolation": "LINEAR"} for j in ("chest", "head")]}
    case("an animation in which nothing moves", still, ["motion.static"], spec={"idle": SPEC["idle"]})

    def spike(ch):
        ch["values"][20] = fx.quat_mul(ch["values"][20], q((0, 1, 0), 25))
    case("a limb that flips for one frame and comes back", edit(clips(), "idle", "arm_r", "rotation", spike), ["motion.pops"])

    def leap(ch):
        ch["values"][30:] = np.array([fx.quat_mul(v, q((0, 1, 0), 35)) for v in ch["values"][30:]])
    c = edit(clips(), "idle", "arm_r", "rotation", leap)
    fx.channel(c["idle"], "arm_r", "rotation")["values"][-1] = fx.channel(c["idle"], "arm_r", "rotation")["values"][0]
    case("a limb that leaps in one frame and stays", c, ["motion.pops"])
    c = clips()
    noisy = fx.channel(c["idle"], "arm_r", "rotation")
    noisy["values"] = np.array([fx.quat_mul(v, q((0, 1, 0), 4 * (-1) ** i if 0 < i < 60 else 0)) for i, v in enumerate(noisy["values"])])
    case("a limb that shakes with noise", c, ["motion.jitter"], absent=["motion.pops"])
    sparse = clips()
    sparse["attack"] = [{"joint": "arm_l", "path": "rotation", "times": np.array([0.0, 0.25, 0.5, 0.75, 1.0]),
                         "values": np.array([fx.quat_mul(fx.idle(0.0)["arm_l"]["rotation"], q((0, 1, 0), a)) for a in (0, 40, -90, 30, 0)]),
                         "interpolation": "LINEAR"}] + [ch for ch in sparse["attack"] if ch["joint"] != "arm_l"]
    case("five keys a second with nothing between them", sparse, ["motion.no_easing"], absent=["motion.pops"])
    case("channels that never change are only noted", clips(), info=["motion.constant_channels", "motion.redundant_keys"], clean=True)
    dense = fx.prop()
    slow = [{"joint": "wheel", "path": "rotation", "times": np.arange(3601) / 60.0,
             "values": np.array([q((0, 0, 1), 0.1 * i) for i in range(3601)]), "interpolation": "LINEAR"}]
    case("3,600 keys for one slow turn", {"turn": slow}, ["motion.redundant_keys"], model=dense, profile_name="prop",
         spec={"turn": {"loop": False, "seconds": 60}})

    print("loops and hand-overs")
    c = clips()
    c["idle"] = fx.bake(FIGURE, lambda t: fx.idle(t * 0.9), 2.0)       # nine tenths of a cycle: the end is not the start
    case("a loop whose last pose is not its first", c, ["loops.seam_pose"])
    c = clips()

    def sawtooth(t):
        pose = fx.idle(t)
        u = t / 2.0
        pose["arm_r"] = {"rotation": q((0, 0, 1), 8 + 30 * (u if u < 0.999 else 0.0))}   # runs one way, then is put back
        return pose
    c["idle"] = fx.bake(FIGURE, sawtooth, 2.0)
    arm = fx.channel(c["idle"], "arm_r", "rotation")
    arm["values"] = np.array([q((0, 0, 1), 8 + 30 * math.sin(math.pi * i / 60) ** 0.5) for i in range(61)])   # leaves fast, returns fast
    case("a loop whose motion changes direction at the seam", c, ["loops.seam_velocity"], absent=["loops.seam_pose"])
    c = clips()
    c["attack"] = fx.bake(FIGURE, lambda t: fx.attack(t * 0.8), 1.0)   # stops four fifths of the way through: the arm is still out
    case("an attack that does not come back to the idle pose", c, ["loops.handover"])
    cut = copy.deepcopy(SPEC)
    cut["attack"]["from"] = {"animation": "idle", "fade": 0}
    case("a cut out of a loop that is moving", clips(), ["loops.handover"], spec=cut)

    print("deformation")

    def metres(ch):
        ch["values"][10:20] = ch["values"][10:20] + [0, 60.0, 0]
    case("a bone keyed in the wrong units flies off", edit(clips(), "idle", "hips", "translation", metres), ["deform.bounds"])
    stray = copy.deepcopy(FIGURE)
    stray["joint_index"][9 * 24] = [[name for name, _, _ in fx.JOINTS].index("hand_l"), 0, 0, 0]   # a corner of the right foot follows the left hand
    case("one vertex weighted to the wrong bone", clips(), ["deform.stretch"], model=stray)
    c = clips()
    twist = fx.channel(c["attack"], "hand_l", "rotation")
    twist["values"] = np.array([q((1, 0, 0), 180 * fx.smooth(i / 10) * (1 - fx.smooth((i - 20) / 10))) for i in range(len(twist["times"]))])
    case("a wrist twisted half a turn (the candy-wrapper)", c, ["deform.collapse"])

    def sink(ch):
        ch["values"][:] = ch["values"] - np.outer(np.sin(np.pi * np.arange(len(ch["values"])) / (len(ch["values"]) - 1)), [0, 0.2, 0])
    case("feet that sink through the floor", edit(clips(), "idle", "hips", "translation", sink), ["deform.underground"])

    def hop(ch):
        ch["values"][:] = ch["values"] + np.outer(np.sin(np.pi * np.arange(len(ch["values"])) / (len(ch["values"]) - 1)), [0, 0.3, 0])
    case("a hop in an animation declared grounded", edit(clips(), "idle", "hips", "translation", hop), ["deform.floating"])

    def walk_off(ch):
        ch["values"][:] = ch["values"] + np.outer(np.arange(len(ch["values"])) / (len(ch["values"]) - 1), [0, 0, 1.5])
    case("an attack that walks away and stays there", edit(clips(), "attack", "hips", "translation", walk_off), ["deform.in_place"])

    print("game readiness")
    case("a model with no animations", {}, ["ready.no_animations"])
    case("an animation the spec asks for is missing", {"idle": clips()["idle"]}, ["ready.missing"])
    c = clips()
    c["wave"] = copy.deepcopy(c["attack"])
    case("an animation the spec does not describe", c, ["ready.undeclared"])
    case("a name that is not in the convention", {"idle": clips()["idle"], "ArmatureAction.001": clips()["attack"]}, ["ready.names"],
         spec={"idle": SPEC["idle"], "ArmatureAction.001": {"loop": False}})
    c = clips()
    case("two animations with one name", [("idle", c["idle"]), ("attack", c["attack"]), ("idle", c["idle"])], ["ready.names"])
    case("an animation with no name", [("idle", c["idle"]), ("attack", c["attack"]), ("", c["attack"])], ["ready.names"])
    typo = copy.deepcopy(SPEC)
    typo["idle"]["looping"] = True
    case("a misspelt key in the spec", clips(), ["ready.spec"], spec=typo)
    longer = copy.deepcopy(SPEC)
    longer["attack"]["seconds"] = 1.4
    case("an animation shorter than the game's timing", clips(), ["ready.duration"], spec=longer)
    late = copy.deepcopy(SPEC)
    late["attack"]["impacts"] = [0.2]
    case("the game strikes while the animation is nearly still", clips(), ["ready.impact"], spec=late)
    c = clips()
    c["idle"] = fx.bake(FIGURE, fx.idle, 2.0, fps=120)
    case("keys 120 times a second", c, ["ready.key_rate"])
    case("over its keyframe budget", clips(), ["ready.max_keys"], max_keys=500)
    case("over its budget of key data", clips(), ["ready.max_animation_kilobytes"], max_animation_kilobytes=5)
    case("more animations than the profile allows", clips(), ["ready.max_animations"], max_animations=1)
    case("more nodes than the profile allows", clips(), ["ready.max_nodes"], max_nodes=8)
    case("more joints than the profile allows", clips(), ["ready.max_joints"], max_joints=8)
    case("more skinned parts than the engine animates", clips(), ["ready.max_skinned_parts"], skinned_parts=17)
    broken = os.path.join(WORK, "broken.glb")
    with open(broken, "wb") as out:
        out.write(b"glTF\x02\x00\x00\x00")
    findings, _, _ = KIT.validate(broken, profile())
    report("a file that is not a glTF", [] if any(f.check == "ready.structure" for f in findings) else ["missed ready.structure"])
    if ENGINE:
        def lie(document):      # the file says one length, its keys another: only the engine's own loader can tell
            document["animations"][0]["channels"] = document["animations"][0]["channels"][:1]
            sampler = document["animations"][0]["samplers"][document["animations"][0]["channels"][0]["sampler"]]
            document["accessors"][sampler["output"]]["type"] = "MAT4"
        case("a sampler the engine refuses", clips(), ["ready.engine_load"], mutate=lie, engine=ENGINE)

    print("visual consistency (the sign-off)")
    path = os.path.join(WORK, "signed.glb")
    fx.write_glb(path, FIGURE, clips())
    for name, review, expected in (
            ("a model nobody has looked at", None, True),
            ("a review of an older version of the file", {"verdict": "pass", "sha256": "0" * 64}, True),
            ("a review that failed it", {"verdict": "fail", "sha256": KIT.digest(path), "notes": "the attack has no weight"}, True),
            ("a review that passed this exact file", {"verdict": "pass", "sha256": KIT.digest(path)}, False)):
        findings, _, _ = KIT.validate(path, profile(), review=review, require_review=True)
        blocked = any(f.check == "visual.review" for f in findings)
        report(name, [] if blocked == expected else ["the sign-off did not hold"])
    from animkit import sheet as sheets
    findings, _, subject = KIT.validate(path, profile())
    image = sheets.review(path, profile(), findings, subject, ["profile: character", "", "Judge against the art direction:"], None)
    out = os.path.join(WORK, "sheet.png")
    # Saving a Sheet writes the overview and one page per animation (and says so: keep the test's output to its own lines).
    stdout, sys.stdout = sys.stdout, open(os.devnull, "w", encoding="utf-8")
    try:
        image.save(out)
    finally:
        sys.stdout.close()
        sys.stdout = stdout
    pages = [f for f in os.listdir(WORK) if f.startswith("sheet_")]
    report("the review sheet draws an overview and a page per animation",
           [] if os.path.getsize(out) > 2000 and len(pages) == 2 else [f"{len(pages)} pages"])

    print("waivers")
    c = edit(clips(), "idle", "arm_r", "rotation", spike)

    def spike_two(ch):
        ch["values"][40] = fx.quat_mul(ch["values"][40], q((0, 1, 0), 25))
    c = edit(c, "idle", "arm_r", "rotation", spike_two)
    path = os.path.join(WORK, "waived.glb")
    fx.write_glb(path, FIGURE, c)

    def left(waivers, check="motion.pops", **more):
        findings, _, _ = KIT.validate(path, profile(), waivers, **more)
        return any(f.check == check and not f.waived for f in findings)

    for name, got, expected in (
            ("a flaw with no waiver fails", left({}), True),
            ("a waiver with a reason lets it through", left({"motion.pops": "By design: the arm twitches"}), False),
            ("a capped waiver holds while the count is under the cap", left({"motion.pops": {"reason": "known", "max": 2}}), False),
            ("a capped waiver stops holding when the flaw grows", left({"motion.pops": {"reason": "known", "max": 1}}), True),
            ("a waiver with no reason is not a waiver", left({"motion.pops": {"max": 9}}), True),
            ("the visual review cannot be waived", left({"visual.review": "trust me"}, check="visual.review", require_review=True), True)):
        report(name, [] if got == expected else ["the waiver rule did not hold"])

    print("the manifest")
    folder = os.path.join(WORK, "game")
    os.makedirs(os.path.join(folder, "models"))
    fx.write_glb(os.path.join(folder, "models", "figure.glb"), FIGURE, clips())
    fx.write_glb(os.path.join(folder, "models", "crate.glb"), FIGURE, {})        # no animations: not animkit's to list
    fx.write_glb(os.path.join(folder, "models", "unlisted.glb"), FIGURE, clips())
    import json
    manifest = os.path.join(folder, "animations.json")

    def run(models):
        with open(manifest, "w", encoding="utf-8") as out:
            json.dump({"root": ".", "folders": ["models"], "models": models}, out)
        stdout, sys.stdout = sys.stdout, open(os.devnull, "w", encoding="utf-8")
        try:
            return KIT.validate_main(["--manifest", manifest, "--no-review"])
        finally:
            sys.stdout.close()
            sys.stdout = stdout
    entry = {"profile": "character", "overrides": {"animations": SPEC}}
    report("an animated model the manifest does not list fails the run", [] if run({"models/figure.glb": entry}) == 1 else ["it passed"])
    report("a model with no animations need not be listed",
           [] if run({"models/figure.glb": entry, "models/unlisted.glb": entry}) == 0 else ["it failed"])

    if "--blender" in sys.argv:
        blender_helpers()
    shutil.rmtree(WORK, ignore_errors=True)
    print(f"\n{'all checks proved' if not FAILURES else str(len(FAILURES)) + ' FAILED: ' + ', '.join(FAILURES)}")
    return 1 if FAILURES else 0


def blender_helpers():
    sys.path.insert(0, os.path.join(HERE, "blender"))
    import animkit_bpy_test
    animkit_bpy_test.run(KIT, WORK, report, profile)


if __name__ == "__main__":
    sys.exit(main())
