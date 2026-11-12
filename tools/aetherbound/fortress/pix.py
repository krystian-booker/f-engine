"""Small pixel-art toolkit shared by the fortress generators: palettes, tileable
noise, index-painted canvases with a height channel (for normal maps) and an
emissive channel."""
import numpy as np
from PIL import Image


def hexrgb(h):
    h = h.lstrip("#")
    return np.array([int(h[i:i + 2], 16) for i in (0, 2, 4)], dtype=np.float32)


def ramp(*colors):
    return np.stack([hexrgb(c) for c in colors])


# Albedo ramps, dark to light. Stone is a warm grey so that blue ambient turns
# it indigo and firelight turns it umber.
STONE = ramp("#262330", "#36323f", "#47424e", "#5a545c", "#6f686a", "#857d79", "#9c9388", "#b3a998", "#c9c0ad")
WARM = ramp("#2c2328", "#3f3033", "#554042", "#6c5350", "#84685f", "#9c7f70", "#b39883", "#c7af97", "#d8c5ac")
COLD = ramp("#20222e", "#2e3140", "#3e4253", "#505566", "#646a79", "#7a808c", "#91979f", "#a8adb1", "#bfc3c2")
MOSS = ramp("#1f2a24", "#2a3a2e", "#384d37", "#4a6240", "#5f7848")
WOOD = ramp("#1e1512", "#2e1f18", "#412b1f", "#573a27", "#6e4b31", "#86603f", "#9f7850", "#b89263")
CHAR = ramp("#0b0a0c", "#131114", "#1c181b", "#272022", "#33292a", "#403433")
IRON = ramp("#15161b", "#202229", "#2d3038", "#3c4049", "#4f545c", "#666b72", "#82868a", "#a2a5a5")
BRONZE = ramp("#1c1510", "#2b2118", "#3d2f22", "#4a3b2c", "#5e4b36", "#766044", "#8f7753")
VERDI = ramp("#1c2f2b", "#2a4a42", "#3f6b5e", "#5a8a78", "#7ba792")
GOLD = ramp("#5a4420", "#8a6a2c", "#c9a24a", "#e8c874", "#fff0b0")
RED = ramp("#2a0d0f", "#421316", "#5e1a1c", "#7c2423", "#9a322b", "#b5473a", "#cc6450")
BLACKC = ramp("#0c0b10", "#15141b", "#1f1e27", "#2b2a35", "#3a3946")
FIRE = ramp("#3a0a04", "#7a1a06", "#c43a0a", "#ff7a1e", "#ffa838", "#ffd27a", "#fff3c8")
VIOLET = ramp("#1e0c3a", "#3a1a66", "#6336b8", "#9b5cff", "#c39cff", "#e2ccff", "#ffffff")
CREAM = ramp("#3c352f", "#5a4f44", "#7a6c5c", "#9a8b76", "#b8aa92")


class Rng(np.random.Generator):
    pass


def rng(seed):
    return np.random.default_rng(seed)


