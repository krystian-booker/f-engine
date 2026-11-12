"""The shared rig: a pose (pelvis, spine, head, hand and foot targets, cloth
angles, weapon) is solved into joints and frames, and a character's costume
is hung on them as primitives for the ray-caster."""
import numpy as np
from render import ball, seg, box, frame, euler, xf, xd, norm, rx, ry, rz
from pixel import Palette

# ---------------------------------------------------------------- palette
PAL = Palette()
_ = PAL.add
#                 outline    shadow     mid        light      highlight
_("skin",        ["#55303c", "#b06c5c", "#dc9c78", "#f6c8a0", "#ffe6cc"], bands=(-0.25, 0.1))
_("skin_pale",   ["#4c2f40", "#a4746e", "#d0a48e", "#eccbb2", "#fbe8d8"], bands=(-0.25, 0.1))
_("skin_tan",    ["#472630", "#925646", "#bd8058", "#dda878", "#f2c898"], bands=(-0.25, 0.1))
_("hair_sand",   ["#4a2a20", "#96602e", "#cc9a40", "#ecc86e", "#fff0b0"], gloss=0.93, bands=(-0.05, 0.42))
_("hair_black",  ["#0a0b16", "#181a2e", "#272b48", "#3c4268", "#6870a0"], gloss=0.9, bands=(-0.05, 0.45))
_("hair_auburn", ["#3a1420", "#742a28", "#aa442c", "#d86c3a", "#f8a460"], gloss=0.93, bands=(-0.05, 0.42))
_("hair_grey",   ["#2a2b3c", "#5a5d72", "#898da0", "#bbbfcc", "#eceef4"], gloss=0.93, bands=(-0.05, 0.42))
_("hair_iron",   ["#1c1a24", "#383642", "#54525e", "#767482", "#a4a4b0"], gloss=0.95, bands=(-0.05, 0.42))
_("navy",        ["#0b1026", "#18244c", "#243872", "#33509a", "#5274c8"])
_("royal",       ["#0e1a4a", "#1c3688", "#2a54c4", "#4078ee", "#74a6ff"])
_("royal_d",     ["#0e1a4a", "#162a6c", "#1c3688", "#2a54c4", "#4078ee"])
_("gold",        ["#4a2e14", "#8e6022", "#cc982e", "#f4cc50", "#fff4b0"], gloss=0.8)
_("cream",       ["#4c3e46", "#9a8678", "#cdb99a", "#efe2c4", "#fffbec"])
_("cream_d",     ["#4c3e46", "#84705f", "#b49e82", "#d6c5a4", "#efe2c4"])
_("teal",        ["#0a3040", "#16666c", "#1e9890", "#40cab6", "#92f2de"])
_("leather",     ["#22131a", "#482a22", "#6e442c", "#96623a", "#bc8650"])
_("leather_d",   ["#180e14", "#32201e", "#4c3026", "#684630", "#86603e"])
_("charcoal",    ["#101120", "#282a3c", "#40445a", "#5e647c", "#949ab4"], gloss=0.86)
_("charcoal_l",  ["#161828", "#343850", "#525870", "#747c96", "#a8b0c8"], gloss=0.82)
_("crimson",     ["#34081a", "#6e122c", "#aa1c34", "#de3240", "#ff705e"])
_("crimson_d",   ["#34081a", "#560e26", "#84162e", "#b82238", "#de3240"])
_("steel",       ["#242a3c", "#566078", "#8892a8", "#bac4d4", "#f4f8fc"], gloss=0.8)
_("steel_d",     ["#1c2030", "#40485e", "#687288", "#96a0b6", "#c8d0de"], gloss=0.86)
_("ochre",       ["#3a2414", "#7c5022", "#b47e2a", "#dea83c", "#f8d470"])
_("ochre_d",     ["#3a2414", "#623c1c", "#8c5e24", "#b47e2a", "#dea83c"])
_("rust",        ["#220a12", "#4c1a22", "#742a28", "#98392e", "#c05a40"], gloss=0.86)
_("rust_d",      ["#1a0810", "#36121a", "#541c20", "#742a28", "#98392e"], gloss=0.92)
_("black",       ["#07070c", "#17161f", "#272632", "#3c3a4c", "#5e5c74"], gloss=0.88)
_("blade",       ["#262c44", "#6a7892", "#a6b6cc", "#dae4f0", "#ffffff"], gloss=0.6, bands=(-0.3, 0.05))
_("blade_d",     ["#262c44", "#4c5870", "#7c8aa4", "#aebcd0", "#dae4f0"], bands=(-0.3, 0.05))
_("wood",        ["#28181a", "#543624", "#7e5430", "#aa7840", "#cc9a58"])
_("crystal",     ["#12465a", "#2a90aa", "#58d2da", "#aaf6f2", "#f2ffff"], gloss=0.55, bands=(-0.3, 0.0))
_("white",       ["#40445a", "#9aa0b8", "#cfd4e2", "#f2f4fa", "#ffffff"])
_("visor",       ["#050508", "#0a0a10", "#0e0e16", "#14141e", "#1c1c28"])
_("fx",          ["#8fb4ff", "#b8d4ff", "#dceaff", "#f4f9ff", "#ffffff"])
del _
M = PAL.index

