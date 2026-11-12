"""Mesh kit for the chamber generator: parametric patches mapped onto trim
sheets, in engine coordinates (+x east, +y up, +z south, 1 unit = 1 cell).

Three texture pages exist: 'floor' (a unique 512 x 512 top-down painting of
the whole 32 x 32 map), 'stone' and 'metal' (256 x 256 trim sheets that wrap
in U; V selects a band). `surf` splits a patch wherever V would leave its
band, so any surface can tile a band at the sheet's texel density.
"""
import math
import numpy as np

PAGES = {"floor": (512, 512), "stone": (256, 256), "metal": (256, 256)}


class Band:
    def __init__(self, page, y0, h, ppu=16):
        self.page, self.y0, self.h, self.ppu = page, y0, h, ppu

    @property
    def units(self):
        return self.h / self.ppu


class Mesh:
    """Vertices with position, normal, uv (pixels) and a tint, quads per page."""

    def __init__(self):
        self.parts = {}

    def _part(self, page):
        return self.parts.setdefault(page, {"P": [], "N": [], "UV": [], "C": [], "F": [], "n": 0})

    def add(self, page, P, N, UV, faces, tint=1.0):
        part = self._part(page)
        P = np.asarray(P, np.float64).reshape(-1, 3)
        N = np.asarray(N, np.float64).reshape(-1, 3)
        UV = np.asarray(UV, np.float64).reshape(-1, 2)
        faces = np.asarray(faces, np.int64)
        length = np.linalg.norm(N, axis=1, keepdims=True)
        N = N / np.maximum(length, 1e-9)
        # Wind every face so that its geometric normal agrees with its vertex normals.
        if faces.shape[1] == 4:  # the diagonals' cross product survives a collapsed edge
            geometric = np.cross(P[faces[:, 2]] - P[faces[:, 0]], P[faces[:, 3]] - P[faces[:, 1]])
        else:
            geometric = np.cross(P[faces[:, 1]] - P[faces[:, 0]], P[faces[:, 2]] - P[faces[:, 0]])
        wanted = N[faces].mean(axis=1)
        flip = (geometric * wanted).sum(axis=1) < 0
        faces = np.where(flip[:, None], faces[:, ::-1], faces)
        faces = faces[np.linalg.norm(geometric, axis=1) > 1e-9]
        if not len(faces):
            return
        if callable(tint):
            C = np.asarray(tint(P), np.float64).reshape(-1, 3)
        else:
            C = np.broadcast_to(np.asarray(tint, np.float64), (len(P), 3)) if np.ndim(tint) else np.full((len(P), 3), float(tint))
        part["P"].append(P)
        part["N"].append(N)
        part["UV"].append(UV)
        part["C"].append(np.array(C))
        part["F"].append(faces + part["n"])
        part["n"] += len(P)

    def arrays(self, page):
        part = self.parts[page]
        quads = [f for f in part["F"] if f.shape[1] == 4]
        tris = [f for f in part["F"] if f.shape[1] == 3]
        return (np.concatenate(part["P"]), np.concatenate(part["N"]), np.concatenate(part["UV"]), np.concatenate(part["C"]),
                np.concatenate(quads) if quads else np.zeros((0, 4), np.int64),
                np.concatenate(tris) if tris else np.zeros((0, 3), np.int64))

    def triangles(self):
        total = 0
        for part in self.parts.values():
            for f in part["F"]:
                total += len(f) * (2 if f.shape[1] == 4 else 1)
        return total


def _grid_faces(ns, nv):
    i, j = np.meshgrid(np.arange(ns), np.arange(nv), indexing="ij")
    a = (i * (nv + 1) + j).ravel()
    return np.stack([a, a + (nv + 1), a + (nv + 1) + 1, a + 1], axis=1)


