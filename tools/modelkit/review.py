#!/usr/bin/env python3
"""The sixth check: look at the model.

    python3 tools/modelkit/review.py models/crate.glb --profile prop --out /tmp/crate.png
    python3 tools/modelkit/review.py --manifest tools/aetherbound/models.json models/colossus.glb
    python3 tools/modelkit/review.py --manifest tools/aetherbound/models.json models/colossus.glb \
        --sign pass --by "name" --notes "what was looked at and judged"

Without --sign it writes the model's review sheet: the form from six sides
(shaded by the file's own normals, with faces seen from behind in red), the
textures on the surface, texel density, silhouettes at game size next to a
hero, the UV layout, and every finding of validate.py. With --engine it adds
shots from the real renderer. Read the sheet against the game's art direction
(the manifest names the document) and the questions in README.md.

With --sign it records the verdict (pass or fail) in the manifest, against the
file's hash: validate.py refuses a model whose review is missing, failed, or
older than the file.
"""
import argparse
import datetime
import json
import os
import shutil
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

import validate  # noqa: E402
from modelkit import checks, gltf, render  # noqa: E402

QUESTIONS = [
    "Silhouette: is it recognisable in black at game size, and distinct from its neighbours?",
    "Form: do the big shapes read first, with detail where the eye should go and rest elsewhere?",
    "Scale: is it the right size next to a hero (the red bar), and are its parts in proportion?",
    "Shading: is the clay view clean (no dark blotches, no faceting where it should be smooth, no red)?",
    "Textures: is the texel density even (green), with no smears, seams in plain sight or mirrored text?",
    "Materials: do the colours, values and glow sit in the game's palette and lighting?",
    "Style: would it look made by the same hand as the rest of the game?",
]


