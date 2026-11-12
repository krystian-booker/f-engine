"""Animations of the Colossus: a small control rig (hips, feet and hands with two-bone IK, FK
spine and head, grips, eyes, spire fragments), key poses with eased tracks per control,
procedural layers (breathing, tremble, shiver, the sundering drift) and spring secondaries
(tassets, pauldrons). Everything is sampled at 30 fps and keyed bone by bone as Blender actions.

Controls (Blender axes: x = its left, -y = forward, z = up; rotations are (pitch, roll, yaw) in
degrees: pitch bows forward, roll leans left, yaw turns left):
  hip_p, hip_r             pelvis position and rotation in model space
  spine_r, chest_r, head_r relative rotations
  foot?_p, foot?_r         ankle position and foot rotation in model space
  knee?_pole               direction the knee points; knee?_pin / knee?_pw: pin the knee to a point
  hand?_c                  wrist target as an offset from the shoulder in chest space (follows the torso)
  hand?_w, hand?_k         wrist target in model space and its weight (1 = planted in the world)
  elb?_pole                elbow direction in chest space
  hand?_r                  wrist (flex, deviate, twist) in the hand's frame;  grip?: 0 open .. 1 fist
  paul?_r                  extra pauldron rotation;  eyes, core: scales
  tremble, shiver          amplitudes of the body's tremble and the spire fragments' shiver
  wrist_min                the wrists are kept at least this high (fists leaving or reaching the floor)
"""
import math
import numpy as np
from meshkit import euler, unit, rot_x, rot_y, rot_z

FPS = 30
SX = np.diag([-1.0, 1.0, 1.0])


def V(*a):
    return np.array(a, dtype=float)


def axis_angle(axis, deg):
    a = unit(axis)
    t = math.radians(deg)
    K = np.array([[0, -a[2], a[1]], [a[2], 0, -a[0]], [-a[1], a[0], 0]])
    return np.eye(3) + math.sin(t) * K + (1 - math.cos(t)) * (K @ K)


def rot_log(R):
    c = max(-1.0, min(1.0, (np.trace(R) - 1) / 2))
    ang = math.acos(c)
    if ang < 1e-6:
        return np.zeros(3)
    v = np.array([R[2, 1] - R[1, 2], R[0, 2] - R[2, 0], R[1, 0] - R[0, 1]]) / (2 * math.sin(ang))
    return v * ang


def rot_exp(v):
    ang = np.linalg.norm(v)
    if ang < 1e-9:
        return np.eye(3)
    return axis_angle(v / ang, math.degrees(ang))


def mat_quat(R):
    """Rotation matrix to (w, x, y, z)."""
    t = np.trace(R)
    if t > 0:
        s = math.sqrt(t + 1.0) * 2
        q = ((0.25 * s), (R[2, 1] - R[1, 2]) / s, (R[0, 2] - R[2, 0]) / s, (R[1, 0] - R[0, 1]) / s)
    elif R[0, 0] > R[1, 1] and R[0, 0] > R[2, 2]:
        s = math.sqrt(1.0 + R[0, 0] - R[1, 1] - R[2, 2]) * 2
        q = ((R[2, 1] - R[1, 2]) / s, 0.25 * s, (R[0, 1] + R[1, 0]) / s, (R[0, 2] + R[2, 0]) / s)
    elif R[1, 1] > R[2, 2]:
        s = math.sqrt(1.0 + R[1, 1] - R[0, 0] - R[2, 2]) * 2
        q = ((R[0, 2] - R[2, 0]) / s, (R[0, 1] + R[1, 0]) / s, 0.25 * s, (R[1, 2] + R[2, 1]) / s)
    else:
        s = math.sqrt(1.0 + R[2, 2] - R[0, 0] - R[1, 1]) * 2
        q = ((R[1, 0] - R[0, 1]) / s, (R[0, 2] + R[2, 0]) / s, (R[1, 2] + R[2, 1]) / s, 0.25 * s)
    q = np.array(q)
    return q / np.linalg.norm(q)


EASE = {
    "lin": lambda u: u,
    "inout": lambda u: u * u * (3 - 2 * u),
    "inout2": lambda u: u * u * u * (u * (6 * u - 15) + 10),
    "in": lambda u: u * u,
    "in3": lambda u: u * u * u,
    "out": lambda u: 1 - (1 - u) ** 2,
    "out3": lambda u: 1 - (1 - u) ** 3,
    "over": lambda u: 1 + 2.70158 * (u - 1) ** 3 + 1.70158 * (u - 1) ** 2,
    "hold": lambda u: 0.0 if u < 1 else 1.0,
}


def limb(root, target, L1, L2, pole, pin=None, pw=0.0):
    d = target - root
    dist = np.linalg.norm(d)
    dist = min(max(dist, abs(L1 - L2) + 1e-3), L1 + L2 - 1e-3)
    ax = unit(d)
    a = (L1 * L1 - L2 * L2 + dist * dist) / (2 * dist)
    h = math.sqrt(max(L1 * L1 - a * a, 0.0))
    pp = pole - ax * np.dot(pole, ax)
    pp = unit(pp) if np.linalg.norm(pp) > 1e-6 else unit(np.cross(ax, (1, 0, 0)))
    mid = root + ax * a + pp * h
    if pw > 0:
        mid = mid * (1 - pw) + np.asarray(pin) * pw
        mid = root + unit(mid - root) * L1
    end = mid + unit(target - mid) * L2
    a1, a2 = unit(mid - root), unit(end - mid)
    b = np.cross(a1, a2)
    if np.linalg.norm(b) < 1e-3:
        b = np.cross(pp, ax)
    b = unit(b)
    return mid, end, a1, a2, b


def frame(a, b):
    b = unit(b - a * np.dot(a, b))
    return np.stack([a, b, np.cross(a, b)], axis=1)


