"""The automated checks: mesh integrity, normals and shading, topology, UV
mapping and game readiness. Each returns Findings; validate.py decides what
they mean for the verdict.

Everything is measured on the exported glTF, in world units, after welding
vertices that share a position: glTF splits a vertex wherever its normal or
UV changes, so the file's own indices say nothing about the surface.
"""
import math
from collections import defaultdict

import numpy as np

from kitcommon.kit import ERROR, INFO, WARN, Finding  # noqa: F401  (the kits share one Finding)

GROUPS = [
    ("integrity", "Mesh integrity"),
    ("normals", "Normals and shading"),
    ("topology", "Topology"),
    ("uv", "UV mapping"),
    ("ready", "Game readiness"),
]


# ------------------------------------------------------------------ surface

class Surface:
    """One mesh object (a node's primitives together), welded by position."""

    def __init__(self, name, primitives):
        self.name = name
        self.primitives = primitives
        positions, faces, owner = [], [], []
        offset = 0
        for index, p in enumerate(primitives):
            positions.append(p.world_positions)
            faces.append(p.indices + offset)
            owner.append(np.full(len(p.indices), index))
            offset += len(p.positions)
        self.positions = np.concatenate(positions) if positions else np.zeros((0, 3))
        self.faces = np.concatenate(faces) if faces else np.zeros((0, 3), dtype=np.int64)
        self.owner = np.concatenate(owner) if owner else np.zeros(0, dtype=np.int64)
        finite = np.isfinite(self.positions).all(axis=1) if len(self.positions) else np.zeros(0, dtype=bool)
        self.finite = finite
        safe = np.where(finite[:, None], self.positions, 0.0)
        self.size = float(np.linalg.norm(safe.max(axis=0) - safe.min(axis=0))) if len(safe) else 0.0
        # Weld: vertices closer than a hair (relative to the object) are one point.
        self.epsilon = min(1e-3, max(1e-6, self.size * 1e-5))
        keys = np.round(safe / self.epsilon).astype(np.int64)
        _, self.weld, counts = np.unique(keys, axis=0, return_inverse=True, return_counts=True)
        self.weld = self.weld.reshape(-1)
        self.welded_count = len(counts)
        self.welded_positions = np.zeros((self.welded_count, 3))
        self.welded_positions[self.weld] = safe
        self.wfaces = self.weld[self.faces] if len(self.faces) else np.zeros((0, 3), dtype=np.int64)
        a, b, c = (self.welded_positions[self.wfaces[:, i]] for i in range(3)) if len(self.wfaces) else (np.zeros((0, 3)),) * 3
        self.cross = np.cross(b - a, c - a)
        self.area = np.linalg.norm(self.cross, axis=1) / 2
        with np.errstate(invalid="ignore", divide="ignore"):
            self.face_normals = np.where(self.area[:, None] > 0, self.cross / (2 * self.area[:, None]), 0.0)
        collapsed = (self.wfaces[:, 0] == self.wfaces[:, 1]) | (self.wfaces[:, 1] == self.wfaces[:, 2]) | \
                    (self.wfaces[:, 0] == self.wfaces[:, 2])
        edges = np.stack([np.linalg.norm(b - a, axis=1), np.linalg.norm(c - b, axis=1), np.linalg.norm(a - c, axis=1)], axis=1) \
            if len(self.wfaces) else np.zeros((0, 3))
        self.edge_lengths = edges
        longest = edges.max(axis=1) if len(edges) else np.zeros(0)
        # Degenerate: two corners welded together, or thinner than the weld distance.
        with np.errstate(invalid="ignore", divide="ignore"):
            altitude = np.where(longest > 0, 2 * self.area / longest, 0.0)
        self.altitude = altitude
        self.degenerate = collapsed | (altitude < self.epsilon)
        self.good = ~self.degenerate
        self._edges = None

    def edges(self):
        """Undirected edges of the non-degenerate faces: (pairs, face count per edge,
        the faces of each directed half-edge)."""
        if self._edges is None:
            f = self.wfaces[self.good]
            ids = np.nonzero(self.good)[0]
            half = np.concatenate([f[:, [0, 1]], f[:, [1, 2]], f[:, [2, 0]]])
            half_face = np.concatenate([ids, ids, ids])
            forward = half[:, 0] < half[:, 1]
            pairs = np.where(forward[:, None], half, half[:, ::-1])
            unique, inverse, counts = np.unique(pairs, axis=0, return_inverse=True, return_counts=True)
            self._edges = (unique, counts, inverse.reshape(-1), half_face, forward)
        return self._edges

    def components(self):
        """Connected components over welded vertices: a label per face."""
        parent = np.arange(self.welded_count)

        def find(x):
            while parent[x] != x:
                parent[x] = parent[parent[x]]
                x = parent[x]
            return x

        f = self.wfaces[self.good]
        for a, b in np.concatenate([f[:, [0, 1]], f[:, [1, 2]]]) if len(f) else []:
            ra, rb = find(a), find(b)
            if ra != rb:
                parent[ra] = rb
        roots = np.array([find(v) for v in f[:, 0]]) if len(f) else np.zeros(0, dtype=np.int64)
        labels = np.full(len(self.wfaces), -1)
        labels[self.good] = np.unique(roots, return_inverse=True)[1] if len(roots) else roots
        return labels


def surfaces(model):
    grouped = defaultdict(list)
    for primitive in model.primitives:
        if primitive.mode == 4 and primitive.positions is not None:
            grouped[primitive.node].append(primitive)
    return [Surface(group[0].node_name or group[0].mesh_name or f"node{node}", group) for node, group in grouped.items()]


def _percent(part, whole):
    return 100.0 * part / whole if whole else 0.0


# ---------------------------------------------------------------- integrity