def surf(m, band, fn, s0, s1, t0, t1, ds=1.0, dt=1.0, u0=0.0, v0=0.0, us=1.0, tint=1.0, clip=None, keep=None):
    """A patch fn(S, T) -> (positions, normals) over [s0, s1] x [t0, t1] (units).
    U runs with s (wrapping freely), V with t inside the band. `clip(S)` cuts
    the patch off at a ragged upper edge t <= clip(s) (a broken wall's top)."""
    hb = band.units
    width, height = PAGES[band.page]
    t = t0
    while t < t1 - 1e-7:
        vloc = (v0 + (t - t0)) % hb
        if hb - vloc < 1e-6:
            vloc = 0.0
        tb = min(t1, t + (hb - vloc))
        ns = max(1, int(math.ceil((s1 - s0) / ds - 1e-6)))
        nv = max(1, int(math.ceil((tb - t) / dt - 1e-6)))
        S, T = np.meshgrid(np.linspace(s0, s1, ns + 1), np.linspace(t, tb, nv + 1), indexing="ij")
        if clip is not None:
            T = np.clip(np.minimum(T, clip(S)), t, None)
        P, N = fn(S, T)
        U = (u0 + (S - s0) * us) * band.ppu / width
        V = (band.y0 + band.h - (vloc + (T - t)) * band.ppu) / height
        faces = _grid_faces(ns, nv)
        if keep is not None:
            faces = faces[keep(P.reshape(-1, 3)[faces])]
        m.add(band.page, P, np.broadcast_to(N, P.shape), np.stack([U, V], axis=-1), faces, tint)
        t = tb


def floor_surf(m, fn, s0, s1, t0, t1, ds=0.5, dt=0.5, tint=1.0, keep=None):
    """A patch textured by the top-down floor painting (16 px per unit).
    keep(centres) -> mask drops the quads whose centres it rejects."""
    ns = max(1, int(math.ceil((s1 - s0) / ds - 1e-6)))
    nv = max(1, int(math.ceil((t1 - t0) / dt - 1e-6)))
    S, T = np.meshgrid(np.linspace(s0, s1, ns + 1), np.linspace(t0, t1, nv + 1), indexing="ij")
    P, N = fn(S, T)
    UV = np.stack([P[..., 0] / 32.0, P[..., 2] / 32.0], axis=-1)
    faces = _grid_faces(ns, nv)
    if keep is not None:
        faces = faces[keep(P.reshape(-1, 3)[faces].mean(axis=1))]
    m.add("floor", P, np.broadcast_to(N, P.shape), UV, faces, tint)


def vec(*v):
    return np.array(v, np.float64)


def plane(origin, du, dv, normal=None):
    origin, du, dv = vec(*origin), vec(*du), vec(*dv)
    n = np.cross(du, dv) if normal is None else vec(*normal)

    def fn(S, T):
        return origin + S[..., None] * du + T[..., None] * dv, n
    return fn


def quad(m, band, origin, du, dv, w, h, normal=None, **kw):
    """A flat rectangle: origin + s*du + t*dv, s in [0, w], t in [0, h] (du, dv unit vectors)."""
    surf(m, band, plane(origin, du, dv, normal), 0, w, 0, h, **kw)


def box(m, band, lo, hi, faces="nsewt", top=None, **kw):
    """Axis-aligned box. faces: n s e w (sides), t (top), b (bottom)."""
    x0, y0, z0 = lo
    x1, y1, z1 = hi
    h = y1 - y0
    if "s" in faces:
        quad(m, band, (x0, y0, z1), (1, 0, 0), (0, 1, 0), x1 - x0, h, (0, 0, 1), **kw)
    if "n" in faces:
        quad(m, band, (x1, y0, z0), (-1, 0, 0), (0, 1, 0), x1 - x0, h, (0, 0, -1), **kw)
    if "e" in faces:
        quad(m, band, (x1, y0, z1), (0, 0, -1), (0, 1, 0), z1 - z0, h, (1, 0, 0), **kw)
    if "w" in faces:
        quad(m, band, (x0, y0, z0), (0, 0, 1), (0, 1, 0), z1 - z0, h, (-1, 0, 0), **kw)
    if "t" in faces:
        quad(m, top or band, (x0, y1, z1), (1, 0, 0), (0, 0, -1), x1 - x0, z1 - z0, (0, 1, 0), **kw)
    if "b" in faces:
        quad(m, top or band, (x0, y0, z0), (1, 0, 0), (0, 0, 1), x1 - x0, z1 - z0, (0, -1, 0), **kw)