def value_noise(w, h, cell, seed, octaves=1, tile=True):
    """Tileable value noise in 0..1, (h, w)."""
    r = rng(seed)
    out = np.zeros((h, w), dtype=np.float32)
    amp, total = 1.0, 0.0
    for o in range(octaves):
        c = max(1, int(cell / (2 ** o)))
        gw, gh = max(1, w // c), max(1, h // c)
        g = r.random((gh, gw)).astype(np.float32)
        ys = (np.arange(h) / c) % gh
        xs = (np.arange(w) / c) % gw
        y0 = np.floor(ys).astype(int)
        x0 = np.floor(xs).astype(int)
        fy = ys - y0
        fx = xs - x0
        fy = fy * fy * (3 - 2 * fy)
        fx = fx * fx * (3 - 2 * fx)
        y1 = (y0 + 1) % gh
        x1 = (x0 + 1) % gw
        a = g[np.ix_(y0, x0)]
        b = g[np.ix_(y0, x1)]
        cc = g[np.ix_(y1, x0)]
        d = g[np.ix_(y1, x1)]
        v = (a * (1 - fx)[None, :] + b * fx[None, :]) * (1 - fy)[:, None] + \
            (cc * (1 - fx)[None, :] + d * fx[None, :]) * fy[:, None]
        out += v * amp
        total += amp
        amp *= 0.5
    return out / total


class Canvas:
    """RGB + alpha + height + emissive, painted by ramp lookups."""

    def __init__(self, w, h, base=(0, 0, 0), alpha=255):
        self.w, self.h = w, h
        self.rgb = np.zeros((h, w, 3), dtype=np.float32) + np.array(base, dtype=np.float32)
        self.a = np.full((h, w), alpha, dtype=np.float32)
        self.height = np.zeros((h, w), dtype=np.float32)
        self.emit = np.zeros((h, w, 3), dtype=np.float32)

    # wrap-around pixel access helpers
    def put(self, x, y, color, height=None, wrap=True):
        if wrap:
            x %= self.w
            y %= self.h
        elif not (0 <= x < self.w and 0 <= y < self.h):
            return
        self.rgb[y, x] = color
        if height is not None:
            self.height[y, x] = height

    def rect(self, x0, y0, w, h, color, height=None):
        ys = np.arange(y0, y0 + h) % self.h
        xs = np.arange(x0, x0 + w) % self.w
        self.rgb[np.ix_(ys, xs)] = color
        if height is not None:
            self.height[np.ix_(ys, xs)] = height

    def normal_map(self, strength=2.0, wrap=True):
        """Tangent-space normal map in glTF's convention, which is the engine's:
        red leans right, green leans up the image."""
        hgt = self.height
        if wrap:
            dx = (np.roll(hgt, -1, 1) - np.roll(hgt, 1, 1)) * 0.5
            dy = (np.roll(hgt, -1, 0) - np.roll(hgt, 1, 0)) * 0.5
        else:
            p = np.pad(hgt, 1, mode="edge")
            dx = (p[1:-1, 2:] - p[1:-1, :-2]) * 0.5
            dy = (p[2:, 1:-1] - p[:-2, 1:-1]) * 0.5
        nx, ny, nz = -dx * strength, dy * strength, np.ones_like(hgt)  # dy is taken down the image, green points up it
        ln = np.sqrt(nx * nx + ny * ny + nz * nz)
        n = np.stack([nx / ln, ny / ln, nz / ln], -1)
        return np.clip(n * 127.5 + 127.5, 0, 255).astype(np.uint8)

    def image(self):
        out = np.concatenate([np.clip(self.rgb, 0, 255), np.clip(self.a, 0, 255)[..., None]], -1)
        return Image.fromarray(out.astype(np.uint8), "RGBA")

    def emissive_image(self):
        return Image.fromarray(np.clip(self.emit, 0, 255).astype(np.uint8), "RGB")

    def normal_image(self, strength=2.0, wrap=True):
        return Image.fromarray(self.normal_map(strength, wrap), "RGB")


def save(img, path, colors=None):
    """Writes a PNG; `colors` quantises to a palette (smaller files, and it is
    pixel art anyway)."""
    path = str(path)
    if colors and img.mode in ("RGB", "RGBA"):
        if img.mode == "RGBA":
            alpha = img.getchannel("A")
            if alpha.getextrema() == (255, 255):
                img = img.convert("RGB").quantize(colors, method=Image.Quantize.MEDIANCUT, dither=Image.Dither.NONE)
            else:
                img = img.quantize(colors, method=Image.Quantize.FASTOCTREE, dither=Image.Dither.NONE)
        else:
            img = img.quantize(colors, method=Image.Quantize.MEDIANCUT, dither=Image.Dither.NONE)
    img.save(path, optimize=True)
