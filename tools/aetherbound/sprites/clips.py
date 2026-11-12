"""The clip table and every pose of it. A clip function takes the weapon
kind of the character ("sword", "staff", "axe", "spear") and returns one
pose per frame; the same poses serve all four directions."""
import numpy as np
from rig import pose
from render import norm

# name, frame durations, loop, events by frame index
CLIPS = [
    ("idle", [0.30] * 4, True, {}),
    ("walk", [0.10] * 6, True, {0: "step", 3: "step"}),
    ("run", [0.08] * 6, True, {0: "step", 3: "step"}),
    ("ready", [0.22] * 4, True, {}),
    ("attack", [0.10, 0.08, 0.06, 0.10, 0.12, 0.14], False, {3: "hit"}),
    ("cast", [0.16] * 4, True, {}),
    ("guard", [0.4] * 2, True, {}),
    ("hurt", [0.12, 0.28], False, {}),
    ("critical", [0.5] * 2, True, {}),
    ("ko", [0.5], False, {}),
    ("victory", [0.4] * 2, True, {}),
    ("kneel", [0.5], False, {}),
    ("thrown", [0.5], False, {}),
    ("rise", [0.35] * 3, False, {}),
    ("reach", [0.5], False, {}),
    ("raise", [0.35] * 2, True, {}),
    ("shield", [0.5], False, {}),
    ("look_up", [0.5], False, {}),
    ("hold", [0.5], False, {}),
]

HANG = (1.9, -10.7, 0.8)
POLE_ARM = dict(staff=True, spear=True)


def lerp(a, b, t):
    return tuple(np.asarray(a, float) * (1 - t) + np.asarray(b, float) * t)


def field(wk, **kw):
    """A pose with the weapon as it is carried outside battle."""
    base = {}
    if wk in ("staff", "spear"):
        base.update(weapon="hand", handR=(3.8, -7.4, 0.2), wdir=(-0.3, 0.95, -0.02), wgrip=0.6,
                    elbowR=(0.7, -0.4, -1.0), mirror_e=True)
    base.update(kw)
    return pose(**base)


def battle(wk, **kw):
    base = dict(weapon="hand", shield="arm")
    base.update(kw)
    return pose(**base)


# ------------------------------------------------------------------ field
def idle(wk):
    out = []
    for br, sway in ((0.0, 0.0), (0.5, 0.25), (1.0, 0.4), (0.5, 0.25)):
        kw = dict(pel=(sway * 0.5, -0.75 * br, 0), chest_r=(1.2 * br, 0, 0), head_r=(0.8 * br, 0, 0),
                  handL=(HANG[0] + 0.25 * br, HANG[1] + 0.2 * br, HANG[2]),
                  cape=(7 + 1.5 * br, 0, 4 + br), sash=(4 + br, 0), hair=(br * 2, 0))
        if wk not in POLE_ARM:
            kw["handR"] = (HANG[0] + 0.25 * br, HANG[1] + 0.2 * br, HANG[2])
        out.append(field(wk, **kw))
    return out


def _legs(i, stride, stance, swing, n=6):
    """Foot targets of a gait frame: the right leg leads at frame 0."""
    half = n // 2
    res = {}
    for side, off, x in (("R", 0, -3.0), ("L", half, 3.0)):
        k = (i + off) % n
        z, y, pitch = stance[k] if k < half else swing[k - half]
        res["foot" + side] = (x, y, z * stride)
        res["foot" + side + "_r"] = (pitch, 10.0)
    return res


def walk(wk):
    out = []
    stance = ((1.0, 0.0, -14), (0.34, 0.0, 0), (-0.34, 0.0, 6))
    swing = ((-1.0, 0.6, 32), (-0.4, 3.4, 34), (0.55, 2.8, 2))
    bob = (-0.5, -1.1, 0.2)
    for i in range(6):
        ph = 2 * np.pi * i / 6
        c = np.cos(ph)
        kw = _legs(i, 5.0, stance, swing)
        kw.update(pel=(0, bob[i % 3], 0), pel_r=(3, 6 * c, -1.5 * c), chest_r=(1, -11 * c, 1.5 * c),
                  head_r=(-2, 5 * c, 0),
                  handL=(1.9, -10.6 + 0.9 * max(c, 0), 0.8 + 3.4 * c),
                  cape=(15 + 3 * np.sin(2 * ph + 0.6), 7 * np.sin(ph + 0.5), 8 + 6 * np.cos(2 * ph)),
                  sash=(12 + 5 * np.sin(2 * ph), 4 * np.sin(ph)), hair=(8 + 5 * np.sin(2 * ph + 1.0), 5 * np.sin(ph)))
        if wk in POLE_ARM:
            kw.update(handR=(3.8, -7.4 + 0.3 * abs(c), 0.2 + 0.5 * c), wdir=(-0.3, 0.95, -0.02 + 0.04 * c))
        else:
            kw.update(handR=(1.9, -10.6 + 0.9 * max(-c, 0), 0.8 - 3.4 * c))
        out.append(field(wk, **kw))
    return out


