"""Sound effects for Aetherbound: one function per id, each layered as
transient + body + tail. Small interface sounds are mono, big ones stereo.
`EFFECTS` maps ids to functions returning a finished array (already levelled:
see `fin`, peak targets grade the effects against each other).
"""
import numpy as np
from synth import (SR, TAU, hz, db, tt, lp, hp, bp, reson, saw, pulse, noise, stereo, fade_in, fade_out, pan_gains,
                   morph_lp, morph_bp, reverb, fm_bell, glass_pad, harp, cymbal, tamtam, brass, lufs)


def R(seed):
    return np.random.default_rng(seed)


def env(n, tau, attack=0.0008):
    t = tt(n)
    return np.minimum(1.0, t / attack) * np.exp(-t / tau)


def thump(f0, f1, dur, tau_f=0.03, tau_a=0.12, drive=1.0):
    """Pitch-dropping sine: the body of an impact."""
    n = int(dur * SR)
    t = tt(n)
    f = f1 + (f0 - f1) * np.exp(-t / tau_f)
    y = np.sin(TAU * np.cumsum(f) / SR) * np.exp(-t / tau_a)
    if drive > 1.0:
        y = np.tanh(y * drive) / np.tanh(drive)
    y[:24] *= fade_in(24)
    return y


def nburst(rng, dur, f_lo, f_hi, tau, attack=0.0005, order=1):
    n = int(dur * SR)
    return bp(noise(n, rng), f_lo, f_hi, order) * env(n, tau, attack)


def whoosh(rng, dur, f_a, f_b, peak=0.5, q=2.2, sharp=2.0):
    """Noise through a sweeping band-pass with a bell-shaped level."""
    n = int(dur * SR)
    x = np.arange(n) / n
    pos = x if f_b >= f_a else 1.0 - x
    amp = np.where(x < peak, (x / max(peak, 1e-3)) ** sharp, ((1 - x) / max(1 - peak, 1e-3)) ** sharp)
    return morph_bp(noise(n, rng), pos, min(f_a, f_b), max(f_a, f_b), q=q, steps=9) * amp * 3.0


def impact(rng, fb=60.0, weight=1.0, crack=1.0, dur=0.6, drive=2.0):
    """A blow in four layers: a pitch-dropping thump at `fb`, a knock an
    octave or two above it, a broadband crunch and a crack. `weight` lengthens
    all of them. Mono."""
    y = np.zeros(int(dur * SR))
    put(y, thump(fb * 2.6, fb, dur, 0.02 + 0.03 * weight, 0.05 + 0.13 * weight, drive), 0.0, 0.5)
    put(y, nburst(rng, dur, fb * 1.6, fb * 7.0, 0.035 + 0.07 * weight, order=2), 0.0, 4.5)
    put(y, nburst(rng, dur, 350, 3200, 0.022 + 0.05 * weight), 0.0, 3.2)
    put(y, nburst(rng, min(dur, 0.12), 1800, 9000, 0.004 + 0.006 * weight), 0.0, 2.6 * crack)
    return y


def modal(rng, freqs, amps, decays, dur):
    n = int(dur * SR)
    t = tt(n)
    y = np.zeros(n)
    for f, a, d in zip(freqs, amps, decays):
        if f < 0.45 * SR:
            y += a * np.sin(TAU * f * t + rng.uniform(0, TAU)) * np.exp(-t / d)
    y[:16] *= fade_in(16)
    return y


_METAL = np.array([1.0, 1.47, 2.09, 2.76, 3.41, 4.2, 5.4, 6.3, 7.8, 8.93, 11.2, 13.3])


def metal(rng, f, dur, ring=1.0, bright=1.0):
    """Struck steel: inharmonic partials, upper ones dying sooner."""
    i = np.arange(len(_METAL))
    amps = rng.uniform(0.5, 1.0, len(i)) / (1 + 0.3 * i / bright)
    decs = ring * dur * 0.3 / (1 + 0.3 * i)
    return modal(rng, f * _METAL * rng.uniform(0.985, 1.015, len(i)), amps, decs, dur)


def grains(rng, dur, count, f_lo, f_hi, tau=(0.02, 0.12), when=1.0, pitch=0.0, amp=(0.3, 1.0)):
    """A scatter of tiny glass pings (stereo). `when` skews them late (>1)
    or early (<1); `pitch` >0 makes later ones higher, <0 lower."""
    n = int(dur * SR)
    out = np.zeros((n, 2))
    for _ in range(count):
        x = rng.uniform(0, 1) ** (1.0 / when)
        f = np.exp(rng.uniform(np.log(f_lo), np.log(f_hi))) * 2.0 ** (pitch * (x - 0.5) * 2)
        d = rng.uniform(*tau)
        m = min(n - int(x * n), int(d * 7 * SR))
        if m < 16 or f > 0.45 * SR:
            continue
        g = np.sin(TAU * f * tt(m) + rng.uniform(0, TAU)) * env(m, d, 0.0006) * rng.uniform(*amp)
        gl, gr = pan_gains(rng.uniform(-0.9, 0.9))
        i = int(x * n)
        out[i:i + m, 0] += g * gl
        out[i:i + m, 1] += g * gr
    return out


def crackle(rng, dur, rate, f_lo, f_hi, tau=0.004, shape=None):
    """Random filtered ticks (fire, debris, arcs). `shape(x)` scales the
    density/level over the normalised duration. Mono."""
    n = int(dur * SR)
    y = np.zeros(n)
    k = int(rate * dur)
    for _ in range(k):
        x = rng.uniform(0, 1)
        a = rng.uniform(0.2, 1.0) ** 2 * (shape(x) if shape else 1.0)
        m = int(rng.uniform(0.5, 2.0) * tau * 6 * SR)
        i = int(x * n)
        m = min(m, n - i)
        if m < 8:
            continue
        y[i:i + m] += noise(m, rng) * env(m, tau * rng.uniform(0.5, 2.0), 0.0003) * a
    return bp(y, f_lo, f_hi, 1)


def put(dst, src, t, gain=1.0, pan=None):
    """Add src into dst at t seconds (dst stereo or mono)."""
    i = int(t * SR)
    if dst.ndim == 2 and src.ndim == 1:
        src = stereo(src, 0.0 if pan is None else pan)
    elif dst.ndim == 1 and src.ndim == 2:
        src = src.mean(axis=1)
    elif dst.ndim == 2 and pan:
        src = src * np.array([min(1.0, 1.0 - pan), min(1.0, 1.0 + pan)])[None, :]
    m = min(len(src), len(dst) - i)
    if m > 0:
        dst[i:i + m] += src[:m] * gain
    return dst


def buf(dur, ch=2):
    return np.zeros((int(dur * SR), ch)) if ch == 2 else np.zeros(int(dur * SR))