def engine_shots(path, engine, animations, height):
    """Shots of the model in the real renderer on a neutral stage: a list of (image, caption)."""
    from PIL import Image
    work = tempfile.mkdtemp(prefix="modelkit-shots-")
    game = os.path.join(work, "game")
    shutil.copytree(os.path.join(HERE, "stage"), game)
    name = "model" + os.path.splitext(path)[1]
    os.makedirs(os.path.join(game, "models"), exist_ok=True)
    shutil.copy(path, os.path.join(game, "models", name))
    distance = max(6.0, height * 2.6)
    shots = [{"yaw": 35, "pitch": 18, "animation": None, "tick": 30, "caption": "engine: three-quarter"},
             {"yaw": 0, "pitch": 25, "animation": None, "tick": 34, "caption": "engine: game camera"}]
    tick = 40
    for animation in animations[:6]:
        tick += 30
        shots.append({"yaw": 30, "pitch": 15, "animation": animation["name"], "at": animation["seconds"] * 0.5,
                      "tick": tick, "caption": f"engine: '{animation['name']}' halfway"})
    with open(os.path.join(game, "shots.json"), "w", encoding="utf-8") as out:
        json.dump({"model": "models/" + name, "height": height, "distance": distance, "shots": shots}, out)
    user = os.path.join(work, "user")
    command = [engine, game, "--mute", "--user-data", user]
    if shutil.which("xvfb-run") and not os.environ.get("DISPLAY"):
        command = ["xvfb-run", "-a", "-s", "-screen 0 1280x720x24"] + command
    try:
        subprocess.run(command, capture_output=True, text=True, timeout=600)
    except (OSError, subprocess.TimeoutExpired) as error:
        print(f"  (no engine shots: {error})")
        return []
    images = []
    for index, shot in enumerate(shots, 1):
        file = os.path.join(user, "screenshots", f"shot_{index}.png")
        if os.path.isfile(file):
            image = Image.open(file).convert("RGB")
            w, h = image.size
            images.append((image.crop(((w - h) // 2, 0, (w + h) // 2, h)), shot["caption"]))
    shutil.rmtree(work, ignore_errors=True)
    return images


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("model", help="a .glb or .gltf file (with --manifest: its path as the manifest lists it, or a real path)")
    parser.add_argument("--profile", default="default")
    parser.add_argument("--manifest")
    parser.add_argument("--out", help="where to write the sheet (default: the manifest's reviews folder, or next to the model)")
    parser.add_argument("--engine", help="path to the windowed fengine: add shots from the real renderer")
    parser.add_argument("--sign", choices=["pass", "fail"], help="record the verdict of the review in the manifest")
    parser.add_argument("--by", default="", help="who reviewed it")
    parser.add_argument("--notes", default="", help="what was judged, in a sentence or two")
    args = parser.parse_args()

    manifest, entry, key = None, {}, None
    path = args.model
    profiles = validate.load_profiles()
    profile_name = args.profile
    art = None
    if args.manifest:
        with open(args.manifest, encoding="utf-8") as file:
            manifest = json.load(file)
        root = validate.manifest_root(args.manifest, manifest)
        for name in manifest.get("models", {}):
            if name == args.model or os.path.abspath(os.path.join(root, name)) == os.path.abspath(args.model):
                key = name
        if key is None:
            raise SystemExit(f"{args.model} is not listed in {args.manifest}")
        entry = manifest["models"][key]
        path = os.path.join(root, key)
        profiles = validate.load_profiles(manifest.get("profiles"))
        profile_name = entry.get("profile", "default")
        art = manifest.get("art_direction")
    profile = validate.profile_named(profiles, profile_name)

    if args.sign:
        if not manifest:
            raise SystemExit("--sign needs --manifest: the verdict is recorded there")
        if not args.by or len(args.notes) < 20:
            raise SystemExit("--sign needs --by and --notes saying what was judged (a sentence or two)")
        entry["review"] = {"verdict": args.sign, "sha256": validate.sha256(path), "by": args.by, "notes": args.notes,
                           "date": datetime.date.today().isoformat()}
        with open(args.manifest, "w", encoding="utf-8") as out:
            json.dump(manifest, out, indent=2)
            out.write("\n")
        print(f"recorded: {key} reviewed '{args.sign}' by {args.by}")
        return 0

    findings, model = validate.validate(path, profile, entry.get("waive", {}))
    if model is None:
        raise SystemExit(f"{path}: {findings[0].message}")
    stats = model.stats
    low, high = stats.get("bounds", [[0, 0, 0], [0, 0, 0]])
    lines = [f"profile: {profile['name']}",
             f"{stats['triangles']:,} triangles  {stats['vertices']:,} vertices  {stats['primitives']} draws",
             f"{stats['materials']} materials  {stats['images']} textures  {stats['animations']} animations",
             f"size {high[0] - low[0]:.2f} x {high[1] - low[1]:.2f} x {high[2] - low[2]:.2f} units", ""]
    groups, _ = validate.verdict(findings)
    for group, label in checks.GROUPS:
        lines.append(f"[{groups[group].upper()}] {label}")
        for finding in findings:
            if finding.group == group and finding.severity != checks.INFO:
                count = f"{finding.count:,} " if finding.count else ""
                lines.append(f"{'waived' if finding.waived else finding.severity}: {count}{finding.message}"[:84])
    lines += ["", "Judge against the art direction" + (f" ({art}):" if art else ":")]
    for question in QUESTIONS:
        words, row = question.split(), ""
        for word in words:
            if len(row) + len(word) > 80:
                lines.append(row)
                row = "   "
            row += word + " "
        lines.append(row)
    extra = []
    if args.engine:
        height = max(0.5, high[1] - low[1])
        extra = engine_shots(path, args.engine, model.animations, height)
    target = profile.get("texel_density")
    if isinstance(target, list):
        target = (target[0] * target[1]) ** 0.5
    title = os.path.relpath(path) if manifest else os.path.basename(path)
    image = render.sheet(model, title, lines, target, extra=extra, layout=profile.get("review", "object"))
    out = args.out
    if not out:
        if manifest:
            folder = os.path.join(os.path.dirname(os.path.abspath(args.manifest)), manifest.get("reviews", "model_reviews"))
            os.makedirs(folder, exist_ok=True)
            out = os.path.join(folder, key.replace("/", "_").replace("..", "").strip("_").rsplit(".", 1)[0] + ".png")
        else:
            out = os.path.splitext(path)[0] + ".review.png"
    image.save(out)
    print(f"wrote {out}")
    print("Look at it, then record the verdict with --sign pass|fail --by ... --notes ...")
    return 0


if __name__ == "__main__":
    sys.exit(main())
