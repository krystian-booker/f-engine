"""Mesh builder for the fortress: faces are collected per material in world
space (x east, y up, z south), with world-projected UVs at 16 texels per unit
for the tiling stone textures and strip UVs at 32 for the prop atlas.
blender_io.py turns the result into Blender meshes, bakes AO and exports."""
import math
import numpy as np
from textures import STRIPS, ATLAS

TEX = {"wall": 256, "floor": 256, "rock": 256, "block": 128, "mist": 256}
TEXELS = 16.0


def v3(*a):
    return np.array(a if len(a) == 3 else a[0], dtype=np.float64)


def rot_y(a):
    c, s = math.cos(a), math.sin(a)
    return np.array([[c, 0, s], [0, 1, 0], [-s, 0, c]])


def rot_x(a):
    c, s = math.cos(a), math.sin(a)
    return np.array([[1, 0, 0], [0, c, -s], [0, s, c]])


def rot_z(a):
    c, s = math.cos(a), math.sin(a)
    return np.array([[c, -s, 0], [s, c, 0], [0, 0, 1]])


def euler(yaw=0.0, pitch=0.0, roll=0.0):
    return rot_y(yaw) @ rot_x(pitch) @ rot_z(roll)


def frame_from(direction, roll=0.0):
    """Rotation whose local +x points along `direction`."""
    d = v3(direction)
    d = d / np.linalg.norm(d)
    up = v3(0, 1, 0) if abs(d[1]) < 0.95 else v3(0, 0, 1)
    z = np.cross(d, up)
    z /= np.linalg.norm(z)
    y = np.cross(z, d)
    m = np.stack([d, y, z], 1)
    return m @ rot_x(roll)