def fin(y, peak_db=-3.0, fade=0.02, dur=None, hp_hz=32.0, sat=0.0):
    """Trim to `dur`, remove DC/rumble, fade the end, level to a peak."""
    if dur is not None:
        n = int(round(dur * SR))
        if len(y) < n:
            pad = np.zeros((n - len(y),) + y.shape[1:])
            y = np.concatenate([y, pad])
        y = y[:n]
    y = hp(y, hp_hz, 2)
    if sat > 0:
        y = y / (np.abs(y).max() + 1e-12)
        y = np.tanh(y * (1 + 3 * sat)) / np.tanh(1 + 3 * sat)
    nf = min(len(y), int(fade * SR))
    w = fade_out(nf)
    y[-nf:] *= w[:, None] if y.ndim == 2 else w
    na = min(len(y), 12)
    w = fade_in(na)
    y[:na] *= w[:, None] if y.ndim == 2 else w
    return y * (db(peak_db) / (np.abs(y).max() + 1e-12))


def bell(f, dur, vel=0.5, decay=0.3, index=2.0, ratio=3.5, seed=0, strike=1.0):
    return fm_bell(f, dur, vel, ratio=ratio, index=index, decay=decay, seed=seed, strike=strike)


# -------------------------------------------------------------- interface

def cursor():
    n = int(0.08 * SR)
    t = tt(n)
    y = np.sin(TAU * 1568 * t) * env(n, 0.016) + 0.4 * np.sin(TAU * 2349 * t) * env(n, 0.01)
    y += nburst(R(1), 0.08, 2000, 7000, 0.002) * 0.35
    return fin(y, -12, 0.01)


def confirm():
    y = buf(0.45)
    put(y, bell(hz('A5'), 0.3, 0.5, decay=0.1, index=1.6), 0.0)
    put(y, bell(hz('E6'), 0.38, 0.55, decay=0.15, index=1.6, seed=1), 0.07)
    put(y, grains(R(2), 0.3, 6, 4000, 8000, (0.01, 0.03), amp=(0.03, 0.08)), 0.08)
    return fin(y, -9, 0.05)


def cancel():
    y = buf(0.32)
    put(y, bell(hz('E5'), 0.2, 0.5, decay=0.07, index=1.2), 0.0)
    put(y, bell(hz('A4'), 0.26, 0.5, decay=0.1, index=1.2, seed=1), 0.06)
    return fin(lp(y, 2600, 1), -10, 0.04)


def buzzer():
    n = int(0.36 * SR)
    t = tt(n)
    y = saw(np.full(n, 98.0)) + saw(np.full(n, 104.5), 0.3) + 0.5 * pulse(np.full(n, 196.0), 0.4)
    gate = ((t < 0.13) | ((t > 0.17) & (t < 0.34))).astype(float)
    gate = lp(gate, 150, 1)
    y = lp(y, 1100, 2) * gate
    return fin(y, -8, 0.01)


def text():
    n = int(0.035 * SR)
    t = tt(n)
    y = nburst(R(3), 0.035, 1500, 5000, 0.003) * 0.6 + np.sin(TAU * 1200 * t) * env(n, 0.006) * 0.6
    return fin(y, -15, 0.008)


def text_system():
    n = int(0.045 * SR)
    t = tt(n)
    y = pulse(np.full(n, 2637.0), 0.5) * (t < 0.012) + pulse(np.full(n, 1760.0), 0.25) * ((t >= 0.012) & (t < 0.022))
    y = y * env(n, 0.012, 0.0003) + nburst(R(4), 0.045, 3000, 9000, 0.0015) * 0.5
    y = np.round(y * 12) / 12            # a little quantisation grit
    return fin(lp(y, 9000, 1), -13, 0.008)


def atb_ready():
    y = buf(0.55)
    put(y, bell(hz('E6'), 0.4, 0.5, decay=0.14, index=1.5), 0.0)
    put(y, bell(hz('A6'), 0.48, 0.55, decay=0.2, index=1.5, seed=1), 0.06)
    return fin(y, -9, 0.06)


def item():
    y = buf(0.65)
    for i, p in enumerate(('D6', 'F#6', 'A6')):
        put(y, harp(hz(p), 0.5, 0.7, seed=i), 0.065 * i, 1.0)
        put(y, bell(hz(p), 0.4, 0.3, decay=0.12, index=1.0, seed=i), 0.065 * i, 0.5)
    put(y, bell(hz('D7'), 0.4, 0.3, decay=0.16, index=1.2, seed=5), 0.2, 0.6)
    return fin(y, -9, 0.06)


# ------------------------------------------------------------------ field

def step():
    rng = R(7)
    y = buf(0.2, 1)
    put(y, impact(rng, 95.0, 0.05, 0.6, 0.18, 1.0), 0.0)
    put(y, nburst(rng, 0.12, 1500, 6000, 0.03, 0.01) * 0.12, 0.025)      # grit under the sole
    return fin(lp(y, 6500, 1), -9, 0.02, sat=0.3)


# ----------------------------------------------------------------- battle

def slash():
    rng = R(8)
    y = buf(0.4)
    put(y, whoosh(rng, 0.2, 900, 5200, 0.55), 0.0, 0.6, -0.2)
    put(y, whoosh(rng, 0.16, 2500, 9000, 0.6, q=1.5), 0.03, 0.3, 0.3)
    put(y, nburst(rng, 0.05, 3000, 10000, 0.007), 0.105, 0.7)
    put(y, metal(rng, 3100, 0.28, ring=1.0), 0.1, 0.16, 0.1)
    return fin(y, -5, 0.04)


def slash_heavy():
    rng = R(9)
    y = buf(0.62)
    put(y, whoosh(rng, 0.32, 500, 3800, 0.62), 0.0, 0.7, -0.25)
    put(y, whoosh(rng, 0.3, 180, 1200, 0.65, q=1.6), 0.0, 0.7, 0.1)
    put(y, nburst(rng, 0.06, 2500, 9000, 0.009), 0.2, 0.8)
    put(y, metal(rng, 2100, 0.42, ring=1.1), 0.2, 0.22, 0.2)
    put(y, impact(rng, 70.0, 0.4, 0.5, 0.3, 2.0), 0.2, 0.45)
    return fin(y, -4, 0.05)


def hit():
    rng = R(10)
    y = buf(0.3, 1)
    put(y, impact(rng, 82.0, 0.25, 1.0, 0.3, 1.6), 0.0)
    return fin(y, -4, 0.03, sat=0.6)


def hit_heavy():
    rng = R(11)
    y = buf(0.5, 1)
    put(y, impact(rng, 60.0, 0.7, 1.0, 0.5, 2.5), 0.0)
    put(y, metal(rng, 700, 0.3, ring=0.8), 0.01, 0.25)
    y = reverb(y, 0.6, 0.14, seed=5)
    return fin(y, -2.5, 0.08, dur=0.75, sat=0.6)


