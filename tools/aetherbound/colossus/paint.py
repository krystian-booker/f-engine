"""Pixel-art painter for the charts of meshkit: old bronze, verdigris, gold inlay, dark joints,
cables and Aetherite. Every texel gets a tone index into a short ramp (so surfaces stay in a
limited palette with hard steps), from the plate's edge distance (rim light on upper edges,
outline on lower ones), its facing, height on the figure, and blotchy noise."""
import math
import numpy as np
from meshkit import chart_fields, noise3, fbm3, hex_rgb

RAMPS = {
    "bronze":    ["#120d0a", "#221a13", "#35291c", "#4a3b2c", "#63503a", "#82694a", "#a88d62"],
    "verdigris": ["#0e1a18", "#182b27", "#263f39", "#34554c", "#446b60", "#5b8477", "#7ea092"],
    "gold":      ["#3a2a10", "#5f4719", "#8a6a2a", "#b08a3a", "#c9a24a", "#e2c36c", "#f4e3a0"],
    "dark":      ["#08080b", "#101015", "#191a21", "#24252e", "#31333f", "#424553", "#585c6e"],
    "cable":     ["#070609", "#110e13", "#1d181e", "#2c242a", "#42353a", "#5b494c", "#766163"],
    "scorch":    ["#0b0616", "#170b2b", "#291247", "#3a1a66", "#5a2d96", "#8048cf", "#b48cff"],
    "stone":     ["#101119", "#1b1d2e", "#27293d", "#34364d", "#484a63", "#60627e", "#7f819c"],
}
RAMPS = {k: np.array([hex_rgb(h) for h in v]) for k, v in RAMPS.items()}
VIOLET = hex_rgb("#9b5cff")
CORE = hex_rgb("#e2ccff")
DEEP = hex_rgb("#3a1a66")


def _segments_mask(px, py, segs, width):
    m = np.zeros(px.shape, dtype=bool)
    for (x0, y0, x1, y1) in segs:
        ex, ey = x1 - x0, y1 - y0
        L2 = ex * ex + ey * ey + 1e-9
        t = np.clip(((px - x0) * ex + (py - y0) * ey) / L2, 0, 1)
        dist = np.hypot(px - (x0 + t * ex), py - (y0 + t * ey))
        m |= dist < width / 2.0
    return m


