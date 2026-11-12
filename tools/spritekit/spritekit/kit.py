"""The sprite kit: what kitcommon needs to know about sprite sheets (how one
is checked, loaded by the engine and drawn for review), and the two things
the shared command lines lack: a review that is several images, and review
sheets that see the rest of the manifest (the line-up of the cast)."""
import os
import sys

HERE = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.dirname(HERE))

from kitcommon import kit as common  # noqa: E402

from . import checks, sheet as sheets  # noqa: E402

QUESTIONS = [
    "Silhouette: is the figure recognisable in black at game size, and distinct from the rest of the cast (line-up)?",
    "Proportions and palette: same head-to-body ratio, outline weight, number of tones and colour family as the others of its profile?",
    "Pixels: clean clusters and one consistent outline; no orphan pixels, no banding, no smooth gradients or blur in the pixel art?",
    "Ground: do the feet stay on the ground row (cyan) through every grounded clip, and the body on the pivot in clips on the spot?",
    "Motion: does the walk read as a walk (contact, pass, contact), and does each clip read as what its name says at a glance?",
    "Attack: is there anticipation, one clear hit frame (where the 'hit' event is marked), and a recovery?",
    "Loops: does the last frame of each looping clip lead into the first (the seam strip), with no pop?",
    "Light: is the shading direction the same in every frame and direction, and does the normal map light the side the light is on?",
    "Spec: the clips, frame counts, directions and colours the art direction document asks for?",
]

# What is asked of a sprite that is not a figure: a prop, an effect, a still image, a particle.
OBJECT_QUESTIONS = [
    "Reads: is it clear what it is (a slash, a flame, a glow, a shadow, a dropped sword) at game size, at a glance?",
    "Frame: does it stay inside its frames: no straight cut edge, no box showing when it is drawn additively (the engine shot)?",
    "Time: does a flipbook build, peak and die away (or loop without a seam), with no frame that pops, blinks or repeats?",
    "Colour: are its colours those the art direction gives that thing (fire, Aetherite, frost, smoke, steel), and its family's (line-up)?",
    "Style: do its edges (hard pixels or soft glow) and level of detail match the others of its profile and the world it is drawn in?",
    "Spec: what the art direction document and the game's use of it (scale, blend, how long it is shown) ask for?",
]


def engine_load(path, sheet, engine):
    """Load the sheet with the engine itself (fe.app.sprite_clips reads the
    .anim.json, fe.app.image_size decodes each image) and hold what it says
    about every clip against what this kit read. Returns problems."""
    name = os.path.basename(path)
    files = {"sprites/" + name: path}
    for other in (sheet.atlas_name, sheet.normal_name, sheet.emissive_name):
        source = os.path.join(sheet.folder, other)
        if other and os.path.isfile(source):
            files["sprites/" + other] = source
    lua = [f'  local clips = fe.app.sprite_clips("sprites/{name}")', "  local count = 0", "  for _ in pairs(clips) do count = count + 1 end",
           f'  t.eq(count, {len(sheet.clips)}, "clips")']
    for clip in sheet.clips.values():
        key = clip.name.replace("\\", "\\\\").replace('"', '\\"')
        lua.append(f'  t.ok(clips["{key}"] ~= nil, "clip {key}")')
        lua.append(f'  t.eq(clips["{key}"].loop, {str(clip.loop).lower()}, "loop of {key}")')
        lua.append(f'  t.eq(clips["{key}"].directions, {len(clip.directions)}, "directions of {key}")')
        lua.append(f'  t.ok(math.abs(clips["{key}"].length - {clip.length(0)!r}) < 1e-6, "length of {key}")')
    for label, other, image in (("atlas", sheet.atlas_name, sheet.atlas), ("normal", sheet.normal_name, sheet.normal),
                                ("emissive", sheet.emissive_name, sheet.emissive)):
        if other and image is not None:
            lua.append(f'  local w, h = fe.app.image_size("sprites/{other}")')
            lua.append(f'  t.eq(w, {image.shape[1]}, "{label} width")')
            lua.append(f'  t.eq(h, {image.shape[0]}, "{label} height")')
    return common.engine_test(engine, files, "\n".join(lua))


def check(path, profile, engine):
    """(findings, stats line, the loaded sheet) for one .anim.json."""
    sheet = sheets.load(path)
    findings, sheet.measured = checks.run(sheet, profile)
    if engine and sheet.json is not None:
        if sheet.loadable:
            for problem in engine_load(path, sheet, engine):
                findings.append(checks.Finding("ready.engine_load", checks.ERROR, f"the engine does not load it as this kit reads it: {problem}", None,
                                               None, "The engine's own loader is the last word; fix what it names."))
        else:
            # What this kit calls unloadable the engine must refuse too, or the kit is wrong about the format.
            if not common.engine_test(engine, {"sprites/" + os.path.basename(path): path},
                                      f'  fe.app.sprite_clips("sprites/{os.path.basename(path)}")'):
                findings.append(checks.Finding("ready.engine_load", checks.INFO, "the engine loads this file although this kit refuses it"))
    return findings, checks.stats(sheet), sheet


class SpriteKit(common.Kit):
    """kitcommon's Kit, plus: the review of a sprite may be several pages
    (a character has nineteen clips in four directions), and the review sheet
    is told about the manifest so it can line the sprite up with the others
    of its profile."""

    def __init__(self):
        super().__init__(name="spritekit", here=HERE, section="sprites", groups=checks.GROUPS, noun="sprite",
                         is_asset=lambda filename: filename.endswith(".anim.json"), files=sheets.named_files,
                         check=check, sheet=self.draw, questions=QUESTIONS, unwaivable=checks.UNWAIVABLE)
        self.cast = {}          # profile name -> [(key, path)] of the manifest being reviewed
        self.manifest_path = None
        self.reviewing = None   # the profile of the sprite in hand: the questions depend on what kind of thing it is

    @property
    def questions(self):
        figure = self.reviewing is None or self.reviewing.get("grounded")
        return self._questions if figure else OBJECT_QUESTIONS

    @questions.setter
    def questions(self, value):
        self._questions = value

    def validate(self, path, profile, *args, **kwargs):
        self.reviewing = profile
        return super().validate(path, profile, *args, **kwargs)

    def _entries(self, manifest_path):
        manifest, root, entries = super()._entries(manifest_path)
        self.manifest_path = manifest_path
        self.cast = {}
        for key, entry in entries.items():
            self.cast.setdefault(entry.get("profile", "default"), []).append((key, os.path.join(root, key)))
        return manifest, root, entries

    def draw(self, path, profile, findings, sheet, lines, engine):
        from . import draw
        others = [(key, other) for key, other in self.cast.get(profile["name"], []) if os.path.abspath(other) != os.path.abspath(path)]
        title = os.path.relpath(path)
        return draw.Pages(draw.pages(sheet, profile, findings, title, lines, others, engine, HERE))