def run(wk):
    out = []
    stance = ((0.75, 0.0, -12), (0.0, 0.0, 4), (-0.75, 0.7, 34))
    swing = ((-1.0, 4.6, 66), (-0.15, 5.2, 44), (0.62, 3.4, 0))
    bob = (-0.7, -1.7, 0.7)
    fwd, bck = np.array((1.0, -5.0, 5.2)), np.array((2.2, -6.6, -4.6))
    for i in range(6):
        ph = 2 * np.pi * i / 6
        c = np.cos(ph)
        t = (c + 1) / 2
        kw = _legs(i, 6.2, stance, swing)
        kw.update(pel=(0, bob[i % 3], -1.0), pel_r=(15, 9 * c, -2 * c), chest_r=(7, -16 * c, 2 * c),
                  head_r=(-13, 7 * c, 0),
                  handL=tuple(bck + (fwd - bck) * t), elbowL=(0.6, -0.6, -1.0),
                  cape=(52 + 7 * np.sin(2 * ph + 0.5), 4 * np.sin(ph), 12 + 14 * np.cos(2 * ph)),
                  sash=(46 + 10 * np.sin(2 * ph), 6 * np.sin(ph)), hair=(36 + 10 * np.sin(2 * ph + 1.0), 8 * np.sin(ph)))
        if wk in POLE_ARM:
            kw.update(weapon="hand", handR=(2.6, -8.6 + 0.5 * c, 1.0 + 1.2 * c), wdir=(-0.12, 0.3, 0.94), wgrip=5.0,
                      elbowR=(0.7, -0.3, -1.0), mirror_e=True)
            out.append(pose(**kw))
        else:
            kw.update(handR=tuple(fwd + (bck - fwd) * t), elbowR=(0.6, -0.6, -1.0))
            out.append(field(wk, **kw))
    return out


# ----------------------------------------------------------------- battle
STANCE = dict(footL=(2.6, 0, 3.4), footR=(-3.4, 0, -3.0), footR_r=(0, 38), footL_r=(0, 4))


def _ready_pose(wk, dy=0.0, tw=0.0):
    """The battle stance; `tw` nudges the weapon (breathing)."""
    kw = dict(STANCE)
    kw.update(pel=(0, -1.5 + dy, 0), pel_r=(5, -22, 0), chest_r=(3, -6, 0), head_r=(-4, 26, 0),
              cape=(10 + 2 * tw, 0, 5), sash=(6 + 2 * tw, 0), hair=(2 * tw, 0))
    if wk == "sword":
        kw.update(handR=("w", -3.2, 15.2 + dy + 0.3 * tw, 5.6), wdir=(-0.24, 0.8 + 0.02 * tw, 0.55), wedge=(0, -0.6, 0.8),
                  handL=("shaft", -1.9), elbowL=(0.8, -0.6, -0.4), elbowR=(0.8, -0.6, -0.6))
    elif wk == "staff":
        kw.update(handR=("w", -2.8, 14.6 + dy + 0.3 * tw, 4.6), wdir=(0.32, 0.9, 0.3), wgrip=-1.0,
                  handL=("shaft", 7.0), elbowL=(0.8, -0.6, -0.4), elbowR=(0.8, -0.6, -0.6))
    elif wk == "axe":
        kw.update(handL=(-0.6, -5.2, 5.6), elbowL=(1.0, -0.5, 0.0), sdir=(0.3, 0.08, 1.0),
                  handR=(3.2, -4.6 + 0.3 * tw, -1.2), wdir=(-0.15, 0.92, -0.3), wedge=(0, 0.3, 1.0),
                  elbowR=(0.8, -0.8, -0.6))
    elif wk == "spear":
        kw.update(handR=("w", -3.6, 14.6 + dy + 0.2 * tw, -2.6), wdir=(0.14, 0.1 + 0.01 * tw, 1.0), wedge=(1, 0, 0),
                  handL=("shaft", 8.5), elbowL=(0.8, -0.8, 0.0), elbowR=(0.8, -0.6, -0.8))
    return kw


