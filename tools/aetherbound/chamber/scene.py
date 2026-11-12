"""Geometry of the two maps, from shared parts: `build(ruined)` returns a
kit.Mesh in engine coordinates plus the facts map.json needs (blocked cells,
where fires burn, where the machinery glows)."""
import math
import numpy as np
from kit import (Mesh, surf, floor_surf, plane, quad, box, obox, rotation, revolve, cylinder, disc, tube, arc_points, vec)
from textures import band
from layout import (CX, CZ, RADIUS, WALL, DX, DZ, DAIS, DAIS_TOP, PILLAR_R, PILLAR_ANGLES, STUMP_ANGLES, PASSAGE,
                    BRAZIERS, polar, CRATER_R, crater_depth)

TOP = 18.0   # the height at which the chamber's walls are lost in the dark


def shaft_dark(P):
    """The tower's shaft goes up into darkness: everything fades above the firelight."""
    y = P[:, 1]
    k = np.clip(1.0 - (y - 5.0) / 10.0, 0.0, 1.0)
    shade = (0.1 + 0.9 * k * k * (3 - 2 * k)) * np.clip(1.0 + y / 6.5, 0.05, 1.0)
    return np.stack([shade * 0.96, shade * 0.97, shade * 1.0 + 0.02], axis=1)


def moonlit(P):
    """The ruins lie open to the sky; only what sinks below the floor is lost."""
    y = P[:, 1]
    shade = np.clip(1.0 + y / 4.0, 0.15, 1.0) * np.clip(1.12 - y / 15.0, 0.42, 1.0)   # tall stumps stand dark against the sky
    return np.stack([shade, shade, shade], axis=1)