def guard():
    rng = R(12)
    y = buf(0.58)
    put(y, metal(rng, 820, 0.55, ring=1.0), 0.0, 0.6, -0.1)
    put(y, metal(rng, 1340, 0.45, ring=0.9), 0.0, 0.3, 0.2)
    put(y, thump(170, 90, 0.2, 0.02, 0.04, 1.5), 0.0, 0.6)
    put(y, nburst(rng, 0.03, 2500, 9000, 0.003), 0.0, 0.6)
    return fin(y, -5, 0.06, sat=0.2)


def miss():
    rng = R(13)
    y = buf(0.34)
    put(y, whoosh(rng, 0.32, 3600, 700, 0.35, q=1.5, sharp=1.5), 0.0, 1.0, -0.3)
    put(y, whoosh(rng, 0.28, 5000, 1500, 0.3, q=1.2, sharp=1.5), 0.02, 0.5, 0.3)
    return fin(y, -10, 0.04)


def spell_cast():
    rng = R(14)
    y = buf(1.05)
    n = int(0.85 * SR)
    t = tt(n)
    x = t / 0.85
    for ch, dc in enumerate((0.996, 1.004)):
        f = 380.0 * dc * 4.0 ** x
        tone = np.sin(TAU * np.cumsum(f) / SR) + 0.4 * np.sin(TAU * np.cumsum(f * 1.5) / SR)
        y[:n, ch] += tone * x ** 1.6 * (0.7 + 0.3 * np.sin(TAU * (8 + 14 * x) * t)) * 0.3
    put(y, grains(rng, 0.9, 46, 1500, 4500, (0.015, 0.06), when=2.2, pitch=0.7), 0.0, 0.45)
    b = bell(hz('A5'), 0.8, 0.6, decay=0.5, index=2.5)[::-1]
    put(y, b, 0.05, 0.5)
    put(y, bell(hz('E7'), 0.3, 0.5, decay=0.08, index=1.5, seed=3), 0.85, 0.7)
    put(y, whoosh(rng, 0.85, 800, 6000, 0.9, q=1.5), 0.0, 0.25)
    y[int(0.87 * SR):] *= 0.4
    y = reverb(y, 0.9, 0.25, seed=7)
    return fin(y, -6, 0.1, dur=1.3)


def ember():
    rng = R(15)
    y = buf(1.3)
    put(y, whoosh(rng, 0.25, 120, 1000, 0.75, q=1.5), 0.0, 0.8)
    put(y, impact(rng, 60.0, 0.9, 0.3, 0.6, 2.0), 0.2, 0.55)
    n = int(1.0 * SR)
    t = tt(n)
    x = t / 1.0
    shape = np.minimum(1.0, t / 0.04) * np.exp(-t / 0.33)
    for ch in range(2):
        roar = morph_lp(noise(n, rng), shape ** 0.6, 250.0, 5000.0, steps=5)
        flut = 0.7 + 0.3 * np.sin(TAU * (22 + 6 * ch) * t + ch) * np.sin(TAU * 3.1 * t)
        y[int(0.2 * SR):int(0.2 * SR) + n, ch] += roar * shape * flut * 1.6
    ck = crackle(rng, 1.0, 70, 1500, 8000, 0.003, shape=lambda v: (1 - v) ** 1.2)
    put(y, ck, 0.22, 1.0, -0.3)
    put(y, crackle(rng, 1.0, 50, 1200, 7000, 0.004, shape=lambda v: (1 - v) ** 1.2), 0.25, 1.0, 0.4)
    return fin(y, -4, 0.15)


def frost():
    rng = R(16)
    y = buf(1.0)
    put(y, whoosh(rng, 0.5, 7000, 1500, 0.25, q=1.6), 0.0, 0.5, -0.2)
    put(y, grains(rng, 0.9, 70, 3000, 10000, (0.006, 0.04), when=0.8), 0.05, 0.5)
    for t0, a in ((0.33, 1.0), (0.47, 0.7), (0.55, 0.45)):
        put(y, nburst(rng, 0.04, 2500, 11000, 0.004), t0, a, rng.uniform(-0.5, 0.5))
        put(y, metal(rng, rng.uniform(3000, 4500), 0.12, ring=0.6), t0, 0.12 * a)
    ring = modal(rng, [2093, 2794, 3520, 4186, 5588], [1, 0.7, 0.8, 0.5, 0.3], [0.35, 0.3, 0.3, 0.22, 0.15], 0.8)
    put(y, ring, 0.33, 0.14)
    y = reverb(y, 1.0, 0.25, damp=0.3, seed=9)
    return fin(y, -5, 0.12, dur=1.3)


def mend():
    rng = R(17)
    y = buf(1.2)
    for i, p in enumerate(('D5', 'F#5', 'A5', 'D6', 'F#6')):
        put(y, bell(hz(p), 0.9, 0.45, decay=0.4, index=1.0, strike=0.4, seed=i), 0.09 * i, 0.8, -0.3 + 0.15 * i)
    for p in ('D4', 'A4', 'F#5'):
        put(y, glass_pad(hz(p), 0.5, 0.6, attack=0.3, release=0.5, seed=int(hz(p))), 0.0, 1.6)
    put(y, whoosh(rng, 0.8, 900, 4500, 0.7, q=1.2, sharp=1.5), 0.0, 0.12)
    put(y, grains(rng, 0.8, 18, 3000, 7000, (0.02, 0.06), when=1.5, amp=(0.1, 0.3)), 0.3, 0.4)
    y = reverb(y, 1.3, 0.3, seed=11)
    return fin(y, -7, 0.15, dur=1.6)


def ward():
    rng = R(18)
    y = buf(1.0)
    n = int(0.9 * SR)
    t = tt(n)
    f = 294.0 - 98.0 * np.exp(-t / 0.09)
    for ch, dc in enumerate((0.994, 1.006)):
        tone = np.sin(TAU * np.cumsum(f * dc) / SR) + 0.5 * np.sin(TAU * np.cumsum(f * dc * 2.0) / SR)
        y[:n, ch] += np.tanh(tone * 1.5) * np.minimum(1, t / 0.07) * np.exp(-t / 0.3) * 0.5
    ring = modal(rng, [1174.7, 1760, 2349.3, 3520], [1, 0.8, 0.5, 0.35], [0.5, 0.45, 0.35, 0.25], 0.9)
    put(y, ring * np.minimum(1, tt(len(ring)) / 0.12), 0.05, 0.2)
    put(y, thump(300, 150, 0.12, 0.02, 0.03), 0.27, 0.4)
    put(y, bell(hz('A5'), 0.5, 0.5, decay=0.2, index=1.5), 0.27, 0.5)
    put(y, whoosh(rng, 0.3, 500, 3000, 0.8, q=1.5), 0.0, 0.15)
    y = reverb(y, 1.0, 0.25, seed=13)
    return fin(y, -6, 0.12, dur=1.3)


