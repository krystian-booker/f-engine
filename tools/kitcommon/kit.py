"""What every asset kit shares (modelkit, animkit, texturekit, spritekit):
findings, profiles, a game's manifest with waivers and visual reviews, the
report, and the two command lines (`validate.py`, `review.py`).

A kit supplies the part that knows its kind of asset:

    kit = Kit(
        name="spritekit", here=HERE, section="sprites",
        groups=[("sheet", "Sheet integrity"), ...],          # the measured checks, in order
        is_asset=lambda filename: filename.endswith(".anim.json"),
        files=lambda path: [path, atlas, normal],            # what the asset is made of (the review is bound to their hash)
        check=lambda path, profile, engine: (findings, stats_line, subject),
        sheet=lambda path, profile, findings, subject, lines, engine: PIL.Image,
        questions=[...],                                     # what the reviewer answers
    )
    sys.exit(kit.validate_main())     # in validate.py
    sys.exit(kit.review_main())       # in review.py

and everything else behaves the same in every kit: exit code 0 only with no
errors and no unwaived warnings; a manifest lists every asset the game ships
with its profile, its waivers (a reason, and a cap so a flaw cannot grow) and
a visual review bound to the files' hash; an asset in the game's folders that
the manifest does not list fails the run.
"""
import argparse
import datetime
import fnmatch
import hashlib
import json
import os
import shutil
import subprocess
import tempfile

ERROR, WARN, INFO = "error", "warn", "info"


class Finding:
    """One thing a check found. `check` is "group.name"; `count` is how many
    (faces, frames, pixels...), which is what a waiver's cap is compared with."""

    def __init__(self, check, severity, message, count=None, where=None, fix=None):
        self.check, self.severity, self.message = check, severity, message
        self.count, self.where, self.fix = count, where, fix
        self.waived = None  # the reason, when a manifest waives this check

    @property
    def group(self):
        return self.check.split(".")[0]

    def to_json(self):
        return {key: value for key, value in vars(self).items() if value is not None}


# ------------------------------------------------------------------ files

def sha256(*paths):
    """The hash of one file, or of several taken together (in the order given)."""
    digest = hashlib.sha256()
    for path in paths:
        with open(path, "rb") as file:
            for block in iter(lambda: file.read(1 << 20), b""):
                digest.update(block)
    return digest.hexdigest()


def manifest_root(manifest_path, manifest):
    """The folder a manifest's paths start from: its own, or its "root" relative to it."""
    return os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(manifest_path)), manifest.get("root", ".")))


def load_profiles(profiles_path, extra=None):
    with open(profiles_path, encoding="utf-8") as file:
        profiles = json.load(file)
    for name, values in (extra or {}).items():
        profiles[name] = {**profiles.get(name, {}), **values}
    return profiles


def profile_named(profiles, name, overrides=None):
    """`default` overlaid with the named profile and then with an asset's own `overrides`."""
    if name not in profiles or name.startswith("_"):
        known = ", ".join(sorted(key for key in profiles if not key.startswith("_") and key != "default"))
        raise SystemExit(f"no profile '{name}' (known: {known})")
    merged = {**profiles.get("default", {}), **profiles[name], **(overrides or {}), "name": name}
    return {key: value for key, value in merged.items() if not key.startswith("_")}


# ------------------------------------------------------------------ waivers and reviews

REVIEW = "visual.review"


def apply_waivers(findings, waivers, unwaivable=()):
    """Mark the findings a manifest waives. A waiver is a reason, or
    {"reason", "max"}: with a cap it holds only while the total count of that
    check stays at or under it. Nothing in `unwaivable` (and never the visual
    review) can be waived."""
    totals = {}
    for finding in findings:
        totals[finding.check] = totals.get(finding.check, 0) + (finding.count or 1)
    for finding in findings:
        waiver = (waivers or {}).get(finding.check)
        if not waiver or finding.severity == INFO or finding.check == REVIEW or finding.check in unwaivable:
            continue
        reason, cap = (waiver, None) if isinstance(waiver, str) else (waiver.get("reason"), waiver.get("max"))
        if not reason:
            continue
        if cap is not None and totals[finding.check] > cap:
            finding.message += f" (waived up to {cap:,}; now {totals[finding.check]:,})"
            continue
        finding.waived = reason
    return findings


