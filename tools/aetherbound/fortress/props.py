"""Prop builders for the fortress: braziers, banners, crates, barrels, carts,
barricades, siege wreckage, guard bodies, weapons, statues, tents, pillars,
arches. Each draws into a Kit (meshkit.py) in world space."""
import math
import numpy as np
from meshkit import v3, euler, rot_y, frame_from
from textures import STRIPS, ATLAS

DARK = (0.55, 0.55, 0.6)


def hashf(*a):
    x = math.sin(sum((i + 1) * 12.9898 * float(v) for i, v in enumerate(a)) + 4.123) * 43758.5453
    return x - math.floor(x)


# ------------------------------------------------------------------ fire holders
def brazier(k, x, y, z, s=1.0, legs=True):
    """Iron bowl of coals on three legs. Returns the flame position."""
    top = y + 1.0 * s
    if legs:
        for i in range(3):
            a = i * 2.094 + 0.5
            k.beam("iron", (x + math.cos(a) * 0.32 * s, y, z + math.sin(a) * 0.32 * s),
                   (x + math.cos(a) * 0.14 * s, top - 0.28 * s, z + math.sin(a) * 0.14 * s), 0.07 * s, 0.07 * s)
    k.prism("props", x, z, 0.16 * s, 0.42 * s, top - 0.3 * s, top, n=8, cap=False, strip="iron", bottom=True)
    # rim
    k.prism("props", x, z, 0.46 * s, 0.46 * s, top, top + 0.06 * s, n=8, cap=False, strip="iron")
    # coals
    y0, h = STRIPS["coals"]
    ring = [v3(x + math.sin(i * math.pi / 4) * 0.42 * s, top + 0.02 * s, z + math.cos(i * math.pi / 4) * 0.42 * s)
            for i in range(8)]
    u0 = k.rng.random() * 8
    uv = [((u0 + (p[0] - x)) * 32 / ATLAS, (y0 + h / 2 + (p[2] - z) * 30) / ATLAS) for p in ring]
    k.poly("props", ring, uv=uv, out=v3(0, 1, 0))
    return (x, top + 0.15 * s, z)


def sconce(k, x, y, z, nx, nz):
    """Wall torch bracket; (nx, nz) is the wall's outward normal."""
    k.beam("iron", (x, y, z), (x + nx * 0.35, y + 0.15, z + nz * 0.35), 0.06, 0.06)
    k.beam("wood", (x + nx * 0.35, y + 0.05, z + nz * 0.35), (x + nx * 0.42, y + 0.6, z + nz * 0.42), 0.09, 0.09)
    return (x + nx * 0.45, y + 0.75, z + nz * 0.45)


# ------------------------------------------------------------------ cloth
def banner(k, x, ytop, z, nx, nz, width=1.6, length=4.0, strip="cloth", tatter=0.5, seed=0, bar=True, col=(1, 1, 1)):
    """Hanging banner on a wall whose outward normal is (nx, nz); U runs down
    the cloth so the emblem sits upright."""
    r = np.random.default_rng(seed)
    tx, tz = -nz, nx  # along the wall
    cols = 4
    rows = max(2, int(length / 0.5))
    y0, sh = STRIPS[strip]
    off = 0.12

    def pt(i, j, cut=0.0):
        s = (i / cols - 0.5) * width
        d = j / rows * length - cut
        wave = math.sin(j * 0.9 + i * 0.7 + seed) * 0.06 * (j / rows) + 0.03 * math.sin(i * 2.1)
        return v3(x + tx * s + nx * (off + wave), ytop - d, z + tz * s + nz * (off + wave))

    for i in range(cols):
        # ragged hem: each column ends at its own length
        cut_a = r.uniform(0, tatter) * length * 0.35
        cut_b = r.uniform(0, tatter) * length * 0.35
        for j in range(rows):
            last = j == rows - 1
            P = [pt(i, j), pt(i + 1, j), pt(i + 1, j + 1, cut_b if last else 0), pt(i, j + 1, cut_a if last else 0)]
            # U down the banner (32 px per unit), V across its width
            uv = []
            for q, (ii, jj) in zip(P, ((i, j), (i + 1, j), (i + 1, j + 1), (i, j + 1))):
                d = ytop - q[1]
                uv.append(((d + 0.75) * 32.0 / ATLAS, (y0 + 1 + (ii / cols) * (sh - 2)) / ATLAS))
            shade = 0.82 + 0.18 * math.sin(i * 1.7 + seed)
            c = np.asarray(col) * shade
            k.poly("props", P, uv=uv, col=c, out=v3(nx, 0, nz))
            k.poly("props", P, uv=uv, col=c * 0.7, out=v3(-nx, 0, -nz))
    if bar:
        k.beam("iron", (x - tx * (width / 2 + 0.15) + nx * off, ytop + 0.04, z - tz * (width / 2 + 0.15) + nz * off),
               (x + tx * (width / 2 + 0.15) + nx * off, ytop + 0.04, z + tz * (width / 2 + 0.15) + nz * off), 0.08,
               0.08)


