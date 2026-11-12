"""The Aetherbound Colossus: skeleton and armour, built with meshkit.

Blender coordinates: x = its left, -y = forward, z = up, origin between the feet.
"""
import math
import numpy as np
from meshkit import Builder, vec, unit, rot_x, rot_y, rot_z, euler, xf, aim, apply

# ---- skeleton: name -> (parent, rest position of the joint) -------------------------------------
CHEST = (0.0, 0.1, 3.5)
CHEST_TILT = 24.0                                   # the torso is built hunched forward by this much
MC = xf(CHEST, rot_x(CHEST_TILT))


def cl(x, y, z):
    """Chest-local coordinates (x left, -y front, z up along the tilted torso) to model space."""
    return apply(MC, (x, y, z))


SKEL = {
    "root": (None, (0, 0, 0)),
    "hips": ("root", (0, 0.2, 2.5)),
    "spine": ("hips", (0, 0.2, 2.9)),
    "chest": ("spine", CHEST),
    "head": ("chest", tuple(cl(0, -1.4, 1.85))),
    "eye_L": ("head", (0, 0, 0)),                    # set in build()
    "core": ("chest", (0, 0, 0)),
    "pauldron_L": ("chest", tuple(cl(2.4, 0.2, 2.1))),
    "upper_arm_L": ("chest", tuple(cl(2.4, 0.2, 2.0))),
    "forearm_L": ("upper_arm_L", (3.0, -0.2, 3.85)),
    "hand_L": ("forearm_L", (3.15, -1.05, 2.05)),
    "fingers1_L": ("hand_L", (0, 0, 0)),
    "fingers2_L": ("fingers1_L", (0, 0, 0)),
    "thumb_L": ("hand_L", (0, 0, 0)),
    "thigh_L": ("hips", (0.85, 0.2, 2.35)),
    "shin_L": ("thigh_L", (1.12, -0.42, 1.3)),
    "foot_L": ("shin_L", (1.18, 0.12, 0.45)),
    "tasset_F": ("hips", (0, -0.4, 2.72)),
    "tasset_B": ("hips", (0, 0.8, 2.74)),
    "tasset_L": ("hips", (0.95, -0.2, 2.74)),
}


def mirror_skel():
    for n in list(SKEL):
        if n.endswith("_L"):
            p, h = SKEL[n]
            SKEL[n[:-2] + "_R"] = (p[:-2] + "_R" if p.endswith("_L") else p, (-h[0], h[1], h[2]))


SPIRE_ENTRY = cl(0.14, 1.36, 1.9)                   # where the spire goes in, high on the back
SPIRE_EXIT = cl(-0.14, -1.3, 0.74)                  # and comes out, low on the chest: the wound, the core
SPIRE_AXIS = unit(SPIRE_EXIT - SPIRE_ENTRY)         # from high behind to low in front
SPIRE_BACK_TIP = SPIRE_ENTRY - SPIRE_AXIS * 2.55
SPIRE_FRONT_TIP = SPIRE_EXIT + SPIRE_AXIS * 1.55
N_SHARDS = 15


def J(name):
    return vec(SKEL[name][1])


def ell_patch(centre, radii, lon, lat, cols, rows, M=None, flare=0.0):
    """Grid on an ellipsoid: lon = azimuth range in degrees (0 = +x, 90 = +y, increasing counter-
    clockwise seen from above), lat = (bottom, top) in degrees. Row 0 is the top. flare pushes
    the lower rows outwards (a lame lifting off the one below)."""
    g = np.zeros((rows + 1, cols + 1, 3))
    for r in range(rows + 1):
        th = math.radians(lat[1] + (lat[0] - lat[1]) * r / rows)
        k = 1.0 + flare * r / rows
        for c in range(cols + 1):
            ph = math.radians(lon[0] + (lon[1] - lon[0]) * c / cols)
            g[r, c] = (centre[0] + radii[0] * k * math.cos(th) * math.cos(ph),
                       centre[1] + radii[1] * k * math.cos(th) * math.sin(ph),
                       centre[2] + radii[2] * math.sin(th))
    if M is not None:
        g = apply(M, g.reshape(-1, 3)).reshape(g.shape)
    return g


def cyl_patch(radii, lon, z, cols, rows, M=None, flare=(1.0, 1.0), centre=(0, 0)):
    """Grid on an elliptical cylinder about local z: z = (bottom, top); flare = radius scale (top, bottom)."""
    g = np.zeros((rows + 1, cols + 1, 3))
    for r in range(rows + 1):
        t = r / rows
        k = flare[0] + (flare[1] - flare[0]) * t
        zz = z[1] + (z[0] - z[1]) * t
        for c in range(cols + 1):
            ph = math.radians(lon[0] + (lon[1] - lon[0]) * c / cols)
            g[r, c] = (centre[0] + radii[0] * k * math.cos(ph), centre[1] + radii[1] * k * math.sin(ph), zz)
    if M is not None:
        g = apply(M, g.reshape(-1, 3)).reshape(g.shape)
    return g


