"""The six characters: build numbers and costumes hung on the solved rig."""
import numpy as np
from render import ball, seg, box, frame, euler, xf, xd, norm, rx, ry, rz
from rig import (BASE, M, G_HEAD, G_TORSO, G_ARML, G_ARMR, G_LEGL, G_LEGR, G_CAPE, G_WEAPON, G_SHIELD,
                 G_SKIRT, G_SCAB, G_FX, G_HAIR, G_TAIL)


def build_of(**kw):
    b = dict(BASE)
    b.update(kw)
    return b


CHARS = {
    "cael": dict(build=build_of(scale=1.04), weapon="sword", skin="skin", eye="#1c2c6c", costume="cael"),
    "rowan": dict(build=build_of(scale=1.05, shw=5.7), weapon="sword", skin="skin_pale", eye="#5c1020",
                  costume="rowan"),
    "lyra": dict(build=build_of(scale=1.025, shw=4.9, torso_r=(3.8, 4.1), arm_r=(2.0, 1.8, 1.65), hand_r=1.75,
                                leg_r=(2.5, 2.2, 1.95), head_r=7.8),
                 weapon="staff", skin="skin", eye="#14503c", costume="lyra"),
    "darius": dict(build=build_of(scale=1.065, shw=6.6, torso_r=(5.1, 5.8), arm_r=(2.8, 2.5, 2.2), hand_r=2.25,
                                  leg_r=(3.2, 2.8, 2.45), foot_r=1.95, head_r=7.7, neck=9.2, hipw=2.9),
                   weapon="axe", skin="skin_tan", eye="#2a2c3c", costume="darius"),
    "guard": dict(build=build_of(scale=1.06, shw=5.9, torso_r=(4.5, 5.1), head_r=7.4, head_y=6.5), weapon="sword", skin="skin",
                  eye="#000000", costume="guard"),
    "lancer": dict(build=build_of(scale=1.06, shw=5.6, torso_r=(4.2, 4.7), head_r=7.4, head_y=6.5), weapon="spear", skin="skin",
                   eye="#000000", costume="lancer"),
}


def chain(root, down, back, side, angles, lengths, radii, mat, grp, xhint, sx, sz, side_angle=0.0, floor=None):
    """A hanging strip of cloth: segments leaving `root`, each at its own
    angle (degrees from `down` towards `back`). Returns prims."""
    out = []
    p = np.asarray(root, float)
    for i, (ang, ln) in enumerate(zip(angles, lengths)):
        a = np.radians(ang)
        d = down * np.cos(a) + back * np.sin(a)
        sa = np.radians(side_angle)
        d = norm(d * np.cos(sa) + side * np.sin(sa))
        q = p + d * ln
        if floor is not None and q[1] < floor and p[1] >= floor:
            # The cloth meets the ground: the rest lies along it.
            flat = d.copy()
            flat[1] = 0
            if np.linalg.norm(flat) < 0.15:
                flat = np.asarray(back, float).copy()
                flat[1] = 0
            run = np.sqrt(max(ln * ln - (p[1] - floor) ** 2, 0.0))
            q = np.array([p[0], floor, p[2]]) + norm(flat) * run
        out.append(seg(p, q, radii[i], radii[i + 1], mat, grp, xhint=xhint, sx=sx, sz=sz))
        p = q
    return out


def stripes(base, dark, freq, phase=0.0, level=0.55, axis=0):
    def f(p):
        return np.where(np.sin(p[axis] * freq + phase) > level, dark, base)
    return f