def check_integrity(model, profile, surfs):
    out = []
    for s in surfs:
        total = len(s.faces)
        if not s.finite.all():
            out.append(Finding("integrity.nan", ERROR, "positions that are not numbers", int((~s.finite).sum()), s.name,
                               "Something divided by zero in the generator; find the vertex and its source."))
        degenerate = int(s.degenerate.sum())
        if degenerate:
            out.append(Finding("integrity.degenerate_faces", ERROR, "zero-area faces (collapsed or needle-thin triangles)",
                               degenerate, s.name, "Merge by distance, then dissolve degenerate faces before export."))
        f = np.sort(s.wfaces[s.good], axis=1)
        if len(f):
            _, counts = np.unique(f, axis=0, return_counts=True)
            duplicates = int((counts - 1).sum())
            if duplicates:
                out.append(Finding("integrity.duplicate_faces", ERROR, "faces drawn twice on the same three points (z-fighting)",
                                   duplicates, s.name, "Remove doubled geometry: duplicated objects joined together, or an extrude of zero length."))
        unique, counts, inverse, _, forward = s.edges()
        # More than two faces on an edge is a defect (a fin, an internal face),
        # unless it is two clean surfaces that touch there: then the faces come
        # in pairs running along the edge in opposite directions.
        towards = np.bincount(inverse, weights=forward.astype(np.float64), minlength=len(unique))
        crowded = counts > 2
        touching = crowded & (counts % 2 == 0) & (towards * 2 == counts)
        non_manifold = int((crowded & ~touching).sum())
        if non_manifold:
            out.append(Finding("integrity.non_manifold_edges", ERROR, "edges shared by three or more faces", non_manifold, s.name,
                               "Internal faces or parts fused along an edge: delete the inner faces, or keep the parts as separate shells."))
        if touching.any():
            out.append(Finding("integrity.touching_shells", INFO, "edges where separate surfaces touch", int(touching.sum()), s.name))
        boundary = int((counts == 1).sum())
        if boundary and profile.get("watertight"):
            out.append(Finding("integrity.open_edges", WARN, "open (boundary) edges on a model that should be closed", boundary, s.name,
                               "Cap the holes, or use a profile that does not ask for a closed mesh."))
        elif boundary:
            out.append(Finding("integrity.open_edges", INFO, "open (boundary) edges", boundary, s.name))
        used = np.zeros(len(s.positions), dtype=bool)
        used[s.faces.reshape(-1)] = True
        loose = int((~used).sum())
        if loose:
            out.append(Finding("integrity.loose_vertices", WARN, "vertices no face uses", loose, s.name,
                               "Delete loose vertices and edges before export."))
        labels = s.components()
        if total and labels.max() >= 0:
            sizes = np.bincount(labels[labels >= 0])
            scraps = int((sizes == 1).sum())
            if scraps:
                out.append(Finding("integrity.loose_parts", WARN, "lone triangles connected to nothing", scraps, s.name,
                                   "Leftovers of a boolean or a delete: select by loose parts and remove them."))
        # Vertices the exporter did not need to split: identical in every attribute.
        for p in s.primitives:
            columns = [np.round(p.positions / max(s.epsilon, 1e-9)).astype(np.int64)]
            for attribute, scale in ((p.normals, 1000), (p.uvs, 100000), (p.colors, 1000), (p.weights, 1000)):
                if attribute is not None:
                    columns.append(np.round(np.nan_to_num(attribute) * scale).astype(np.int64))
            if p.joints is not None:
                columns.append(p.joints)
            table = np.concatenate(columns, axis=1)
            duplicates = len(table) - len(np.unique(table, axis=0))
            if duplicates > max(8, len(table) * profile.get("duplicate_vertex_ratio", 0.02)):
                out.append(Finding("integrity.duplicate_vertices", WARN,
                                   f"duplicate vertices ({_percent(duplicates, len(table)):.0f}% of the buffer): the mesh was exported unwelded",
                                   duplicates, p.label, "Merge by distance before export; the exporter splits vertices only where normals or UVs differ."))
    return out


# ------------------------------------------------------------------ normals

