"""texturekit's Kit: kitcommon's, plus what textures need that other assets do not.

- A texture inside a model is addressed `models/colossus.glb#colossus_albedo`;
  its review is bound to the .glb's hash, and a .glb's embedded images are
  discovered, so one the manifest does not list fails the run.
- Not every image is a texture: the atlas, normal and emissive images that an
  `.anim.json` names are sprite sheets (spritekit's), and are left out.
- A name on the command line may be a group: a folder (`ui`), a model
  (`maps/fortress/geometry.glb`: every texture in it) or a glob
  (`maps/fortress/sky_*`), so related textures are reviewed and signed together.
- The checks get the game around the texture: who references a file, which
  other texture has the same pixels, the game's palette.
"""
import fnmatch
import hashlib
import json
import os
import re
import sys

from kitcommon import kit as common
from kitcommon.kit import ERROR, INFO, WARN, Finding

from . import checks, sheet
from . import image as ti
from . import tools

VALUE_OPTIONS = ("--profile", "--manifest", "--engine", "--json", "--out", "--sign", "--by", "--notes")
REFERENCE = re.compile(r"""["']([^"'\n]+\.(?:png|jpe?g|tga|bmp|gif|cube))["']""", re.IGNORECASE)

QUESTIONS = [
    "1. Scale: at the size the game shows it (1:1 and the small mips), does it still read as what it is?",
    "2. Seams and repeat: tiled 3x3, is there a line where it wraps, or one blotch that turns into a lattice?",
    "3. Palette and value: do its colours and its light-to-dark range sit in the art direction's palette (the "
    "swatches on the sheet), neither crushed nor washed out?",
    "4. Pixel discipline: hard edges, a small deliberate palette, no blur, no stray anti-aliasing, no stretched "
    "or half-size texels; the same texel size as the textures next to it (see the group sheet)?",
    "5. Edges: on black and on white, is there a halo or a cut-off edge? Does a normal map light as bumps, not "
    "dents, from both light directions?",
    "6. Authorship: does everything in it look meant (no noise that reads as static, no generated-by-accident "
    "pattern), and made by the same hand as its neighbours?",
]


