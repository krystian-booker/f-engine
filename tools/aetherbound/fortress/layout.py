"""The fortress map: walkable grid, terrain, architecture, wreckage, lights and
emitters. build_scene() returns a Scene whose kit holds the geometry."""
import math
import numpy as np
from meshkit import Kit, v3, euler, rot_y, rot_x, rot_z, frame_from
from textures import STRIPS, ATLAS
import props as P

W, D = 32, 72
FIRE_COL = (1.0, 0.6, 0.25)
VIOLET_COL = (0.58, 0.32, 1.0)


def hashf(*a):
    x = math.sin(sum((i + 1) * 12.9898 * float(v) for i, v in enumerate(a)) + 4.123) * 43758.5453
    return x - math.floor(x)


class Scene:
    def __init__(self):
        self.kit = Kit()
        self.lights = []
        self.emitters = []
        self.stains = []          # (x, y, z, radius, strength)
        self.walk = np.zeros((D, W), dtype=bool)
        self.height = np.zeros((D, W), dtype=np.float64)
        self.ramp = [["."] * W for _ in range(D)]
        self.rise = np.ones((D, W), dtype=np.float64)
        self.points = {}
        self.zones = []
        self.fires = []
        self.fire_log = []

    # ------------------------------------------------------------ grid
    def walkable(self, x0, x1, z0, z1, h=0.0):
        """Inclusive cell ranges."""
        self.walk[z0:z1 + 1, x0:x1 + 1] = True
        self.height[z0:z1 + 1, x0:x1 + 1] = h

    def is_walk(self, x, z):
        i, j = int(math.floor(x)), int(math.floor(z))
        return 0 <= i < W and 0 <= j < D and bool(self.walk[j, i])

    def near_walk(self, x, z, m=0.0):
        return any(self.is_walk(x + dx, z + dz) for dx in (-m, 0, m) for dz in (-m, 0, m))

    def ground(self, x, z):
        i, j = int(math.floor(x)), int(math.floor(z))
        if not (0 <= i < W and 0 <= j < D):
            return 0.0
        h = self.height[j, i]
        if self.ramp[j][i] == "n":
            h += self.rise[j, i] * (1 - (z - j))
        return float(h)

    def point(self, name, x, z, facing=(0, -1), y=None):
        self.points[name] = {"position": [x, self.ground(x, z) if y is None else y, z], "facing": list(facing),
                             "layer": 0}

    # ------------------------------------------------------------ light and fire
    def light(self, pos, color=FIRE_COL, intensity=3.0, radius=8.0, flicker=0.22, speed=7.0, shadows=False,
              curve=None):
        self.lights.append({"type": "point", "position": [round(float(v), 3) for v in pos],
                            "color": [round(float(c), 3) for c in color], "intensity": round(float(intensity), 3),
                            "radius": round(float(radius), 3), "flicker": flicker, "flicker_speed": speed,
                            "flicker_curve": curve or [], "shadows": bool(shadows)})

    def emitter(self, pos, **s):
        e = {"position": [round(float(v), 3) for v in pos]}
        e.update(s)
        self.emitters.append(e)

    def flames(self, pos, s=1.0, area=None, rate=None):
        self.emitter(pos, sprite="fx/fortress/flame.anim.json", rate=rate or (12 + 8 * s), lifetime=0.8,
                     lifetime_jitter=0.2, velocity=[0.1, 1.3 + 0.9 * s, 0], velocity_jitter=[0.2 * s, 0.35, 0.2 * s],
                     area=area or [0.28 * s, 0.05, 0.28 * s], size=0.34 * s + 0.08, size_end=0.14 * s + 0.03,
                     color=[1.5, 1.05, 0.62, 1.0], color_end=[1.3, 0.3, 0.06, 0.0], blend="additive", max=96)

    def embers(self, pos, s=1.0, rate=None, area=None):
        self.emitter(pos, sprite="fx/fortress/ember.anim.json", rate=rate or (3 + 4 * s), lifetime=3.2,
                     lifetime_jitter=1.2, velocity=[0.35, 1.5 + 0.5 * s, 0.05], velocity_jitter=[0.7, 0.7, 0.7],
                     gravity=[0.25, -0.12, 0.05], drag=0.25, area=area or [0.3 * s, 0.1, 0.3 * s], size=0.045,
                     size_end=0.02, color=[3.2, 1.5, 0.45, 1.0], color_end=[2.2, 0.35, 0.05, 0.0], blend="additive",
                     max=96)

    def smoke(self, pos, s=1.0, rate=None, life=5.5):
        self.emitter(pos, sprite="fx/fortress/smoke.anim.json", rate=rate or (1.5 + 0.7 * s), lifetime=life,
                     lifetime_jitter=1.0, velocity=[0.45, 1.3 + 0.3 * s, 0.1], velocity_jitter=[0.25, 0.3, 0.25],
                     gravity=[0.12, 0.0, 0.03], area=[0.25 * s, 0.1, 0.25 * s], size=0.36 * s + 0.1,
                     size_end=1.25 * s + 0.4, color=[0.8, 0.42, 0.28, 0.46], color_end=[0.16, 0.15, 0.24, 0.0],
                     blend="alpha", max=64)

    def haze(self, pos, s=1.0):
        self.emitter(pos, sprite="fx/fortress/haze.anim.json", rate=3.0, lifetime=1.7, lifetime_jitter=0.3,
                     velocity=[0.1, 1.2, 0], velocity_jitter=[0.15, 0.2, 0.15], area=[0.3 * s, 0.1, 0.3 * s],
                     size=0.55 * s + 0.2, size_end=1.0 * s + 0.3, color=[1, 1, 1, 0.9], color_end=[1, 1, 1, 0.0],
                     distort=0.45, max=24)

    def foreground(self, x, z):
        """Where a fire at (x, z) sits for the field camera, which looks north
        from 11.6 south of and 5.9 above the party: 'near' = at the feet of
        someone walking just north of it, 'fore' = between the lens and the
        party (smoke there would drift across the picture), else None."""
        kind = None
        for cz in range(D):
            for cx in range(W):
                if not self.walk[cz, cx] or self.height[cz, cx] > 30:
                    continue
                lat = max(cx - x, x - (cx + 1), 0.0)
                d = z - (cz + 1)            # south of the cell's south edge
                d0 = z - cz
                if lat <= 1.0 and d0 > 0.3 and d <= 4.0:
                    return "near"
                if lat <= 3.2 and d0 > 4.0 and d <= 12.5:
                    kind = "fore"
        return kind

    def fire(self, pos, s=1.0, light=True, smoke=True, haze=None, embers=True, shadows=False, intensity=None,
             radius=None, stain=True, area=None, force=False):
        x, y, z = pos
        where = None if force else self.foreground(x, z)
        self.fire_log.append((round(x, 1), round(y, 1), round(z, 1), s, where))
        if where:
            smoke = False
            haze = False
        if where == "near" and s > 0.62:
            # smoulder: embers and glow, no tongues of flame in front of the party
            self.embers((x, y + 0.2, z), s, rate=5 + 3 * s)
            if light:
                self.light((x, y + 0.5, z), FIRE_COL, 0.8 * (intensity if intensity is not None else 2.0 + 1.6 * s),
                           radius if radius is not None else 5.0 + 3.6 * s, flicker=0.3, speed=5.0)
            if stain:
                self.stains.append((x, y, z, 1.6 + 1.6 * s, 0.55))
            self.fires.append((x, y, z, s))
            return
        self.flames((x, y + 0.15 * s, z), s, area=area)
        if embers:
            self.embers((x, y + 0.5 * s, z), s)
        if smoke:
            self.smoke((x, y + 1.8 * s, z), s)
        if haze if haze is not None else s >= 0.9:
            self.haze((x, y + 1.0 * s, z), s)
        if light:
            self.light((x, y + 0.45 + 0.35 * s, z), FIRE_COL,
                       1.3 * (intensity if intensity is not None else 2.0 + 1.6 * s),
                       1.08 * (radius if radius is not None else 5.0 + 3.6 * s), flicker=0.24, speed=6.0 + 3 * hashf(x, z),
                       shadows=shadows)
        if stain:
            self.stains.append((x, y, z, 1.6 + 1.6 * s, 0.55))
        self.fires.append((x, y, z, s))


# ---------------------------------------------------------------------------- walkable grid
def make_grid(S):
    S.walkable(13, 18, 50, 69)           # bridge
    for (x, z) in ((13, 55), (13, 56), (13, 57), (14, 56), (18, 59), (18, 60), (17, 59), (13, 69), (18, 69)):
        S.walk[z, x] = False              # bites out of the deck
    S.walkable(14, 17, 46, 49)           # gate passage
    S.walkable(4, 17, 40, 45)            # first yard (fight 1)
    S.walkable(4, 8, 33, 39)             # west side, north
    S.walkable(4, 25, 33, 37)            # east along z 33..37 (fight 2)
    S.walkable(21, 25, 23, 32)           # east side, north
    S.walkable(12, 25, 23, 26)           # back west to the stair foot
    for z in range(14, 23):              # grand stair, ramps rising north
        for x in range(13, 19):
            S.walk[z, x] = True
            S.height[z, x] = (22 - z) / 3.0
            S.ramp[z][x] = "n"
            S.rise[z, x] = 1.0 / 3.0
    S.walkable(11, 20, 8, 13, 3.0)       # landing

    S.point("start", 16, 64)
    S.point("cael", 15.5, 63.5)
    S.point("rowan", 16.6, 62.4)
    S.point("lyra", 14.6, 64.6)
    S.point("darius", 17.2, 65.0)
    S.point("gate", 16, 49.5)
    S.point("enc1_centre", 10.5, 42.5, (-1, 0))
    for i, (x, z) in enumerate(((13.2, 41.2), (14.0, 42.1), (13.3, 43.0), (14.2, 43.9))):
        S.point(f"enc1_party{i + 1}", x, z, (-1, 0))
    for i, (x, z) in enumerate(((7.8, 41.8), (7.2, 43.4))):
        S.point(f"enc1_enemy{i + 1}", x, z, (1, 0))
    S.point("enc2_centre", 17.5, 35.5, (1, 0))
    for i, (x, z) in enumerate(((15.0, 34.1), (14.2, 35.0), (15.2, 35.9), (14.0, 36.8))):
        S.point(f"enc2_party{i + 1}", x, z, (1, 0))
    for i, (x, z) in enumerate(((20.6, 34.2), (21.4, 35.6), (20.4, 37.0))):
        S.point(f"enc2_enemy{i + 1}", x, z, (-1, 0))
    S.point("stairs", 16, 22.5)
    S.point("door", 16, 9.5)
    # The crystal floats over the tower's crown. --check wants every point on walkable cells, so the
    # four cells of the crown under it are marked walkable (an island nothing can reach).
    S.walkable(15, 16, 3, 4, 39.2)
    S.points["crystal"] = {"position": [16.0, 46.0, 4.0], "facing": [0, 1], "layer": 0}
    S.zones = [
        {"id": "enc1", "type": "event", "layer": -1, "min": [9, 40], "max": [15, 45], "props": {}},
        {"id": "enc2", "type": "event", "layer": -1, "min": [12, 33], "max": [17, 38], "props": {}},
        {"id": "to_chamber", "type": "event", "layer": -1, "min": [13, 8], "max": [19, 10], "props": {}},
    ]


# ---------------------------------------------------------------------------- terrain
HX0, HZ0, HS = -8.0, -4.0, 0.5
HNX, HNZ = 96, 110