def ball(B, centre, r, sides=8, rings=4, M=None, squash=(1, 1, 1)):
    prof = [(r * math.cos(math.radians(a)), r * math.sin(math.radians(a))) for a in np.linspace(90, -90, rings + 1)]
    prof[0] = (0.0, r); prof[-1] = (0.0, -r)
    m = xf(centre) if M is None else M @ xf(centre)
    if squash != (1, 1, 1):
        m = m @ np.diag([squash[0], squash[1], squash[2], 1.0])
    B.lathe(prof, sides, m, sharp=False)


def cyl(B, a, b, r, sides=8, caps=True, extra=None, r2=None, x_hint=(1, 0, 0)):
    """A cylinder from a (bottom) to b (top)."""
    a, b = vec(a), vec(b)
    L = np.linalg.norm(b - a)
    r2 = r if r2 is None else r2
    prof = [(r2, L), (r, 0.0)]
    if caps:
        prof = [(0.0, L)] + prof + [(0.0, 0.0)]
    B.lathe(prof, sides, aim(a, b - a, x_hint), extra=extra)


def cone(B, base, tip, r, sides=4, extra=None):
    base, tip = vec(base), vec(tip)
    B.lathe([(0.0, np.linalg.norm(tip - base)), (r, 0.0)], sides, aim(base, tip - base), extra=extra)


def blend(a, b):
    def w(t):
        s = min(max((t - 0.25) / 0.5, 0.0), 1.0)
        s = s * s * (3 - 2 * s)
        return [(a, 1 - s), (b, s)] if 0 < s < 1 else [(a, 1.0)] if s <= 0 else [(b, 1.0)]
    return w


def blend3(a, b, c):
    def w(t):
        if t < 0.5:
            s = min(max(t / 0.5, 0.0), 1.0)
            return [(a, 1 - s), (b, s)] if s > 0 else [(a, 1.0)]
        s = min(max((t - 0.5) / 0.5, 0.0), 1.0)
        return [(b, 1 - s), (c, s)] if s < 1 else [(c, 1.0)]
    return w


def arc_path(a, b, bulge, n=5):
    a, b, bulge = vec(a), vec(b), vec(bulge)
    return [a + (b - a) * t + bulge * math.sin(math.pi * t) for t in np.linspace(0, 1, n)]


