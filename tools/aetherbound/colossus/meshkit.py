"""Procedural mesh and pixel-texture kit for the Aetherbound models.

Geometry is built in Blender coordinates (x = the figure's left, -y = forward,
z = up) as plain arrays: every vertex already is a glTF vertex (one normal,
one UV, up to four bone weights), so the vertex budget is exact. Every surface
gets a chart in a texture atlas at a fixed texel density; the painter fills
charts texel by texel from the 3D position, normal and distance to the plate's
edge, which is what gives the hand-pixelled plates (rim light, outline, rivets,
inlay lines) instead of a blurry bake.
"""
import math
import numpy as np

UP = np.array([0.0, 0.0, 1.0])


def vec(*a):
    return np.array(a if len(a) > 1 else a[0], dtype=float)


def unit(v):
    v = np.asarray(v, dtype=float)
    n = np.linalg.norm(v)
    return v / n if n > 1e-12 else v


def rot_x(deg):
    a = math.radians(deg); c, s = math.cos(a), math.sin(a)
    return np.array([[1, 0, 0], [0, c, -s], [0, s, c]], dtype=float)


def rot_y(deg):
    a = math.radians(deg); c, s = math.cos(a), math.sin(a)
    return np.array([[c, 0, s], [0, 1, 0], [-s, 0, c]], dtype=float)


def rot_z(deg):
    a = math.radians(deg); c, s = math.cos(a), math.sin(a)
    return np.array([[c, -s, 0], [s, c, 0], [0, 0, 1]], dtype=float)


def euler(pitch=0.0, roll=0.0, yaw=0.0):
    """pitch about x (positive bows forward), roll about y (leans left), yaw about z (turns left)."""
    return rot_z(yaw) @ rot_x(pitch) @ rot_y(roll)


def xf(pos=(0, 0, 0), rot=None):
    m = np.eye(4)
    if rot is not None:
        m[:3, :3] = rot
    m[:3, 3] = pos
    return m


def aim(origin, z_axis, x_hint=(1, 0, 0)):
    """A frame at origin whose local z points along z_axis."""
    z = unit(z_axis)
    x = np.asarray(x_hint, dtype=float)
    x = x - z * np.dot(x, z)
    if np.linalg.norm(x) < 1e-6:
        x = np.array([0.0, 1.0, 0.0]) - z * z[1]
    x = unit(x)
    y = np.cross(z, x)
    m = np.eye(4)
    m[:3, 0], m[:3, 1], m[:3, 2], m[:3, 3] = x, y, z, origin
    return m


def apply(m, p):
    p = np.asarray(p, dtype=float)
    return p @ m[:3, :3].T + m[:3, 3]


class Chart:
    def __init__(self, cw, ch, grid_p, grid_n, cls, poly=None, margin=2, open_u=False, params=None):
        self.cw, self.ch = float(cw), float(ch)
        self.margin = int(margin)
        self.w = int(math.ceil(cw)) + 2 * self.margin
        self.h = int(math.ceil(ch)) + 2 * self.margin
        self.grid_p = np.asarray(grid_p, dtype=float)   # [rows, cols, 3] over the content rectangle
        self.grid_n = np.asarray(grid_n, dtype=float)
        self.cls = cls
        self.poly = poly                                  # convex polygon in content texels, or None for the rectangle
        self.open_u = open_u                              # left and right edges wrap (tubes): no edge there
        self.params = dict(params or {})
        self.x = self.y = 0
        self.atlas = "body"


