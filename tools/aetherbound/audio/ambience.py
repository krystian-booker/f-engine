"""Looping ambiences: war, wind, hum.

The beds are built to be periodic (noise synthesised in the frequency domain,
tones and LFOs with whole numbers of cycles per loop, circular reverb), and
one-shot events go through a wrapping `Song`, so every file loops as a whole
without a seam.
"""
import numpy as np
from synth import (SR, TAU, Song, make_ir, circ_reverb, periodic_noise, tt, hz, db, lp, hp, bp, reson, saw, noise,
                   stereo, fade_in, fade_out, pan_gains, brass, anvil, master, _formants)


def _lfo(n, rng, cycles=(1, 2, 3, 5), lo=0.0, hi=1.0, sharp=1.0):
    """Smooth periodic control signal in lo..hi with whole cycles per loop."""
    x = np.arange(n) / n
    y = np.zeros(n)
    for c in cycles:
        y += rng.uniform(0.5, 1.0) / np.sqrt(c) * np.sin(TAU * (c * x + rng.uniform(0, 1)))
    y = (y - y.min()) / (y.max() - y.min())
    return lo + (hi - lo) * y ** sharp


def _whistle(n, rng, f0, depth, width, cycles):
    """Narrow band of noise whose centre wanders (periodically): the wind
    finding an edge."""
    base = periodic_noise(n, rng, 0.5, width) + 1j * periodic_noise(n, rng, 0.5, width)
    fc = f0 * 2.0 ** (depth * (_lfo(n, rng, cycles) - 0.5) * 2.0)
    cyc = np.sum(fc) / SR
    fc *= np.round(cyc) / cyc                      # whole number of carrier cycles per loop
    ph = TAU * np.cumsum(fc) / SR
    return np.real(base * np.exp(1j * ph))


def _wind_bed(L, rng, strength=1.0, dark=0.0):
    n = int(L * SR)
    out = np.zeros((n, 2))
    gust = _lfo(n, rng, (1, 2, 3, 5, 8), 0.25, 1.0, 1.3)
    gust2 = _lfo(n, rng, (2, 3, 7, 11), 0.2, 1.0, 1.6)
    common = periodic_noise(n, rng, 90, 2200, slope=-4.0)
    for ch in range(2):
        own = periodic_noise(n, rng, 90, 2200, slope=-4.0)
        body = (0.6 * common + 0.6 * own) * gust
        hiss = periodic_noise(n, rng, 1800, 9000 - 3000 * dark, slope=-3.0) * gust ** 2 * 0.16 * (1 - 0.5 * dark)
        mid = periodic_noise(n, rng, 300, 3600, slope=-3.0) * gust2 * 0.4
        low = periodic_noise(n, rng, 30, 140, slope=0.0) * (0.5 + 0.5 * gust2) * 0.18
        out[:, ch] = body + hiss + mid + low
    for f0, depth, width, cyc, a in ((620.0, 0.35, 45.0, (2, 3, 5), 0.5), (1350.0, 0.3, 70.0, (1, 3, 4, 7), 0.3),
                                     (2900.0, 0.25, 110.0, (2, 5, 9), 0.12 * (1 - dark))):
        w = _whistle(n, rng, f0, depth, width, cyc) * _lfo(n, rng, (1, 2, 4, 7), 0.0, 1.0, 2.5) * a
        gl, gr = pan_gains(rng.uniform(-0.7, 0.7))
        out[:, 0] += w * gl
        out[:, 1] += w * gr
    return out * strength * 0.1