class Game:
    """The game a manifest describes, as far as textures need it: its files, who names
    which, and which textures hold the same pixels. Everything is read once, on demand."""

    def __init__(self, manifest_path, manifest, root, entries):
        self.manifest_path, self.manifest, self.root, self.entries = manifest_path, manifest, root, entries
        self.ignore = manifest.get("ignore", [])
        self._files = self._named = self._lua = self._hashes = self._palette = self._sprites = None

    def relative(self, path):
        return os.path.relpath(path, self.root).replace(os.sep, "/")

    def files(self):
        """Every file under the manifest's folders that no `ignore` glob matches, relative to the root."""
        if self._files is None:
            found = set()
            for folder in self.manifest.get("folders", ["."]):
                for base, _, names in os.walk(os.path.join(self.root, folder)):
                    for name in names:
                        relative = self.relative(os.path.normpath(os.path.join(base, name)))
                        if not any(fnmatch.fnmatch(relative, pattern) for pattern in self.ignore):
                            found.add(relative)
            self._files = sorted(found)
        return self._files

    def models(self):
        return [os.path.join(self.root, name) for name in self.files() if name.lower().endswith(ti.MODEL_EXTENSIONS)]

    def sprite_images(self):
        """Images an .anim.json names as atlas, normal or emissive: sprite sheets, not textures."""
        if self._sprites is None:
            self._sprites = set()
            for name in self.files():
                if not name.endswith(".anim.json"):
                    continue
                try:
                    with open(os.path.join(self.root, name), encoding="utf-8") as file:
                        sprite = json.load(file)
                except (OSError, ValueError):
                    continue
                for key in ("atlas", "normal", "emissive"):
                    if isinstance(sprite.get(key), str) and sprite[key]:
                        self._sprites.add(os.path.normpath(os.path.join(self.root, os.path.dirname(name), sprite[key])))
        return self._sprites

    def _read_references(self):
        """Every string in the game's JSON, Lua and glTF that ends like an image or a LUT,
        resolved against the game folder and against the folder of the file that holds it."""
        self._named, lua = set(), []
        for name in self.files():
            path = os.path.join(self.root, name)
            lower = name.lower()
            try:
                if lower.endswith(".glb"):
                    text = json.dumps(ti.model(path).json.get("images", []))
                elif lower.endswith((".lua", ".json", ".gltf")):
                    with open(path, encoding="utf-8", errors="replace") as file:
                        text = file.read()
                else:
                    continue
            except (OSError, ValueError):
                continue
            if lower.endswith(".lua"):
                lua.append(text)
            for match in REFERENCE.finditer(text):
                reference = match.group(1).replace("\\/", "/")
                self._named.add(os.path.normpath(os.path.join(self.root, reference)))
                self._named.add(os.path.normpath(os.path.join(os.path.dirname(path), reference)))
        self._lua = "\n".join(lua)

    def referenced(self, texture):
        """(whether the game names this file, a note when it only does so by building the name in code)."""
        if self._named is None:
            self._read_references()
        path = os.path.normpath(os.path.abspath(texture.container))
        if path in self._named:
            return True, None
        relative = self.relative(path)
        folder, name = os.path.split(relative)
        stem, extension = os.path.splitext(name)
        quoted = lambda text: f'"{text}"' in self._lua or f"'{text}'" in self._lua  # noqa: E731
        if folder and quoted(stem) and (f'"{folder}/' in self._lua or f"'{folder}/" in self._lua) and quoted(extension):
            return True, f'no file names it whole; Lua builds the name from "{folder}/", "{stem}" and "{extension}"'
        return False, None

    def _hash(self, key):
        try:
            texture = ti.load(os.path.join(self.root, key))
        except (OSError, ValueError, KeyError, IndexError):
            return None
        if texture.rgba is None:
            return None
        digest = hashlib.sha1(f"{texture.width}x{texture.height}".encode())
        digest.update(texture.rgba.tobytes())
        return digest.hexdigest()

    def copies(self, texture):
        """[(other key, relation, severity)]: the manifest's other textures with the same pixels."""
        if self._hashes is None:
            self._hashes = {}
            for key in self.entries:
                value = self._hash(key)
                if value:
                    self._hashes.setdefault(value, []).append(key)
        mine = self.relative(texture.path) if not texture.embedded else \
            self.relative(texture.container) + "#" + texture.name
        digest = hashlib.sha1(f"{texture.width}x{texture.height}".encode())
        digest.update(texture.rgba.tobytes())
        out = []
        for other in self._hashes.get(digest.hexdigest(), []):
            if other == mine:
                continue
            container, inner = ti.split(other)
            if not texture.embedded:
                out.append((other, "the game ships it twice: as this file and inside that model" if inner else "the game ships it twice",
                            WARN))
            elif inner and container == self.relative(texture.container):
                out.append((other, "the same model carries, and uploads, two copies", WARN))
            else:
                out.append((other, "each .glb carries its own copy" if inner else "that loose file is the copy to remove", INFO))
        return out

    def palette(self):
        """The game's palette as (n, 3), when the manifest names one ("palette": a file relative to the manifest)."""
        if self._palette is None:
            name = self.manifest.get("palette")
            self._palette = tools.load_palette(os.path.join(os.path.dirname(os.path.abspath(self.manifest_path)), name)) if name else False
        return self._palette if self._palette is not False else None

    def unlisted(self):
        """Textures the game ships that the manifest does not list: loose images and LUTs that
        are not sprite sheets, and every image embedded in a model."""
        listed, missing = set(self.entries), []
        sprites = self.sprite_images()
        for name in self.files():
            lower = name.lower()
            if lower.endswith(ti.MODEL_EXTENSIONS):
                try:
                    inner = ti.embedded(os.path.join(self.root, name))
                except (OSError, ValueError):
                    continue  # modelkit's to report
                missing += [f"{name}#{image}" for image, _ in inner if f"{name}#{image}" not in listed]
            elif (ti.is_image_name(lower) or lower.endswith(ti.LUT_EXTENSION)) and name not in listed \
                    and os.path.normpath(os.path.join(self.root, name)) not in sprites:
                missing.append(name)
        return missing


