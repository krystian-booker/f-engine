"""A tiny orthographic ray-caster for character sprites.

A character is a list of primitives (ellipsoids, tapered capsules, boxes),
each an affine image of a canonical shape, posed in character space (units
are sprite pixels, +y up, +z the way the character faces, +x its left). The
caster looks at them like the game's camera does (pitched down), several
samples per sprite pixel, and returns per-sample buffers: depth, a key
(group and material) and the view-space normal. `pixel.py` turns those into
pixel art.
"""
import numpy as np

SIZE = 64          # frame size in pixels
SS = 4             # samples per pixel, per axis
GROUND = 60.0      # pixel row of the ground under the character's origin
PITCH = 17.0       # degrees the view looks down
FAR = 200.0

SPH, CONE, BOX = 0, 1, 2


def rx(a):
    c, s = np.cos(np.radians(a)), np.sin(np.radians(a))
    return np.array([[1, 0, 0], [0, c, -s], [0, s, c]], float)


def ry(a):
    c, s = np.cos(np.radians(a)), np.sin(np.radians(a))
    return np.array([[c, 0, s], [0, 1, 0], [-s, 0, c]], float)


def rz(a):
    c, s = np.cos(np.radians(a)), np.sin(np.radians(a))
    return np.array([[c, -s, 0], [s, c, 0], [0, 0, 1]], float)


def euler(pitch=0.0, yaw=0.0, roll=0.0):
    """Yaw about y, then pitch about x (positive leans forward, towards +z),
    then roll about z."""
    return ry(yaw) @ rx(pitch) @ rz(roll)


def frame(pos=(0, 0, 0), rot=None):
    m = np.eye(4)
    if rot is not None:
        m[:3, :3] = rot
    m[:3, 3] = pos
    return m


def xf(m, p):
    """Transform a point by a 4x4."""
    p = np.asarray(p, float)
    return m[:3, :3] @ p + m[:3, 3]


def xd(m, d):
    return m[:3, :3] @ np.asarray(d, float)


def norm(v):
    v = np.asarray(v, float)
    n = np.linalg.norm(v)
    return v / n if n > 1e-9 else v


class Prim:
    __slots__ = ("kind", "m", "par", "mat", "grp", "thin")

    def __init__(self, kind, m, par, mat, grp, thin=False):
        self.kind, self.m, self.par, self.mat, self.grp, self.thin = kind, m, par, mat, grp, thin

    def copy(self):
        return Prim(self.kind, self.m.copy(), self.par, self.mat, self.grp, self.thin)


def ball(centre, radii, mat, grp, rot=None, thin=False):
    """Ellipsoid: `radii` along the axes of `rot` (a 3x3, default identity)."""
    radii = np.broadcast_to(np.asarray(radii, float), (3,))
    m = np.eye(4)
    r = np.eye(3) if rot is None else rot
    m[:3, :3] = r * radii[None, :]
    m[:3, 3] = centre
    return Prim(SPH, m, None, mat, grp, thin)


def seg(a, b, ra, rb, mat, grp, xhint=(1, 0, 0), sx=1.0, sz=1.0, thin=False, cut=1e9):
    """Tapered capsule from a (radius ra) to b (radius rb). `sx`, `sz` scale
    its cross-section along the local x (nearest to `xhint`) and z. `cut`
    removes the rounded end beyond b + cut (an open hem)."""
    a, b = np.asarray(a, float), np.asarray(b, float)
    d = b - a
    h = np.linalg.norm(d)
    if h < 1e-6:
        return ball(a, max(ra, rb), mat, grp, thin=thin)
    y = d / h
    x = np.asarray(xhint, float)
    x = x - y * np.dot(x, y)
    if np.linalg.norm(x) < 1e-6:
        x = np.array([0, 0, 1.0]) - y * y[2]
    x = x / np.linalg.norm(x)
    z = np.cross(x, y)
    m = np.eye(4)
    m[:3, 0], m[:3, 1], m[:3, 2], m[:3, 3] = x * sx, y, z * sz, a
    return Prim(CONE, m, (h, ra, rb, cut), mat, grp, thin)


