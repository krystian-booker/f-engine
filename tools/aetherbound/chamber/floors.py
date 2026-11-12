"""The floor paintings: one 512 x 512 top-down pixel painting per map (16 px
per unit) with a normal map and an emissive map. The chamber's is a radial
flagstone floor with glowing Aetherite conduits and the dais; the ruins' is
the same floor scorched, cracked and dead, with the crater."""
import math
import numpy as np
from layout import (CX, CZ, DX, DZ, RING, CONDUITS, NODES, PASSAGE, CRATER_R, crater_depth)
from paint import (STONE, SLATE, WARM, DIRT, BRONZE, GOLD, AETHER, ASH, vnoise, fbm, bevel_light, tones, save, crack)

N = 512
PPU = 16.0
HALL_RINGS = [0.0, 1.0, 2.2, 3.6, 5.0, 6.4, 7.8, 8.3, 9.7, 11.1, 12.2, 13.4]
HALL_PALETTE = [2, 0, 0, 1, 0, 0, 3, 0, 1, 0, 0]          # per ring: 0 stone 1 slate 2 warm 3 inlay
DAIS_RINGS = [0.0, 0.95, 1.9, 2.7, 3.1, 3.5, 4.75]
DAIS_SLABS = [1, 8, 12, 18, 22, 16]
DAIS_PALETTE = [2, 2, 1, 2, 2, 0]


def seg_distance(X, Z, a, b):
    ax, az = a
    bx, bz = b
    dx, dz = bx - ax, bz - az
    t = np.clip(((X - ax) * dx + (Z - az) * dz) / (dx * dx + dz * dz), 0, 1)
    return np.hypot(X - (ax + t * dx), Z - (az + t * dz)), t


def layout(rng):
    """Slab ids, the distance (px) from each pixel to its slab's edge, a palette and a zone
    (0 hall, 1 dais, 2 passage, 3 outside)."""
    yy, xx = np.mgrid[0:N, 0:N]
    X, Z = (xx + 0.5) / PPU, (yy + 0.5) / PPU
    ident = np.zeros((N, N), int)
    dist = np.full((N, N), 9.0)
    palette = np.zeros((N, N), int)
    zone = np.full((N, N), 3)
    count = 1

    def rings(cx, cz, bounds, slabs, palettes, zone_id, mask):
        nonlocal count
        r = np.hypot(X - cx, Z - cz)
        theta = np.arctan2(-(Z - cz), X - cx)
        for k in range(len(bounds) - 1):
            r0, r1 = bounds[k], bounds[k + 1]
            inside = mask & (r >= r0) & (r < r1)
            n = slabs[k]
            phase = rng.uniform(0, 2 * math.pi)
            f = ((theta + phase) % (2 * math.pi)) / (2 * math.pi) * n
            index = np.floor(f).astype(int)
            frac = f - index
            arc = np.minimum(frac, 1 - frac) * (2 * math.pi / n) * np.maximum(r, 0.05) if n > 1 else np.full_like(r, 9.0)
            radial = np.minimum(r - r0, r1 - r) if r0 > 0 else (r1 - r)
            ident[inside] = (count + index)[inside]
            dist[inside] = (np.minimum(arc, radial) * PPU)[inside]
            palette[inside] = palettes[k]
            zone[inside] = zone_id
            count += n

    hall_r = np.hypot(X - CX, Z - CZ)
    hall_slabs = [1] + [max(4, int(round(2 * math.pi * 0.5 * (a + b) / (0.55 if HALL_PALETTE[i] == 3 else 1.75))))
                        for i, (a, b) in enumerate(zip(HALL_RINGS[1:-1], HALL_RINGS[2:]), start=1)]
    rings(CX, CZ, HALL_RINGS, hall_slabs, HALL_PALETTE, 0, np.ones((N, N), bool))
    dais_r = np.hypot(X - DX, Z - DZ)
    rings(DX, DZ, DAIS_RINGS, DAIS_SLABS, DAIS_PALETTE, 1, dais_r < DAIS_RINGS[-1])
    # The seam where the dais apron meets the hall rings.
    seam = np.abs(dais_r - DAIS_RINGS[-1]) * PPU
    dist = np.where((zone == 0) & (seam < dist), seam, dist)
    # The east passage: rectangular slabs.
    x0, z0, x1, z1 = PASSAGE
    passage = (X >= x0 - 0.9) & (Z >= z0 - 0.35) & (Z < z1 + 0.35) & (hall_r > 12.2)
    row = np.floor(Z - z0 + 0.35 * 0).astype(int)
    col = np.floor((X + (row % 2) * 1.0) / 2.0).astype(int)
    fx = (X + (row % 2) * 1.0) / 2.0 - col
    fz = Z - np.floor(Z)
    d = np.minimum(np.minimum(fx, 1 - fx) * 2.0, np.minimum(fz, 1 - fz)) * PPU
    ident[passage] = (count + row * 40 + col)[passage]
    dist[passage] = d[passage]
    palette[passage] = 0
    zone[passage] = 2
    outside = zone == 3
    return X, Z, ident, dist, palette, zone, outside