def flag_pole(k, x, y, z, h=5.0, strip="cloth", seed=0, yaw=0.0):
    k.beam("wood", (x, y, z), (x, y + h, z), 0.1, 0.1)
    nx, nz = math.sin(yaw), math.cos(yaw)
    banner(k, x + nz * 0.55, y + h - 0.1, z - nx * 0.55, nx, nz, width=1.0, length=2.2, strip=strip, tatter=0.9,
           seed=seed, bar=True)


# ------------------------------------------------------------------ containers
def crate(k, x, y, z, s=0.6, yaw=0.0, tilt=0.0, char=False):
    with k.at((x, y + s / 2, z), yaw, tilt, tilt * 0.6):
        k.cbox("props", (0, 0, 0), (s / 2, s / 2, s / 2), c=0.02, bottom=True, chip=0, strip="char" if char else "wood")
        for sx in (-1, 1):
            k.cbox("props", (sx * (s / 2 - 0.05), 0, 0), (0.035, s / 2 + 0.012, s / 2 + 0.012), c=0.0, bottom=True,
                   chip=0, strip="iron", col=(0.8, 0.8, 0.85))


def barrel(k, x, y, z, s=1.0, yaw=0.0, lying=False):
    h, r = 0.85 * s, 0.3 * s
    if lying:
        with k.at((x, y + r, z), yaw, 0, math.pi / 2):
            k.prism("props", 0, 0, r * 0.88, r, -h / 2, 0, n=8, cap=False, strip="wood", bottom=True)
            k.prism("props", 0, 0, r, r * 0.88, 0, h / 2, n=8, cap=True, strip="wood")
            for yy in (-h * 0.3, h * 0.3):
                k.prism("props", 0, 0, r * 1.03, r * 1.03, yy - 0.03, yy + 0.03, n=8, cap=False, strip="iron")
    else:
        k.prism("props", x, z, r * 0.88, r, y, y + h / 2, n=8, cap=False, strip="wood")
        k.prism("props", x, z, r, r * 0.88, y + h / 2, y + h, n=8, cap=True, strip="wood")
        for yy in (y + h * 0.2, y + h * 0.8):
            k.prism("props", x, z, r * 1.0, r * 1.0, yy - 0.03, yy + 0.03, n=8, cap=False, strip="iron")


def sack_pile(k, x, y, z, n=4, seed=0):
    r = np.random.default_rng(seed)
    for i in range(n):
        a = r.uniform(0, 6.28)
        d = r.uniform(0, 0.5)
        lvl = 0 if i < n - 1 else 1
        k.cbox("props", (x + math.cos(a) * d, y + 0.16 + lvl * 0.28, z + math.sin(a) * d), (0.38, 0.16, 0.24), c=0.09,
               rot=rot_y(r.uniform(0, 3)), strip="rope", col=(0.75, 0.68, 0.55), chip=0)