def _hum_bed(L, rng, level=1.0):
    """The crystal: low tones a hair apart (so they beat slowly), a faint
    tritone that should not be there, and glass far above. All frequencies
    are whole cycles per loop."""
    n = int(L * SR)
    t = tt(n)
    q = 1.0 / L
    out = np.zeros((n, 2))

    def fq(f):
        return np.round(f / q) * q

    for f, a, split in ((55.0, 1.0, 0.25), (82.5, 0.6, 0.2), (110.0, 0.6, 0.3), (77.78, 0.2, 0.15),
                        (165.0, 0.42, 0.35), (220.3, 0.3, 0.4), (311.1, 0.12, 0.3), (330.0, 0.16, 0.45),
                        (440.2, 0.08, 0.5)):
        for ch in range(2):
            f1 = fq(f) + ch * q
            f2 = fq(f + split) + ch * q
            out[:, ch] += a * (np.sin(TAU * f1 * t + rng.uniform(0, TAU)) + 0.8 * np.sin(TAU * f2 * t + rng.uniform(0, TAU)))
    throb = 0.72 + 0.28 * np.sin(TAU * fq(0.15) * t)
    out *= throb[:, None]
    out = np.tanh(out * 0.8) / 0.8
    for f, a in ((880.0, 0.09), (1244.5, 0.05), (1320.0, 0.06), (1760.0, 0.035), (2489.0, 0.02), (3520.0, 0.012)):
        for ch in range(2):
            l = 0.5 + 0.5 * np.sin(TAU * fq(rng.uniform(0.1, 0.4)) * t + rng.uniform(0, TAU))
            out[:, ch] += a * l ** 2 * np.sin(TAU * (fq(f) + ch * q * 3) * t + rng.uniform(0, TAU))
    for ch in range(2):
        out[:, ch] += periodic_noise(n, rng, 150, 2500, slope=-5.0) * 0.03 * (0.5 + 0.5 * np.sin(TAU * fq(0.1) * t + ch))
    return out * 0.2 * level


def hum():
    """20 s loop: the crystal's low unnatural hum, beating slowly."""
    rng = np.random.default_rng(101)
    y = _hum_bed(20.0, rng)
    y += 0.35 * circ_reverb(y, make_ir(3.0, 0.02, 0.5, 41))
    return {'audio': master(y, target_lufs=-23.0, comp=False, loop=True, hp_hz=20.0), 'loop': True,
            'loop_start': 0.0, 'loop_end': 20.0}


def _pebble(rng):
    n = int(rng.uniform(0.01, 0.04) * SR)
    f = rng.uniform(1500, 5000)
    return bp(noise(n, rng), f * 0.7, f * 1.4, 1) * np.exp(-tt(n) / rng.uniform(0.003, 0.01))


def wind():
    """30 s loop: cold wind over rubble; gusts, a thin whistle, now and then
    a trickle of grit."""
    L = 30.0
    rng = np.random.default_rng(202)
    y = _wind_bed(L, rng, 1.0)
    s = Song(L, tail=4.0, reverbs={'hall': make_ir(1.8, 0.01, 0.6, 43)}, wrap=True)
    for _ in range(9):                      # trickles of grit
        t0 = rng.uniform(0, L)
        pan = rng.uniform(-0.8, 0.8)
        k = rng.integers(3, 14)
        for i in range(k):
            s.add(_pebble(rng) * rng.uniform(0.2, 1.0) * 0.05, t0 + i * rng.uniform(0.02, 0.12) + rng.uniform(0, 0.05),
                  1.0, pan + rng.uniform(-0.1, 0.1), 0.5, 'grit')
    y = y + s.mix(loop=True) + 0.25 * circ_reverb(y, make_ir(2.2, 0.01, 0.6, 45))
    return {'audio': master(y, target_lufs=-23.0, comp=False, loop=True), 'loop': True, 'loop_start': 0.0,
            'loop_end': L}


def _boom(rng, near=0.0):
    """A distant explosion: the low end of a blast and its roll."""
    n = int(4.5 * SR)
    t = tt(n)
    nz = noise(n, rng)
    body = lp(nz, 180 + 500 * near, 2) * (np.exp(-t / 0.5) + 0.35 * np.exp(-t / 1.6))
    roll = lp(noise(n, rng), 120, 2) * np.minimum(1, t / 0.3) * np.exp(-t / 1.4) * (1 + 0.5 * np.sin(TAU * 3.1 * t))
    sub = np.sin(TAU * np.cumsum(38.0 + 30.0 * np.exp(-t / 0.15)) / SR) * np.exp(-t / 0.6)
    crack = bp(noise(n, rng), 400, 2500, 1) * np.exp(-t / 0.05) * near
    y = body * 3.0 + roll * 2.0 + sub * 0.8 + crack * 0.6
    y[:200] *= fade_in(200)
    y[-4000:] *= fade_out(4000)
    return y * 0.25


def _clang(rng):
    """Steel on steel, far off."""
    f = rng.uniform(900, 2600)
    y = anvil(f, rng.uniform(0.5, 1.0), rng.uniform(0.25, 0.5), seed=int(f))
    return lp(hp(y, 700, 2), 5500, 2)