def conduit_field(X, Z):
    """Distance (px) to the nearest conduit centre line, and a coordinate along it."""
    best = np.full(X.shape, 99.0)
    along = np.zeros(X.shape)
    for line in CONDUITS:
        run = 0.0
        for a, b in zip(line[:-1], line[1:]):
            d, t = seg_distance(X, Z, a, b)
            length = math.hypot(b[0] - a[0], b[1] - a[1])
            closer = d < best
            along = np.where(closer, run + t * length, along)
            best = np.where(closer, d, best)
            run += length
    ring = np.abs(np.hypot(X - DX, Z - DZ) - RING)
    theta = np.arctan2(-(Z - DZ), X - DX)
    along = np.where(ring < best, theta * RING, along)
    best = np.minimum(best, ring)
    for (nx, nz), r in NODES:
        node = np.abs(np.hypot(X - nx, Z - nz) - r)
        best = np.minimum(best, node)
    return best * PPU, along


def sigil_field(X, Z):
    """Thin glowing lines on the dais: spokes, an inner ring and a star on the seal (px distance)."""
    r = np.hypot(X - DX, Z - DZ)
    theta = np.arctan2(-(Z - DZ), X - DX)
    best = np.full(X.shape, 99.0)
    for k in range(8):
        a = math.radians(45 * k)
        inner = (DX + 0.95 * math.cos(a), DZ - 0.95 * math.sin(a))
        outer = (DX + RING * math.cos(a), DZ - RING * math.sin(a))
        d, _ = seg_distance(X, Z, inner, outer)
        best = np.minimum(best, d)
    best = np.minimum(best, np.abs(r - 2.3))
    best = np.minimum(best, np.abs(r - 0.95))
    # An eight-pointed star inside the seal.
    for k in range(8):
        a, b = math.radians(45 * k), math.radians(45 * k + 135)
        p = (DX + 0.9 * math.cos(a), DZ - 0.9 * math.sin(a))
        q = (DX + 0.9 * math.cos(b), DZ - 0.9 * math.sin(b))
        d, _ = seg_distance(X, Z, p, q)
        best = np.minimum(best, d)
    return best * PPU, r, theta