def dress(c, s):
    """Primitives of character `c` on skeleton `s`, and marks for the face."""
    b, p = s.b, s.p
    out = []
    cr, pr, hr = s.chest[:3, :3], s.pel[:3, :3], s.head[:3, :3]
    cx = cr[:, 0]
    costume = c["costume"]
    skin = M[c["skin"]]
    R = b["head_r"]
    hc = s.headc
    head_radii = np.array([R, R * 0.95, R * 0.97])
    out.append(ball(hc, head_radii, skin, G_HEAD, rot=hr))
    # neck
    out.append(seg(xf(s.chest, (0, b["neck"] - 1.5, 0.2)), xf(s.head, (0, 1.5, 0.3)), 2.0, 2.0, skin, G_HEAD))

    def H(x, y, z):  # head-local point, relative to the head's centre
        return hc + hr @ np.array([x, y, z], float)

    def C(x, y, z):
        return xf(s.chest, (x, y, z))

    def P(x, y, z):
        return xf(s.pel, (x, y, z))

    # Cloth basis: straight down, and "back" as the chest sees it.
    if p["cape_body"]:
        down, back = -cr[:, 1], -cr[:, 2]
    else:
        down = np.array([0, -1.0, 0])
        back = -cr[:, 2].copy()
        back[1] = 0
        back = norm(back) if np.linalg.norm(back) > 0.2 else np.array([0, 0, -1.0])
    side = np.cross(down, back)  # towards the character's left

    mats = {}
    if costume == "cael":
        mats = dict(torso=lambda q: np.where((np.abs(q[0]) < 0.75) & (q[2] > 0), M["gold"], M["navy"]),
                    pelvis=M["navy"], upper=M["navy"],
                    fore=lambda q: np.where(q[1] > b["fore"] - 1.7, M["gold"], M["navy"]),
                    hand=M["leather"], thigh=M["cream"], shin=M["leather"], foot=M["leather"],
                    shoulder=M["navy"])
    elif costume == "rowan":
        mats = dict(torso=lambda q: np.where((q[2] > 1.2) & (q[1] > 3.0), M["charcoal_l"], M["charcoal"]),
                    pelvis=M["charcoal"], upper=M["charcoal"], fore=M["charcoal_l"], hand=M["black"],
                    thigh=M["charcoal"], shin=M["charcoal_l"], foot=M["charcoal"], shoulder=M["charcoal_l"])
    elif costume == "lyra":
        mats = dict(torso=lambda q: np.where((np.abs(q[0]) < 0.7) & (q[2] > 0), M["teal"], M["cream"]),
                    pelvis=M["cream"], upper=M["cream"],
                    fore=lambda q: np.where(q[1] > b["fore"] - 1.8, M["teal"], M["cream"]),
                    hand=skin, thigh=M["cream_d"], shin=M["leather"], foot=M["leather"], shoulder=M["cream"])
    elif costume == "darius":
        mats = dict(torso=lambda q: np.where(np.abs(q[0]) < 2.9, M["ochre"], M["steel"]),
                    pelvis=M["leather"], upper=M["steel_d"], fore=M["steel"], hand=M["leather"],
                    thigh=M["steel_d"], shin=M["steel"], foot=M["steel_d"], shoulder=M["steel"])
    elif costume == "guard":
        mats = dict(torso=M["rust"], pelvis=M["black"], upper=M["black"], fore=M["rust"], hand=M["black"],
                    thigh=M["black"], shin=M["rust"], foot=M["black"], shoulder=M["rust"])
    elif costume == "lancer":
        mats = dict(torso=lambda q: np.where((q[2] > 0.5) & (q[1] > 2.5), M["rust"], M["black"]),
                    pelvis=M["black"], upper=M["black"], fore=M["rust_d"], hand=M["black"],
                    thigh=M["black"], shin=M["rust_d"], foot=M["black"], shoulder=M["rust"])

    # ---- body
    tr = b["torso_r"]
    out.append(seg(P(0, 0.2, 0), C(0, 5.0, 0), tr[0], tr[1], mats["torso"], G_TORSO, xhint=cx,
                   sx=b["torso_sx"], sz=b["torso_sz"]))
    out.append(ball(P(0, -0.2, 0), (tr[0] * 1.2, 3.3, tr[0] * 0.92), mats["pelvis"], G_TORSO, rot=pr))
    ar, lr = b["arm_r"], b["leg_r"]
    for side_name, sg, ga, gl in (("L", 1.0, G_ARML, G_LEGL), ("R", -1.0, G_ARMR, G_LEGR)):
        sh, el, hand = s.sh[side_name], s.el[side_name], s.hand[side_name]
        out.append(seg(sh, el, ar[0], ar[1], mats["upper"], ga, xhint=cx))
        out.append(seg(el, hand, ar[1], ar[2], mats["fore"], ga, xhint=cx))
        hr_ = b["hand_r"] * (np.array([1.25, 1.3, 0.75]) if p["palm" + side_name] else np.ones(3))
        out.append(ball(hand + norm(hand - el) * 0.6, hr_, mats["hand"], ga))
        hipj, knee, ankle, f = s.hipj[side_name], s.knee[side_name], s.ankle[side_name], s.footd[side_name]
        out.append(seg(hipj, knee, lr[0], lr[1], mats["thigh"], gl))
        out.append(seg(knee, ankle, lr[1], lr[2], mats["shin"], gl))
        shin = norm(ankle - knee)
        heel = ankle - f * 0.9 + shin * 0.15
        out.append(seg(heel, heel + f * b["foot_len"], b["foot_r"], b["foot_r"] * 0.86, mats["foot"], gl))

    def skirt_dir(o, kmid, length):
        d = norm(-pr[:, 1] * 0.62 + norm(kmid - o) * 0.38)
        if not p["cape_body"] and o[1] + d[1] * length < 1.0:
            # Kneeling or sitting: the hem stays on the ground.
            y = -(o[1] - 1.0) / length
            h = d.copy()
            h[1] = 0
            if np.linalg.norm(h) < 0.05:
                h = pr[:, 2].copy()
                h[1] = 0
            d = norm(h) * np.sqrt(max(1 - y * y, 0)) + np.array([0, y, 0])
        return d

    # Skirt of a coat follows the thighs a little.
    def skirt(length, r0, r1, mat, sx=1.18, sz=0.92, top=0.8, cut=1e9):
        kmid = (s.knee["L"] + s.knee["R"]) / 2
        o = P(0, top, 0)
        d = skirt_dir(o, kmid, length)
        spread = abs((pr.T @ (s.knee["L"] - s.knee["R"]))[2])
        out.append(seg(o, o + d * length, r0, r1, mat, G_SKIRT, xhint=pr[:, 0], sx=sx,
                       sz=sz + min(spread, 6) * 0.035, cut=cut))

    cape, hair_sw, sash = p["cape"], p["hair"], p["sash"]
    fl = None if p["cape_body"] else 0.9
    q8 = R / 8.6

    def tufts(mat, items, grp=G_HAIR):
        for a, bb, r0, r1 in items:
            out.append(seg(H(*(np.array(a) * q8)), H(*(np.array(bb) * q8)), r0 * q8, r1 * q8, mat, grp))

    def under(length, r1, sx, sz, top, mat):
        kmid = (s.knee["L"] + s.knee["R"]) / 2
        o = P(0, top, 0)
        d = skirt_dir(o, kmid, length)
        out.append(ball(o + d * (length - 0.4), (r1 * sx * 0.9, 0.6, r1 * sz * 0.9), mat, G_SKIRT, rot=pr))

    if costume == "cael":
        trim = lambda q: np.where(q[1] > 4.9 - 1.0, M["gold"],
                                  np.where((np.abs(q[0]) < 0.75) & (q[2] > 0), M["gold"], M["navy"]))
        skirt(4.9, 4.6, 5.9, trim, cut=0.3)
        under(4.9, 5.9, 1.18, 0.92, 0.8, M["navy"])
        out.append(ball(P(0, 1.9, 0), (5.5, 1.1, 4.2), M["leather"], G_TORSO, rot=pr))
        out.append(ball(P(0, 1.9, 3.9), (1.1, 1.1, 0.8), M["gold"], G_TORSO, rot=pr))
        # mantle and cape
        out.append(ball(C(0, 7.1, -1.0), (7.3, 2.2, 4.4), M["royal"], G_CAPE, rot=cr))
        out.append(ball(C(0, 6.6, 3.4), (1.25, 1.25, 1.0), M["gold"], G_TORSO, rot=cr))
        fold = stripes(M["royal"], M["royal_d"], 1.55, 0.6, 0.5)
        out += chain(C(0, 7.0, -4.0), down, back, side, (cape[0], cape[0] + cape[2]), (5.8, 5.4),
                     (5.0, 5.8, 6.4), fold, G_CAPE, cx, 1.32, 0.3, cape[1], floor=fl)
        hm = M["hair_sand"]
        out.append(ball(H(0, 1.6 * q8, -2.6 * q8), (R + 0.8, R + 0.5, R + 1.5), hm, G_HAIR, rot=hr))
        tufts(hm, (((2.0, 6.6, 5.6), (-1.6, 3.4, 8.8), 2.6, 1.0),
                   ((-2.5, 6.6, 5.4), (-5.6, 3.2, 7.0), 2.4, 1.0),
                   ((4.6, 6.2, 5.0), (6.4, 3.0, 6.2), 2.3, 1.0),
                   ((-7.0, 4.6, 2.6), (-8.3, -0.8, 2.2), 2.4, 0.9),
                   ((7.0, 4.6, 2.6), (8.3, -0.8, 2.2), 2.4, 0.9),
                   ((-2.5, 8.4, -1.0), (-5.5, 12.0, -2.5), 2.8, 0.5),
                   ((2.5, 8.6, -2.0), (5.0, 11.6, -4.5), 2.8, 0.5),
                   ((0.0, 7.5, -5.5), (-0.5, 9.5, -10.4), 3.0, 0.6),
                   ((6.5, 3.0, -5.0), (10.2, 2.4, -8.0), 2.6, 0.6),
                   ((-6.5, 3.0, -5.0), (-10.2, 2.9, -8.0), 2.6, 0.6),
                   ((0.0, 1.0, -8.0), (0.0, -3.8, -10.6), 3.0, 0.8)))
    elif costume == "rowan":
        out.append(ball(s.sh["R"] + cr @ np.array([-0.7, 0.9, 0]), (3.3, 2.9, 3.3), M["charcoal_l"], G_ARMR, rot=cr))
        out.append(ball(P(0, 2.2, 0), (5.6, 1.6, 4.3), M["crimson"], G_TORSO, rot=pr))
        out.append(ball(P(4.7, 2.0, 1.6), (1.5, 1.5, 1.5), M["crimson"], G_TORSO, rot=pr))
        pdown = norm(down * 0.8 - pr[:, 1] * 0.2)
        out += chain(P(5.0, 1.6, 1.4), pdown, back, side, (sash[0], sash[0] + 6), (5.0, 5.0), (1.5, 1.7, 1.0),
                     M["crimson"], G_TAIL, pr[:, 2], 1.0, 0.45, 8 + sash[1], floor=fl)
        out += chain(P(5.4, 1.6, 0.4), pdown, back, side, (sash[0] + 10, sash[0] + 20), (4.0, 4.0),
                     (1.4, 1.5, 0.9), M["crimson_d"], G_TAIL, pr[:, 2], 1.0, 0.45, 2 + sash[1], floor=fl)
        # half-cloak over the left shoulder
        out.append(ball(C(3.8, 7.2, -0.6), (4.9, 2.6, 4.5), M["crimson"], G_CAPE, rot=cr))
        fold = stripes(M["crimson"], M["crimson_d"], 1.7, 0.2, 0.5)
        out += chain(C(3.2, 7.0, -3.9), down, back, side, (cape[0], cape[0] + cape[2], cape[0] + cape[2] * 1.5),
                     (6.0, 5.8, 4.8), (4.2, 4.9, 5.4, 5.6), fold, G_CAPE, cx, 1.22, 0.3, cape[1] + 3,
                     floor=None if fl is None else 1.8)
        hm = M["hair_black"]
        out.append(ball(H(0, 1.6 * q8, -2.6 * q8), (R + 0.9, R + 0.5, R + 1.6), hm, G_HAIR, rot=hr))
        tufts(hm, (((3.2, 6.6, 5.4), (0.2, 1.6, 8.9), 2.7, 0.9),
                   ((-1.8, 6.6, 5.6), (-5.4, 1.4, 7.2), 2.6, 0.9),
                   ((5.4, 6.0, 4.6), (6.9, 1.6, 5.8), 2.4, 0.9),
                   ((-7.0, 4.4, 2.6), (-8.5, -4.4, 2.0), 2.5, 0.8),
                   ((7.0, 4.4, 2.6), (8.5, -5.4, 2.0), 2.5, 0.8),
                   ((4.0, -2.0, -6.5), (5.8, -9.0, -8.0), 2.8, 0.6),
                   ((-4.0, -2.0, -6.5), (-5.8, -8.4, -8.0), 2.8, 0.6),
                   ((0.0, -2.0, -7.5), (0.0, -10.0, -9.2), 3.0, 0.6),
                   ((-1.5, 8.6, -1.0), (-3.0, 11.6, -4.0), 2.6, 0.5),
                   ((3.0, 8.0, -3.0), (6.5, 10.4, -6.0), 2.6, 0.5)))
    elif costume == "lyra":
        ln = 9.6
        fold = lambda q: np.where(q[1] > ln - 1.3, M["teal"],
                                  np.where((np.abs(q[0]) < 0.6) & (q[2] > 0), M["teal"],
                                           np.where(np.sin(np.arctan2(q[0], q[2]) * 5 + 0.5) > 0.72, M["cream_d"], M["cream"])))
        skirt(ln, 4.2, 6.6, fold, sx=1.12, sz=0.95, top=1.2, cut=0.3)
        under(ln, 6.6, 1.12, 0.95, 1.2, M["cream_d"])
        out.append(ball(P(0, 2.0, 0), (5.0, 1.1, 3.9), M["teal"], G_TORSO, rot=pr))
        out.append(ball(C(0, 6.9, -0.2), (6.3, 1.9, 4.1), M["teal"], G_TORSO, rot=cr))
        hm = M["hair_auburn"]
        out.append(ball(H(0, 1.6 * q8, -2.6 * q8), (R + 0.8, R + 0.5, R + 1.5), hm, G_HAIR, rot=hr))
        tufts(hm, (((-1.6, 6.6, 5.6), (1.8, 3.4, 8.8), 2.6, 1.0),
                   ((2.6, 6.6, 5.4), (5.8, 3.0, 6.8), 2.4, 1.0),
                   ((-4.6, 6.2, 5.0), (-6.4, 3.0, 6.2), 2.3, 1.0),
                   ((7.2, 4.4, 2.6), (8.3, -7.6, 3.0), 2.3, 0.8),
                   ((-7.2, 4.4, 2.6), (-8.3, -7.6, 3.0), 2.3, 0.8)))
        # ponytail
        hb = -hr[:, 2].copy()
        hb[1] = 0
        hb = norm(hb) if np.linalg.norm(hb) > 0.2 else back
        hd = np.array([0, -1.0, 0])
        if p["cape_body"]:
            hd, hb = down, back
        root = H(0, 4.6 * q8, -8.8 * q8)
        out.append(ball(root, 1.9, M["teal"], G_HAIR))
        out += chain(root + hb * 1.0, hd, hb, np.cross(hd, hb), (42 + hair_sw[0], 10 + hair_sw[0], -6 + hair_sw[0] * 1.3),
                     (4.6, 4.8, 4.4), (2.5, 2.9, 2.2, 0.7), hm, G_TAIL, hr[:, 0], 1.0, 0.8, hair_sw[1])
    elif costume == "darius":
        for sn, sg, g in (("L", 1.0, G_ARML), ("R", -1.0, G_ARMR)):
            out.append(ball(s.sh[sn] + cr @ np.array([sg * 0.8, 1.0, 0]), (3.8, 3.1, 3.8), M["steel"], g, rot=cr))
        out.append(ball(P(0, 2.2, 0), (6.5, 1.3, 5.1), M["leather"], G_TORSO, rot=pr))
        out.append(ball(P(0, 2.2, 4.9), (1.2, 1.2, 0.8), M["gold"], G_TORSO, rot=pr))
        pdown = norm(down * 0.7 - pr[:, 1] * 0.3)
        hem = lambda q: np.where(q[1] > 3.4, M["ochre_d"], M["ochre"])
        tab = chain(P(0, 1.0, 4.5), pdown, back, side, (-sash[0] * 0.6 - 4, -sash[0] * 0.6), (4.0, 4.0), (3.0, 3.2, 3.3),
                    M["ochre"], G_SKIRT, pr[:, 0], 1.0, 0.3, sash[1], floor=fl)
        tab[-1].mat = hem
        if pr[1, 2] > -0.5:  # lying face down it is under him
            out += tab
        tab = chain(P(0, 1.0, -4.3), pdown, back, side, (sash[0] + 4, sash[0] + 8), (4.0, 4.0), (3.0, 3.2, 3.3),
                    M["ochre"], G_SKIRT, pr[:, 0], 1.0, 0.3, sash[1], floor=fl)
        tab[-1].mat = hem
        out += tab
        hm = lambda q: np.where((np.abs(np.abs(np.arctan2(q[0], q[2])) - 1.25) < 0.3) & (q[1] < 0.75), M["hair_grey"], M["hair_iron"])
        out.append(ball(H(0, 2.0 * q8, -2.6 * q8), (R + 0.6, R + 0.3, R + 1.3), hm, G_HAIR, rot=hr))
        tufts(M["hair_iron"], (((-2.0, 7.0, 5.2), (0.5, 4.8, 7.8), 2.0, 1.0),
                               ((3.0, 6.8, 5.0), (5.6, 4.2, 6.2), 2.0, 1.0),
                               ((-4.6, 6.6, 4.8), (-6.0, 4.2, 5.6), 2.0, 1.0)))
        tufts(M["hair_iron"], (((0.0, 6.0, -6.5), (0.0, 1.0, -10.6), 3.4, 1.6),
                               ((4.5, 3.0, -6.5), (5.5, -3.5, -9.0), 3.2, 1.2),
                               ((-4.5, 3.0, -6.5), (-5.5, -3.5, -9.0), 3.2, 1.2)))
        tufts(M["hair_grey"], (((0.0, -1.0, -8.6), (0.0, -6.0, -8.6), 2.6, 1.0),))
        bm = lambda q: np.where(np.abs(q[0]) < 0.36, M["hair_grey"], M["hair_iron"])
        out.append(ball(H(0, -6.0 * q8, 4.3 * q8), (4.9 * q8, 3.7 * q8, 4.3 * q8), bm, G_HAIR, rot=hr))
        tufts(M["hair_iron"], (((7.6, 3.0, 2.0), (5.6, -5.4, 4.0), 1.7, 2.2),
                               ((-7.6, 3.0, 2.0), (-5.6, -5.4, 4.0), 1.7, 2.2),
                               ((-2.8, -3.9, 7.6), (2.8, -3.9, 7.6), 1.1, 1.1)))
    elif costume in ("guard", "lancer"):
        helm = M["rust"]
        tee = costume == "guard"

        def hmat(q):
            ax = np.abs(q[0])
            front = q[2] > 0.15
            if tee:
                hole = ((q[2] > -0.2) & (q[1] > -0.42) & (q[1] < -0.06)) | (front & (ax < 0.17) & (q[1] <= -0.42) & (q[1] > -0.92))
                brow = (q[1] >= -0.06) & (q[1] < 0.1)
                dark = (q[1] < -0.3) & (q[2] <= -0.2)
            else:
                hole = (q[2] > -0.1) & (q[1] > -0.4) & (q[1] < -0.1)
                brow = ((q[2] > -0.1) & (q[1] <= -0.4)) | ((q[1] < -0.3) & (q[2] <= -0.1))
                dark = (ax < 0.13) & (q[1] > -0.1)
            return np.where(hole, M["visor"], np.where(brow, M["black"], np.where(dark, M["rust_d"], helm)))

        out.append(ball(hc + hr @ np.array([0, 0.2, 0.0]), (R + 0.3, R + 0.6, R + 0.5), hmat, G_HAIR, rot=hr))
        out.append(ball(H(0, -3.6, -3.6), (R - 0.2, 3.4, R - 1.6), M["black"] if not tee else M["rust_d"], G_HAIR, rot=hr @ rx(-18)))
        out.append(ball(H(0, -6.9, 0.3), (5.4, 2.2, 5.2), M["black"], G_HAIR, rot=hr))
        for sn, sg, g in (("L", 1.0, G_ARML), ("R", -1.0, G_ARMR)):
            out.append(ball(s.sh[sn] + cr @ np.array([sg * 0.6, 0.9, 0]), (3.4, 2.8, 3.4), M["rust"], g, rot=cr))
        out.append(ball(P(0, 2.0, 0), (5.6, 1.2, 4.5), M["black"], G_TORSO, rot=pr))
        hb = -hr[:, 2].copy()
        hb[1] = 0
        hb = norm(hb) if np.linalg.norm(hb) > 0.2 else back
        hd, hbk = (down, back) if p["cape_body"] else (np.array([0, -1.0, 0]), hb)
        if costume == "guard":
            out.append(ball(H(0, R + 0.8, -1.0), (1.2, 2.3, 6.0), M["black"], G_HAIR, rot=hr))
            skirt(4.2, 4.8, 6.0, lambda q: np.where(q[1] > 3.2, M["rust"], M["rust_d"]), cut=0.3)
            under(4.2, 6.0, 1.18, 0.92, 0.8, M["black"])
            out.append(ball(C(0, 7.0, -1.2), (6.9, 2.0, 4.0), M["black"], G_CAPE, rot=cr))
            out += chain(C(0, 6.8, -4.0), down, back, side, (cape[0], cape[0] + cape[2]), (5.0, 4.8),
                         (4.8, 5.4, 5.8), stripes(M["black"], M["rust_d"], 1.5, 0.4, 0.8), G_CAPE, cx, 1.25, 0.3,
                         cape[1], floor=fl)
        else:
            out.append(ball(H(0, R + 0.9, -0.5), (1.6, 1.4, 2.4), M["rust_d"], G_HAIR, rot=hr))
            out += chain(H(0, R + 1.2, -1.0), hd, hbk, np.cross(hd, hbk),
                         (104 + hair_sw[0] * 0.4, 58 + hair_sw[0] * 0.7, 20 + hair_sw[0], 2 + hair_sw[0] * 1.3),
                         (3.6, 4.4, 4.8, 4.6), (1.5, 2.3, 2.6, 2.0, 0.6), M["crimson_d"], G_TAIL, hr[:, 0], 1.0, 0.75,
                         hair_sw[1])
            pdown = norm(down * 0.7 - pr[:, 1] * 0.3)
            out += chain(P(0, 1.0, -3.9), pdown, back, side, (sash[0] + 6, sash[0] + 12), (4.6, 4.6),
                         (4.0, 4.6, 4.9), stripes(M["rust_d"], M["rust"], 1.6, 0, 0.6), G_SKIRT, pr[:, 0], 1.2, 0.3,
                         sash[1], floor=fl)

    # ---- weapon
    out += weapon_prims(c, s, P, C)
    if costume == "darius":
        out += shield_prims(s, P, C)
    if p["fx"]:
        out += [q.copy() for q in p["fx"]]

    k = b["scale"]
    sh = np.asarray(p["shift"], float)
    for q in out:
        q.m[:3, 3] += sh
        q.m[:3, :] *= k
    hc = hc + sh
    ex, ey, my = 3.1 * q8 / 0.92, -1.9 * q8 / 0.92, -5.3 * q8 / 0.92
    zs = np.sqrt(max(head_radii[2] ** 2 * (1 - (ex / head_radii[0]) ** 2 - (ey / head_radii[1]) ** 2), 1))
    marks = dict(
        scale=k, radius=R * k,
        head=hc * k, head_rot=hr,
        eyes=[(H(sg * ex, ey, zs) * k, hr @ norm((sg * ex / head_radii[0] ** 2, ey / head_radii[1] ** 2, zs / head_radii[2] ** 2)))
              for sg in (1, -1)],
        mouth=H(0, my, np.sqrt(max(head_radii[2] ** 2 * (1 - (my / head_radii[1]) ** 2), 1))) * k,
        knees={n: (s.knee[n] + sh) * k for n in ("L", "R")},
        joints={n: ((s.sh[n] + sh) * k, (s.el[n] + sh) * k, (s.hand[n] + sh) * k) for n in ("L", "R")},
        neck=(xf(s.chest, (0, b["neck"], 0)) + sh) * k,
    )
    return out, marks