class Builder:
    def __init__(self, ruined, seed=3):
        self.m = Mesh()
        self.ruined = ruined
        self.rng = np.random.default_rng(seed + (100 if ruined else 0))
        self.tint = moonlit if ruined else shaft_dark
        self.blocked = set()      # cells nothing may walk on
        self.fires = []           # (x, y, z) of each brazier's flame
        self.info = {}

    def block(self, x0, z0, x1, z1):
        for x in range(int(math.floor(x0)), int(math.ceil(x1))):
            for z in range(int(math.floor(z0)), int(math.ceil(z1))):
                self.blocked.add((x, z))

    def block_disc(self, x, z, r):
        for cx in range(int(x - r) - 1, int(x + r) + 2):
            for cz in range(int(z - r) - 1, int(z + r) + 2):
                nx, nz = min(max(x, cx), cx + 1), min(max(z, cz), cz + 1)
                if math.hypot(nx - x, nz - z) < r:
                    self.blocked.add((cx, cz))

    # ------------------------------------------------------------------ ring wall

    def ring_face(self, name, r, a0, a1, y0, y1, inward=True, clip=None, ds=1.0, ppu=16):
        length = math.radians(a1 - a0) * RADIUS
        sign = -1.0 if inward else 1.0

        def fn(S, T):
            a = math.radians(a0) + S / RADIUS
            c, s = np.cos(a), np.sin(a)
            P = np.stack([CX + r * c, y0 + T, CZ - r * s], axis=-1)
            return P, np.stack([sign * c, np.zeros_like(c), -sign * s], axis=-1)
        cut = (lambda S: clip(a0 + np.degrees(S / RADIUS)) - y0) if clip else None
        surf(self.m, band(name, ppu), fn, 0, length, 0, y1 - y0, ds=ds, dt=1.0, u0=math.radians(a0) * RADIUS,
             clip=cut, tint=self.tint)

    def ring_top(self, name, r0, r1, a0, a1, height, ds=1.0, down=False):
        length = math.radians(a1 - a0) * RADIUS

        def fn(S, T):
            a = math.radians(a0) + S / RADIUS
            r = r0 + T
            y = height(a0 + np.degrees(S / RADIUS)) if callable(height) else np.full_like(a, height)
            return np.stack([CX + r * np.cos(a), y, CZ - r * np.sin(a)], axis=-1), vec(0, -1 if down else 1, 0)
        surf(self.m, band(name), fn, 0, length, 0, r1 - r0, ds=ds, dt=1.0, u0=math.radians(a0) * RADIUS, tint=self.tint)

    def ring_cap(self, a, y0, y1, facing):
        """A radial face closing a wall's end at angle a between two heights; facing 1 looks counter-clockwise."""
        if y1 - y0 < 0.05:
            return
        x, z = polar(RADIUS - 0.18, a)
        c, s = math.cos(math.radians(a)), math.sin(math.radians(a))
        n = (-s * facing, 0, -c * facing)
        quad(self.m, band("rough"), (x, y0, z), (c, 0, -s), (0, 1, 0), WALL + 0.18, y1 - y0, n, tint=self.tint)

    def ring_run(self, a0, a1, h, dressed=True, ds=1.0):
        """A stretch of the ring wall of one height: plinth, ashlar, a string course, dark brick above."""
        self.ring_face("big", RADIUS - 0.18, a0, a1, 0, min(1.6, h), ds=ds)
        if h > 1.6:
            self.ring_top("mould", RADIUS - 0.18, RADIUS, a0, a1, 1.6, ds=ds)
            self.ring_face("ashlar", RADIUS, a0, a1, 1.6, min(9.0, h), ds=ds)
        if h > 9.0:
            self.ring_face("trim", RADIUS - 0.14, a0, a1, 9.0, min(10.0, h), ds=ds)
            self.ring_top("mould", RADIUS - 0.14, RADIUS, a0, a1, 9.0, ds=ds, down=True)
            if h > 10.0:
                self.ring_top("mould", RADIUS - 0.14, RADIUS, a0, a1, 10.0, ds=ds)
                self.ring_face("brick", RADIUS, a0, a1, 10.0, h, ds=ds)
        if dressed:
            self.ring_top("rough", RADIUS - 0.18, RADIUS + WALL, a0, a1, h, ds=ds)
            self.ring_face("brick", RADIUS + WALL, a0, a1, -3.0, h, inward=False, ds=ds)

    def ring_wall(self, edges, heights, dressed=True, ds=1.0):
        """The ring wall as columns: edges are n + 1 angles, heights the n column heights between them.
        Broken masonry steps down in whole courses, so a ragged crown is a staircase of columns."""
        edges, heights = list(edges), list(heights)
        i = 0
        while i < len(heights):
            j = i
            while j + 1 < len(heights) and abs(heights[j + 1] - heights[i]) < 1e-6:
                j += 1
            h = heights[i]
            before = heights[i - 1] if i > 0 else 0.0
            after = heights[j + 1] if j + 1 < len(heights) else 0.0
            if h > 0.05:
                self.ring_run(edges[i], edges[j + 1], h, dressed, ds)
                self.ring_cap(edges[i], min(before, h), h, -1)
                self.ring_cap(edges[j + 1], min(after, h), h, 1)
            i = j + 1

    # ------------------------------------------------------------------ parts

    def tall_box(self, lo, hi, faces="nsew", plinth=0.18):
        """Masonry mass: a plinth of big blocks, ashlar, then dark brick above."""
        x0, y0, z0 = lo
        x1, y1, z1 = hi
        m, t = self.m, self.tint
        box(m, band("big"), (x0 - plinth, y0, z0 - plinth), (x1 + plinth, min(y1, y0 + 1.6), z1 + plinth), faces + "t",
            top=band("mould"), tint=t)
        if y1 > y0 + 1.6:
            box(m, band("ashlar"), (x0, y0 + 1.6, z0), (x1, min(y1, 9.0), z1), faces, tint=t)
        if y1 > 9.0:
            box(m, band("trim"), (x0 - 0.14, 9.0, z0 - 0.14), (x1 + 0.14, min(10.0, y1), z1 + 0.14), faces, tint=t)
        if y1 > 10.0:
            box(m, band("brick"), (x0, 10.0, z0), (x1, y1, z1), faces, tint=t)

    def pillar(self, x, z, height=11.0, broken=None):
        m, t, rng = self.m, self.tint, self.rng
        box(m, band("big"), (x - 0.85, 0, z - 0.85), (x + 0.85, 0.45, z + 0.85), "nsewt", top=band("rough"), tint=t)
        revolve(m, band("mould"), (x, 0, z), [(0.82, 0.45), (0.82, 0.6), (0.66, 0.8), (0.6, 0.86)], u_units=5, seg=12, tint=t)
        self.block(x - 0.85, z - 0.85, x + 0.85, z + 0.85)
        if broken is not None:
            jag = broken + rng.uniform(-0.35, 0.35, 13)
            jag[-1] = jag[0]

            def cut(S):
                return np.interp(S, np.linspace(0, 4, 13), jag) - 0.86
            revolve(m, band("flute"), (x, 0, z), [(0.6, 0.86), (0.6, broken + 0.4)], u_units=4, seg=12, clip=cut, tint=t)
            disc(m, band("rough"), (x, broken - 0.3, z), 0, 0.6, seg=12, tint=t)
            return
        revolve(m, band("flute"), (x, 0, z), [(0.6, 0.86), (0.6, height)], u_units=4, seg=12, tint=t)
        revolve(m, band("trim"), (x, 0, z), [(0.6, height), (0.7, height + 0.12), (0.95, height + 0.5), (0.95, height + 0.66)],
                u_units=6, seg=12, tint=t)
        box(m, band("mould"), (x - 1.05, height + 0.66, z - 1.05), (x + 1.05, height + 1.1, z + 1.05), "nsewb", tint=t)
        box(m, band("ashlar"), (x - 0.8, height + 1.1, z - 0.8), (x + 0.8, TOP, z + 0.8), "nsew", tint=t)

    def brazier(self, x, z, lit=True, fallen=False):
        m, t = self.m, self.tint
        if fallen:
            rot = rotation(yaw=self.rng.uniform(0, 360), roll=90)
            centre = vec(x, 0.5, z)
            revolve(m, band("iron", 32), centre, [(0.14, -0.6), (0.14, 0.2), (0.42, 0.4), (0.55, 0.62)], seg=10, axis=rot,
                    u_units=2, tint=t)
            revolve(m, band("iron", 32), centre, [(0.5, 0.6), (0.38, 0.38)], seg=10, axis=rot, u_units=2, inward=True, tint=t)
            return
        revolve(m, band("iron", 32), (x, 0, z), [(0.46, 0.0), (0.42, 0.1), (0.16, 0.22), (0.11, 0.3), (0.11, 0.86), (0.16, 0.92)],
                seg=10, u_units=2, tint=t)
        revolve(m, band("gold", 32), (x, 0, z), [(0.16, 0.92), (0.46, 1.04), (0.58, 1.26)], seg=12, u_units=4, tint=t)
        revolve(m, band("iron", 32), (x, 0, z), [(0.58, 1.26), (0.6, 1.36), (0.52, 1.36)], seg=12, u_units=4, tint=t)
        disc(m, band("coal", 32), (x, 1.3, z), 0, 0.52, seg=12, tint=t if not lit else 1.0)
        self.block_disc(x, z, 0.45)
        if lit:
            self.fires.append((x, 1.36, z))

    def tank(self, x, z, r=1.5, height=6.3, slit=270.0):
        m, t = self.m, self.tint
        revolve(m, band("iron"), (x, 0, z), [(r + 0.12, 0), (r + 0.12, 0.35), (r, 0.45)], seg=16, tint=t)
        revolve(m, band("tank"), (x, 0, z), [(r, 0.45), (r, height)], a0=slit - 180, a1=slit + 180, seg=16, u_units=8, tint=t)
        revolve(m, band("plate"), (x, 0, z), [(r, height), (r * 0.9, height + 0.5), (r * 0.6, height + 0.95),
                                               (0.34, height + 1.2)], seg=16, tint=t)
        tube(m, band("pipe"), [(x, height + 1.1, z), (x, TOP, z)], 0.34, seg=8, tint=t)
        self.block_disc(x, z, r + 0.1)

    def gear(self, centre, r, thick=0.5, teeth=18, turn=0.0, yaw=0.0):
        m, t = self.m, self.tint
        rot = rotation(yaw=yaw, pitch=90)   # the gear's axis points south, turned by yaw
        centre = vec(*centre)
        disc(m, band("gear"), centre + rot @ vec(0, thick / 2, 0), 0, r, seg=24, axis=rot, tint=t)
        revolve(m, band("iron"), centre, [(r, -thick / 2), (r, thick / 2)], seg=24, axis=rot, tint=t)
        for k in range(teeth):
            a = turn + 360.0 * k / teeth
            where = centre + rot @ vec((r + 0.12) * math.cos(math.radians(a)), 0, -(r + 0.12) * math.sin(math.radians(a)))
            obox(m, band("iron"), where, (0.36, thick, 2.2 * r / teeth), rot @ rotation(yaw=a), faces="nsewt", tint=t)
        revolve(m, band("gold", 32), centre, [(0.42, thick / 2), (0.42, thick / 2 + 0.25), (0.2, thick / 2 + 0.3)], seg=10,
                axis=rot, u_units=2, tint=t)

    def chain(self, x, z, y_top, y_bottom, hook=True):
        m, t = self.m, self.tint
        b = band("chain", 32)
        length = y_top - y_bottom
        quad(m, b, (x - 0.125, y_top, z), (0, -1, 0), (1, 0, 0), length, 0.25, (0, 0, 1), dt=0.25, ds=3.0, tint=t)
        quad(m, b, (x, y_top, z - 0.125), (0, -1, 0), (0, 0, 1), length, 0.25, (1, 0, 0), dt=0.25, ds=3.0, u0=0.25, tint=t)
        if hook:
            obox(m, band("iron", 32), (x, y_bottom - 0.12, z), (0.3, 0.28, 0.3), rotation(yaw=30), tint=t)

    def rubble(self, x, z, count, spread, size=(0.25, 0.7), y=0.0, flat=0.6):
        """Loose fallen blocks; nobody walks through the heap."""
        rng = self.rng
        self.block_disc(x, z, spread * 0.75)
        for _ in range(count):
            a, d = rng.uniform(0, 2 * math.pi), spread * math.sqrt(rng.uniform(0, 1))
            s = rng.uniform(*size)
            dims = (s * rng.uniform(0.8, 1.6), s * rng.uniform(0.5, 1.0) * flat + 0.08, s * rng.uniform(0.7, 1.3))
            rot = rotation(yaw=rng.uniform(0, 360), pitch=rng.uniform(-14, 14), roll=rng.uniform(-14, 14))
            obox(self.m, band("rough" if rng.random() < 0.6 else "ashlar"), (x + d * math.cos(a), y + dims[1] * 0.42, z + d * math.sin(a)),
                 dims, rot, tint=self.tint)

    # ------------------------------------------------------------------ floor and dais

    def floor(self):
        def flat(S, T):
            return np.stack([S, np.zeros_like(S), T], axis=-1), vec(0, 1, 0)
        if not self.ruined:
            floor_surf(self.m, flat, 0, 32, 0, 32, tint=self.edge_fade, keep=self.floor_extent)
            floor_surf(self.m, lambda S, T: (np.stack([S, np.zeros_like(S), T], axis=-1), vec(0, 1, 0)), 32, 34, 14, 18.5,
                       tint=self.tint)
            return

        def bowl(S, T):
            d = np.hypot(S - DX, T - DZ)
            y = -np.vectorize(crater_depth)(d)
            return np.stack([S, y, T], axis=-1), vec(0, 1, 0)
        floor_surf(self.m, bowl, 0, 32, 0, 32, ds=0.5, dt=0.5, tint=self.edge_fade)

    @staticmethod
    def floor_extent(P):
        """Which floor quads exist (by their centres): the hall with its wall's footing, and the passage."""
        r = np.hypot(P[:, 0] - CX, P[:, 2] - CZ)
        return (r < RADIUS + WALL + 0.1) | ((P[:, 0] > 26) & (P[:, 2] > 13.9) & (P[:, 2] < 18.6))

    def edge_fade(self, P):
        """The ground outside the ring wall falls away into the dark."""
        r = np.hypot(P[:, 0] - CX, P[:, 2] - CZ)
        k = np.clip((16.6 - r) / 2.2, 0.0, 1.0)
        base = self.tint(P)
        return base * (0.25 + 0.75 * k)[:, None]

    def skirt(self):
        """The rock the hall stands on, falling away into the dark round the open south side."""
        def fn(S, T):
            a = S / RADIUS
            r = RADIUS + WALL - 0.1 + T
            x, z = CX + r * np.cos(a), CZ - r * np.sin(a)
            lumps = np.sin(x * 1.3 + 0.7 * np.sin(z * 1.1)) * np.cos(z * 1.6) + 0.5 * np.sin(x * 2.9 + z * 2.1)
            y = -0.25 - 1.15 * T ** 1.15 + 0.35 * lumps * np.clip(T, 0, 1)
            return np.stack([x, y, z], axis=-1), np.stack([np.cos(a) * 0.7, np.full_like(a, 0.7), -np.sin(a) * 0.7], axis=-1)
        surf(self.m, band("rough"), fn, math.radians(172) * RADIUS, math.radians(372) * RADIUS, 0, 12, ds=1.2, dt=1.0, tint=self.tint)

    def dais(self):
        m = self.m
        previous_r, previous_h = None, 0.0
        for r, h in DAIS:
            revolve(m, band("gold", 32), (DX, 0, DZ), [(r, previous_h), (r, h)], seg=40, u_units=round(2 * math.pi * r * 2) / 2)
            previous_h = h
        for (r_out, h), r_in in zip(DAIS, [DAIS[1][0], DAIS[2][0], 0.0]):
            disc(m, None, (DX, h, DZ), r_in, r_out, seg=40, floor=True, dt=0.45)
        self.block_disc(DX, DZ, DAIS[0][0] - 0.05)

    # ------------------------------------------------------------------ the chamber

    def chamber(self):
        m, t, rng = self.m, self.tint, self.rng
        self.floor()
        self.skirt()
        self.dais()
        gap = math.degrees(math.asin(2.0 / RADIUS))   # the passage's half-angle on the ring

        # Ring wall: the north arc at full height, stepping down in the south-west; knee-high along the south.
        self.ring_wall([gap, 188.0], [TOP], dressed=False)
        west = [TOP, 14.0, 13.0, 9.5, 9.0, 6.0, 5.5, 3.5, 3.0, 1.5]
        self.ring_wall(np.linspace(188.0, 228.0, len(west) + 1), west)
        edges = np.linspace(228.0, 360.0 - gap - 1.0, 38)
        knee = np.round((0.5 + 0.55 * rng.random(37)) * 4) / 4
        knee[rng.random(37) < 0.2] = 0.0
        knee[edges[:-1] > 326.0] = 0.0          # nothing between the camera and the heroes at the door
        self.ring_wall(edges, knee)

        # The masses the hall is cut from: wings east and west of the ring; the door wall and the stair beyond it.
        self.tall_box((28.75, 0, 11.0), (46.0, TOP, 14.0), "s")
        self.tall_box((-12.0, 0, 13.0), (3.4, TOP, 18.9), "s")
        self.door()
        for x in range(27, 32):
            self.blocked.add((x, 13))
            self.blocked.add((x, 18))
        self.blocked.update({(30, 14), (31, 14), (31, 15), (31, 16), (31, 17)})

        for a in PILLAR_ANGLES:
            self.pillar(*polar(PILLAR_R, a))
        for a in STUMP_ANGLES:
            x, z = polar(PILLAR_R, a)
            self.pillar(x, z, broken=rng.uniform(0.9, 1.25))
            self.rubble(x + rng.uniform(-1, 1), z + rng.uniform(-0.6, 0.6), 5, 1.3)
        for x, z in BRAZIERS:
            self.brazier(x, z)
        self.machinery()
        self.rubble(4.6, 21.6, 7, 1.2, (0.3, 0.9))
        self.rubble(27.6, 20.4, 6, 0.9, (0.25, 0.6))
        self.rubble(20.5, 5.6, 4, 0.6, (0.2, 0.5))

    def door(self):
        """The door wall at the passage's east end, one leaf open against the wall and one torn off, and the stair up."""
        m, t = self.m, self.tint
        x = 31.0
        for z0, z1, h in ((14.0, 15.0, 7.4), (17.0, 17.7, 6.2), (17.7, 18.4, 2.4)):
            box(m, band("big"), (x - 0.12, 0, z0), (x + 0.82, 1.6, z1), "nswt", top=band("rough"), tint=t)
            if h > 1.6:
                box(m, band("ashlar"), (x, 1.6, z0), (x + 0.7, h, z1), "nswt", top=band("rough"), tint=t)
        box(m, band("ashlar"), (x, 4.6, 15.0), (x + 0.7, 7.0, 17.0), "wtb", top=band("rough"), tint=t)
        box(m, band("trim"), (x - 0.14, 4.4, 14.6), (x + 0.02, 5.4, 17.4), "nswtb", tint=t)
        box(m, band("mould"), (x - 0.2, 0, 14.6), (x + 0.02, 4.4, 15.0), "nswt", tint=t)
        box(m, band("mould"), (x - 0.2, 0, 17.0), (x + 0.02, 4.4, 17.4), "nswt", tint=t)
        # The north leaf stands open against the passage wall.
        leaf = band("plate")
        quad(m, leaf, (x - 1.05, 0, 14.32), (1, 0, 0), (0, 1, 0), 1.05, 4.3, (0, 0, 1), tint=t)
        quad(m, band("iron"), (x - 1.05, 0, 14.2), (0, 0, 1), (0, 1, 0), 0.12, 4.3, (-1, 0, 0), tint=t)
        for y in (0.7, 2.0, 3.3):
            quad(m, band("gold", 32), (x - 1.05, y, 14.34), (1, 0, 0), (0, 1, 0), 1.05, 0.5, (0, 0, 1), tint=t)
        # The south leaf lies where it fell.
        rot = rotation(yaw=24, pitch=-4)
        obox(m, leaf, (29.7, 0.1, 17.55), (4.2, 0.14, 1.05), rot, tint=t)
        for dx in (-1.3, 0.0, 1.3):
            obox(m, band("gold", 32), vec(29.7, 0.19, 17.55) + rot @ vec(dx, 0, 0), (0.5, 0.06, 1.05), rot, faces="t", tint=t)
        # Beyond the door: a stair climbing east towards the burning fortress.
        steps = 13
        for k in range(steps):
            x0 = 33.4 + 0.9 * k
            box(m, band("big"), (x0, 0, 14.0), (x0 + 0.9, 0.42 * (k + 1), 18.4), "swt", top=band("rough"), tint=t)
        self.info["door_light"] = [(33.0, 2.2, 16.2), (38.5, 5.2, 16.2)]
        # A terrace under the stair's south side, with a parapet and a watch fire: the edge of the diorama.
        box(m, band("big"), (31.0, -4.0, 18.4), (42.0, 0.0, 22.6), "swe", tint=t)
        quad(m, band("ashlar"), (31.0, 0.0, 22.6), (1, 0, 0), (0, 0, -1), 11.0, 4.2, (0, 1, 0), tint=t)
        for x0, x1, h in ((31.0, 33.0, 0.5), (33.6, 36.4, 0.75), (36.4, 37.8, 0.4), (39.0, 42.0, 0.75)):
            box(m, band("big"), (x0, 0.0, 22.2), (x1, h, 22.6), "nsewt", top=band("rough"), tint=t)
        self.rubble(37.6, 20.6, 7, 1.4, (0.25, 0.7))
        self.rubble(32.6, 21.4, 4, 0.6, (0.2, 0.5))
        self.brazier(35.2, 20.3)

    def machinery(self):
        """The north wall: an engine with a furnace and a great gear between two boilers, pumps, pipes and gantries."""
        m, t, rng = self.m, self.tint, self.rng

        def wall_z(x):
            return CZ - math.sqrt(RADIUS ** 2 - (x - CX) ** 2)

        # Engine block.
        box(m, band("plate"), (14.2, 0, 3.0), (17.8, 2.6, 5.6), "sewt", tint=t)
        box(m, band("gold", 32), (14.1, 2.6, 3.0), (17.9, 3.1, 5.7), "sewtb", tint=t)
        box(m, band("iron"), (14.6, 3.1, 3.0), (17.4, 5.3, 5.1), "sewt", tint=t)
        box(m, band("plate"), (15.0, 5.3, 3.0), (17.0, 6.2, 4.6), "sewt", tint=t)
        self.block(14.2, 3.0, 17.8, 5.6)
        # Furnace mouth: a grate full of fire in a gold frame.
        quad(m, band("hot"), (14.7, 0.35, 5.62), (1, 0, 0), (0, 1, 0), 1.5, 1.0, (0, 0, 1))
        quad(m, band("hot"), (14.7, 1.35, 5.62), (1, 0, 0), (0, 1, 0), 1.5, 0.4, (0, 0, 1), u0=3.0)
        box(m, band("gold", 32), (14.5, 0.1, 5.6), (14.7, 1.95, 5.72), "sewt", tint=t)
        box(m, band("gold", 32), (16.2, 0.1, 5.6), (16.4, 1.95, 5.72), "sewt", tint=t)
        box(m, band("gold", 32), (14.5, 1.75, 5.6), (16.4, 2.0, 5.74), "sewtb", tint=t)
        self.info["furnace"] = (15.45, 1.0, 5.9)
        # The great gear and its companions, behind a housing arch.
        revolve(m, band("plate"), (16.0, 7.2, 3.6), [(3.0, 0.0), (3.0, 0.55)], a0=0, a1=180, seg=24, axis=rotation(pitch=90), tint=t)
        disc(m, band("plate"), (16.0, 7.2, 4.15), 0, 3.0, a0=0, a1=180, seg=24, axis=rotation(pitch=90), tint=t)
        self.gear((16.0, 7.6, 4.5), 2.25, teeth=20)
        self.gear((13.35, 9.55, 4.4), 1.05, thick=0.4, teeth=11, turn=9)
        self.gear((18.5, 9.75, 4.45), 0.8, thick=0.4, teeth=9, turn=4)
        # Boilers either side of the engine, with their sight slits to the hall.
        self.tank(10.6, 6.0)
        self.tank(21.6, 6.0)
        self.info["slits"] = [(10.6, 3.2, 7.7), (21.6, 3.2, 7.7)]
        # Pumps between the outer pillars: an iron bed, a flywheel turned to the hall, a cylinder and its riser.
        for a in (147.6, 32.4):
            x, z = polar(11.75, a)
            rot = rotation(yaw=a - 90)
            obox(m, band("iron"), (x, 0.6, z), (2.0, 1.2, 1.5), rot, top=band("plate"), faces="nsewt", tint=t)
            obox(m, band("gold", 32), (x, 1.2, z), (2.1, 0.25, 1.6), rot, faces="nsewt", tint=t)
            barrel = rot @ rotation(roll=90)
            revolve(m, band("plate"), vec(x, 2.0, z) + rot @ vec(0.25, 0, -0.1), [(0.62, -0.75), (0.62, 0.75)], seg=12, axis=barrel, u_units=4, tint=t)
            for end in (-0.75, 0.75):
                disc(m, band("gear", 32), vec(x, 2.0, z) + rot @ vec(0.25 + end, 0, -0.1), 0, 0.62, seg=12, axis=barrel, down=end < 0, tint=t)
            self.gear(vec(x, 1.75, z) + rot @ vec(-0.55, 0, 0.8), 0.78, thick=0.22, teeth=10, yaw=a - 90)
            wx, wz = polar(12.55, a)
            tube(m, band("pipe_v"), [tuple(vec(x, 2.5, z) + rot @ vec(0.25, 0, -0.1)), tuple(vec(x, 4.6, z) + rot @ vec(0.25, 0, -0.1)),
                                     (wx, 5.6, wz), (wx, TOP, wz)], 0.2, tint=t)
            self.block_disc(x, z, 1.15)
        self.info["pump"] = polar(11.75, 147.6)

        # Pipes running round the wall, on brackets.
        for y, r, radius, name in ((0.75, 12.45, 0.3, "pipe"), (6.3, 12.6, 0.24, "pipe_v"), (11.2, 12.62, 0.3, "pipe")):
            for a0, a1 in ((22, 64), (116, 158)) if y < 6 else ((20, 160),):
                points = [polar(r, a)[:1] + (y,) + polar(r, a)[1:] for a in np.linspace(a0, a1, int((a1 - a0) / 4) + 1)]
                tube(m, band(name), points, radius, seg=8, tint=t)
                for a in np.arange(a0 + 3, a1, 14):
                    x, z = polar(r + 0.15, a)
                    obox(m, band("iron", 32), (x, y - radius - 0.1, z), (0.25, 0.2, 0.5), rotation(yaw=a - 90), tint=t)
        # Organ pipes up the wall into the dark.
        for x, r, z_off in ((14.45, 0.26, 0.4), (15.0, 0.2, 0.35), (17.0, 0.2, 0.35), (17.55, 0.26, 0.4), (12.4, 0.3, 0.45),
                            (19.6, 0.3, 0.45), (8.6, 0.24, 0.4), (23.4, 0.24, 0.4), (9.2, 0.18, 0.3), (22.8, 0.18, 0.3)):
            base = 5.3 if 14 < x < 18 else 0.0
            tube(m, band("pipe" if r > 0.22 else "pipe_v"), [(x, base, wall_z(x) + z_off), (x, TOP, wall_z(x) + z_off)], r, seg=8, tint=t)
        # Feeds from the boilers into the engine, behind the pillars.
        for x_tank, x_engine, sign in ((10.6, 14.6, 1), (21.6, 17.4, -1)):
            y = 4.2
            tube(m, band("pipe"), [(x_tank + sign * 1.2, y, 5.3), (x_tank + sign * 1.9, y, 4.3), (x_engine, y, 4.3)], 0.28, seg=8, tint=t)
            tube(m, band("pipe_v"), [(x_tank + sign * 1.0, 6.9, 5.4), (x_tank + sign * 1.4, 7.9, 4.6), (x_tank + sign * 1.4, 9.2, 4.2),
                                     (x_engine - sign * 0.4, 9.2, 3.9)], 0.2, seg=8, tint=t)
        # Glass conduit tubes carrying Aetherite down into the floor channels.
        for x, z_out, y in ((17.0, 5.6, 1.3), (11.5, 7.25, 1.3)):
            points = [(x, y, z_out - 0.3), (x, y, z_out + 0.25)] + arc_points((x, y - 0.5, z_out + 0.25), 0.5, 90, 0, 4, "zy") + \
                     [(x, 0.0, z_out + 0.75)]
            tube(m, band("tube"), points, 0.2, seg=8)
            revolve(m, band("gold", 32), (x, 0, z_out + 0.75), [(0.36, 0.0), (0.36, 0.14), (0.24, 0.2)], seg=10, u_units=2, tint=t)
        px, pz = polar(11.75, 147.6)
        tube(m, band("tube"), [(px + 0.35, 1.0, pz + 0.3), (6.2, 1.0, 10.0)] + arc_points((6.2, 0.5, 10.0), 0.5, 90, 0, 4, "zy") +
             [(6.2, 0.0, 10.5)], 0.18, seg=8)
        revolve(m, band("gold", 32), (6.2, 0, 10.5), [(0.34, 0.0), (0.34, 0.14), (0.22, 0.2)], seg=10, u_units=2, tint=t)

        # Gantries: one either side between the outer pillars, with ladders; a bridge high before the gear.
        for a0, a1, ladder in ((20.0, 44.5, 26.0), (135.5, 160.0, 154.0)):
            self.gantry(a0, a1, 4.7)
            x, z = polar(11.62, ladder)
            c, s = math.cos(math.radians(ladder)), math.sin(math.radians(ladder))
            quad(m, band("grate", 24), (x + 0.33 * s, 0, z + 0.33 * c), (0, 1, 0), (-s, 0, -c), 4.7, 0.66, (-c, 0, s), tint=t)
        box(m, band("grate"), (13.9, 9.3, 5.3), (18.1, 9.36, 6.2), "tb", tint=t)
        quad(m, band("grate"), (13.9, 9.36, 6.2), (1, 0, 0), (0, 1, 0), 4.2, 0.9, (0, 0, 1), tint=t)
        quad(m, band("grate"), (13.9, 9.36, 5.3), (1, 0, 0), (0, 1, 0), 4.2, 0.9, (0, 0, 1), tint=t)
        # Valve wheels on the boilers and the engine.
        for x, y, z in ((10.6, 2.0, 7.56), (21.6, 2.0, 7.56), (17.1, 3.9, 5.16)):
            ring = [(x + 0.32 * math.cos(a), y + 0.32 * math.sin(a), z) for a in np.linspace(0, 2 * math.pi, 13)]
            tube(m, band("pipe_v", 32), ring, 0.05, seg=5, tint=t)
            tube(m, band("pipe_v", 32), [(x - 0.3, y, z), (x + 0.3, y, z)], 0.035, seg=4, tint=t)
            tube(m, band("pipe_v", 32), [(x, y - 0.3, z), (x, y + 0.3, z)], 0.035, seg=4, tint=t)
            tube(m, band("pipe", 32), [(x, y, z - 0.12), (x, y, z + 0.02)], 0.06, seg=5, tint=t)
        # Chains hanging out of the dark.
        for x, z, bottom in ((14.3, 6.6, 6.4), (18.3, 7.0, 4.9), (8.3, 9.6, 7.2), (23.2, 9.2, 5.8), (20.2, 6.8, 8.6),
                             (12.2, 7.4, 9.4), (5.6, 13.8, 6.6), (25.6, 13.0, 8.0)):
            self.chain(x, z, TOP, bottom)

    def gantry(self, a0, a1, y):
        m, t = self.m, self.tint
        r0, r1 = 11.55, RADIUS
        length = math.radians(a1 - a0) * RADIUS

        def deck(S, T):
            a = math.radians(a0) + S / RADIUS
            r = r0 + T
            return np.stack([CX + r * np.cos(a), np.full_like(a, y), CZ - r * np.sin(a)], axis=-1), vec(0, 1, 0)
        surf(m, band("grate"), deck, 0, length, 0, r1 - r0, ds=0.8, dt=1.0, tint=t)

        def rail(S, T):
            a = math.radians(a0) + S / RADIUS
            return (np.stack([CX + r0 * np.cos(a), y + T, CZ - r0 * np.sin(a)], axis=-1),
                    np.stack([-np.cos(a), np.zeros_like(a), np.sin(a)], axis=-1))
        surf(m, band("grate"), rail, 0, length, 0, 0.9, ds=0.8, tint=t)
        revolve(m, band("iron", 32), (CX, 0, CZ), [(r0 - 0.04, y - 0.16), (r0 - 0.04, y + 0.02)], a0=a0, a1=a1, seg=30, inward=True, tint=t)
        for a in np.linspace(a0 + 1.5, a1 - 1.5, 4):
            x, z = polar(12.3, a)
            obox(m, band("iron", 32), (x, y - 0.75, z), (0.16, 1.9, 0.16), rotation(yaw=a - 90, pitch=-42), tint=t)

    # ------------------------------------------------------------------ the ruins

    def ground(self, X, Z):
        """Height of the rubble: a slope fallen against the north wall, a berm outside the ring, flat where people walk."""
        X, Z = np.asarray(X, np.float64), np.asarray(Z, np.float64)
        r = np.hypot(X - CX, Z - CZ)
        sin_a = -(Z - CZ) / np.maximum(r, 1e-6)
        cos_a = (X - CX) / np.maximum(r, 1e-6)
        w = np.clip((sin_a + 0.12) / 0.5, 0, 1)
        w = w * w * (3 - 2 * w)
        start = 8.7 + 1.7 * np.clip(np.abs(cos_a) * 1.5 - 0.6, 0, 1)
        k = np.clip((r - start) / (12.9 - start), 0, 1.6)
        slope = np.where(k < 1, k ** 1.35, 1 + 0.12 * (k - 1)) * (1.0 + 3.6 * np.clip(sin_a, 0, 1) ** 1.4)
        lumps = (np.sin(X * 1.7 + 0.5 * np.sin(Z * 1.3)) * np.cos(Z * 1.9 + 0.7 * np.sin(X * 0.9)) +
                 0.6 * np.sin(X * 3.9 + Z * 2.3) * np.sin(Z * 3.1 - X * 1.7))
        h = w * slope * (1.0 + 0.16 * lumps)
        outside = np.clip((r - 14.2) / 2.5, 0, 1)
        berm = (1.3 + 1.5 * w + 0.5 * lumps) * (1 - np.clip((r - 20) / 30, 0, 1) * 0.6)
        south = -np.clip((r - 14.6) * 0.22, 0, 3.5) * (1 - w)
        h = h * (1 - outside) + outside * (berm * np.clip(w * 3 + 0.25, 0, 1) + south)
        # Keep the east passage and the stair beyond it clear.
        passage = np.clip((X - 25.5) / 1.5, 0, 1) * np.clip(1 - np.abs(Z - 16.1) / 2.6, 0, 1) * np.clip((40 - X) / 4, 0, 1)
        return h * (1 - np.clip(passage * 2.5, 0, 1))

    def terrain(self):
        m, t = self.m, self.tint

        def fn(S, T):
            e = 0.05
            y = self.ground(S, T)
            nx = (self.ground(S - e, T) - self.ground(S + e, T)) / (2 * e)
            nz = (self.ground(S, T - e) - self.ground(S, T + e)) / (2 * e)
            return np.stack([S, y - 0.06, T], axis=-1), np.stack([nx, np.ones_like(nx), nz], axis=-1)
        surf(m, band("rough"), fn, 0, 32, 0, 14.5, ds=0.5, dt=0.5, tint=t, keep=lambda quads: quads[:, :, 1].max(axis=1) > -0.03)
        # The land beyond the hall, out to the fog.
        for s0, s1, t0, t1 in ((-70, 0, -70, 34), (32, 102, -70, 34), (0, 32, -70, 0)):
            surf(m, band("rough"), fn, s0, s1, t0, t1, ds=2.0, dt=2.0, tint=t)
        for x in range(1, 31):
            for z in range(1, 15):
                xs, zs = np.meshgrid(np.linspace(x + 0.1, x + 0.9, 3), np.linspace(z + 0.1, z + 0.9, 3))
                if self.ground(xs, zs).max() > 0.34:
                    self.blocked.add((x, z))

    def scatter(self, x, z, count, spread, size=(0.25, 0.8), flat=0.7, block=False):
        """Fallen blocks resting on the rubble or the floor."""
        rng = self.rng
        for _ in range(count):
            a, d = rng.uniform(0, 2 * math.pi), spread * math.sqrt(rng.uniform(0, 1))
            px, pz = x + d * math.cos(a), z + d * math.sin(a)
            s = rng.uniform(*size)
            dims = (s * rng.uniform(0.8, 1.7), s * rng.uniform(0.5, 1.0) * flat + 0.1, s * rng.uniform(0.7, 1.3))
            rot = rotation(yaw=rng.uniform(0, 360), pitch=rng.uniform(-22, 22), roll=rng.uniform(-22, 22))
            y = max(float(self.ground(px, pz)), 0.0)
            obox(self.m, band(("rough", "ashlar", "big", "brick")[int(rng.integers(0, 4))]), (px, y + dims[1] * 0.3, pz), dims, rot,
                 top=band("rough"), tint=self.tint)
            if block and s > 0.45:
                self.block_disc(px, pz, 0.3 * s)

    def drum(self, x, z, yaw, length=1.8, r=0.6, sink=0.0):
        """A fallen pillar drum lying on its side."""
        m, t = self.m, self.tint
        y = max(float(self.ground(x, z)), 0.0) + r - sink
        rot = rotation(yaw=yaw, roll=90)
        revolve(m, band("flute"), (x, y, z), [(r, -length / 2), (r, length / 2)], u_units=4, seg=12, axis=rot, tint=t)
        disc(m, band("rough"), vec(x, y, z) + rot @ vec(0, length / 2, 0), 0, r, seg=12, axis=rot, tint=t)
        disc(m, band("rough"), vec(x, y, z) + rot @ vec(0, -length / 2, 0), 0, r, seg=12, axis=rot, down=True, tint=t)
        axis = rot @ vec(0, 1, 0)
        for k in (-0.4, 0.0, 0.4):
            self.block_disc(x + axis[0] * length * k, z + axis[2] * length * k, r * 0.9)

    def stump_profile(self, n, a0, a1):
        """Column heights of the broken ring wall: two spires either side of a breach in the north (the last shot
        looks up past them from the crater), lower teeth beyond, rubble in the south."""
        rng = self.rng
        edges = np.linspace(a0, a1, n + 1)
        a = 0.5 * (edges[:-1] + edges[1:])

        step = edges[1] - edges[0]

        def tower(centre, height, reach, flat=1.0):
            """A stump that steps down from its crown: `flat` columns of full height, gone `reach` columns out."""
            d = np.clip((np.abs(a - centre) / step - flat) / reach, 0, 1)
            return height * (1 - d ** 0.75)
        base = 0.8 + 2.4 * np.clip(np.sin(np.radians(a)), 0, 1) ** 0.8
        towers = np.maximum.reduce([tower(109, 16.0, 8), tower(85, 13.5, 6.5), tower(125, 6.0, 4, 0.6), tower(70.5, 6.5, 4, 0.6),
                                    tower(54, 4.5, 6), tower(25, 3.5, 6), tower(142, 2.5, 5), tower(168, 2.0, 7)])
        jag = np.where(towers > 0.5, rng.uniform(0.84, 1.0, n), rng.uniform(0.5, 1.0, n))
        heights = (base + towers) * jag
        bitten = (rng.random(n) < 0.22) & (towers > 2.0) & (towers < 12.0)      # stones torn out of the flanks
        heights = np.where(bitten, heights - rng.uniform(1.5, 4.5, n), heights)
        for centre, width in ((98, 3.2), (153, 5.0), (39, 3.0), (183, 5), (133, 2.5)):   # breaches
            heights = np.where(np.abs(a - centre) < width, np.minimum(heights, rng.uniform(0.5, 2.4, n)), heights)
        south = np.sin(np.radians(a)) < -0.05
        heights = np.where(south, rng.uniform(0.0, 0.8, n) * (rng.random(n) > 0.3), heights)
        return edges, np.round(heights * 2) / 2

    def ruins(self):
        m, t, rng = self.m, self.tint, self.rng
        self.floor()
        self.terrain()
        gap = math.degrees(math.asin(2.3 / RADIUS))
        n = int(math.radians(360 - 2 * gap) * RADIUS / 0.6)
        edges, heights = self.stump_profile(n, gap, 360 - gap)
        self.ring_wall(edges, heights)
        for x in range(27, 32):
            self.blocked.add((x, 13))
            self.blocked.add((x, 18))
        self.blocked.update({(31, 14), (31, 15), (31, 16), (31, 17)})

        # What is left of the pillars: jagged shafts, and two that fell across the floor.
        for a, h in zip(PILLAR_ANGLES, (2.2, None, 4.3, 8.0, 5.6, 1.4, None)):
            x, z = polar(PILLAR_R, a)
            if h is not None:
                self.pillar(x, z, broken=h)
            else:
                self.pillar(x, z, broken=0.9)
        for a in STUMP_ANGLES:
            self.pillar(*polar(PILLAR_R, a), broken=rng.uniform(0.6, 1.0))
        x, z = polar(PILLAR_R, 46.8)
        for k, (d, yaw) in enumerate(((2.0, 49), (4.1, 53), (6.3, 44))):
            self.drum(x - 0.75 * d, z + 0.66 * d, yaw + 90 - 49 + 41, sink=0.05 * k)
        box(m, band("mould"), (17.2, 0, 12.0), (19.0, 0.42, 13.6), "nsewt", top=band("rough"), tint=t)
        self.block(17.2, 12.0, 19.0, 13.6)
        x, z = polar(PILLAR_R, 196.0)
        self.drum(x + 1.5, z + 1.3, 130, sink=0.04)
        self.drum(x + 3.0, z + 2.9, 122, sink=0.1)

        # The door wall and the stair, broken down.
        for z0, z1, h in ((14.0, 15.0, 3.1), (17.0, 17.7, 1.5)):
            box(m, band("big"), (30.88, 0, z0), (31.82, min(h, 1.6), z1), "nswt", top=band("rough"), tint=t)
            if h > 1.6:
                box(m, band("ashlar"), (31.0, 1.6, z0), (31.7, h, z1), "nswt", top=band("rough"), tint=t)
        for k in range(13):
            x0 = 33.4 + 0.9 * k
            box(m, band("big"), (x0, 0, 14.0), (x0 + 0.9, 0.42 * (k + 1), 18.4), "swt", top=band("rough"), tint=t)
        self.scatter(32.6, 16.4, 16, 1.6, (0.3, 1.0))
        self.scatter(35.5, 16.0, 14, 2.2, (0.4, 1.2))

        # The crater's rim: the dais's steps heaved up and thrown outwards (the east side stays open).
        for a in np.arange(52, 232, 20.0):
            a = a + rng.uniform(-6, 6)
            d = rng.uniform(3.9, 4.7)
            x, z = DX + d * math.cos(math.radians(a)), DZ - d * math.sin(math.radians(a))
            rot = rotation(yaw=a - 90 + rng.uniform(-18, 18), pitch=rng.uniform(16, 42), roll=rng.uniform(-10, 10))
            obox(m, band("big"), (x, 0.22, z), (rng.uniform(1.2, 1.9), 0.3, rng.uniform(0.7, 1.1)), rot, top=band("mould"), tint=t)
            self.block_disc(x, z, 0.55)
        for a in np.arange(75, 240, 33.0):
            d = rng.uniform(1.9, 3.0)
            x, z = DX + d * math.cos(math.radians(a)), DZ - d * math.sin(math.radians(a))
            y = -crater_depth(d)
            obox(m, band("rough"), (x, y + 0.2, z), (0.22, rng.uniform(0.5, 1.1), 0.3),
                 rotation(yaw=a, pitch=rng.uniform(-25, 25), roll=rng.uniform(-30, -8)), tint=t)

        # Wreck of the machinery in the rubble.
        self.wreck()
        # Fallen blocks everywhere, thick on the slope.
        for _ in range(150):
            a = rng.uniform(20, 160)
            r = rng.uniform(9.4, 12.6)
            x, z = polar(r, a)
            self.scatter(x, z, 1, 0.2, (0.3, 1.3), block=True)
        for x, z, count, spread in ((8.6, 11.4, 5, 1.0), (22.4, 11.8, 6, 1.2), (26.8, 19.6, 6, 1.0), (5.4, 20.8, 7, 1.3), (14.2, 25.8, 6, 1.6),
                                    (21.6, 24.6, 5, 1.2), (9.0, 24.4, 5, 1.0), (27.4, 12.4, 5, 0.8), (16.0, 10.6, 4, 0.9)):
            self.scatter(x, z, count, spread, (0.25, 0.7), block=True)
        for a in np.arange(200, 345, 7.0):
            x, z = polar(13.4 + rng.uniform(-0.5, 1.4), a + rng.uniform(-3, 3))
            self.scatter(x, z, 2, 0.6, (0.3, 0.9))
        # Great slabs of the tower wall, half buried.
        for x, z, size, yaw, pitch in ((12.6, 8.2, (2.6, 1.3, 1.7), 20, 28), (20.6, 8.6, (2.2, 1.5, 1.4), -34, -22), (7.4, 9.6, (1.8, 1.1, 1.5), 60, 18),
                                       (24.6, 9.4, (2.0, 1.2, 1.2), 15, 30), (17.0, 6.2, (3.0, 1.6, 1.6), -8, -35)):
            y = float(self.ground(x, z))
            obox(m, band("ashlar"), (x, y + 0.25, z), size, rotation(yaw=yaw, pitch=pitch, roll=9), top=band("rough"), tint=t)
            self.block_disc(x, z, 1.0)
        # Braziers: thrown down and cold, but for one that still smoulders.
        for i, (x, z) in enumerate(BRAZIERS):
            if i == 1:
                self.brazier(x, z, lit=True)
            else:
                self.brazier(x + rng.uniform(-0.4, 0.4), z + rng.uniform(-0.4, 0.4), fallen=True)
                self.block_disc(x, z, 0.4)
        # Far off: what is left of the fortress, shapes in the haze.
        for x, z, w, d, h in ((-34, -38, 16, 8, 15), (-12, -52, 10, 10, 26), (22, -58, 26, 6, 11), (56, -44, 12, 12, 19), (74, -20, 8, 16, 9),
                              (-52, -8, 8, 20, 10), (40, -64, 6, 6, 32), (-58, -52, 9, 9, 21)):
            box(m, band("brick"), (x, -2, z), (x + w, h * 0.45, z + d), "nsewt", top=band("rough"), tint=0.5)
            for _ in range(5):   # a ragged skyline of what still stands on it
                fx, fw = rng.uniform(0, 0.8), rng.uniform(0.12, 0.3)
                top = h * rng.uniform(0.5, 1.0)
                box(m, band("brick"), (x + w * fx, h * 0.45, z + d * 0.2), (x + w * min(1.0, fx + fw), top, z + d * 0.8), "nsewt",
                    top=band("rough"), tint=0.5)
        self.info["smoke"] = [(12.0, 3.6, 7.4), (20.4, 3.2, 8.0), (6.2, 1.4, 20.8), (33.5, 1.5, 16.0), (24.6, 1.6, 11.6)]

    def wreck(self):
        m, t, rng = self.m, self.tint, self.rng
        # The west boiler still stands, split open; the east one lies in the rubble.
        x, z = 10.6, 6.0
        jag = 5.45 + rng.uniform(-0.85, 0.85, 17)
        jag[-1] = jag[0]
        revolve(m, band("tank"), (x, 0, z), [(1.5, 0.45), (1.5, 7.2)], a0=90, a1=450, seg=16, u_units=8,
                clip=lambda S: np.interp(S, np.linspace(0, 8, 17), jag) - 0.45, tint=t)
        revolve(m, band("iron"), (x, 0, z), [(1.44, 0.45), (1.44, 7.2)], a0=90, a1=450, seg=16, u_units=8, inward=True,
                clip=lambda S: np.interp(S, np.linspace(0, 8, 17), jag) - 0.45, tint=t)
        rot = rotation(yaw=118, roll=78)
        y = float(self.ground(21.4, 7.6))
        revolve(m, band("tank"), (21.4, y + 0.7, 7.6), [(1.5, -2.6), (1.5, 2.2)], seg=16, u_units=8, axis=rot, tint=t)
        revolve(m, band("plate"), (21.4, y + 0.7, 7.6), [(1.5, 2.2), (1.35, 2.7), (0.9, 3.15), (0.34, 3.4)], seg=16, axis=rot, tint=t)
        self.block_disc(21.4, 7.6, 1.6)
        # The engine block, heeled over, and the great gear thrown clear of it.
        obox(m, band("plate"), (16.0, float(self.ground(16.0, 4.6)) + 0.4, 4.6), (3.6, 2.6, 2.4), rotation(yaw=8, pitch=-14, roll=11),
             top=band("iron"), tint=t)
        centre = vec(18.4, float(self.ground(18.4, 9.4)) + 0.55, 9.6)
        rot = rotation(yaw=-24, pitch=62)
        disc(m, band("gear"), centre + rot @ vec(0, 0.25, 0), 0, 2.25, seg=24, axis=rot, tint=t)
        disc(m, band("iron"), centre - rot @ vec(0, 0.25, 0), 0, 2.25, seg=24, axis=rot, down=True, tint=t)
        revolve(m, band("iron"), centre, [(2.25, -0.25), (2.25, 0.25)], seg=24, axis=rot, tint=t)
        for k in range(20):
            if k in (3, 4, 11):
                continue
            a = 18.0 * k
            where = centre + rot @ vec(2.37 * math.cos(math.radians(a)), 0, -2.37 * math.sin(math.radians(a)))
            obox(m, band("iron"), where, (0.36, 0.5, 0.25), rot @ rotation(yaw=a), tint=t)
        revolve(m, band("gold", 32), centre, [(0.42, 0.25), (0.42, 0.5), (0.2, 0.55)], seg=10, axis=rot, u_units=2, tint=t)
        self.block_disc(18.4, 9.6, 2.0)
        # Pipes: broken off at all heights against the wall, bent ones reaching out of the rubble.
        for x, top, lean in ((14.45, 7.5, 0.6), (15.0, 5.5, -0.4), (17.55, 8.6, 0.9), (12.4, 6.8, -1.2), (19.6, 5.4, 0.5), (8.6, 6.2, -0.5),
                             (23.4, 7.4, 1.0), (9.2, 8.2, 0.3)):
            z0 = CZ - math.sqrt(RADIUS ** 2 - (x - CX) ** 2) + 0.45
            tube(m, band("pipe"),
                 [(x, 0.5, z0), (x + lean * 0.2, top * 0.6, z0 + 0.1), (x + lean, top, z0 + 0.5 + abs(lean) * 0.6)], rng.uniform(0.18, 0.3), seg=8, tint=t)
        for x, z, yaw, reach in ((13.6, 8.8, 200, 2.6), (22.8, 9.8, -40, 2.2), (9.4, 8.4, 150, 1.8), (19.4, 6.4, 60, 3.2), (6.6, 12.2, 190, 1.6)):
            y = float(self.ground(x, z))
            c, s_ = math.cos(math.radians(yaw)), -math.sin(math.radians(yaw))
            points = [(x, y - 0.4, z), (x + 0.15 * reach * c, y + 0.7 * reach, z + 0.15 * reach * s_),
                      (x + 0.5 * reach * c, y + 1.15 * reach, z + 0.5 * reach * s_), (x + 1.0 * reach * c, y + 1.0 * reach, z + 1.0 * reach * s_)]
            tube(m, band("pipe_v" if rng.random() < 0.5 else "pipe"), points, rng.uniform(0.12, 0.22), seg=7, tint=t)
        # Chains and a length of gantry hanging from the tallest stumps.
        for a, top, bottom in ((111, 15.0, 8.6), (107.5, 12.0, 7.6), (85, 11.0, 6.4)):
            x, z = polar(RADIUS - 0.35, a)
            self.chain(x, z, top, bottom)
        self.gantry(104.0, 117.0, 9.4)


def build(ruined):
    b = Builder(ruined)
    if ruined:
        b.ruins()
    else:
        b.chamber()
    return b


EMISSIVE = {"chamber": {"floor": 3.2, "metal": 3.6}, "ruins": {"floor": 1.3, "metal": 0.12}}