def box(centre, half, mat, grp, rot=None, thin=False):
    half = np.asarray(half, float)
    m = np.eye(4)
    r = np.eye(3) if rot is None else rot
    m[:3, :3] = r * half[None, :]
    m[:3, 3] = centre
    return Prim(BOX, m, None, mat, grp, thin)


def view_matrix(yaw, pitch=None):
    """Character space to view space (x right, y up, z towards the viewer)."""
    return rx(PITCH if pitch is None else pitch) @ ry(yaw)


def project(view, p, ground=None):
    """Character-space point to (pixel x, pixel y, view z)."""
    v = view @ np.asarray(p, float)
    return SIZE / 2 + v[0], (GROUND if ground is None else ground) - v[1], v[2]


def _hit_sphere(o, d):
    a = d @ d
    b = d @ o
    c = np.einsum("ij,ij->j", o, o) - 1.0
    disc = b * b - a * c
    ok = disc > 0
    t = (-b - np.sqrt(np.where(ok, disc, 0))) / a
    p = o + t[None, :] * d[:, None]
    return ok, t, p, p


def _hit_box(o, d):
    d = np.where(np.abs(d) < 1e-9, 1e-9, d)
    t1 = (-1 - o) / d[:, None]
    t2 = (1 - o) / d[:, None]
    lo, hi = np.minimum(t1, t2), np.maximum(t1, t2)
    tn, tf = lo.max(axis=0), hi.min(axis=0)
    axis = lo.argmax(axis=0)
    ok = tn < tf
    p = o + tn[None, :] * d[:, None]
    n = np.zeros_like(o)
    n[axis, np.arange(o.shape[1])] = -np.sign(d)[axis]
    return ok, tn, p, n


def _hit_cone(o, d, h, ra, rb, cut=1e9):
    """Rounded cone from the origin (radius ra) to (0, h, 0) (radius rb);
    after Inigo Quilez's closed form. `d` need not be unit length."""
    s = np.linalg.norm(d)
    rd = d / s
    ba = np.array([0, h, 0.0])
    oa = o
    ob = o - ba[:, None]
    rr = ra - rb
    m0 = h * h
    m1 = h * oa[1]
    m2 = h * rd[1]
    m3 = rd @ oa
    m5 = np.einsum("ij,ij->j", oa, oa)
    m6 = rd @ ob
    m7 = np.einsum("ij,ij->j", ob, ob)
    d2 = m0 - rr * rr
    k2 = d2 - m2 * m2
    k1 = d2 * m3 - m1 * m2 + m2 * rr * ra
    k0 = d2 * m5 - m1 * m1 + m1 * rr * ra * 2.0 - m0 * ra * ra
    n = o.shape[1]
    t = np.full(n, np.inf)
    nrm = np.zeros_like(o)
    disc = k1 * k1 - k0 * k2
    if abs(k2) > 1e-9:
        okb = disc > 0
        tb = (-np.sqrt(np.where(okb, disc, 0)) - k1) / k2
        y = m1 - ra * rr + tb * m2
        okb &= (y > 0) & (y < d2)
        t = np.where(okb, tb, t)
        nb = d2 * (oa + tb[None, :] * rd[:, None]) - ba[:, None] * y[None, :]
        nrm = np.where(okb[None, :], nb, nrm)
    h1 = m3 * m3 - m5 + ra * ra
    ok1 = h1 > 0
    t1 = -m3 - np.sqrt(np.where(ok1, h1, 0))
    use = ok1 & (t1 < t)
    t = np.where(use, t1, t)
    nrm = np.where(use[None, :], (oa + t1[None, :] * rd[:, None]) / max(ra, 1e-6), nrm)
    h2 = m6 * m6 - m7 + rb * rb
    ok2 = h2 > 0
    t2 = -m6 - np.sqrt(np.where(ok2, h2, 0))
    use = ok2 & (t2 < t)
    t = np.where(use, t2, t)
    nrm = np.where(use[None, :], (ob + t2[None, :] * rd[:, None]) / max(rb, 1e-6), nrm)
    ok = np.isfinite(t)
    t = np.where(ok, t, 0)
    p = o + t[None, :] * rd[:, None]
    ok &= p[1] <= h + cut
    return ok, t / s, p, nrm