def paint_floor(ruined, seed=11):
    rng = np.random.default_rng(seed)
    X, Z, ident, dist, palette, zone, outside = layout(rng)
    slabs = ident.max() + 1
    per_slab = rng.normal(0, 0.5, slabs)
    tilt = rng.normal(0, 0.05, (slabs, 2))
    hall_r = np.hypot(X - CX, Z - CZ)
    dais_r = np.hypot(X - DX, Z - DZ)
    dais_theta = np.arctan2(-(Z - DZ), X - DX)

    mottle = fbm(N, N, 26, rng, 4, tile_x=False, tile_y=False) - 0.5
    grain = vnoise(N, N, 3, rng, False, False) - 0.5
    height = np.clip(dist / 1.7, 0, 1) * (0.85 + 0.3 * mottle) + 0.02 * grain
    base = 3.9 + per_slab[ident] + 2.0 * mottle
    base = np.where(palette == 3, 2.6 + 0.6 * per_slab[ident], base)

    cracked = np.zeros((N, N), bool)
    for _ in range(26 if not ruined else 60):
        crack(cracked, rng, rng.uniform(60, 450), rng.uniform(40, 470), rng.uniform(10, 46), 0.7, wrap_x=False)
    missing = np.zeros((N, N), bool)
    scorch = np.zeros((N, N))
    ash = np.zeros((N, N), bool)
    if ruined:
        # Radial cracks torn out from the crater, and concentric ones round it.
        for k in range(34):
            a = rng.uniform(0, 2 * math.pi)
            start = rng.uniform(1.2, 3.2)
            crack(cracked, rng, (DZ - start * math.sin(a)) * PPU, (DX + start * math.cos(a)) * PPU,
                  rng.uniform(40, 170), 0.5, direction=math.atan2(-math.sin(a), math.cos(a)), wrap_x=False)
        ring_crack = (np.abs(dais_r - 3.9 - 0.5 * vnoise(N, N, 40, rng, False, False)) * PPU < 0.7) & \
                     (vnoise(N, N, 30, rng, False, False) > 0.35)
        ring_crack |= (np.abs(dais_r - 6.6 - 0.9 * vnoise(N, N, 50, rng, False, False)) * PPU < 0.6) & \
                      (vnoise(N, N, 34, rng, False, False) > 0.55)
        cracked |= ring_crack
        # Slabs blown out, most of them near the crater and under the fallen wall.
        chance = np.clip(0.34 - dais_r * 0.035, 0.05, 0.3) + np.clip((7.5 - Z) * 0.06, 0, 0.35)
        gone = rng.random(slabs)
        slab_x = np.zeros(slabs)
        slab_chance = np.zeros(slabs)
        np.maximum.at(slab_chance, ident.ravel(), chance.ravel())
        missing = (gone[ident] < slab_chance[ident]) & (zone != 3) & (palette != 3) & (dais_r > 2.0)
        streaks = vnoise(1, 720, 3, rng)[0][((dais_theta + math.pi) / (2 * math.pi) * 719).astype(int)]
        scorch = np.clip(1.3 - dais_r / (7.0 + 6.0 * streaks), 0, 1) ** 1.2
        scorch = np.maximum(scorch, np.clip(1.0 - dais_r / CRATER_R, 0, 1) ** 0.4)
        ash = (fbm(N, N, 34, rng, 3, tile_x=False, tile_y=False) > 0.6) & (scorch < 0.55)
    height = np.where(cracked, 0.12, height)

    light, _, _ = bevel_light(height, False, False)
    tone = base + 3.3 * light
    tone = np.where(dist < 0.9, 1.0 + 0.5 * per_slab[ident], tone)
    tone = np.where(cracked, 0.8, tone)

    ramps = np.stack([STONE, SLATE, WARM, BRONZE, DIRT, GOLD, AETHER, ASH])
    pal = palette.copy()

    # Ground outside the wall line, and (ruins) the dirt under missing slabs.
    rough = fbm(N, N, 9, rng, 3, tile_x=False, tile_y=False)
    pebbles = vnoise(N, N, 2.2, rng, False, False)
    dirt_tone = 2.4 + 3.4 * (rough - 0.5) + np.where(pebbles > 0.8, 2.0, 0.0)
    dirt_height = 0.25 + 0.5 * rough + np.where(pebbles > 0.8, 0.3, 0.0)
    soil = outside | missing
    tone = np.where(soil, dirt_tone, tone)
    height = np.where(soil, dirt_height * (0.4 if ruined else 0.7), height)
    pal = np.where(soil, 4, pal)
    if ruined:
        edge = missing & (dist < 1.5)
        tone = np.where(edge, 0.6, tone)

    # Gold studs in the inlay ring.
    stud = (palette == 3) & (dist > 2.2) & (dist < 4.4) & ~ruined
    tone = np.where(stud, 4.6, tone)
    pal = np.where(stud, 5, pal)

    emissive = np.zeros((N, N, 3))
    # Conduits.
    d, along = conduit_field(X, Z)
    channel = (d < 4.6) & ~((zone == 1) & (dais_r < 3.5))
    flow = fbm(N, N, 7, rng, 2, tile_x=False, tile_y=False)
    studs = (np.floor(along * 2.0) % 2 == 0)
    rim = channel & (d >= 2.1) & (d < 3.6)
    gap = channel & (d >= 3.6)
    glass = channel & (d < 2.1)
    core = channel & (d < 1.05)
    tone = np.where(gap, 0.7, tone)
    tone = np.where(rim, 3.6 + 1.2 * (flow - 0.5) + np.where(studs & (d < 3.0), 1.6, 0.0) - np.where(d > 3.0, 1.2, 0.0), tone)
    pal = np.where(rim | gap, 3, pal)
    height = np.where(gap, 0.1, np.where(rim, 0.95, np.where(glass, 0.45, height)))
    if not ruined:
        tone = np.where(glass, np.where(core, 6.0, 4.0 + 1.5 * (flow - 0.5)), tone)
        pal = np.where(glass, 6, pal)
        glow = np.where(core, 1.0, 0.55) * (0.75 + 0.5 * flow)
        emissive = np.where(glass[..., None], tones(np.where(core, 6.0, 4.0), AETHER) / 255.0 * glow[..., None], emissive)
    else:
        broken = glass & (vnoise(N, N, 14, rng, False, False) > 0.62)
        tone = np.where(glass, 0.6 + 1.8 * flow, tone)
        pal = np.where(glass, 4, pal)
        tone = np.where(rim, tone - 1.4, tone)
        ember = glass & ~broken & (flow > 0.7) & (dais_r < 9)
        tone = np.where(ember, 3.2, tone)
        pal = np.where(ember, 6, pal)
        emissive = np.where(ember[..., None], AETHER[4] / 255.0 * 0.5, emissive)

    # The dais: sigil lines on its top and steps (chamber); the crater's glassy veins (ruins).
    s, r, theta = sigil_field(X, Z)
    if not ruined:
        on_dais = dais_r < RING - 0.3
        line = on_dais & (s < 1.6)
        hot = on_dais & (s < 0.75)
        gold_ring = on_dais & ((np.abs(dais_r - 2.56) * PPU < 1.0) | (np.abs(dais_r - 1.75) * PPU < 0.8))
        tone = np.where(gold_ring, 4.4 + 1.4 * (flow - 0.5), tone)
        pal = np.where(gold_ring, 5, pal)
        tone = np.where(line, np.where(hot, 6.0, 3.6), tone)
        pal = np.where(line, 6, pal)
        height = np.where(line, 0.35, height)
        glow = np.where(hot, 1.0, 0.5)
        emissive = np.where(line[..., None], tones(np.where(hot, 6.0, 4.0), AETHER) / 255.0 * glow[..., None], emissive)
    else:
        # The crater: fused, scorched ground. Shock rings, a few glassy patches, and thin veins of
        # afterglow along the cracks, brightest at the centre.
        bowl = dais_r < CRATER_R
        radial = vnoise(1, 720, 4.0, rng)[0][((dais_theta + math.pi) / (2 * math.pi) * 719).astype(int)]
        glassy = fbm(N, N, 22, rng, 2, tile_x=False, tile_y=False)
        k = dais_r / CRATER_R
        rings = np.sin(dais_r * 5.2 + 2.0 * radial) > 0.72
        t = 1.4 + 2.2 * k + 1.0 * (radial - 0.5) + 0.8 * (glassy - 0.5) - np.where(rings, 0.9, 0.0)
        patch = bowl & (glassy > 0.66) & (k > 0.25)
        t = np.where(patch, 3.4 + 1.2 * (radial - 0.5), t)
        veins = np.zeros((N, N), bool)
        for i in range(13):
            a = rng.uniform(0, 2 * math.pi)
            crack(veins, rng, DZ * PPU, DX * PPU, rng.uniform(22, 50), 0.55, direction=a, wrap_x=False)
        tone = np.where(bowl, np.where(cracked & ~veins, 0.4, t), tone)
        pal = np.where(bowl, np.where(patch, 1, 4), pal)
        height = np.where(bowl, 0.5 + 0.25 * glassy - np.where(cracked | veins, 0.4, 0.0) - np.where(rings, 0.15, 0.0), height)
        vein = bowl & veins & (dais_r < 3.1)
        tone = np.where(vein, np.where(dais_r < 1.2, 5.0, 3.0), tone)
        pal = np.where(vein, 6, pal)
        emissive = np.where(vein[..., None], tones(np.where(dais_r < 1.2, 5.0, 4.0), AETHER) / 255.0
                            * np.clip(1.2 - dais_r / 3.2, 0.3, 1.0)[..., None], emissive)

    if ruined:
        tone = np.where(pal < 6, tone - 2.4 * scorch, tone) + np.where(pal < 6, 0.5, 0.0)
        pal = np.where(ash & (pal <= 2) & ~cracked & (dist > 1.5) & (dais_r > CRATER_R), 7, pal)
        tone = np.where(pal == 7, np.maximum(tone, 3.4) + 0.6, tone)
        debris = (vnoise(N, N, 2.0, rng, False, False) > 0.86) & (fbm(N, N, 40, rng, 2, tile_x=False, tile_y=False) > 0.5)
        tone = np.where(debris & (pal < 6), tone + 1.6, tone)
        height = np.where(debris, np.minimum(height + 0.35, 1.0), height)

    index = np.clip(np.rint(tone), 0, 7).astype(int)
    colour = ramps[pal, index]
    lit = emissive.sum(axis=-1) > 0
    colour = np.where(lit[..., None], colour * 0.4, colour)

    # Normal map: the height field plus a slight tilt of every slab.
    _, dx, dy = bevel_light(height, False, False)
    strength = 2.6
    nx = -dx * strength + np.where(soil, 0, tilt[ident][..., 0])
    ny = dy * strength + np.where(soil, 0, tilt[ident][..., 1])
    n = np.stack([nx, ny, np.ones_like(nx)], axis=-1)
    n /= np.linalg.norm(n, axis=-1, keepdims=True)
    normal = np.clip((n * 0.5 + 0.5) * 255 + 0.5, 0, 255).astype(np.uint8)
    return colour, normal, emissive * 255.0


def write_floor(folder, ruined):
    colour, normal, emissive = paint_floor(ruined)
    save(f"{folder}/floor.png", colour)
    save(f"{folder}/floor_n.png", normal)
    save(f"{folder}/floor_e.png", emissive)