def enemy_fall():
    rng = R(20)
    y = buf(1.05)
    put(y, whoosh(rng, 0.5, 1800, 200, 0.4, q=1.6), 0.0, 0.6, -0.2)
    put(y, impact(rng, 62.0, 0.5, 0.6, 0.45, 2.0), 0.45, 0.8)
    for i, t0 in enumerate((0.47, 0.56, 0.63, 0.74)):
        put(y, metal(rng, rng.uniform(900, 2400), 0.18, ring=0.8), t0, 0.22 * (1 - 0.18 * i), rng.uniform(-0.5, 0.5))
    put(y, hp(noise(int(0.5 * SR), rng), 4000, 1) * env(int(0.5 * SR), 0.14, 0.05), 0.5, 0.12)
    return fin(y, -5, 0.1, sat=0.3)


def encounter():
    rng = R(22)
    y = buf(1.5)
    n = int(0.45 * SR)
    t = tt(n)
    x = t / 0.45
    rise = hp(noise(n, rng, 2), 1500, 1) * (x ** 3)[:, None] * 0.35
    tone = lp(saw(110.0 * 2.0 ** x) + saw(155.6 * 2.0 ** x, 0.4), 2500, 2) * x ** 2 * 0.3
    y[:n] += rise + tone[:, None]
    t0 = 0.45
    put(y, impact(rng, 52.0, 1.0, 1.0, 0.9, 3.0), t0, 0.8)
    put(y, metal(rng, 2400, 0.9, ring=1.2), t0, 0.14, -0.3)
    put(y, metal(rng, 1790, 0.8, ring=1.1), t0 + 0.02, 0.12, 0.3)
    m = int(0.9 * SR)
    tm = tt(m)
    e = np.minimum(1, tm / 0.01) * np.exp(-tm / 0.28)
    for p, pan in (('D2', 0.2), ('Ab2', -0.2), ('D3', 0.0), ('Ab3', 0.1)):
        st = lp(saw(np.full(m, hz(p)), rng.uniform(0, 1)), 400 + 2600 * 0.7, 2)
        put(y, morph_lp(st, e, 300, 3500, steps=4) * e, t0, 0.3, pan)
    put(y, cymbal(1.0, 0.7, seed=3, dark=0.3), t0, 0.7)
    y = reverb(y, 1.0, 0.2, seed=15)
    return fin(y, -2.5, 0.15, dur=1.9, sat=0.35)


# ------------------------------------------------------------------ world

def _blast(rng, dur, f_hi, tau, close=1.0):
    """Explosion body: noise whose low-pass closes as it decays."""
    n = int(dur * SR)
    t = tt(n)
    e = np.minimum(1.0, t / 0.003) * (0.7 * np.exp(-t / tau) + 0.3 * np.exp(-t / (tau * 3.5)))
    out = np.zeros((n, 2))
    for ch in range(2):
        out[:, ch] = morph_lp(noise(n, rng), np.exp(-t / (tau * 0.8)) * close, 120.0, f_hi, steps=6) * e
    return out


def explosion_far():
    rng = R(23)
    y = buf(2.6)
    put(y, _blast(rng, 2.5, 1100, 0.45), 0.0, 2.0)
    put(y, thump(95, 44, 1.5, 0.15, 0.45, 2.0), 0.0, 0.5)
    n = int(2.4 * SR)
    t = tt(n)
    roll = lp(noise(n, rng, 2), 170, 2) * (np.minimum(1, t / 0.4) * np.exp(-t / 1.0) * (1 + 0.5 * np.sin(TAU * 2.7 * t)))[:, None]
    put(y, roll, 0.1, 3.0)
    y = reverb(lp(y, 1400, 1), 3.0, 0.5, predelay=0.05, damp=0.8, seed=17)
    return fin(y, -6, 0.6, dur=3.8)


def explosion_near():
    rng = R(24)
    y = buf(3.0)
    put(y, impact(rng, 46.0, 2.6, 1.5, 1.8, 3.5), 0.0, 0.8)
    put(y, _blast(rng, 2.2, 9000, 0.38), 0.0, 2.2)
    n = int(2.6 * SR)
    t = tt(n)
    put(y, lp(noise(n, rng, 2), 150, 2) * (np.minimum(1, t / 0.2) * np.exp(-t / 0.9))[:, None], 0.05, 2.0)
    put(y, crackle(rng, 2.4, 45, 900, 6500, 0.005, shape=lambda v: (1 - v) ** 2), 0.25, 0.9, -0.4)
    put(y, crackle(rng, 2.4, 35, 700, 5000, 0.006, shape=lambda v: (1 - v) ** 2), 0.3, 0.9, 0.5)
    y = reverb(y, 2.0, 0.22, seed=19)
    return fin(y, -1.5, 0.5, dur=3.6, sat=0.25)


def rumble():
    rng = R(25)
    n = int(4.0 * SR)
    t = tt(n)
    y = np.zeros((n, 2))
    ov = np.minimum(1, t / 0.5) * np.minimum(1, (4.0 - t) / 1.2)
    for ch in range(2):
        shake = 0.55 + 0.45 * lp(noise(n, rng), 5.0, 2) / 0.012
        shake = np.clip(shake, 0.1, 1.8)
        y[:, ch] += bp(noise(n, rng), 40, 150, 2) * shake * 3.0
        y[:, ch] += bp(noise(n, rng), 150, 600, 1) * (0.4 + 0.6 * np.clip(lp(noise(n, rng), 1.5, 1) / 0.008, -1, 1) ** 2) * 0.35
    put(y, crackle(rng, 3.8, 30, 800, 4500, 0.004), 0.1, 0.5, -0.5)
    put(y, crackle(rng, 3.8, 30, 600, 4000, 0.005), 0.1, 0.5, 0.5)
    for t0, f0 in ((0.6, 46.0), (2.1, 39.0)):
        m = int(1.3 * SR)
        tm = tt(m)
        groan = lp(saw(f0 * (1 + 0.12 * np.sin(TAU * 0.6 * tm) + 0.02 * np.sin(TAU * 7 * tm))), 260, 2)
        put(y, groan * np.sin(np.pi * tm / 1.3) ** 2, t0, 0.5, rng.uniform(-0.4, 0.4))
    y *= ov[:, None]
    return fin(y, -5, 0.05, dur=4.0)