def sword_prims(wf, style, hilt_only=False):
    """Weapon-local y runs from the grip up the blade; x is the edge."""
    r = wf[:3, :3]

    def W(x, y, z):
        return xf(wf, (x, y, z))

    hilt = {"cael": ("gold", "leather"), "rowan": ("charcoal_l", "crimson_d"), "guard": ("black", "rust_d")}[style]
    length = {"cael": 19.0, "rowan": 20.0, "guard": 17.0}[style]
    out = [ball(W(0, -2.9, 0), 1.05, M[hilt[0]], G_WEAPON, thin=True),
           seg(W(0, -2.2, 0), W(0, 1.6, 0), 0.8, 0.8, M[hilt[1]], G_WEAPON, thin=True),
           box(W(0, 2.2, 0), (2.9, 0.65, 0.85), M[hilt[0]], G_WEAPON, rot=r, thin=True)]
    if not hilt_only:
        bm = lambda q: np.where(q[0] > 0.1, M["blade"], M["blade_d"])
        out.append(box(W(0, 2.8 + length / 2, 0), (1.2, length / 2, 0.5), bm, G_WEAPON, rot=r, thin=True))
        tm = lambda q: np.where(q[0] > 0.1, M["blade"], M["blade_d"])
        out.append(seg(W(0, 2.8 + length - 0.2, 0), W(0, 2.8 + length + 2.4, 0), 1.2, 0.15, tm, G_WEAPON,
                       xhint=r[:, 0], sz=0.42, thin=True))
    return out