def check_normals(model, profile, surfs):
    out = []
    for s in surfs:
        offset = 0
        for p in s.primitives:
            count = len(p.indices)
            faces = slice(offset, offset + count)
            offset += count
            if p.normals is None:
                out.append(Finding("normals.missing", ERROR, "no vertex normals", None, p.label, "Export with normals."))
                continue
            length = np.linalg.norm(p.normals, axis=1)
            bad = ~np.isfinite(length) | (length < 0.5)
            if bad.any():
                out.append(Finding("normals.degenerate", ERROR, "zero-length or invalid vertex normals", int(bad.sum()), p.label,
                                   "Recalculate normals; a zero normal usually sits on a degenerate face."))
            off = np.isfinite(length) & (length >= 0.5) & (np.abs(length - 1) > 0.02)
            if off.any():
                out.append(Finding("normals.not_unit", WARN, "vertex normals that are not unit length", int(off.sum()), p.label,
                                   "Normalise normals on export."))
            good = s.good[faces]
            if not good.any():
                continue
            # Normals into world space (inverse transpose), compared with each face's own.
            linear = p.world[:3, :3]
            try:
                normal_matrix = np.linalg.inv(linear).T
            except np.linalg.LinAlgError:
                normal_matrix = linear
            world_normals = p.normals @ normal_matrix.T
            with np.errstate(invalid="ignore", divide="ignore"):
                world_normals = world_normals / np.linalg.norm(world_normals, axis=1, keepdims=True)
            world_normals = np.nan_to_num(world_normals)
            face_normals = s.face_normals[faces]
            if np.linalg.det(linear) < 0:
                face_normals = -face_normals  # a mirrored node: the engine turns the winding back
            corner = world_normals[p.indices]  # (faces, 3, 3)
            dots = np.einsum("fcx,fx->fc", corner, face_normals)
            flipped = good & (dots.mean(axis=1) < -0.2)
            if flipped.any():
                share = _percent(int(flipped.sum()), int(good.sum()))
                out.append(Finding("normals.flipped", ERROR if share > 1 else WARN,
                                   f"faces whose normals point into the surface ({share:.1f}% of faces)", int(flipped.sum()), p.label,
                                   "Recalculate normals outside; check the winding the generator emits."))
            # A vertex normal far from its face's own: smoothing carried across a hard edge.
            # Judged by surface, not by count: a thousand hair-thin rims matter less than one broad panel.
            limit = math.cos(math.radians(profile.get("max_normal_deviation", 80)))
            smeared = good & ~flipped & (dots.min(axis=1) < limit)
            areas = s.area[faces]
            share = _percent(float(areas[smeared].sum()), float(areas[good].sum()))
            if share > profile.get("smeared_normal_percent", 1.0):
                out.append(Finding("normals.shading", WARN,
                                   f"faces shaded by normals more than {profile.get('max_normal_deviation', 80)} degrees off their own "
                                   f"({share:.1f}% of the surface): smoothing runs across hard edges",
                                   int(smeared.sum()), p.label, "Split normals at sharp edges (auto smooth by angle, or mark sharp)."))
            if p.tangents is not None:
                w = p.tangents[:, 3]
                if (np.abs(np.abs(w) - 1) > 0.01).any():
                    out.append(Finding("normals.tangents", WARN, "tangents whose handedness is not +1 or -1", None, p.label,
                                       "Re-export tangents, or leave them out: the engine derives them from the UVs."))
        # Winding: two faces that share an edge must run along it in opposite directions.
        unique, counts, inverse, half_face, forward = s.edges()
        two = counts[inverse] == 2
        if two.any():
            order = np.argsort(inverse[two], kind="stable")
            direction = forward[two][order].reshape(-1, 2)
            pair = half_face[two][order].reshape(-1, 2)
            # Two open sheets that meet at an angle also share an edge this
            # way; a flipped face lies against its neighbour, facing back.
            facing = np.einsum("ij,ij->i", s.face_normals[pair[:, 0]], s.face_normals[pair[:, 1]])
            same = int(((direction[:, 0] == direction[:, 1]) & (facing < -0.3)).sum())
            if same:
                out.append(Finding("normals.inconsistent_winding", ERROR,
                                   "edges whose two faces are wound the same way (one of them faces inwards)", same, s.name,
                                   "Recalculate normals; if it persists the surface is twisted (a Moebius join) there."))
        # A closed shell with negative volume is inside out.
        labels = s.components()
        if len(unique) and labels.max() >= 0:
            edge_faces = half_face
            edge_label = labels[edge_faces]
            open_labels = set(np.unique(edge_label[counts[inverse] != 2]).tolist())
            a, b, c = (s.welded_positions[s.wfaces[:, i]] for i in range(3))
            signed = np.einsum("ij,ij->i", a, np.cross(b, c)) / 6
            inverted = 0
            for label in range(labels.max() + 1):
                if label in open_labels:
                    continue
                members = labels == label
                if members.sum() >= 4 and signed[members].sum() < -1e-9:
                    inverted += 1
            if inverted:
                out.append(Finding("normals.inside_out", ERROR, "closed shells that are inside out", inverted, s.name,
                                   "Flip the normals of those parts."))
    return out


# ----------------------------------------------------------------- topology

def _t_junctions(s, limit):
    """Welded vertices that sit on the inside of an open edge of a face lying
    in the same plane as one of their own faces: a cut that stopped short.
    (A vertex of some other piece that happens to touch an edge is not one.)"""
    unique, counts, inverse, half_face, _ = s.edges()
    is_boundary = counts == 1
    boundary = unique[is_boundary]
    if not len(boundary):
        return 0, False
    edge_face = np.zeros(len(unique), dtype=np.int64)
    edge_face[inverse] = half_face
    boundary_face = edge_face[is_boundary]
    capped = len(boundary) > limit
    if capped:
        pick = np.linspace(0, len(boundary) - 1, limit).astype(np.int64)
        boundary, boundary_face = boundary[pick], boundary_face[pick]
    points = s.welded_positions
    lengths = np.linalg.norm(points[boundary[:, 1]] - points[boundary[:, 0]], axis=1)
    cell = max(float(np.median(lengths)), s.epsilon * 10)
    grid = defaultdict(list)
    faces = s.wfaces[s.good]
    face_ids = np.nonzero(s.good)[0]
    used = np.unique(faces.reshape(-1))
    cells = np.floor(points[used] / cell).astype(np.int64)
    for vertex, key in zip(used.tolist(), map(tuple, cells.tolist())):
        grid[key].append(vertex)
    vertex_faces = defaultdict(list)
    for face, corners in zip(face_ids.tolist(), faces.tolist()):
        for corner in corners:
            vertex_faces[corner].append(face)
    tolerance = max(s.epsilon * 4, 1e-5)
    found = 0
    for (a, b), length, face in zip(boundary.tolist(), lengths.tolist(), boundary_face.tolist()):
        if length <= tolerance * 4:
            continue
        pa, pb = points[a], points[b]
        lo = np.floor((np.minimum(pa, pb) - tolerance) / cell).astype(np.int64)
        hi = np.floor((np.maximum(pa, pb) + tolerance) / cell).astype(np.int64)
        if np.prod(hi - lo + 1) > 4096:
            continue  # a very long edge through a dense mesh: not worth the walk
        candidates = []
        for x in range(lo[0], hi[0] + 1):
            for y in range(lo[1], hi[1] + 1):
                for z in range(lo[2], hi[2] + 1):
                    candidates.extend(grid.get((x, y, z), ()))
        c = np.array([v for v in candidates if v != a and v != b], dtype=np.int64)
        if not len(c):
            continue
        direction = (pb - pa) / length
        along = (points[c] - pa) @ direction
        away = np.linalg.norm(points[c] - pa - np.outer(along, direction), axis=1)
        on_edge = c[(along > tolerance * 2) & (along < length - tolerance * 2) & (away < tolerance)]
        normal = s.face_normals[face]
        if any(abs(float(s.face_normals[other] @ normal)) > 0.999 for vertex in on_edge.tolist() for other in vertex_faces[vertex]):
            found += 1
    return found, capped