def collapse():
    rng = R(26)
    y = buf(3.2)
    for i, t0 in enumerate((0.0, 0.22, 0.5, 0.68, 1.0, 1.25, 1.62, 2.1)):
        a = (1.0, 0.6, 0.9, 0.5, 1.0, 0.7, 0.8, 0.5)[i]
        pan = rng.uniform(-0.6, 0.6)
        put(y, impact(rng, rng.uniform(50, 78), rng.uniform(0.5, 1.0), 0.7, 0.6, 2.5), t0, 0.7 * a, pan)
    put(y, crackle(rng, 3.0, 160, 500, 5000, 0.004, shape=lambda v: (1 - v) ** 1.3 * (0.3 + 0.7 * (v > 0.02))), 0.05, 0.7, -0.4)
    put(y, crackle(rng, 3.0, 160, 400, 4500, 0.005, shape=lambda v: (1 - v) ** 1.3), 0.08, 0.7, 0.4)
    n = int(3.0 * SR)
    t = tt(n)
    put(y, hp(noise(n, rng, 2), 3000, 1) * (np.minimum(1, t / 0.3) * np.exp(-t / 0.9))[:, None], 0.1, 0.06)
    put(y, lp(noise(n, rng, 2), 140, 2) * (np.minimum(1, t / 0.2) * np.exp(-t / 1.2))[:, None], 0.0, 1.5)
    y = reverb(y, 1.2, 0.18, seed=21)
    return fin(y, -2, 0.4, dur=3.8, sat=0.2)


def crystal_pulse():
    rng = R(27)
    y = buf(1.8)
    n = int(1.8 * SR)
    t = tt(n)
    e = np.minimum(1, t / 0.11) ** 2 * np.exp(-np.clip(t - 0.11, 0, None) / 0.5)
    f = 55.0 * (1 - 0.1 * np.clip(t / 1.2, 0, 1))
    ph = TAU * np.cumsum(f) / SR
    body = np.sin(ph) + 0.9 * np.sin(2 * ph + 0.3) + 0.5 * np.sin(3 * ph + 0.9) + 0.3 * np.sin(4 * ph) + 0.3 * np.sin(1.5 * ph)
    y += (np.tanh(body * 1.2) * e * 0.8)[:, None]
    b = lp(fm_bell(220.0, 1.6, 0.8, ratio=1.41, index=5.0, decay=0.6, strike=0.0), 2400, 2)
    put(y, b * np.minimum(1, tt(len(b)) / 0.1)[:, None], 0.02, 0.8)
    put(y, grains(rng, 1.2, 14, 1800, 5000, (0.03, 0.12), when=0.8, amp=(0.05, 0.2)), 0.12, 0.5)
    y = reverb(y, 2.0, 0.3, seed=23)
    return fin(y, -4, 0.3, dur=2.4)


def eyes_ignite():
    rng = R(28)
    y = buf(1.4)
    n = int(0.36 * SR)
    t = tt(n)
    x = t / 0.36
    f = 180.0 * 10.0 ** x
    whine = np.sin(TAU * np.cumsum(f) / SR + 2.0 * np.sin(TAU * np.cumsum(f * 0.5) / SR)) * x ** 2
    put(y, whine, 0.0, 0.35)
    t0 = 0.36
    put(y, nburst(rng, 0.06, 2000, 11000, 0.008), t0, 1.0, -0.3)
    put(y, nburst(rng, 0.06, 2000, 11000, 0.008), t0 + 0.035, 0.9, 0.3)       # two eyes
    m = int(1.0 * SR)
    tm = tt(m)
    e = np.minimum(1, tm / 0.012) * np.exp(-tm / 0.38)
    for p, pan in (('D2', -0.2), ('Ab2', 0.2)):
        st = saw(np.full(m, hz(p)), rng.uniform(0, 1)) + saw(np.full(m, hz(p) * 1.006), rng.uniform(0, 1))
        put(y, morph_lp(st, e, 200, 2600, steps=4) * e * (0.8 + 0.2 * np.sin(TAU * 9 * tm)), t0, 0.5, pan)
    put(y, _blast(rng, 0.6, 5000, 0.1), t0, 0.5)
    put(y, modal(rng, [880, 1244.5, 1760, 2489], [1, 0.9, 0.5, 0.4], [0.5, 0.45, 0.3, 0.25], 0.9), t0, 0.12)
    put(y, thump(90, 42, 0.5, 0.05, 0.16, 2.0), t0, 0.6)
    y = reverb(y, 1.4, 0.25, seed=25)
    return fin(y, -2.5, 0.2, dur=1.9)


def system_alert():
    n = int(0.42 * SR)
    t = tt(n)
    y = np.zeros(n)
    for i, f in enumerate((1568.0, 2093.0, 1568.0)):
        a = 0.03 + i * 0.085
        seg = (t >= a) & (t < a + 0.06)
        tone = pulse(np.full(n, f), 0.3) + 0.5 * pulse(np.full(n, f * 0.5), 0.5)
        g = lp(seg.astype(float), 400, 1)
        y += tone * g
    y = reson(y, 1200, 3, 1.0) + reson(y, 2600, 4, 0.8) + 0.3 * y
    put(y, nburst(R(29), 0.03, 3000, 9000, 0.004), 0.0, 0.3)
    y = np.round(y / np.abs(y).max() * 20) / 20
    return fin(lp(y, 8000, 1), -8, 0.02)


def colossus_step():
    rng = R(30)
    y = buf(1.2)
    put(y, impact(rng, 47.0, 1.4, 0.6, 1.0, 3.0), 0.0)
    put(y, metal(rng, 180, 0.7, ring=1.0), 0.02, 0.18, 0.3)
    put(y, crackle(rng, 0.8, 40, 700, 4500, 0.005, shape=lambda v: (1 - v) ** 1.5), 0.08, 0.4, -0.3)
    y = reverb(y, 1.4, 0.3, seed=27)
    return fin(y, -2.5, 0.3, dur=1.9, sat=0.45)


def colossus_slam():
    rng = R(31)
    y = buf(2.4)
    put(y, whoosh(rng, 0.18, 250, 1500, 0.95, q=1.5, sharp=2.5), 0.0, 0.5)
    t0 = 0.18
    put(y, impact(rng, 44.0, 2.4, 1.2, 1.8, 4.0), t0, 0.9)
    put(y, _blast(rng, 1.4, 3500, 0.2), t0, 1.6)
    put(y, metal(rng, 262, 1.3, ring=1.0), t0, 0.22, -0.2)
    put(y, metal(rng, 391, 1.0, ring=0.9), t0 + 0.01, 0.14, 0.3)
    put(y, crackle(rng, 1.8, 70, 600, 5500, 0.005, shape=lambda v: (1 - v) ** 1.6), t0 + 0.05, 0.8, -0.4)
    put(y, crackle(rng, 1.8, 60, 500, 4500, 0.006, shape=lambda v: (1 - v) ** 1.6), t0 + 0.08, 0.8, 0.4)
    y = reverb(y, 2.2, 0.3, seed=29)
    return fin(y, -1.5, 0.5, dur=3.2, sat=0.3)