def paint_body(c, ctx):
    """Returns albedo [h, w, 3], height [h, w], emissive [h, w, 3] for a body chart."""
    F = chart_fields(c)
    px, py, pos, nrm, d = F["px"], F["py"], F["pos"], F["nrm"], F["d"]
    P = c.params
    cls = c.cls
    lit = -F["ey"] * 0.85 - F["ex"] * 0.45            # > 0 on upper and left edges (image space)
    seed = P.get("seed", 0)
    # the plate's own pattern is sampled in whole texels so it stays crisp
    q = np.floor(pos * 32.0) / 32.0
    blotch = fbm3(q, 1.1, 11) - 0.5
    grain = noise3(q, 9.0, 23) - 0.5
    speck = noise3(q, 31.0, 5) - 0.5

    tone = np.full(px.shape, 2.7)
    tone += 0.95 * nrm[..., 2]                         # upward faces catch the light
    tone += -0.35 * nrm[..., 1]                        # the front a touch lighter than the back
    tone += (pos[..., 2] - 3.6) * 0.14                 # darker towards the feet
    tone += blotch * 1.5 + grain * 0.7 + speck * 0.35
    tone += P.get("tone", 0.0)
    height = np.zeros(px.shape)

    edge = P.get("edge", 1.4)
    if cls in ("bronze", "verdigris", "gold", "stone"):
        zone1 = d < edge
        zone2 = (d >= edge) & (d < edge + 1.0)
        tone = np.where(zone1 & (lit > 0.25), tone + 1.7, tone)
        tone = np.where(zone1 & (lit < -0.25), tone - 1.6, tone)
        tone = np.where(zone1 & (np.abs(lit) <= 0.25), tone + 0.4, tone)
        tone = np.where(zone2 & (lit > 0.25), tone + 0.5, tone)
        tone = np.where(zone2 & (lit < -0.25), tone - 0.7, tone)
        tone = np.where(d < -1.3, tone - 1.3, tone)    # the plate's thickness
        height = np.clip(d / (edge + 1.0), -1.0, 1.0) * 0.8
    ramp_id = np.full(px.shape, {"bronze": 0, "verdigris": 1, "gold": 2, "dark": 3, "cable": 4, "stone": 6}[cls])

    if cls == "bronze":
        streak = noise3(q * np.array([3.0, 3.0, 0.7]), 1.6, 41) - 0.5
        crev = np.clip(1.0 - d / 5.0, 0, 1) * (lit < 0.2)
        v = fbm3(q, 1.9, 77) + 0.22 * crev + 0.25 * streak - 0.1 * nrm[..., 2] + P.get("patina", 0.0)
        mask = (v > 0.69) & (d > 0.6)
        ramp_id = np.where(mask, 1, ramp_id)
        rim = (v > 0.69) & (v < 0.72)
        tone = np.where(rim, tone - 0.9, tone)
        height += mask * 0.15
    elif cls == "verdigris":
        v = fbm3(q, 2.3, 91)
        worn = ((v < 0.36) | ((d < edge) & (lit > 0.25) & (grain > -0.1))) & (d > -1.0)
        ramp_id = np.where(worn, 0, ramp_id)
        tone = np.where(worn, tone + 0.5, tone)
    elif cls == "dark":
        ribs = P.get("ribs", 0)
        if ribs:
            band = np.floor(py / ribs).astype(int) % 2
            tone = tone + np.where(band == 0, 0.7, -0.5)
            height += band * 0.5
        tone = tone + 0.3
    elif cls == "cable":
        pitch = P.get("ribs", 3)
        band = (np.floor((py + px * P.get("skew", 0.0)) / pitch).astype(int) % 2)
        tone = tone + np.where(band == 0, 0.9, -0.6)
        height += band * 0.6
    elif cls == "gold":
        tone = tone + 0.6

    # decorations ---------------------------------------------------------------------------
    inlay = P.get("inlay")
    if inlay and cls in ("bronze", "verdigris"):
        band = (d >= inlay) & (d < inlay + P.get("inlay_w", 1.25))
        ramp_id = np.where(band, 2, ramp_id)
        tone = np.where(band, 3.2 + 1.3 * (lit > 0) + grain * 1.2, tone)
        below = (d >= inlay + P.get("inlay_w", 1.25)) & (d < inlay + P.get("inlay_w", 1.25) + 1.0)
        tone = np.where(below, tone - 0.7, tone)
        height = np.where(band, height + 0.35, height)
    trim = P.get("trim")
    if trim:
        band = (d >= 0.0) & (d < trim)
        ramp_id = np.where(band, 2, ramp_id)
        tone = np.where(band, 3.0 + 1.6 * (lit > 0.25) - 1.2 * (lit < -0.25) + grain, tone)
        tone = np.where((d >= trim) & (d < trim + 1.0), tone - 1.0, tone)
    motif = P.get("motif")
    if motif:
        segs = [(a * c.cw, b * c.ch, e * c.cw, f * c.ch) for a, b, e, f in motif]
        m = _segments_mask(px, py, segs, P.get("motif_w", 1.6)) & (d > 1.0)
        sh = _segments_mask(px, py - 1.0, segs, P.get("motif_w", 1.6)) & ~m & (d > 1.0)
        ramp_id = np.where(m, 2, ramp_id)
        tone = np.where(m, 3.6 + grain * 1.5, tone)
        tone = np.where(sh, tone - 1.0, tone)
        height = np.where(m, height + 0.35, height)
    lines = P.get("lines")
    if lines:                                           # engraved panel lines
        segs = [(a * c.cw, b * c.ch, e * c.cw, f * c.ch) for a, b, e, f in lines]
        m = _segments_mask(px, py, segs, 1.1) & (d > 1.0)
        hl = _segments_mask(px, py - 1.0, segs, 1.1) & ~m & (d > 1.0)
        tone = np.where(m, tone - 2.0, tone)
        tone = np.where(hl, tone + 0.8, tone)
        height = np.where(m, height - 0.6, height)
    rivets = P.get("rivets")
    if rivets and min(c.cw, c.ch) >= 9:
        pts = []
        inset = P.get("rivet_inset", 3.5 if not inlay else inlay + 3.5)
        if c.poly is not None:
            poly = np.array(c.poly)
            cen = poly.mean(axis=0)
            for i in range(len(poly)):
                a, b = poly[i], poly[(i + 1) % len(poly)]
                L = np.linalg.norm(b - a)
                e = (b - a) / max(L, 1e-6)
                nin = cen - (a + b) / 2
                nin = nin - e * np.dot(nin, e)
                nin = nin / (np.linalg.norm(nin) + 1e-9)
                k = max(1, int(round((L - 2 * inset) / rivets)))
                if L < 2 * inset + 2:
                    continue
                for j in range(k + 1):
                    pts.append(a + e * (inset + (L - 2 * inset) * j / k) + nin * inset)
        else:
            for yy in (inset, c.ch - inset):
                k = max(1, int(round((c.cw - 2 * inset) / rivets)))
                for j in range(k + 1):
                    pts.append((inset + (c.cw - 2 * inset) * j / k, yy))
            if not c.open_u:
                for xx in (inset, c.cw - inset):
                    k = max(1, int(round((c.ch - 2 * inset) / rivets)))
                    for j in range(1, k):
                        pts.append((xx, inset + (c.ch - 2 * inset) * j / k))
        for (rx, ry) in pts:
            rx, ry = math.floor(rx), math.floor(ry)
            dx, dy = px - 0.5 - rx, py - 0.5 - ry
            body = (dx >= 0) & (dx < 2) & (dy >= 0) & (dy < 2)
            if P.get("rivet_gold"):
                ramp_id = np.where(body, 2, ramp_id)
            tone = np.where(body, np.where((dx < 1) & (dy < 1), 5.4, np.where((dx >= 1) & (dy >= 1), 1.8, 3.8)), tone)
            shadow = (dx >= 0) & (dx < 3) & (dy >= 2) & (dy < 3)
            tone = np.where(shadow & ~body, tone - 1.2, tone)
            height = np.where(body, height + 0.8, height)

    # the wound: scorched violet around it, glowing cracks ---------------------------------------
    emis = np.zeros(px.shape + (3,))
    f = np.zeros(px.shape)
    for (wp, wr) in ctx.get("wounds", []):
        f = np.maximum(f, np.clip(1.0 - np.linalg.norm(pos - np.asarray(wp), axis=2) / wr, 0, 1))
    if f.max() > 0 and not P.get("no_wound"):
        crack = np.abs(fbm3(q, 2.6, 301, 2) - 0.5)
        crack2 = np.abs(fbm3(q, 4.4, 311, 2) - 0.5)
        lines_m = ((crack < 0.012 + 0.03 * f) | (crack2 < 0.02 * f)) & (f > 0.08)
        scorch = f > (0.35 + grain * 0.3)
        ramp_id = np.where(scorch, 5, ramp_id)
        tone = np.where(scorch, tone - 0.6, tone - 1.2 * f)
        glow = np.where(lines_m, np.clip(f * 1.6, 0, 1), 0.0) + np.clip(f - 0.72, 0, 1) * 2.2
        glow = np.clip(glow, 0, 1)
        emis = (VIOLET * (1 - glow[..., None] ** 2) + CORE * glow[..., None] ** 2) * glow[..., None]
        tone = np.where(lines_m, tone + 2.0 * f, tone)
        height = np.where(lines_m, height - 0.8, height)

    ti = np.clip(np.rint(tone), 0, 6).astype(int)
    stack = np.stack([RAMPS["bronze"], RAMPS["verdigris"], RAMPS["gold"], RAMPS["dark"], RAMPS["cable"], RAMPS["scorch"], RAMPS["stone"]])
    albedo = stack[ramp_id, ti]
    height += (grain * 0.25 + speck * 0.15)
    return albedo, height, emis