class Rig:
    def __init__(self, skel, info):
        self.skel = skel
        self.info = info
        self.rest = {n: np.array(h, dtype=float) for n, (p, h) in skel.items()}
        self.parent = {n: p for n, (p, h) in skel.items()}
        self.order = []
        left = dict(self.parent)
        while left:
            for n in list(left):
                if left[n] is None or left[n] in self.order:
                    self.order.append(n); del left[n]
        self.limbs = {}
        for side in "LR":
            for kind, (a, b, c) in (("leg", ("thigh_", "shin_", "foot_")), ("arm", ("upper_arm_", "forearm_", "hand_"))):
                p0, p1, p2 = self.rest[a + side], self.rest[b + side], self.rest[c + side]
                a1, a2 = unit(p1 - p0), unit(p2 - p1)
                bb = unit(np.cross(a1, a2))
                self.limbs[kind + side] = dict(L1=np.linalg.norm(p1 - p0), L2=np.linalg.norm(p2 - p1),
                                               F1=frame(a1, bb), F2=frame(a2, bb))
        self.hand_frame = info["hand_frame_L"][:3, :3]
        rng = np.random.RandomState(7)
        self.shard_rand = rng.uniform(0, 1, (len(info["shards"]), 8))

    def off(self, name):
        return self.rest[name] - self.rest[self.parent[name]]

    def solve(self, c, t=0.0, L=1.0):
        """World transforms {bone: (position, rotation, scale)} for a control dict."""
        out = {"root": (np.zeros(3), np.eye(3), 1.0)}
        tr = float(c["tremble"])

        def nz(k, f):          # looping tremble noise: k = channel, f = whole cycles per animation
            return math.sin(2 * math.pi * (f * t / L) + k * 1.7) * 0.6 + math.sin(2 * math.pi * ((f + 6) * t / L) + k * 2.9) * 0.4
        f0 = max(1, round(8.5 * L))
        hp = np.asarray(c["hip_p"], dtype=float) + tr * V(nz(1, f0) * 0.012, nz(2, f0 + 2) * 0.012, 0)
        hR = euler(*c["hip_r"])
        out["hips"] = (hp, hR, 1.0)
        sR = hR @ euler(*c["spine_r"])
        sp = hp + hR @ self.off("spine")
        out["spine"] = (sp, sR, 1.0)
        cR = sR @ euler(*(np.asarray(c["chest_r"]) + tr * V(nz(3, f0 + 1) * 0.9, nz(4, f0 + 3) * 0.7, nz(5, f0) * 0.6)))
        cp = sp + sR @ self.off("chest")
        out["chest"] = (cp, cR, 1.0)
        hdR = cR @ euler(*(np.asarray(c["head_r"]) + tr * V(nz(6, f0 + 4) * 1.6, nz(7, f0 + 2) * 1.8, nz(8, f0 + 5) * 1.4)))
        hdp = cp + cR @ self.off("head")
        out["head"] = (hdp, hdR, 1.0)
        for s in "LR":
            out["eye_" + s] = (hdp + hdR @ self.off("eye_" + s), hdR, max(float(c["eyes"]), 1e-4))
        out["core"] = (cp + cR @ self.off("core"), cR, float(c["core"]))
        for s in "LR":
            sgn = 1.0 if s == "L" else -1.0
            # ---- leg
            lg = self.limbs["leg" + s]
            hipj = hp + hR @ self.off("thigh_" + s)
            knee, ankle, a1, a2, b = limb(hipj, np.asarray(c[f"foot{s}_p"], dtype=float), lg["L1"], lg["L2"],
                                         np.asarray(c[f"knee{s}_pole"], dtype=float), c[f"knee{s}_pin"], float(c[f"knee{s}_pw"]))
            Rt = frame(a1, b) @ lg["F1"].T
            Rs = frame(a2, b) @ lg["F2"].T
            out["thigh_" + s] = (hipj, Rt, 1.0)
            out["shin_" + s] = (knee, Rs, 1.0)
            out["foot_" + s] = (ankle, euler(*c[f"foot{s}_r"]), 1.0)
            # ---- arm
            ar = self.limbs["arm" + s]
            shp = cp + cR @ self.off("upper_arm_" + s)
            tgt = shp + cR @ (np.asarray(c[f"hand{s}_c"], dtype=float) + tr * V(nz(9 + sgn, f0 + 1), nz(11 + sgn, f0 + 3), nz(13 + sgn, f0)) * 0.03)
            k = float(c[f"hand{s}_k"])
            if k > 0:
                tgt = tgt * (1 - k) + np.asarray(c[f"hand{s}_w"], dtype=float) * k
            tgt[2] = max(tgt[2], float(c["wrist_min"]))
            elb, wr, a1, a2, b = limb(shp, tgt, ar["L1"], ar["L2"], cR @ np.asarray(c[f"elb{s}_pole"], dtype=float))
            Ru = frame(a1, b) @ ar["F1"].T
            Rf = frame(a2, b) @ ar["F2"].T
            out["upper_arm_" + s] = (shp, Ru, 1.0)
            out["forearm_" + s] = (elb, Rf, 1.0)
            HF = self.hand_frame

            def local(E):
                R = HF @ E @ HF.T
                return R if s == "L" else SX @ R @ SX
            fl, dv, tw = c[f"hand{s}_r"]
            Rh = Rf @ local(rot_z(tw) @ rot_x(dv) @ rot_y(fl))
            out["hand_" + s] = (wr, Rh, 1.0)
            g = float(c[f"grip{s}"])
            R1 = Rh @ local(rot_y(12 + 78 * g))
            p1 = wr + Rh @ self.off("fingers1_" + s)
            out["fingers1_" + s] = (p1, R1, 1.0)
            out["fingers2_" + s] = (p1 + R1 @ self.off("fingers2_" + s), R1 @ local(rot_y(15 + 85 * g)), 1.0)
            out["thumb_" + s] = (wr + Rh @ self.off("thumb_" + s), Rh @ local(rot_z(-40 * g) @ rot_y(10 + 30 * g)), 1.0)
            # ---- pauldron: follows the upper arm part of the way
            rel = cR.T @ Ru
            Rp = cR @ rot_exp(rot_log(rel) * 0.45)
            e = euler(*c[f"paul{s}_r"])
            Rp = Rp @ (e if s == "L" else SX @ e @ SX)
            out["pauldron_" + s] = (cp + cR @ self.off("pauldron_" + s), Rp, 1.0)
            # ---- tasset over this thigh lifts with it
            d0 = unit(self.rest["shin_" + s] - self.rest["thigh_" + s])
            d1 = hR.T @ unit(knee - hipj)
            lift = math.degrees(math.atan2(-d1[1], -d1[2]) - math.atan2(-d0[1], -d0[2]))
            out["tasset_" + s] = (hp + hR @ self.off("tasset_" + s), hR @ rot_x(-max(lift, -10.0) * 0.8), 1.0)
            out["_lift" + s] = lift
        lf = max(out.pop("_liftL"), out.pop("_liftR"))
        lb = min(0.0, 0.0)
        out["tasset_F"] = (hp + hR @ self.off("tasset_F"), hR @ rot_x(-max(lf, 0.0) * 0.45), 1.0)
        out["tasset_B"] = (hp + hR @ self.off("tasset_B"), hR @ rot_x(lb), 1.0)
        # ---- spire fragments: attached, shivering
        sh = float(c["shiver"])
        for i in range(len(self.info["shards"])):
            name = f"shard_{i}"
            r = self.shard_rand[i]
            fi = max(1, round((5.0 + 6.0 * r[0]) * L))
            wob = V(math.sin(2 * math.pi * fi * t / L + r[1] * 6.28), math.sin(2 * math.pi * (fi + 3) * t / L + r[2] * 6.28),
                    math.sin(2 * math.pi * (fi + 5) * t / L + r[3] * 6.28))
            amp = sh * (1.0 if i >= 2 else 0.45)
            Rl = rot_exp(np.radians(wob * amp * 3.0))
            pl = self.off(name) + wob[[1, 2, 0]] * amp * 0.035
            out[name] = (cp + cR @ pl, cR @ Rl, 1.0)
        return out

    def to_local(self, world):
        """{bone: (loc, quat wxyz, scale)} in pose-basis terms (rest orientations are identity)."""
        res = {}
        for n in self.order:
            p, R, s = world[n]
            par = self.parent[n]
            if par is None:
                res[n] = (p, mat_quat(R), s)
                continue
            pp, Rp, _ = world[par]
            res[n] = (Rp.T @ (p - pp) - self.off(n), mat_quat(Rp.T @ R), s)
        return res


# ---- poses -----------------------------------------------------------------------------------------
def pose(**kw):
    c = dict(
        hip_p=V(0, 0.3, 2.4), hip_r=V(5, 0, 0), spine_r=V(4, 0, 0), chest_r=V(4, 0, 0), head_r=V(-8, 0, 0),
        footL_p=V(1.32, 0.02, 0.45), footL_r=V(0, 0, 14), kneeL_pole=V(0.35, -1, 0.1), kneeL_pin=V(0, 0, 0), kneeL_pw=0.0,
        footR_p=V(-1.32, 0.34, 0.45), footR_r=V(0, 0, -14), kneeR_pole=V(-0.35, -1, 0.1), kneeR_pin=V(0, 0, 0), kneeR_pw=0.0,
        handL_c=V(0.72, -1.15, -3.1), handL_w=V(0, 0, 0), handL_k=0.0, elbL_pole=V(0.6, 1, 0.2), handL_r=V(0, 0, 0), gripL=0.75,
        handR_c=V(-0.72, -1.15, -3.1), handR_w=V(0, 0, 0), handR_k=0.0, elbR_pole=V(-0.6, 1, 0.2), handR_r=V(0, 0, 0), gripR=0.75,
        paulL_r=V(0, 0, 0), paulR_r=V(0, 0, 0), eyes=1.0, core=1.0, tremble=0.0, shiver=0.0, wrist_min=0.0)
    for k, v in kw.items():
        if k not in c:
            raise KeyError(k)
        c[k] = np.array(v, dtype=float) if not np.isscalar(v) else float(v)
    return c