def colossus_sweep():
    rng = R(32)
    y = buf(1.5)
    put(y, whoosh(rng, 0.85, 120, 1100, 0.65, q=1.4), 0.0, 1.0, -0.4)
    put(y, whoosh(rng, 0.8, 500, 3200, 0.7, q=1.6), 0.05, 0.5, 0.4)
    m = int(0.9 * SR)
    tm = tt(m)
    f = 55.0 + 40.0 * np.sin(np.pi * tm / 0.9) ** 2
    groan = lp(saw(f) + saw(f * 1.5, 0.3), 420, 2) * np.sin(np.pi * tm / 0.9) ** 1.5
    put(y, groan, 0.0, 0.6)
    put(y, nburst(rng, 0.4, 200, 3000, 0.1), 0.8, 0.5, 0.5)
    put(y, metal(rng, 330, 0.7, ring=1.0), 0.8, 0.15, 0.5)
    put(y, thump(100, 40, 0.5, 0.05, 0.15, 2.0), 0.8, 0.5)
    y = reverb(y, 1.4, 0.25, seed=31)
    return fin(y, -3, 0.3, dur=2.0)


def aether_charge():
    rng = R(33)
    D = 2.0
    n = int(D * SR)
    t = tt(n)
    x = t / D
    y = np.zeros((n, 2))
    trem = 0.6 + 0.4 * np.sin(TAU * np.cumsum(5.0 + 30.0 * x ** 2) / SR)
    for i, f0 in enumerate((110.0, 164.8, 220.0, 293.7, 440.0, 554.4)):
        f = f0 * 2.0 ** (2.4 * x ** 1.5) * (1 + 0.004 * np.sin(TAU * (3 + i) * t))
        ph = TAU * np.cumsum(f) / SR
        tone = np.sin(ph + (1.0 + 3.0 * x) * np.sin(ph * 1.41))
        gl, gr = pan_gains(-0.6 + 0.24 * i)
        y[:, 0] += tone * gl
        y[:, 1] += tone * gr
    y *= (x ** 1.8 * trem * 0.16)[:, None]
    for ch in range(2):
        y[:, ch] += morph_bp(noise(n, rng), x ** 1.3, 400, 9000, q=1.4, steps=8) * x ** 2.2 * 0.8
    put(y, grains(rng, D, 90, 1500, 5000, (0.01, 0.05), when=2.5, pitch=0.8, amp=(0.1, 0.4)), 0.0, 0.4)
    sub = np.sin(TAU * np.cumsum(45.0 + 35.0 * x) / SR) * x ** 1.5 * 0.18
    y += sub[:, None]
    return fin(y, -3, 0.008, dur=D)


def aether_pulse():
    rng = R(34)
    y = buf(2.4)
    put(y, nburst(rng, 0.06, 2000, 12000, 0.01), 0.0, 1.4)
    fr = np.exp(rng.uniform(np.log(1500), np.log(9000), 22))
    put(y, modal(rng, fr, rng.uniform(0.3, 1.0, 22), rng.uniform(0.08, 0.6, 22), 1.5), 0.0, 0.1, -0.2)
    fr = np.exp(rng.uniform(np.log(1500), np.log(9000), 22))
    put(y, modal(rng, fr, rng.uniform(0.3, 1.0, 22), rng.uniform(0.08, 0.6, 22), 1.5), 0.0, 0.1, 0.2)
    put(y, impact(rng, 48.0, 1.8, 1.0, 1.4, 3.5), 0.0, 0.8)
    put(y, whoosh(rng, 0.8, 9000, 300, 0.04, q=1.3, sharp=1.6), 0.0, 0.7, -0.2)
    put(y, _blast(rng, 1.5, 7000, 0.22), 0.0, 1.6)
    m = int(2.0 * SR)
    tm = tt(m)
    hum_ = (np.sin(TAU * 55 * tm) + 0.6 * np.sin(TAU * 82.6 * tm) + 0.3 * np.sin(TAU * 110.4 * tm)) * np.exp(-tm / 0.7)
    put(y, hum_, 0.05, 0.35)
    put(y, grains(rng, 1.8, 50, 2000, 9000, (0.02, 0.1), when=0.6, pitch=-0.6, amp=(0.1, 0.4)), 0.05, 0.35)
    y = reverb(y, 2.5, 0.3, seed=33)
    return fin(y, -1.5, 0.5, dur=3.4, sat=0.25)


def resonance():
    """Rowan's strike: the motif (A D E F A C) as a run of crystal bells
    over a swelling chord, then the impact."""
    rng = R(35)
    y = buf(3.2)
    for i, p in enumerate(('A5', 'D6', 'E6', 'F6', 'A6', 'C7')):
        put(y, bell(hz(p), 1.2, 0.5 + 0.06 * i, decay=0.45, index=2.2, seed=i), 0.06 + 0.15 * i, 0.55, -0.5 + 0.2 * i)
    for p in ('D4', 'A4', 'E5', 'F5'):
        put(y, glass_pad(hz(p), 0.85, 0.8, attack=0.8, release=0.15, seed=int(hz(p))), 0.0, 1.2)
    put(y, whoosh(rng, 1.05, 600, 8000, 0.97, q=1.3, sharp=2.5), 0.0, 0.5)
    put(y, grains(rng, 1.0, 50, 2000, 6000, (0.01, 0.05), when=2.5, pitch=0.6, amp=(0.1, 0.4)), 0.0, 0.4)
    t0 = 1.05
    put(y, nburst(rng, 0.07, 1500, 12000, 0.012), t0, 1.2)
    put(y, impact(rng, 48.0, 2.0, 1.0, 1.6, 3.5), t0, 0.8)
    put(y, _blast(rng, 1.2, 8000, 0.16), t0, 1.4)
    put(y, grains(rng, 1.6, 170, 2000, 12000, (0.01, 0.25), when=0.45, amp=(0.2, 1.0)), t0, 0.3)
    for i, p in enumerate(('D5', 'A5', 'D6', 'A6', 'E7')):
        put(y, bell(hz(p), 2.1, 0.7, decay=0.9 - 0.1 * i, index=3.0, seed=10 + i), t0, 0.35, -0.4 + 0.2 * i)
    y = reverb(y, 3.0, 0.33, seed=35)
    return fin(y, -1.5, 0.7, dur=4.6, sat=0.2)