def stair_y(z):
    """Height of the stair tread at z (treads 0.5 deep, centred on the ramp)."""
    k = int(math.floor((23.0 - z) / 0.5 - 1e-9))
    k = max(0, min(17, k))
    return (k + 0.5) / 6.0


def south_wall_h(x):
    """Ruined south curtain: knee-high stumps west of the gate, rising east."""
    j = hashf(math.floor(x * 2), 3.3)
    if x < 3:
        return 2.0
    if x < 4:
        return 1.0 + 0.5 * (j > 0.5)
    if x < 12:
        return 0.5 + 0.5 * (j > 0.45)
    if x < 14:
        return 1.0                           # west pier stump
    if x < 18:
        return 0.0
    if x < 20:
        return 1.0 + 0.5 * (j > 0.6)        # east pier stump
    if x < 22.5:
        return 1.0 + 0.5 * (j > 0.5)
    if x < 25:
        return 2.0 + 0.5 * (j > 0.5)
    return 3.0 + 0.5 * (j > 0.4) + (0.5 if x > 27.5 else 0)


def west_wall_h(z):
    """Tall in the north (a backdrop), broken down to a parapet beside the
    west leg of the walk, where it would loom in the camera's foreground."""
    j = hashf(math.floor(z * 2), 7.1)
    if z < 28:
        return 6.0
    if z < 33:
        return max(2.0, 6.0 - (z - 28) * 0.8 - 0.5 * (j > 0.5))
    return 2.0 + 0.5 * (j > 0.62) - (0.5 if 37 < z < 41 else 0)


def east_wall_h(z):
    j = hashf(math.floor(z * 2), 9.7)
    if 27 <= z < 30.5:                       # breach from a siege stone
        return 3.0 + abs(z - 28.75) * 1.0 + 0.5 * (j > 0.5)
    if z < 27:
        return 6.0
    return 3.0 + 0.5 * (j > 0.5)


def terrain(S):
    k = S.kit
    H = np.full((HNZ, HNX), np.nan)
    TOP = np.full((HNZ, HNX), "floor", dtype=object)
    SIDE = np.full((HNZ, HNX), "wall", dtype=object)

    def cells(x0, x1, z0, z1):
        for j in range(int(round((z0 - HZ0) / HS)), int(round((z1 - HZ0) / HS))):
            for i in range(int(round((x0 - HX0) / HS)), int(round((x1 - HX0) / HS))):
                if 0 <= j < HNZ and 0 <= i < HNX:
                    yield j, i, HX0 + (i + 0.5) * HS, HZ0 + (j + 0.5) * HS

    def setr(x0, x1, z0, z1, h, top=None, side=None):
        for j, i, x, z in cells(x0, x1, z0, z1):
            H[j, i] = h(x, z) if callable(h) else h
            if top:
                TOP[j, i] = top
            if side:
                SIDE[j, i] = side

    # courtyard floor and gate passage; their sides show only in the fissure
    setr(3, 29, 20, 47.5, 0.0, "floor", "rock")
    setr(14, 18, 47.5, 50.0, 0.0, "floor", "rock")
    # north terrace
    setr(3, 29, 8, 20, 3.0, "floor", "wall")
    # grand stair, cut into the terrace and running out into the yard
    setr(13, 19, 14, 23, lambda x, z: stair_y(z), "block", "block")
    # stair cheeks: kerb beside the upper flight, stepped walls and plinths below
    for x0 in (12, 19):
        setr(x0, x0 + 1, 14, 20, 3.35, "block", "wall")
        setr(x0, x0 + 1, 20, 22, lambda x, z: math.ceil(stair_y(z) * 2) / 2 + 0.5, "block", "wall")
        setr(x0, x0 + 1, 22, 23, 1.5, "block", "wall")
    # west and east curtain walls with a wall-walk
    def crumble(h, x, z, inner):
        """Ruined stretches lose blocks unevenly, more so towards the yard."""
        if h > 4.6 or h <= 0:
            return h
        n = hashf(math.floor(x * 2), math.floor(z * 2), 4.4)
        drop = 0.5 * (n > 0.55) + 0.5 * (n > 0.86) + (0.5 if inner and n > 0.3 else 0)
        return max(0.5, h - drop)

    setr(0, 3, 4, 47.5, lambda x, z: crumble(west_wall_h(z), x, z, x > 2.5), "block", "wall")
    setr(29, 32, 4, 47.5, lambda x, z: crumble(east_wall_h(z), x, z, x < 29.5), "block", "wall")
    for j, i, x, z in cells(0, 3, 4, 28):
        TOP[j, i] = "floor"
    for j, i, x, z in cells(29, 32, 4, 27):
        TOP[j, i] = "floor"
    # south curtain: stumps
    setr(0, 14, 47.5, 50.0, lambda x, z: crumble(south_wall_h(x), x, z, z < 48.0), "block", "wall")
    setr(18, 32, 47.5, 50.0, lambda x, z: crumble(south_wall_h(x), x, z, z < 48.0), "block", "wall")
    # inner north wall either side of the great tower
    setr(-6, 9.0, 4, 8, 12.0, "floor", "wall")
    setr(23.0, 38, 4, 8, 12.0, "floor", "wall")
    # fissure torn across the yard between the two fights
    for j, i, x, z in cells(9, 29, 38, 40):
        edge_n = 38.0 + 0.5 * (hashf(i, 1.7) > 0.55)
        edge_s = 40.0 - 0.5 * (hashf(i, 5.1) > 0.5)
        if edge_n <= z <= edge_s:
            H[j, i] = -2.2 - 0.6 * hashf(i, j)
            TOP[j, i] = "rock"
            SIDE[j, i] = "rock"
    # a branch of it runs south to the gate's east pier and cuts the yard off from the east
    for j, i, x, z in cells(18.0, 19.5, 40, 46.5):
        edge_w = 18.0 + 0.5 * (hashf(j, 2.7) > 0.6)
        edge_e = 19.5 - 0.5 * (hashf(j, 6.1) > 0.5)
        if edge_w <= x <= edge_e:
            H[j, i] = -2.2 - 0.6 * hashf(i, j)
            TOP[j, i] = "rock"
            SIDE[j, i] = "rock"
    # the NW ruin (pump house): walls of a roofless building
    def ruin_h(x, z):
        j = hashf(math.floor(x * 2), math.floor(z * 2), 2.2)
        wallx = x < 3.5 or x > 10.5
        wallz = z > 25.5 or z < 20.5
        if not (wallx or wallz):
            return 0.0
        if z > 25.5 and 6 < x < 7.5:
            return 0.0                              # doorway
        if (wallx and x > 10.5 and 22 < z < 23.5) or (z > 25.5 and 8.5 < x < 9.5):
            return 1.0                              # window sills
        h = 4.5 - 1.5 * (j > 0.6) - 1.0 * (j > 0.85)
        if x > 9 and z > 24:
            h = min(h, 2.0 + j)                     # collapsed corner
        return math.floor(h * 2) / 2
    setr(3, 11, 20, 26, ruin_h, "block", "wall")
    for j, i, x, z in cells(3.5, 10.5, 20.5, 25.5):
        TOP[j, i] = "floor"

    def tint(p, n):
        # the grand stair: dark risers, treads lighter towards the nosing, so every step reads
        if 13.0 <= p[0] <= 19.0 and 14.0 <= p[2] <= 23.0 and p[1] < 3.05:
            if abs(n[1]) < 0.5:
                return (0.42, 0.4, 0.46) if abs(n[2]) > 0.5 else (0.7, 0.68, 0.74)
            f = (p[2] * 2) % 1.0
            f = 1.0 if f < 1e-6 and p[2] > 14.01 else f
            return (0.62 + 0.3 * f,) * 3
        return (1, 1, 1)

    k.columns(H, HX0, HZ0, HS, TOP, SIDE, base=-3.0, vt=1.0, col=tint)
    S.H = H
    return H


def hfield(S, x, z):
    i, j = int(math.floor((x - HX0) / HS)), int(math.floor((z - HZ0) / HS))
    if 0 <= j < HNZ and 0 <= i < HNX and not np.isnan(S.H[j, i]):
        return float(S.H[j, i])
    return 0.0


