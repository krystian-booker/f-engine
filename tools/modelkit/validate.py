#!/usr/bin/env python3
"""Is this model ready for the game?

    python3 tools/modelkit/validate.py models/crate.glb --profile prop
    python3 tools/modelkit/validate.py --manifest tools/aetherbound/models.json

Runs the five automated checks (mesh integrity, normals and shading,
topology, UV mapping, game readiness) on the exported glTF and, with a
manifest, enforces the sixth: that someone looked at the model's review
sheet and signed it off against the game's art direction.

Exit code 0 when every model passes: no errors, and no warnings unless a
manifest waives them with a reason. `--lenient` lets warnings through.
See tools/modelkit/README.md.
"""
import argparse
import hashlib
import json
import os
import shutil
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.dirname(HERE))

from kitcommon import kit  # noqa: E402
from modelkit import checks, gltf  # noqa: E402

LABELS = dict(checks.GROUPS)
LABELS["visual"] = "Visual consistency"
ORDER = [group for group, _ in checks.GROUPS] + ["visual"]


# What no reason excuses: a file the engine cannot read, and the look nobody judged.
UNWAIVABLE = {"ready.structure", "ready.engine_load", "visual.review"}


def load_profiles(extra=None):
    with open(os.path.join(HERE, "profiles.json"), encoding="utf-8") as file:
        profiles = json.load(file)
    for name, values in (extra or {}).items():
        profiles[name] = {**profiles.get(name, {}), **values}
    return profiles


def profile_named(profiles, name):
    if name not in profiles or name.startswith("_"):
        known = ", ".join(sorted(key for key in profiles if not key.startswith("_") and key != "default"))
        raise SystemExit(f"no profile '{name}' (known: {known})")
    merged = {**profiles["default"], **profiles[name], "name": name}
    return {key: value for key, value in merged.items() if not key.startswith("_")}


def manifest_root(manifest_path, manifest):
    """The folder a manifest's model paths start from: its own, or its "root" relative to it."""
    return os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(manifest_path)), manifest.get("root", ".")))


def sha256(path):
    digest = hashlib.sha256()
    with open(path, "rb") as file:
        for block in iter(lambda: file.read(1 << 20), b""):
            digest.update(block)
    return digest.hexdigest()


def engine_load(path, engine):
    """Load the model with the engine itself; returns a list of problems."""
    work = tempfile.mkdtemp(prefix="modelkit-")
    game = os.path.join(work, "game")
    os.makedirs(os.path.join(game, "tests"))
    os.makedirs(os.path.join(game, "models"))
    name = "model" + os.path.splitext(path)[1]
    folder = os.path.dirname(os.path.abspath(path))
    # Next to the model: buffers and textures a .gltf names.
    for entry in os.listdir(folder):
        source = os.path.join(folder, entry)
        if os.path.isfile(source) and entry != os.path.basename(path) and os.path.getsize(source) < 64 << 20 \
                and os.path.splitext(entry)[1].lower() in (".bin", ".png", ".jpg", ".jpeg", ".ktx"):
            shutil.copy(source, os.path.join(game, "models", entry))
    shutil.copy(path, os.path.join(game, "models", name))  # the engine refuses links that leave the game folder
    with open(os.path.join(game, "game.json"), "w", encoding="utf-8") as out:
        json.dump({"title": "modelkit", "id": "modelkit"}, out)
    with open(os.path.join(game, "main.lua"), "w", encoding="utf-8") as out:
        out.write("return { init = function() end, update = function() end, draw = function() end }\n")
    with open(os.path.join(game, "tests", "load.lua"), "w", encoding="utf-8") as out:
        out.write('return { ["the engine loads the model"] = function(t)\n'
                  f'  local animations = fe.app.model_animations("models/{name}")\n'
                  '  t.ok(type(animations) == "table")\nend }\n')
    try:
        result = subprocess.run([engine, "--test", game], capture_output=True, text=True, timeout=120)
    except (OSError, subprocess.TimeoutExpired) as error:
        return [f"could not run the engine: {error}"]
    finally:
        shutil.rmtree(work, ignore_errors=True)
    if result.returncode == 0:
        return []
    # The line after the FAIL line is the loader's own message.
    lines = (result.stdout + result.stderr).splitlines()
    for index, line in enumerate(lines):
        if "FAIL" in line and index + 1 < len(lines):
            return [lines[index + 1].strip()]
    return ["the engine refused the model"]


def validate(path, profile, waivers=None, review=None, engine=None, require_review=False):
    """Returns (findings, model or None)."""
    findings = []
    try:
        model = gltf.load(path)
    except (gltf.GltfError, OSError, KeyError, IndexError, ValueError) as error:
        return [checks.Finding("ready.structure", checks.ERROR, f"cannot be read as glTF 2.0: {error}", None, None,
                               "Export again; this file is damaged or not a glTF.")], None
    findings.extend(checks.run(model, profile))
    if engine:
        for problem in engine_load(path, engine):
            findings.append(checks.Finding("ready.engine_load", checks.ERROR, f"the engine cannot load it: {problem}", None, None,
                                           "The engine's own loader is the last word; fix what it names."))
    if require_review:
        findings.extend(kit.review_findings(review, sha256(path), "model"))
    kit.apply_waivers(findings, waivers, UNWAIVABLE)
    return findings, model