def overload_blast():
    rng = R(36)
    y = buf(4.4)
    n = int(1.6 * SR)
    t = tt(n)
    e = np.minimum(1, t / 0.004) * np.where(t < 0.22, 1.0, np.exp(-(t - 0.22) / 0.38))
    for ch in range(2):
        white = morph_lp(hp(noise(n, rng), 120, 1), np.exp(-t / 0.7), 800.0, 16000.0, steps=6)
        y[:n, ch] += white * e * 1.3
    put(y, nburst(rng, 0.1, 1000, 14000, 0.02), 0.0, 1.2)
    put(y, impact(rng, 43.0, 3.2, 1.0, 2.4, 4.0), 0.0, 0.9)
    put(y, impact(rng, 50.0, 2.0, 0.8, 1.5, 3.0), 0.13, 0.6)
    put(y, grains(rng, 4.0, 230, 1800, 12000, (0.015, 0.3), when=0.5, pitch=-0.5, amp=(0.2, 1.0)), 0.15, 0.3)
    m = int(3.6 * SR)
    tm = tt(m)
    ring = (np.sin(TAU * 3520 * tm) + 0.7 * np.sin(TAU * 1760.8 * tm) + 0.4 * np.sin(TAU * 5282 * tm)) * np.minimum(
        1, tm / 0.3) * np.exp(-tm / 1.1)
    put(y, ring, 0.1, 0.07)
    put(y, lp(noise(m, rng, 2), 130, 2) * (np.minimum(1, tm / 0.3) * np.exp(-tm / 1.3))[:, None], 0.05, 1.5)
    put(y, tamtam(60.0, 4.0, 0.9, seed=3), 0.0, 1.2)
    y = reverb(y, 3.5, 0.4, seed=37)
    return fin(y, -1.2, 1.2, dur=6.0, sat=0.3)


def klaxon():
    rng = R(37)
    y = buf(1.95)
    for k in range(2):
        m = int(0.88 * SR)
        tm = tt(m)
        f = 311.1 * (1.0 - 0.2 * np.exp(-tm / 0.05)) * (1 + 0.006 * np.sin(TAU * 6.5 * tm))
        s = saw(f) + 0.8 * saw(f * 1.189, 0.3) + 0.6 * saw(f * 0.5, 0.6) + 0.3 * pulse(f * 2.0, 0.3)
        s = lp(s, 3800, 2)
        s = s + reson(s, 950, 4, 1.4) + reson(s, 1900, 5, 0.9)
        e = np.minimum(1, tm / 0.035) * np.minimum(1, (0.88 - tm) / 0.1)
        put(y, np.tanh(s * 0.8) * e, k * 0.98, 1.0, 0.0)
    y = reverb(y, 1.2, 0.2, seed=39)
    return fin(y, -8.5, 0.2, dur=2.6)


def _zap(rng, dur=0.05, f0=3500.0, f1=300.0):
    n = int(dur * SR)
    x = np.arange(n) / n
    f = f0 * (f1 / f0) ** x
    return (saw(f, rng.uniform(0, 1)) * np.sign(np.sin(TAU * 180 * x * dur * rng.uniform(3, 9)))
            + noise(n, rng) * 0.5) * (1 - x) ** 1.5


def _unbound(seed, dur, base, violence):
    rng = R(seed)
    n = int(dur * SR)
    t = tt(n)
    x = t / dur
    y = np.zeros((n, 2))
    # the charge itself: tones that cannot hold their pitch
    for i, ratio in enumerate((1.0, 1.498, 2.0, 2.12, 2.997)[:3 + int(2 * violence)]):
        wob = lp(noise(n, rng), 10.0 + 12.0 * violence, 2) / 0.012 * (0.02 + 0.05 * violence) * (0.4 + x)
        f = base * ratio * 2.0 ** (0.7 * x ** 2) * (1.0 + np.clip(wob, -0.4, 0.4))
        ph = TAU * np.cumsum(f) / SR
        tone = np.sin(ph + (1.5 + 3.0 * violence * x) * np.sin(ph * 1.41 + i))
        gl, gr = pan_gains(rng.uniform(-0.7, 0.7))
        a = 1.0 / (1 + i)
        y[:, 0] += tone * gl * a
        y[:, 1] += tone * gr * a
    flut = 0.55 + 0.45 * np.clip(lp(noise(n, rng), 18.0, 1) / 0.03, -1, 1)
    y = np.tanh(y * (1.0 + 2.5 * violence)) * ((0.25 + 0.75 * x ** 1.3) * flut * 0.5)[:, None]
    # arcs
    for _ in range(int(8 + 14 * violence)):
        t0 = dur * rng.uniform(0.0, 0.97) ** 0.7
        z = _zap(rng, rng.uniform(0.02, 0.07), rng.uniform(2500, 7000), rng.uniform(200, 800))
        put(y, z, t0, (0.25 + 0.5 * violence) * rng.uniform(0.5, 1.0) * (0.4 + 0.6 * t0 / dur), rng.uniform(-0.8, 0.8))
    for ch in range(2):
        y[:, ch] += morph_bp(noise(n, rng), x, 600, 7000, q=1.3, steps=7) * x ** 2 * (0.5 + 0.8 * violence)
    # heart
    for t0 in np.arange(0.0, dur - 0.3, 0.8 - 0.25 * violence):
        put(y, thump(100, 52, 0.4, 0.03, 0.1, 2.0), t0, 0.6 + 0.3 * violence)
        put(y, thump(90, 48, 0.4, 0.03, 0.09, 2.0), t0 + 0.2, 0.4 + 0.2 * violence)
    growl = lp(saw(np.full(n, 41.2)) * (0.6 + 0.4 * np.sin(TAU * (9 + 6 * violence) * t)), 220, 2)
    y += (growl * x * 0.5 * violence)[:, None]
    put(y, grains(rng, dur, int(30 + 40 * violence), 2000, 8000, (0.01, 0.06), when=2.0, pitch=0.5, amp=(0.1, 0.4)), 0.0, 0.4)
    return y, rng


def unbound_1():
    y, rng = _unbound(38, 1.7, 220.0, 0.35)
    y = reverb(y, 0.9, 0.18, seed=41)
    return fin(y, -4, 0.25, dur=2.1)


def unbound_2():
    y, rng = _unbound(39, 2.2, 164.8, 1.0)
    n = len(y)
    m = int(0.4 * SR)
    xs = np.arange(m) / m
    surge = np.sin(TAU * np.cumsum(300.0 * 12.0 ** xs) / SR) * xs ** 2
    put(y, surge, 2.2 - 0.4, 0.6)
    y[-int(0.004 * SR):] *= fade_out(int(0.004 * SR))[:, None]
    tail = buf(0.9)
    put(tail, impact(rng, 52.0, 1.0, 1.2, 0.8, 3.0), 0.0, 0.9)
    y = np.concatenate([y, tail])
    y = reverb(y, 1.2, 0.2, seed=43)
    return fin(y, -2, 0.3, dur=3.4, sat=0.3)