class Builder:
    def __init__(self, density=32.0):
        self.density = density
        self.pos, self.nrm, self.uv, self.chart_of, self.weights, self.mat_of_tri = [], [], [], [], [], []
        self.tris = []
        self.charts = []
        self.state = dict(bone="root", cls="bronze", mat="body", params={}, density=None)
        self._mirror = None

    # ---- state -----------------------------------------------------------------------------
    def set(self, **kw):
        for k, v in kw.items():
            if k not in self.state:
                raise KeyError(k)
            self.state[k] = v

    def D(self):
        return self.state["density"] or self.density

    def _weights(self, t=None):
        b = self.state["bone"]
        if callable(b):
            return b(t)
        if isinstance(b, str):
            return [(b, 1.0)]
        return list(b)

    def _chart(self, cw, ch, gp, gn, poly=None, margin=2, open_u=False, extra=None):
        params = dict(self.state["params"])
        if extra:
            params.update(extra)
        c = Chart(cw, ch, gp, gn, self.state["cls"], poly, margin, open_u, params)
        c.atlas = self.state["mat"]
        self.charts.append(c)
        return len(self.charts) - 1

    def _v(self, p, n, chart, px, py, w=None):
        self.pos.append(np.asarray(p, dtype=float))
        self.nrm.append(unit(n))
        self.uv.append((float(px), float(py)))
        self.chart_of.append(chart)
        self.weights.append(w if w is not None else self._weights())
        return len(self.pos) - 1

    def _tri(self, a, b, c):
        self.tris.append((a, b, c))
        self.mat_of_tri.append(self.state["mat"])

    def _quad(self, a, b, c, d):
        self._tri(a, b, c)
        self._tri(a, c, d)

    # ---- mirroring ---------------------------------------------------------------------------
    def mirror_begin(self):
        self._mirror = (len(self.pos), len(self.tris), len(self.charts))

    def mirror_end(self, share=True):
        """Copy everything since mirror_begin to the other side (x -> -x, _L <-> _R)."""
        v0, t0, c0 = self._mirror
        self._mirror = None
        nv = len(self.pos)
        chart_map = {}
        if not share:
            for ci in range(c0, len(self.charts)):
                c = self.charts[ci]
                gp = c.grid_p.copy(); gp[..., 0] *= -1
                gn = c.grid_n.copy(); gn[..., 0] *= -1
                m = Chart(c.cw, c.ch, gp, gn, c.cls, c.poly, c.margin, c.open_u, c.params)
                m.atlas = c.atlas
                self.charts.append(m)
                chart_map[ci] = len(self.charts) - 1

        def swap(name):
            if name.endswith("_L"):
                return name[:-2] + "_R"
            if name.endswith("_R"):
                return name[:-2] + "_L"
            return name
        for i in range(v0, nv):
            p = self.pos[i].copy(); p[0] *= -1
            n = self.nrm[i].copy(); n[0] *= -1
            self.pos.append(p); self.nrm.append(n); self.uv.append(self.uv[i])
            self.chart_of.append(chart_map.get(self.chart_of[i], self.chart_of[i]))
            self.weights.append([(swap(b), w) for b, w in self.weights[i]])
        for ti in range(t0, len(self.tris)):
            a, b, c = self.tris[ti]
            self.tris.append((a - v0 + nv, c - v0 + nv, b - v0 + nv))
            self.mat_of_tri.append(self.mat_of_tri[ti])

    # ---- planar chart helper -----------------------------------------------------------------
    def _planar(self, pts, normal, margin=2, extra=None, up_hint=None):
        """Chart for planar points; returns (chart index, function 3D point -> content texel)."""
        n = unit(normal)
        up = UP if up_hint is None else unit(up_hint)
        v = up - n * np.dot(up, n)
        if np.linalg.norm(v) < 0.35:
            alt = np.array([0.0, -1.0, 0.0])
            v = alt - n * np.dot(alt, n)
            if np.linalg.norm(v) < 0.35:
                alt = np.array([1.0, 0.0, 0.0])
                v = alt - n * np.dot(alt, n)
        v = unit(v)
        u = np.cross(v, n)
        pts = np.asarray(pts, dtype=float)
        us, vs = pts @ u, pts @ v
        u0, v1 = us.min(), vs.max()
        D = self.D()
        cw, ch = max((us.max() - u0) * D, 1.0), max((v1 - vs.min()) * D, 1.0)
        o = n * np.dot(pts[0], n) + u * u0 + v * v1          # 3D point of content texel (0, 0)
        gp = np.array([[o, o + u * cw / D], [o - v * ch / D, o + u * cw / D - v * ch / D]])
        gn = np.broadcast_to(n, (2, 2, 3)).copy()
        poly = [((p @ u - u0) * D, (v1 - p @ v) * D) for p in pts]
        ci = self._chart(cw, ch, gp, gn, poly, margin, extra=extra)

        def to_px(p):
            return (np.dot(p, u) - u0) * D, (v1 - np.dot(p, v)) * D
        return ci, to_px

    # ---- primitives --------------------------------------------------------------------------
    def hexa(self, corners, chamfer=0.04, skip="", M=None, extra=None):
        """A hexahedron from corners[ix][iy][iz] (2x2x2x3), chamfered edges with rounded normals.
        skip names faces not to build: any of -x +x -y +y -z +z."""
        c = np.asarray(corners, dtype=float).reshape(2, 2, 2, 3)
        if M is not None:
            c = apply(M, c.reshape(-1, 3)).reshape(2, 2, 2, 3)
        centre = c.reshape(-1, 3).mean(axis=0)
        faces = {
            "-x": [(0, 0, 0), (0, 0, 1), (0, 1, 1), (0, 1, 0)], "+x": [(1, 0, 0), (1, 1, 0), (1, 1, 1), (1, 0, 1)],
            "-y": [(0, 0, 0), (1, 0, 0), (1, 0, 1), (0, 0, 1)], "+y": [(0, 1, 0), (0, 1, 1), (1, 1, 1), (1, 1, 0)],
            "-z": [(0, 0, 0), (0, 1, 0), (1, 1, 0), (1, 0, 0)], "+z": [(0, 0, 1), (1, 0, 1), (1, 1, 1), (0, 1, 1)],
        }
        built = {}
        for name, idx in faces.items():
            if name in skip:
                continue
            P = np.array([c[i] for i in idx])
            n = unit(np.cross(P[2] - P[0], P[3] - P[1]))
            if np.dot(n, P.mean(axis=0) - centre) < 0:
                idx = idx[::-1]; P = P[::-1]; n = -n
            ins = []
            for k in range(4):
                e1, e2 = unit(P[(k + 1) % 4] - P[k]), unit(P[(k - 1) % 4] - P[k])
                ins.append(P[k] + chamfer * (e1 + e2))
            ci, to_px = self._planar(P, n, extra=extra)
            vid = [self._v(ins[k], n, ci, *to_px(ins[k])) for k in range(4)]
            self._quad(*vid)
            built[name] = dict(idx=idx, P=P, n=n, ins=ins, ci=ci, to_px=to_px, vid=vid,
                               area=np.linalg.norm(np.cross(P[1] - P[0], P[3] - P[0])), dup={})
        if chamfer <= 0:
            return

        def dup(owner, other, corner):
            """A vertex at `other`'s inset position for `corner`, textured from `owner`'s chart."""
            fo, ft = built[owner], built[other]
            key = (other, corner)
            if key not in fo["dup"]:
                p = ft["ins"][ft["idx"].index(corner)]
                fo["dup"][key] = self._v(p, ft["n"], fo["ci"], *fo["to_px"](p))
            return fo["dup"][key]

        names = list(built)
        for a in range(len(names)):
            for b in range(a + 1, len(names)):
                A, Bn = names[a], names[b]
                shared = [k for k in built[A]["idx"] if k in built[Bn]["idx"]]
                if len(shared) != 2:
                    continue
                own, oth = (A, Bn) if built[A]["area"] >= built[Bn]["area"] else (Bn, A)
                fo = built[own]
                k1, k2 = shared
                a1, a2 = fo["vid"][fo["idx"].index(k1)], fo["vid"][fo["idx"].index(k2)]
                b1, b2 = dup(own, oth, k1), dup(own, oth, k2)
                nn = np.cross(self.pos[a2] - self.pos[a1], self.pos[b1] - self.pos[a1])
                if np.dot(nn, fo["n"] + built[oth]["n"]) > 0:
                    self._quad(a1, a2, b2, b1)
                else:
                    self._quad(a2, a1, b1, b2)
        for ix in range(2):
            for iy in range(2):
                for iz in range(2):
                    corner = (ix, iy, iz)
                    inc = [nm for nm in names if corner in built[nm]["idx"]]
                    if len(inc) != 3:
                        continue
                    inc.sort(key=lambda nm: -built[nm]["area"])
                    own = inc[0]
                    fo = built[own]
                    a0 = fo["vid"][fo["idx"].index(corner)]
                    b0, c0 = dup(own, inc[1], corner), dup(own, inc[2], corner)
                    nn = np.cross(self.pos[b0] - self.pos[a0], self.pos[c0] - self.pos[a0])
                    if np.dot(nn, c[corner] - centre) > 0:
                        self._tri(a0, b0, c0)
                    else:
                        self._tri(a0, c0, b0)

    def box(self, centre, size, M=None, chamfer=0.04, skip="", taper=(1, 1), top_shift=(0, 0), extra=None):
        """A box along local z; taper scales the top face (x, y), top_shift slides it."""
        sx, sy, sz = [s / 2.0 for s in size]
        c = np.zeros((2, 2, 2, 3))
        for ix in range(2):
            for iy in range(2):
                for iz in range(2):
                    tx, ty = (taper if iz else (1, 1))
                    ox, oy = (top_shift if iz else (0, 0))
                    c[ix, iy, iz] = (centre[0] + (ix * 2 - 1) * sx * tx + ox, centre[1] + (iy * 2 - 1) * sy * ty + oy,
                                     centre[2] + (iz * 2 - 1) * sz)
        self.hexa(c, chamfer, skip, M, extra)

    def plate(self, pts, thick, M=None, bevel=0.05, depth=0.0, extra=None, flip=False):
        """A convex polygon plate in the local xy plane (front at z = thick, facing +z), bevelled rim,
        thin sides textured from the chart margin. depth sinks the sides below z = 0."""
        pts = [np.asarray(p, dtype=float) for p in pts]
        n = len(pts)
        area = sum(pts[i][0] * pts[(i + 1) % n][1] - pts[(i + 1) % n][0] * pts[i][1] for i in range(n))
        if area < 0:
            pts = pts[::-1]
        M = np.eye(4) if M is None else M
        out_n, inset = [], []
        for i in range(n):
            p0, p1, p2 = pts[i - 1], pts[i], pts[(i + 1) % n]
            e1, e2 = unit(p1 - p0), unit(p2 - p1)
            n1, n2 = np.array([e1[1], -e1[0]]), np.array([e2[1], -e2[0]])
            o = unit(n1 + n2)
            out_n.append(o)
            k = 1.0 / max(np.dot(o, n1), 0.3)
            inset.append(p1 - o * bevel * k)
        bz = bevel * 0.7
        D = self.D()
        zax = M[:3, 2]
        front = [apply(M, (q[0], q[1], thick)) for q in inset]
        ring = [apply(M, (q[0], q[1], thick - bz)) for q in pts]
        base = [apply(M, (q[0], q[1], -depth)) for q in pts]
        margin = int(math.ceil((thick + depth) * D)) + 2
        ci, to_px = self._planar(ring, zax, margin=margin, extra=extra)
        cen = np.mean([to_px(p) for p in ring], axis=0)
        vf = [self._v(front[i], zax, ci, *to_px(front[i])) for i in range(n)]
        vr, vb = [], []
        for i in range(n):
            o3 = M[:3, :3] @ np.array([out_n[i][0], out_n[i][1], 0.0])
            px = np.array(to_px(ring[i]))
            d = unit(px - cen)
            vr.append(self._v(ring[i], o3 + zax * 0.35, ci, *px))
            vb.append(self._v(base[i], o3, ci, *(px + d * (thick + depth - bz) * D * 1.2)))
        for i in range(1, n - 1):
            self._tri(vf[0], vf[i], vf[i + 1])
        for i in range(n):
            j = (i + 1) % n
            if bevel > 0:
                self._quad(vf[i], vr[i], vr[j], vf[j])
            self._quad(vr[i], vb[i], vb[j], vr[j])

    def shell(self, grid, thick=0.06, rim="tblr", extra=None, wrap=False):
        """A curved armour plate from a grid of points [rows][cols] (row 0 = top edge, seen from
        outside with columns running left to right), with a thin rim on the named edges
        (t, b, l, r). wrap closes the columns into a ring (no l/r rims)."""
        G = np.asarray(grid, dtype=float)
        R, C = G.shape[:2]
        du = np.zeros_like(G); dv = np.zeros_like(G)
        if wrap:
            du = np.roll(G, -1, axis=1) - np.roll(G, 1, axis=1)
        else:
            du[:, 1:-1] = G[:, 2:] - G[:, :-2]; du[:, 0] = G[:, 1] - G[:, 0]; du[:, -1] = G[:, -1] - G[:, -2]
        dv[1:-1] = G[2:] - G[:-2]; dv[0] = G[1] - G[0]; dv[-1] = G[-1] - G[-2]
        N = np.cross(dv, du)
        N /= np.linalg.norm(N, axis=2, keepdims=True) + 1e-12
        D = self.D()
        Gc = np.concatenate([G, G[:, :1]], axis=1) if wrap else G
        cw = np.linalg.norm(Gc[:, 1:] - Gc[:, :-1], axis=2).sum(axis=1).mean() * D
        ch = np.linalg.norm(G[1:] - G[:-1], axis=2).sum(axis=0).mean() * D
        margin = int(math.ceil(thick * D)) + 2
        Nc = np.concatenate([N, N[:, :1]], axis=1) if wrap else N
        ci = self._chart(cw, ch, Gc, Nc, None, margin, open_u=wrap, extra=extra)
        CC = C + 1 if wrap else C
        ids = [[self._v(Gc[r, c], Nc[r, c], ci, cw * c / (CC - 1), ch * r / (R - 1)) for c in range(CC)] for r in range(R)]
        for r in range(R - 1):
            for c in range(CC - 1):
                self._quad(ids[r][c], ids[r + 1][c], ids[r + 1][c + 1], ids[r][c + 1])
        if thick <= 0:
            return
        td = thick * D

        def rim_strip(cells, outward_px, flip):
            prev = None
            for (r, c) in cells:
                p, n = Gc[r, c], Nc[r, c]
                px = np.array([cw * c / (CC - 1), ch * r / (R - 1)]) + np.array(outward_px) * td
                # the rim's outward direction in 3D: along the surface, away from the plate
                t = (-dv[r, c % C] if outward_px[1] < 0 else dv[r, c % C] if outward_px[1] > 0 else
                     -du[r, c % C] if outward_px[0] < 0 else du[r, c % C])
                t = unit(t - n * np.dot(t, n))
                vid = self._v(p - n * thick, t, ci, *px)
                if prev is not None:
                    a = prev
                    if flip:
                        self._quad(a[0], a[1], vid, ids[r][c])
                    else:
                        self._quad(ids[r][c], vid, a[1], a[0])
                prev = (ids[r][c], vid)
        if "t" in rim:
            rim_strip([(0, c) for c in range(CC)], (0, -1), False)
        if "b" in rim:
            rim_strip([(R - 1, c) for c in range(CC)], (0, 1), True)
        if not wrap and "l" in rim:
            rim_strip([(r, 0) for r in range(R)], (-1, 0), True)
        if not wrap and "r" in rim:
            rim_strip([(r, C - 1) for r in range(R)], (1, 0), False)

    def tube(self, path, radius, sides=6, extra=None, cap_ends=False, weights=None, twist0=None):
        """A smooth tube along a path (cables, pistons). radius is a number or one per point.
        weights(t) gives the bone weights at parameter t in 0..1 along the path."""
        P = np.asarray(path, dtype=float)
        k = len(P)
        rad = np.full(k, radius, dtype=float) if np.isscalar(radius) else np.asarray(radius, dtype=float)
        tang = np.zeros_like(P)
        tang[1:-1] = P[2:] - P[:-2]; tang[0] = P[1] - P[0]; tang[-1] = P[-1] - P[-2]
        tang /= np.linalg.norm(tang, axis=1, keepdims=True) + 1e-12
        x = np.array([1.0, 0, 0]) if twist0 is None else np.asarray(twist0, dtype=float)
        x = x - tang[0] * np.dot(x, tang[0])
        if np.linalg.norm(x) < 1e-3:
            x = np.array([0, 1.0, 0]) - tang[0] * tang[0][1]
        x = unit(x)
        seg = np.concatenate([[0.0], np.cumsum(np.linalg.norm(P[1:] - P[:-1], axis=1))])
        D = self.D()
        cw, ch = 2 * math.pi * rad.max() * D, seg[-1] * D
        rings_p, rings_n = [], []
        for i in range(k):
            x = unit(x - tang[i] * np.dot(x, tang[i]))
            y = np.cross(tang[i], x)
            ang = np.linspace(0, 2 * math.pi, sides + 1)
            nn = np.cos(ang)[:, None] * x + np.sin(ang)[:, None] * y
            rings_n.append(nn); rings_p.append(P[i] + nn * rad[i])
        gp, gn = np.array(rings_p), np.array(rings_n)
        ci = self._chart(cw, ch, gp, gn, None, 2, open_u=True, extra=extra)
        ids = []
        for i in range(k):
            t = seg[i] / max(seg[-1], 1e-9)
            w = weights(t) if weights else self._weights(t)
            ids.append([self._v(gp[i, s], gn[i, s], ci, cw * s / sides, ch * t, w) for s in range(sides + 1)])
        for i in range(k - 1):
            for s in range(sides):
                self._quad(ids[i][s], ids[i][s + 1], ids[i + 1][s + 1], ids[i + 1][s])
        if cap_ends:
            for i, sgn in ((0, -1), (k - 1, 1)):
                t = seg[i] / max(seg[-1], 1e-9)
                w = weights(t) if weights else self._weights(t)
                cvid = self._v(P[i], tang[i] * sgn, ci, cw * 0.5, ch * t, w)
                for s in range(sides):
                    if sgn > 0:
                        self._tri(ids[i][s], ids[i][s + 1], cvid)
                    else:
                        self._tri(ids[i][s + 1], ids[i][s], cvid)

    def lathe(self, profile, sides=8, M=None, extra=None, sharp=True, arc=(0.0, 360.0)):
        """A surface of revolution about local z from a profile of (radius, z) points listed from
        the top down (so the outside faces out). sharp gives every profile segment its own normals."""
        M = np.eye(4) if M is None else M
        prof = np.asarray(profile, dtype=float)
        full = abs(arc[1] - arc[0]) >= 359.9
        ang = np.radians(np.linspace(arc[0], arc[1], sides + 1))
        cs, sn = np.cos(ang), np.sin(ang)
        seglen = np.linalg.norm(prof[1:] - prof[:-1], axis=1)
        cum = np.concatenate([[0.0], np.cumsum(seglen)])
        D = self.D()
        cw = math.radians(abs(arc[1] - arc[0])) * prof[:, 0].max() * D
        ch = cum[-1] * D
        segn = []
        for i in range(len(prof) - 1):
            d = prof[i + 1] - prof[i]
            segn.append(unit(np.array([-d[1], d[0]])))      # (radial, z) normal: outward for a top-down profile
        vn = []
        for i in range(len(prof)):
            a = segn[max(i - 1, 0)]; b = segn[min(i, len(segn) - 1)]
            vn.append(unit(a + b))

        def ring(i, n2):
            r, z = prof[i]
            p = np.stack([cs * r, sn * r, np.full_like(cs, z)], axis=1)
            n = np.stack([cs * n2[0], sn * n2[0], np.full_like(cs, n2[1])], axis=1)
            return apply(M, p), n @ M[:3, :3].T
        gp = np.array([ring(i, vn[i])[0] for i in range(len(prof))])
        gn = np.array([ring(i, vn[i])[1] for i in range(len(prof))])
        # the grid rows are not evenly spaced along the profile; resample for the painter
        rows = max(2, int(ch / 4) + 2)
        tt = np.linspace(0, cum[-1], rows)
        gp2 = np.array([[np.interp(t, cum, gp[:, s, a]) for a in range(3)] for t in tt for s in range(sides + 1)]).reshape(rows, sides + 1, 3)
        gn2 = np.array([[np.interp(t, cum, gn[:, s, a]) for a in range(3)] for t in tt for s in range(sides + 1)]).reshape(rows, sides + 1, 3)
        ci = self._chart(cw, ch, gp2, gn2, None, 2, open_u=full, extra=extra)

        def make(i, n2):
            p, n = ring(i, n2)
            return [self._v(p[s], n[s], ci, cw * s / sides, cum[i] * D) for s in range(sides + 1)]
        if sharp:
            for i in range(len(prof) - 1):
                a, b = make(i, segn[i]), make(i + 1, segn[i])
                for s in range(sides):
                    self._strip_quad(a, b, s, prof[i][0], prof[i + 1][0])
        else:
            rings = [make(i, vn[i]) for i in range(len(prof))]
            for i in range(len(prof) - 1):
                for s in range(sides):
                    self._strip_quad(rings[i], rings[i + 1], s, prof[i][0], prof[i + 1][0])

    def _strip_quad(self, a, b, s, ra, rb):
        if ra < 1e-6:
            self._tri(a[s], b[s], b[s + 1])
        elif rb < 1e-6:
            self._tri(a[s], b[s], a[s + 1])
        else:
            self._quad(a[s], b[s], b[s + 1], a[s + 1])

    def shard(self, base, axis, length, radius, sides=6, tip=0.35, butt=0.18, seed=0, squash=1.0, extra=None, x_hint=(1, 0, 0)):
        """A double-terminated crystal: flat facets, one chart with a column per facet.
        base is the butt point, base + axis * length the sharp tip."""
        rng = np.random.RandomState(seed)
        M = aim(base, axis, x_hint)
        ang = np.linspace(0, 2 * math.pi, sides, endpoint=False) + rng.uniform(0, 1.0)
        ang += rng.uniform(-0.22, 0.22, sides)
        rr = radius * rng.uniform(0.8, 1.15, sides)
        z0, z1 = length * butt, length * (1 - tip)
        z0s = z0 + rng.uniform(-0.25, 0.25, sides) * length * butt
        z1s = z1 + rng.uniform(-0.3, 0.3, sides) * length * tip * 0.5
        lo = [apply(M, (math.cos(a) * r, math.sin(a) * r * squash, z)) for a, r, z in zip(ang, rr, z0s)]
        hi = [apply(M, (math.cos(a) * r * 0.9, math.sin(a) * r * 0.9 * squash, z)) for a, r, z in zip(ang, rr, z1s)]
        p_tip, p_butt = apply(M, (0, 0, length)), apply(M, (0, 0, 0))
        D = self.D()
        colw = max(2 * math.pi * radius / sides * D, 2.0)
        cw, ch = colw * sides, length * D
        ax = unit(axis)
        gp = np.array([[p_tip - ax * length * (r / 3.0) + (apply(M, (math.cos(a) * radius, math.sin(a) * radius * squash, 0)) - p_butt)
                        for a in list(ang) + [ang[0] + 2 * math.pi]] for r in range(4)])
        gn = gp - (p_tip - ax * length * (np.arange(4)[:, None, None] / 3.0))
        ci = self._chart(cw, ch, gp, gn, None, 1, open_u=True, extra=dict(extra or {}, facets=sides))
        for s in range(sides):
            t = (s + 1) % sides
            x0, x1 = colw * s + 0.5, colw * (s + 1) - 0.5
            y_hi, y_lo = (length - (z1s[s] + z1s[t]) / 2) * D, (length - (z0s[s] + z0s[t]) / 2) * D
            n = unit(np.cross(hi[s] - lo[s], lo[t] - lo[s]))
            if np.dot(n, (lo[s] + lo[t]) / 2 - apply(M, (0, 0, z0))) < 0:
                n = -n
            q = [self._v(lo[s], n, ci, x0, y_lo), self._v(lo[t], n, ci, x1, y_lo),
                 self._v(hi[t], n, ci, x1, y_hi), self._v(hi[s], n, ci, x0, y_hi)]
            self._face(q, n)
            nt = unit(np.cross(hi[t] - hi[s], p_tip - hi[s]))
            nt = nt if np.dot(nt, n) > 0 else -nt
            self._face([self._v(hi[s], nt, ci, x0, y_hi), self._v(hi[t], nt, ci, x1, y_hi),
                        self._v(p_tip, nt, ci, (x0 + x1) / 2, 0.5)], nt)
            nb = unit(np.cross(lo[s] - lo[t], p_butt - lo[t]))
            nb = nb if np.dot(nb, n) > 0 else -nb
            self._face([self._v(lo[t], nb, ci, x1, y_lo), self._v(lo[s], nb, ci, x0, y_lo),
                        self._v(p_butt, nb, ci, (x0 + x1) / 2, ch - 0.5)], nb)

    def _face(self, ids, n):
        """A triangle or quad wound to face along n."""
        a, b, c = self.pos[ids[0]], self.pos[ids[1]], self.pos[ids[2]]
        if np.dot(np.cross(b - a, c - a), n) < 0:
            ids = ids[::-1]
        if len(ids) == 3:
            self._tri(*ids)
        else:
            self._quad(*ids)

    # ---- finishing ---------------------------------------------------------------------------
    def pack(self, sizes):
        """Shelf-pack the charts of each atlas; sizes maps atlas name -> (w, h). Returns the fill."""
        fill = {}
        for name, (W, H) in sizes.items():
            cs = [c for c in self.charts if c.atlas == name]
            cs.sort(key=lambda c: (-c.h, -c.w))
            x = y = 1
            row_h = 0
            used = 0
            for c in cs:
                if x + c.w + 1 > W:
                    x = 1; y += row_h + 1; row_h = 0
                c.x, c.y = x, y
                x += c.w + 1
                row_h = max(row_h, c.h)
                used += c.w * c.h
            if y + row_h + 1 > H:
                raise RuntimeError(f"atlas {name} overflows: needs {y + row_h + 1} rows of {H}; lower the density")
            fill[name] = (used / float(W * H), y + row_h + 1)
        self.sizes = sizes
        return fill

    def arrays(self):
        pos = np.array(self.pos); nrm = np.array(self.nrm)
        uv = np.zeros((len(pos), 2))
        for i, (px, py) in enumerate(self.uv):
            c = self.charts[self.chart_of[i]]
            if c.atlas not in self.sizes:
                continue
            W, H = self.sizes[c.atlas]
            uv[i] = ((c.x + c.margin + px) / W, 1.0 - (c.y + c.margin + py) / H)   # Blender's v runs up
        return pos, nrm, uv, np.array(self.tris, dtype=int)