def _planar_groups(s):
    """Faces joined across edges where they lie in one plane: the polygons the
    triangles were cut from. Returns a label per face (-1 for degenerate)."""
    unique, counts, inverse, half_face, _ = s.edges()
    parent = np.arange(len(s.wfaces))

    def find(x):
        while parent[x] != x:
            parent[x] = parent[parent[x]]
            x = parent[x]
        return x

    two = counts[inverse] == 2
    order = np.argsort(inverse[two], kind="stable")
    pairs = half_face[two][order].reshape(-1, 2)
    normals = s.face_normals
    flat = np.einsum("ij,ij->i", normals[pairs[:, 0]], normals[pairs[:, 1]]) > 0.99995
    same_part = s.owner[pairs[:, 0]] == s.owner[pairs[:, 1]]
    for a, b in pairs[flat & same_part].tolist():
        ra, rb = find(a), find(b)
        if ra != rb:
            parent[ra] = rb
    labels = np.array([find(i) for i in range(len(parent))])
    labels[~s.good] = -1
    return labels


def check_topology(model, profile, surfs):
    out = []
    for s in surfs:
        good = s.good
        total = int(good.sum())
        if not total:
            continue
        longest = s.edge_lengths.max(axis=1)
        with np.errstate(invalid="ignore", divide="ignore"):
            ratio = np.where(s.altitude > 0, longest / s.altitude, np.inf)
        slivers = good & (ratio > profile.get("sliver_ratio", 40))
        share = _percent(int(slivers.sum()), total)
        if share > profile.get("sliver_percent", 2.0):
            out.append(Finding("topology.slivers", WARN,
                               f"sliver triangles, more than {profile.get('sliver_ratio', 40)} times longer than they are wide ({share:.1f}% of faces)",
                               int(slivers.sum()), s.name,
                               "Add edge loops along long thin faces, or build the shape from quads of sane proportion; slivers shade and skin badly."))
        needles = good & (ratio > profile.get("needle_ratio", 400))
        if needles.any():
            out.append(Finding("topology.needles", WARN,
                               f"needle triangles, more than {profile.get('needle_ratio', 400)} times longer than wide", int(needles.sum()), s.name,
                               "Usually a boolean cut that grazed an edge: clean the cut, merge the near-coincident vertices."))
        junctions, capped = _t_junctions(s, profile.get("t_junction_edges", 20000))
        if junctions:
            out.append(Finding("topology.t_junctions", WARN,
                               "T-junctions: a vertex sits on the middle of another face's edge" + (" (sampled)" if capped else ""),
                               junctions, s.name,
                               "Cut the long edge at that vertex (knife, or merge the vertex into the edge); T-junctions crack and flicker."))
        # The polygons behind the triangles: how many corners, and how they were fanned.
        labels = _planar_groups(s)
        valid = labels >= 0
        if valid.any():
            sizes = np.bincount(labels[valid])
            big = np.nonzero(sizes >= profile.get("ngon_triangles", 7))[0]
            worst, complex_polygons = 0, 0
            for label in big.tolist():
                faces = s.wfaces[labels == label]
                half = np.concatenate([faces[:, [0, 1]], faces[:, [1, 2]], faces[:, [2, 0]]])
                keys = np.sort(half, axis=1)
                unique_edges, counts = np.unique(keys, axis=0, return_counts=True)
                outline = unique_edges[counts == 1]
                # A region with vertices inside it is a tessellated surface,
                # not one polygon: only the latter was left to the exporter.
                if len(np.setdiff1d(np.unique(faces), np.unique(outline))):
                    continue
                # Corners: outline vertices where the outline turns.
                neighbours = defaultdict(list)
                for a, b in outline.tolist():
                    neighbours[a].append(b)
                    neighbours[b].append(a)
                corners = 0
                for vertex, around in neighbours.items():
                    if len(around) != 2:
                        corners += 1
                        continue
                    u = s.welded_positions[around[0]] - s.welded_positions[vertex]
                    v = s.welded_positions[around[1]] - s.welded_positions[vertex]
                    cosine = float(u @ v / (np.linalg.norm(u) * np.linalg.norm(v) + 1e-30))
                    if cosine > -0.9995:
                        corners += 1
                if corners > profile.get("ngon_corners", 16):
                    complex_polygons += 1
                    worst = max(worst, corners)
            if complex_polygons:
                out.append(Finding("topology.ngons", WARN,
                                   f"flat polygons of more than {profile.get('ngon_corners', 8)} corners (the largest has {worst}), triangulated as they fell",
                                   complex_polygons, s.name,
                                   "Cut large n-gons into quads on purpose (grid fill, or edge loops) so the triangulation is yours, not the exporter's."))
        # Faces lying in the same plane on top of each other without being joined.
        overlap = _coplanar_overlap(s, labels, profile.get("overlap_faces", 40000))
        if overlap:
            out.append(Finding("topology.coplanar_overlap", WARN,
                               "faces lying on top of other faces in the same plane (z-fighting; typical of a boolean that did not cut)",
                               overlap, s.name,
                               "Apply the boolean and delete what it left inside, or move one surface off the other."))
    return out