# ---------------------------------------------------------------------------- bridge and chasm
def bridge(S):
    k = S.kit
    X0, X1 = 12.4, 19.6
    TOPY, BOT = 0.0, -1.1
    # deck top, per half cell so bites and the torn end can be cut out
    def deck(x, z):
        if z < 50.0 or z > 70.9:
            return False
        if 13 <= x < 19 and z < 70 and not S.is_walk(x, z):
            return False                                   # bites
        if z >= 70:                                        # torn, jagged end
            return z < 70.0 + 0.9 * hashf(math.floor(x * 2), 8.8) ** 0.7
        if (x < 13 or x >= 19):
            # parapet strip: gone next to a bite
            return S.is_walk(min(18.5, max(13.5, x)), z)
        return True

    n_x = int(round((X1 - X0) / 0.5)) + 1
    xs = [X0] + [13.0 + 0.5 * i for i in range(13)] + [X1]
    zs = [50.0 + 0.5 * i for i in range(43)]
    for a, b in zip(xs[:-1], xs[1:]):
        for z in zs:
            xm, zm = (a + b) / 2, z + 0.25
            if not deck(xm, zm):
                continue
            P_ = [(a, TOPY, z), (a, TOPY, z + 0.5), (b, TOPY, z + 0.5), (b, TOPY, z)]
            k.poly("floor", P_, out=v3(0, 1, 0))
            # broken edges: inner faces where the neighbour is missing
            for (dx, dz, pa, pb) in ((-1, 0, (a, z), (a, z + 0.5)), (1, 0, (b, z + 0.5), (b, z)),
                                     (0, -1, (b, z), (a, z)), (0, 1, (a, z + 0.5), (b, z + 0.5))):
                xn = xm + dx * (b - a)
                zn = zm + dz * 0.5
                if xn < X0 or xn > X1:
                    continue  # outer faces are the spandrels
                if not deck(xn, zn) and zn > 50.0:
                    Q = [(pa[0], BOT, pa[1]), (pb[0], BOT, pb[1]), (pb[0], TOPY, pb[1]), (pa[0], TOPY, pa[1])]
                    k.poly("rock", Q, out=v3(dx, 0, dz), col=(0.8, 0.8, 0.85))
    # piers and arches under the deck
    piers = [(50.0, 51.2), (56.6, 58.6), (63.4, 65.4)]
    spans = [(51.2, 56.6), (58.6, 63.4), (65.4, 70.2)]
    for (za, zb) in piers[1:]:
        k.box("wall", (X0 - 0.3, -46, za), (X1 + 0.3, BOT, zb), faces="xXzZ", tess=4.0, col=(0.8, 0.8, 0.86))
        k.box("block", (X0 - 0.6, BOT - 0.5, za - 0.2), (X1 + 0.6, BOT, zb + 0.2), faces="xXzZYy", tess=4.0)
        # cutwater buttresses
        for sx, xx in ((-1, X0 - 0.3), (1, X1 + 0.3)):
            k.prism("wall", xx, (za + zb) / 2, 1.0, 0.7, -46, -2.2, n=6, cap=True, tess=6.0, col=(0.75, 0.75, 0.82))
    for si, (za, zb) in enumerate(spans):
        c, rad = (za + zb) / 2, (zb - za) / 2
        seg = 10
        torn = si == 2
        for i in range(seg):
            t0, t1 = i / seg, (i + 1) / seg
            z0_, z1_ = za + (zb - za) * t0, za + (zb - za) * t1
            if torn and z0_ > 69.4:
                continue
            ya = BOT - 0.25 - (rad - math.sqrt(max(0.0, rad * rad - (z0_ - c) ** 2))) * 0.9
            yb = BOT - 0.25 - (rad - math.sqrt(max(0.0, rad * rad - (z1_ - c) ** 2))) * 0.9
            for sx, xx in ((-1, X0), (1, X1)):
                Q = [(xx, ya, z0_), (xx, yb, z1_), (xx, TOPY, z1_), (xx, TOPY, z0_)]
                k.poly("wall", Q, out=v3(sx, 0, 0), col=(0.86, 0.86, 0.9))
            Q = [(X0, ya, z0_), (X1, ya, z0_), (X1, yb, z1_), (X0, yb, z1_)]
            k.poly("wall", Q, out=v3(0, -1, 0), col=(0.5, 0.5, 0.6))
        P.arch(k, za, zb, BOT - 0.25 - rad * 0.9, X0 - 0.06, depth=0.3, rise=rad * 0.9, blocks=11, axis="z",
               missing=(9, 10) if torn else (), thick=0.5, col=(0.9, 0.9, 0.95))
        P.arch(k, za, zb, BOT - 0.25 - rad * 0.9, X1 + 0.06, depth=0.3, rise=rad * 0.9, blocks=11, axis="z",
               missing=(8, 9, 10) if torn else (), thick=0.5, col=(0.9, 0.9, 0.95))
    # outer string course along the deck edge
    for xx, sx in ((X0, -1), (X1, 1)):
        for z in np.arange(50.0, 69.5, 1.0):
            if not deck(xx + sx * -0.2, z + 0.5):
                continue
            k.cbox("block", (xx + sx * 0.08, -0.14, z + 0.5), (0.16, 0.14, 0.5), c=0.04, bottom=True, chip=0.2)
    # torn end: dangling blocks and bent rebar
    for i in range(9):
        x = X0 + 0.5 + i * 0.8
        z = 70.0 + 0.9 * hashf(math.floor(x * 2), 8.8) ** 0.7
        k.cbox("block", (x, -0.45 - 0.3 * hashf(i, 4), z + 0.1), (0.35, 0.3 + 0.2 * hashf(i, 9), 0.3), c=0.06,
               rot=euler(hashf(i, 1) - 0.5, 0.25, 0.2 * hashf(i, 2)), bottom=True, chip=0.6, col=(0.85, 0.85, 0.9))
    for i in range(5):
        x = 13.2 + i * 1.3
        k.beam("iron", (x, -0.3, 70.3), (x + 0.2, -0.9 - 0.4 * hashf(i, 3), 71.2 + 0.5 * hashf(i, 6)), 0.06, 0.06)

    # parapets: bevelled blocks with gaps, posts every 5 units carrying braziers
    posts = (52.5, 57.5, 62.5, 67.5)
    for xx, sx in ((12.68, -1), (19.32, 1)):
        for z in np.arange(50.0, 70.0, 1.0):
            zc = z + 0.5
            if not deck(xx, zc):
                continue
            if any(abs(zc - p) < 0.6 for p in posts):
                continue
            j = hashf(z, sx, 3.9)
            if j < 0.17:
                continue                                      # block knocked into the chasm
            hgt = 0.9 if j > 0.34 else 0.45                   # or broken to half height
            k.cbox("block", (xx, hgt / 2, zc), (0.26, hgt / 2, 0.49), c=0.05, chip=0.45 if hgt < 0.9 else 0.25)
            if hgt == 0.9 and j > 0.5:
                k.cbox("block", (xx, 0.96, zc), (0.31, 0.07, 0.5), c=0.03, chip=0.2)   # coping
        for p in posts:
            if not deck(xx, p):
                continue
            broken = p > 66 or (p, sx) == (57.5, -1)
            k.cbox("block", (xx, 0.7 if not broken else 0.35, p), (0.36, 0.7 if not broken else 0.35, 0.36), c=0.06,
                   chip=0.3)
            if not broken:
                k.cbox("block", (xx, 1.47, p), (0.42, 0.08, 0.42), c=0.04, chip=0.2)
                lit = (p, sx) in ((62.5, -1), (62.5, 1), (57.5, 1), (52.5, -1), (52.5, 1))
                pos = P.brazier(k, xx, 1.55, p, 0.62, legs=False)
                if lit:
                    S.fire(pos, 0.6, smoke=False, intensity=3.6, radius=8.5, stain=False)
    # loose stones along the parapet feet (not where the party stands)
    k.rubble(16, 60, 4.0, 46, size=(0.05, 0.12), seed=5, squash=(0.9, 2.4),
             avoid=lambda x, z: not (12.9 < x < 19.1) or abs(x - 16) < 2.2 or not S.is_walk(x, z))
    # a fallen parapet block and shields near the bites
    P.arrows(k, 12.7, 0.9, 61.4, 3, seed=2, spread=0.12)
    P.arrows(k, 19.3, 0.9, 53.4, 3, seed=3, spread=0.12)
    P.arrows(k, 19.3, 0.9, 66.4, 2, seed=4, spread=0.12)