def ready(wk):
    return [battle(wk, **_ready_pose(wk, dy, tw)) for dy, tw in ((0, 0), (-0.5, 0.5), (-0.9, 1.0), (-0.5, 0.5))]


def _arc(points, r0=0.5, r1=1.5):
    """A slash smear through 3D points (tail first)."""
    from render import seg
    from rig import M, G_FX
    out = []
    n = len(points) - 1
    for i in range(n):
        a, b = r0 + (r1 - r0) * i / n, r0 + (r1 - r0) * (i + 1) / n
        out.append(seg(points[i], points[i + 1], a, b, M["fx"], G_FX, thin=True))
    return out


def _tip(kw, length):
    """Where the weapon's far end is, for hands given in character space."""
    h = np.array(kw["handR"][1:], float)
    return h + norm(kw["wdir"]) * length


def _slash(wk):
    """Overhead cut with a step: sword, staff and axe share it."""
    length = dict(sword=24.0, staff=24.0, axe=15.0)[wk]
    two = wk != "axe"
    frames = []
    # (pelvis offset, pelvis rot, chest rot, head rot, hand R, weapon dir, foot L, foot R, cape)
    keys = [
        ((0, -1.2, -1.2), (2, -26, 0), (-2, -10, 0), (-2, 30, 0), (-4.6, 21.0, -0.5), (-0.1, 0.94, -0.3),
         (2.6, 0, 3.4, 0), (-3.4, 0, -3.0, 0), (6, 0, 4)),
        ((0, -0.9, -2.0), (-4, -32, 0), (-8, -16, 0), (4, 38, 0), (-4.0, 24.4, -2.6), (-0.25, 0.5, -0.83),
         (2.6, 0.9, 3.8, -10), (-3.4, 0, -3.0, 0), (2, 0, 2)),
        ((0, -1.4, 0.4), (6, -10, 0), (4, 6, 0), (-6, 6, 0), (-2.4, 25.0, 3.4), (0.1, 0.84, 0.54),
         (2.6, 0.6, 4.8, -14), (-3.4, 0.2, -3.0, 20), (22, 0, 10)),
        ((0, -2.6, 1.2), (12, 6, 0), (14, 14, 0), (-16, -16, 0), (-1.0, 16.6, 6.0), (0.56, -0.36, 0.75),
         (2.6, 0, 5.2, 0), (-3.4, 0.4, -3.2, 34), (36, 0, 16)),
        ((0, -2.9, 1.0), (14, 10, 0), (16, 16, 0), (-16, -20, 0), (1.5, 14.2, 6.6), (0.8, -0.3, 0.52),
         (2.6, 0, 5.2, 0), (-3.4, 0.4, -3.2, 34), (24, 0, -6)),
        ((0, -2.0, 0.4), (8, -8, 0), (8, 2, 0), (-8, 6, 0), (-1.0, 14.6, 6.4), (0.2, 0.6, 0.78),
         (2.6, 0, 4.2, 0), (-3.4, 0.2, -3.0, 16), (12, 0, 2)),
    ]
    prev_tip = None
    for i, (pel, pel_r, chest_r, head_r, hr, wd, fl, fr, cape) in enumerate(keys):
        kw = dict(pel=pel, pel_r=pel_r, chest_r=chest_r, head_r=head_r,
                  handR=("w",) + hr, wdir=wd, wedge=(0, -0.5, 0.86) if wd[1] > 0 else (0, 0.86, 0.5),
                  footL=fl[:3], footL_r=(fl[3], 4), footR=fr[:3], footR_r=(fr[3], 38),
                  cape=cape, sash=(cape[0] * 0.8, 0), hair=(cape[0] * 0.7, 0), elbowR=(0.9, 0.2, -0.6))
        if two:
            kw.update(handL=("shaft", -1.9 if wk == "sword" else 6.0), elbowL=(0.9, 0.2, -0.3))
            if wk == "staff":
                kw["wgrip"] = -1.0
        else:
            kw.update(handL=(2.6, -6.4, 1.6 + (2.0 if i in (0, 1) else 0)), elbowL=(1.0, -0.4, -0.5),
                      sdir=(1.0, 0.1, 0.5))
        if i in (2, 3, 4):
            kw["mouth"] = "open"
        tip = _tip(kw, length)
        if i == 2:
            # smear from the wind-up to here, through an arc overhead
            a, b = prev_tip, tip
            mid = (a + b) / 2 + np.array([0, 5.5, 0])
            pts = [(1 - t) ** 2 * a + 2 * (1 - t) * t * mid + t * t * b for t in np.linspace(0.25, 1, 6)]
            kw["fx"] = _arc(pts, 0.35, 1.6)
        if i == 3:
            a, b = prev_tip, tip
            mid = (a + b) / 2 + np.array([0, 5.5, 6.0])
            pts = [(1 - t) ** 2 * a + 2 * (1 - t) * t * mid + t * t * b for t in np.linspace(0.0, 1, 9)]
            kw["fx"] = _arc(pts, 0.3, 2.3)
        if i == 4:
            a, b = prev_tip, tip
            mid = (a + b) / 2 + np.array([0, 1.0, 4.0])
            pts = [(1 - t) ** 2 * a + 2 * (1 - t) * t * mid + t * t * b for t in np.linspace(0.45, 1, 5)]
            kw["fx"] = _arc(pts, 0.3, 0.9)
        prev_tip = tip
        frames.append(battle(wk, **kw))
    return frames