# ----------------------------------------------------------------- groups
G_HEAD, G_TORSO, G_ARML, G_ARMR, G_LEGL, G_LEGR, G_CAPE, G_WEAPON, G_SHIELD, G_SKIRT, G_SCAB, G_FX, G_HAIR, G_TAIL = range(14)

# ------------------------------------------------------------------ build
BASE = dict(
    scale=1.0,
    hip=14.3, hipw=2.6, thigh=6.8, shin=6.4, ankle=1.6,
    waist=3.2, shw=5.6, shy=7.0, neck=8.8,
    upper=5.8, fore=5.4,
    head_r=7.9, head_y=6.9, head_z=0.8,
    torso_r=(4.3, 4.8), torso_sx=1.2, torso_sz=0.86,
    arm_r=(2.3, 2.05, 1.85), hand_r=1.95,
    leg_r=(2.7, 2.35, 2.05), foot_r=1.65, foot_len=3.5,
)

POSE = dict(
    pel=(0.0, 0.0, 0.0), pel_r=(0.0, 0.0, 0.0), chest_r=(0.0, 0.0, 0.0), head_r=(0.0, 0.0, 0.0),
    footL=None, footR=None, footL_r=(0.0, 12.0), footR_r=(0.0, 12.0), kneeL=None, kneeR=None,
    handL=(1.9, -10.7, 0.8), handR=(1.9, -10.7, 0.8), elbowL=None, elbowR=None,
    weapon="stow", wdir=(0.0, 1.0, 0.0), wedge=(0.0, 0.0, 1.0), wpos=None, wgrip=0.0,
    palmL=False, palmR=False,
    shield="back", sdir=(0.3, 0.0, 1.0), spos=None,
    cape=(7.0, 0.0, 4.0), cape_body=False, hair=(0.0, 0.0), sash=(4.0, 0.0),
    eyes="open", mouth="closed", lie=0.0, lift=0.0, shift=(0.0, 0.0, 0.0), fx=None, mirror_e=False,
)


def pose(**kw):
    p = dict(POSE)
    for k, v in kw.items():
        if k not in p:
            raise KeyError(k)
        p[k] = v
    return p


def ik(a, target, l1, l2, pole):
    """Two-bone IK: returns the middle joint and the (possibly clamped) end."""
    a, target = np.asarray(a, float), np.asarray(target, float)
    d = target - a
    dist = np.linalg.norm(d)
    if dist < 1e-6:
        d, dist = np.array([0, -1.0, 0]), 1e-6
    u = d / dist
    reach = min(dist, (l1 + l2) * 0.999)
    reach = max(reach, abs(l1 - l2) + 1e-3)
    ca = (l1 * l1 + reach * reach - l2 * l2) / (2 * l1 * reach)
    ca = np.clip(ca, -1, 1)
    sa = np.sqrt(1 - ca * ca)
    pole = np.asarray(pole, float)
    pp = pole - u * np.dot(pole, u)
    if np.linalg.norm(pp) < 1e-6:
        pp = np.array([0, 0, 1.0]) - u * u[2]
    pp = pp / np.linalg.norm(pp)
    mid = a + u * (l1 * ca) + pp * (l1 * sa)
    return mid, a + u * reach