def paint_crystal(c, ctx):
    """Aetherite: albedo (deep violet facets) and an emissive image with bright veins and a hot
    core line. Returns albedo, emissive."""
    F = chart_fields(c)
    px, py, pos = F["px"], F["py"], F["pos"]
    P = c.params
    n = P.get("facets", 6)
    colw = c.cw / n
    col = np.clip(np.floor(px / colw), 0, n - 1).astype(int)
    seed = P.get("seed", 0)
    rng = np.random.RandomState(1000 + seed)
    # every facet has its own flat tone, as cut gems are drawn: two bright, the rest stepping down
    tones = np.array([0.42, 0.95, 0.62, 0.3, 0.75, 0.5, 0.36, 0.85])[:n]
    tones = np.roll(tones, rng.randint(0, n))
    base = tones[col]
    fx = (px - col * colw) / colw                         # 0..1 across the facet
    v = py / c.ch                                         # 0 at the tip
    q = np.floor(pos * 24.0) / 24.0
    vein = np.abs(fbm3(q * np.array([1.0, 1.0, 0.45]), 2.2, 400 + seed, 2) - 0.5)
    veins = vein < P.get("vein", 0.022)
    cloud = fbm3(q, 1.3, 500 + seed) - 0.5
    hot = P.get("hot", 0.5)                               # where along the shard the glow is hottest (0 tip .. 1 butt)
    heat = np.clip(1.0 - np.abs(v - hot) / P.get("hot_width", 0.6), 0, 1)
    lum = base * (0.55 + 0.5 * heat) + cloud * 0.35
    lum = np.where((fx < 0.09) | (fx > 0.91), lum + 0.3, lum)        # bright facet edges
    lum = np.where(veins, lum + 0.55, lum)
    lum = lum * P.get("gain", 1.0)
    steps = np.array([0.0, 0.16, 0.3, 0.46, 0.64, 0.84, 1.05])
    li = np.clip(np.digitize(lum, steps) - 1, 0, 6)
    ramp = np.array([hex_rgb(h) for h in ("#1c0a3a", "#2e1260", "#4a2396", "#6b3bd0", "#9b5cff", "#c39dff", "#e2ccff")])
    emis_ramp = ramp * np.array([0.04, 0.09, 0.17, 0.3, 0.52, 0.85, 1.2])[:, None]
    return ramp[li] * 0.7, emis_ramp[li]


def normal_from_height(h, strength=1.6):
    gx = np.zeros_like(h); gy = np.zeros_like(h)
    gx[:, 1:-1] = (h[:, 2:] - h[:, :-2]) * 0.5
    gy[1:-1, :] = (h[2:, :] - h[:-2, :]) * 0.5
    n = np.stack([-gx * strength, -gy * strength, np.ones_like(h)], axis=2)
    n /= np.linalg.norm(n, axis=2, keepdims=True)
    return n * 0.5 + 0.5