def last_light():
    """8 s: everything rising into one white tone, which is cut off at
    exactly 7.0 s (start it 7 s before the moment it should fall silent)."""
    rng = R(40)
    D, cut = 8.0, 7.0
    n = int(cut * SR)
    t = tt(n)
    x = t / cut
    y = np.zeros((n, 2))
    target = (880.0, 1760.0, 1760.0, 3520.0, 1760.0, 880.0, 3520.0, 1760.0, 5280.0, 1760.0, 3520.0, 1760.0, 880.0, 1760.0)
    for i, ft in enumerate(target):
        f0 = np.exp(rng.uniform(np.log(45), np.log(700)))
        conv = np.clip(x / 0.92, 0, 1) ** rng.uniform(0.7, 1.6)       # all arrive by 6.4 s
        f = f0 * (ft / f0) ** conv
        ph = TAU * np.cumsum(f) / SR
        tone = np.sin(ph) + 0.35 * (1 - conv) * np.sin(2 * ph) + 0.2 * (1 - conv) * np.sin(3 * ph)
        gl, gr = pan_gains(rng.uniform(-0.8, 0.8) * (1 - conv))       # they also close in to the centre
        y[:, 0] += tone * gl
        y[:, 1] += tone * gr
    trem = 1.0 - 0.3 * (1 - x) * (0.5 + 0.5 * np.sin(TAU * np.cumsum(3.0 + 25.0 * x ** 2) / SR))
    y *= (x ** 1.7 * trem * 0.11)[:, None]
    for ch in range(2):
        white = morph_lp(noise(n, rng), x ** 1.2, 300.0, 18000.0, steps=7)
        y[:, ch] += hp(white, 150, 1) * x ** 3.0 * 0.5
        y[:, ch] += bp(noise(n, rng), 28, 100, 2) * np.sin(np.pi * np.clip(x / 0.8, 0, 1)) ** 2 * 1.2
    put(y, grains(rng, cut, 260, 1500, 9000, (0.01, 0.06), when=2.2, pitch=0.7, amp=(0.05, 0.3)), 0.0, 0.5)
    y *= np.minimum(1.0, t / 0.6)[:, None]
    nf = int(0.003 * SR)
    y[-nf:] *= fade_out(nf)[:, None]                                   # the cut
    y = hp(y, 25, 2)
    y = np.tanh(y / np.abs(y).max() * 1.6) / np.tanh(1.6)
    y = y * db(-1.5) / np.abs(y).max()
    y[-nf:] *= fade_out(nf)[:, None]
    return np.concatenate([y, np.zeros((int(D * SR) - n, 2))])


def aether_wall():
    rng = R(41)
    y = buf(1.2)
    put(y, _zap(rng, 0.13, 2800, 140), 0.0, 0.8, -0.2)
    put(y, _zap(rng, 0.1, 5200, 400), 0.01, 0.5, 0.3)
    put(y, impact(rng, 52.0, 1.3, 1.0, 0.9, 3.0), 0.0, 0.9)
    put(y, _blast(rng, 0.7, 6000, 0.1), 0.0, 1.2)
    put(y, crackle(rng, 0.5, 90, 2000, 9000, 0.002, shape=lambda v: (1 - v) ** 2), 0.02, 0.6, 0.2)
    put(y, modal(rng, [880, 1244.5, 1865, 2637], [1, 0.8, 0.6, 0.4], [0.4, 0.35, 0.25, 0.2], 0.8), 0.0, 0.08)
    put(y, whoosh(rng, 0.55, 1600, 300, 0.25, q=1.5), 0.12, 0.6, 0.5)       # the body going back through the air
    y = reverb(y, 1.2, 0.25, seed=45)
    return fin(y, -2.5, 0.3, dur=1.8, sat=0.2)


def sword_fall():
    rng = R(42)
    y = buf(2.0)
    ratios = np.array([1.0, 1.63, 2.41, 3.17, 4.52, 5.9, 7.31, 9.4])
    f0 = 1150.0
    for i, (t0, a) in enumerate(((0.0, 1.0), (0.2, 0.55), (0.34, 0.32), (0.43, 0.2), (0.49, 0.12), (0.53, 0.07))):
        amps = rng.uniform(0.4, 1.0, len(ratios)) / (1 + 0.25 * np.arange(len(ratios)))
        ring = 1.0 if i == 0 else 0.7
        decs = ring * np.array([0.55, 0.45, 0.4, 0.3, 0.22, 0.16, 0.1, 0.07])
        r = modal(rng, f0 * ratios * (1 + 0.004 * i), amps, decs, 1.9 - t0)
        put(y, r, t0, 0.3 * a, -0.15 + 0.08 * i)
        put(y, nburst(rng, 0.03, 2500, 11000, 0.003), t0, 0.7 * a)
        put(y, nburst(rng, 0.06, 500, 2500, 0.012), t0, 0.5 * a)            # the stone under it
        put(y, thump(210, 120, 0.08, 0.01, 0.02), t0, 0.35 * a)
    # the blade chattering flat before it lies still
    n = len(y)
    t = tt(n)
    chat = 1.0 - 0.5 * np.clip((t - 0.5) / 0.1, 0, 1) * np.clip((0.95 - t) / 0.3, 0, 1) * (
        0.5 + 0.5 * np.sign(np.sin(TAU * np.cumsum(30.0 + 50.0 * np.clip(t - 0.5, 0, 1)) / SR)))
    y *= chat[:, None]
    y = reverb(y, 1.6, 0.22, damp=0.4, seed=47)
    return fin(y, -3, 0.5, dur=2.6)


def star():
    rng = R(43)
    y = buf(0.9)
    put(y, bell(hz('A7'), 0.8, 0.5, decay=0.3, index=1.2, strike=0.6), 0.0, 1.0, -0.1)
    put(y, bell(hz('E8'), 0.6, 0.4, decay=0.2, index=0.8, strike=0.3, seed=2), 0.045, 0.5, 0.2)
    put(y, grains(rng, 0.5, 7, 5000, 9500, (0.01, 0.04), amp=(0.05, 0.2)), 0.02, 0.5)
    y = reverb(y, 2.2, 0.5, damp=0.3, seed=49)
    return fin(y, -9, 0.6, dur=2.4)


EFFECTS = {
    'cursor': cursor, 'confirm': confirm, 'cancel': cancel, 'buzzer': buzzer, 'text': text,
    'text_system': text_system, 'step': step, 'slash': slash, 'slash_heavy': slash_heavy, 'hit': hit,
    'hit_heavy': hit_heavy, 'guard': guard, 'miss': miss, 'spell_cast': spell_cast, 'ember': ember, 'frost': frost,
    'mend': mend, 'ward': ward, 'item': item, 'enemy_fall': enemy_fall, 'atb_ready': atb_ready,
    'encounter': encounter, 'explosion_far': explosion_far, 'explosion_near': explosion_near, 'rumble': rumble,
    'collapse': collapse, 'crystal_pulse': crystal_pulse, 'eyes_ignite': eyes_ignite, 'system_alert': system_alert,
    'colossus_step': colossus_step, 'colossus_slam': colossus_slam, 'colossus_sweep': colossus_sweep,
    'aether_charge': aether_charge, 'aether_pulse': aether_pulse, 'resonance': resonance,
    'overload_blast': overload_blast, 'klaxon': klaxon, 'unbound_1': unbound_1, 'unbound_2': unbound_2,
    'last_light': last_light, 'aether_wall': aether_wall, 'sword_fall': sword_fall, 'star': star,
}