def _thrust():
    keys = [
        # pelvis, pelvis rot, chest rot, head rot, rear hand, dir, front-hand reach, foot L z, cape
        ((0, -1.4, -1.0), (4, -28, 0), (2, -10, 0), (-4, 34, 0), (-3.8, 15.0, -4.6), (0.14, 0.12, 1.0), 8.5, 3.4, 8),
        ((0, -1.6, -2.4), (2, -38, 0), (0, -14, 0), (-2, 46, 0), (-4.2, 16.0, -7.2), (0.16, 0.1, 1.0), 9.5, 3.0, 3),
        ((0, -2.0, 0.6), (8, -14, 0), (6, 0, 0), (-8, 12, 0), (-3.0, 15.4, -1.6), (0.1, 0.04, 1.0), 8.0, 5.0, 24),
        ((0, -2.8, 2.6), (14, 6, 0), (10, 12, 0), (-14, -14, 0), (-1.6, 14.6, 1.6), (0.06, -0.02, 1.0), 7.0, 6.2, 40),
        ((0, -2.7, 2.2), (13, 4, 0), (9, 10, 0), (-13, -12, 0), (-1.8, 14.6, 0.8), (0.07, -0.01, 1.0), 7.0, 6.2, 22),
        ((0, -1.9, 0.4), (7, -14, 0), (5, -2, 0), (-6, 14, 0), (-3.2, 14.6, -1.8), (0.12, 0.08, 1.0), 8.2, 4.4, 10),
    ]
    frames = []
    for i, (pel, pel_r, chest_r, head_r, hr, wd, reach, fz, cape) in enumerate(keys):
        kw = dict(pel=pel, pel_r=pel_r, chest_r=chest_r, head_r=head_r, handR=("w",) + hr, wdir=wd, wedge=(1, 0, 0),
                  handL=("shaft", reach), elbowL=(0.8, -0.8, 0.0), elbowR=(0.8, -0.6, -0.8),
                  footL=(2.6, 0.6 if i == 2 else 0, fz), footL_r=(0, 4), footR=(-3.4, 0.3 if i in (3, 4) else 0, -3.0),
                  footR_r=(30 if i in (3, 4) else 0, 38), cape=(cape, 0, 6), sash=(cape, 0), hair=(cape * 0.8, 0))
        if i == 3:
            from render import seg
            from rig import M, G_FX
            tip = np.array(hr) + norm(wd) * 24.0
            kw["fx"] = [seg(tip - norm(wd) * 13.0 + np.array([0, 1.6, 0]), tip + np.array([0, 0.4, 0]), 0.3, 0.9, M["fx"], G_FX, thin=True),
                        seg(tip - norm(wd) * 10.0 - np.array([0, 1.6, 0]), tip - np.array([0, 0.4, 0]), 0.3, 0.8, M["fx"], G_FX, thin=True)]
        frames.append(battle("spear", **kw))
    return frames


def attack(wk):
    return _thrust() if wk == "spear" else _slash(wk)