def rotation(yaw=0.0, pitch=0.0, roll=0.0):
    """Rotation matrix: yaw about y, then pitch about x, then roll about z (degrees)."""
    cy, sy = math.cos(math.radians(yaw)), math.sin(math.radians(yaw))
    cp, sp = math.cos(math.radians(pitch)), math.sin(math.radians(pitch))
    cr, sr = math.cos(math.radians(roll)), math.sin(math.radians(roll))
    Ry = np.array([[cy, 0, sy], [0, 1, 0], [-sy, 0, cy]])
    Rx = np.array([[1, 0, 0], [0, cp, -sp], [0, sp, cp]])
    Rz = np.array([[cr, -sr, 0], [sr, cr, 0], [0, 0, 1]])
    return Ry @ Rx @ Rz


def obox(m, band, centre, size, rot=None, top=None, faces="nsewtb", **kw):
    """A box of full `size` about `centre`, turned by the matrix `rot`."""
    rot = np.eye(3) if rot is None else rot
    centre = vec(*centre)
    hx, hy, hz = (s / 2 for s in size)
    X, Y, Z = rot[:, 0], rot[:, 1], rot[:, 2]

    def side(origin, du, dv, w, h, n, b):
        surf(m, b, plane(centre + origin, du, dv, n), 0, w, 0, h, **kw)
    if "s" in faces:
        side(-hx * X - hy * Y + hz * Z, X, Y, 2 * hx, 2 * hy, Z, band)
    if "n" in faces:
        side(hx * X - hy * Y - hz * Z, -X, Y, 2 * hx, 2 * hy, -Z, band)
    if "e" in faces:
        side(hx * X - hy * Y + hz * Z, -Z, Y, 2 * hz, 2 * hy, X, band)
    if "w" in faces:
        side(-hx * X - hy * Y - hz * Z, Z, Y, 2 * hz, 2 * hy, -X, band)
    if "t" in faces:
        side(-hx * X + hy * Y + hz * Z, X, -Z, 2 * hx, 2 * hz, Y, top or band)
    if "b" in faces:
        side(-hx * X - hy * Y - hz * Z, X, Z, 2 * hx, 2 * hz, -Y, top or band)


def revolve(m, band, centre, profile, a0=0.0, a1=360.0, seg=12, axis=None, inward=False, u_units=None, dt=1.0, v0=0.0,
            **kw):
    """Surface of revolution about the vertical through `centre` (or about
    `axis`: a rotation matrix whose Y column is the axis). `profile` is a
    list of (radius, height) points; each segment becomes a patch whose U
    runs round (u_units: units of texture the full circle takes) and V along
    the profile. Angles in degrees, counter-clockwise seen from above with 90 = north."""
    centre = vec(*centre)
    rot = np.eye(3) if axis is None else axis
    span = math.radians(a1 - a0)
    rmax = max(r for r, _ in profile)
    if u_units is None:
        u_units = max(1.0, round(2 * math.pi * rmax))
    circumference = u_units * (a1 - a0) / 360.0
    v = v0
    for (r0, y0), (r1, y1) in zip(profile[:-1], profile[1:]):
        length = math.hypot(r1 - r0, y1 - y0)
        if length < 1e-6:
            continue
        nr, ny = (y1 - y0) / length, -(r1 - r0) / length  # outward normal of the segment
        if inward:
            nr, ny = -nr, -ny

        def fn(S, T, r0=r0, y0=y0, r1=r1, y1=y1, length=length, nr=nr, ny=ny):
            a = math.radians(a0) + S / circumference * span
            k = T / length
            r = r0 + (r1 - r0) * k
            y = y0 + (y1 - y0) * k
            local = np.stack([r * np.cos(a), y, -r * np.sin(a)], axis=-1)
            normal = np.stack([nr * np.cos(a), np.full_like(a, ny), -nr * np.sin(a)], axis=-1)
            return centre + local @ rot.T, normal @ rot.T
        surf(m, band, fn, 0, circumference, 0, length, ds=circumference / max(3, int(seg * (a1 - a0) / 360.0 + 0.5)),
             dt=dt, v0=v, **kw)
        v += length


