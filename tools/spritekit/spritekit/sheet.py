"""Reads a sprite sheet the way the engine does (engine/assets/SpriteAnimation.cpp
and GpuAssets::spriteSheet): the .anim.json, its atlas and the optional normal
and emissive images laid out like it.

What the engine refuses is recorded as an error in `Sheet.problems` with the
engine's reason, and the rest of the file is still read as far as it goes so
the other checks can say everything that is wrong in one run. What the engine
accepts but ignores (a misspelt key) is recorded as a warning.
"""
import json
import math
import os

import numpy as np

ROOT_KEYS = ("atlas", "normal", "emissive", "pixels_per_unit", "pivot", "clips")
CLIP_KEYS = ("loop", "directions")
FRAME_KEYS = ("rect", "duration", "event")
DIRECTION_NAMES = {1: ("",), 4: ("south", "west", "north", "east"),
                   8: ("south", "south-west", "west", "north-west", "north", "north-east", "east", "south-east")}
DEFAULT_DURATION, DEFAULT_PPU, DEFAULT_PIVOT = 0.15, 32, (0.5, 1.0)   # SpriteAnimation.h
MAX_IMAGE = 16384                                                     # Image.cpp


class Frame:
    """One frame of one direction of a clip: its rectangle in the atlas, how long it shows, its event."""

    def __init__(self, clip, direction, index, rect, duration, event):
        self.clip, self.direction, self.index = clip, direction, index
        self.rect, self.duration, self.event = rect, duration, event
        self.inside = True   # whether the rectangle lies inside the atlas

    @property
    def where(self):
        return f"{self.clip.name} {self.clip.direction_name(self.direction)} frame {self.index}".replace("  ", " ")


class Clip:
    def __init__(self, name, loop, loop_given):
        self.name, self.loop, self.loop_given = name, loop, loop_given
        self.directions = []   # a list of frames per direction row

    def direction_name(self, row):
        names = DIRECTION_NAMES.get(len(self.directions))
        return names[row] if names else f"row {row}"

    def length(self, row=0):
        return sum(frame.duration for frame in self.directions[row])

    def starts(self, row=0):
        """When each frame of a row begins, in seconds."""
        out, time = [], 0.0
        for frame in self.directions[row]:
            out.append(time)
            time += frame.duration
        return out


class Sheet:
    """A loaded sprite sheet. `atlas`, `normal` and `emissive` are (height,
    width, 4) uint8 arrays, or None when the image is absent or unreadable."""

    def __init__(self, path):
        self.path = path
        self.folder = os.path.dirname(os.path.abspath(path))
        self.problems = []          # (id, "error" or "warn", message, where, fix)
        self.json = None
        self.atlas_name = self.normal_name = self.emissive_name = ""
        self.atlas = self.normal = self.emissive = None
        self.pixels_per_unit, self.ppu_given = DEFAULT_PPU, False
        self.pivot, self.pivot_given = DEFAULT_PIVOT, False
        self.clips = {}
        self._cache = {}

    # --- what the checks read

    @property
    def loadable(self):
        """Whether the engine would load the .anim.json (it does not look at the images then)."""
        return not any(severity == "error" and check == "sheet.json" for check, severity, *_ in self.problems)

    def frames(self):
        for clip in self.clips.values():
            for row in clip.directions:
                yield from row

    def rects(self):
        """The distinct frame rectangles that lie inside the atlas."""
        seen = {}
        for frame in self.frames():
            if frame.inside:
                seen.setdefault(frame.rect, frame)
        return seen

    def pixels(self, frame, image=None):
        """The frame's pixels (a view of the atlas, or of `image`), or None when it lies outside."""
        image = self.atlas if image is None else image
        if image is None or not frame.inside:
            return None
        x, y, w, h = frame.rect
        return image[y:y + h, x:x + w]

    def files(self):
        return [self.path] + [os.path.join(self.folder, name) for name in (self.atlas_name, self.normal_name, self.emissive_name) if name]

    def problem(self, check, severity, message, where=None, fix=None):
        self.problems.append((check, severity, message, where, fix))


def named_files(path):
    """The .anim.json and the images it names: what a sprite is made of. A
    file that cannot be read is just itself."""
    try:
        with open(path, encoding="utf-8") as file:
            data = json.load(file)
    except (OSError, ValueError):
        return [path]
    folder = os.path.dirname(os.path.abspath(path))
    names = [data.get(key) for key in ("atlas", "normal", "emissive")] if isinstance(data, dict) else []
    return [path] + [os.path.join(folder, name) for name in names if isinstance(name, str) and name]