def mix(base, **kw):
    c = dict(base)
    for k, v in kw.items():
        if k not in c:
            raise KeyError(k)
        c[k] = np.array(v, dtype=float) if not np.isscalar(v) else float(v)
    return c


IDLE = pose()

# dormant: down on the left knee, right foot planted forward, slumped onto its fists
DORMANT = pose(
    hip_p=(0, 0.5, 2.0), hip_r=(12, 0, 0), spine_r=(12, 0, 0), chest_r=(12, 0, 0), head_r=(24, 0, 0),
    footR_p=(-1.3, -0.85, 0.45), footR_r=(0, 0, -10), kneeR_pole=(-0.5, -1, 0.3),
    footL_p=(1.25, 0.95, 0.72), footL_r=(150, 0, 6), kneeL_pin=(1.12, 0.0, 1.0), kneeL_pw=1.0,
    handL_w=(1.95, -2.1, 1.36), handL_k=1.0, handR_w=(-2.0, -2.3, 1.36), handR_k=1.0,
    elbL_pole=(1, 0.5, 0.3), elbR_pole=(-1, 0.5, 0.3), gripL=1.0, gripR=1.0, eyes=0.0, core=0.8, wrist_min=1.36)

# the end of stagger: down on the right knee, a step and a half behind where it stood
KNEEL = pose(
    hip_p=(0, 1.4, 2.0), hip_r=(11, 0, 0), spine_r=(11, 0, 0), chest_r=(11, 0, 0), head_r=(20, 0, 0),
    footL_p=(1.3, 0.25, 0.45), footL_r=(0, 0, 10), kneeL_pole=(0.5, -1, 0.3),
    footR_p=(-1.25, 1.85, 0.72), footR_r=(150, 0, -6), kneeR_pin=(-1.12, 0.9, 1.0), kneeR_pw=1.0,
    handL_w=(1.95, -1.2, 1.36), handL_k=1.0, handR_w=(-2.0, -1.4, 1.36), handR_k=1.0,
    elbL_pole=(1, 0.5, 0.3), elbR_pole=(-1, 0.5, 0.3), gripL=1.0, gripR=1.0, eyes=0.7, core=1.2, wrist_min=1.36)

# overload: planted wide, arched back, arms spread, head thrown back
OVER = pose(
    hip_p=(0, 0.35, 2.5), hip_r=(-4, 0, 0), spine_r=(-10, 0, 0), chest_r=(-16, 0, 0), head_r=(-30, 0, 0),
    footL_p=(1.5, -0.1, 0.45), footL_r=(0, 0, 22), footR_p=(-1.5, 0.4, 0.45), footR_r=(0, 0, -22),
    handL_c=(2.3, 0.5, 1.7), handR_c=(-2.3, 0.5, 1.7), elbL_pole=(1, 0.5, -0.6), elbR_pole=(-1, 0.5, -0.6),
    handL_r=(-25, 0, 0), handR_r=(-25, 0, 0), gripL=0.25, gripR=0.25, core=1.5, tremble=1.0, shiver=1.0)


class Anim:
    def __init__(self, name, length, base, loop=False):
        self.name, self.length, self.loop = name, length, loop
        self.base = base
        self.tracks = {}
        self.layers = []
        self.post = None

    def key(self, t, ease="inout", **kw):
        for k, v in kw.items():
            if k not in self.base:
                raise KeyError(k)
            v = np.array(v, dtype=float) if not np.isscalar(v) else float(v)
            self.tracks.setdefault(k, []).append((float(t), v, ease))
        return self

    def keypose(self, t, p, ease="inout", only=None, skip=()):
        for k, v in p.items():
            if (only is None or k in only) and k not in skip:
                self.tracks.setdefault(k, []).append((float(t), v, ease))
        return self

    def sample(self, t):
        c = {}
        for k, v0 in self.base.items():
            tr = self.tracks.get(k)
            if not tr:
                c[k] = v0
                continue
            tr.sort(key=lambda e: e[0])
            if t <= tr[0][0]:
                c[k] = tr[0][1]
            elif t >= tr[-1][0]:
                c[k] = tr[-1][1]
            else:
                for i in range(1, len(tr)):
                    if t <= tr[i][0]:
                        t0, a, _ = tr[i - 1]
                        t1, b, e = tr[i]
                        u = EASE[e]((t - t0) / max(t1 - t0, 1e-9))
                        if k.endswith("_c"):       # hand targets swing about the shoulder: keep the arm's length
                            la, lb = np.linalg.norm(a), np.linalg.norm(b)
                            v = a + (b - a) * u
                            c[k] = unit(v) * (la + (lb - la) * u)
                        else:
                            c[k] = a + (b - a) * u
                        break
        c = {k: (v.copy() if isinstance(v, np.ndarray) else v) for k, v in c.items()}
        for fn in self.layers:
            fn(t, c)
        return c


def spring_pass(rig, worlds, loop):
    """Secondary motion: tassets swing on the pelvis' accelerations, pauldrons bounce on the
    chest's. Mutates the world transforms. For loops the simulation runs twice round."""
    n = len(worlds)
    dt = 1.0 / FPS
    hp = np.array([w["hips"][0] for w in worlds])
    cpos = np.array([w["chest"][0] + w["chest"][1] @ rig.off("upper_arm_L") * 0 for w in worlds])

    def accel(p, i):
        if loop:
            m = n - 1
            a, b, c = p[(i - 1) % m], p[i % m], p[(i + 1) % m]
        else:
            a, b, c = p[max(i - 1, 0)], p[i], p[min(i + 1, n - 1)]
        return (a - 2 * b + c) / (dt * dt)
    # bone, driven by, mode, stiffness, damping, gain (degrees per unit/s^2 of acceleration, times stiffness), limit
    specs = [("tasset_F", "hips", 0, 55.0, 5.5, 34.0, 26.0), ("tasset_B", "hips", 0, 45.0, 5.0, -30.0, 26.0),
             ("tasset_L", "hips", 0, 50.0, 5.5, 32.0, 26.0), ("tasset_R", "hips", 0, 50.0, 5.5, 32.0, 26.0),
             ("pauldron_L", "chest", 1, 120.0, 7.0, 34.0, 9.0), ("pauldron_R", "chest", 1, 120.0, 7.0, 34.0, 9.0)]
    for bone, par, mode, k, damp, gain, lim in specs:
        th, om = 0.0, 0.0
        passes = 2 if loop else 1
        for ps in range(passes):
            for i in range(n):
                if loop and i == n - 1 and ps == 0:
                    continue
                R = worlds[i][par][1]
                a = accel(hp if par == "hips" else cpos, i)
                drive = (R.T @ a)[1] if mode == 0 else (R.T @ a)[2]
                drive = max(-40.0, min(40.0, drive))
                om += (-k * th - damp * om - gain * drive) * dt
                th += om * dt
                th = max(-lim, min(lim, th))
                if ps == passes - 1:
                    p, Rb, s = worlds[i][bone]
                    if not loop:                       # come to rest by the last frame, so clips join without a pop
                        u = min((n - 1 - i) / (0.3 * FPS), 1.0)
                        th_out = th * u * u * (3 - 2 * u)
                    else:
                        th_out = th
                    if mode == 0:
                        worlds[i][bone] = (p, Rb @ rot_x(th_out if bone != "tasset_B" else -th_out), s)
                    else:
                        sg = 1.0 if bone.endswith("_L") else -1.0
                        worlds[i][bone] = (p, Rb @ rot_y(-sg * abs(th_out) * 0.9) @ rot_x(th_out * 0.3), s)
        if loop:
            worlds[n - 1][bone] = worlds[0][bone]