def cast(wk):
    out = []
    for i in range(4):
        ph = 2 * np.pi * i / 4
        s_, c_ = np.sin(ph), np.cos(ph)
        up = 0.5 * s_
        kw = dict(pel=(0, -0.6 + 0.3 * s_, -0.3), pel_r=(-2, -8, 0), chest_r=(-4, -4, 0), head_r=(-8, 10, 0),
                  footL=(3.0, 0, 1.2), footR=(-3.2, 0, -1.4), footR_r=(0, 24),
                  handL=(1.6, -1.2 + up, 6.4), elbowL=(1.0, -0.6, 0.0), palmL=True,
                  cape=(34 + 8 * s_, 5 * c_, 10 + 12 * c_), sash=(30 + 8 * c_, 6 * s_), hair=(26 + 8 * c_, 6 * s_),
                  elbowR=(1.0, -0.2, -0.4))
        if wk == "sword":
            kw.update(handR=("w", -4.6, 19.6 + up, 4.4), wdir=(-0.14, 1.0, 0.06), wedge=(1, 0, 0))
        elif wk == "staff":
            kw.update(handR=(2.2, 3.2 + up, 2.6), wdir=(0.04, 1.0, 0.12), wgrip=1.0)
        elif wk == "axe":
            kw.update(handR=(2.6, 3.4 + up, 2.0), wdir=(-0.1, 1.0, 0.1), wedge=(0, 0, 1),
                      handL=(2.4, -5.6, 3.0), palmL=False, sdir=(0.8, 0.1, 0.6))
        elif wk == "spear":
            kw.update(handR=(2.2, 0.6 + up, 2.6), wdir=(0.04, 1.0, 0.1), wgrip=3.0)
        out.append(battle(wk, **kw))
    return out


def guard(wk):
    out = []
    for dy in (0.0, -0.45):
        kw = dict(STANCE)
        kw.update(pel=(0, -2.7 + dy, -0.6), pel_r=(8, -24, 0), chest_r=(8, -4, 0), head_r=(4, 28, 0),
                  cape=(8, 0, 3), sash=(5, 0))
        if wk == "sword":
            kw.update(handR=("w", -5.0, 12.0 + dy, 6.2), wdir=(0.68, 0.66, 0.3), wedge=(0.3, -0.3, 0.9),
                      handL=("shaft", 11.5), elbowL=(1.0, -0.4, 0.0), elbowR=(1.0, -0.6, -0.4))
        elif wk == "staff":
            kw.update(handR=("w", -4.4, 13.0 + dy, 6.0), wdir=(0.6, 0.76, 0.24), wgrip=-3.0,
                      handL=("shaft", 9.0), elbowL=(1.0, -0.4, 0.0), elbowR=(1.0, -0.6, -0.4))
        elif wk == "axe":
            kw.update(handL=(-2.2, -3.4, 6.4), elbowL=(1.0, -0.4, 0.2), sdir=(0.1, 0.1, 1.0),
                      handR=(3.0, -8.6, -2.0), wdir=(-0.2, 0.5, -0.85), wedge=(0, 0.86, 0.5))
        elif wk == "spear":
            kw.update(handR=("w", -4.6, 11.6 + dy, 5.2), wdir=(0.5, 0.84, 0.2), wgrip=-4.0, wedge=(1, 0, 0),
                      handL=("shaft", 9.0), elbowL=(1.0, -0.4, 0.0), elbowR=(1.0, -0.6, -0.4))
        out.append(battle(wk, **kw))
    return out


def _loose_weapon(wk, kw, low=True):
    """Weapon hanging from the right hand after a blow."""
    if wk == "axe":
        kw.update(wdir=(-0.2, -0.3, 0.9), wedge=(0, 1, 0.3))
    elif wk in ("staff", "spear"):
        kw.update(wdir=(-0.25, 0.9, 0.35), wgrip=2.0)
    else:
        kw.update(wdir=(-0.3, -0.35, 0.88), wedge=(0, 0.9, 0.4))


def hurt(wk):
    out = []
    for k in (1.0, 0.55):
        kw = dict(pel=(0, -1.2 * k, -2.6 * k), pel_r=(-12 * k, -10, 3 * k), chest_r=(-15 * k, 0, 5 * k),
                  head_r=(-14 * k, 8, 8 * k),
                  footL=(2.8, 0, 2.6), footR=(-3.4, 0, -4.2), footR_r=(0, 30),
                  handL=(4.4, -5.2 - 3 * (1 - k), 1.6), elbowL=(1.0, -0.2, -0.6),
                  handR=(4.2, -6.4 - 2 * (1 - k), 1.2), elbowR=(1.0, -0.2, -0.6),
                  cape=(2, 0, -6 * k), sash=(0, 0), hair=(-14 * k, 0),
                  eyes="hurt", mouth="open", sdir=(1.0, 0.2, 0.4), lift=(1.0, 0.0))
        _loose_weapon(wk, kw)
        out.append(battle(wk, **kw))
    return out


