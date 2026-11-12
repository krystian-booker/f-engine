"""Ambient occlusion for the atlas, ray-cast texel by texel against the mesh (short rays, a fixed
set of directions): unlike a renderer's bake it has no noise, so it posterises into clean steps
that sit well in pixel art, and mirrored halves that share a chart get one consistent answer."""
import hashlib
import math
import os
import numpy as np
from mathutils import Vector
from mathutils.bvhtree import BVHTree
from meshkit import chart_fields


def _dirs(n):
    out = []
    for i in range(n):
        u = (i + 0.5) / n
        r = math.sqrt(u)                       # cosine-weighted
        ph = i * 2.399963
        out.append((r * math.cos(ph), r * math.sin(ph), math.sqrt(max(1 - u, 0.0))))
    return np.array(out)


def compute(B, size, atlas="body", maxd=0.5, rays=12, cache_dir=None):
    """Returns occlusion [h, w] in 0..1 (1 = open) for the charts of an atlas."""
    pos = np.array(B.pos)
    tris = np.array(B.tris, dtype=int)
    key = hashlib.sha1(pos.tobytes() + tris.tobytes() + repr((size, maxd, rays, [(c.x, c.y, c.w, c.h) for c in B.charts])).encode()).hexdigest()[:16]
    if cache_dir:
        path = os.path.join(cache_dir, f"ao_{atlas}_{key}.npy")
        if os.path.exists(path):
            return np.load(path)
    bvh = BVHTree.FromPolygons([Vector(p) for p in pos], [tuple(int(i) for i in t) for t in tris])
    W, H = size
    occ = np.ones((H, W))
    D = _dirs(rays)
    for c in B.charts:
        if c.atlas != atlas:
            continue
        F = chart_fields(c)
        P, N, d = F["pos"], F["nrm"], F["d"]
        out = np.ones((c.h, c.w))
        ys, xs = np.nonzero(d > -0.6)
        if len(ys) == 0:
            continue
        n = N[ys, xs]
        p = P[ys, xs] + n * 0.03
        ref = np.where(np.abs(n[:, 2:3]) < 0.9, np.array([[0.0, 0.0, 1.0]]), np.array([[1.0, 0.0, 0.0]]))
        t1 = np.cross(ref, n); t1 /= np.linalg.norm(t1, axis=1, keepdims=True) + 1e-12
        t2 = np.cross(n, t1)
        acc = np.zeros(len(ys))
        for k in range(rays):
            dv = t1 * D[k, 0] + t2 * D[k, 1] + n * D[k, 2]
            for i in range(len(ys)):
                hit = bvh.ray_cast((p[i, 0], p[i, 1], p[i, 2]), (dv[i, 0], dv[i, 1], dv[i, 2]), maxd)
                if hit[0] is not None:
                    acc[i] += 1.0 - hit[3] / maxd
        out[ys, xs] = 1.0 - acc / rays
        occ[c.y:c.y + c.h, c.x:c.x + c.w] = out
    if cache_dir:
        os.makedirs(cache_dir, exist_ok=True)
        np.save(path, occ)
    return occ