def _shout(rng, many=1):
    """A voice (or a line of them) too far away to make out."""
    dur = rng.uniform(0.5, 1.3) if many == 1 else rng.uniform(1.8, 3.0)
    n = int(dur * SR)
    t = tt(n)
    out = np.zeros(n)
    for v in range(many):
        f0 = rng.uniform(110, 230)
        x = t / dur
        contour = f0 * (1.0 + 0.35 * np.sin(np.pi * np.clip(x * rng.uniform(0.9, 1.3), 0, 1)) - 0.15 * x)
        contour *= 1.0 + 0.01 * np.sin(TAU * rng.uniform(5, 7) * t)
        src = saw(contour, rng.uniform(0, 1)) + noise(n, rng) * 0.25
        src = lp(src, 1800, 1)
        vowel = rng.choice(['ah', 'oh', 'eh'])
        y = _formants(src[:, None], ('t', vowel))[:, 0]
        st, ln = rng.uniform(0, 0.25) * dur * (many > 1), rng.uniform(0.6, 1.0)
        e = np.clip((t - st) / 0.06, 0, 1) * np.clip((st + ln * dur - t) / (0.25 * dur), 0, 1)
        out += y * e ** 1.5
    out = bp(out, 350, 2600, 1)
    return out / (np.abs(out).max() + 1e-9) * 0.2


def war():
    """45 s loop: distant explosions, clashing steel and shouts lost in the
    wind, under the low unnatural hum; once, a horn."""
    L = 45.0
    rng = np.random.default_rng(303)
    bed = _wind_bed(L, rng, 0.8, dark=0.3) + _hum_bed(L, rng, 0.3)
    far = make_ir(4.2, 0.06, 0.75, 47)
    s = Song(L, tail=9.0, reverbs={'far': far}, wrap=True)
    # explosions: irregular, a pair close together, one nearer
    for t0, near, g, pan in ((2.5, 0.0, 0.7, -0.5), (9.8, 0.15, 0.9, 0.4), (11.1, 0.0, 0.55, 0.6), (18.9, 0.6, 1.25, -0.2),
                             (26.0, 0.0, 0.6, 0.7), (31.7, 0.2, 0.85, -0.7), (33.0, 0.1, 0.7, -0.4),
                             (40.6, 0.0, 0.65, 0.2)):
        s.add(_boom(rng, near), t0, g * 1.3, pan, 0.6, 'explosions', bus='far')
    # steel: little flurries
    for t0 in (4.6, 7.3, 14.2, 16.0, 21.8, 24.1, 28.9, 35.6, 37.4, 42.9):
        pan = rng.uniform(-0.8, 0.8)
        tt_ = t0
        for i in range(rng.integers(2, 6)):
            s.add(_clang(rng), tt_, rng.uniform(0.18, 0.45), pan + rng.uniform(-0.15, 0.15), 1.6, 'steel', bus='far')
            tt_ += rng.uniform(0.12, 0.5)
    # voices
    for t0, many, g in ((6.1, 1, 0.14), (12.9, 7, 0.2), (15.4, 1, 0.12), (20.3, 1, 0.16), (23.0, 1, 0.1), (29.6, 9, 0.22),
                        (34.4, 1, 0.14), (38.7, 1, 0.12), (43.5, 5, 0.16)):
        s.add(_shout(rng, many), t0, g * 3.2, rng.uniform(-0.8, 0.8), 1.8, 'voices', bus='far')
    # a war horn, once, from the other side of the valley
    for p, t0, d in (('D3', 22.2, 1.1), ('A3', 23.3, 2.4)):
        s.add(lp(brass(hz(p), d, 0.7, voices=2, bright=0.5, attack=0.12, release=0.5, seed=3), 1500, 1), t0, 0.24, 0.5,
              2.0, 'horn', bus='far')
    ev = s.mix(loop=True)
    y = bed + ev + 0.2 * circ_reverb(bed, make_ir(2.5, 0.02, 0.6, 49))
    print(s.report())
    return {'audio': master(y, target_lufs=-22.0, comp=False, loop=True, hp_hz=22.0), 'loop': True,
            'loop_start': 0.0, 'loop_end': L}