def _bounds(prim):
    if prim.kind == CONE:
        h, ra, rb = prim.par[:3]
        r = max(ra, rb)
        lo, hi = np.array([-r, -ra, -r]), np.array([r, h + rb, r])
    else:
        lo, hi = -np.ones(3), np.ones(3)
    return lo, hi


def cast(prims, yaw, pitch=None, ground=None):
    """Ray-cast the primitives. Returns buffers of SIZE*SS square:
    depth (view z of the surface, larger is nearer; -inf where empty),
    grp, mat (int16, -1 empty), normal (3 floats, view space), thin (bool)."""
    n = SIZE * SS
    ground = GROUND if ground is None else ground
    view = view_matrix(yaw, pitch)
    depth = np.full((n, n), -np.inf)
    grp = np.full((n, n), -1, np.int16)
    mat = np.full((n, n), -1, np.int16)
    thin = np.zeros((n, n), bool)
    normal = np.zeros((n, n, 3))
    for prim in prims:
        a = view @ prim.m[:3, :3]
        c = view @ prim.m[:3, 3]
        lo, hi = _bounds(prim)
        corners = np.array([[x, y, z] for x in (lo[0], hi[0]) for y in (lo[1], hi[1]) for z in (lo[2], hi[2])])
        pts = corners @ a.T + c
        x0 = int(np.floor((SIZE / 2 + pts[:, 0].min()) * SS)) - 1
        x1 = int(np.ceil((SIZE / 2 + pts[:, 0].max()) * SS)) + 1
        y0 = int(np.floor((ground - pts[:, 1].max()) * SS)) - 1
        y1 = int(np.ceil((ground - pts[:, 1].min()) * SS)) + 1
        x0, x1, y0, y1 = max(x0, 0), min(x1, n), max(y0, 0), min(y1, n)
        if x0 >= x1 or y0 >= y1:
            continue
        xs = (np.arange(x0, x1) + 0.5) / SS - SIZE / 2
        ys = ground - (np.arange(y0, y1) + 0.5) / SS
        gx, gy = np.meshgrid(xs, ys)
        count = gx.size
        origin = np.stack([gx.ravel(), gy.ravel(), np.full(count, FAR)])
        ainv = np.linalg.inv(a)
        o = ainv @ (origin - c[:, None])
        d = ainv @ np.array([0, 0, -1.0])
        if prim.kind == SPH:
            ok, t, p, nl = _hit_sphere(o, d)
        elif prim.kind == BOX:
            ok, t, p, nl = _hit_box(o, d)
        else:
            ok, t, p, nl = _hit_cone(o, d, *prim.par)
        z = FAR - t
        sub = depth[y0:y1, x0:x1].ravel()
        ok &= z > sub
        if not ok.any():
            continue
        idx = np.nonzero(ok)[0]
        nv = (ainv.T @ nl[:, idx])
        nv /= np.maximum(np.linalg.norm(nv, axis=0), 1e-9)[None, :]
        m = prim.mat(p[:, idx]) if callable(prim.mat) else prim.mat
        iy, ix = np.divmod(idx, x1 - x0)
        depth[y0 + iy, x0 + ix] = z[idx]
        grp[y0 + iy, x0 + ix] = prim.grp
        mat[y0 + iy, x0 + ix] = m
        thin[y0 + iy, x0 + ix] = prim.thin
        normal[y0 + iy, x0 + ix] = nv.T
    return depth, grp, mat, normal, thin