def _is_int(value):
    return isinstance(value, int) and not isinstance(value, bool)


def _is_number(value):
    return isinstance(value, (int, float)) and not isinstance(value, bool)


def _reject_constant(name):
    raise ValueError(f"'{name}' is not JSON (the engine's parser refuses it)")


def load_image(path):
    """An image as the engine decodes it: 8-bit RGBA. Returns (array, None) or (None, why not)."""
    from PIL import Image
    if not os.path.isfile(path):
        return None, "the file does not exist"
    try:
        with Image.open(path) as image:
            if image.format not in ("PNG", "JPEG", "BMP", "TGA", "GIF"):
                return None, f"{image.format} is not a format the engine decodes (use PNG)"
            if image.width > MAX_IMAGE or image.height > MAX_IMAGE:
                return None, f"{image.width}x{image.height} is beyond the engine's {MAX_IMAGE} pixel limit"
            return np.asarray(image.convert("RGBA")), None
    except Exception as error:  # noqa: BLE001  (pillow raises many kinds; all mean "not an image")
        return None, f"it does not decode ({error})"


def load(path):
    """Read a sheet. Raises OSError when the .anim.json cannot be opened;
    everything else is reported in `Sheet.problems`."""
    sheet = Sheet(path)
    with open(path, encoding="utf-8") as file:
        text = file.read()
    try:
        data = json.loads(text, parse_constant=_reject_constant)
    except ValueError as error:
        sheet.problem("sheet.json", "error", f"not valid JSON: {error}", None, "Write it with json.dump (spritekit.tools.write_anim_json).")
        return sheet
    sheet.json = data
    if not isinstance(data, dict):
        sheet.problem("sheet.json", "error", "the file is not a JSON object", None, "An .anim.json is {atlas, pixels_per_unit, pivot, clips}.")
        return sheet
    refuse = lambda message, where=None, fix=None: sheet.problem(  # noqa: E731
        "sheet.json", "error", message, where, fix or "The engine refuses the whole sheet for this: write what SpriteAnimation::fromJson reads.")

    for key in data:
        if key not in ROOT_KEYS:
            sheet.problem("sheet.unknown_key", "warn", f"'{key}' is not a key the engine reads (it knows {', '.join(ROOT_KEYS)})", None,
                          "Fix the spelling or remove it: the engine ignores it and uses its default.")
    atlas = data.get("atlas")
    if not isinstance(atlas, str) or not atlas:
        refuse("'atlas' must name the atlas image" if "atlas" in data else "there is no 'atlas'")
    else:
        sheet.atlas_name = atlas
    for key in ("normal", "emissive"):
        if key in data:
            if isinstance(data[key], str):
                setattr(sheet, key + "_name", data[key])
            else:
                refuse(f"'{key}' must be a file name")
    if "pixels_per_unit" in data:
        ppu = data["pixels_per_unit"]
        sheet.ppu_given = True
        if not _is_number(ppu) or not math.isfinite(ppu) or int(ppu) < 1 or int(ppu) > 4096:
            refuse(f"'pixels_per_unit' is {ppu!r}; the engine takes a whole number from 1 to 4096")
        else:
            sheet.pixels_per_unit = int(ppu)
            if int(ppu) != ppu:
                sheet.problem("sheet.json_types", "warn", f"'pixels_per_unit' is {ppu}; the engine cuts it to {int(ppu)}", None,
                              "Use a whole number.")
    if "pivot" in data:
        pivot = data["pivot"]
        sheet.pivot_given = True
        if not isinstance(pivot, list) or len(pivot) < 2 or not all(_is_number(v) for v in pivot[:2]):
            refuse(f"'pivot' is {pivot!r}; the engine takes [x, y] as fractions of the frame")
        else:
            sheet.pivot = (float(pivot[0]), float(pivot[1]))
            if len(pivot) > 2:
                sheet.problem("sheet.json_types", "warn", "'pivot' has more than two numbers; the engine reads the first two", None,
                              "Write [x, y].")
    clips = data.get("clips")
    if not isinstance(clips, dict):
        refuse("'clips' must be an object of clip name to clip" if "clips" in data else "there is no 'clips'")
        clips = {}
    elif not clips:
        refuse("a sprite needs at least one clip")
    for name, body in clips.items():
        _read_clip(sheet, name, body, refuse)

    # The images. The engine loads the .anim.json without them; the sprite is then invisible or unlit.
    if sheet.atlas_name:
        sheet.atlas = _read_image(sheet, "atlas", sheet.atlas_name)
    for key in ("normal", "emissive"):
        name = getattr(sheet, key + "_name")
        if name:
            setattr(sheet, key, _read_image(sheet, key, name))
    if sheet.atlas is not None:
        height, width = sheet.atlas.shape[:2]
        for frame in sheet.frames():
            x, y, w, h = frame.rect
            frame.inside = x >= 0 and y >= 0 and w >= 1 and h >= 1 and x + w <= width and y + h <= height
    return sheet