def bake(rig, anim):
    n = int(round(anim.length * FPS))
    worlds = []
    for f in range(n + 1):
        t = f / FPS
        c = anim.sample(t if not anim.loop else (t if f < n else 0.0))
        worlds.append(rig.solve(c, t if f < n or not anim.loop else 0.0, anim.length))
    if anim.post:
        anim.post(rig, worlds)
    spring_pass(rig, worlds, anim.loop)
    return worlds


# ---- the animations ----------------------------------------------------------------------------------
def make_animations(rig):
    A = {}
    tau = 2 * math.pi

    # ---------------------------------------------------------------- idle: heavy slow sway
    def idle_layer(t, c, L=3.0):
        s, s2, s3 = math.sin(tau * t / L), math.sin(tau * t / L - 0.7), math.sin(tau * t / L - 1.4)
        br = 0.5 - 0.5 * math.cos(tau * t / L)
        c["hip_p"] += V(0.07 * s, 0.0, -0.05 * br)
        c["hip_r"] += V(0.8 * br, 1.2 * s, 0)
        c["spine_r"] += V(1.0 * br, -0.8 * s2, 0)
        c["chest_r"] += V(-2.4 * br, -1.0 * s2, 1.2 * s2)
        c["head_r"] += V(1.5 * br, 1.4 * s3, -2.0 * s3)
        c["handL_c"] += V(0.03 * s3, 0.1 * s3 - 0.05 * br, 0.03 * br)
        c["handR_c"] += V(0.03 * s3, -0.1 * s3 - 0.05 * br, 0.03 * br)
        c["core"] += 0.08 * math.sin(tau * 2 * t / L)
        c["shiver"] += 0.12
    a = Anim("idle", 3.0, IDLE, loop=True)
    a.layers.append(idle_layer)
    A["idle"] = a
    idle0 = a.sample(0.0)
    idle0["shiver"] = 0.0          # the layer adds it back when sampled

    def idle_at(anim, t, ease="inout", skip=()):
        """Key the idle pose (without the idle layer's shiver, which the engine fades in)."""
        anim.keypose(t, mix(idle0, shiver=0.12), ease, skip=skip)

    # ---------------------------------------------------------------- dormant
    def dormant_layer(t, c, L=4.0):
        br = 0.5 - 0.5 * math.cos(tau * t / L)
        c["chest_r"] += V(-0.7 * br, 0, 0)
        c["head_r"] += V(0.5 * br, 0, 0)
        c["core"] += 0.1 * math.sin(tau * t / L)
        c["shiver"] += 0.06
    a = Anim("dormant", 4.0, DORMANT, loop=True)
    a.layers.append(dormant_layer)
    A["dormant"] = a
    dormant0 = a.sample(0.0)
    dormant0["shiver"] = 0.06

    # ---------------------------------------------------------------- awaken
    a = Anim("awaken", 5.0, IDLE)
    a.keypose(0.0, dormant0)
    a.key(2.9, "lin", wrist_min=1.36).key(3.3, "inout", wrist_min=0.0)
    # eyes ignite at 0.6 s
    a.key(0.5, "hold", eyes=0.0).key(0.6, "out", eyes=1.35).key(0.72, "inout", eyes=0.8).key(0.86, "inout", eyes=1.15).key(1.1, "inout", eyes=1.0)
    a.key(0.52, "lin", head_r=dormant0["head_r"]).key(0.64, "out", head_r=dormant0["head_r"] + V(-5, 0, 0)).key(0.8, "inout", head_r=dormant0["head_r"] + V(-2, 0, 0))
    a.key(0.6, "lin", core=0.8).key(0.9, "out", core=1.3).key(1.5, "inout", core=1.0)
    a.key(0.6, "lin", shiver=0.06).key(0.75, "out", shiver=0.9).key(1.4, "inout", shiver=0.15)
    # the head lifts and looks about
    a.key(1.6, "inout2", head_r=(-6, 0, 10)).key(2.1, "inout", head_r=(-10, 0, -6))
    a.key(0.9, "lin", chest_r=dormant0["chest_r"]).key(1.6, "inout", chest_r=(6, 0, 3))
    # a dip onto the fists, then it heaves itself up
    press = mix(dormant0, hip_p=(0, 0.5, 2.0), hip_r=(18, 0, 0), spine_r=(16, 0, 0), chest_r=(15, 0, 0))
    a.keypose(1.0, dormant0, "lin", only=("hip_p", "hip_r", "spine_r"))
    a.keypose(1.85, press, "inout", only=("hip_p", "hip_r", "spine_r", "chest_r"))
    rise = mix(idle0, hip_p=(0, 0.1, 2.2), hip_r=(16, 0, 0), spine_r=(10, 0, 0), chest_r=(8, 0, 0))
    a.keypose(2.75, rise, "inout2", only=("hip_p", "hip_r", "spine_r", "chest_r"))
    tall = mix(idle0, hip_p=(0, 0.3, 2.56), hip_r=(-2, 0, 0), spine_r=(-5, 0, 0), chest_r=(-10, 0, 0))
    a.keypose(3.45, tall, "inout", only=("hip_p", "hip_r", "spine_r", "chest_r"))
    # the kneeling leg comes forward and plants
    a.key(1.85, "lin", footL_p=dormant0["footL_p"], footL_r=dormant0["footL_r"], kneeL_pw=1.0)
    a.key(2.08, "in", footL_p=(1.28, 0.95, 1.4), footL_r=(115, 0, 7), kneeL_pw=0.0)
    a.key(2.3, "lin", footL_p=(1.3, 0.7, 1.72), footL_r=(70, 0, 8))
    a.key(2.2, "inout", hip_p=(0, 0.3, 2.14), hip_r=(18, 0, 0))
    a.key(2.52, "out", footL_p=(1.32, 0.12, 0.98), footL_r=(18, 0, 12))
    a.key(2.64, "in", footL_p=idle0["footL_p"], footL_r=idle0["footL_r"])
    # the planted foot shuffles back under it
    a.key(2.9, "lin", footR_p=dormant0["footR_p"], footR_r=dormant0["footR_r"], kneeR_pole=dormant0["kneeR_pole"])
    a.key(3.1, "in", footR_p=(-1.3, -0.3, 0.75), footR_r=(-8, 0, -12))
    a.key(3.28, "in", footR_p=idle0["footR_p"], footR_r=idle0["footR_r"], kneeR_pole=idle0["kneeR_pole"])
    # fists leave the floor
    for s, sg in (("L", 1), ("R", -1)):
        a.key(2.3, "lin", **{f"hand{s}_k": 1.0}).key(2.8, "inout", **{f"hand{s}_k": 0.0})
        a.key(2.0, "lin", **{f"hand{s}_c": (sg * 0.7, -1.3, -2.7), f"elb{s}_pole": dormant0[f"elb{s}_pole"]})
        a.key(2.9, "inout", **{f"hand{s}_c": (sg * 0.9, -1.2, -3.0), f"elb{s}_pole": idle0[f"elb{s}_pole"]})
        # the flex: arms drawn out and back, fists clenched, chest out
        a.key(3.45, "inout", **{f"hand{s}_c": (sg * 2.1, -0.3, -2.3), f"grip{s}": 1.0, f"hand{s}_r": (20, 0, 0)})
        a.key(3.75, "over", **{f"hand{s}_c": (sg * 2.5, 0.5, -1.9), f"hand{s}_r": (35, 0, 0)})
        a.key(4.2, "inout", **{f"hand{s}_c": (sg * 2.45, 0.45, -1.95)})
        a.key(5.0, "inout2", **{f"hand{s}_c": idle0[f"hand{s}_c"], f"grip{s}": idle0[f"grip{s}"], f"hand{s}_r": (0, 0, 0)})
    a.key(3.45, "inout", head_r=(-12, 0, 0)).key(3.8, "over", head_r=(-30, 0, 0)).key(4.2, "inout", head_r=(-27, 0, 0))
    a.key(3.8, "over", chest_r=(-20, 0, 0), spine_r=(-9, 0, 0)).key(4.2, "inout", chest_r=(-18, 0, 0), spine_r=(-8, 0, 0))
    a.key(3.4, "lin", tremble=0.0).key(3.8, "in", tremble=0.9).key(4.2, "lin", tremble=0.8).key(4.5, "out", tremble=0.0)
    a.key(3.5, "lin", core=1.0).key(3.8, "out", core=1.45).key(4.6, "inout", core=1.0)
    a.key(3.5, "lin", shiver=0.15).key(3.8, "out", shiver=0.8).key(4.6, "inout", shiver=0.12)
    # settles heavily into the idle stance
    drop = mix(idle0, hip_p=idle0["hip_p"] + V(0, 0, -0.12), hip_r=idle0["hip_r"] + V(3, 0, 0), chest_r=idle0["chest_r"] + V(4, 0, 0))
    a.keypose(4.62, drop, "in", only=("hip_p", "hip_r", "spine_r", "chest_r"))
    a.key(4.62, "in", head_r=(2, 0, 0))
    idle_at(a, 5.0, "inout", skip=("wrist_min", "eyes", "footL_p", "footL_r", "footR_p", "footR_r", "kneeL_pw", "kneeR_pole", "handL_c", "handR_c",
                                    "gripL", "gripR", "handL_r", "handR_r", "handL_k", "handR_k", "tremble", "core", "shiver"))
    A["awaken"] = a

    # ---------------------------------------------------------------- slam: right fist, impact at 0.9
    a = Anim("slam", 1.8, IDLE)
    idle_at(a, 0.0)
    wind = mix(idle0, hip_p=(0.05, 0.42, 2.3), hip_r=(8, 0, -6), spine_r=(6, 0, -6), chest_r=(8, 0, -10), head_r=(-4, 0, 8),
               handR_c=(-0.9, -0.8, -2.9), gripR=1.0, handL_c=(0.9, -1.3, -2.9))
    a.keypose(0.14, wind, "out", only=("hip_p", "hip_r", "spine_r", "chest_r", "head_r", "handR_c", "gripR", "handL_c"))
    top = mix(idle0, hip_p=(0.0, 0.5, 2.58), hip_r=(-4, 0, -4), spine_r=(-8, 0, -4), chest_r=(-14, 0, -7), head_r=(4, 0, 6),
              handR_c=(0.1, 0.5, 3.15), handR_r=(-20, 0, 0), elbR_pole=(-1, 0.4, 0.2), gripR=1.0,
              handL_c=(1.3, -1.6, -2.3), paulR_r=(0, 0, 0))
    a.keypose(0.54, top, "inout2", only=("hip_p", "hip_r", "spine_r", "chest_r", "head_r", "handR_r", "elbR_pole", "handL_c"))
    a.key(0.34, "in", handR_c=(-2.3, -1.5, 0.2)).key(0.54, "out", handR_c=top["handR_c"])
    a.key(0.66, "out", handR_c=(0.15, 0.9, 3.1), chest_r=(-17, 0, -8), hip_p=(0.0, 0.52, 2.6), hip_r=(-4, 0, -4), spine_r=(-8, 0, -4),
          handR_r=(-20, 0, 0), elbR_pole=(-1, 0.4, 0.2), handL_c=(1.3, -1.6, -2.3))
    a.key(0.8, "in", handR_c=(0.2, -2.3, 2.3), handR_r=(-14, 0, 0))
    hit = mix(idle0, hip_p=(0.0, -0.05, 2.02), hip_r=(13, 0, 5), spine_r=(9, 0, 5), chest_r=(11, 0, 10), head_r=(-2, 0, -6),
              handR_w=(-1.45, -3.45, 1.42), handR_k=1.0, handR_c=(0.6, -2.5, -2.2), handR_r=(-10, 0, 0), elbR_pole=(-1, 0.6, 0.6),
              handL_c=(1.3, 0.2, -3.0))
    a.keypose(0.9, hit, "in", only=("hip_p", "hip_r", "spine_r", "chest_r", "elbR_pole", "handL_c"))
    a.keypose(0.9, hit, "lin", only=("handR_c", "handR_r"))
    a.key(0.8, "lin", handR_k=0.0).key(0.9, "in", handR_k=1.0, handR_w=hit["handR_w"])
    a.key(0.68, "lin", head_r=top["head_r"]).key(0.93, "in", head_r=(12, 0, -6)).key(1.12, "out", head_r=(-4, 0, -6))
    # the jolt of the impact, then a held beat
    a.key(0.97, "out", hip_p=(0.0, -0.05, 2.1), chest_r=(8, 0, 10)).key(1.08, "inout", hip_p=(0.0, -0.05, 2.03), chest_r=(10, 0, 10))
    a.key(1.22, "inout", hip_p=(0.0, -0.03, 2.04), chest_r=(10, 0, 9), hip_r=(13, 0, 5), spine_r=(9, 0, 5), handL_c=(1.3, 0.2, -3.0),
          handR_c=(0.6, -2.5, -1.7), handR_r=(-10, 0, 0), elbR_pole=(-1, 0.6, 0.6))
    a.key(1.22, "lin", handR_k=1.0).key(1.5, "inout", handR_k=0.0)
    a.key(0.86, "lin", core=1.0).key(0.92, "out", core=1.4).key(1.3, "inout", core=1.0)
    a.key(0.88, "lin", shiver=0.12).key(0.93, "out", shiver=1.2).key(1.4, "inout", shiver=0.12)
    idle_at(a, 1.8, "inout2", skip=("handR_k", "handR_w", "core", "shiver"))
    A["slam"] = a

    # ---------------------------------------------------------------- sweep: left arm across the front, impact at 1.0
    a = Anim("sweep", 2.0, IDLE)
    idle_at(a, 0.0)
    wind = mix(idle0, hip_p=(0.22, 0.4, 2.22), hip_r=(8, 3, 14), spine_r=(5, 0, 12), chest_r=(6, 4, 18), head_r=(-4, 0, -30),
               handL_c=(2.0, 1.3, -2.2), handL_r=(-10, 0, 0), elbL_pole=(1, 0.8, 0.3), gripL=1.0, handR_c=(-0.6, -1.5, -2.9))
    a.keypose(0.5, wind, "inout2", only=("hip_p", "hip_r", "spine_r", "chest_r", "head_r", "handL_c", "handL_r", "elbL_pole", "gripL", "handR_c"))
    a.key(0.64, "out", hip_p=(0.26, 0.44, 2.2), chest_r=(6, 4, 21), handL_c=(1.9, 1.6, -2.1))
    a.key(0.88, "in", hip_p=(0.0, 0.1, 2.1), hip_r=(14, 0, -4), spine_r=(8, 0, -6), chest_r=(10, -3, -10), head_r=(0, 0, -6),
          handL_c=(2.0, -2.7, -1.25), handL_r=(-25, 0, 40), elbL_pole=(1, 0.2, 0.6))
    a.key(1.0, "lin", hip_p=(-0.14, 0.0, 2.08), hip_r=(15, -2, -12), spine_r=(9, 0, -12), chest_r=(11, -4, -20), head_r=(0, 0, 6),
          handL_c=(0.3, -3.45, -0.8), handL_r=(-30, 0, 60))
    a.key(1.14, "out", hip_p=(-0.24, 0.02, 2.1), hip_r=(14, -3, -18), spine_r=(8, 0, -16), chest_r=(9, -5, -30), head_r=(0, 0, 16),
          handL_c=(-1.6, -3.1, -0.8), handL_r=(-30, 0, 70), handR_c=(-1.2, 0.2, -3.0))
    a.key(1.4, "out", hip_p=(-0.26, 0.06, 2.14), chest_r=(8, -5, -34), handL_c=(-2.4, -2.5, -1.3))
    a.key(0.95, "lin", core=1.0).key(1.02, "out", core=1.3).key(1.4, "inout", core=1.0)
    idle_at(a, 2.0, "inout2", skip=("core",))
    A["sweep"] = a

    # ---------------------------------------------------------------- pulse: release at 1.6
    a = Anim("pulse", 2.6, IDLE)
    idle_at(a, 0.0)
    curl = mix(idle0, hip_p=(0, 0.2, 2.2), hip_r=(10, 0, 0), spine_r=(8, 0, 0), chest_r=(12, 0, 0), head_r=(8, 0, 0),
               handL_c=(-0.5, -2.0, -2.0), handR_c=(0.5, -2.0, -2.0), gripL=1.0, gripR=1.0, core=0.7)
    a.keypose(0.34, curl, "inout", only=("hip_p", "hip_r", "spine_r", "chest_r", "head_r", "handL_c", "handR_c", "gripL", "gripR", "core"))
    rear = mix(idle0, hip_p=(0, 0.45, 2.56), hip_r=(-6, 0, 0), spine_r=(-10, 0, 0), chest_r=(-18, 0, 0), head_r=(-26, 0, 0),
               handL_c=(2.2, 1.7, -1.9), handR_c=(-2.2, 1.7, -1.9), elbL_pole=(0.6, 1, 0.3), elbR_pole=(-0.6, 1, 0.3),
               handL_r=(-20, 0, 0), handR_r=(-20, 0, 0), gripL=0.3, gripR=0.3, core=1.7)
    a.keypose(1.3, rear, "inout2", only=("hip_p", "hip_r", "spine_r", "chest_r", "head_r", "handL_c", "handR_c", "elbL_pole", "elbR_pole",
                                         "handL_r", "handR_r", "gripL", "gripR", "core"))
    a.key(1.5, "inout", hip_p=(0, 0.5, 2.6), chest_r=(-22, 0, 0), head_r=(-30, 0, 0), core=1.85, handL_c=(2.25, 2.0, -1.7), handR_c=(-2.25, 2.0, -1.7))
    # release: the chest snaps forward, the arms are left behind
    a.key(1.6, "in3", hip_p=(0, 0.2, 2.36), hip_r=(4, 0, 0), spine_r=(2, 0, 0), chest_r=(4, 0, 0), head_r=(-8, 0, 0), core=2.3,
          handL_c=(2.3, 2.5, -1.2), handR_c=(-2.3, 2.5, -1.2), gripL=0.0, gripR=0.0)
    a.key(1.7, "out", hip_p=(0, 0.16, 2.33), chest_r=(7, 0, 0), head_r=(4, 0, 0), core=1.2, handL_c=(2.4, 1.6, -2.1), handR_c=(-2.4, 1.6, -2.1))
    a.key(1.95, "inout", hip_p=(0, 0.2, 2.34), chest_r=(6, 0, 0), head_r=(2, 0, 0), core=0.8)
    a.key(0.34, "lin", tremble=0.0, shiver=0.12).key(1.3, "in", tremble=1.0, shiver=1.3).key(1.56, "lin", tremble=1.3, shiver=1.6)
    a.key(1.62, "lin", tremble=0.2, shiver=2.2).key(2.1, "out", tremble=0.0, shiver=0.2)
    idle_at(a, 2.6, "inout2", skip=("tremble", "shiver"))
    a.key(2.6, "inout", tremble=0.0, shiver=0.12)
    A["pulse"] = a

    # ---------------------------------------------------------------- hit: flinch
    a = Anim("hit", 0.6, IDLE)
    idle_at(a, 0.0)
    fl = mix(idle0, hip_p=idle0["hip_p"] + V(0.05, 0.14, -0.06), hip_r=(0, 0, -3), spine_r=(-2, 0, -4), chest_r=(-8, -3, -8),
             handL_c=idle0["handL_c"] + V(0.25, -0.35, 0.25), handR_c=idle0["handR_c"] + V(-0.3, -0.3, 0.3))
    a.keypose(0.1, fl, "out3", only=("hip_p", "hip_r", "spine_r", "chest_r"))
    a.keypose(0.17, fl, "out", only=("handL_c", "handR_c"))
    a.key(0.06, "out", head_r=(4, 0, 4)).key(0.2, "inout", head_r=(-18, 4, -10)).key(0.4, "inout", head_r=(-6, 0, -2))
    a.key(0.03, "lin", eyes=0.35).key(0.1, "lin", eyes=1.3).key(0.16, "lin", eyes=0.6).key(0.3, "lin", eyes=1.0)
    a.key(0.04, "out", shiver=1.6).key(0.45, "inout", shiver=0.12)
    a.key(0.22, "inout", hip_p=idle0["hip_p"] + V(0.04, 0.12, -0.03), chest_r=(-5, -2, -6))
    idle_at(a, 0.6, "inout", skip=("eyes", "shiver"))
    A["hit"] = a

    # ---------------------------------------------------------------- stagger: two steps back, down on the right knee
    a = Anim("stagger", 2.5, IDLE)
    idle_at(a, 0.0)
    reel = mix(idle0, hip_p=(0.05, 0.6, 2.46), hip_r=(-8, 0, -4), spine_r=(-8, 0, -4), chest_r=(-14, -3, -8), head_r=(-22, 5, -12),
               handL_c=(1.6, -1.6, -1.6), handR_c=(-1.9, -1.2, -1.2), gripL=0.2, gripR=0.2)
    a.keypose(0.22, reel, "out3", only=("hip_p", "hip_r", "spine_r", "chest_r", "handL_c", "handR_c", "gripL", "gripR"))
    a.key(0.08, "out", head_r=(6, 0, 4)).key(0.3, "inout", head_r=(-24, 5, -12))
    # step one: the left foot catches it
    a.key(0.16, "lin", footL_p=idle0["footL_p"], footL_r=idle0["footL_r"])
    a.key(0.36, "out", footL_p=(1.34, 0.2, 0.8), footL_r=(-14, 0, 12))
    a.key(0.52, "in", footL_p=(1.3, 0.25, 0.45), footL_r=(0, 0, 10))
    a.key(0.58, "inout", hip_p=(0.1, 0.95, 2.26), hip_r=(-2, 2, -2), spine_r=(-4, 0, -2), chest_r=(-8, -2, -4))
    # step two: the right leg reaches far back and lands on its toes
    a.key(0.56, "lin", footR_p=idle0["footR_p"], footR_r=idle0["footR_r"])
    a.key(0.66, "out", footR_p=(-1.31, 0.5, 0.95), footR_r=(14, 0, -13))
    a.key(0.8, "lin", footR_p=(-1.3, 1.2, 1.72), footR_r=(70, 0, -10))
    a.key(1.0, "lin", footR_p=(-1.25, 1.8, 1.4), footR_r=(115, 0, -6))
    a.key(1.02, "inout", hip_p=(0.0, 1.3, 2.2), hip_r=(6, 0, 0), spine_r=(4, 0, 0), chest_r=(2, 0, 0), head_r=(-6, 0, 0),
          handL_c=(1.2, -1.5, -2.6), handR_c=(-1.2, -1.5, -2.6), gripL=0.5, gripR=0.5)
    # the knee gives way: it crashes down at 1.45
    a.key(1.12, "lin", kneeR_pw=0.0, kneeR_pin=KNEEL["kneeR_pin"])
    a.keypose(1.45, KNEEL, "in", only=("hip_p", "hip_r", "footR_p", "footR_r", "kneeR_pw", "kneeL_pole"))
    a.key(1.45, "in", spine_r=(16, 0, 0), chest_r=(15, 0, 0), handR_c=(-0.6, -1.9, -1.7), handL_c=(0.6, -1.9, -1.7))
    a.key(1.5, "in", head_r=(30, 0, 0))
    a.key(1.3, "lin", handR_k=0.0, handL_k=0.0).key(1.5, "in", handR_k=1.0, handR_w=KNEEL["handR_w"], gripR=1.0)
    a.key(1.56, "in", handL_k=1.0, handL_w=KNEEL["handL_w"], gripL=1.0)
    a.key(1.56, "out", hip_p=KNEEL["hip_p"] + V(0, 0, 0.07)).key(1.7, "inout", hip_p=KNEEL["hip_p"])
    a.keypose(2.1, KNEEL, "inout", only=("spine_r", "chest_r", "head_r", "elbL_pole", "elbR_pole"))
    a.key(0.0, "lin", eyes=1.0).key(0.06, "lin", eyes=0.3).key(0.14, "lin", eyes=1.2).key(0.3, "lin", eyes=0.5).key(0.5, "lin", eyes=1.0)
    a.key(1.4, "lin", eyes=1.0).key(1.5, "lin", eyes=0.3).key(1.62, "lin", eyes=0.9).key(1.8, "lin", eyes=0.4).key(2.2, "inout", eyes=KNEEL["eyes"])
    a.key(0.0, "lin", shiver=0.12).key(0.08, "out", shiver=2.0).key(0.8, "inout", shiver=0.5).key(1.45, "lin", shiver=0.5).key(1.5, "out", shiver=1.8)
    a.key(2.5, "inout", shiver=0.6)
    a.key(0.0, "lin", core=1.0).key(0.1, "out", core=1.6).key(2.5, "inout", core=KNEEL["core"])
    a.key(1.5, "lin", tremble=0.0).key(1.6, "out", tremble=0.6).key(2.5, "inout", tremble=0.25)
    kneel_end = mix(KNEEL, shiver=0.6, tremble=0.25)
    a.keypose(2.5, kneel_end, "inout", skip=("eyes", "shiver", "core", "tremble", "wrist_min"))
    a.key(1.2, "lin", wrist_min=0.0).key(1.45, "inout", wrist_min=1.36)
    A["stagger"] = a

    # ---------------------------------------------------------------- overload: trembling loop
    def over_layer(t, c, L=2.0):
        br = math.sin(tau * t / L)
        br2 = math.sin(tau * t / L - 0.9)
        c["hip_p"] += V(0, 0, 0.03 * br)
        c["chest_r"] += V(-2.0 * br, 0, 0)
        c["head_r"] += V(-2.5 * br2, 0, 0)
        c["handL_c"] += V(0.06 * br2, 0.08 * br2, 0.08 * br2)
        c["handR_c"] += V(-0.06 * br2, 0.08 * br2, 0.08 * br2)
        c["core"] += 0.25 * math.sin(tau * 2 * t / L)
        c["eyes"] += 0.2 * math.sin(tau * 3 * t / L)
    a = Anim("overload", 2.0, OVER, loop=True)
    a.layers.append(over_layer)
    A["overload"] = a

    # ---------------------------------------------------------------- overload_enter: from the knee to the overload stance
    over0 = A["overload"].sample(0.0)
    a = Anim("overload_enter", 3.0, OVER)
    a.keypose(0.0, kneel_end)
    a.key(1.5, "lin", wrist_min=1.36).key(1.9, "inout", wrist_min=0.0)
    a.key(0.5, "inout", head_r=(6, 0, 0), chest_r=(8, 0, 0), core=1.6, tremble=0.7, shiver=1.2, eyes=1.2)
    a.key(0.6, "lin", hip_p=KNEEL["hip_p"], hip_r=KNEEL["hip_r"], spine_r=KNEEL["spine_r"])
    a.key(1.0, "inout", hip_p=(0, 1.2, 2.2), hip_r=(14, 0, 0))
    a.key(1.25, "inout", hip_p=(0, 0.9, 2.28), hip_r=(14, 0, 0), spine_r=(8, 0, 0), chest_r=(4, 0, 0), head_r=(-4, 0, 0))
    a.key(0.9, "lin", handR_k=1.0, handL_k=1.0).key(1.28, "inout", handR_k=0.0, handL_k=0.0, handR_c=(-1.0, -1.8, -2.0), handL_c=(1.0, -1.8, -2.0))
    # the right leg drives forward and stamps down
    a.key(0.75, "lin", footR_p=KNEEL["footR_p"], footR_r=KNEEL["footR_r"], kneeR_pw=1.0)
    a.key(0.98, "in", footR_p=(-1.3, 1.8, 1.4), footR_r=(115, 0, -7), kneeR_pw=0.0)
    a.key(1.2, "lin", footR_p=(-1.38, 1.4, 1.72), footR_r=(70, 0, -10))
    a.key(1.42, "out", footR_p=(-1.5, 0.6, 0.98), footR_r=(18, 0, -20))
    a.key(1.52, "in", footR_p=OVER["footR_p"], footR_r=OVER["footR_r"])
    a.key(1.6, "lin", footL_p=KNEEL["footL_p"], footL_r=KNEEL["footL_r"], kneeL_pole=KNEEL["kneeL_pole"])
    a.key(1.8, "out", footL_p=(1.45, 0.05, 0.72), footL_r=(-6, 0, 18))
    a.key(1.95, "in", footL_p=OVER["footL_p"], footL_r=OVER["footL_r"], kneeL_pole=OVER["kneeL_pole"])
    a.key(1.6, "inout", hip_p=(0, 0.5, 2.3), hip_r=(8, 0, 0))
    a.key(2.0, "inout", hip_p=(0, 0.35, 2.4), hip_r=(2, 0, 0), spine_r=(0, 0, 0), chest_r=(-4, 0, 0), head_r=(-10, 0, 0))
    # arms spread, the back arches, the head is thrown back
    a.key(1.3, "inout", handL_c=(1.0, -1.7, -2.4), handR_c=(-1.0, -1.7, -2.4), elbL_pole=KNEEL["elbL_pole"], elbR_pole=KNEEL["elbR_pole"])
    a.key(2.5, "inout2", handL_c=(2.4, 0.6, 1.85), handR_c=(-2.4, 0.6, 1.85), elbL_pole=OVER["elbL_pole"], elbR_pole=OVER["elbR_pole"],
          handL_r=OVER["handL_r"], handR_r=OVER["handR_r"], gripL=0.2, gripR=0.2)
    a.key(2.55, "inout2", hip_p=(0, 0.38, 2.54), hip_r=(-5, 0, 0), spine_r=(-12, 0, 0), chest_r=(-19, 0, 0))
    a.key(2.62, "inout2", head_r=(-36, 0, 0))
    a.key(1.5, "lin", tremble=0.7, shiver=1.2).key(2.5, "in", tremble=1.3, shiver=1.6)
    a.key(1.0, "lin", core=1.6).key(2.5, "inout", core=1.9)
    a.keypose(3.0, over0, "inout")
    A["overload_enter"] = a

    # ---------------------------------------------------------------- sunder: the spire tears apart
    a = Anim("sunder", 6.0, OVER)
    a.keypose(0.0, over0)
    a.key(0.9, "in", hip_p=(0, 0.42, 2.44), spine_r=(-14, 0, 0), chest_r=(-24, 0, 0), head_r=(-38, 0, 0), tremble=1.8, shiver=2.6, core=2.2,
          handL_c=(2.5, 1.0, 2.0), handR_c=(-2.5, 1.0, 2.0), gripL=0.0, gripR=0.0)
    # the hands come in to claw at the wound
    a.key(1.3, "in", handL_c=(1.9, -2.3, 0.9), handR_c=(-1.9, -2.3, 0.9), elbL_pole=(1, 0.3, 0.0), elbR_pole=(-1, 0.3, 0.0),
          handL_r=(20, 0, 0), handR_r=(20, 0, 0), gripL=0.2, gripR=0.2)
    a.key(1.7, "out", handL_c=(-1.25, -2.2, -1.15), handR_c=(1.2, -2.25, -1.3), elbL_pole=(1, 0.2, 0.3), elbR_pole=(-1, 0.2, 0.3),
          handL_r=(55, 0, 0), handR_r=(55, 0, 0), gripL=0.42, gripR=0.42)
    a.key(1.7, "inout", hip_p=(0, 0.3, 2.3), hip_r=(4, 0, 0), spine_r=(-4, 0, 0), chest_r=(-10, 0, 0), head_r=(-30, 8, 6))
    a.key(2.5, "inout", hip_p=(0.05, 0.4, 2.22), hip_r=(0, 2, 5), spine_r=(-10, 0, 4), chest_r=(-20, 3, 8), head_r=(-40, -6, -12),
          handL_c=(-1.15, -2.3, -1.0), handR_c=(1.1, -2.3, -1.2), gripL=0.6, gripR=0.5)
    a.key(3.4, "inout", hip_p=(-0.05, 0.36, 2.16), hip_r=(2, -2, -5), spine_r=(-8, 0, -4), chest_r=(-16, -3, -8), head_r=(-34, 6, 10),
          handL_c=(-1.3, -2.2, -1.2), handR_c=(1.25, -2.2, -1.1), gripL=0.45, gripR=0.62)
    a.key(4.6, "inout2", hip_p=(0, 0.42, 2.12), hip_r=(-2, 0, 0), spine_r=(-12, 0, 0), chest_r=(-24, 0, 0), head_r=(-44, 0, 0),
          handL_c=(-1.2, -2.3, -1.05), handR_c=(1.15, -2.3, -1.15), gripL=0.55, gripR=0.55)
    a.key(1.6, "lin", tremble=1.8).key(4.4, "inout", tremble=1.2).key(5.4, "out", tremble=0.35)
    a.key(1.2, "lin", core=2.2).key(2.6, "inout", core=2.6).key(5.0, "inout", core=1.6)
    a.key(1.0, "lin", eyes=1.0).key(1.1, "lin", eyes=1.5).key(2.4, "lin", eyes=1.3).key(2.5, "lin", eyes=0.5).key(2.6, "lin", eyes=1.4)
    a.key(3.6, "lin", eyes=1.2).key(3.7, "lin", eyes=0.4).key(3.85, "lin", eyes=1.3).key(5.2, "inout", eyes=0.9)
    a.key(1.4, "lin", shiver=2.6).key(5.6, "inout", shiver=0.5)

    def sunder_post(rig, worlds):
        """Fragments break off one after another and drift up and outwards, tumbling slowly."""
        info = rig.info
        n = len(info["shards"])
        rng = np.random.RandomState(31)
        mid = (info["core"] + info["back_entry"]) / 2
        order = [7, 11, 8, 12, 9, 13, 10, 14, 3, 6, 2, 5, 4, 1, 0]      # the small growths first, the two great shards last
        release = {}
        for j, i in enumerate(order):
            release[i] = 1.25 + 1.45 * (j / (n - 1)) ** 0.8 + rng.uniform(-0.04, 0.04)
        state = {}
        for f, w in enumerate(worlds):
            t = f / FPS
            cp, cR, _ = w["chest"]
            for i in range(n):
                name = f"shard_{i}"
                if t < release[i]:
                    continue
                if i not in state:
                    p0, R0, _ = w[name]
                    sh = info["shards"][i]
                    outward = unit(sh["centre"] - mid)
                    out_h = unit(V(outward[0] * 0.6, outward[1], 0) + rng.uniform(-0.25, 0.25, 3) * V(0.6, 1, 0))
                    dist = rng.uniform(3.3, 4.3)
                    drift = unit(out_h * rng.uniform(0.55, 0.8) + V(0, 0, 1) * rng.uniform(0.6, 0.9)) * dist
                    if i == 0:
                        drift = V(0.5, 1.9, 3.3)
                    if i == 1:
                        drift = V(-0.6, -2.7, 2.7)
                    state[i] = dict(p0=p0.copy(), R0=R0.copy(), drift=drift, spin=unit(rng.uniform(-1, 1, 3)) * rng.uniform(25, 70),
                                    kick=outward * rng.uniform(0.25, 0.45), ph=rng.uniform(0, 6.28))
                st = state[i]
                u = (t - release[i]) / (6.0 - 0.3 - release[i])
                u = min(max(u, 0.0), 1.0)
                snap = 1 - (1 - min((t - release[i]) / 0.18, 1.0)) ** 3          # it cracks free with a jerk
                glide = 1 - (1 - u) ** 2.2                                          # then drifts, slowing
                p = st["p0"] + st["kick"] * snap + st["drift"] * glide + V(0, 0, 0.06) * math.sin(st["ph"] + t * 2.2) * u
                R = rot_exp(np.radians(st["spin"] * (t - release[i]) * (1.0 - 0.45 * u))) @ st["R0"]
                w[name] = (p, R, 1.0)
    a.post = sunder_post
    A["sunder"] = a
    return A