class Kit:
    def __init__(self):
        self.faces = {}  # material -> list of (P, UV, C, N)
        self.rng = np.random.default_rng(7)
        self.xf = []  # transform stack: (R, t)

    # ------------------------------------------------------------------ core
    def poly(self, mat, pts, uv=None, col=(1, 1, 1), out=None, normals=None, uvoff=(0, 0)):
        P = np.asarray(pts, dtype=np.float64)
        k = len(P)
        if self.xf:
            R, t = self.xf[-1]
            P = P @ R.T + t
            if out is not None:
                out = R @ np.asarray(out, dtype=np.float64)
            if normals is not None:
                normals = np.asarray(normals, dtype=np.float64) @ R.T
        n = np.zeros(3)
        for i in range(1, k - 1):
            n += np.cross(P[i] - P[0], P[i + 1] - P[0])
        ln = np.linalg.norm(n)
        if ln < 1e-12:
            return
        n /= ln
        flip = out is not None and float(np.dot(n, out)) < 0
        if flip:
            n = -n
        if uv is None:
            uv = self.world_uv(mat, P, n, uvoff)
        UV = np.asarray(uv, dtype=np.float64)
        C = np.asarray(col, dtype=np.float64)
        if C.ndim == 1:
            C = np.tile(C[:3], (k, 1))
        N = np.tile(n, (k, 1)) if normals is None else np.asarray(normals, dtype=np.float64)
        if flip:
            P, UV, C, N = P[::-1], UV[::-1], C[::-1], N[::-1]
        self.faces.setdefault(mat, []).append((P.copy(), UV.copy(), C.copy(), N.copy()))

    def at(self, pos=(0, 0, 0), yaw=0.0, pitch=0.0, roll=0.0, scale=1.0):
        """Context manager: everything emitted inside is rotated and moved."""
        kit = self

        class _Ctx:
            def __enter__(self_):
                R = euler(yaw, pitch, roll) * scale
                t = v3(pos)
                if kit.xf:
                    R0, t0 = kit.xf[-1]
                    R, t = R0 @ R, R0 @ t + t0
                kit.xf.append((R, t))

            def __exit__(self_, *a):
                kit.xf.pop()

        return _Ctx()

    def world_uv(self, mat, P, n, off=(0, 0)):
        s = TEXELS / TEX.get(mat, 256)
        ax = int(np.argmax(np.abs(n)))
        if ax == 1:
            u, v = P[:, 0], P[:, 2]
        elif ax == 0:
            u, v = P[:, 2] * (1 if n[0] < 0 else -1), -P[:, 1]
        else:
            u, v = P[:, 0] * (1 if n[2] > 0 else -1), -P[:, 1]
        return np.stack([(u + off[0]) * s, (v + off[1]) * s], 1)

    def _col(self, col, P, n):
        if callable(col):
            return np.array([col(p, n) for p in P])
        return col

    # ------------------------------------------------------------------ flat pieces
    def grid(self, mat, o, a, b, na=1, nb=1, col=(1, 1, 1), out=None, uvoff=(0, 0), disp=None):
        """Parallelogram o + s*a + t*b split into na x nb quads."""
        o, a, b = v3(o), v3(a), v3(b)
        n = np.cross(a, b)
        n /= np.linalg.norm(n)
        if out is not None and np.dot(n, out) < 0:
            n = -n
        for i in range(na):
            for j in range(nb):
                P = [o + a * (i / na) + b * (j / nb), o + a * ((i + 1) / na) + b * (j / nb),
                     o + a * ((i + 1) / na) + b * ((j + 1) / nb), o + a * (i / na) + b * ((j + 1) / nb)]
                if disp is not None:
                    P = [p + n * disp(p) for p in P]
                P = np.array(P)
                self.poly(mat, P, col=self._col(col, P, n), out=n if out is None else out, uvoff=uvoff)

    def box(self, mat, lo, hi, faces="xXyYzZ", tess=1.0, col=(1, 1, 1), uvoff=(0, 0), mats=None):
        """Axis-aligned box; `faces` picks sides (lower case = negative side).
        `mats` maps a face letter to another material (e.g. {'Y': 'floor'})."""
        lo, hi = v3(lo), v3(hi)
        d = hi - lo
        spec = {
            "x": (lo, v3(0, 0, d[2]), v3(0, d[1], 0), v3(-1, 0, 0)),
            "X": (v3(hi[0], lo[1], lo[2]), v3(0, 0, d[2]), v3(0, d[1], 0), v3(1, 0, 0)),
            "y": (lo, v3(d[0], 0, 0), v3(0, 0, d[2]), v3(0, -1, 0)),
            "Y": (v3(lo[0], hi[1], lo[2]), v3(d[0], 0, 0), v3(0, 0, d[2]), v3(0, 1, 0)),
            "z": (lo, v3(d[0], 0, 0), v3(0, d[1], 0), v3(0, 0, -1)),
            "Z": (v3(lo[0], lo[1], hi[2]), v3(d[0], 0, 0), v3(0, d[1], 0), v3(0, 0, 1)),
        }
        for f in faces:
            o, a, b, n = spec[f]
            na = max(1, int(math.ceil(np.linalg.norm(a) / tess - 1e-6)))
            nb = max(1, int(math.ceil(np.linalg.norm(b) / tess - 1e-6)))
            self.grid((mats or {}).get(f, mat), o, a, b, na, nb, col=col, out=n, uvoff=uvoff)

    def cbox(self, mat, centre, half, c=0.06, rot=None, col=(1, 1, 1), bottom=False, chip=0.35, uvoff=None,
             strip=None):
        """Bevelled block: chamfered edges, some corners knocked off."""
        r = self.rng
        centre, half = v3(centre), v3(half)
        R = np.eye(3) if rot is None else rot
        c = min(c, half.min() * 0.6)
        if uvoff is None:
            uvoff = r.random(2) * 8
        signs = [(sx, sy, sz) for sx in (-1, 1) for sy in (-1, 1) for sz in (-1, 1)]
        cc = {}
        for s in signs:
            k = c
            if s[1] > 0 and r.random() < chip:
                k = c * r.uniform(1.8, 4.0)
            cc[s] = min(k, half.min() * 0.9)

        def fv(axis, s):  # vertex of corner s on the face perpendicular to `axis`
            p = np.array([s[0] * half[0], s[1] * half[1], s[2] * half[2]])
            for a in range(3):
                if a != axis:
                    p[a] -= s[a] * cc[s]
            return p

        def emit(local, nrm):
            L = np.array(local)
            W = L @ R.T + centre
            n = R @ nrm
            if not bottom and nrm[1] < -0.9:
                return
            if strip:
                y0, h = STRIPS[strip]
                ax = int(np.argmax(np.abs(nrm)))
                ua, va = [(2, 1), (0, 2), (0, 1)][ax]
                uvs = np.stack([(L[:, ua] + uvoff[0]) * 32.0 / ATLAS,
                                (y0 + 1 + np.clip((L[:, va] - L[:, va].min()) * 32.0, 0, h - 2)) / ATLAS], 1)
            else:
                s_ = TEXELS / TEX.get(mat, 256)
                ax = int(np.argmax(np.abs(nrm)))
                ua, va = [(2, 1), (0, 2), (0, 1)][ax]
                uvs = np.stack([(L[:, ua] + uvoff[0]) * s_, (-L[:, va] + uvoff[1]) * s_], 1)
            self.poly(mat, W, uv=uvs, col=self._col(col, W, n), out=n)

        # faces
        for axis in range(3):
            for sgn in (-1, 1):
                others = [a for a in range(3) if a != axis]
                ring = []
                for sa, sb in ((-1, -1), (1, -1), (1, 1), (-1, 1)):
                    s = [0, 0, 0]
                    s[axis] = sgn
                    s[others[0]] = sa
                    s[others[1]] = sb
                    ring.append(fv(axis, tuple(s)))
                nrm = np.zeros(3)
                nrm[axis] = sgn
                emit(ring, nrm)
        # edges
        for axis in range(3):  # edge runs along `axis`
            o1, o2 = [a for a in range(3) if a != axis]
            for s1 in (-1, 1):
                for s2 in (-1, 1):
                    sa = [0, 0, 0]
                    sb = [0, 0, 0]
                    sa[axis], sb[axis] = -1, 1
                    sa[o1] = sb[o1] = s1
                    sa[o2] = sb[o2] = s2
                    sa, sb = tuple(sa), tuple(sb)
                    nrm = np.zeros(3)
                    nrm[o1] = s1
                    nrm[o2] = s2
                    nrm /= np.linalg.norm(nrm)
                    emit([fv(o1, sa), fv(o1, sb), fv(o2, sb), fv(o2, sa)], nrm)
        # corners
        for s in signs:
            nrm = np.array(s, dtype=float) / math.sqrt(3)
            emit([fv(0, s), fv(1, s), fv(2, s)], nrm)

    def prism(self, mat, cx, cz, r0, r1, y0, y1, n=8, rot=0.0, cap=True, col=(1, 1, 1), tess=2.0, squash=(1, 1),
              smooth=False, uvoff=(0, 0), bottom=False, strip=None, arc=(0, 1)):
        """n-gon frustum from radius r0 at y0 to r1 at y1."""
        ny = max(1, int(math.ceil((y1 - y0) / tess)))
        a0, a1 = arc
        segs = n if (a1 - a0) >= 0.999 else max(1, int(round(n * (a1 - a0))))

        def ring(t):
            r = r0 + (r1 - r0) * t
            y = y0 + (y1 - y0) * t
            return [v3(cx + math.sin(rot + (a0 + (a1 - a0) * i / segs) * 2 * math.pi) * r * squash[0], y,
                       cz + math.cos(rot + (a0 + (a1 - a0) * i / segs) * 2 * math.pi) * r * squash[1])
                    for i in range(segs + 1)]

        per = 2 * math.pi * max(r0, r1) / n
        for j in range(ny):
            A, B = ring(j / ny), ring((j + 1) / ny)
            for i in range(segs):
                P = np.array([A[i], A[i + 1], B[i + 1], B[i]])
                mid = P.mean(0)
                out = v3(mid[0] - cx, 0.0, mid[2] - cz)
                if strip:
                    sy, sh = STRIPS[strip]
                    u0, u1 = i * per * 32 / ATLAS, (i + 1) * per * 32 / ATLAS
                    va = (sy + 1 + (sh - 2) * (1 - j / ny)) / ATLAS
                    vb = (sy + 1 + (sh - 2) * (1 - (j + 1) / ny)) / ATLAS
                    uv = [(u0, va), (u1, va), (u1, vb), (u0, vb)]
                else:
                    s_ = TEXELS / TEX.get(mat, 256)
                    u0, u1 = (i * per + uvoff[0]) * s_, ((i + 1) * per + uvoff[0]) * s_
                    uv = [(u0, -P[0][1] * s_), (u1, -P[1][1] * s_), (u1, -P[2][1] * s_), (u0, -P[3][1] * s_)]
                normals = None
                if smooth:
                    normals = []
                    for p in P:
                        nn = v3(p[0] - cx, 0, p[2] - cz)
                        normals.append(nn / (np.linalg.norm(nn) + 1e-9))
                nf = out / (np.linalg.norm(out) + 1e-9)
                self.poly(mat, P, uv=uv, col=self._col(col, P, nf), out=out, normals=normals)
        if cap:
            top = ring(1.0)[:-1] if segs == n else ring(1.0)
            P = np.array(top)
            self.poly(mat if not strip else mat, P, col=self._col(col, P, v3(0, 1, 0)), out=v3(0, 1, 0),
                      uv=None if not strip else [((p[0]) * 32 / ATLAS, (STRIPS[strip][0] + 4) / ATLAS) for p in P])
        if bottom:
            P = np.array(ring(0.0)[:-1])
            self.poly(mat, P, col=self._col(col, P, v3(0, -1, 0)), out=v3(0, -1, 0),
                      uv=None if not strip else [((p[0]) * 32 / ATLAS, (STRIPS[strip][0] + 4) / ATLAS) for p in P])

    # ------------------------------------------------------------------ prop atlas pieces
    def strip_quad(self, strip, P, u0, u1, v0=0.0, v1=1.0, col=(1, 1, 1), out=None, two_sided=False, mat="props"):
        """Quad P[0..3] (P0->P1 runs along U, P0->P3 along V) from a strip."""
        y0, h = STRIPS[strip]
        ua, ub = u0 * 32.0 / ATLAS, u1 * 32.0 / ATLAS
        va, vb = (y0 + 0.5 + v0 * (h - 1)) / ATLAS, (y0 + 0.5 + v1 * (h - 1)) / ATLAS
        uv = [(ua, va), (ub, va), (ub, vb), (ua, vb)]
        P = np.asarray(P, dtype=np.float64)
        self.poly(mat, P, uv=uv, col=col, out=out)
        if two_sided:
            n = np.cross(P[1] - P[0], P[3] - P[0])
            if out is not None and np.dot(n, out) < 0:
                n = -n
            self.poly(mat, P, uv=uv, col=col, out=-n)

    def beam(self, strip, p0, p1, w, h, col=(1, 1, 1), roll=0.0, caps=True, voff=None, mat="props", u0=None):
        """Timber or metal bar from p0 to p1 with a w x h section."""
        p0, p1 = v3(p0), v3(p1)
        L = float(np.linalg.norm(p1 - p0))
        if L < 1e-6:
            return
        R = frame_from(p1 - p0, roll)
        y0, sh = STRIPS[strip]
        u_start = float(self.rng.random()) * 16 if u0 is None else u0
        rows = max(1, sh // 16)

        def face(local, nrm, across, row):
            L_ = np.array(local)
            W = L_ @ R.T + p0
            # V: one plank row (16 px = 0.5 unit) per 0.5 unit of width, clamped to the strip
            span = min(sh - 2, max(4.0, across * 32.0))
            base = y0 + 1 + (row % rows) * 16 if span <= 14 else y0 + 1
            span = min(span, y0 + sh - 1 - base)
            uvs = []
            for q in L_:
                t = q[1] if abs(nrm[2]) > 0.5 else q[2]
                tmin = -h / 2 if abs(nrm[2]) > 0.5 else -w / 2
                size = h if abs(nrm[2]) > 0.5 else w
                uvs.append(((u_start + q[0]) * 32.0 / ATLAS, (base + (t - tmin) / size * span) / ATLAS))
            n = R @ nrm
            self.poly(mat, W, uv=uvs, col=self._col(col, W, n), out=n)

        hw, hh = w / 2, h / 2
        row = int(self.rng.integers(0, 4)) if voff is None else voff
        face([(0, -hh, hw), (L, -hh, hw), (L, hh, hw), (0, hh, hw)], v3(0, 0, 1), h, row)
        face([(0, -hh, -hw), (L, -hh, -hw), (L, hh, -hw), (0, hh, -hw)], v3(0, 0, -1), h, row + 1)
        face([(0, hh, -hw), (L, hh, -hw), (L, hh, hw), (0, hh, hw)], v3(0, 1, 0), w, row + 2)
        face([(0, -hh, -hw), (L, -hh, -hw), (L, -hh, hw), (0, -hh, hw)], v3(0, -1, 0), w, row + 3)
        if caps:
            for x, nx in ((0, -1), (L, 1)):
                Lc = np.array([(x, -hh, -hw), (x, -hh, hw), (x, hh, hw), (x, hh, -hw)])
                W = Lc @ R.T + p0
                uvs = [((u_start + 0.1 + (q[2] + hw)) * 32.0 / ATLAS, (y0 + 2 + (q[1] + hh) / h * 12) / ATLAS)
                       for q in Lc]
                n = R @ v3(nx, 0, 0)
                self.poly(mat, W, uv=uvs, col=self._col(col, W, n) if callable(col) else np.asarray(col) * 0.8,
                          out=n)

    def panel(self, strip, centre, half, rot=None, col=(1, 1, 1), grain=0):
        """Flat board/plate box; `grain` is the local axis U runs along."""
        self.cbox("props", centre, half, c=0.0, rot=rot, col=col, bottom=True, chip=0, strip=strip)

    # ------------------------------------------------------------------ heightfield of columns
    def columns(self, H, x0, z0, s, top_mat, side_mat, base=-2.0, col=None, vt=1.0, skip_top=None):
        """Columns of height H[j, i] (NaN = nothing) on cells of size s.
        top_mat/side_mat: material name, or arrays of names per cell."""
        nz, nx = H.shape

        def tm(j, i):
            return top_mat if isinstance(top_mat, str) else top_mat[j][i]

        def sm(j, i):
            return side_mat if isinstance(side_mat, str) else side_mat[j][i]

        def hv(j, i):
            if 0 <= j < nz and 0 <= i < nx and not np.isnan(H[j, i]):
                return H[j, i]
            return None

        for j in range(nz):
            for i in range(nx):
                h = hv(j, i)
                if h is None:
                    continue
                xa, xb = x0 + i * s, x0 + (i + 1) * s
                za, zb = z0 + j * s, z0 + (j + 1) * s
                if skip_top is None or not skip_top[j][i]:
                    P = np.array([(xa, h, za), (xa, h, zb), (xb, h, zb), (xb, h, za)])
                    n = v3(0, 1, 0)
                    self.poly(tm(j, i), P, col=self._col(col, P, n) if col else (1, 1, 1), out=n)
                for (dj, di, pa, pb, n) in (
                        (0, -1, (xa, za), (xa, zb), v3(-1, 0, 0)), (0, 1, (xb, zb), (xb, za), v3(1, 0, 0)),
                        (-1, 0, (xb, za), (xa, za), v3(0, 0, -1)), (1, 0, (xa, zb), (xb, zb), v3(0, 0, 1))):
                    o = hv(j + dj, i + di)
                    low = base if o is None else o
                    if low >= h - 1e-6:
                        continue
                    # split at whole multiples of vt
                    ys = [low]
                    y = math.floor(low / vt + 1e-6) * vt + vt
                    while y < h - 1e-6:
                        ys.append(y)
                        y += vt
                    ys.append(h)
                    for ya, yb in zip(ys[:-1], ys[1:]):
                        P = np.array([(pa[0], ya, pa[1]), (pb[0], ya, pb[1]), (pb[0], yb, pb[1]), (pa[0], yb, pa[1])])
                        self.poly(sm(j, i), P, col=self._col(col, P, n) if col else (1, 1, 1), out=n)

    # ------------------------------------------------------------------ organic pieces
    def mound(self, mat, cx, cz, rx, rz, h, y0=0.0, seed=0, res=0.5, col=(1, 1, 1), power=1.4, lean=(0, 0),
              clip=None):
        """Faceted rubble heap: a noisy dome on a jittered grid."""
        r = np.random.default_rng(seed)
        nx = max(2, int(math.ceil(2 * rx / res)))
        nz = max(2, int(math.ceil(2 * rz / res)))
        pts = {}
        for j in range(nz + 1):
            for i in range(nx + 1):
                u, w = i / nx * 2 - 1, j / nz * 2 - 1
                x = cx + u * rx + (r.uniform(-.3, .3) * res if 0 < i < nx else 0)
                z = cz + w * rz + (r.uniform(-.3, .3) * res if 0 < j < nz else 0)
                d = math.hypot(u - lean[0] * 0.5, w - lean[1] * 0.5)
                t = max(0.0, 1 - d ** power)
                y = y0 + h * t * (0.75 + 0.5 * r.random()) if t > 0 else y0 - 0.05
                if clip is not None:
                    y = min(y, clip(x, z))
                pts[(i, j)] = v3(x, y, z)
        for j in range(nz):
            for i in range(nx):
                a, b, c_, d_ = pts[(i, j)], pts[(i, j + 1)], pts[(i + 1, j + 1)], pts[(i + 1, j)]
                if max(a[1], b[1], c_[1], d_[1]) <= y0 - 0.04:
                    continue
                for tri in ((a, b, c_), (a, c_, d_)) if (i + j) % 2 else ((a, b, d_), (b, c_, d_)):
                    P = np.array(tri)
                    self.poly(mat, P, col=self._col(col, P, v3(0, 1, 0)), out=v3(0, 1, 0))

    def rubble(self, cx, cz, radius, count, size=(0.12, 0.4), y=0.0, seed=0, mat="block", col=(1, 1, 1),
               height=None, squash=(1, 1), avoid=None):
        """Scattered loose stones (bevelled, tumbled)."""
        r = np.random.default_rng(seed)
        keep = self.rng
        self.rng = r
        for _ in range(count):
            a = r.uniform(0, 2 * math.pi)
            d = radius * math.sqrt(r.random())
            x, z = cx + math.cos(a) * d * squash[0], cz + math.sin(a) * d * squash[1]
            if avoid is not None and avoid(x, z):
                continue
            s = r.uniform(*size)
            half = v3(s * r.uniform(0.7, 1.5), s * r.uniform(0.45, 0.9), s * r.uniform(0.7, 1.3))
            yy = (height(x, z) if height else y)
            R = euler(r.uniform(0, 6.28), r.uniform(-0.35, 0.35), r.uniform(-0.35, 0.35))
            k = r.uniform(0.75, 1.0)
            cc = np.asarray(col if not callable(col) else col(v3(x, yy, z), v3(0, 1, 0))) * k
            self.cbox(mat, (x, yy + half[1] * 0.7, z), half, c=s * 0.18, rot=R, col=cc, bottom=False, chip=0.5)
        self.rng = keep

    # ------------------------------------------------------------------ output
    def arrays(self):
        """material -> dict(P (n,3,3), UV (n,3,2), C (n,3,3), N (n,3,3)) triangles."""
        out = {}
        for mat, faces in self.faces.items():
            P, UV, C, N = [], [], [], []
            for (p, uv, c, n) in faces:
                for i in range(1, len(p) - 1):
                    idx = [0, i, i + 1]
                    P.append(p[idx])
                    UV.append(uv[idx])
                    C.append(c[idx])
                    N.append(n[idx])
            out[mat] = {"P": np.array(P), "UV": np.array(UV), "C": np.array(C), "N": np.array(N)}
        return out

    def triangle_count(self):
        return sum(len(p) - 2 for faces in self.faces.values() for (p, _, _, _) in faces)