def cylinder(m, band, centre, r, y0, y1, seg=12, cap=None, cap_bottom=False, **kw):
    revolve(m, band, centre, [(r, y0), (r, y1)], seg=seg, **kw)
    if cap is not None:
        disc(m, cap, (centre[0], centre[1] + y1, centre[2]), 0, r, seg=seg, axis=kw.get("axis"), tint=kw.get("tint", 1.0))
    if cap_bottom:
        disc(m, cap or band, (centre[0], centre[1] + y0, centre[2]), 0, r, seg=seg, down=True, axis=kw.get("axis"),
             tint=kw.get("tint", 1.0))


def disc(m, band, centre, r0, r1, a0=0.0, a1=360.0, seg=12, down=False, axis=None, floor=False, **kw):
    """A flat ring (or disc) facing up (or down), mapped polar: U round, V outwards."""
    centre = vec(*centre)
    rot = np.eye(3) if axis is None else axis
    n = vec(0, -1 if down else 1, 0) @ rot.T
    span = math.radians(a1 - a0)
    u_units = max(1.0, round(2 * math.pi * r1)) * (a1 - a0) / 360.0

    def fn(S, T):
        a = math.radians(a0) + S / u_units * span
        r = r0 + T
        local = np.stack([r * np.cos(a), np.zeros_like(a), -r * np.sin(a)], axis=-1)
        return centre + local @ rot.T, n
    steps = max(3, int(seg * (a1 - a0) / 360.0 + 0.5))
    if floor:
        floor_surf(m, fn, 0, u_units, 0, r1 - r0, ds=u_units / steps, dt=kw.pop("dt", 0.5), **kw)
    else:
        surf(m, band, fn, 0, u_units, 0, r1 - r0, ds=u_units / steps, **kw)


def tube(m, band, points, r, seg=8, u0=0.0, closed_ends=False, **kw):
    """A pipe of radius r along a polyline (list of 3D points), U along its
    length and V round it (the band's height takes the whole circumference)."""
    pts = np.array(points, np.float64)
    keep = np.concatenate([[True], np.linalg.norm(np.diff(pts, axis=0), axis=1) > 1e-6])
    pts = pts[keep]
    d = np.diff(pts, axis=0)
    seglen = np.linalg.norm(d, axis=1)
    cum = np.concatenate([[0], np.cumsum(seglen)])
    tangents = np.zeros_like(pts)
    tangents[:-1] += d / seglen[:, None]
    tangents[1:] += d / seglen[:, None]
    tangents /= np.linalg.norm(tangents, axis=1, keepdims=True)
    # A frame carried along the line without twisting.
    up = vec(0, 1, 0) if abs(tangents[0][1]) < 0.9 else vec(1, 0, 0)
    side = np.cross(tangents[0], up)
    side /= np.linalg.norm(side)
    frames = []
    for t in tangents:
        side = side - t * side.dot(t)
        side /= np.linalg.norm(side)
        frames.append((side.copy(), np.cross(t, side)))
    ring = np.linspace(0, 2 * math.pi, seg + 1)
    P = np.zeros((len(pts), seg + 1, 3))
    N = np.zeros_like(P)
    for i, (a, b) in enumerate(frames):
        N[i] = np.cos(ring)[:, None] * a + np.sin(ring)[:, None] * b
        P[i] = pts[i] + r * N[i]
    width, height = PAGES[band.page]
    U = (u0 + cum)[:, None] * band.ppu / width * np.ones((1, seg + 1))
    V = (band.y0 + band.h * (ring / (2 * math.pi)))[None, :] / height * np.ones((len(pts), 1))
    m.add(band.page, P, N, np.stack([U, V], axis=-1), _grid_faces(len(pts) - 1, seg), kw.get("tint", 1.0))


def arc_points(centre, r, a0, a1, n, plane="xy"):
    """Points on an arc, for pipe elbows. plane: 'xy' (facing south), 'zy' or 'xz'."""
    out = []
    for i in range(n + 1):
        a = math.radians(a0 + (a1 - a0) * i / n)
        c, s = r * math.cos(a), r * math.sin(a)
        if plane == "xy":
            out.append((centre[0] + c, centre[1] + s, centre[2]))
        elif plane == "zy":
            out.append((centre[0], centre[1] + s, centre[2] + c))
        else:
            out.append((centre[0] + c, centre[1], centre[2] - s))
    return out


def polar(centre_x, centre_z, r, a):
    return centre_x + r * math.cos(math.radians(a)), centre_z - r * math.sin(math.radians(a))