def key_all(arm, skel, info, only=None):
    """Key every animation on the armature; returns the named points for colossus.json."""
    import blend
    rig = Rig(skel, info)
    anims = make_animations(rig)
    baked = {}
    for name, a in anims.items():
        if only and name not in only:
            continue
        worlds = bake(rig, a)
        baked[name] = worlds
        frames = [rig.to_local(w) for w in worlds]
        blend.key_action(arm, name, frames, skel)

    def gl(p):          # Blender (x, y, z) -> glTF / engine model space (x, up, forward)
        return [round(float(p[0]), 3), round(float(p[2]), 3), round(float(-p[1]), 3)]

    def at(world, bone, rest_point):
        p, R, _ = world[bone]
        return p + R @ (np.asarray(rest_point) - rig.rest[bone])
    points = {}
    w0 = rig.solve(anims["idle"].sample(0.0), 0.0, 3.0)
    points["eye_left"] = gl(at(w0, "eye_L", rig.rest["eye_L"]))
    points["eye_right"] = gl(at(w0, "eye_R", rig.rest["eye_R"]))
    points["core"] = gl(at(w0, "chest", info["core"]))
    points["spire_tip"] = gl(at(w0, "chest", info["spire_tip"]))
    points["spire_front_tip"] = gl(at(w0, "chest", info["spire_front_tip"]))
    points["head"] = gl(at(w0, "head", info["head"]))
    fist = info["hand_frame_L"][:3, 3] + info["hand_frame_L"][:3, :3] @ V(0.1, 0, -1.2)      # the middle of the left fist
    fist_r = fist * V(-1, 1, 1)
    if "slam" in baked:
        p = at(baked["slam"][int(round(0.9 * FPS))], "hand_R", fist_r)
        points["fist_slam"] = gl(V(p[0], p[1], 0.0))
    if "sweep" in baked:
        p = at(baked["sweep"][int(round(1.0 * FPS))], "hand_L", fist)
        points["sweep_centre"] = gl(p)
    points["height"] = 7.0
    points["yaw_facing_plus_x"] = 90
    points["yaw_facing_minus_x"] = -90
    return points, baked, rig