def _coplanar_overlap(s, labels, limit):
    good = np.nonzero(s.good)[0]
    if len(good) > limit or not len(good):
        return 0
    # Faces in one plane facing the same way fight for the same pixels. (Two
    # shells back to back, a crate on a floor, never show both.)
    canonical = s.face_normals[good]
    centre = s.welded_positions[s.wfaces[good]].mean(axis=1)
    distance = np.einsum("ij,ij->i", canonical, centre)
    step = max(s.epsilon * 20, 1e-4)
    keys = np.concatenate([np.round(canonical * 200).astype(np.int64), np.round(distance / step).astype(np.int64)[:, None]], axis=1)
    _, plane = np.unique(keys, axis=0, return_inverse=True)
    plane = plane.reshape(-1)
    order = np.argsort(plane, kind="stable")
    starts = np.nonzero(np.diff(plane[order], prepend=-1))[0]
    ends = np.append(starts[1:], len(order))
    overlapping = 0
    for start, end in zip(starts.tolist(), ends.tolist()):
        if end - start < 2 or end - start > 600:
            continue
        members = good[order[start:end]]
        groups = labels[members]
        if len(np.unique(groups)) < 2:
            continue
        n = canonical[order[start]]
        axis = np.cross(n, [1.0, 0, 0] if abs(n[0]) < 0.9 else [0, 1.0, 0])
        axis /= np.linalg.norm(axis)
        other = np.cross(n, axis)
        triangles = s.welded_positions[s.wfaces[members]]
        flat = np.stack([triangles @ axis, triangles @ other], axis=2)  # (faces, 3, 2)
        centres = flat.mean(axis=1)
        a, b, c = flat[:, 0], flat[:, 1], flat[:, 2]
        # centre i inside triangle j (strictly), for faces of different polygons
        def side(p, q, r):
            return (q[None, :, 0] - p[None, :, 0]) * (r[:, None, 1] - p[None, :, 1]) - \
                   (q[None, :, 1] - p[None, :, 1]) * (r[:, None, 0] - p[None, :, 0])
        d1, d2, d3 = side(a, b, centres), side(b, c, centres), side(c, a, centres)
        tolerance = 1e-9
        inside = ((d1 > tolerance) & (d2 > tolerance) & (d3 > tolerance)) | ((d1 < -tolerance) & (d2 < -tolerance) & (d3 < -tolerance))
        inside &= groups[:, None] != groups[None, :]
        overlapping += int(inside.any(axis=1).sum())
    return overlapping


# ----------------------------------------------------------------------- UV

def _texture_of(model, material_index, slot="baseColorTexture"):
    if material_index is None or material_index < 0 or material_index >= len(model.materials):
        return None
    material = model.materials[material_index]
    reference = material.get("pbrMetallicRoughness", {}).get(slot) if slot == "baseColorTexture" else material.get(slot)
    if not reference:
        return None
    texture = model.textures[reference["index"]] if reference["index"] < len(model.textures) else None
    if texture is None or "source" not in texture or texture["source"] >= len(model.images):
        return None
    return model.images[texture["source"]]


def _uses_textures(model, material_index):
    if material_index is None or material_index < 0 or material_index >= len(model.materials):
        return False
    material = model.materials[material_index]
    return bool(material.get("pbrMetallicRoughness", {}).get("baseColorTexture") or material.get("normalTexture")
                or material.get("emissiveTexture"))


def uv_islands(p):
    """A label per face: faces joined where they share an edge in UV space."""
    keys = np.round(p.uvs * 100000).astype(np.int64)
    _, uv_weld = np.unique(np.concatenate([keys, np.round(p.positions * 100000).astype(np.int64)], axis=1), axis=0, return_inverse=True)
    uv_weld = uv_weld.reshape(-1)
    faces = uv_weld[p.indices]
    parent = np.arange(uv_weld.max() + 1 if len(uv_weld) else 0)

    def find(x):
        while parent[x] != x:
            parent[x] = parent[parent[x]]
            x = parent[x]
        return x

    for a, b in np.concatenate([faces[:, [0, 1]], faces[:, [1, 2]]]).tolist() if len(faces) else []:
        ra, rb = find(a), find(b)
        if ra != rb:
            parent[ra] = rb
    roots = np.array([find(v) for v in faces[:, 0]]) if len(faces) else np.zeros(0, dtype=np.int64)
    return np.unique(roots, return_inverse=True)[1].reshape(-1) if len(roots) else roots


def texel_density(p, image):
    """Pixels per world unit of each face (nan where it has no area), and its weight."""
    world = p.world_positions
    a, b, c = (world[p.indices[:, i]] for i in range(3))
    area = np.linalg.norm(np.cross(b - a, c - a), axis=1) / 2
    ua, ub, uc = (p.uvs[p.indices[:, i]] for i in range(3))
    uv_area = np.abs((ub[:, 0] - ua[:, 0]) * (uc[:, 1] - ua[:, 1]) - (ub[:, 1] - ua[:, 1]) * (uc[:, 0] - ua[:, 0])) / 2
    with np.errstate(invalid="ignore", divide="ignore"):
        density = np.sqrt(uv_area * image["width"] * image["height"] / area)
    density[~np.isfinite(density)] = np.nan
    return density, area, uv_area