KNEEL = dict(footR=(-3.0, 0.2, -6.4), footR_r=(78, 0), kneeR=(0, -1, 0.6),
             footL=(3.2, 0, 4.6), footL_r=(0, 4), kneeL=(0.1, 0.4, 1), lift=3.0)


def _planted(wk, kw, dy=0.0):
    """Leaning on the weapon, its point or butt on the ground."""
    if wk == "sword":
        kw.update(weapon="hand", handR=("w", -4.6, 23.4 + dy, 4.2), wdir=(0, -1, 0.03), wedge=(1, 0, 0))
    elif wk == "staff":
        kw.update(weapon="hand", handR=("w", -4.8, 19.0 + dy, 4.2), wdir=(0.03, 1, 0.0), wgrip=4.0)
    elif wk == "spear":
        kw.update(weapon="hand", handR=("w", -4.8, 19.0 + dy, 4.2), wdir=(0.03, 1, 0.0), wgrip=4.0)
    elif wk == "axe":
        kw.update(weapon="hand", handR=("w", -5.0, 16.6 + dy, 4.4), wdir=(0, -1, 0.05), wedge=(-1, 0, 0), wgrip=2.6)


def critical(wk):
    out = []
    for br in (0.0, 1.0):
        kw = dict(KNEEL)
        kw.update(pel=(0, -5.0, 0), pel_r=(6, -6, 0), chest_r=(20 + 4 * br, 0, 0), head_r=(16 + 4 * br, 0, 0),
                  handL=("w", 3.6, 11.4, 5.6), elbowL=(1, -0.4, -0.4), elbowR=(1.0, -0.5, 0),
                  cape=(10, 0, 4), sash=(4, 0), hair=(-8, 0), mouth="open" if br else "closed", eyes="hurt",
                  shield="arm", sdir=(1.0, 0.0, 0.3))
        _planted(wk, kw, -0.6 * br)
        if wk == "axe":
            kw.update(handL=(2.4, -8.0, 2.0))
        out.append(pose(**kw))
    return out


LIE = dict(pel=(0, -11.5, -8.0), pel_r=(88, 0, 0), chest_r=(-4, 0, 6), head_r=(-10, 62, 0),
           footL=("p", 3.4, -12.4, -0.8), footR=("p", -2.4, -12.9, -0.3), footL_r=(70, 14), footR_r=(74, 10),
           kneeL=(0.6, 0, 1), kneeR=(-0.2, 0, 1), cape_body=True, lie=58.0, lift=(5.0, 3.0))


def _dropped(wk, kw):
    if wk == "sword":
        kw.update(weapon="ground", wpos=(-10.0, 4.0, 22.0, 0))
    elif wk == "axe":
        kw.update(weapon="ground", wpos=(-10.0, 6.0, 30.0, 0), shield="back")
    else:
        kw.update(weapon="ground", wpos=(-9.0, -2.0, 12.0, 0))


def ko(wk):
    kw = dict(LIE)
    kw.update(handL=(3.4, 4.6, 2.4), handR=(4.0, 3.0, 2.6), elbowL=(1, -0.5, 0.3), elbowR=(1, -0.5, 0.3),
              cape=(6, 4, 3), sash=(4, 0), hair=(4, 6), eyes="closed")
    _dropped(wk, kw)
    return [pose(**kw)]


def victory(wk):
    out = []
    for up in (0.0, 1.0):
        kw = dict(pel=(0, 0.5 * up - 0.2, 0), pel_r=(-2, 0, 0), chest_r=(-4, 0, -2), head_r=(-8, 0, 0),
                  footL=(3.3, 0, 0.4), footR=(-3.3, 0, -0.2),
                  handR=(3.0, 4.6 + 0.6 * up, 1.0), elbowR=(1.0, -0.3, -0.3), wdir=(-0.16, 1.0, 0.06),
                  handL=(2.6, -6.6, 1.2), elbowL=(1.0, 0.1, -0.5),
                  cape=(10 + 6 * up, 0, 6 + 6 * up), sash=(6 + 4 * up, 0), hair=(4 * up, 0), mouth="open",
                  sdir=(1.0, 0.1, 0.5))
        if wk in ("staff", "spear"):
            kw.update(wgrip=3.0, handR=(3.0, 3.0 + 0.6 * up, 1.4))
        if wk == "axe":
            kw.update(wedge=(-1, 0, 0.2))
        out.append(battle(wk, **kw))
    return out