# ---- noise ---------------------------------------------------------------------------------------
def _hash(ix, iy, iz, seed):
    h = (ix.astype(np.int64) * 374761393 + iy.astype(np.int64) * 668265263 + iz.astype(np.int64) * 2147483647 + seed * 144665) & 0xFFFFFFFF
    h = ((h ^ (h >> 13)) * 1274126177) & 0xFFFFFFFF
    return ((h ^ (h >> 16)) & 0xFFFF) / 65535.0


def noise3(p, scale, seed=0):
    """Smooth value noise in 0..1 at 3D points p[..., 3]."""
    q = np.asarray(p, dtype=float) * scale
    i = np.floor(q).astype(np.int64)
    f = q - i
    f = f * f * (3 - 2 * f)
    out = 0.0
    for dx in (0, 1):
        for dy in (0, 1):
            for dz in (0, 1):
                w = (f[..., 0] if dx else 1 - f[..., 0]) * (f[..., 1] if dy else 1 - f[..., 1]) * (f[..., 2] if dz else 1 - f[..., 2])
                out = out + w * _hash(i[..., 0] + dx, i[..., 1] + dy, i[..., 2] + dz, seed)
    return out


def fbm3(p, scale, seed=0, octaves=3):
    out, amp, tot = 0.0, 1.0, 0.0
    for o in range(octaves):
        out = out + amp * noise3(p, scale * (2 ** o), seed + o * 17)
        tot += amp
        amp *= 0.5
    return out / tot