def _read_image(sheet, key, name):
    if "\\" in name or os.path.isabs(name) or ".." in name.replace("\\", "/").split("/"):
        sheet.problem("sheet.missing_image", "error", f"'{key}' is '{name}': an image is named relative to the .anim.json, inside the game", None,
                      "Keep the image next to the .anim.json and name it by its file name.")
        return None
    image, why = load_image(os.path.join(sheet.folder, name))
    if image is None:
        effect = "the sprite is not drawn at all" if key == "atlas" else f"the sprite is drawn without its {key} image, silently"
        sheet.problem("sheet.missing_image", "error", f"the {key} image '{name}' cannot be used: {why}; {effect}", None,
                      "Write the image next to the .anim.json, as an 8-bit PNG, and name it exactly (case matters on Linux).")
    return image


def _read_clip(sheet, name, body, refuse):
    if not isinstance(body, dict):
        refuse("a clip must be an object {loop, directions}", name)
        return
    for key in body:
        if key not in CLIP_KEYS:
            sheet.problem("sheet.unknown_key", "warn", f"'{key}' is not a key of a clip (the engine reads {', '.join(CLIP_KEYS)})", name,
                          "Fix the spelling or remove it: the engine ignores it and uses its default.")
    loop = body.get("loop", True)
    if not isinstance(loop, bool):
        refuse(f"'loop' is {loop!r}; it must be true or false", name)
        loop = True
    clip = Clip(name, loop, "loop" in body)
    directions = body.get("directions")
    if not isinstance(directions, list):
        refuse("'directions' must be a list of direction rows" if "directions" in body else "the clip has no 'directions'", name)
        return
    for row, frames in enumerate(directions):
        out = []
        if not isinstance(frames, list):
            refuse("a direction must be a list of frames", f"{name} row {row}")
            frames = []
        for index, frame in enumerate(frames):
            where = f"{name} row {row} frame {index}"
            if not isinstance(frame, dict):
                refuse("a frame must be an object {rect, duration, event}", where)
                continue
            for key in frame:
                if key not in FRAME_KEYS:
                    sheet.problem("sheet.unknown_key", "warn", f"'{key}' is not a key of a frame (the engine reads {', '.join(FRAME_KEYS)})",
                                  where, "Fix the spelling or remove it: the engine ignores it and uses its default.")
            rect = frame.get("rect")
            if not isinstance(rect, list) or len(rect) != 4 or not all(_is_number(v) for v in rect):
                refuse("'rect' must be [x, y, width, height]", where)
                continue
            if not all(_is_int(v) or float(v).is_integer() for v in rect):
                sheet.problem("sheet.json_types", "warn", f"'rect' {rect} is not whole pixels; the engine cuts the fractions off", where,
                              "Write whole pixel coordinates.")
            rect = tuple(int(v) for v in rect)
            duration = frame.get("duration", DEFAULT_DURATION)
            event = frame.get("event", "")
            if not _is_number(duration) or not math.isfinite(duration) or duration <= 0:
                refuse(f"'duration' is {duration!r}; it must be a number of seconds above 0", where)
                continue
            if not isinstance(event, str):
                refuse(f"'event' is {event!r}; it must be a string", where)
                continue
            if rect[0] < 0 or rect[1] < 0 or rect[2] < 1 or rect[3] < 1:
                refuse(f"'rect' {list(rect)} is negative or has no size", where)
                continue
            out.append(Frame(clip, row, index, rect, float(duration), event))
        if not out:
            refuse("a direction has no frames", f"{name} row {row}")
        clip.directions.append(out)
    if len(clip.directions) not in (1, 4, 8):
        refuse(f"the clip has {len(clip.directions)} directions; the engine takes 1, 4 or 8", name)
    if clip.directions and all(clip.directions):
        sheet.clips[name] = clip