def kneel(wk):
    kw = dict(KNEEL)
    kw.update(pel=(0, -5.0, 0), pel_r=(4, 0, 0), chest_r=(8, 0, 0), head_r=(26, 0, 0),
              handL=("w", 3.6, 11.6, 5.4), elbowL=(1, -0.4, -0.4),
              handR=(-3.4, -3.6, 4.4), elbowR=(1.0, -0.6, 0.2), cape=(9, 0, 4), sash=(4, 0), eyes="closed")
    if wk in POLE_ARM:
        kw.pop("handR")
        kw.pop("elbowR")
        _planted(wk, kw)
        return [pose(**kw)]
    return [field(wk, **kw)]


def thrown(wk):
    kw = dict(pel=(0, -1.0, 5.0), pel_r=(-72, 0, 0), chest_r=(20, 0, 0), head_r=(24, 0, 0),
              footL=("p", 3.2, -8.6, 8.6), footR=("p", -2.8, -10.6, 6.0), footL_r=(40, 10), footR_r=(50, 10),
              kneeL=(0.3, 0.3, 1), kneeR=(-0.2, 0.3, 1),
              handL=(3.0, -1.6, 9.6), handR=(3.4, -3.4, 9.0), elbowL=(1, -0.6, 0), elbowR=(1, -0.6, 0),
              cape_body=True, cape=(-38, 0, -30), sash=(-40, 0), hair=(-50, 0), eyes="hurt", mouth="open",
              lie=-35.0, sdir=(1.0, 0.3, 0.2))
    if wk == "sword":
        kw.update(wdir=(-0.2, 0.75, 0.6), wedge=(0, -0.6, 0.8))
    elif wk == "axe":
        kw.update(wdir=(-0.2, 0.75, 0.6), wedge=(0, -0.6, 0.8))
    else:
        kw.update(wdir=(-0.3, 0.6, 0.74), wgrip=3.0)
    return [battle(wk, **kw)]


def rise(wk):
    a = dict(LIE)
    a.update(pel=(0, -10.6, -8.0), pel_r=(80, 0, 0), chest_r=(-26, 0, 0), head_r=(-22, 20, 0),
             handL=("w", 5.6, 1.6, 6.0), handR=("w", -5.6, 1.6, 6.5), elbowL=(1, 0.2, -0.6), elbowR=(1, 0.2, -0.6),
             cape=(8, 3, 4), sash=(4, 0), hair=(10, 4), eyes="hurt", lie=58.0)
    _dropped(wk, a)
    b = dict(KNEEL)
    b.update(pel=(0, -5.0, -1.0), pel_r=(8, -4, 0), chest_r=(16, 0, 0), head_r=(4, 0, 0),
             handL=("w", 3.6, 11.4, 4.6), elbowL=(1, -0.4, -0.4), elbowR=(1.0, -0.5, 0),
             cape=(10, 0, 4), sash=(4, 0), lie=20.0, shield="arm", sdir=(1.0, 0.0, 0.3))
    _planted(wk, b, -0.4)
    if wk == "axe":
        b.update(handL=(2.4, -8.0, 2.0))
    c = dict(pel=(0, -1.2, 0), pel_r=(6, -8, 0), chest_r=(8, 0, 0), head_r=(-2, 8, 0),
             footL=(3.0, 0, 1.6), footR=(-3.2, 0, -1.6), footR_r=(0, 24),
             handL=(2.2, -10.2, 1.2), handR=(2.4, -9.6, 2.0), cape=(8, 0, 5), sash=(5, 0),
             shield="arm", sdir=(1.0, 0.0, 0.3))
    if wk == "sword":
        c.update(weapon="hand", wdir=(-0.45, -0.3, 0.84), wedge=(0, 0.86, 0.5))
    elif wk == "axe":
        c.update(weapon="hand", wdir=(-0.1, -0.2, 0.97), wedge=(0, 1, 0.2))
    else:
        c.update(weapon="hand", handR=(3.8, -7.4, 0.2), wdir=(-0.3, 0.95, -0.02), wgrip=-1.5)
    return [pose(**a), pose(**b), pose(**c)]