def build(density=32.0):
    B = Builder(density)
    info = {}
    S = B.set
    INL = dict(inlay=4)

    # ================================ legs (left, mirrored) ===================================
    B.mirror_begin()
    ankle, knee, hip = J("foot_L"), J("shin_L"), J("thigh_L")
    fx = ankle[0]
    # --- foot: a broad clawed boot
    S(bone="foot_L", cls="bronze", params=dict(tone=-0.8))
    B.box((fx, -0.2, 0.17), (1.16, 1.72, 0.34), taper=(0.9, 0.94), chamfer=0.05, skip="-z")
    S(cls="bronze", params=dict(inlay=3, rivets=10, rivet_inset=6, tone=0.3))
    B.shell(ell_patch((fx, -0.55, 0.3), (0.63, 0.68, 0.5), (-178, -2), (4, 86), 6, 3), 0.07, rim="b")
    S(cls="gold", params=dict(tone=-1.2))
    for dx in (-0.39, 0.0, 0.39):
        B.box((0, 0, 0.3), (0.31, 0.3, 0.6), M=xf((fx + dx, -0.92, 0.17), rot_x(97)), taper=(0.3, 0.3), chamfer=0.03, skip="-z")
    S(cls="verdigris", params=dict(trim=1.2, rivets=10))
    B.box((0, 0, 0), (0.9, 0.66, 0.15), M=xf((fx, -0.16, 0.66), rot_x(-26)), chamfer=0.04, skip="-z")
    S(cls="bronze", params=dict(tone=-0.5))
    B.box((fx, 0.6, 0.25), (0.84, 0.46, 0.5), taper=(0.8, 0.7), top_shift=(0, -0.1), chamfer=0.05, skip="-z")
    S(cls="dark", params=dict(ribs=3))
    cyl(B, (fx - 0.52, ankle[1], 0.45), (fx + 0.52, ankle[1], 0.45), 0.33, 8)
    # --- shin: a flared greave
    Ms = aim(ankle, knee - ankle)
    S(bone="shin_L", cls="dark", params=dict(ribs=4))
    B.box((0, 0.06, 0.48), (0.62, 0.7, 0.82), M=Ms, chamfer=0.05, skip="-z+z")
    S(cls="bronze", params=dict(inlay=4, rivets=14, lines=[(0.5, 0.22, 0.5, 0.8)]))
    B.box((0, -0.3, 0.52), (0.92, 0.42, 1.0), M=Ms, taper=(1.32, 1.0), top_shift=(0, -0.1), chamfer=0.06)
    S(cls="bronze", params=dict(tone=-0.3, rivets=12))
    B.box((0.5, 0.08, 0.52), (0.15, 0.7, 0.8), M=Ms, taper=(1.0, 1.15), chamfer=0.035)
    B.box((-0.5, 0.08, 0.52), (0.15, 0.7, 0.8), M=Ms, taper=(1.0, 1.15), chamfer=0.035)
    S(cls="bronze", params=dict(tone=-0.6))
    B.box((0, 0.44, 0.6), (0.56, 0.32, 0.66), M=Ms, taper=(1.1, 0.7), chamfer=0.05, skip="-y")
    S(cls="gold", params=dict(tone=-0.6))
    B.box((0, 0.02, 0.08), (0.98, 0.94, 0.12), M=Ms, chamfer=0.035)
    S(cls="cable", params=dict(ribs=3))
    B.tube([apply(Ms, (0.18, 0.56, 0.75)), apply(Ms, (0.2, 0.66, 0.4)), apply(Ms, (0.18, 0.5, 0.05))], 0.075, 5)
    # knee cap: a pointed shield with a boss
    Mk = aim(apply(Ms, (0, -0.54, 0.98)), -Ms[:3, 1] + Ms[:3, 2] * 0.2, x_hint=(1, 0, 0))
    S(cls="verdigris", params=dict(trim=1.3, inlay=4.5))
    B.plate([(-0.44, -0.5), (0.44, -0.5), (0.58, 0.2), (0, 0.66), (-0.58, 0.2)], 0.18, Mk, bevel=0.07)
    S(cls="gold", params=dict(tone=-0.4))
    cone(B, apply(Mk, (0, 0.02, 0.16)), apply(Mk, (0, 0.02, 0.48)), 0.17, 6)
    # --- thigh
    Mt = aim(knee, hip - knee)
    S(bone="thigh_L", cls="dark", params=dict(ribs=3))
    cyl(B, apply(Mt, (0, 0, 0.05)), apply(Mt, (0, 0, 1.2)), 0.36, 8, caps=False)
    cyl(B, knee + vec(-0.5, 0, 0), knee + vec(0.5, 0, 0), 0.36, 8)
    S(cls="bronze", params=dict(inlay=4, rivets=12))
    B.box((0, -0.27, 0.62), (0.86, 0.32, 0.9), M=Mt, taper=(1.2, 1.0), chamfer=0.06)
    S(cls="bronze", params=dict(tone=-0.3, rivets=12))
    B.box((0.44, 0.05, 0.62), (0.15, 0.7, 0.8), M=Mt, taper=(1.0, 1.1), chamfer=0.035)
    S(cls="cable", params=dict(ribs=3), bone="thigh_L")
    B.tube(arc_path(hip + vec(0.05, 0.46, -0.1), knee + vec(0, 0.46, 0.15), (0, 0.18, 0), 4), 0.09, 5)
    B.tube(arc_path(hip + vec(-0.3, 0.3, -0.15), knee + vec(-0.3, 0.3, 0.2), (-0.1, 0.12, 0), 4), 0.075, 5)
    # --- side tasset: two layered hanging plates
    S(bone="tasset_L", cls="bronze", params=dict(inlay=4, rivets=12))
    Mta = xf(J("tasset_L"), rot_z(34) @ rot_x(90 - 10))
    B.plate([(-0.52, 0.04), (0.52, 0.04), (0.52, -0.74), (0, -1.14), (-0.52, -0.74)], 0.1, Mta @ xf((0, 0, 0.2)), bevel=0.06, depth=0.1)
    S(cls="verdigris", params=dict(trim=1.3))
    B.plate([(-0.42, 0.1), (0.42, 0.1), (0.38, -0.36), (0, -0.58), (-0.38, -0.36)], 0.1, Mta @ xf((0, 0, 0.3)), bevel=0.05)
    # --- hip ball
    S(bone="hips", cls="dark", params=dict(ribs=3))
    ball(B, hip, 0.5, 8, 4)
    B.mirror_end(share=True)

    # ================================ pelvis and waist ========================================
    S(bone="hips", cls="bronze", params=dict(tone=-0.9))
    B.box((0, 0.2, 2.42), (1.72, 1.12, 0.62), taper=(1.16, 1.06), chamfer=0.07)
    S(cls="gold", params=dict(lines=[(0.0, 0.5, 1.0, 0.5)], tone=-0.5))
    Mb = xf((0, 0.2, 2.78)) @ np.diag([1.0, 0.72, 1.0, 1.0])
    B.lathe([(1.0, 0.12), (1.14, 0.12), (1.14, -0.12), (1.0, -0.12)], 14, Mb)
    S(bone="tasset_F", cls="verdigris", params=dict(inlay=3.5, motif=[(0.5, 0.2, 0.5, 0.75), (0.3, 0.42, 0.5, 0.2), (0.7, 0.42, 0.5, 0.2)]))
    B.plate([(-0.46, 0.06), (0.46, 0.06), (0.38, -0.56), (0, -0.96), (-0.38, -0.56)], 0.2,
            xf(J("tasset_F"), rot_x(90 - 8)) @ xf((0, 0, 0.16)), bevel=0.07, depth=0.16)
    S(bone="tasset_B", cls="bronze", params=dict(inlay=4, rivets=12))
    B.plate([(-0.7, 0.02), (0.7, 0.02), (0.6, -0.7), (0, -1.02), (-0.6, -0.7)], 0.12,
            xf(J("tasset_B"), rot_z(180) @ rot_x(90 - 10)) @ xf((0, 0, 0.1)), bevel=0.06, depth=0.1)
    # waist: ribbed dark column, cables, an abdominal plate, spine blocks
    S(bone="spine", cls="dark", params=dict(ribs=0))
    Mw = xf(J("spine"), rot_x(12)) @ np.diag([1.2, 0.95, 1.0, 1.0])
    B.lathe([(0.62, 0.8), (0.76, 0.68), (0.62, 0.56), (0.76, 0.44), (0.62, 0.32), (0.76, 0.2), (0.64, 0.08), (0.72, -0.08)], 10, Mw)
    S(cls="verdigris", params=dict(trim=1.3))
    B.plate([(-0.36, 0.3), (0.36, 0.3), (0.28, -0.3), (-0.28, -0.3)], 0.12, xf((0, -0.62, 3.1), rot_x(90 + 8)), bevel=0.05)
    S(cls="bronze", params=dict(tone=-0.3))
    B.box((0, 0.98, 3.02), (0.46, 0.3, 0.3), chamfer=0.05, skip="-y")
    B.box((0, 0.96, 3.38), (0.5, 0.3, 0.3), chamfer=0.05, skip="-y")
    S(cls="cable", params=dict(ribs=3, skew=0.5))
    for sx in (-1, 1):
        B.tube(arc_path((sx * 0.8, -0.2, 2.72), cl(sx * 1.05, -0.5, 0.3), (sx * 0.24, -0.12, 0), 5), 0.1, 5,
               weights=blend3("hips", "spine", "chest"))
        B.tube(arc_path((sx * 0.85, 0.6, 2.72), cl(sx * 1.1, 0.6, 0.3), (sx * 0.22, 0.14, 0), 5), 0.1, 5,
               weights=blend3("hips", "spine", "chest"))

    # ================================ chest ====================================================
    Mc = MC
    S(bone="chest", cls="bronze", params=dict(tone=-1.1))
    B.box((0, -0.05, 1.15), (3.3, 2.1, 2.1), M=Mc, taper=(1.2, 0.9), top_shift=(0, 0.1), chamfer=0.1, skip="-z")
    B.mirror_begin()
    # pectoral plates
    S(cls="bronze", params=dict(inlay=4, rivets=14, tone=0.2))
    B.shell(ell_patch((0.88, -0.8, 1.5), (1.0, 0.62, 0.82), (-172, -8), (-48, 62), 6, 4, Mc), 0.09)
    # clavicle plates between collar and pauldron
    S(cls="verdigris", params=dict(trim=1.3, rivets=10))
    B.shell(ell_patch((1.35, -0.45, 1.95), (0.8, 0.8, 0.5), (-150, -10), (22, 82), 5, 2, Mc), 0.07)
    # side lames
    S(cls="bronze", params=dict(trim=1.2, tone=0.1))
    B.shell(ell_patch((1.44, -0.05, 1.0), (0.62, 1.1, 0.95), (-78, 78), (22, 58), 5, 2, Mc, flare=0.08), 0.07)
    S(cls="verdigris", params=dict(rivets=12))
    B.shell(ell_patch((1.42, -0.05, 0.97), (0.62, 1.08, 0.95), (-74, 74), (-10, 28), 5, 2, Mc, flare=0.08), 0.07)
    S(cls="bronze", params=dict(trim=1.2, tone=-0.2))
    B.shell(ell_patch((1.36, -0.05, 0.97), (0.62, 1.04, 0.97), (-70, 70), (-42, -4), 5, 2, Mc, flare=0.06), 0.07)
    # shoulder ball
    S(cls="dark", params=dict(ribs=3))
    ball(B, J("upper_arm_L") + vec(-0.12, 0, 0), 0.62, 8, 4)
    # exhaust stacks on the hump
    for (ox, oy, oz, ln) in ((0.7, 1.22, 2.55, 0.85), (1.2, 1.08, 2.25, 0.62)):
        a = cl(ox, oy, oz)
        b = a + unit(vec(0.2, 0.62, 1.0)) * ln
        S(cls="dark", params=dict(ribs=4))
        cyl(B, a - unit(b - a) * 0.3, b, 0.18, 7)
        S(cls="bronze", params=dict(tone=0.6, edge=0.6))
        cyl(B, b - unit(b - a) * 0.12, b + unit(b - a) * 0.04, 0.215, 7, caps=False)
    B.mirror_end(share=False)
    # abdominal lames across the front
    S(cls="verdigris", params=dict(trim=1.3))
    B.shell(ell_patch((0, -0.2, 0.62), (1.5, 1.1, 0.66), (-164, -16), (-30, 18), 7, 2, Mc, flare=0.05), 0.07)
    S(cls="bronze", params=dict(rivets=14, tone=-0.1))
    B.shell(ell_patch((0, -0.2, 0.28), (1.36, 1.02, 0.52), (-160, -20), (-34, 14), 7, 2, Mc, flare=0.05), 0.07)
    # the hump: three big overlapping lames up the back
    S(cls="bronze", params=dict(inlay=4, rivets=16))
    B.shell(ell_patch((0, 0.4, 1.8), (1.7, 1.3, 1.5), (-8, 188), (40, 86), 12, 3, Mc), 0.1)
    S(cls="verdigris", params=dict(trim=1.4, patina=0.1))
    B.shell(ell_patch((0, 0.4, 1.78), (1.79, 1.39, 1.52), (4, 176), (12, 46), 12, 2, Mc, flare=0.04), 0.09)
    S(cls="bronze", params=dict(rivets=16, tone=-0.2))
    B.shell(ell_patch((0, 0.4, 1.76), (1.82, 1.42, 1.52), (10, 170), (-22, 18), 12, 2, Mc, flare=0.04), 0.09)
    S(cls="bronze", params=dict(tone=-0.4, inlay=3))
    B.shell(ell_patch((0, 0.3, 1.0), (1.56, 1.2, 0.95), (20, 160), (-50, -16), 6, 2, Mc), 0.08)
    # dorsal fins down the spine below the wound
    S(cls="bronze", params=dict(trim=1.2, tone=0.4))
    hc, hr = vec(0, 0.4, 1.76), vec(1.82, 1.42, 1.52)
    def hump(lat, out=0.0):
        t = math.radians(lat)
        p = hc + vec(0, hr[1] * math.cos(t), hr[2] * math.sin(t))
        n = unit(vec(0, math.cos(t) / hr[1], math.sin(t) / hr[2]))
        return p + n * out
    for lat, sc in ((24, 1.0), (0, 0.85), (-24, 0.7)):
        a0, a1 = hump(lat + 11 * sc, -0.05), hump(lat - 11 * sc, -0.05)
        t0, t1 = hump(lat + 5 * sc, 0.22 * sc), hump(lat - 13 * sc, 0.62 * sc)
        B.hexa([[[a0 + vec(-0.08, 0, 0), t0 + vec(-0.03, 0, 0)], [a1 + vec(-0.08, 0, 0), t1 + vec(-0.03, 0, 0)]],
                [[a0 + vec(0.08, 0, 0), t0 + vec(0.03, 0, 0)], [a1 + vec(0.08, 0, 0), t1 + vec(0.03, 0, 0)]]], 0.02, M=Mc, skip="-z")
    # collar around the back of the head
    S(cls="bronze", params=dict(trim=1.4, tone=0.2))
    B.shell(cyl_patch((0.9, 0.84), (-35, 215), (1.95, 2.55), 8, 1, Mc, flare=(1.14, 1.0), centre=(0, -0.86)), 0.08)
    # neck
    H = J("head")
    S(bone=blend("chest", "head"), cls="dark", params=dict(ribs=3))
    B.tube([cl(0, -0.6, 1.75), H + vec(0, 0.3, -0.02), H + vec(0, -0.1, 0.12)], 0.34, 7)

    # the reactor core: a gold ring in the sternum, a glowing dome in it, pierced by the spire
    core = SPIRE_EXIT
    SKEL["core"] = ("chest", tuple(core))
    info["core"] = core
    core_n = unit(Mc[:3, :3] @ vec(-0.05, -1, -0.3))
    Mcore = aim(core - core_n * 0.1, core_n)
    S(bone="chest", cls="gold", params=dict(no_wound=True, tone=-0.3))
    B.lathe([(0.46, 0.16), (0.64, 0.12), (0.7, -0.12)], 10, Mcore, sharp=True)
    S(bone="core", cls="crystal", mat="crystal", params=dict(seed=90, hot=0.5, hot_width=2.0, gain=1.35, vein=0.05), density=20)
    B.lathe([(0.0, 0.34), (0.3, 0.27), (0.48, 0.1)], 8, Mcore, sharp=True, extra=dict(facets=8))
    S(mat="body", density=None)

    # torn plate petals around the entry wound
    back_entry = SPIRE_ENTRY
    info["back_entry"] = back_entry
    S(bone="chest", cls="bronze", params=dict(tone=-0.5, edge=1.0))
    rng = np.random.RandomState(4)
    for centre, nrm, r0 in ((back_entry, -SPIRE_AXIS, 0.46),):
        ref = unit(np.cross(nrm, (1, 0, 0)))
        for k in range(7):
            a = math.radians(k * 360 / 7 + rng.uniform(-12, 12))
            radial = math.cos(a) * ref + math.sin(a) * np.cross(nrm, ref)
            tilt = math.radians(rng.uniform(25, 55))
            ln = rng.uniform(0.34, 0.6)
            yv = unit(nrm * math.cos(tilt) + radial * math.sin(tilt))
            xv = unit(np.cross(nrm, radial))
            Mp = np.eye(4)
            Mp[:3, 0], Mp[:3, 1], Mp[:3, 2], Mp[:3, 3] = xv, yv, np.cross(xv, yv), centre + radial * r0 - nrm * 0.12
            B.plate([(-0.17, 0.0), (0.17, 0.0), (0.0, ln)], 0.05, Mp, bevel=0.0)

    # ================================ head =====================================================
    Mh = xf(H, rot_x(8)) @ np.diag([1.2, 1.2, 1.2, 1.0])
    S(bone="head", cls="bronze", params=dict(inlay=3))
    B.shell(ell_patch((0, -0.38, 0.34), (0.52, 0.62, 0.42), (0, 360), (8, 88), 10, 3, Mh)[:, :-1], 0.0, wrap=True)
    S(cls="dark", params=dict())
    B.box((0, -0.38, 0.12), (0.84, 1.0, 0.56), M=Mh, chamfer=0.05, skip="-z")
    S(cls="verdigris", params=dict(lines=[(0.5, 0.6, 0.5, 1.0)], tone=0.3))
    B.box((0, -0.86, 0.0), (0.82, 0.14, 0.52), M=Mh, taper=(1.06, 1.0), chamfer=0.035, skip="+y")
    # the eye slit: a dark recess across the mask; the eyes sit in it
    S(cls="dark", params=dict(tone=-3.0))
    B.box((0, -0.925, 0.06), (0.78, 0.04, 0.2), M=Mh, chamfer=0.0, skip="+y")
    for sx in (-1, 1):
        B.box((sx * 0.425, -0.74, 0.07), (0.03, 0.4, 0.2), M=Mh, chamfer=0.0, skip="-x" if sx > 0 else "+x")
    # brow: a heavy ridge shading the slit
    S(cls="bronze", params=dict(trim=1.2, tone=0.2))
    B.box((0, -0.86, 0.3), (1.0, 0.34, 0.17), M=Mh @ xf((0, 0, 0), rot_x(14)), taper=(0.9, 0.8), chamfer=0.04)
    # jaw
    S(cls="bronze", params=dict(lines=[(0.25, 0.2, 0.25, 0.9), (0.5, 0.2, 0.5, 0.9), (0.75, 0.2, 0.75, 0.9)], tone=-0.2))
    B.box((0, -0.8, -0.36), (0.62, 0.36, 0.3), M=Mh, taper=(1.25, 1.0), chamfer=0.04, skip="+z")
    # cheek guards and a low crest
    S(cls="verdigris", params=dict(trim=1.2))
    for sx in (-1, 1):
        B.plate([(-0.3, -0.2), (0.28, -0.14), (0.32, 0.2), (-0.3, 0.24)], 0.08,
                Mh @ xf((sx * 0.44, -0.5, -0.28), rot_z(sx * 90 - sx * 10) @ rot_x(90)), bevel=0.04)
    S(cls="bronze", params=dict(trim=1.2, tone=0.4))
    B.hexa([[[(-0.06, -1.02, 0.4), (-0.035, -1.04, 0.58)], [(-0.06, 0.1, 0.5), (-0.035, 0.36, 1.0)]],
            [[(0.06, -1.02, 0.4), (0.035, -1.04, 0.58)], [(0.06, 0.1, 0.5), (0.035, 0.36, 1.0)]]], 0.02, M=Mh)
    S(cls="bronze", params=dict(trim=1.1, tone=0.3))
    for sx in (-1, 1):
        B.hexa([[[(sx * 0.46, -0.42, 0.2), (sx * 0.56, 0.0, 0.52)], [(sx * 0.48, 0.16, 0.22), (sx * 0.68, 0.66, 0.9)]],
                [[(sx * 0.53, -0.42, 0.2), (sx * 0.6, 0.0, 0.52)], [(sx * 0.55, 0.16, 0.22), (sx * 0.72, 0.66, 0.9)]]], 0.015, M=Mh)
    # eyes: thin glowing slivers, slanted in a scowl; each on its own bone so it can be scaled from 0
    S(mat="eyes", cls="crystal", params=dict())
    for side, sx in (("L", 1), ("R", -1)):
        e = apply(Mh, (sx * 0.2, -0.975, 0.06))
        SKEL["eye_" + side] = ("head", tuple(e))
        S(bone="eye_" + side)
        B.box((0, 0, 0), (0.3, 0.06, 0.1), M=xf(e, rot_x(8) @ rot_y(sx * 16)), chamfer=0.0, skip="+y")
        B.box((0, 0, 0), (0.05, 0.3, 0.085), M=xf(apply(Mh, (sx * 0.452, -0.74, 0.085)), rot_x(8)), chamfer=0.0, skip="-x" if sx > 0 else "+x")
    info["head"] = apply(Mh, (0, -0.4, 0.25))
    S(mat="body")

    # ================================ arms (left, mirrored) ====================================
    B.mirror_begin()
    sh, el, wr = J("upper_arm_L"), J("forearm_L"), J("hand_L")
    # --- pauldron: layered dome lames with a crest fin
    pc = J("pauldron_L") + vec(0.15, 0.05, 0.08)
    S(bone="pauldron_L", cls="bronze", params=dict(inlay=4, rivets=16, tone=0.2))
    B.shell(ell_patch(pc, (1.12, 1.22, 1.05), (-120, 120), (44, 89), 12, 3), 0.1)
    S(cls="verdigris", params=dict(trim=1.4))
    B.shell(ell_patch(pc, (1.2, 1.3, 1.07), (-104, 104), (14, 50), 12, 2, flare=0.05), 0.09)
    S(cls="bronze", params=dict(rivets=14, inlay=4, tone=-0.1))
    B.shell(ell_patch(pc, (1.3, 1.4, 1.1), (-92, 92), (-22, 20), 12, 2, flare=0.05), 0.09)
    S(cls="bronze", params=dict(trim=1.3, tone=0.5))
    B.hexa([[[pc + vec(0.0, -0.9, 0.58), pc + vec(0.22, -0.7, 0.98)], [pc + vec(0.0, 0.76, 0.64), pc + vec(0.46, 1.0, 1.5)]],
            [[pc + vec(0.16, -0.9, 0.52), pc + vec(0.34, -0.7, 0.95)], [pc + vec(0.16, 0.76, 0.58), pc + vec(0.56, 1.0, 1.47)]]], 0.025)
    S(cls="gold", params=dict(tone=-0.4))
    for az in (-72, -36, 0, 36, 72):
        ca, sa = math.cos(math.radians(az)), math.sin(math.radians(az))
        p = pc + vec(1.33 * ca, 1.43 * sa, 0.0)
        n = unit(vec(ca / 1.33, sa / 1.43, 0.0))
        cone(B, p - n * 0.03, p + n * 0.3, 0.14, 4)
    # --- upper arm
    Mu = aim(el, sh - el)
    S(bone="upper_arm_L", cls="dark", params=dict(ribs=3))
    cyl(B, apply(Mu, (0, 0, 0.1)), apply(Mu, (0, 0, 1.5)), 0.33, 8, caps=False)
    S(cls="verdigris", params=dict(trim=1.3))
    B.shell(cyl_patch((0.48, 0.5), (-80, 80), (0.42, 1.22), 5, 2, Mu, flare=(0.95, 1.08)), 0.07)
    S(cls="gold", params=dict(tone=-1.0))
    B.tube([apply(Mu, (0.05, -0.42, 1.45)), apply(Mu, (0.05, -0.44, 0.2))], 0.07, 5)
    B.tube([apply(Mu, (0.05, 0.42, 1.45)), apply(Mu, (0.05, 0.44, 0.2))], 0.07, 5)
    S(cls="cable", params=dict(ribs=3, skew=0.5))
    B.tube(arc_path(sh + vec(-0.8, -0.5, -0.55), apply(Mu, (-0.28, -0.1, 0.9)), (0.05, -0.2, -0.35), 5), 0.09, 5,
           weights=blend("chest", "upper_arm_L"))
    B.tube(arc_path(sh + vec(-0.8, 0.5, -0.5), apply(Mu, (-0.28, 0.15, 0.9)), (0.05, 0.2, -0.35), 5), 0.09, 5,
           weights=blend("chest", "upper_arm_L"))
    # elbow hinge
    S(cls="dark", params=dict(ribs=3), bone="upper_arm_L")
    cyl(B, el + vec(-0.5, 0, 0), el + vec(0.5, 0, 0), 0.4, 8)
    # --- forearm: a massive layered gauntlet
    Mf = aim(wr, el - wr)
    S(bone="forearm_L", cls="bronze", params=dict(tone=-0.9))
    B.box((0, 0, 0.95), (1.1, 1.2, 1.8), M=Mf, taper=(0.72, 0.7), chamfer=0.08)
    S(cls="bronze", params=dict(inlay=4, rivets=14, tone=0.1))
    B.shell(cyl_patch((0.68, 0.74), (-155, 65), (1.24, 1.92), 8, 2, Mf, flare=(0.86, 1.0)), 0.09)
    S(cls="verdigris", params=dict(trim=1.4))
    B.shell(cyl_patch((0.75, 0.81), (-150, 60), (0.64, 1.36), 8, 2, Mf, flare=(1.0, 1.08)), 0.09)
    S(cls="bronze", params=dict(inlay=4, rivets=12,
                                motif=[(0.3, 0.3, 0.42, 0.62), (0.42, 0.62, 0.54, 0.3), (0.54, 0.3, 0.66, 0.62), (0.66, 0.62, 0.78, 0.3)]))
    B.shell(cyl_patch((0.82, 0.88), (-146, 56), (0.06, 0.76), 8, 2, Mf, flare=(1.04, 1.16)), 0.1)
    # elbow blade: a fin along the back of the forearm running to a spike past the elbow
    S(cls="bronze", params=dict(trim=1.3, tone=0.5))
    B.hexa([[[(0.34, 0.7, 0.7), (0.38, 0.68, 1.95)], [(0.34, 1.04, 1.0), (0.46, 1.4, 2.85)]],
            [[(0.5, 0.7, 0.7), (0.52, 0.68, 1.95)], [(0.5, 1.04, 1.0), (0.56, 1.4, 2.85)]]], 0.025, M=Mf)
    S(cls="gold", params=dict(tone=-0.5))
    B.lathe([(0.64, 0.1), (0.75, 0.1), (0.75, -0.08), (0.64, -0.08)], 10, Mf @ np.diag([1.0, 1.08, 1.0, 1.0]))
    S(cls="cable", params=dict(ribs=3, skew=0.5))
    B.tube(arc_path(apply(Mu, (-0.3, 0.0, 0.5)), apply(Mf, (-0.56, 0.0, 1.2)), (-0.12, 0.25, 0.0), 5), 0.09, 5,
           weights=blend("upper_arm_L", "forearm_L"))
    B.tube(arc_path(apply(Mu, (-0.27, -0.2, 0.45)), apply(Mf, (-0.5, -0.4, 1.3)), (-0.1, -0.2, 0.0), 5), 0.08, 5,
           weights=blend("upper_arm_L", "forearm_L"))
    # --- hand: palm, back plate, knuckle guard, two rows of fingers, thumb
    S(bone="hand_L", cls="bronze", params=dict(tone=-0.8))
    B.box((0, 0, -0.46), (0.58, 1.16, 0.92), M=Mf, chamfer=0.06)
    S(cls="verdigris", params=dict(inlay=3, motif=[(0.5, 0.25, 0.5, 0.75), (0.25, 0.5, 0.75, 0.5)]))
    B.plate([(-0.6, -0.46), (0.6, -0.46), (0.5, 0.44), (-0.5, 0.44)], 0.15, Mf @ xf((0.29, 0, -0.5), rot_y(90) @ rot_z(90)), bevel=0.06)
    S(cls="bronze", params=dict(tone=0.2))
    B.box((0.16, 0, -0.97), (0.6, 1.28, 0.27), M=Mf, chamfer=0.05)
    S(cls="gold", params=dict(tone=-0.4))
    for k in range(4):
        y = -0.45 + 0.3 * k
        cone(B, apply(Mf, (0.46, y, -0.97)), apply(Mf, (0.7, y, -1.0)), 0.125, 4)
    k1 = apply(Mf, (0.05, 0, -0.97))
    k2 = apply(Mf, (0.05, 0, -1.55))
    SKEL["fingers1_L"] = ("hand_L", tuple(k1)); SKEL["fingers2_L"] = ("fingers1_L", tuple(k2))
    th = apply(Mf, (-0.14, -0.58, -0.35))
    SKEL["thumb_L"] = ("hand_L", tuple(th))
    info["hand_frame_L"] = Mf.copy()
    for k in range(4):
        y = -0.45 + 0.3 * k
        ln = (0.52, 0.58, 0.56, 0.48)[k]
        S(bone="fingers1_L", cls="bronze", params=dict(tone=0.1))
        B.box((0.05, y, -0.97 - ln / 2), (0.34, 0.27, ln), M=Mf, chamfer=0.035)
        S(bone="fingers2_L", cls="bronze", params=dict(tone=-0.2))
        B.box((0.05, y, -1.0 - ln - 0.22), (0.31, 0.25, 0.46), M=Mf, taper=(1.25, 1.1), chamfer=0.035)
    S(bone="thumb_L", cls="bronze", params=dict(tone=0.0))
    B.box((-0.16, -0.65, -0.6), (0.32, 0.3, 0.58), M=Mf, chamfer=0.035)
    B.box((-0.18, -0.67, -1.06), (0.28, 0.26, 0.42), M=Mf, taper=(1.2, 1.1), chamfer=0.035)
    B.mirror_end(share=True)
    mirror_skel()

    # ================================ the spire ================================================
    S(mat="crystal", cls="crystal", density=20)
    ax = SPIRE_AXIS
    side = unit(np.cross(ax, (1, 0, 0)))            # perpendicular to the spire, roughly up-forward
    lat = unit(np.cross(side, ax))                  # roughly x
    mid = (back_entry + core) / 2
    Lb = np.linalg.norm(SPIRE_BACK_TIP - mid)
    Lf = np.linalg.norm(SPIRE_FRONT_TIP - mid)
    shards = [
        # base point, direction, length, radius, sides, seed, hot (0 tip .. 1 butt)
        (mid + ax * 0.9, -ax, Lb + 0.9, 0.5, 6, 1, 0.95),                                # 0: the great back shard
        (mid - ax * 0.8, ax, Lf + 0.8, 0.42, 6, 2, 0.95),                                # 1: the front shard
        (mid + ax * 0.3 + lat * 0.36 + side * 0.14, unit(-ax + lat * 0.11 + side * 0.07), Lb - 0.6, 0.3, 5, 3, 0.9),
        (mid + ax * 0.3 - lat * 0.34 + side * 0.24, unit(-ax - lat * 0.14 + side * 0.12), Lb - 1.2, 0.26, 5, 4, 0.9),
        (mid + ax * 0.3 - lat * 0.06 - side * 0.4, unit(-ax - lat * 0.03 - side * 0.15), Lb - 0.95, 0.28, 5, 5, 0.9),
        (mid - ax * 0.3 + lat * 0.3 - side * 0.12, unit(ax + lat * 0.17 - side * 0.08), Lf - 0.55, 0.24, 5, 6, 0.9),
        (mid - ax * 0.3 - lat * 0.26 + side * 0.24, unit(ax - lat * 0.15 + side * 0.17), Lf - 0.9, 0.21, 5, 7, 0.9),
    ]
    rng = np.random.RandomState(12)
    for centre, out in ((back_entry, -ax), (core, ax)):
        for k in range(4):
            a = k * math.pi / 2 + rng.uniform(-0.5, 0.5)
            radial = math.cos(a) * lat + math.sin(a) * side
            d = unit(out * rng.uniform(0.5, 1.0) + radial)
            shards.append((centre - out * 0.3 + radial * 0.22, d, rng.uniform(0.85, 1.3), rng.uniform(0.11, 0.17), 5, 20 + len(shards), 0.9))
    assert len(shards) == N_SHARDS
    info["shards"] = []
    for i, (base, d, ln, rad, sides, seed, hot) in enumerate(shards):
        name = f"shard_{i}"
        SKEL[name] = ("chest", tuple(base + d * ln * 0.45))
        S(bone=name, params=dict(seed=seed, hot=hot, hot_width=1.1, gain=1.15 if i < 7 else 1.4, vein=0.03))
        B.shard(base, d, ln, rad, sides, tip=0.3 if i < 2 else 0.4, butt=0.12, seed=seed)
        info["shards"].append(dict(base=base, dir=d, length=ln, centre=base + d * ln * 0.45))
    S(mat="body", density=None)

    info["wounds"] = [(core, 1.05), (back_entry, 1.2)]
    info["spire_tip"] = shards[0][0] + shards[0][1] * shards[0][2]
    info["spire_front_tip"] = shards[1][0] + shards[1][1] * shards[1][2]
    return B, info