def review_findings(review, digest, what="asset"):
    """The sixth check: the recorded visual review must be a pass of exactly these files."""
    if not review:
        return [Finding(REVIEW, ERROR, "no visual review on record", None, None,
                        "Make the review sheet (review.py), look at it against the art direction, and record the verdict (review.py --sign).")]
    if review.get("sha256") != digest:
        return [Finding(REVIEW, ERROR, f"the {what} changed after its visual review", None, None,
                        "Review the new version and sign it off again.")]
    if review.get("verdict") != "pass":
        return [Finding(REVIEW, ERROR, f"its visual review says '{review.get('verdict')}': {review.get('notes', '')}", None, None,
                        "Fix what the review names, then review again.")]
    return []


def verdict(findings, order, lenient=False):
    """Per group: 'fail', 'warn', 'waived' or 'pass'; and whether the asset passes."""
    groups = {group: "pass" for group in order}
    rank = {"pass": 0, "waived": 1, "warn": 2, "fail": 3}
    ok = True
    for finding in findings:
        if finding.severity == INFO:
            continue
        state = "waived" if finding.waived else "fail" if finding.severity == ERROR else "warn"
        groups.setdefault(finding.group, "pass")
        if rank[state] > rank[groups[finding.group]]:
            groups[finding.group] = state
        if state == "fail" or (state == "warn" and not lenient):
            ok = False
    return groups, ok


# ------------------------------------------------------------------ the engine

def engine_test(engine, files, test_lua, game_json=None, timeout=120):
    """Run a throwaway game's test under `fengine_headless --test` and return
    the problems (an empty list when it passes). `files` maps a path inside
    the game to a source file to copy there (the engine refuses links that
    leave the game folder); `test_lua` is the body of a function(t) that
    asserts with t.ok / t.eq."""
    work = tempfile.mkdtemp(prefix="kit-")
    game = os.path.join(work, "game")
    os.makedirs(os.path.join(game, "tests"))
    try:
        for inside, source in files.items():
            target = os.path.join(game, inside)
            os.makedirs(os.path.dirname(target), exist_ok=True)
            shutil.copy(source, target)
        with open(os.path.join(game, "game.json"), "w", encoding="utf-8") as out:
            json.dump(game_json or {"title": "kit", "id": "kit"}, out)
        with open(os.path.join(game, "main.lua"), "w", encoding="utf-8") as out:
            out.write("return { init = function() end, update = function() end, draw = function() end }\n")
        with open(os.path.join(game, "tests", "load.lua"), "w", encoding="utf-8") as out:
            out.write('return { ["the engine loads it"] = function(t)\n' + test_lua + "\nend }\n")
        try:
            result = subprocess.run([engine, "--test", game], capture_output=True, text=True, timeout=timeout)
        except (OSError, subprocess.TimeoutExpired) as error:
            return [f"could not run the engine: {error}"]
        if result.returncode == 0:
            return []
        lines = (result.stdout + result.stderr).splitlines()
        for index, line in enumerate(lines):
            if "FAIL" in line and index + 1 < len(lines):  # the line after FAIL is the engine's own message
                return [lines[index + 1].strip()]
        return ["the engine refused it"]
    finally:
        shutil.rmtree(work, ignore_errors=True)


def windowed(command):
    """The command to run the windowed runtime: under xvfb-run where there is no display."""
    if shutil.which("xvfb-run") and not os.environ.get("DISPLAY"):
        return ["xvfb-run", "-a", "-s", "-screen 0 1280x720x24"] + command
    return command


# ------------------------------------------------------------------ review sheets

def label(image, text):
    """A caption bar across the top of a tile."""
    from PIL import ImageDraw
    draw = ImageDraw.Draw(image)
    draw.rectangle([0, 0, image.width, 15], fill=(0, 0, 0))
    draw.text((4, 2), text, fill=(255, 255, 255))
    return image