def hex_rgb(h):
    h = h.lstrip("#")
    return np.array([int(h[i:i + 2], 16) for i in (0, 2, 4)], dtype=float) / 255.0


def chart_fields(c):
    """Per-texel fields of a chart: content coordinates, 3D position, normal, edge distance, edge side."""
    ys, xs = np.mgrid[0:c.h, 0:c.w]
    px = xs - c.margin + 0.5
    py = ys - c.margin + 0.5
    gu = np.clip(px / c.cw, -0.2, 1.2) * (c.grid_p.shape[1] - 1)
    gv = np.clip(py / c.ch, -0.2, 1.2) * (c.grid_p.shape[0] - 1)
    iu = np.clip(np.floor(gu).astype(int), 0, c.grid_p.shape[1] - 2)
    iv = np.clip(np.floor(gv).astype(int), 0, c.grid_p.shape[0] - 2)
    fu, fv = (gu - iu)[..., None], (gv - iv)[..., None]

    def bil(g):
        return (g[iv, iu] * (1 - fu) * (1 - fv) + g[iv, iu + 1] * fu * (1 - fv) +
                g[iv + 1, iu] * (1 - fu) * fv + g[iv + 1, iu + 1] * fu * fv)
    pos, nrm = bil(c.grid_p), bil(c.grid_n)
    nrm = nrm / (np.linalg.norm(nrm, axis=2, keepdims=True) + 1e-12)
    if c.poly is not None:
        poly = np.array(c.poly)
        k = len(poly)
        area = sum(poly[i][0] * poly[(i + 1) % k][1] - poly[(i + 1) % k][0] * poly[i][1] for i in range(k))
        d = np.full(px.shape, 1e9); ex = np.zeros(px.shape); ey = np.zeros(px.shape)
        for i in range(k):
            a, b = poly[i], poly[(i + 1) % k]
            e = b - a
            L = np.linalg.norm(e)
            if L < 1e-6:
                continue
            nin = np.array([-e[1], e[0]]) / L * (1 if area > 0 else -1)   # inward normal
            di = (px - a[0]) * nin[0] + (py - a[1]) * nin[1]
            m = di < d
            d = np.where(m, di, d); ex = np.where(m, -nin[0], ex); ey = np.where(m, -nin[1], ey)
    else:
        cand = [(py, 0.0, -1.0), (c.ch - py, 0.0, 1.0)]
        if not c.open_u:
            cand += [(px, -1.0, 0.0), (c.cw - px, 1.0, 0.0)]
        d = np.full(px.shape, 1e9); ex = np.zeros(px.shape); ey = np.zeros(px.shape)
        for di, ox, oy in cand:
            m = di < d
            d = np.where(m, di, d); ex = np.where(m, ox, ex); ey = np.where(m, oy, ey)
    return dict(px=px, py=py, pos=pos, nrm=nrm, d=d, ex=ex, ey=ey)