def weapon_prims(c, s, P, C):
    p, b = s.p, s.b
    kind, style = c["weapon"], c["costume"]
    out = []
    pr, cr = s.pel[:3, :3], s.chest[:3, :3]
    mode = p["weapon"]
    wf = s.wframe
    if mode == "ground":
        x, z, yaw, tilt = p["wpos"]
        rot = ry(yaw) @ rx(90) @ rz(tilt)
        wf = frame((x, 0.7, z), rot)
    if kind == "sword":
        # scabbard on the left hip
        top = P(b["torso_r"][0] * 1.2 + 0.1, -0.4, -1.4)
        d = norm(pr @ np.array([0.12, -0.9, -0.42]))
        if not p["cape_body"] and top[1] + d[1] * 12.5 < 1.2:
            # Kneeling: the scabbard's tip drags on the ground behind.
            y = -(top[1] - 1.2) / 12.5
            h = d.copy()
            h[1] = 0
            d = norm(h) * np.sqrt(max(1 - y * y, 0)) + np.array([0, y, 0])
        sm = lambda q: np.where(q[1] > 10.6, M["gold" if style == "cael" else "charcoal_l"],
                                M["leather_d"] if style != "guard" else M["black"])
        out.append(seg(top, top + d * 12.5, 1.3, 0.95, sm, G_SCAB, thin=True))
        if mode == "stow":
            x = norm(np.cross(d, pr[:, 0]))
            rot = np.stack([x, -d, np.cross(x, -d)], axis=1)
            out += sword_prims(frame(top - d * 2.9, rot), style, hilt_only=True)
        elif wf is not None:
            out += sword_prims(wf, style)
    if kind == "axe" and mode == "stow":
        y = norm(cr @ np.array([-0.78, 0.62, 0.0]))
        z = norm(cr @ np.array([0.0, 0.0, 1.0]))
        x = np.cross(y, z)
        wf = frame(C(2.5, 0.5, -7.6), np.stack([x, y, z], axis=1))
    if kind == "sword" or wf is None:
        return out
    if kind == "staff":
        def W(x, y, z):
            return xf(wf, (x, y, z))
        out.append(seg(W(0, -15, 0), W(0, 19.5, 0), 0.8, 0.8, M["wood"], G_WEAPON, thin=True))
        out.append(ball(W(0, -15, 0), 1.0, M["gold"], G_WEAPON, thin=True))
        out.append(ball(W(0, 19.5, 0), 1.3, M["gold"], G_WEAPON, thin=True))
        for sg in (1, -1):
            out.append(seg(W(sg * 0.6, 19.8, 0), W(sg * 2.9, 22.0, 0), 0.75, 0.7, M["gold"], G_WEAPON, thin=True))
            out.append(seg(W(sg * 2.9, 22.0, 0), W(sg * 2.3, 24.6, 0), 0.7, 0.4, M["gold"], G_WEAPON, thin=True))
        out.append(seg(W(0, 20.9, 0), W(0, 23.0, 0), 0.3, 1.7, M["crystal"], G_WEAPON, thin=True))
        out.append(seg(W(0, 23.0, 0), W(0, 25.6, 0), 1.7, 0.2, M["crystal"], G_WEAPON, thin=True))
    elif kind == "axe":
        r = wf[:3, :3]

        def W(x, y, z):
            return xf(wf, (x, y, z))
        out.append(seg(W(0, -5, 0), W(0, 14.5, 0), 0.9, 0.9, M["wood"], G_WEAPON, thin=True))
        out.append(ball(W(0, -5, 0), 1.1, M["steel_d"], G_WEAPON, thin=True))
        am = lambda q: np.where(q[0] > 0.45, M["blade"], M["steel_d"])
        out.append(ball(W(2.6, 12.3, 0), (4.3, 4.6, 0.8), am, G_WEAPON, rot=r, thin=True))
        out.append(seg(W(-0.5, 12.3, 0), W(-3.0, 12.3, 0), 1.3, 0.3, M["steel_d"], G_WEAPON, sz=0.6, thin=True))
        out.append(ball(W(0, 12.3, 0), (1.2, 2.2, 1.2), M["gold"], G_WEAPON, rot=r, thin=True))
    elif kind == "spear":
        r = wf[:3, :3]

        def W(x, y, z):
            return xf(wf, (x, y, z))
        out.append(seg(W(0, -15, 0), W(0, 18.5, 0), 0.75, 0.75, M["wood"], G_WEAPON, thin=True))
        out.append(ball(W(0, 18.0, 0), (1.5, 1.2, 1.5), M["rust"], G_WEAPON, rot=r, thin=True))
        bm = lambda q: np.where(q[0] > 0.1, M["blade"], M["blade_d"])
        out.append(seg(W(0, 19.0, 0), W(0, 21.0, 0), 0.6, 1.7, bm, G_WEAPON, xhint=r[:, 0], sz=0.45, thin=True))
        out.append(seg(W(0, 21.0, 0), W(0, 25.6, 0), 1.7, 0.15, bm, G_WEAPON, xhint=r[:, 0], sz=0.45, thin=True))
        out.append(ball(W(0, -15, 0), 0.9, M["steel_d"], G_WEAPON, thin=True))
    return out