def compose(tiles, title, lines, columns=4, side=520):
    """Tiles (PIL images, any sizes) in rows of `columns`, with the title and
    the lines (numbers, findings, questions) down the right side."""
    from PIL import Image, ImageDraw
    rows, x, y, row_height, width = [], 0, 0, 0, 0
    for index, tile in enumerate(tiles):
        if index and index % columns == 0:
            y, x, row_height = y + row_height, 0, 0
        rows.append((tile, x, y))
        x += tile.width
        row_height = max(row_height, tile.height)
        width = max(width, x)
    height = max(y + row_height, 40 + 15 * (len(lines) + 2))
    out = Image.new("RGB", (max(width, 64) + side, height), (20, 21, 28))
    for tile, x, y in rows:
        out.paste(tile.convert("RGB"), (x, y))
    draw = ImageDraw.Draw(out)
    draw.text((max(width, 64) + 12, 10), title, fill=(255, 220, 140))
    for row, line in enumerate(lines):
        colour = (255, 120, 120) if line.startswith("error") else (255, 210, 120) if line.startswith("warn") else (200, 204, 216)
        draw.text((max(width, 64) + 12, 34 + row * 15), line[:84], fill=colour)
    return out


def wrap(text, width=80, indent="   "):
    lines, row = [], ""
    for word in text.split():
        if len(row) + len(word) > width:
            lines.append(row)
            row = indent
        row += word + " "
    return lines + [row]


# ------------------------------------------------------------------ a kit