def floor_report(B, rig, baked):
    """How far below the floor each clip dips (rigid skinning by the heaviest bone), and which bone does it."""
    pos = np.array(B.pos)
    bone = [max(w, key=lambda e: e[1])[0] for w in B.weights]
    names = sorted(set(bone))
    idx = {n: np.array([i for i, b in enumerate(bone) if b == n]) for n in names}
    rep = {}
    for name, worlds in baked.items():
        worst = {}
        for f, w in enumerate(worlds):
            for n in names:
                p, R, sc = w[n]
                z = float((((pos[idx[n]] - rig.rest[n]) * sc) @ R[2] + p[2]).min())
                if z < worst.get(n, (-0.04, 0))[0]:
                    worst[n] = (z, f / FPS)
        rep[name] = sorted(((z, n, t) for n, (z, t) in worst.items()))[:4]
    return rep


def top_of(B, rig, world):
    """Highest point of the body (not the spire) in a pose."""
    pos = np.array(B.pos)
    top = 0.0
    for i, w in enumerate(B.weights):
        n = max(w, key=lambda e: e[1])[0]
        if n.startswith("shard_"):
            continue
        p, R, sc = world[n]
        top = max(top, float(((pos[i] - rig.rest[n]) * sc) @ R[2] + p[2]))
    return top