class TextureKit(common.Kit):
    def __init__(self, here):
        super().__init__(name="texturekit", here=here, section="textures", groups=checks.GROUPS,
                         is_asset=lambda filename: False,  # discovery is unlisted() below: it has to look inside models
                         check=self._check, sheet=self._sheet, questions=QUESTIONS, files=self._files, noun="texture")
        self.game = None
        self._engine = {}
        self._reviewed, self._written = [], []

    # --- what kitcommon calls

    def _files(self, path):
        """The file a texture's review is bound to: itself, or the model that embeds it."""
        return [ti.split(path)[0]]

    def _entries(self, manifest_path):
        manifest, root, entries = super()._entries(manifest_path)
        if self.game is None or self.game.manifest_path != manifest_path:
            self.game = Game(manifest_path, manifest, root, entries)
        else:
            self.game.manifest, self.game.entries = manifest, entries
        return manifest, root, entries

    def _check(self, path, profile, engine):
        texture = ti.load(path, profile, models=self.game.models() if self.game else ())
        findings = checks.run(texture, profile, self.game)
        if engine:
            findings += self.engine_load(texture, engine)
        return findings, checks.stats_line(texture, profile), texture

    def _sheet(self, path, profile, findings, texture, lines, engine):
        swatches = (self.game.manifest.get("swatches") if self.game else None) or {}
        title = self.game.relative(path) if self.game and os.path.abspath(path).startswith(self.game.root) else path
        image = sheet.draw(texture, profile, findings, lines, swatches, title)
        self._reviewed.append((path, profile, findings, texture))
        return image

    # --- the engine

    def engine_load(self, texture, engine):
        """Decode it with the engine itself: `fe.app.image_size` for a file, `fe.app.model_animations`
        (which loads the model and decodes its images) for an embedded one. No binding loads a
        .cube without a renderer, so LUTs are checked by this kit's own reading of the engine's parser."""
        if texture.is_lut:
            return []
        container = os.path.abspath(texture.container)
        extension = os.path.splitext(container)[1].lower()
        if texture.embedded:
            if container not in self._engine:
                files = {"m/model" + extension: container}
                if extension == ".gltf":  # the buffers and images it points at
                    document = ti.model(container).json
                    for item in document.get("buffers", []) + document.get("images", []):
                        uri = item.get("uri")
                        if uri and not uri.startswith("data:") and os.path.isfile(os.path.join(os.path.dirname(container), uri)):
                            files["m/" + uri] = os.path.join(os.path.dirname(container), uri)
                self._engine[container] = common.engine_test(engine, files, f'fe.app.model_animations("m/model{extension}")')
            problems = self._engine[container]
        else:
            inside = "t/texture" + (extension or ".png")
            problems = common.engine_test(engine, {inside: container},
                                          f'local w, h = fe.app.image_size("{inside}")\n'
                                          f't.eq(w, {texture.width}, "width")\nt.eq(h, {texture.height}, "height")')
        return [Finding("ready.engine_load", ERROR, f"the engine does not load it as expected: {problem}", None, None,
                        "Write it again as a plain 8-bit PNG (tools.save_png)." if not texture.embedded else
                        "Fix the model's image (see the message), then export again.") for problem in problems]

    # --- the command lines

    def _expanded(self, argv):
        """argv with every name that stands for several textures replaced by them: with a
        manifest, a folder, a model or a glob of its keys; without one, a model file becomes
        its embedded images."""
        argv = list(sys.argv[1:] if argv is None else argv)
        manifest, names, rest, index = None, [], [], 0
        while index < len(argv):
            token = argv[index]
            if token in VALUE_OPTIONS:
                if token == "--manifest" and index + 1 < len(argv):
                    manifest = argv[index + 1]
                rest += argv[index:index + 2]
                index += 2
                continue
            if token.startswith("--"):
                if token.startswith("--manifest="):
                    manifest = token.split("=", 1)[1]
                rest.append(token)
            else:
                names.append(token)
            index += 1
        out = []
        if manifest:
            _, root, entries = self._entries(manifest)
            for name in names:
                if self._key(root, entries, name):
                    out.append(name)
                    continue
                inside = os.path.relpath(os.path.abspath(name), root).replace(os.sep, "/")
                group = []
                for candidate in (name.replace(os.sep, "/").rstrip("/"), inside.rstrip("/")):
                    group = [key for key in entries
                             if key.startswith(candidate + "/") or key.startswith(candidate + "#") or fnmatch.fnmatch(key, candidate)]
                    if group:
                        break
                out += group or [name]
        else:
            for name in names:
                if "#" not in name and name.lower().endswith(ti.MODEL_EXTENSIONS) and os.path.isfile(name):
                    try:
                        out += [f"{name}#{image}" for image, _ in ti.embedded(name)] or [name]
                        continue
                    except (OSError, ValueError):
                        pass
                out.append(name)
        seen = set()
        return [name for name in out if not (name in seen or seen.add(name))] + rest, manifest, bool(names)

    def validate_main(self, argv=None, description=None):
        argv, manifest, named = self._expanded(argv)
        missing = []
        if manifest and not named:
            missing = self.game.unlisted()
            for key in missing:
                print(f"\n{key}\n  => NOT READY: not listed in {os.path.basename(manifest)} (add it with a profile)")
        code = super().validate_main(argv, description)
        if missing:
            print(f"{len(missing)} texture(s) the game ships are not in the manifest; see the top of this report")
        return 1 if missing else code

    def _sheet_path(self, args, manifest, key, path, several):
        """kitcommon names a sheet after the key up to its last dot, which for `model.glb#image`
        would give every texture of a model the same file: name it model-image instead."""
        name = key or path
        container, inner = ti.split(name)
        if inner is not None:
            name = os.path.splitext(container)[0] + "-" + inner + ".png"
        out = super()._sheet_path(args, manifest, name if key else None, name, several)
        self._written.append(out)
        return out

    def review_main(self, argv=None, description=None):
        argv, manifest, _ = self._expanded(argv)
        self._reviewed, self._written = [], []
        written = self._written
        code = super().review_main(argv, description)
        if len(self._reviewed) > 1 and written:
            swatches = (self.game.manifest.get("swatches") if self.game else None) or {}
            keys = [self.game.relative(path) if self.game else os.path.basename(path) for path, _, _, _ in self._reviewed]
            common_part = os.path.commonprefix(keys)
            cut = max(common_part.rfind(mark) for mark in "/#_")      # back to a whole word
            name = re.sub(r"[^A-Za-z0-9]+", "_", common_part[:cut + 1]).strip("_") or "textures"
            out = os.path.join(os.path.dirname(written[0]), f"group_{name}.png")
            sheet.draw_group(self._reviewed, keys, swatches).save(out)
            print(f"wrote {out}  (the group together, at one scale: look at this one first)")
        return code