def check_uv(model, profile, surfs):
    out = []
    layout = profile.get("uv", "any")
    for s in surfs:
        for p in s.primitives:
            textured = _uses_textures(model, p.material)
            if p.uvs is None:
                if textured:
                    out.append(Finding("uv.missing", ERROR, "no UVs on a part whose material has textures", None, p.label,
                                       "Unwrap it, or give it a material without textures."))
                elif profile.get("require_uvs"):
                    out.append(Finding("uv.missing", WARN, "no UVs", None, p.label, "Unwrap it."))
                continue
            if not np.isfinite(p.uvs).all():
                out.append(Finding("uv.invalid", ERROR, "UVs that are not numbers", None, p.label, "Fix the unwrap that produced them."))
                continue
            image = _texture_of(model, p.material)
            if not len(p.indices):
                continue
            density, area, uv_area = texel_density(p, image or {"width": 1, "height": 1})
            solid = area > (s.epsilon ** 2)
            collapsed = solid & (uv_area < 1e-12)
            share = _percent(float(area[collapsed].sum()), float(area[solid].sum()))
            if textured and share > profile.get("uv_collapsed_percent", 0.5):
                out.append(Finding("uv.collapsed", ERROR if share > 5 else WARN,
                                   f"faces with no area in UV space ({share:.1f}% of the surface): the texture smears to a line there",
                                   int(collapsed.sum()), p.label, "Unwrap those faces; a projection straight along a face collapses it."))
            if image and image["width"]:
                valid = solid & ~collapsed & np.isfinite(density)
                if valid.any():
                    order = np.argsort(density[valid])
                    weights = np.cumsum(area[valid][order])
                    weights /= weights[-1]
                    sorted_density = density[valid][order]
                    low, median, high = (float(sorted_density[np.searchsorted(weights, q)]) for q in (0.1, 0.5, 0.9))
                    p.density = (low, median, high)
                    spread = high / low if low > 0 else float("inf")
                    if spread > profile.get("texel_density_spread", 2.5):
                        out.append(Finding("uv.texel_density_spread", WARN,
                                           f"texel density varies {spread:.1f}x across the part ({low:.0f} to {high:.0f} px per unit; median {median:.0f})",
                                           None, p.label, "Scale UV islands to one density; give small or hidden faces less only on purpose."))
                    target = profile.get("texel_density")
                    if target:
                        if isinstance(target, (int, float)):
                            tolerance = profile.get("texel_density_tolerance", 0.5)
                            target = [target * (1 - tolerance), target * (1 + tolerance)]
                        if median < target[0] or median > target[1]:
                            out.append(Finding("uv.texel_density", WARN,
                                               f"median texel density is {median:.0f} px per unit; the profile asks for {target[0]:.0f} to {target[1]:.0f}",
                                               None, p.label, "Rescale the UVs or change the texture's size."))
                # Islands too small to hold a texel.
                islands = uv_islands(p)
                if len(islands):
                    texels = np.bincount(islands, weights=uv_area * image["width"] * image["height"])
                    surface = np.bincount(islands, weights=area)
                    tiny = (texels < profile.get("tiny_island_texels", 4)) & (surface > s.size ** 2 * 1e-6)
                    if tiny.sum() > max(4, len(texels) * 0.2):
                        out.append(Finding("uv.tiny_islands", WARN,
                                           f"UV islands smaller than {profile.get('tiny_island_texels', 4)} texels ({int(tiny.sum())} of {len(texels)} islands)",
                                           int(tiny.sum()), p.label, "Join seams or scale those islands up; they sample one texel and waste a draw of the unwrap."))
            # Stretch: how unevenly the UVs scale the surface in its two directions.
            world = p.world_positions
            e1, e2 = world[p.indices[:, 1]] - world[p.indices[:, 0]], world[p.indices[:, 2]] - world[p.indices[:, 0]]
            u1, u2 = p.uvs[p.indices[:, 1]] - p.uvs[p.indices[:, 0]], p.uvs[p.indices[:, 2]] - p.uvs[p.indices[:, 0]]
            if image and image["width"]:
                u1, u2 = u1 * [image["width"], image["height"]], u2 * [image["width"], image["height"]]
            # 2D frame on each face, then the singular values of UV = J * frame.
            x_axis = e1 / (np.linalg.norm(e1, axis=1, keepdims=True) + 1e-30)
            normal = np.cross(e1, e2)
            y_axis = np.cross(normal / (np.linalg.norm(normal, axis=1, keepdims=True) + 1e-30), x_axis)
            p1 = np.stack([np.einsum("ij,ij->i", e1, x_axis), np.einsum("ij,ij->i", e1, y_axis)], axis=1)
            p2 = np.stack([np.einsum("ij,ij->i", e2, x_axis), np.einsum("ij,ij->i", e2, y_axis)], axis=1)
            determinant = p1[:, 0] * p2[:, 1] - p1[:, 1] * p2[:, 0]
            ok = solid & ~collapsed & (np.abs(determinant) > 1e-18)
            if ok.any():
                inverse = np.stack([p2[:, 1], -p1[:, 1], -p2[:, 0], p1[:, 0]], axis=1)[ok] / determinant[ok][:, None]
                j = np.stack([u1[ok][:, 0] * inverse[:, 0] + u2[ok][:, 0] * inverse[:, 1],
                              u1[ok][:, 0] * inverse[:, 2] + u2[ok][:, 0] * inverse[:, 3],
                              u1[ok][:, 1] * inverse[:, 0] + u2[ok][:, 1] * inverse[:, 1],
                              u1[ok][:, 1] * inverse[:, 2] + u2[ok][:, 1] * inverse[:, 3]], axis=1).reshape(-1, 2, 2)
                singular = np.linalg.svd(j, compute_uv=False)
                with np.errstate(invalid="ignore", divide="ignore"):
                    stretch = singular[:, 0] / singular[:, 1]
                stretched = np.isfinite(stretch) & (stretch > profile.get("uv_stretch", 3.0))
                share = _percent(float(area[ok][stretched].sum()), float(area[ok].sum()))
                if textured and share > profile.get("uv_stretch_percent", 10.0):
                    out.append(Finding("uv.stretch", WARN,
                                       f"UVs stretched more than {profile.get('uv_stretch', 3.0)}:1 on {share:.0f}% of the surface",
                                       int(stretched.sum()), p.label, "Relax or re-project those faces (a box projection per facing usually fixes it)."))
            # Overlap, where each face is meant to own its texels.
            beyond = (p.uvs.min() < -0.01) or (p.uvs.max() > 1.01)
            if layout == "unique" and beyond:
                out.append(Finding("uv.out_of_range", WARN, "UVs leave the 0..1 square on a model that should have a unique unwrap",
                                   None, p.label, "Pack the islands inside 0..1."))
            if layout == "unique" and not beyond:
                share = _overlap_share(p)
                if share > profile.get("uv_overlap_percent", 2.0):
                    out.append(Finding("uv.overlap", WARN, f"UV islands overlap on {share:.0f}% of the used texture area", None, p.label,
                                       "Repack with a margin; stack islands only where the same pixels are wanted (and waive the check with that reason)."))
            if p.extra_uv_sets and not profile.get("allow_extra_uv_sets"):
                out.append(Finding("uv.extra_sets", INFO, "extra UV sets the engine does not read", p.extra_uv_sets, p.label))
    return out