def cliffs(S):
    """The rock the fortress stands on: a south face under the curtain wall
    and returns up both sides, flaring out as they fall into the fog."""
    k = S.kit
    depth = 44.0

    def face(o, along, length, outward, seed):
        nu = int(length / 2.0)
        nv = 14
        pts = {}
        for j in range(nv + 1):
            for i in range(nu + 1):
                t = j / nv
                d = t * depth
                bulge = 0.6 + d * 0.22 + 2.4 * hashf(i // 2, j // 2, seed) + 1.2 * hashf(i, j, seed + 1)
                if j == 0:
                    bulge = 0.0
                pos = v3(o) + v3(along) * (i / nu * length) + v3(0, -d - (0.7 * hashf(i, j, seed + 2) if j else 0), 0) \
                    + v3(outward) * bulge
                pts[(i, j)] = pos
        for j in range(nv):
            for i in range(nu):
                a, b, c, d_ = pts[(i, j)], pts[(i + 1, j)], pts[(i + 1, j + 1)], pts[(i, j + 1)]
                shade = 0.95 - 0.5 * (j / nv)
                tris = ((a, b, c), (a, c, d_)) if (i + j) % 2 else ((a, b, d_), (b, c, d_))
                for tri in tris:
                    k.poly("rock", np.array(tri), out=v3(outward) + v3(0, 0.3, 0), col=(shade, shade, shade * 1.06))

    face((-9, -3.0, 50.0), (1, 0, 0), 21.4, (0, 0, 1), 1)            # south face, west of the bridge
    face((19.6, -3.0, 50.0), (1, 0, 0), 21.4, (0, 0, 1), 2)          # south face, east of the bridge
    face((12.4, -3.0, 50.0), (1, 0, 0), 7.2, (0, 0, 1), 5)           # under the abutment
    face((-9, -3.0, -6), (0, 0, 1), 56.0, (-1, 0, 0), 3)             # west side
    face((41, -3.0, 50.0), (0, 0, -1), 56.0, (1, 0, 0), 4)           # east side
    # rock shelf outside the walls up to the cliff edge
    for (x0, x1, z0, z1) in ((-9, 0, -6, 50.0), (32, 41, -6, 50.0)):
        for x in np.arange(x0, x1, 3.0):
            for z in np.arange(z0, z1, 4.0):
                xa, xb, za, zb = x, min(x1, x + 3.0), z, min(z1, z + 4.0)
                Q = [(xa, -3.0, za), (xa, -3.0, zb), (xb, -3.0, zb), (xb, -3.0, za)]
                k.poly("rock", Q, out=v3(0, 1, 0), col=(0.7, 0.7, 0.76))
    # fog floor far below: opaque, so the fog has something to settle on
    Q = [(-400, -40, -300), (-400, -40, 500), (460, -40, 500), (460, -40, -300)]
    k.poly("rock", Q, out=v3(0, 1, 0), col=(0.2, 0.2, 0.3))


def mist_sheets(S):
    """Drifts of mist lying in the chasm at three depths (alpha-blended, lit
    by the moon and by the fires on the bridge)."""
    k = S.kit
    regions = ((-90, -9.5, -20, 150), (41.5, 122, -20, 150), (-9.5, 12.3, 50.3, 150), (19.7, 41.5, 50.3, 150),
               (12.3, 19.7, 71.6, 150))
    for i, y in enumerate((-2.4, -4.6, -7.2)):
        for (x0, x1, z0, z1) in regions:
            nx = max(1, int(math.ceil((x1 - x0) / 16)))
            nz = max(1, int(math.ceil((z1 - z0) / 16)))
            for ix in range(nx):
                for iz in range(nz):
                    xa, xb = x0 + (x1 - x0) * ix / nx, x0 + (x1 - x0) * (ix + 1) / nx
                    za, zb = z0 + (z1 - z0) * iz / nz, z0 + (z1 - z0) * (iz + 1) / nz
                    Q = np.array([(xa, y, za), (xa, y, zb), (xb, y, zb), (xb, y, za)])
                    s = 1.0 / 32.0
                    uv = [((q[0] + 11.3 * i) * s, (q[2] + 7.7 * i) * s) for q in Q]
                    k.poly("mist", Q, uv=uv, out=v3(0, 1, 0), col=(0.36, 0.38, 0.46))


# ---------------------------------------------------------------------------- architecture
def round_tower(S, cx, cz, r, y0, y1, n=8, batter=0.5, broken=0.0, seed=0, col=(1, 1, 1), parapet=True,
                windows=(), roof=None):
    """Octagonal wall tower with a battered foot, a corbelled parapet and
    (optionally) a jagged, broken top or a spire."""
    k = S.kit
    rot = math.pi / n
    k.prism("wall", cx, cz, r + batter, r, y0, y0 + 6.0, n=n, rot=rot, cap=False, col=col, tess=2.0)
    k.prism("wall", cx, cz, r, r, y0 + 6.0, y1, n=n, rot=rot, cap=False, col=col, tess=2.0)
    if broken > 0:
        # ragged crown: per-face blocks of different heights
        for i in range(n):
            a = rot + (i + 0.5) * 2 * math.pi / n
            hgt = broken * hashf(i, seed)
            if hgt < 0.3:
                continue
            seg = 2 * r * math.tan(math.pi / n)
            k.cbox("wall", (cx + math.sin(a) * (r * math.cos(math.pi / n) - 0.3), y1 + hgt / 2,
                            cz + math.cos(a) * (r * math.cos(math.pi / n) - 0.3)), (seg / 2, hgt / 2, 0.3), c=0.05,
                   rot=rot_y(a), col=col, chip=0.7, bottom=True)
        k.prism("floor", cx, cz, r - 0.5, r - 0.5, y1 - 0.4, y1 - 0.3, n=n, rot=rot, cap=True, col=(0.5, 0.5, 0.55))
    else:
        k.prism("block", cx, cz, r + 0.1, r + 0.45, y1, y1 + 0.5, n=n, rot=rot, cap=False, col=col)
        k.prism("wall", cx, cz, r + 0.45, r + 0.45, y1 + 0.5, y1 + 1.1, n=n, rot=rot, cap=roof is None, col=col)
        if parapet and roof is None:
            for i in range(n):
                a = rot + (i + 0.5) * 2 * math.pi / n
                rr = (r + 0.45) * math.cos(math.pi / n) - 0.25
                k.cbox("block", (cx + math.sin(a) * rr, y1 + 1.55, cz + math.cos(a) * rr), (0.6, 0.45, 0.25), c=0.06,
                       rot=rot_y(a), col=col, chip=0.3)
        if roof:
            k.prism("block", cx, cz, r + 0.7, 0.12, y1 + 1.1, y1 + 1.1 + roof, n=n, rot=rot, cap=True,
                    col=(0.42, 0.42, 0.6), tess=2.0)
    for (ang, y, strip) in windows:
        a = ang
        rr = r * math.cos(math.pi / n) + 0.03
        cxx, czz = cx + math.sin(a) * rr, cz + math.cos(a) * rr
        tx, tz = math.cos(a), -math.sin(a)
        Pq = [(cxx - tx * 0.5, y, czz - tz * 0.5), (cxx + tx * 0.5, y, czz + tz * 0.5),
              (cxx + tx * 0.5, y + 1.0, czz + tz * 0.5), (cxx - tx * 0.5, y + 1.0, czz - tz * 0.5)]
        u = int(k.rng.integers(0, 16))
        k.strip_quad(strip, Pq, u, u + 1, 1.0, 0.0, out=v3(math.sin(a), 0, math.cos(a)))


def architecture(S):
    k = S.kit
    # ---- flanking towers at the south corners (off the path, tall)
    round_tower(S, -2.2, 47.2, 3.0, -14.0, 12.0, broken=3.2, seed=3,
                windows=[(0.0, 5.0, "window"), (math.pi / 2, 7.5, "window"), (0.0, 9.0, "window")])
    round_tower(S, 34.2, 47.2, 3.0, -14.0, 15.0, broken=2.4, seed=8,
                windows=[(0.0, 6.0, "window"), (-math.pi / 2, 9.0, "window"), (0.0, 11.5, "window"),
                         (math.pi / 4, 4.0, "window")])
    S.fire((-2.2, 12.2, 47.2), 2.2, intensity=7, radius=17)
    S.fire((34.2, 15.2, 47.2), 2.6, intensity=8, radius=19)
    k.beam("char", (-3.5, 12.0, 46.4), (-0.5, 14.6, 48.2), 0.3, 0.3)
    k.beam("char", (32.9, 15.0, 48.2), (35.7, 17.8, 46.1), 0.3, 0.3)
    k.beam("char", (35.5, 15.0, 48.0), (33.3, 16.9, 46.5), 0.26, 0.26)
    # ---- corner towers of the inner north wall
    round_tower(S, 0.2, 5.6, 3.0, -6.0, 17.0, seed=1, roof=None, broken=2.8,
                windows=[(0.0, 9.0, "window"), (0.0, 13.0, "window"), (math.pi / 4, 11.0, "window")])
    round_tower(S, 31.8, 5.6, 3.0, -6.0, 18.0, seed=2, roof=6.5,
                windows=[(0.0, 9.5, "window"), (0.0, 14.0, "window"), (-math.pi / 4, 12.0, "window"),
                         (0.0, 16.3, "window_violet")])
    S.fire((0.2, 17.2, 5.6), 2.0, intensity=6, radius=16)
    # ---- crenellations
    def wall_merlons(x_out, zs, hfn, seed):
        r = np.random.default_rng(seed)
        for z in zs:
            h = hfn(z + 0.5)
            if abs(hfn(z + 0.5) - hfn(z + 1.5)) > 0.4 or h < 3.4 or r.random() < 0.2:
                continue
            k.cbox("block", (x_out, h + 0.45, z + 0.5), (0.25, 0.45, 0.5), c=0.06, chip=0.3)
    wall_merlons(0.3, range(8, 44, 2), west_wall_h, 11)
    wall_merlons(31.7, range(8, 44, 2), east_wall_h, 12)
    P.merlons(k, -5.5, 7.7, 8.7, 7.7, 12.0, step=2.0, seed=4, missing=0.15)
    P.merlons(k, 23.3, 7.7, 37.5, 7.7, 12.0, step=2.0, seed=5, missing=0.15)
    # string course and plinth along the inner north wall and the terrace front
    for (xa, xb) in ((3.0, 8.9), (23.1, 29.0)):
        k.box("block", (xa, 9.6, 8.0), (xb, 10.0, 8.25), faces="xXYyZ", tess=2.0)
        k.box("block", (xa, 3.0, 8.0), (xb, 3.6, 8.3), faces="xXYZ", tess=2.0)
    for (xa, xb) in ((11.0, 12.0), (20.0, 29.0)):
        k.box("block", (xa, 2.75, 20.0), (xb, 3.12, 20.18), faces="xXYyZ", tess=2.0)
    # terrace balustrade: low blocks and posts on the east side (the west meets the ruin's wall)
    for x in np.arange(20.5, 28.6, 1.0):
        if hashf(x, 2.9) < 0.22:
            continue
        k.cbox("block", (x + 0.25, 3.3, 19.7), (0.46, 0.3, 0.2), c=0.05, chip=0.35)
    # ---- lit windows and arrow slits on the inner north wall
    for x, y, strip in ((4.2, 6.0, "window"), (7.8, 6.0, "window"), (24.2, 6.0, "window"),
                        (27.8, 6.0, "window"), (4.6, 10.4, "window"), (27.4, 10.4, "window")):
        u = int(k.rng.integers(0, 16))
        k.strip_quad(strip, [(x - 0.5, y, 8.03), (x + 0.5, y, 8.03), (x + 0.5, y + 1, 8.03), (x - 0.5, y + 1, 8.03)],
                     u, u + 1, 1.0, 0.0, out=v3(0, 0, 1))
    # ---- banners
    P.banner(k, 6.0, 9.4, 8.26, 0, 1, width=2.0, length=5.0, seed=1, tatter=0.5)
    P.banner(k, 26.0, 9.4, 8.26, 0, 1, width=2.0, length=5.0, seed=2, tatter=0.8)
    P.banner(k, 3.02, 5.4, 28.0, 1, 0, width=1.6, length=3.4, seed=3, tatter=0.7)
    P.banner(k, 28.98, 2.9, 34.6, -1, 0, width=1.4, length=2.1, seed=4, tatter=0.6)
    P.banner(k, 28.98, 5.4, 24.5, -1, 0, width=1.6, length=3.2, seed=6, tatter=0.9, strip="cloth_dark")
    P.banner(k, 23.5, 2.7, 20.2, 0, 1, width=1.4, length=2.1, seed=7, tatter=0.5)
    P.banner(k, 26.5, 2.7, 20.2, 0, 1, width=1.4, length=2.1, seed=8, tatter=1.0, strip="cloth_dark")
    P.flag_pole(k, 1.5, 6.0, 12.0, 5.0, seed=9, yaw=0.3)
    P.flag_pole(k, 30.5, 6.0, 14.0, 5.0, seed=10, yaw=-0.2, strip="cloth_dark")
    # ---- gatehouse remains: springers, tumbled voussoirs, the gate itself
    for (x, y, z, rot_, half) in ((13.4, 1.22, 49.1, 0.1, (0.55, 0.22, 0.8)), (18.6, 1.24, 49.1, -0.15, (0.55, 0.24, 0.8)),
                                  (12.6, 0.3, 46.6, 0.7, (0.5, 0.3, 0.35)), (19.9, 0.32, 46.9, -0.4, (0.5, 0.32, 0.35)),
                                  (11.4, 0.25, 46.9, 1.9, (0.45, 0.25, 0.3)), (20.4, 0.28, 47.0, 1.2, (0.5, 0.28, 0.3)),
                                  (9.6, 0.22, 46.8, 0.3, (0.4, 0.22, 0.3))):
        k.cbox("block", (x, y, z), half, c=0.07, rot=euler(rot_, 0.1, 0.08), chip=0.6)
    k.rubble(11.3, 46.9, 1.3, 16, size=(0.1, 0.24), seed=21, squash=(1.5, 0.45))
    k.rubble(20.6, 46.95, 1.0, 14, size=(0.1, 0.26), seed=22, squash=(1.0, 0.4), avoid=lambda x, z: x < 19.7)
    k.rubble(7.0, 46.9, 2.5, 18, size=(0.08, 0.2), seed=23, squash=(1.3, 0.22))
    # the gate leaf blown flat into the yard (iron-bound timber) and its twin hanging off the east pier
    with k.at((15.75, 0.0, 43.7), 0.22):
        k.cbox("props", (0, 0.045, 0), (1.05, 0.045, 2.2), c=0.0, bottom=False, chip=0, strip="wood",
               col=(0.8, 0.8, 0.85))
        for zz in (-1.6, 0.0, 1.6):
            k.cbox("props", (0, 0.1, zz), (1.07, 0.02, 0.12), c=0.0, bottom=False, chip=0, strip="iron")
    with k.at((19.9, 0.0, 46.9), -0.5, 0, -0.5):
        k.cbox("props", (0, 1.4, 0), (0.07, 1.6, 0.9), c=0.0, bottom=True, chip=0, strip="char")
        for yy in (0.5, 1.5, 2.5):
            k.cbox("props", (0, yy, 0), (0.09, 0.1, 0.92), c=0.0, bottom=True, chip=0, strip="iron")


def shard(k, x, y, z, h=0.5, r=0.12, lean=(0, 0), seed=0):
    """A small Aetherite crystal: violet, emissive."""
    y0, sh = STRIPS["conduit"]
    tip = v3(x + lean[0], y + h, z + lean[1])
    n = 5
    ring = [v3(x + math.sin(i * 2 * math.pi / n + seed) * r, y + h * 0.25, z + math.cos(i * 2 * math.pi / n + seed) * r)
            for i in range(n)]
    base = v3(x, y - 0.05, z)
    for i in range(n):
        a, b = ring[i], ring[(i + 1) % n]
        u = (16 + 32 * ((i + seed) % 8)) / ATLAS
        for tri, row in (((a, b, tip), 13 + (i % 3) * 2), ((b, a, base), 18)):
            uv = [(u - 0.01, (y0 + row) / ATLAS), (u + 0.01, (y0 + row) / ATLAS), (u, (y0 + row + 1) / ATLAS)]
            k.poly("props", np.array(tri), uv=uv, out=(a + b) / 2 - v3(x, (a[1] + b[1]) / 2, z))


def tower(S):
    """The great central tower: a stepped keep 40 units tall with corner
    turrets, Aetherite conduits climbing its faces and a clawed crown under
    the crystal."""
    k = S.kit
    cx = 16.0
    col = (0.5, 0.5, 0.62)
    # (y0, y1, half width, centre z): every stage steps back from the south face
    stages = [(3.0, 14.0, 7.0, 1.0), (14.0, 24.0, 5.8, 1.4), (24.0, 32.0, 4.6, 1.8), (32.0, 38.0, 3.4, 2.2)]
    for si, (ya, yb, hw, cz) in enumerate(stages):
        faces = "xXzZ" if si else "xXz"
        k.box("wall", (cx - hw, ya, cz - hw), (cx + hw, yb, cz + hw), faces=faces, tess=2.0, col=col)
        k.box("block", (cx - hw - 0.3, yb - 0.5, cz - hw - 0.3), (cx + hw + 0.3, yb, cz + hw + 0.3), faces="xXzZy",
              tess=2.0, col=col)
        k.box("floor", (cx - hw - 0.3, yb - 0.1, cz - hw - 0.3), (cx + hw + 0.3, yb, cz + hw + 0.3), faces="Y",
              tess=2.0, col=(0.55, 0.55, 0.65))
        P.merlons(k, cx - hw, cz + hw + 0.05, cx + hw, cz + hw + 0.05, yb, step=1.6, size=(0.8, 0.7, 0.4), seed=si,
                  missing=0.1, col=col)
        for sx in (-1, 1):
            P.merlons(k, cx + sx * (hw + 0.05), cz - hw, cx + sx * (hw + 0.05), cz + hw, yb, step=1.6,
                      size=(0.8, 0.7, 0.4), seed=si + 5 + sx, missing=0.1, col=col)
        # corner pilasters (those on the south face of the first stage stay behind z = 8)
        for sx in (-1, 1):
            for sz in (-1, 1):
                if si == 0 and sz > 0:
                    k.box("wall", (cx + sx * hw - 0.7, ya, cz + hw - 1.4), (cx + sx * hw + 0.7, yb + 0.9, cz + hw + 0.2),
                          faces="xXzZY", tess=2.0, col=col)
                else:
                    k.box("wall", (cx + sx * hw - 0.6, ya, cz + sz * hw - 0.6),
                          (cx + sx * hw + 0.6, yb + 0.9, cz + sz * hw + 0.6), faces="xXzZY", tess=2.0, col=col)
        # buttress ribs on the south face of the upper stages
        if si:
            for dx in (-hw * 0.5, hw * 0.5):
                k.box("wall", (cx + dx - 0.35, ya, cz + hw), (cx + dx + 0.35, yb - 0.5, cz + hw + 0.25), faces="xXZ",
                      tess=2.0, col=col)
    # corner turrets rising from the first setback
    for sx in (-1, 1):
        for sz in (-1, 1):
            tx, tz = cx + sx * 6.2, 1.0 + sz * 6.2 - (0.5 if sz > 0 else 0)
            k.prism("wall", tx, tz, 1.25, 1.25, 14.0, 28.5, n=8, rot=math.pi / 8, cap=False, col=col, tess=3.0)
            k.prism("block", tx, tz, 1.3, 1.6, 28.5, 29.0, n=8, rot=math.pi / 8, cap=False, col=col)
            k.prism("block", tx, tz, 1.65, 0.1, 29.0, 34.0, n=8, rot=math.pi / 8, cap=True, col=(0.4, 0.4, 0.6),
                    tess=3.0)
            if sz > 0:
                u = int(k.rng.integers(0, 16))
                zf = tz + 1.25 * math.cos(math.pi / 8) + 0.03
                k.strip_quad("window_violet", [(tx - 0.5, 23.0, zf), (tx + 0.5, 23.0, zf), (tx + 0.5, 24.0, zf),
                                               (tx - 0.5, 24.0, zf)], u, u + 1, 1.0, 0.0, out=v3(0, 0, 1))
    # ---- south face of the first stage, with the great door
    z = 8.0
    k.grid("wall", (9.0, 3, z), (5.0, 0, 0), (0, 11, 0), 3, 6, out=v3(0, 0, 1), col=col)
    k.grid("wall", (18.0, 3, z), (5.0, 0, 0), (0, 11, 0), 3, 6, out=v3(0, 0, 1), col=col)
    seg = 8
    for i in range(seg):
        xa, xb = 14 + i * 0.5, 14.5 + i * 0.5
        ya = 7 + math.sqrt(max(0.0, 4 - (xa - 16) ** 2))
        yb = 7 + math.sqrt(max(0.0, 4 - (xb - 16) ** 2))
        for (lo_a, lo_b, hi) in ((ya, yb, 10.0), (10.0, 10.0, 12.0), (12.0, 12.0, 14.0)):
            k.poly("wall", [(xa, lo_a, z), (xb, lo_b, z), (xb, hi, z), (xa, hi, z)], out=v3(0, 0, 1), col=col)
        k.poly("wall", [(xa, ya, z), (xb, yb, z), (xb, yb, z - 1.2), (xa, ya, z - 1.2)], out=v3(0, -1, 0),
               col=(0.45, 0.42, 0.6))
    for xx, sx in ((14.0, 1), (18.0, -1)):
        k.grid("wall", (xx, 3, z - 1.2), (0, 0, 1.2), (0, 4, 0), 1, 4, out=v3(sx, 0, 0), col=(0.5, 0.48, 0.66))
    k.grid("floor", (14, 3.0, z - 1.2), (4, 0, 0), (0, 0, 1.2), 4, 1, out=v3(0, 1, 0), col=(0.6, 0.55, 0.8))
    # the violet light of the chamber behind the leaves
    y0, sh = STRIPS["conduit"]
    glow_uv = [(16 / ATLAS, (y0 + 11.3) / ATLAS), (17 / ATLAS, (y0 + 11.3) / ATLAS), (17 / ATLAS, (y0 + 11.7) / ATLAS),
               (16 / ATLAS, (y0 + 11.7) / ATLAS)]
    for inset, row, dz in ((0.0, 11.5, 1.9), (0.55, 12.8, 1.86), (1.15, 14.6, 1.82)):
        uvq = [(16 / ATLAS, (y0 + row - 0.2) / ATLAS), (17 / ATLAS, (y0 + row - 0.2) / ATLAS),
               (17 / ATLAS, (y0 + row + 0.2) / ATLAS), (16 / ATLAS, (y0 + row + 0.2) / ATLAS)]
        k.poly("props", [(14 + inset, 3, z - dz), (18 - inset, 3, z - dz), (18 - inset, 9 - inset * 1.6, z - dz),
                         (14 + inset, 9 - inset * 1.6, z - dz)], uv=uvq, out=v3(0, 0, 1))
    for i in range(seg):
        xa, xb = 14 + i * 0.5, 14.5 + i * 0.5
        ya = 7 + math.sqrt(max(0.0, 4 - (xa - 16) ** 2))
        yb = 7 + math.sqrt(max(0.0, 4 - (xb - 16) ** 2))
        k.strip_quad("bronze", [(xa, 7.0, z - 1.15), (xb, 7.0, z - 1.15), (xb, yb, z - 1.15), (xa, ya, z - 1.15)],
                     xa, xb, 1.0, 0.0, out=v3(0, 0, 1))
    k.beam("bronze", (14, 7.0, z - 1.1), (18, 7.0, z - 1.1), 0.25, 0.3)
    P.door_leaf(k, (15.0, 5.0, z - 1.1), w=2.0, h=4.0)
    P.door_leaf(k, (17.55, 5.0, z - 1.75), w=2.0, h=4.0, rot=rot_y(1.05))
    P.arch(k, 14, 18, 7.0, z - 0.13, depth=0.5, blocks=9, thick=0.7, col=(0.95, 0.93, 1.0))
    for xx in (13.65, 18.35):
        for yy in (3.5, 4.5, 5.5, 6.5):
            k.cbox("block", (xx, yy, z - 0.15), (0.36, 0.5, 0.27), c=0.05, col=(0.95, 0.93, 1.0), bottom=True, chip=0.2)
    k.cbox("block", (16, 9.75, z - 0.1), (0.5, 0.55, 0.3), c=0.06, col=(1, 1, 1), bottom=True)   # keystone
    for xx in (11.4, 20.6):
        k.box("wall", (xx - 0.4, 3, z), (xx + 0.4, 14, z + 0.2), faces="xXZ", tess=2.0, col=col)
    # ---- windows

    def window(x, y, zf, strip="window_violet", s=1.5):
        u = int(k.rng.integers(0, 16))
        k.strip_quad(strip, [(x - s / 2, y, zf), (x + s / 2, y, zf), (x + s / 2, y + s, zf), (x - s / 2, y + s, zf)],
                     u, u + 1, 1.0, 0.0, out=v3(0, 0, 1))
    for x, y in ((10.0, 6.0), (22.0, 6.0), (10.0, 10.6), (22.0, 10.6), (12.6, 11.0), (19.4, 11.0)):
        window(x, y, 8.03, "window", 1.2)
    for (ya, yb, hw, cz), xs_, yy in ((stages[1], (-4.2, -1.5, 1.5, 4.2), (16.2, 20.4)),
                                      (stages[2], (-3.2, 0.0, 3.2), (26.0, 28.9)), (stages[3], (-1.7, 1.7), (33.6,))):
        for dx in xs_:
            for y in yy:
                window(cx + dx, y, cz + hw + 0.03)
    for sx in (-1, 1):
        for (ya, yb, hw, cz), yy in ((stages[0], (7.0, 10.5)), (stages[1], (17.0, 20.5)), (stages[2], (26.5,))):
            for dz in (-2.5, 1.5, 4.2):
                for y in yy:
                    if abs(dz) > hw - 1.2:
                        continue
                    u = int(k.rng.integers(0, 16))
                    xx = cx + sx * (hw + 0.03)
                    k.strip_quad("window_violet" if y > 12 else "window",
                                 [(xx, y, cz + dz + sx * 0.6), (xx, y, cz + dz - sx * 0.6),
                                  (xx, y + 1.2, cz + dz - sx * 0.6), (xx, y + 1.2, cz + dz + sx * 0.6)], u, u + 1, 1.0,
                                 0.0, out=v3(sx, 0, 0))
    # ---- conduits climbing the south face and the flanks

    def conduit(pts, w=0.45):
        for a, b in zip(pts[:-1], pts[1:]):
            k.beam("conduit", a, b, w, w * 0.7)
    offs = (3.2, 2.9, 2.3, 1.6)
    for sx in (-1, 1):
        pts = [(16 + sx * offs[0], 3.0, 8.1)]
        for si, (ya, yb, hw, cz) in enumerate(stages):
            zf = cz + hw + (0.1 if si == 0 else 0.3)
            pts.append((16 + sx * offs[si], yb + 0.25, zf))
            if si < 3:
                nz = stages[si + 1][3] + stages[si + 1][2] + 0.3
                pts.append((16 + sx * offs[si + 1], yb + 0.25, nz))
        conduit(pts)
        pts = []
        for si, (ya, yb, hw, cz) in enumerate(stages):
            xf = cx + sx * (hw + 0.28)
            if si == 0:
                pts.append((xf, 3.0, cz + 3.0))
            pts.append((xf, yb + 0.25, cz + 3.0))
            if si < 3:
                pts.append((cx + sx * (stages[si + 1][2] + 0.28), yb + 0.25, stages[si + 1][3] + 3.0))
        conduit(pts, 0.4)
    for (ya, yb, hw, cz) in stages[:3]:
        k.beam("bronze", (cx - hw - 0.32, yb - 0.75, cz + hw + 0.34), (cx + hw + 0.32, yb - 0.75, cz + hw + 0.34), 0.12,
               0.45)
    # ---- crown: bronze claws around the crystal, a glowing focus beneath it
    az = 4.0
    k.prism("props", cx, az, 1.7, 1.45, 38.0, 39.2, n=8, cap=True, strip="bronze")
    k.prism("props", cx, az, 0.3, 0.22, 39.2, 39.9, n=8, cap=True, strip="conduit")
    hw, cz = stages[3][2], stages[3][3]
    for sx in (-1, 1):
        for sz in (-1, 1):
            off = v3(sx * (hw - 0.3), 0, cz + sz * (hw - 0.3) - az)
            pts = [v3(cx, y, az) + off * f for f, y in ((1.0, 38.0), (1.3, 41.2), (1.08, 44.2), (0.62, 46.6))]
            for i, (a, b) in enumerate(zip(pts[:-1], pts[1:])):
                w = 0.85 - i * 0.22
                k.beam("bronze", a, b, w, w)
            k.beam("conduit", pts[0], pts[1], 0.3, 0.95)
    S.tower_top = (cx, 39.9, az)


# ---------------------------------------------------------------------------- dressing
def sight_limit(z_path_south):
    """Tallest a thing may stand at z, south of a path whose southernmost
    walkable row ends at z_path_south, without hiding feet from the field camera."""
    return lambda x, z: max(0.12, 0.48 * (z - z_path_south) - 0.05)


def debris_fire(S, x, z, s=1.0, y=0.0, n=4, seed=0, **kw):
    """A heap of burning timber: charred beams criss-crossed, with a fire."""
    k = S.kit
    r = np.random.default_rng(seed)
    for i in range(n):
        a = r.uniform(0, math.pi)
        L = r.uniform(0.9, 1.7) * s
        dx, dz = math.cos(a) * L / 2, math.sin(a) * L / 2
        lift = r.uniform(0.05, 0.5) * s
        k.beam("char", (x - dx, y + 0.08, z - dz), (x + dx, y + 0.1 + lift, z + dz), 0.16 * s + 0.04, 0.16 * s + 0.04)
    k.rubble(x, z, 0.7 * s, int(5 * s) + 2, size=(0.06, 0.14), y=y, seed=seed + 50, mat="block", col=(0.5, 0.45, 0.45))
    S.fire((x, y + 0.2 * s, z), s, **kw)


def dressing(S):
    k = S.kit
    # ================= the breach and the first yard (fight 1) =================
    # rubble apron inside the south wall: low, the camera looks over it
    k.mound("rock", 8.0, 47.0, 4.6, 0.75, 0.42, seed=3, res=0.5, col=(0.8, 0.8, 0.85))
    k.mound("rock", 11.6, 47.9, 2.2, 1.6, 0.9, seed=4, res=0.5, col=(0.8, 0.8, 0.85), clip=sight_limit(46.0))
    k.rubble(8.5, 46.8, 4.5, 26, size=(0.07, 0.17), seed=31, squash=(1.0, 0.16))
    P.body(k, 6.4, 46.55, 0.3, seed=1)
    P.body(k, 10.2, 46.6, 2.9, seed=2)
    P.spear(k, 8.3, 0.0, 46.7, 0.4, 0.9, 1.8)
    P.arrows(k, 5.2, 0.0, 46.6, 4, seed=7, spread=0.45)
    debris_fire(S, 4.9, 47.0, 0.75, seed=1, intensity=3.4, radius=8.5)
    # west wall foot beside fight 1: stores, a torch
    P.crate(k, 3.45, 0, 44.4, 0.7, 0.2)
    P.crate(k, 3.4, 0.7, 44.5, 0.5, 0.6)
    P.barrel(k, 3.45, 0, 43.3)
    P.sack_pile(k, 3.5, 0, 41.2, 4, seed=3)
    P.barrel(k, 3.5, 0, 39.6, lying=True, yaw=0.4)
    P.weapon_rack(k, 3.35, 0, 37.2, math.pi / 2)
    P.crate(k, 3.45, 0, 35.3, 0.65, -0.3, char=True)
    P.body(k, 3.55, 34.2, 1.7, seed=3, shield=False)
    for z in (42.3, 36.2):
        pos = P.sconce(k, 3.02, 1.25, z, 1, 0)
        S.fire(pos, 0.5, smoke=False, intensity=3.2, radius=8.0, stain=False)
    # loose stones on the broken tops of the low walls
    k.rubble(1.5, 40.5, 6.5, 44, size=(0.1, 0.26), seed=41, squash=(0.2, 1.0), height=lambda x, z: hfield(S, x, z))
    k.rubble(30.5, 39.0, 8.0, 44, size=(0.1, 0.26), seed=42, squash=(0.16, 1.0), height=lambda x, z: hfield(S, x, z))
    k.rubble(8.0, 48.7, 5.5, 30, size=(0.1, 0.24), seed=43, squash=(1.0, 0.2), height=lambda x, z: hfield(S, x, z))
    k.rubble(24.0, 48.7, 4.5, 26, size=(0.1, 0.24), seed=44, squash=(1.0, 0.2), height=lambda x, z: hfield(S, x, z))
    # ================= east of the gate: the fallen gatehouse =================
    # (low in the north, where the camera of the east leg looks over it; piled up against the wall in the south)
    def c_limit(x, z):
        return max(0.22, min(0.9 * (z - 43.3), 0.48 * (z - 38.0) - 0.05))
    k.mound("rock", 24.6, 45.6, 5.0, 2.6, 2.8, seed=6, res=0.55, lean=(0.3, 0.7), power=1.1, col=(0.8, 0.8, 0.84),
            clip=c_limit)
    k.mound("rock", 24.0, 42.2, 4.4, 1.7, 0.3, seed=16, res=0.6, col=(0.75, 0.75, 0.8))
    for (x, y, z, half, ry) in ((23.3, 1.0, 45.3, (0.9, 0.5, 0.6), 1.2), (26.2, 1.9, 46.3, (1.0, 0.55, 0.6), -0.3),
                                (27.7, 1.3, 45.0, (0.7, 0.45, 0.5), 0.9), (21.0, 0.6, 45.9, (0.6, 0.4, 0.5), 0.4),
                                (25.2, 0.22, 42.4, (0.6, 0.2, 0.45), 2.0), (22.4, 0.2, 41.4, (0.5, 0.18, 0.36), 0.2),
                                (27.6, 0.2, 41.2, (0.5, 0.18, 0.4), 0.4), (20.5, 0.2, 43.0, (0.45, 0.18, 0.35), 1.1)):
        k.cbox("block", (x, y, z), half, c=0.08, rot=euler(ry, 0.1, 0.08), chip=0.7)
    k.rubble(23.8, 42.6, 4.2, 44, size=(0.08, 0.18), seed=33, squash=(1.0, 0.62),
             avoid=lambda x, z: x < 19.7 or z > 45.6 or z < 40.2 or x > 28.8)
    k.beam("char", (22.2, 0.5, 44.8), (25.4, 2.0, 46.0), 0.3, 0.3)
    k.beam("char", (24.9, 0.25, 43.6), (27.4, 1.9, 45.6), 0.26, 0.26)
    k.beam("wood", (20.4, 0.14, 41.6), (23.4, 0.18, 42.5), 0.22, 0.22)
    S.fire((26.6, 2.3, 45.9), 1.1, intensity=5.0, radius=11, shadows=True)
    P.ram(k, 21.9, 45.1, yaw=1.25)
    P.body(k, 20.4, 41.0, 0.7, seed=4)
    P.body(k, 22.9, 40.6, 2.2, seed=5)
    P.body(k, 26.4, 40.9, 4.0, seed=6)
    P.shield_flat(k, 21.4, 0.02, 40.7, 0.3)
    P.arrows(k, 21.6, 0.0, 41.9, 3, seed=8, spread=0.3)
    # ================= the fissure =================
    for i, (xa, xb) in enumerate(((9.6, 13.4), (14.2, 17.6), (18.8, 22.9), (23.6, 28.4))):
        ya, yb = -0.34 - 0.1 * hashf(i, 1), -0.3 - 0.25 * hashf(i, 2)
        k.beam("conduit", (xa, ya, 38.78 + 0.08 * hashf(i, 3)), (xb, yb, 38.8 - 0.08 * hashf(i, 4)), 0.42, 0.42)
        k.beam("conduit", (xb, yb, 38.8), (xb + 0.5, yb - 1.3, 39.1), 0.4, 0.4)
    # Aetherite grown up through the crack: shards that just clear the floor
    for i, x in enumerate((10.3, 12.2, 15.2, 16.4, 18.3, 20.9, 23.2, 24.1, 26.3, 28.0)):
        zz = 39.0 + 0.5 * (hashf(i, 21) - 0.5)
        shard(k, x, -1.2, zz, 1.25 + 0.35 * hashf(i, 22), 0.2, lean=(0.3 * (hashf(i, 23) - 0.5), 0.2 * (hashf(i, 24) - 0.5)),
              seed=i)
        shard(k, x + 0.3, -1.0, zz + 0.25, 0.8 + 0.3 * hashf(i, 25), 0.13, lean=(0.2, -0.1), seed=i + 2)
    for i, x in enumerate((10.6, 13.0, 16.1, 19.4, 21.8, 25.0, 27.6)):
        k.cbox("floor", (x, -0.95 - 0.5 * hashf(i, 7), 39.0 + 0.3 * (hashf(i, 8) - 0.5)),
               (0.7 + 0.3 * hashf(i, 5), 0.12, 0.5), c=0.03, rot=euler(hashf(i, 3) * 3, 0.5 * (hashf(i, 4) - 0.5),
                                                                  0.7 * (hashf(i, 6) - 0.5)), bottom=True, chip=0.5)
        shard(k, x + 0.9, -2.4, 38.9, 1.0 + 0.6 * hashf(i, 9), 0.2, lean=(0.2 * (hashf(i, 2) - 0.5), 0.15), seed=i)
        shard(k, x + 1.3, -2.4, 39.3, 0.6 + 0.4 * hashf(i, 10), 0.14, lean=(0.1, -0.1), seed=i + 3)
    # broken lips: slabs sagging into the gap along both edges
    for i, x in enumerate(np.arange(9.4, 28.8, 0.9)):
        for sgn, zz in ((1, 38.62), (-1, 39.42)):
            if hashf(i, sgn, 2.2) < 0.25:
                continue
            k.cbox("floor", (x + 0.2 * hashf(i, sgn), -0.16 - 0.1 * hashf(i, sgn, 5), zz + 0.1 * (hashf(i, 7) - 0.5)),
                   (0.42, 0.07, 0.3), c=0.02, rot=euler(0.3 * (hashf(i, sgn, 9) - 0.5), sgn * (0.35 + 0.3 * hashf(i, sgn, 3)),
                                                         0.2 * (hashf(i, 4) - 0.5)), bottom=True, chip=0.6,
                   col=(0.75, 0.72, 0.8))
    for x in (11.5, 17.0, 22.5, 26.5):
        S.light((x, -0.35, 39.2), VIOLET_COL, 4.2, 6.0, flicker=0.12, speed=2.0,
                curve=[[0, 0.8], [1.3, 1.15], [2.9, 0.8]])
        S.emitter((x, -1.2, 39.0), sprite="fx/fortress/mote.anim.json", rate=5, lifetime=3.0, lifetime_jitter=1.0,
                  velocity=[0.05, 0.7, 0], velocity_jitter=[0.25, 0.3, 0.2], area=[2.2, 0.3, 0.5], size=0.05,
                  size_end=0.02, color=[1.6, 0.9, 2.6, 1], color_end=[0.9, 0.3, 2.0, 0], blend="additive", max=48)
    for x in (14.5, 20.3, 24.4):
        S.embers((x, -0.6, 39.05), 0.8, rate=5)
        S.light((x, -0.7, 39.0), FIRE_COL, 2.4, 5.0)
    # the branch towards the gate
    for i, zz in enumerate((40.9, 42.4, 43.7, 45.2)):
        shard(k, 18.75 + 0.3 * (hashf(i, 31) - 0.5), -1.2, zz, 1.25 + 0.3 * hashf(i, 32), 0.19,
              lean=(0.2 * (hashf(i, 33) - 0.5), 0.2), seed=i)
        k.cbox("floor", (18.35 + 0.8 * (i % 2), -0.18, zz + 0.5), (0.3, 0.07, 0.4), c=0.02,
               rot=euler(0.4 * hashf(i, 34), 0.2, (0.5 if i % 2 == 0 else -0.5)), bottom=True, chip=0.6,
               col=(0.75, 0.72, 0.8))
    k.beam("conduit", (18.75, -0.4, 40.3), (18.8, -0.6, 44.6), 0.4, 0.4)
    S.light((18.75, -0.35, 43.0), VIOLET_COL, 3.6, 5.5, flicker=0.12, speed=2.0, curve=[[0, 0.8], [1.5, 1.15], [3.1, 0.8]])
    S.emitter((18.75, -1.0, 43.0), sprite="fx/fortress/mote.anim.json", rate=4, lifetime=3.0, lifetime_jitter=1.0,
              velocity=[0.05, 0.7, 0], velocity_jitter=[0.2, 0.3, 0.25], area=[0.4, 0.3, 2.6], size=0.05,
              size_end=0.02, color=[1.6, 0.9, 2.6, 1], color_end=[0.9, 0.3, 2.0, 0], blend="additive", max=48)
    # ================= the defenders' barricade (north of fight 2) =================
    P.palisade(k, 9.3, 32.35, 11.6, 32.25, 1.9, seed=1, burnt=0.25)
    P.cart(k, 13.2, 0.0, 31.9, yaw=0.15, roll=1.25, char=True)
    P.cheval(k, 15.7, 32.2, 2.2, 0.05)
    P.palisade(k, 17.3, 32.3, 20.4, 32.4, 1.9, seed=2, burnt=0.6)
    P.mantlet(k, 8.4, 32.1, 0.2)
    S.fire((13.4, 0.7, 31.8), 1.1, intensity=4.6, radius=10.5, shadows=True)
    S.fire((19.6, 1.2, 32.3), 0.7, intensity=3.0, radius=7.5)
    for (x, z, s_, yaw) in ((10.2, 31.2, 0.7, 0.1), (10.95, 31.3, 0.6, 0.5), (10.5, 31.25, 0.55, 0.3),
                            (17.8, 31.3, 0.7, 0.2), (18.6, 31.2, 0.6, 0.7), (16.3, 31.0, 0.55, 1.0)):
        P.crate(k, x, 0.7 if (x == 10.5) else 0.0, z, s_, yaw)
    P.barrel(k, 11.8, 0, 31.2)
    P.barrel(k, 19.5, 0, 31.1)
    P.barrel(k, 14.6, 0, 30.9, lying=True, yaw=1.0)
    P.sack_pile(k, 12.3, 0, 30.6, 5, seed=5)
    # behind the barricade: a dry fountain, a toppled statue, the dead
    P.fountain(k, 14.4, 29.2, 1.7)
    P.statue(k, 9.2, 30.4, yaw=-0.2)
    P.body(k, 11.6, 27.6, 0.4, seed=7)
    P.body(k, 17.6, 28.2, 3.6, seed=8)
    P.body(k, 19.6, 29.6, 1.2, seed=9)
    P.shield_flat(k, 16.4, 0.02, 27.5, 0.5)
    P.spear(k, 18.4, 0.02, 27.4, 0.3, 0.0, 2.0)
    P.spear(k, 12.4, 0.0, 28.4, 2.0, 1.0, 1.6)
    P.arrows(k, 17.2, 0.0, 30.2, 4, seed=9, spread=0.5)
    k.rubble(14.0, 28.0, 6.5, 30, size=(0.05, 0.11), seed=35, squash=(1.0, 0.16),
             avoid=lambda x, z: S.near_walk(x, z, 0.15))
    debris_fire(S, 18.8, 29.0, 0.6, seed=12, intensity=2.8, radius=7)
    debris_fire(S, 9.9, 28.4, 0.55, seed=13, intensity=2.6, radius=6.5)
    # the hoist and the Aetherite stores (west, off the path)
    top = P.crane(k, 5.3, 29.9, h=5.2, seed=2)
    S.fire(top, 1.5, intensity=5.5, radius=13)
    for (x, z, s_, yaw, yy) in ((3.6, 27.3, 0.8, 0.1, 0), (4.45, 27.25, 0.7, 0.4, 0), (3.9, 27.3, 0.6, 0.9, 0.8),
                                (7.4, 28.2, 0.75, 0.3, 0), (7.6, 29.2, 0.6, 1.1, 0)):
        P.crate(k, x, yy, z, s_, yaw)
    for i, (x, z) in enumerate(((7.3, 30.6), (7.9, 30.9), (7.5, 31.2), (8.2, 30.4), (6.9, 31.0))):
        shard(k, x, 0.0, z, 0.5 + 0.5 * hashf(i, 3), 0.13, lean=(0.2 * (hashf(i, 1) - 0.5), 0.2 * (hashf(i, 2) - 0.5)),
              seed=i)
    P.crate(k, 7.6, 0, 30.8, 0.9, 0.5, char=True)
    S.light((7.6, 0.9, 30.9), VIOLET_COL, 2.6, 5.0, flicker=0.1, speed=2.0, curve=[[0, 0.85], [1.7, 1.15], [3.1, 0.85]])
    # ================= the NW ruin: a burning shell =================
    for (a, b) in (((4.0, 0.3, 21.4), (8.6, 2.6, 24.8)), ((9.6, 0.2, 21.2), (5.2, 3.3, 22.8)),
                   ((4.4, 0.2, 24.9), (9.9, 1.4, 23.6)), ((6.8, 0.2, 20.9), (7.4, 4.0, 22.2)),
                   ((3.9, 4.4, 23.4), (7.2, 2.2, 23.0))):
        k.beam("char", a, b, 0.28, 0.28)
    k.mound("rock", 7.0, 23.0, 3.2, 2.3, 0.9, seed=9, res=0.6, col=(0.6, 0.55, 0.55))
    S.fire((6.6, 0.8, 22.8), 1.9, intensity=4.6, radius=13, shadows=True)
    S.fire((9.4, 0.5, 24.4), 0.9, intensity=3.0, radius=7)
    P.arch(k, 6.0, 7.5, 2.2, 25.75, depth=0.5, blocks=5, thick=0.4, missing=(3,))
    k.rubble(8.6, 26.6, 1.6, 14, size=(0.08, 0.2), seed=36, squash=(1.2, 0.3),
             avoid=lambda x, z: S.near_walk(x, z, 0.2))
    k.rubble(10.4, 26.4, 0.8, 8, size=(0.1, 0.24), seed=37, avoid=lambda x, z: S.near_walk(x, z, 0.2))
    # ================= the east side =================
    P.cart(k, 27.5, 0.0, 31.0, yaw=1.3, roll=1.2, char=True, load=False)
    S.fire((27.4, 0.6, 30.8), 1.0, intensity=3.4, radius=8.5)
    for (x, z, s_, yaw, yy) in ((27.2, 35.0, 0.85, 0.1, 0), (28.05, 35.1, 0.7, 0.5, 0), (27.5, 35.05, 0.6, 0.8, 0.85),
                                (27.3, 36.0, 0.7, 1.2, 0), (28.2, 36.1, 0.6, 0.2, 0)):
        P.crate(k, x, yy, z, s_, yaw)
    P.barrel(k, 26.6, 0, 35.7)
    P.barrel(k, 26.55, 0, 34.4, lying=True, yaw=0.3)
    P.spear(k, 27.9, 0.0, 33.9, 1.2, 1.25, 2.1)
    P.spear(k, 28.1, 0.0, 34.0, 1.5, 1.2, 2.0)
    P.weapon_rack(k, 28.5, 0, 26.2, math.pi / 2)
    P.crate(k, 27.0, 0, 27.4, 0.7, 0.3)
    P.crate(k, 27.75, 0, 27.6, 0.6, 0.8)
    P.barrel(k, 26.6, 0, 28.3)
    P.barrel(k, 28.3, 0, 33.2)
    P.sack_pile(k, 26.7, 0, 32.9, 4, seed=7)
    P.body(k, 26.7, 37.4, 0.9, seed=10)
    P.cart(k, 27.2, 0.0, 23.6, yaw=1.45, roll=0.0, load=True)
    for z in (25.2, 33.0, 37.2):
        pos = P.sconce(k, 28.98, 2.3, z, -1, 0)
        S.fire(pos, 0.5, smoke=False, intensity=2.4, radius=7.0, stain=False)
    k.rubble(27.6, 28.9, 1.6, 20, size=(0.1, 0.3), seed=38, squash=(0.8, 1.3))   # fallen from the breach above
    k.cbox("block", (27.9, 0.4, 29.4), (0.6, 0.4, 0.5), c=0.08, rot=euler(0.6, 0.2, 0.1), chip=0.7)
    S.fire((30.4, 3.4, 28.8), 1.3, intensity=4.5, radius=11)
    # ================= foot of the stair and the terrace front =================
    for x in (12.5, 19.5):
        pos = P.brazier(k, x, 1.5, 22.5, 0.95)
        S.fire(pos, 0.95, intensity=3.4, radius=9.5, shadows=(x < 13), stain=False, smoke=False, force=True)
    for i, (x, z) in enumerate(((20.6, 20.6), (21.3, 20.7), (22.4, 20.55))):
        P.barrel(k, x, 0, z, 1.0 - 0.1 * i)
    P.crate(k, 24.6, 0, 20.7, 0.8, 0.2)
    P.crate(k, 25.5, 0, 20.65, 0.65, 0.6)
    P.crate(k, 24.9, 0.8, 20.7, 0.55, 0.4)
    P.sack_pile(k, 27.6, 0, 20.9, 5, seed=11)
    P.body(k, 23.3, 21.6, 2.6, seed=11)
    P.ladder(k, (26.6, 0.0, 21.6), (26.9, 3.3, 20.2), missing=(2, 5))
    for x in (21.9, 25.0, 28.0):
        pos = P.sconce(k, x, 1.75, 20.02, 0, 1)
        S.fire(pos, 0.45, smoke=False, intensity=2.0, radius=6.0, stain=False)
    P.body(k, 11.5, 22.4, 1.0, seed=12)
    # ================= the terrace and landing =================
    for (x, z) in ((10.4, 8.7), (21.6, 8.7), (10.4, 13.3), (21.6, 13.3)):
        k.cbox("block", (x, 3.3, z), (0.45, 0.3, 0.45), c=0.06)
        pos = P.brazier(k, x, 3.6, z, 0.9)
        S.fire(pos, 0.9, intensity=3.4, radius=9, stain=False, shadows=(x < 11 and z < 9), smoke=False, force=True)
    for x, br in ((5.0, None), (8.0, 2.4), (24.0, None), (27.0, 3.4)):
        P.pillar(k, x, 11.0, 3.0, 5.6, 0.9, broken=br)
    k.cbox("block", (8.9, 3.4, 12.6), (0.45, 0.4, 1.3), c=0.05, rot=euler(0.5, 0, 0.0), chip=0.5)
    k.cbox("block", (26.0, 3.35, 12.9), (0.45, 0.35, 0.9), c=0.05, rot=euler(-0.9, 0, 0.0), chip=0.5)
    P.statue(k, 5.6, 16.0, 3.0, fallen=False, yaw=0.0)
    P.statue(k, 26.4, 16.2, 3.0, fallen=True, yaw=2.6)
    P.ballista(k, 24.3, 3.0, 17.4, yaw=-math.pi / 2)
    P.body(k, 9.7, 10.6, 0.4, y=3.0, seed=13)
    P.body(k, 22.6, 11.8, 2.2, y=3.0, seed=14)
    P.body(k, 21.8, 15.6, 5.0, y=3.0, seed=15)
    for (x, z, s_) in ((4.0, 18.6, 0.8), (4.8, 18.7, 0.65), (4.3, 18.6, 0.6)):
        P.crate(k, x, 3.0 + (0.8 if s_ == 0.6 else 0), z, s_, 0.3 * s_)
    P.barrel(k, 9.2, 3.0, 18.4)
    P.barrel(k, 9.9, 3.0, 18.7, 0.9)
    k.rubble(7.0, 14.0, 3.0, 16, size=(0.08, 0.22), y=3.0, seed=39)
    k.rubble(25.0, 14.0, 3.0, 16, size=(0.08, 0.22), y=3.0, seed=40)
    debris_fire(S, 7.2, 15.4, 1.0, y=3.0, seed=3, intensity=4.2, radius=10)
    debris_fire(S, 24.0, 9.6, 0.8, y=3.0, seed=4, intensity=3.6, radius=9)
    # ================= fires on the wall tops (the skyline) =================
    debris_fire(S, 1.4, 26.0, 1.3, y=6.0, seed=5, intensity=5.0, radius=13)
    debris_fire(S, 1.5, 15.5, 1.0, y=6.0, seed=6, intensity=4.0, radius=11)
    debris_fire(S, 30.4, 19.5, 1.2, y=6.0, seed=7, intensity=4.6, radius=12)
    debris_fire(S, 5.0, 6.0, 1.6, y=12.0, seed=8, intensity=6.0, radius=15)
    debris_fire(S, 27.6, 6.0, 1.3, y=12.0, seed=9, intensity=5.0, radius=13)


# ---------------------------------------------------------------------------- lighting
def lighting(S):
    pulse = [[0, 0.82], [2.2, 1.18], [4.6, 0.82]]
    cx, cy, cz = 16.0, 46.0, 4.0
    # the crystal itself, and its light falling down the tower's south face
    S.light((cx, 43.5, cz + 1.0), VIOLET_COL, 8.0, 46.0, flicker=0.08, speed=1.5, curve=pulse)
    S.light((cx, 36.0, 17.0), VIOLET_COL, 2.6, 28.0, flicker=0.06, speed=1.5, curve=pulse)
    S.light((cx - 9.5, 22.0, 8.5), VIOLET_COL, 2.8, 14.0, flicker=0.06, speed=1.5, curve=pulse)
    S.light((cx + 9.5, 22.0, 8.5), VIOLET_COL, 2.8, 14.0, flicker=0.06, speed=1.5, curve=pulse)
    # the chamber's glow through the great door
    S.light((16.0, 5.2, 9.0), VIOLET_COL, 5.5, 8.5, flicker=0.1, speed=2.0, curve=[[0, 0.8], [1.6, 1.2], [3.4, 0.8]])
    # halo and motes streaming up to the crystal
    S.emitter((cx, cy, cz), sprite="fx/fortress/glow.anim.json", rate=0.5, lifetime=6.0, lifetime_jitter=1.5,
              velocity=[0, 0, 0], velocity_jitter=[0.2, 0.2, 0.2], area=[0.5, 1.0, 0.5], size=15.0, size_end=21.0,
              color=[0.5, 0.26, 1.0, 0.0], color_end=[0.3, 0.12, 0.7, 0.0], blend="additive", max=8)
    S.emitter((cx, cy, cz), sprite="fx/fortress/glow.anim.json", rate=0.6, lifetime=4.0, lifetime_jitter=1.0,
              velocity=[0, 0, 0], velocity_jitter=[0.3, 0.3, 0.3], area=[0.5, 2.0, 0.5], size=9.0, size_end=12.0,
              color=[0.42, 0.22, 0.9, 0.5], color_end=[0.25, 0.1, 0.6, 0.0], blend="additive", max=8)
    tx, ty, tz = S.tower_top
    S.emitter((tx, ty, tz), sprite="fx/fortress/mote.anim.json", rate=26, lifetime=3.4, lifetime_jitter=1.0,
              velocity=[0, 2.4, 0], velocity_jitter=[0.9, 0.8, 0.9], area=[1.2, 0.3, 1.2], size=0.16, size_end=0.05,
              color=[1.6, 0.9, 2.8, 1], color_end=[1.0, 0.4, 2.4, 0], blend="additive", max=128)
    S.emitter((16.0, 4.0, 7.6), sprite="fx/fortress/mote.anim.json", rate=7, lifetime=2.6, lifetime_jitter=0.8,
              velocity=[0, 0.5, 0.5], velocity_jitter=[0.5, 0.4, 0.3], area=[1.4, 1.0, 0.2], size=0.05, size_end=0.02,
              color=[1.6, 0.9, 2.8, 1], color_end=[1.0, 0.4, 2.4, 0], blend="additive", max=48)
    # ash drifting down over the yard and the bridge
    for (x, z, ax, az) in ((10, 40, 7, 7), (22, 40, 7, 7), (10, 28, 7, 6), (22, 28, 7, 6), (16, 16, 8, 6),
                           (16, 58, 6, 9)):
        S.emitter((x, 7.5, z), sprite="fx/fortress/ash.anim.json", rate=9, lifetime=9.0, lifetime_jitter=2.0,
                  velocity=[0.35, -0.75, 0.1], velocity_jitter=[0.3, 0.2, 0.3], area=[ax, 0.5, az], size=0.05,
                  size_end=0.04, color=[0.75, 0.72, 0.8, 0.9], color_end=[0.6, 0.55, 0.65, 0.0], blend="alpha",
                  unlit=False, max=128)
    # mist breathing up out of the chasm around the bridge
    for (x, z) in ((6, 56), (26, 56), (5, 66), (27, 66), (16, 76)):
        S.emitter((x, -5.5, z), sprite="fx/fortress/mist.anim.json", rate=0.7, lifetime=16.0, lifetime_jitter=3.0,
                  velocity=[0.25, 0.12, 0.0], velocity_jitter=[0.2, 0.08, 0.2], area=[5.0, 1.0, 5.0], size=3.2,
                  size_end=5.5, color=[0.5, 0.56, 0.95, 0.0], color_end=[0.5, 0.56, 0.95, 0.0], blend="alpha", max=16)


# ---------------------------------------------------------------------------- environment
def environment(S):
    sky = "maps/fortress/"
    return {
        "ambient": [0.062, 0.068, 0.108],
        "sun_color": [0.25, 0.3, 0.46],
        "sun_direction": [0.52, -0.62, -0.58],
        "fog_color": [0.017, 0.023, 0.062],
        "fog_density": 0.05,
        "fog_height": -9.0,
        "fog_height_falloff": 0.22,
        "fog_max": 0.97,
        "shadows": True,
        "shadow_distance": 34.0,
        "shadow_strength": 0.8,
        "shadow_bias": 0.03,
        "sprite_light_height": 0.8,
        "anisotropic": True,
        "pixel_snap": False,
        "sky": {
            "mode": "gradient",
            "top": [0.0037, 0.005, 0.018],
            "horizon": [0.02, 0.03, 0.1],
            "bottom": [0.012, 0.016, 0.05],
            "cloud_cover": 0.62,
            "cloud_speed": 0.006,
            "cloud_color": [0.034, 0.03, 0.075],
            "backdrops": [
                {"image": sky + "sky_glow.png", "distance": 46, "yaw": 0, "width": 130, "height": 130,
                 "elevation": -19, "parallax": 1.0, "tint": [0.5, 0.36, 1.0, 0.7]},
                {"image": sky + "sky_ridge.png", "distance": 200, "yaw": -56, "width": 560, "height": 105,
                 "elevation": -44, "parallax": 1.0, "tint": [1.2, 1.2, 1.2, 1]},
                {"image": sky + "sky_ridge.png", "distance": 200, "yaw": 58, "width": 560, "height": 105,
                 "elevation": -42, "parallax": 1.0, "tint": [1.2, 1.2, 1.2, 1]},
                {"image": sky + "sky_ridge.png", "distance": 190, "yaw": 0, "width": 560, "height": 105,
                 "elevation": -40, "parallax": 1.0, "tint": [1.3, 1.3, 1.3, 1]},
                {"image": sky + "sky_smoke.png", "distance": 150, "yaw": -22, "width": 120, "height": 120,
                 "elevation": 0, "parallax": 1.0, "tint": [1.2, 1.1, 1.1, 0.9]},
                {"image": sky + "sky_smoke.png", "distance": 146, "yaw": 25, "width": 110, "height": 110,
                 "elevation": 2, "parallax": 1.0, "tint": [1.2, 1.1, 1.1, 0.85]},
                {"image": sky + "sky_skyline.png", "distance": 136, "yaw": -52, "width": 330, "height": 79,
                 "elevation": -23, "parallax": 1.0, "tint": [1.5, 1.4, 1.4, 1]},
                {"image": sky + "sky_skyline.png", "distance": 136, "yaw": 54, "width": 330, "height": 79,
                 "elevation": -22, "parallax": 1.0, "tint": [1.5, 1.4, 1.4, 1]},
                {"image": sky + "sky_skyline.png", "distance": 130, "yaw": 0, "width": 330, "height": 79,
                 "elevation": -22, "parallax": 1.0, "tint": [1.7, 1.5, 1.5, 1]},
            ],
        },
        "weather": {"kind": "embers", "intensity": 1.3, "wind": [0.5, 0.1, 0.15], "color": [1, 1, 1, 1],
                    "sprite": "fx/fortress/ember.anim.json"},
        "post": {
            "bloom": True, "bloom_strength": 0.48, "bloom_threshold": 0.9, "bloom_knee": 0.6, "bloom_radius": 0.8,
            "bloom_levels": 6,
            "depth_of_field": True, "dof_mode": "tilt", "focus": 0.56, "focus_range": 0.3, "aperture": 1.2,
            "dof_radius": 7,
            "ssao": True, "ssao_radius": 0.7, "ssao_strength": 0.9, "ssao_bias": 0.03,
            "vignette": True, "vignette_strength": 0.6,
            "exposure": 1.15, "tonemap": "aces",
            "color_grade": True, "lut": sky + "grade.cube",
            "antialias": "auto", "distortion": 1.0, "protect_sprites": True,
        },
    }


# ---------------------------------------------------------------------------- assembly
def build_scene():
    S = Scene()
    make_grid(S)
    terrain(S)
    bridge(S)
    cliffs(S)
    mist_sheets(S)
    for name in ("architecture", "tower", "dressing", "lighting"):
        fn = globals().get(name)
        if fn:
            fn(S)
    finish(S)
    flagged = [f for f in S.fire_log if f[4]]
    print("fires in the field camera's foreground (no smoke; 'near' ones only smoulder):", flagged)
    return S


def finish(S):
    """Vertex colour passes over everything: soot around the fires, broad
    variation, darker ruined tops in the foreground."""
    stains = np.array(S.stains, dtype=np.float64) if S.stains else np.zeros((0, 5))
    for mat, faces in S.kit.faces.items():
        if mat in ("mist",):
            continue
        for (P_, UV, C, N) in faces:
            c = P_.mean(0)
            if len(stains):
                near = stains[(np.abs(stains[:, 0] - c[0]) < stains[:, 3] + 1) & (np.abs(stains[:, 2] - c[2]) < stains[:, 3] + 1)
                              & (np.abs(stains[:, 1] - c[1]) < stains[:, 3] + 1)]
                for (x, y, z, r, s) in near:
                    d = np.sqrt(((P_ - np.array([x, y, z])) ** 2).sum(1))
                    f = np.clip(1 - d / r, 0, 1)
                    C *= (1 - s * f * f)[:, None]
            if mat in ("wall", "floor", "block", "rock"):
                C *= 0.9 + 0.2 * hashf(math.floor(c[0] / 3.0), math.floor(c[1] / 3.0), math.floor(c[2] / 3.0))
            # the stumps of the south wall are the foreground of every yard shot: keep them dark
            if mat in ("block", "wall", "rock") and c[1] > 0.12 and (
                    (46.0 < c[2] < 50.2 and 3 < c[0] < 22.5) or (40.0 < c[2] < 47.5 and 19.6 < c[0] < 29.0)):
                C *= 0.6
            elif mat in ("block", "wall") and c[1] > 0.4 and c[2] > 33.0 and (c[0] < 3.02 or c[0] > 28.98) and \
                    abs(N[0][1]) > 0.5:
                C *= 0.7