def shield_prims(s, P, C):
    p = s.p
    cr = s.chest[:3, :3]
    mode = p["shield"]
    if mode == "back":
        centre = C(0.5, 3.4, -6.4)
        n = norm(cr @ np.array([0.0, 0.12, -1.0]))
    elif mode == "ground":
        x, z = p["spos"]
        centre = np.array([x, 1.0, z])
        n = np.array([0.0, 1.0, 0.05])
    else:
        n = norm(p["sdir"])
        centre = (s.el["L"] + s.hand["L"]) / 2 + n * 2.4
    up = np.array([0, 1.0, 0]) if abs(n[1]) < 0.9 else np.array([0, 0, -1.0])
    x = norm(np.cross(up, n))
    y = np.cross(n, x)
    rot = np.stack([x, y, n], axis=1)

    def sm(q):
        rr = np.sqrt(q[0] ** 2 + q[1] ** 2)
        quarter = (q[0] * q[1]) > 0
        return np.where(rr > 0.8, M["steel"], np.where(quarter, M["ochre_d"], M["ochre"]))

    out = [ball(centre, (7.8, 7.8, 1.4), sm, G_SHIELD, rot=rot),
           ball(centre + n * 1.0, (2.2, 2.2, 1.5), M["steel"], G_SHIELD, rot=rot)]
    return out
