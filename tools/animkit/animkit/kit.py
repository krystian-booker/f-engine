"""The animkit Kit: kitcommon's command lines and manifest rules around
animkit's checks and review sheet, plus the two things kitcommon's base class
cannot know: which .glb files of a game are animated at all, and that a
review of motion is several pages.
"""
import argparse
import json
import os
import struct

from kitcommon import kit as common
from kitcommon.kit import ERROR, Finding

from . import checks
from . import rig as rigs

QUESTIONS = [
    "Poses: does every key pose of the filmstrip read as a silhouette (what it is doing, and to whom), with nothing "
    "passing through the body or the ground (red)?",
    "Arcs: do the trails of the hands, feet and head run in clean curves, without kinks, zigzags or straight-line cuts?",
    "Anticipation and follow-through: does each action wind up before it and settle after it, or does it start and "
    "stop like a switch?",
    "Weight: do heavy parts start slowly, land hard and take time to stop? Does the speed graph rise and fall, or "
    "is it flat?",
    "Timing: do the blows land at the moments the game acts on (the marked lines in the speed graph), and are the "
    "holds long enough to read?",
    "Loops and hand-overs: is the seam pair one figure, not two? Do the spikes in the graph belong to the action, "
    "not to a glitch?",
    "Style: does the motion suit the character and the game's art direction (a machine moves like a machine), and "
    "do all its animations look made by the same hand?",
]


def has_animations(path):
    """Does this .glb/.gltf hold any animation? (Only its JSON is read.)"""
    try:
        with open(path, "rb") as file:
            head = file.read(20)
            if head[:4] == b"glTF":
                length, kind = struct.unpack("<II", head[12:20])
                document = json.loads(file.read(length).decode("utf-8")) if kind == 0x4E4F534A else {}
            else:
                document = json.loads((head + file.read()).decode("utf-8"))
        return bool(document.get("animations"))
    except (OSError, ValueError, struct.error, UnicodeDecodeError):
        return False


def engine_load(path, rig, engine):
    """Load the file with the engine and compare the animations it reports
    (fe.app.model_animations: names and lengths) with what was read here."""
    name = "model" + os.path.splitext(path)[1]
    files = {"models/" + name: path}
    folder = os.path.dirname(os.path.abspath(path))
    for entry in os.listdir(folder):   # buffers and textures a .gltf names
        source = os.path.join(folder, entry)
        if os.path.isfile(source) and entry != os.path.basename(path) and os.path.getsize(source) < 64 << 20 \
                and os.path.splitext(entry)[1].lower() in (".bin", ".png", ".jpg", ".jpeg", ".ktx"):
            files["models/" + entry] = source
    expected, seen = [], set()
    for index, clip in enumerate(rig.clips):
        label = clip.name or f"animation{index}"
        if label not in seen:           # of two with one name the engine keeps the first
            seen.add(label)
            expected.append((label, clip.duration))
    lua = [f'  local found = fe.app.model_animations("models/{name}")', "  local count = 0",
           "  for _ in pairs(found) do count = count + 1 end",
           f'  t.ok(count == {len(expected)}, "the engine finds " .. count .. " animations; the file was read as {len(expected)}")']
    for label, duration in expected:
        quoted = json.dumps(label)
        lua.append(f'  t.ok(found[{quoted}] ~= nil, "the engine finds no animation named " .. {quoted})')
        lua.append(f'  t.ok(math.abs(found[{quoted}].length - {duration:.6f}) < 0.002, "the engine plays " .. {quoted} .. " for " .. '
                   f'string.format("%.3f", found[{quoted}].length) .. " s; the file was read as {duration:.3f} s")')
    return common.engine_test(engine, files, "\n".join(lua))


def check(path, profile, engine):
    """Kit's check: (findings, stats line, subject)."""
    rig = rigs.load(path)
    findings, subject = checks.run(rig, profile)
    if engine:
        for problem in engine_load(path, rig, engine):
            findings.append(Finding("ready.engine_load", ERROR, f"the engine does not agree with the file: {problem}", None, None,
                                    "The engine's own loader is the last word; fix what it names."))
    return findings, checks.stats_line(subject), subject


def sheet(path, profile, findings, subject, lines, engine):
    from . import sheet as sheets
    return sheets.review(path, profile, findings, subject, lines, engine)


class AnimKit(common.Kit):
    def __init__(self, here):
        super().__init__(name="animkit", here=here, section="models", noun="model", groups=checks.GROUPS,
                         is_asset=lambda filename: False,   # see validate_main: a model is one of ours only if it is animated
                         check=check, sheet=sheet, questions=QUESTIONS)

    def validate(self, path, profile, waivers=None, review=None, engine=None, require_review=False):
        try:
            return super().validate(path, profile, waivers, review, engine, require_review)
        except rigs.gltf.GltfError as error:
            findings = [Finding("ready.structure", ERROR, f"cannot be read as glTF 2.0: {error}", None, None,
                                "Export again; this file is damaged or not a glTF.")]
            if require_review:
                findings += common.review_findings(review, self.digest(path), self.noun)
            return findings, "", None

    def validate_main(self, argv=None, description=None):
        """kitcommon's, after one thing it cannot do: a game's folders also
        hold models with no animation (modelkit's alone), so "every asset the
        game ships" means every .glb/.gltf that has animations."""
        parser = argparse.ArgumentParser(add_help=False)
        parser.add_argument("assets", nargs="*")
        parser.add_argument("--manifest")
        known, _ = parser.parse_known_args(argv)
        unlisted = []
        if known.manifest and not known.assets and os.path.isfile(known.manifest):
            manifest, root, entries = self._entries(known.manifest)
            listed = {os.path.normpath(os.path.join(root, key)) for key in entries}
            for folder in manifest.get("folders", []):
                for base, _, files in os.walk(os.path.join(root, folder)):
                    for file in sorted(files):
                        full = os.path.normpath(os.path.join(base, file))
                        relative = os.path.relpath(full, root).replace(os.sep, "/")
                        if file.lower().endswith((".glb", ".gltf")) and full not in listed and has_animations(full) \
                                and not any(common.fnmatch.fnmatch(relative, pattern) for pattern in manifest.get("ignore", [])):
                            unlisted.append(relative)
            for relative in unlisted:
                print(f"\n{relative}\n  => NOT READY: it has animations and is not listed in {os.path.basename(known.manifest)} "
                      f"(add it with a profile and its animations' spec)")
        result = super().validate_main(argv, description)
        if unlisted:
            print(f"{len(unlisted)} animated model(s) not listed in the manifest; see above")
        return 1 if unlisted else result