def reach(wk):
    kw = dict(pel=(0, -1.6, 0.6), pel_r=(14, -16, 0), chest_r=(12, -8, 0), head_r=(-16, 22, 0),
              footL=(2.8, 0, 4.2), footR=(-3.2, 0.3, -4.0), footR_r=(30, 20), footL_r=(0, 4),
              handL=(0.6, 0.8, 11.0), elbowL=(0.6, -1.0, 0.0), palmL=True,
              cape=(26, 0, 8), sash=(18, 0), hair=(14, 0), mouth="open")
    if wk in POLE_ARM:
        kw.update(handR=(2.6, -8.4, -1.0), wdir=(0.05, 0.9, 0.45), wgrip=-3.0)
    else:
        kw.update(handR=(2.8, -8.6, -3.6))
    return [field(wk, **kw)]


def raise_(wk):
    out = []
    for up in (0.0, 1.0):
        kw = dict(pel=(0, -0.3 - 0.3 * up, 0), pel_r=(-2, -6, 0), chest_r=(-3, -4, -3), head_r=(-12, 10, 0),
                  footL=(3.4, 0, 0.8), footR=(-3.4, 0, -0.8), footR_r=(0, 22),
                  handL=(2.6, 9.6, 4.6), elbowL=(1.0, 0.2, -0.4), palmL=True,
                  cape=(12 + 8 * up, 3 * up, 6 + 10 * up), sash=(8 + 8 * up, 0), hair=(6 + 8 * up, 0))
        out.append(field(wk, **kw))
    return out


def shield(wk):
    kw = dict(pel=(0, -2.2, -0.8), pel_r=(10, -18, 0), chest_r=(8, -6, 0), head_r=(12, 4, 0),
              footL=(2.6, 0, 3.8), footR=(-3.4, 0.2, -4.4), footR_r=(18, 34), footL_r=(0, 4),
              handL=(-4.6, 2.6, 5.6), elbowL=(1.0, 0.6, 0.4),
              handR=(3.0, -9.4, -2.6), elbowR=(1.0, -0.4, -0.8),
              cape=(62, 0, 14), sash=(56, 0), hair=(48, 0), eyes="closed", mouth="grit")
    if wk == "sword":
        kw.update(wdir=(-0.5, -0.3, -0.8), wedge=(0, 0.86, -0.5))
    elif wk == "axe":
        kw.update(handL=(-1.6, 0.6, 6.6), elbowL=(1.0, -0.4, 0.2), sdir=(0.05, 0.16, 1.0),
                  wdir=(-0.25, -0.3, -0.92), wedge=(0, 1, -0.3))
    else:
        kw.update(handR=(2.6, -7.6, -1.0), wdir=(0.1, 0.9, -0.4), wgrip=-2.0)
    return [battle(wk, **kw)]


def look_up(wk):
    return [field(wk, pel=(0, -0.2, 0), chest_r=(-5, 0, 0), head_r=(-30, 0, 0), cape=(6, 0, 3))]


def hold(wk):
    kw = dict(pel=(0, -8.6, -1.0), pel_r=(4, 0, 0), chest_r=(14, 0, 0), head_r=(30, 0, 0),
              footL=(2.9, 0.1, -2.2), footR=(-2.9, 0.1, -2.2), footL_r=(80, 0), footR_r=(80, 0),
              kneeL=(0.25, -0.15, 1), kneeR=(-0.25, -0.15, 1),
              handL=("w", 1.7, 11.0, 5.6), handR=("w", -1.7, 11.0, 5.6), elbowL=(1, -0.8, -0.2), elbowR=(1, -0.8, -0.2),
              palmL=True, palmR=True, cape=(14, 0, 10), sash=(8, 0), eyes="closed", lift=5.0)
    if wk in POLE_ARM:
        kw.update(weapon="ground", wpos=(-3.0, -7.5, 90.0, 0))
    return [pose(**kw)]


FUNCS = dict(idle=idle, walk=walk, run=run, ready=ready, attack=attack, cast=cast, guard=guard, hurt=hurt,
             critical=critical, ko=ko, victory=victory, kneel=kneel, thrown=thrown, rise=rise, reach=reach,
             shield=shield, look_up=look_up, hold=hold)
FUNCS["raise"] = raise_


def frames_of(clip, wk):
    poses = FUNCS[clip](wk)
    want = len(next(c for c in CLIPS if c[0] == clip)[1])
    assert len(poses) == want, (clip, len(poses), want)
    return poses