def _overlap_share(p, size=512):
    """Share of the covered UV area that more than one island covers."""
    from PIL import Image, ImageDraw
    islands = uv_islands(p)
    if not len(islands):
        return 0.0
    count = np.zeros((size, size), dtype=np.uint16)
    uv = np.clip(p.uvs, 0, 1) * (size - 1)
    for island in range(islands.max() + 1):
        faces = p.indices[islands == island]
        points = uv[faces]
        lo = np.floor(points.reshape(-1, 2).min(axis=0)).astype(int)
        hi = np.ceil(points.reshape(-1, 2).max(axis=0)).astype(int) + 1
        w, h = int(hi[0] - lo[0]), int(hi[1] - lo[1])
        if w <= 0 or h <= 0:
            continue
        mask = Image.new("L", (w, h), 0)
        draw = ImageDraw.Draw(mask)
        for triangle in (points - lo).tolist():
            draw.polygon([tuple(corner) for corner in triangle], fill=1)
        count[lo[1]:hi[1], lo[0]:hi[0]] += np.asarray(mask, dtype=np.uint16)
    covered = int((count > 0).sum())
    return _percent(int((count > 1).sum()), covered)


# ---------------------------------------------------------------- readiness

SUPPORTED_EXTENSIONS = {"KHR_materials_emissive_strength", "KHR_lights_punctual"}