class Kit:
    def __init__(self, name, here, section, groups, is_asset, check, sheet, questions,
                 files=None, unwaivable=(), noun="asset"):
        self.name, self.here, self.section, self.groups = name, here, section, list(groups)
        self.is_asset, self.check, self.sheet, self.questions = is_asset, check, sheet, questions
        self.files = files or (lambda path: [path])
        self.unwaivable, self.noun = set(unwaivable), noun
        self.order = [group for group, _ in self.groups] + ["visual"]
        self.labels = {**dict(self.groups), "visual": "Visual consistency"}

    # --- shared

    def profiles(self, manifest=None):
        return load_profiles(os.path.join(self.here, "profiles.json"), (manifest or {}).get("profiles"))

    def digest(self, path):
        return sha256(*[file for file in self.files(path) if os.path.isfile(file)])

    def validate(self, path, profile, waivers=None, review=None, engine=None, require_review=False):
        """All findings for one asset, with the manifest's waivers applied.
        Returns (findings, stats_line, subject); `subject` is whatever the
        kit's check loaded, for the review sheet."""
        try:
            findings, stats, subject = self.check(path, profile, engine)
        except (OSError, ValueError, KeyError, IndexError) as error:
            findings, stats, subject = [Finding("ready.structure", ERROR, f"cannot be read: {error}", None, None,
                                                "Write the file again; it is damaged or not what its name says.")], "", None
        if require_review:
            findings = findings + review_findings(review, self.digest(path), self.noun)
        apply_waivers(findings, waivers, self.unwaivable | {"ready.structure", "ready.engine_load"})
        return findings, stats, subject

    def report(self, path, profile, findings, stats, lenient, show_info, reviewed):
        groups, ok = verdict(findings, self.order, lenient)
        mark = {"pass": "PASS", "waived": "PASS*", "warn": "WARN", "fail": "FAIL"}
        print(f"\n{path}  (profile: {profile['name']})")
        if stats:
            print(f"  {stats}")
        for group in self.order:
            if group == "visual" and not reviewed:
                print(f"  [ -- ] {self.labels[group]}: not checked here (review.py makes the sheet; a manifest enforces the sign-off)")
                continue
            print(f"  [{mark[groups[group]]:^5}] {self.labels[group]}")
            for finding in findings:
                if finding.group != group or (finding.severity == INFO and not show_info):
                    continue
                count = f"{finding.count:,} " if finding.count else ""
                where = f" in {finding.where}" if finding.where else ""
                tag = "waived" if finding.waived else finding.severity
                print(f"          {tag:>6}  {finding.check}: {count}{finding.message}{where}")
                if finding.waived:
                    print(f"                  waived: {finding.waived}")
                elif finding.fix and finding.severity != INFO:
                    print(f"                  fix: {finding.fix}")
        print(f"  => {'READY' if ok else 'NOT READY'}")
        return ok

    def _entries(self, manifest_path):
        with open(manifest_path, encoding="utf-8") as file:
            manifest = json.load(file)
        return manifest, manifest_root(manifest_path, manifest), manifest.get(self.section, {})

    def _key(self, root, entries, name):
        """The manifest key a command-line name refers to: the key itself, or a real path to it."""
        for key in entries:
            if key == name or os.path.abspath(os.path.join(root, key)) == os.path.abspath(name):
                return key
        return None

    # --- validate.py

    def validate_main(self, argv=None, description=None):
        parser = argparse.ArgumentParser(description=description, formatter_class=argparse.RawDescriptionHelpFormatter)
        parser.add_argument("assets", nargs="*", help=f"{self.noun} files (with --manifest: only these of its entries)")
        parser.add_argument("--profile", default="default", help="a profile of profiles.json")
        parser.add_argument("--manifest", help=f"a game's manifest: its {self.section} with their profiles, waivers and reviews")
        parser.add_argument("--engine", help="path to fengine_headless: also load each one with the engine")
        parser.add_argument("--lenient", action="store_true", help="warnings do not fail the run")
        parser.add_argument("--info", action="store_true", help="also print informational findings")
        parser.add_argument("--json", help="write the full report here")
        parser.add_argument("--no-review", action="store_true", help="with a manifest: skip the visual sign-off (while iterating)")
        args = parser.parse_args(argv)
        if not args.assets and not args.manifest:
            parser.error(f"name a {self.noun} or a manifest")

        jobs, all_ok = [], True
        if args.manifest:
            manifest, root, entries = self._entries(args.manifest)
            profiles = self.profiles(manifest)
            wanted = {self._key(root, entries, name) for name in args.assets}
            if None in wanted:
                parser.error(f"not in {args.manifest}: " + ", ".join(n for n in args.assets if self._key(root, entries, n) is None))
            for key, entry in entries.items():
                if wanted and key not in wanted:
                    continue
                jobs.append((os.path.join(root, key), profile_named(profiles, entry.get("profile", "default"), entry.get("overrides")),
                             entry.get("waive", {}), entry.get("review"), not args.no_review))
            # What the game ships and the manifest does not know is not validated at all: say so.
            listed = {os.path.normpath(os.path.join(root, key)) for key in entries}
            ignore = manifest.get("ignore", [])
            for folder in manifest.get("folders", []) if not wanted else []:
                for base, _, files in os.walk(os.path.join(root, folder)):
                    for file in sorted(files):
                        full = os.path.normpath(os.path.join(base, file))
                        relative = os.path.relpath(full, root).replace(os.sep, "/")
                        if self.is_asset(file) and full not in listed and not any(fnmatch.fnmatch(relative, p) for p in ignore):
                            print(f"\n{relative}\n  => NOT READY: not listed in {os.path.basename(args.manifest)} (add it with a profile)")
                            all_ok = False
        else:
            profile = profile_named(self.profiles(), args.profile)
            jobs = [(path, profile, {}, None, False) for path in args.assets]

        report = []
        for path, profile, waivers, review, require_review in jobs:
            findings, stats, _ = self.validate(path, profile, waivers, review, args.engine, require_review)
            ok = self.report(os.path.relpath(path), profile, findings, stats, args.lenient, args.info, require_review)
            all_ok = all_ok and ok
            report.append({self.noun: path, "profile": profile["name"], "ready": ok, "stats": stats,
                           "findings": [finding.to_json() for finding in findings]})
        if args.json:
            with open(args.json, "w", encoding="utf-8") as out:
                json.dump(report, out, indent=1)
        ready = sum(1 for entry in report if entry["ready"])
        waived = sum(1 for entry in report if entry["ready"] and any(f.get("waived") for f in entry["findings"]))
        print(f"\n{ready} of {len(report)} {self.noun}(s) ready" + (f" ({waived} of them with waived flaws)" if waived else "")
              + ("" if all_ok else "; see above"))
        return 0 if all_ok else 1

    # --- review.py

    def review_main(self, argv=None, description=None):
        parser = argparse.ArgumentParser(description=description, formatter_class=argparse.RawDescriptionHelpFormatter)
        parser.add_argument("assets", nargs="*", help=f"{self.noun} files (with --manifest: its entries, by key or real path)")
        parser.add_argument("--profile", default="default")
        parser.add_argument("--manifest")
        parser.add_argument("--unreviewed", action="store_true", help="with --manifest: every entry whose review is missing or stale")
        parser.add_argument("--out", help="where to write the sheet(s): a .png for one asset, a folder for several "
                                          "(default: the manifest's reviews folder, or next to the file)")
        parser.add_argument("--engine", help="path to the windowed fengine: add shots from the real renderer where the kit can")
        parser.add_argument("--sign", choices=["pass", "fail"], help="record the verdict of the review in the manifest")
        parser.add_argument("--by", default="", help="who reviewed it")
        parser.add_argument("--notes", default="", help="what was judged, in a sentence or two")
        args = parser.parse_args(argv)

        manifest, root, entries, art = None, None, {}, None
        if args.manifest:
            manifest, root, entries = self._entries(args.manifest)
            art = manifest.get("art_direction")
            keys = [self._key(root, entries, name) for name in args.assets]
            if None in keys:
                raise SystemExit(f"not in {args.manifest}: " + ", ".join(n for n, k in zip(args.assets, keys) if k is None))
            if args.unreviewed:
                for key, entry in entries.items():
                    review = entry.get("review")
                    if key not in keys and (not review or review.get("sha256") != self.digest(os.path.join(root, key))):
                        keys.append(key)
            if not keys:
                raise SystemExit("nothing to review" if args.unreviewed else f"name the {self.noun}(s) to review, or --unreviewed")
            jobs = [(key, os.path.join(root, key), entries[key]) for key in keys]
            profiles = self.profiles(manifest)
        else:
            if args.sign:
                raise SystemExit("--sign needs --manifest: the verdict is recorded there")
            if not args.assets:
                raise SystemExit(f"name the {self.noun}(s) to review")
            jobs = [(None, path, {"profile": args.profile}) for path in args.assets]
            profiles = self.profiles()

        if args.sign:
            if not args.by or len(args.notes) < 20:
                raise SystemExit("--sign needs --by and --notes saying what was judged (a sentence or two)")
            for key, path, entry in jobs:
                entry["review"] = {"verdict": args.sign, "sha256": self.digest(path), "by": args.by, "notes": args.notes,
                                   "date": datetime.date.today().isoformat()}
                print(f"recorded: {key} reviewed '{args.sign}' by {args.by}")
            with open(args.manifest, "w", encoding="utf-8") as out:
                json.dump(manifest, out, indent=2)
                out.write("\n")
            return 0

        for key, path, entry in jobs:
            profile = profile_named(profiles, entry.get("profile", "default"), entry.get("overrides"))
            findings, stats, subject = self.validate(path, profile, entry.get("waive", {}))
            if subject is None:
                print(f"{path}: {findings[0].message}")
                continue
            lines = [f"profile: {profile['name']}"] + ([stats] if stats else []) + [""]
            groups, _ = verdict(findings, self.order)
            for group, text in self.groups:
                lines.append(f"[{groups[group].upper()}] {text}")
                for finding in findings:
                    if finding.group == group and finding.severity != INFO:
                        count = f"{finding.count:,} " if finding.count else ""
                        lines.append(f"{'waived' if finding.waived else finding.severity}: {count}{finding.message}"[:84])
            lines += ["", "Judge against the art direction" + (f" ({art}):" if art else ":")]
            for question in self.questions:
                lines += wrap(question)
            image = self.sheet(path, profile, findings, subject, lines, args.engine)
            out = self._sheet_path(args, manifest, key, path, len(jobs) > 1)
            image.save(out)
            print(f"wrote {out}")
        print("Look at each sheet, then record the verdict with --sign pass|fail --by ... --notes ...")
        return 0

    def _sheet_path(self, args, manifest, key, path, several):
        name = (key or os.path.basename(path)).replace("/", "_").replace("\\", "_").replace("..", "").replace("#", "-").strip("_")
        name = name.rsplit(".", 1)[0].replace(".anim", "") + ".png"
        if args.out and not several and args.out.lower().endswith(".png"):
            return args.out
        if args.out:
            os.makedirs(args.out, exist_ok=True)
            return os.path.join(args.out, name)
        if manifest is not None:
            folder = os.path.join(os.path.dirname(os.path.abspath(args.manifest)),
                                  manifest.get("reviews", self.section.rstrip("s") + "_reviews"))
            os.makedirs(folder, exist_ok=True)
            return os.path.join(folder, name)
        return os.path.splitext(path)[0] + ".review.png"