class Skel:
    pass


def solve(p, b):
    """Joints and frames of the posed skeleton, in character space."""
    s = Skel()
    s.b, s.p = b, p
    s.pel = frame(np.array(p["pel"]) + (0, b["hip"], 0), euler(*p["pel_r"]))
    s.chest = s.pel @ frame((0, b["waist"], 0), euler(*p["chest_r"]))
    s.head = s.chest @ frame((0, b["neck"], 0), euler(*p["head_r"]))
    s.headc = xf(s.head, (0, b["head_y"], b["head_z"]))
    s.sh, s.el, s.hand, s.hipj, s.knee, s.ankle, s.footd = {}, {}, {}, {}, {}, {}, {}
    cr = s.chest[:3, :3]
    pr = s.pel[:3, :3]
    for side, sg in (("L", 1.0), ("R", -1.0)):
        # Legs
        hipj = xf(s.pel, (sg * b["hipw"], 0, 0))
        t = p["foot" + side]
        if t is None:
            t = (sg * (b["hipw"] + 0.5), 0.0, 0.0)
        local = len(t) == 4
        if local:
            ankle = xf(s.pel, t[1:])
            fwd = pr @ np.array([0, 0, 1.0])
        else:
            ankle = np.array([t[0], t[1] + b["ankle"], t[2]])
            fwd = np.array([0, 0, 1.0])
        pole = p["knee" + side]
        if pole is None:
            pole = (sg * 0.22, 0, 1.0)
        pole = pr @ np.asarray(pole, float) if local else np.asarray(pole, float)
        knee, ankle = ik(hipj, ankle, b["thigh"], b["shin"], pole)
        shin = norm(ankle - knee)
        fpitch, fyaw = p["foot" + side + "_r"]
        f = ry(sg * fyaw) @ np.array([0, 0, 1.0])
        f = (pr @ f) if local else f
        f = norm(f - shin * np.dot(f, shin)) if local else f
        a = np.radians(fpitch)
        f = norm(f * np.cos(a) + (shin if local else np.array([0, -1.0, 0])) * np.sin(a))
        s.hipj[side], s.knee[side], s.ankle[side], s.footd[side] = hipj, knee, ankle, f
    for side, sg in (("L", 1.0), ("R", -1.0)):
        s.sh[side] = xf(s.chest, (sg * b["shw"], b["shy"], 0))
    # Weapon in a hand needs that hand first; "shaft" hands follow the weapon.
    s.wframe = None
    order = ("R", "L") if p["handR"][0] != "shaft" else ("L", "R")
    for side in order:
        sg = 1.0 if side == "L" else -1.0
        t = p["hand" + side]
        if t[0] == "w":
            target = np.array(t[1:], float)
        elif t[0] == "c":
            target = xf(s.chest, t[1:])
        elif t[0] == "shaft":
            target = s.wframe[:3, 3] + s.wframe[:3, 1] * t[1]
        else:
            target = s.sh[side] + cr @ np.array([sg * t[0], t[1], t[2]])
        pole = p["elbow" + side]
        if pole is None:
            pole = (0.5, -0.3, -1.0)
        pole = cr @ np.array([sg * pole[0], pole[1], pole[2]])
        el, hand = ik(s.sh[side], target, b["upper"], b["fore"], pole)
        s.el[side], s.hand[side] = el, hand
        if s.wframe is None and p["weapon"] == "hand":
            y = norm(p["wdir"])
            x = np.asarray(p["wedge"], float)
            x = x - y * np.dot(x, y)
            if np.linalg.norm(x) < 1e-6:
                x = np.array([1.0, 0, 0]) - y * y[0]
            x = norm(x)
            z = np.cross(x, y)
            rot = np.stack([x, y, z], axis=1)
            s.wframe = frame(hand - y * p["wgrip"], rot)
    return s