def check_readiness(model, profile, surfs):
    out = []
    for severity, message in model.problems:
        out.append(Finding("ready.structure", severity, message))
    triangles = sum(len(p.indices) for p in model.primitives)
    vertices = sum(len(p.positions) for p in model.primitives)
    model.stats = {"triangles": triangles, "vertices": vertices, "primitives": len(model.primitives),
                   "materials": len(model.materials), "images": len(model.images), "skins": len(model.skins),
                   "animations": len(model.animations), "file_bytes": model.file_bytes}

    def budget(key, value, what, fix):
        limit = profile.get(key)
        if limit is not None and value > limit:
            out.append(Finding("ready." + key, ERROR, f"{value:,} {what}; the '{profile['name']}' budget is {limit:,}", None, None, fix))

    budget("max_triangles", triangles, "triangles", "Decimate where it does not show: backs, undersides, flat areas, distant parts.")
    skinned = sum(len(p.positions) for p in model.primitives if p.skinned)
    budget("max_vertices", vertices, "vertices", "Fewer hard edges and UV seams mean fewer split vertices for the same triangles.")
    budget("max_skinned_vertices", skinned, "skinned vertices (the engine skins on the CPU, every frame)",
           "Lower the mesh's density, or make rigid parts separate unskinned nodes.")
    budget("max_materials", len(model.materials), "materials", "Atlas textures so parts can share a material.")
    budget("max_draws", len(model.primitives), "mesh primitives (draw calls)", "Join objects that share a material.")
    budget("max_file_megabytes", model.file_bytes / 1e6, "megabytes", "Smaller textures, or fewer of them.")

    for p in model.primitives:
        if p.mode != 4:
            out.append(Finding("ready.primitive_mode", ERROR, "a primitive that is not a triangle list (points, lines, strips)", None, p.label,
                               "Delete loose edges and points; export triangles."))
        if not p.indexed:
            out.append(Finding("ready.unindexed", WARN, "an unindexed primitive (every triangle has its own three vertices)", None, p.label,
                               "Export with indices."))
        if p.targets:
            out.append(Finding("ready.morph_targets", WARN, "shape keys (morph targets), which the engine ignores", p.targets, p.label,
                               "Bake the shape into the mesh or animate it with bones."))
        if p.material < 0:
            out.append(Finding("ready.no_material", WARN, "a part with no material", None, p.label, "Assign one."))
        if p.skinned:
            if p.joints is None or p.weights is None:
                out.append(Finding("ready.skin_attributes", ERROR, "a skinned mesh without joints and weights", None, p.label,
                                   "Bind the mesh to the armature with weights."))
            else:
                total = p.weights.sum(axis=1)
                unbound = total < 1e-6
                if unbound.any():
                    out.append(Finding("ready.unweighted", ERROR, "vertices bound to no bone", int(unbound.sum()), p.label,
                                       "Weight them; an unweighted vertex follows the first joint."))
                loose = ~unbound & (np.abs(total - 1) > 0.02)
                if loose.any():
                    out.append(Finding("ready.weights", WARN, "vertices whose weights do not sum to 1", int(loose.sum()), p.label,
                                       "Normalise weights (the engine does at load, but the export should be clean)."))
            if p.extra_joint_sets:
                out.append(Finding("ready.influences", ERROR, "more than four bone influences per vertex; the engine reads four", None, p.label,
                                   "Limit total weights to 4 and normalise."))

    # Scale and origin, from the scene's bounds in world units.
    if model.primitives:
        points = np.concatenate([p.world_positions for p in model.primitives])
        low, high = points.min(axis=0), points.max(axis=0)
        size = high - low
        model.stats["bounds"] = [[round(float(v), 3) for v in low], [round(float(v), 3) for v in high]]
        height = profile.get("height")
        if height and not (height[0] <= size[1] <= height[1]):
            out.append(Finding("ready.scale", ERROR, f"{size[1]:.2f} units tall; the '{profile['name']}' profile expects {height[0]} to {height[1]}",
                               None, None, "One unit is one grid cell (a hero is about 1.5). Scale the model and apply the scale."))
        footprint = profile.get("footprint")
        if footprint and max(size[0], size[2]) > footprint:
            out.append(Finding("ready.scale", ERROR, f"{max(size[0], size[2]):.2f} units across; the profile allows {footprint}", None, None,
                               "Scale the model and apply the scale."))
        origin = profile.get("origin", "any")
        tolerance = profile.get("origin_tolerance", 0.05) * max(1.0, float(size.max()))
        if origin == "base_centre":
            if abs(low[1]) > tolerance:
                out.append(Finding("ready.origin", WARN, f"its lowest point is at y = {low[1]:.2f}, not on the ground (y = 0)", None, None,
                                   "Put the origin on the ground under the model."))
            centre = (low + high) / 2
            if max(abs(centre[0]), abs(centre[2])) > max(tolerance, 0.25 * max(size[0], size[2])):
                out.append(Finding("ready.origin", WARN, f"it is centred at x = {centre[0]:.2f}, z = {centre[2]:.2f}, not over its origin", None, None,
                                   "Put the origin at the centre of the base."))
        elif origin == "centre":
            centre = (low + high) / 2
            if np.abs(centre).max() > max(tolerance, 0.1 * float(size.max())):
                out.append(Finding("ready.origin", WARN, "its origin is not at its centre", None, None, "Move the origin to the bounds' centre."))
        elif origin == "corner":
            if low[0] < -tolerance * 4 and not profile.get("allow_backdrop"):
                out.append(Finding("ready.origin", INFO, f"geometry starts at x = {low[0]:.1f}, before the map's origin corner", None, None))

    # Transforms left on mesh nodes.
    scaled, mirrored = [], []
    for index in sorted({p.node for p in model.primitives if not p.skinned}):
        linear = model.world[index][:3, :3]
        lengths = np.linalg.norm(linear, axis=0)
        if np.abs(lengths - 1).max() > 0.001:
            scaled.append(model.nodes[index].get("name") or f"node{index}")
        if np.linalg.det(linear) < 0:
            mirrored.append(model.nodes[index].get("name") or f"node{index}")
    if scaled and profile.get("require_applied_scale", True):
        names = ", ".join(scaled[:4]) + (" ..." if len(scaled) > 4 else "")
        out.append(Finding("ready.transforms", WARN, f"mesh nodes with a scale that is not 1 ({names})", len(scaled), None,
                           "Apply scale before export, so sizes in the file are sizes in the game."))
    if mirrored:
        out.append(Finding("ready.mirrored", INFO, "mirrored mesh nodes (negative scale); the engine turns their winding back", len(mirrored)))

    # Materials and textures.
    used = {p.material for p in model.primitives}
    names = defaultdict(int)
    for index, material in enumerate(model.materials):
        name = material.get("name") or ""
        names[name] += 1
        if not name:
            out.append(Finding("ready.material_names", WARN, f"material {index} has no name", None, None,
                               "Name materials for what they are (stone, bronze, aetherite)."))
        if index not in used:
            out.append(Finding("ready.unused_material", WARN, f"material '{name or index}' is used by nothing", None, None,
                               "Remove unused material slots before export."))
        if material.get("alphaMode") == "BLEND" and not profile.get("allow_blend", True):
            out.append(Finding("ready.blend", WARN, f"material '{name}' is alpha blended", None, None,
                               "Blended parts are sorted and never merged; use opaque or mask unless it must be see-through."))
        emissive = material.get("emissiveFactor", [0, 0, 0])
        if material.get("emissiveTexture") and not any(emissive):
            out.append(Finding("ready.emissive", WARN, f"material '{name}' has an emissive texture and an emissive factor of 0 (it will not glow)",
                               None, None, "Set the emission strength above 0."))
    for name, count in names.items():
        if name and count > 1:
            out.append(Finding("ready.material_names", WARN, f"{count} materials are named '{name}'", None, None, "Give materials unique names."))
    largest = profile.get("max_texture_size")
    for image in model.images:
        if image["missing"]:
            out.append(Finding("ready.missing_texture", ERROR, f"texture file '{image['uri']}' does not exist", None, None,
                               "Embed textures in the .glb, or ship the file next to it."))
            continue
        if not image["width"]:
            out.append(Finding("ready.texture_format", ERROR, f"texture '{image['name']}' is not a PNG or JPEG the engine can decode", None, None,
                               "Use PNG (or JPEG for photographs)."))
            continue
        if largest and max(image["width"], image["height"]) > largest:
            out.append(Finding("ready.texture_size", ERROR,
                               f"texture '{image['name']}' is {image['width']}x{image['height']}; the profile allows {largest}", None, None,
                               "Reduce it; texel density, not texture size, is what the player sees."))
        for side in (image["width"], image["height"]):
            if side & (side - 1):
                out.append(Finding("ready.texture_pot", WARN,
                                   f"texture '{image['name']}' is {image['width']}x{image['height']}: not a power of two (mip chains and wrapping suffer)",
                                   None, None, "Use sizes like 256, 512, 1024."))
                break
    for extension in model.json.get("extensionsRequired", []):
        if extension not in SUPPORTED_EXTENSIONS:
            out.append(Finding("ready.extensions", ERROR, f"requires the glTF extension {extension}, which the engine does not read", None, None,
                               "Turn that export option off."))
    for extension in model.json.get("extensionsUsed", []):
        if extension not in SUPPORTED_EXTENSIONS and extension not in model.json.get("extensionsRequired", []):
            out.append(Finding("ready.extensions", WARN, f"uses the glTF extension {extension}, which the engine ignores", None, None,
                               "Check the model does not depend on it."))

    # Skins and animations.
    limit = profile.get("max_joints")
    for skin in model.skins:
        joints = len(skin.get("joints", []))
        if limit and joints > limit:
            out.append(Finding("ready.max_joints", ERROR, f"a skin of {joints} joints; the profile allows {limit}", None, None, "Remove helper and leaf bones from the deform set."))
    seen = defaultdict(int)
    for animation in model.animations:
        seen[animation["name"]] += 1
        if not animation["name"]:
            out.append(Finding("ready.animation_names", ERROR, "an animation with no name (scripts play animations by name)", None, None, "Name every action."))
        if animation["seconds"] <= 0:
            out.append(Finding("ready.animation_length", WARN, f"animation '{animation['name']}' has no length", None, None, "Key at least two frames."))
    for name, count in seen.items():
        if name and count > 1:
            out.append(Finding("ready.animation_names", ERROR, f"{count} animations are named '{name}'", None, None, "Animation names must be unique."))
    for name in profile.get("animations", []):
        if name not in seen:
            out.append(Finding("ready.animations_missing", ERROR, f"the profile expects an animation named '{name}'", None, None, "Add it, or fix the name."))
    return out


CHECKS = {"integrity": check_integrity, "normals": check_normals, "topology": check_topology, "uv": check_uv,
          "ready": check_readiness}


def run(model, profile):
    surfs = surfaces(model)
    findings = []
    for group, _ in GROUPS:
        findings.extend(CHECKS[group](model, profile, surfs))
    return findings