# ------------------------------------------------------------------ timber
def wheel(k, c, r=0.55, axis=(1, 0, 0), strip="wood", broken=False):
    c = v3(c)
    R = frame_from(axis)
    n = 8
    pts = [c + R @ v3(0, math.cos(i * 2 * math.pi / n) * r, math.sin(i * 2 * math.pi / n) * r) for i in range(n)]
    for i in range(n):
        if broken and i in (1, 2):
            continue
        k.beam(strip, pts[i], pts[(i + 1) % n], 0.1, 0.12)
    for i in range(n // 2):
        if broken and i == 1:
            continue
        k.beam(strip, pts[i], pts[i + n // 2], 0.07, 0.07)
    k.cbox("props", c, (0.1, 0.1, 0.1), c=0.02, rot=R, bottom=True, chip=0, strip="iron")


def cart(k, x, y, z, yaw=0.0, roll=0.0, char=False, load=True):
    """Two-wheeled supply cart; roll tips it over."""
    strip = "char" if char else "wood"
    with k.at((x, y, z), yaw, 0, roll):
        k.cbox("props", (0, 0.62, 0), (1.0, 0.06, 0.55), c=0.0, bottom=True, chip=0, strip=strip)
        for sz in (-1, 1):
            k.cbox("props", (0, 0.86, sz * 0.55), (1.0, 0.2, 0.04), c=0.0, bottom=True, chip=0, strip=strip)
            wheel(k, (0.1, 0.55, sz * 0.68), 0.55, axis=(0, 0, 1), strip=strip, broken=(sz > 0 and roll != 0))
        k.cbox("props", (-1.0, 0.86, 0), (0.04, 0.2, 0.55), c=0.0, bottom=True, chip=0, strip=strip)
        for sz in (-0.4, 0.4):
            k.beam(strip, (1.0, 0.6, sz), (2.3, 0.35, sz), 0.08, 0.08)
        if load:
            crate(k, -0.4, 0.68, 0.0, 0.5, 0.3)
            barrel(k, 0.45, 0.68, -0.1, 0.8)


def stake(k, x, y, z, h=1.8, lean=(0, 0), r=0.13, strip="wood", point=True):
    top = v3(x + lean[0], y + h, z + lean[1])
    base = v3(x, y - 0.1, z)
    d = top - base
    L = np.linalg.norm(d)
    with k.at(base, 0, 0, 0):
        pass
    R = frame_from(d)
    # local +x along the stake; build as a square-ish log from two crossed beams for an octagonal look
    k.beam(strip, base, top, r * 1.8, r * 1.8, roll=0.0)
    k.beam(strip, base, base + d * 0.98, r * 1.8, r * 1.8, roll=math.pi / 4, caps=False)
    if point:
        tip = top + d / L * (r * 3)
        ring = [top + R @ v3(0, math.cos(a) * r * 1.2, math.sin(a) * r * 1.2) for a in np.arange(4) * math.pi / 2]
        y0, sh = STRIPS[strip]
        for i in range(4):
            P = [ring[i], ring[(i + 1) % 4], tip]
            uv = [(0.02 * i, (y0 + 2) / ATLAS), (0.02 * i + 0.02, (y0 + 2) / ATLAS), (0.02 * i + 0.01, (y0 + 12) / ATLAS)]
            k.poly("props", P, uv=uv, out=(P[0] + P[1]) / 2 - top + d / L * 0.05, col=(1.15, 1.1, 1.0))


def palisade(k, x0, z0, x1, z1, h=1.9, y=0.0, seed=0, gap=0.34, burnt=0.3):
    """A row of sharpened stakes lashed to a rail."""
    r = np.random.default_rng(seed)
    L = math.hypot(x1 - x0, z1 - z0)
    n = max(2, int(L / gap))
    px, pz = -(z1 - z0) / L, (x1 - x0) / L
    for i in range(n + 1):
        t = i / n
        if r.random() < 0.12:
            continue
        hh = h * r.uniform(0.75, 1.08)
        strip = "char" if r.random() < burnt else "wood"
        broken = r.random() < 0.18
        stake(k, x0 + (x1 - x0) * t + r.uniform(-.04, .04), y, z0 + (z1 - z0) * t + r.uniform(-.04, .04),
              hh * (0.55 if broken else 1.0), lean=(px * r.uniform(-.05, .3) + r.uniform(-.08, .08),
                                                    pz * r.uniform(-.05, .3) + r.uniform(-.08, .08)), r=0.11,
              strip=strip, point=not broken)
    k.beam("wood", (x0 - px * 0.16, y + h * 0.45, z0 - pz * 0.16), (x1 - px * 0.16, y + h * 0.5, z1 - pz * 0.16),
           0.12, 0.12)
    # braces behind
    for t in (0.2, 0.55, 0.85):
        bx, bz = x0 + (x1 - x0) * t, z0 + (z1 - z0) * t
        k.beam("wood", (bx - px * 0.9, y, bz - pz * 0.9), (bx - px * 0.1, y + h * 0.7, bz - pz * 0.1), 0.1, 0.1)


def cheval(k, x, z, length=2.4, yaw=0.0, y=0.0, char=False):
    """Cheval de frise: a log bristling with crossed spikes."""
    strip = "char" if char else "wood"
    with k.at((x, y, z), yaw):
        k.beam(strip, (-length / 2, 0.55, 0), (length / 2, 0.55, 0), 0.2, 0.2, roll=math.pi / 4)
        n = int(length / 0.4)
        for i in range(n + 1):
            xx = -length / 2 + 0.1 + i * (length - 0.2) / n
            for s in (-1, 1):
                stake(k, xx + 0.04 * s, 0.0, s * 0.42, 1.3, lean=(0, -s * 0.95), r=0.055, strip=strip)


def mantlet(k, x, z, yaw=0.0, y=0.0, char=False, fallen=False):
    """Wheeled wooden siege shield."""
    strip = "char" if char else "wood"
    with k.at((x, y, z), yaw, (-1.35 if fallen else -0.22), 0):
        k.cbox("props", (0, 0.95, 0), (0.85, 0.95, 0.05), c=0.0, bottom=True, chip=0, strip=strip)
        for sx in (-0.6, 0.6):
            k.cbox("props", (sx, 0.95, 0.07), (0.05, 0.9, 0.03), c=0.0, bottom=True, chip=0, strip="iron")
        if not fallen:
            k.beam(strip, (0, 1.3, -0.05), (0, 0.05, -0.95), 0.09, 0.09)


def ladder(k, p0, p1, width=0.55, strip="wood", missing=(3,)):
    p0, p1 = v3(p0), v3(p1)
    d = p1 - p0
    L = np.linalg.norm(d)
    side = np.cross(d / L, v3(0, 1, 0))
    if np.linalg.norm(side) < 0.1:
        side = v3(1, 0, 0)
    side = side / np.linalg.norm(side) * width / 2
    k.beam(strip, p0 - side, p1 - side, 0.08, 0.08)
    k.beam(strip, p0 + side, p1 + side, 0.08, 0.08)
    n = int(L / 0.4)
    for i in range(1, n):
        if i in missing:
            continue
        c = p0 + d * (i / n)
        k.beam(strip, c - side, c + side, 0.05, 0.05)


def ram(k, x, z, yaw=0.0, y=0.0):
    """Wrecked battering ram: iron-capped log slung under a broken A-frame on wheels."""
    with k.at((x, y, z), yaw, 0, 0.12):
        for sz in (-0.7, 0.7):
            k.beam("wood", (-2.0, 0.5, sz), (2.0, 0.5, sz), 0.16, 0.16)
            for sx in (-1.4, 1.4):
                wheel(k, (sx, 0.45, sz * 1.22), 0.45, axis=(0, 0, 1), broken=(sx > 0 and sz > 0))
        for sx in (-1.5, 0.0, 1.5):
            k.beam("wood", (sx, 0.5, -0.7), (sx, 0.5, 0.7), 0.12, 0.12)
        # A-frames, the rear one snapped
        for sx, hh in ((-1.5, 1.9), (1.5, 1.1)):
            k.beam("wood", (sx, 0.5, -0.7), (sx, hh, 0), 0.12, 0.12)
            k.beam("wood", (sx, 0.5, 0.7), (sx, hh, 0), 0.12, 0.12)
        k.beam("char", (-1.5, 1.9, 0), (0.6, 1.5, 0.2), 0.12, 0.12)
        # the ram itself, fallen askew
        k.beam("wood", (-2.6, 0.85, 0.15), (2.4, 0.62, -0.1), 0.42, 0.42, roll=0.3)
        k.cbox("props", (2.7, 0.6, -0.12), (0.34, 0.27, 0.27), c=0.05, bottom=True, chip=0, strip="iron")
        k.beam("rope", (-1.5, 1.85, 0), (-1.4, 0.95, 0.1), 0.05, 0.05)


def crane(k, x, z, y=0.0, h=5.0, seed=0):
    """Timber hoist tower with a snapped jib; returns the top position."""
    r = np.random.default_rng(seed)
    s = 1.1
    for sx in (-1, 1):
        for sz in (-1, 1):
            k.beam("wood" if r.random() < 0.6 else "char", (x + sx * s, y, z + sz * s),
                   (x + sx * s * 0.8, y + h, z + sz * s * 0.8), 0.2, 0.2)
    for lvl in range(3):
        ya, yb = y + lvl * h / 3 + 0.15, y + (lvl + 1) * h / 3
        fa, fb = 1 - 0.2 * lvl / 3, 1 - 0.2 * (lvl + 1) / 3
        for (ax, az, bx, bz) in ((-1, -1, 1, -1), (1, -1, 1, 1), (1, 1, -1, 1), (-1, 1, -1, -1)):
            k.beam("wood", (x + ax * s * fb, yb, z + az * s * fb), (x + bx * s * fb, yb, z + bz * s * fb), 0.12, 0.12)
            if (lvl + ax) % 2 == 0:
                k.beam("char" if lvl == 2 else "wood", (x + ax * s * fa, ya, z + az * s * fa),
                       (x + bx * s * fb, yb, z + bz * s * fb), 0.09, 0.09)
    k.cbox("props", (x, y + h + 0.06, z), (s * 0.95, 0.06, s * 0.95), c=0.0, bottom=True, chip=0, strip="char")
    # jib, broken, hanging east
    k.beam("char", (x, y + h + 0.2, z), (x + 2.6, y + h + 1.2, z + 0.4), 0.22, 0.22)
    k.beam("char", (x + 2.6, y + h + 1.2, z + 0.4), (x + 4.0, y + h - 0.9, z + 0.9), 0.2, 0.2)
    k.beam("rope", (x + 4.0, y + h - 0.9, z + 0.9), (x + 4.05, y + h - 2.6, z + 0.9), 0.05, 0.05)
    k.beam("wood", (x - 0.2, y + h + 0.1, z), (x - 0.3, y + h + 2.0, z - 0.1), 0.18, 0.18)
    return (x, y + h + 0.3, z)


def ballista(k, x, y, z, yaw=0.0):
    with k.at((x, y, z), yaw):
        k.beam("wood", (-1.3, 0.25, -0.5), (1.3, 0.25, -0.5), 0.16, 0.16)
        k.beam("wood", (-1.3, 0.25, 0.5), (1.3, 0.25, 0.5), 0.16, 0.16)
        k.beam("wood", (-0.9, 0.25, -0.5), (-0.9, 0.25, 0.5), 0.14, 0.14)
        k.beam("wood", (0.9, 0.25, -0.5), (0.9, 0.25, 0.5), 0.14, 0.14)
        k.beam("wood", (-0.6, 0.3, 0), (0.2, 1.25, 0), 0.18, 0.18)
        k.beam("wood", (-1.6, 1.0, 0), (1.9, 1.45, 0), 0.2, 0.24)
        k.beam("wood", (1.2, 1.38, -1.5), (1.2, 1.38, 1.5), 0.12, 0.16)
        k.beam("iron", (1.2, 1.38, -1.5), (0.5, 1.38, -1.75), 0.06, 0.1)
        k.beam("rope", (0.5, 1.38, -1.75), (-1.2, 1.1, 0), 0.04, 0.04)
        k.beam("rope", (1.2, 1.38, 1.5), (-0.2, 0.3, 1.2), 0.04, 0.04)


# ------------------------------------------------------------------ the fallen
def body(k, x, z, yaw=0.0, y=0.0, shield=True, weapon=True, seed=0):
    """A fortress guard lying on the ground: rust-red and black plate, closed helm."""
    r = np.random.default_rng(seed)
    y0, sh = STRIPS["armour"]

    def part(c, half, seg, rot=None, col=(1, 1, 1)):
        k.cbox("props", c, half, c=0.03, rot=rot, bottom=True, chip=0, strip="armour", uvoff=(seg * 4 + 0.3, 0), col=col)

    with k.at((x, y, z), yaw):
        part((0, 0.13, 0), (0.3, 0.12, 0.2), 0, col=(0.8, 0.75, 0.75))   # cuirass (red)
        part((-0.42, 0.11, 0), (0.14, 0.1, 0.17), 1)                 # hips (black)
        part((0.44, 0.14, 0.02), (0.13, 0.13, 0.13), 2, rot=rot_y(0.3))  # helm
        a = r.uniform(-0.4, 0.4)
        for s in (-1, 1):
            b = a + s * r.uniform(0.1, 0.5)
            hx, hz = -0.55, s * 0.1
            kx, kz = hx - math.cos(b) * 0.4, hz + s * math.sin(abs(b)) * 0.4
            k.beam("armour", (hx, 0.09, hz), (kx, 0.1, kz), 0.14, 0.13, voff=0, u0=4.4)
            fx, fz = kx - 0.4 * math.cos(b * 0.4), kz + 0.4 * math.sin(b * 0.4)
            k.beam("armour", (kx, 0.1, kz), (fx, 0.07, fz), 0.12, 0.11, voff=0, u0=12.4)
            ax, az = 0.2, s * 0.24
            ex, ez = ax + r.uniform(-0.2, 0.35), az + s * r.uniform(0.2, 0.42)
            k.beam("armour", (ax, 0.13, az), (ex, 0.08, ez), 0.11, 0.1, voff=0, u0=4.4)
        if shield:
            sx, sz = r.uniform(-0.2, 0.5), r.uniform(0.5, 0.8) * (1 if r.random() < 0.5 else -1)
            k.prism("props", sx, sz, 0.36, 0.3, 0.0, 0.07, n=8, cap=True, strip="iron")
        if weapon:
            a = r.uniform(0, 6.28)
            px, pz = r.uniform(-0.3, 0.6), r.uniform(-0.9, 0.9)
            k.beam("iron", (px, 0.03, pz), (px + math.cos(a) * 0.95, 0.03, pz + math.sin(a) * 0.95), 0.07, 0.025)


def spear(k, x, y, z, yaw=0.0, pitch=0.0, length=2.2):
    d = euler(yaw, 0, pitch) @ v3(1, 0, 0)
    p0 = v3(x, y, z)
    p1 = p0 + d * length
    k.beam("wood", p0, p1, 0.045, 0.045)
    k.beam("iron", p1, p1 + d * 0.3, 0.07, 0.03)


def arrows(k, x, y, z, n=3, seed=0, spread=0.5):
    r = np.random.default_rng(seed)
    for _ in range(n):
        px, pz = x + r.uniform(-spread, spread), z + r.uniform(-spread, spread)
        lean = v3(r.uniform(-0.25, 0.25), 0.55, r.uniform(0.1, 0.45))
        k.beam("wood", (px, y, pz), v3(px, y, pz) + lean, 0.025, 0.025)
        tip = v3(px, y, pz) + lean
        k.beam("cloth", tip - lean * 0.2, tip, 0.07, 0.02)


def shield_flat(k, x, y, z, yaw=0.0, tilt=0.0):
    with k.at((x, y, z), yaw, tilt):
        k.prism("props", 0, 0, 0.36, 0.3, 0.0, 0.07, n=8, cap=True, strip="iron")


def weapon_rack(k, x, y, z, yaw=0.0):
    with k.at((x, y, z), yaw):
        k.beam("wood", (-0.8, 0, 0), (-0.8, 1.3, 0), 0.09, 0.09)
        k.beam("wood", (0.8, 0, 0), (0.8, 1.3, 0), 0.09, 0.09)
        k.beam("wood", (-0.8, 1.1, 0), (0.8, 1.1, 0), 0.07, 0.07)
        k.beam("wood", (-0.8, 0.3, 0), (0.8, 0.3, 0), 0.07, 0.07)
        for i in range(4):
            xx = -0.55 + i * 0.37
            k.beam("wood", (xx, 0.05, 0.12), (xx, 1.9, -0.05), 0.04, 0.04)
            k.beam("iron", (xx, 1.9, -0.05), (xx, 2.2, -0.08), 0.06, 0.03)


# ------------------------------------------------------------------ stonework
def pillar(k, x, z, y=0.0, h=5.0, w=0.8, broken=None, col=(1, 1, 1)):
    hh = broken if broken else h
    k.cbox("block", (x, y + 0.2, z), (w / 2 + 0.15, 0.2, w / 2 + 0.15), c=0.06, col=col)
    n = max(1, int(round((hh - 0.4) / 1.0)))
    seg = (hh - 0.4) / n
    for i in range(n):
        k.cbox("block", (x, y + 0.4 + seg * (i + 0.5), z), (w / 2, seg / 2, w / 2), c=0.04, col=col, bottom=False,
               chip=0.5 if (broken and i == n - 1) else 0.2)
    if not broken:
        k.cbox("block", (x, y + h + 0.15, z), (w / 2 + 0.18, 0.15, w / 2 + 0.18), c=0.06, col=col, bottom=True)


def arch(k, x0, x1, y0, z, depth=0.8, rise=None, blocks=9, missing=(), col=(1, 1, 1), axis="x", thick=0.55):
    """Semicircular arch of voussoirs springing at y0 between x0 and x1
    (along x at depth z, or along z at x = `z` when axis == 'z')."""
    c = (x0 + x1) / 2
    rad = abs(x1 - x0) / 2
    rise = rad if rise is None else rise
    for i in range(blocks):
        if i in missing:
            continue
        a = math.pi * (i + 0.5) / blocks
        px = c - math.cos(a) * (rad + thick / 2)
        py = y0 + math.sin(a) * (rise + thick / 2)
        seg = math.pi * (rad + thick / 2) / blocks
        if axis == "x":
            R = euler(0, 0, 0) @ np.array([[math.sin(a), -math.cos(a), 0], [math.cos(a), math.sin(a), 0], [0, 0, 1]])
            # local x tangent to the arch
            k.cbox("block", (px, py, z), (seg / 2 + 0.01, thick / 2, depth / 2), c=0.05, rot=R, bottom=True, col=col,
                   chip=0.25)
        else:
            R = np.array([[0, 0, 1], [math.cos(a), math.sin(a), 0], [-math.sin(a), math.cos(a), 0]])
            k.cbox("block", (z, py, px), (seg / 2 + 0.01, thick / 2, depth / 2), c=0.05, rot=R, bottom=True, col=col,
                   chip=0.25)


def merlons(k, x0, z0, x1, z1, y, step=2.0, size=(1.0, 0.9, 0.5), seed=0, missing=0.18, col=(1, 1, 1)):
    """Crenellation blocks along a wall edge from (x0, z0) to (x1, z1)."""
    r = np.random.default_rng(seed)
    L = math.hypot(x1 - x0, z1 - z0)
    n = max(1, int(L / step))
    ang = math.atan2(-(z1 - z0), x1 - x0)
    for i in range(n):
        if r.random() < missing:
            continue
        t = (i + 0.5) / n
        k.cbox("block", (x0 + (x1 - x0) * t, y + size[1] / 2, z0 + (z1 - z0) * t),
               (size[0] / 2, size[1] / 2, size[2] / 2), c=0.06, rot=rot_y(ang), col=col, chip=0.3)


def statue(k, x, z, y=0.0, yaw=0.0, fallen=True, col=(1, 1, 1)):
    """A robed stone figure on a plinth, toppled and broken."""
    k.cbox("block", (x, y + 0.45, z), (0.8, 0.45, 0.8), c=0.08, col=col)
    k.cbox("block", (x, y + 0.98, z), (0.62, 0.1, 0.62), c=0.04, col=col)
    if not fallen:
        with k.at((x, y + 1.08, z), yaw):
            k.cbox("block", (0, 0.9, 0), (0.42, 0.9, 0.3), c=0.1, col=col)
            k.cbox("block", (0, 2.05, 0), (0.24, 0.28, 0.24), c=0.08, col=col)
        return
    # stumps of the legs on the plinth, torso and head on the ground
    k.cbox("block", (x - 0.15, y + 1.3, z), (0.3, 0.25, 0.26), c=0.08, col=col, chip=0.9)
    with k.at((x, y, z), yaw):
        k.cbox("block", (1.9, 0.33, 0.3), (0.95, 0.33, 0.42), c=0.1, rot=euler(0.25, 0, 0.06), col=col, chip=0.6)
        k.cbox("block", (3.3, 0.24, 0.9), (0.26, 0.24, 0.26), c=0.08, rot=euler(0.9, 0.3, 0.4), col=col)
        k.cbox("block", (2.3, 0.14, -0.7), (0.5, 0.12, 0.14), c=0.05, rot=euler(-0.5, 0, 0), col=col)


def fountain(k, x, z, r=2.2, y=0.0, col=(1, 1, 1)):
    """Dry octagonal basin with a broken centre post."""
    n = 8
    for i in range(n):
        a = (i + 0.5) * 2 * math.pi / n
        if i == 5:
            continue  # smashed gap
        seg = 2 * r * math.tan(math.pi / n)
        k.cbox("block", (x + math.sin(a) * r, y + 0.26, z + math.cos(a) * r), (seg / 2 + 0.02, 0.26, 0.22), c=0.06,
               rot=rot_y(a), col=col, chip=0.4)
    k.prism("floor", x, z, r - 0.15, r - 0.15, y, y + 0.06, n=8, rot=math.pi / 8, cap=True, col=np.asarray(col) * 0.7)
    k.cbox("block", (x, y + 0.3, z), (0.5, 0.3, 0.5), c=0.08, col=col)
    k.cbox("block", (x, y + 0.85, z), (0.3, 0.3, 0.3), c=0.08, col=col, chip=0.9)


def tent(k, x, z, yaw=0.0, y=0.0, w=2.2, d=2.6, h=1.7, collapsed=False):
    y0, sh = STRIPS["cloth_dark"]
    with k.at((x, y, z), yaw):
        ridge = h if not collapsed else h * 0.45
        for s in (-1, 1):
            P = [v3(-d / 2, 0, s * w / 2), v3(d / 2, 0, s * w / 2), v3(d / 2, ridge, s * (0.0 if not collapsed else 0.3)),
                 v3(-d / 2, ridge * (1.0 if not collapsed else 0.3), 0)]
            uv = [(0, (y0 + 2) / ATLAS), (d * 32 / ATLAS, (y0 + 2) / ATLAS), (d * 32 / ATLAS, (y0 + sh - 2) / ATLAS),
                  (0, (y0 + sh - 2) / ATLAS)]
            k.poly("props", P, uv=uv, out=v3(0, 0.5, s), col=(0.9, 0.85, 0.8))
            k.poly("props", P, uv=uv, out=v3(0, -0.5, -s), col=(0.5, 0.45, 0.45))
        k.beam("wood", (-d / 2, 0, 0), (-d / 2, ridge * (1.0 if not collapsed else 0.3) + 0.15, 0), 0.07, 0.07)
        k.beam("wood", (d / 2, 0, 0), (d / 2, ridge + 0.15, 0), 0.07, 0.07)


def door_leaf(k, centre, w=2.0, h=6.0, rot=None, thick=0.14):
    """One leaf of the great bronze door (U runs up the leaf)."""
    R = np.eye(3) if rot is None else rot
    y0, sh = STRIPS["bronze"]
    c = v3(centre)
    for sgn in (1, -1):
        L = np.array([(-w / 2, -h / 2, sgn * thick / 2), (w / 2, -h / 2, sgn * thick / 2),
                      (w / 2, h / 2, sgn * thick / 2), (-w / 2, h / 2, sgn * thick / 2)])
        uv = [((q[1] + h / 2) * 32 / ATLAS, (y0 + (q[0] + w / 2) / w * (sh - 1)) / ATLAS) for q in L]
        k.poly("props", L @ R.T + c, uv=uv, out=R @ v3(0, 0, sgn))
    for sx in (-1, 1):
        L = np.array([(sx * w / 2, -h / 2, -thick / 2), (sx * w / 2, -h / 2, thick / 2),
                      (sx * w / 2, h / 2, thick / 2), (sx * w / 2, h / 2, -thick / 2)])
        uv = [((q[1] + h / 2) * 32 / ATLAS, (y0 + 2 + (q[2] + thick / 2) * 20) / ATLAS) for q in L]
        k.poly("props", L @ R.T + c, uv=uv, out=R @ v3(sx, 0, 0))
    L = np.array([(-w / 2, h / 2, -thick / 2), (w / 2, h / 2, -thick / 2), (w / 2, h / 2, thick / 2),
                  (-w / 2, h / 2, thick / 2)])
    uv = [((q[0] + w) * 32 / ATLAS, (y0 + 2 + (q[2] + thick / 2) * 20) / ATLAS) for q in L]
    k.poly("props", L @ R.T + c, uv=uv, out=R @ v3(0, 1, 0))