def verdict(findings, lenient=False):
    """Per group: 'fail', 'warn', 'waived' or 'pass'; and whether the model passes."""
    groups = {group: "pass" for group in ORDER}
    rank = {"pass": 0, "waived": 1, "warn": 2, "fail": 3}
    ok = True
    for finding in findings:
        if finding.severity == checks.INFO:
            continue
        if finding.waived:
            state = "waived"
        elif finding.severity == checks.ERROR:
            state = "fail"
        else:
            state = "warn"
        if rank[state] > rank[groups[finding.group]]:
            groups[finding.group] = state
        if state == "fail" or (state == "warn" and not lenient):
            ok = False
    return groups, ok


def print_report(path, profile, findings, model, lenient, show_info, reviewed):
    groups, ok = verdict(findings, lenient)
    mark = {"pass": "PASS", "waived": "PASS*", "warn": "WARN", "fail": "FAIL"}
    print(f"\n{path}  (profile: {profile['name']})")
    if model is not None and hasattr(model, "stats"):
        s = model.stats
        size = ""
        if "bounds" in s:
            low, high = s["bounds"]
            size = f", {high[0] - low[0]:.1f} x {high[1] - low[1]:.1f} x {high[2] - low[2]:.1f} units"
        print(f"  {s['triangles']:,} triangles, {s['vertices']:,} vertices, {s['primitives']} draws, {s['materials']} materials, "
              f"{s['images']} textures, {s['animations']} animations, {s['file_bytes'] / 1e6:.1f} MB{size}")
    for group in ORDER:
        if group == "visual" and not reviewed:
            print(f"  [ -- ] {LABELS[group]}: not checked here (review.py makes the sheet; a manifest enforces the sign-off)")
            continue
        print(f"  [{mark[groups[group]]:^5}] {LABELS[group]}")
        for finding in findings:
            if finding.group != group or (finding.severity == checks.INFO and not show_info):
                continue
            count = f"{finding.count:,} " if finding.count else ""
            where = f" in {finding.where}" if finding.where else ""
            tag = "waived" if finding.waived else finding.severity
            print(f"          {tag:>6}  {finding.check}: {count}{finding.message}{where}")
            if finding.waived:
                print(f"                  waived: {finding.waived}")
            elif finding.fix and finding.severity != checks.INFO:
                print(f"                  fix: {finding.fix}")
    print(f"  => {'READY' if ok else 'NOT READY'}")
    return ok


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("models", nargs="*", help=".glb or .gltf files")
    parser.add_argument("--profile", default="default", help="a profile of profiles.json (prop, character, boss, map, ...)")
    parser.add_argument("--manifest", help="a game's models.json: models with their profiles, waivers and reviews")
    parser.add_argument("--engine", help="path to fengine_headless: also load each model with the engine")
    parser.add_argument("--lenient", action="store_true", help="warnings do not fail the run")
    parser.add_argument("--info", action="store_true", help="also print informational findings")
    parser.add_argument("--json", help="write the full report here")
    parser.add_argument("--no-review", action="store_true", help="with a manifest: skip the visual sign-off (while iterating)")
    args = parser.parse_args()
    if not args.models and not args.manifest:
        parser.error("name a model or a manifest")

    report, all_ok = [], True
    jobs = []
    if args.manifest:
        with open(args.manifest, encoding="utf-8") as file:
            manifest = json.load(file)
        root = manifest_root(args.manifest, manifest)
        profiles = load_profiles(manifest.get("profiles"))
        for name, entry in manifest.get("models", {}).items():
            if args.models and not any(os.path.abspath(m) == os.path.normpath(os.path.join(root, name)) for m in args.models):
                continue
            jobs.append((os.path.join(root, name), profile_named(profiles, entry.get("profile", "default")),
                         entry.get("waive", {}), entry.get("review"), not args.no_review))
        # A model in the game that the manifest does not know is not validated at all: say so.
        listed = {os.path.normpath(os.path.join(root, name)) for name in manifest.get("models", {})}
        for folder in manifest.get("folders", []):
            for base, _, files in os.walk(os.path.join(root, folder)):
                for file in sorted(files):
                    full = os.path.normpath(os.path.join(base, file))
                    if file.lower().endswith((".glb", ".gltf")) and full not in listed and not args.models:
                        print(f"\n{full}\n  => NOT READY: not listed in {os.path.basename(args.manifest)} (add it with a profile)")
                        all_ok = False
    else:
        profiles = load_profiles()
        profile = profile_named(profiles, args.profile)
        jobs = [(path, profile, {}, None, False) for path in args.models]

    for path, profile, waivers, review, require_review in jobs:
        findings, model = validate(path, profile, waivers, review, args.engine, require_review)
        ok = print_report(os.path.relpath(path), profile, findings, model, args.lenient, args.info, require_review)
        all_ok = all_ok and ok
        report.append({"model": path, "profile": profile["name"], "ready": ok,
                       "stats": getattr(model, "stats", None), "findings": [f.to_json() for f in findings]})
    if args.json:
        with open(args.json, "w", encoding="utf-8") as out:
            json.dump(report, out, indent=1)
    ready = sum(1 for entry in report if entry["ready"])
    waived = sum(1 for entry in report if entry["ready"] and any(f.get("waived") for f in entry["findings"]))
    print(f"\n{ready} of {len(report)} model(s) ready" + (f" ({waived} of them with waived flaws)" if waived else "")
          + ("" if all_ok else "; see above"))
    return 0 if all_ok else 1


if __name__ == "__main__":
    sys.exit(main())
