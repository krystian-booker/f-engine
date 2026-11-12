"""Small synthesis library for the Aetherbound score and effects.

Everything is numpy/scipy at 44.1 kHz. Instruments are functions
`inst(freq_hz, seconds, vel, ...)` that return a mono (n,) or stereo (n, 2)
array; `Song` places them on a stereo timeline with a dry path and reverb
sends, and `master` brings a mix to a loudness target under a peak ceiling.
"""
import numpy as np
from scipy import signal, ndimage

SR = 44100
TAU = 2.0 * np.pi

# ----------------------------------------------------------------- pitch

_PC = {'C': 0, 'D': 2, 'E': 4, 'F': 5, 'G': 7, 'A': 9, 'B': 11}


def midi(name):
    """'A4' -> 69, 'Bb3' -> 58, 'F#5' -> 78; numbers pass through."""
    if isinstance(name, (int, float, np.integer, np.floating)):
        return float(name)
    pc = _PC[name[0]]
    i = 1
    while name[i] in '#b':
        pc += 1 if name[i] == '#' else -1
        i += 1
    return 12.0 * (int(name[i:]) + 1) + pc


def hz(m):
    return 440.0 * 2.0 ** ((midi(m) - 69.0) / 12.0)


def db(x):
    return 10.0 ** (x / 20.0)


def to_db(x):
    return 20.0 * np.log10(np.maximum(x, 1e-12))


# --------------------------------------------------------------- filters

def _sos(x, sos):
    return signal.sosfilt(sos, x, axis=0)


def lp(x, fc, order=2):
    fc = min(fc, 0.48 * SR)
    return _sos(x, signal.butter(order, fc / (SR / 2), 'low', output='sos'))


def hp(x, fc, order=2):
    return _sos(x, signal.butter(order, fc / (SR / 2), 'high', output='sos'))


def bp(x, f1, f2, order=2):
    f2 = min(f2, 0.48 * SR)
    return _sos(x, signal.butter(order, [f1 / (SR / 2), f2 / (SR / 2)], 'band', output='sos'))


def _biquad(b, a):
    return np.array([[b[0] / a[0], b[1] / a[0], b[2] / a[0], 1.0, a[1] / a[0], a[2] / a[0]]])


def reson(x, f, q, gain=1.0):
    """Band-pass resonator with unity gain at its peak."""
    f = min(f, 0.45 * SR)
    w = TAU * f / SR
    al = np.sin(w) / (2.0 * q)
    return gain * _sos(x, _biquad([al, 0.0, -al], [1 + al, -2 * np.cos(w), 1 - al]))


def peaking(x, f, q, gain_db):
    a_ = 10 ** (gain_db / 40.0)
    w = TAU * f / SR
    al = np.sin(w) / (2.0 * q)
    return _sos(x, _biquad([1 + al * a_, -2 * np.cos(w), 1 - al * a_],
                           [1 + al / a_, -2 * np.cos(w), 1 - al / a_]))


def shelf(x, f, gain_db, high=True, q=0.707):
    a_ = 10 ** (gain_db / 40.0)
    w = TAU * f / SR
    cs, al = np.cos(w), np.sin(w) / (2.0 * q)
    s = 2 * np.sqrt(a_) * al
    if high:
        b = [a_ * ((a_ + 1) + (a_ - 1) * cs + s), -2 * a_ * ((a_ - 1) + (a_ + 1) * cs),
             a_ * ((a_ + 1) + (a_ - 1) * cs - s)]
        a = [(a_ + 1) - (a_ - 1) * cs + s, 2 * ((a_ - 1) - (a_ + 1) * cs), (a_ + 1) - (a_ - 1) * cs - s]
    else:
        b = [a_ * ((a_ + 1) - (a_ - 1) * cs + s), 2 * a_ * ((a_ - 1) - (a_ + 1) * cs),
             a_ * ((a_ + 1) - (a_ - 1) * cs - s)]
        a = [(a_ + 1) + (a_ - 1) * cs + s, -2 * ((a_ - 1) + (a_ + 1) * cs), (a_ + 1) + (a_ - 1) * cs - s]
    return _sos(x, _biquad(b, a))


def morph_lp(x, pos, f_lo, f_hi, steps=5, order=2):
    """Time-varying low-pass: `pos` (0..1 per sample) moves the cutoff from
    f_lo to f_hi (log scale) by cross-fading a small bank of fixed filters."""
    cuts = np.geomspace(f_lo, f_hi, steps)
    bank = [lp(x, c, order) for c in cuts]
    p = np.clip(pos, 0, 1) * (steps - 1)
    out = np.zeros_like(x)
    for i, y in enumerate(bank):
        w = np.clip(1.0 - np.abs(p - i), 0, 1)
        out += y * (w[:, None] if x.ndim == 2 else w)
    return out


def morph_bp(x, pos, f_lo, f_hi, q=3.0, steps=7):
    """Time-varying band-pass (sweeps), same idea as morph_lp."""
    cuts = np.geomspace(f_lo, f_hi, steps)
    p = np.clip(pos, 0, 1) * (steps - 1)
    out = np.zeros_like(x)
    for i, c in enumerate(cuts):
        w = np.clip(1.0 - np.abs(p - i), 0, 1)
        if w.max() <= 0:
            continue
        out += reson(x, c, q) * (w[:, None] if x.ndim == 2 else w)
    return out


# ----------------------------------------------------------- oscillators

def _blep_saw(ph, dt):
    y = 2.0 * ph - 1.0
    m = ph < dt
    x = ph[m] / dt[m]
    y[m] -= x + x - x * x - 1.0
    m = ph > 1.0 - dt
    x = (ph[m] - 1.0) / dt[m]
    y[m] -= x * x + x + x + 1.0
    return y


def saw(fr, ph0=0.0):
    """Band-limited (polyBLEP) sawtooth; `fr` is Hz per sample, last axis time."""
    dt = np.asarray(fr, dtype=float) / SR
    ph = (np.cumsum(dt, axis=-1) + ph0) % 1.0
    return _blep_saw(ph, dt)


def pulse(fr, width=0.5, ph0=0.0):
    dt = np.asarray(fr, dtype=float) / SR
    ph = (np.cumsum(dt, axis=-1) + ph0) % 1.0
    return _blep_saw(ph, dt) - _blep_saw((ph + width) % 1.0, dt)


def sine(fr, ph0=0.0):
    if np.isscalar(fr):
        raise ValueError('sine needs a frequency array; use tone() for constants')
    return np.sin(TAU * (np.cumsum(fr, axis=-1) / SR + ph0))


def tone(f, n, ph=0.0):
    return np.sin(TAU * (f * np.arange(n) / SR + ph))


def tt(n):
    return np.arange(int(n)) / SR


def noise(n, rng, ch=1):
    return rng.standard_normal(int(n)) if ch == 1 else rng.standard_normal((int(n), ch))


def periodic_noise(n, rng, f_lo, f_hi, slope=0.0):
    """Noise that repeats seamlessly every n samples (built in the frequency
    domain), band-limited to f_lo..f_hi with a spectral tilt in dB/octave."""
    n = int(n)
    freqs = np.fft.rfftfreq(n, 1.0 / SR)
    mag = np.zeros_like(freqs)
    m = (freqs >= f_lo) & (freqs <= f_hi)
    mag[m] = (freqs[m] / max(f_lo, 1.0)) ** (slope / 6.0206)
    # soft band edges
    edge = np.clip((freqs - f_lo) / (0.15 * f_lo + 1), 0, 1) * np.clip((f_hi - freqs) / (0.15 * f_hi), 0, 1)
    mag *= np.clip(edge, 0, 1)
    spec = mag * np.exp(1j * rng.uniform(0, TAU, len(freqs)))
    x = np.fft.irfft(spec, n)
    return x / (np.sqrt(np.mean(x * x)) + 1e-12)


# -------------------------------------------------------------- envelopes

def fade_in(n):
    n = max(int(n), 1)
    return np.sin(0.5 * np.pi * np.arange(n) / n) ** 2


def fade_out(n):
    return fade_in(n)[::-1]


def ar(n, a, r):
    """Raised-cosine attack and release over n samples (a, r in seconds)."""
    e = np.ones(int(n))
    na, nr = min(int(a * SR), n), min(int(r * SR), n)
    if na > 0:
        e[:na] = fade_in(na)
    if nr > 0:
        e[-nr:] *= fade_out(nr)
    return e


def curve(points, n, dur=None):
    """Piecewise-linear envelope from (time_fraction or seconds, value) points."""
    xs = np.array([p[0] for p in points], dtype=float)
    ys = np.array([p[1] for p in points], dtype=float)
    x = np.linspace(0, 1, int(n)) if dur is None else np.arange(int(n)) / SR
    return np.interp(x, xs, ys)


def _swell(swell, n_total, n_dur):
    """swell = (start, end) or (start, mid, end) gain across the note."""
    if swell is None:
        return 1.0
    pts = np.linspace(0, 1, len(swell))
    x = np.minimum(np.arange(n_total) / max(n_dur, 1), 1.0)
    y = np.interp(x, pts, swell)
    return y


def pan_gains(p):
    a = (np.clip(p, -1, 1) + 1) * np.pi / 4
    return np.cos(a), np.sin(a)


def stereo(x, p=0.0):
    gl, gr = pan_gains(p)
    return np.stack([x * gl, x * gr], axis=1)


def _rms(x):
    return float(np.sqrt(np.mean(np.square(x)) + 1e-20))


# ------------------------------------------------------------ instruments

_BODY = {  # main body resonances (Hz, Q, gain) by register
    'violin': ((285, 5, 0.5), (470, 6, 0.4), (980, 4, 0.2), (2700, 2.5, 0.45)),
    'viola': ((225, 5, 0.5), (390, 6, 0.4), (800, 4, 0.2), (2200, 2.5, 0.4)),
    'cello': ((105, 5, 0.5), (185, 5, 0.5), (420, 4, 0.3), (1500, 2.5, 0.35)),
    'bass': ((62, 4, 0.5), (110, 5, 0.5), (240, 4, 0.3), (900, 2.5, 0.3)),
}


def _body_for(f):
    return 'bass' if f < 62 else 'cello' if f < 150 else 'viola' if f < 290 else 'violin'


def strings(f, dur, vel=0.6, attack=0.2, release=0.35, voices=7, spread=0.8, center=0.0,
            detune=10.0, vib=0.005, vib_rate=5.5, vib_delay=0.3, bright=1.0, trem=0.0,
            trem_rate=13.0, swell=None, marc=0.0, glide=None, seed=0, body=None, bow=1.0):
    """A bowed string section: detuned band-limited saws, each with its own
    vibrato, drift and attack, through a bridge roll-off and body resonances,
    plus bow noise. Returns stereo. `trem` 0..1 is bowed tremolo, `marc` an
    accent on the attack, `swell` a gain shape across the note."""
    rng = np.random.default_rng(int(seed) * 7919 + int(f * 16))
    n_dur = int(dur * SR)
    n = n_dur + int(release * SR)
    t = tt(n)[None, :]
    V = voices
    det = 2.0 ** (rng.normal(0, detune, (V, 1)) / 1200.0)
    vr = vib_rate * rng.uniform(0.88, 1.12, (V, 1))
    vph = rng.uniform(0, TAU, (V, 1))
    vd = vib * rng.uniform(0.6, 1.3, (V, 1))
    vfade = np.clip((t - vib_delay * rng.uniform(0.6, 1.5, (V, 1))) / 0.5, 0, 1)
    drift = 0.0012 * np.sin(TAU * rng.uniform(0.15, 0.6, (V, 1)) * t + rng.uniform(0, TAU, (V, 1)))
    fr = f * det * (1.0 + vd * vfade * np.sin(TAU * vr * t + vph) + drift)
    if glide is not None:
        fr = fr * np.interp(np.arange(n), np.linspace(0, n, len(glide)), glide)[None, :]
    s = saw(fr, rng.uniform(0, 1, (V, 1)))
    att = np.maximum(0.003, attack * rng.uniform(0.75, 1.35, (V, 1)))
    e = np.sin(0.5 * np.pi * np.clip(t / att, 0, 1)) ** 2
    if trem > 0:
        tr = np.sin(TAU * trem_rate * rng.uniform(0.85, 1.15, (V, 1)) * t + rng.uniform(0, TAU, (V, 1)))
        e = e * (1.0 - trem * (0.5 + 0.5 * tr) ** 2)
    s *= e
    pan = np.clip(center + spread * np.linspace(-1, 1, V)[rng.permutation(V)] if V > 1 else np.array([center]), -1, 1)
    gl, gr = pan_gains(pan)
    st = np.stack([gl @ s, gr @ s], axis=1) / np.sqrt(V)
    fc = max(2.2 * f, 1300.0 + 3600.0 * bright * vel ** 0.8)
    st = lp(lp(st, fc, 2), fc * 1.7, 2)
    y = st.copy()
    for (bf, q, g) in _BODY[body or _body_for(f)]:
        y += reson(st, bf, q, g)
    y = hp(y, max(25.0, 0.55 * f), 1)
    # level by the sustained part, so registers and dynamics stay comparable
    y *= 0.11 / _rms(y[min(int(0.5 * n), int(0.3 * SR)):max(n_dur, 1)] if n_dur > int(0.4 * SR) else y)
    nz = bp(noise(n, rng, 2), 1800, 6500, 1)
    bow_env = (0.02 + 0.10 * marc * np.exp(-t[0] / 0.05)) * bow * (1.0 - 0.5 * (1 - vel))
    y += nz * (bow_env * np.sin(0.5 * np.pi * np.clip(t[0] / max(attack, 0.01), 0, 1)))[:, None] * 0.11
    g = np.ones(n)
    nr = n - n_dur
    if nr > 0:
        g[n_dur:] = fade_out(nr)
    if marc > 0:
        g *= 1.0 + marc * np.exp(-t[0] / 0.14)
    g = g * _swell(swell, n, n_dur)
    return y * (g * vel ** 1.3)[:, None]


def solo_string(f, dur, vel=0.6, attack=0.1, release=0.22, vib=0.009, vib_rate=5.7,
                vib_delay=0.28, bright=1.0, swell=None, seed=0, **kw):
    """One player: a single voice with a wider, delayed vibrato. Mono."""
    y = strings(f, dur, vel, attack=attack, release=release, voices=1, spread=0.0, detune=1.5,
                vib=vib, vib_rate=vib_rate, vib_delay=vib_delay, bright=bright, swell=swell,
                seed=seed, bow=1.6, **kw)
    return y[:, 0] * 1.25


def brass(f, dur, vel=0.7, attack=0.06, release=0.16, voices=3, bright=1.0, swell=None,
          spread=0.35, center=0.0, seed=0, vib=0.0015):
    """Brass section: saws whose low-pass opens with loudness (dark when soft,
    blaring when loud), a pitch scoop and breath on the attack. Stereo."""
    rng = np.random.default_rng(int(seed) * 104729 + int(f * 16) + 3)
    n_dur = int(dur * SR)
    n = n_dur + int(release * SR)
    t = tt(n)[None, :]
    V = voices
    det = 2.0 ** (rng.normal(0, 4.0, (V, 1)) / 1200.0)
    scoop = 1.0 - 0.012 * rng.uniform(0.5, 1.3, (V, 1)) * np.exp(-t / 0.035)
    wob = 0.0008 * np.sin(TAU * rng.uniform(0.3, 1.1, (V, 1)) * t + rng.uniform(0, TAU, (V, 1)))
    vfade = np.clip((t - 0.5) / 0.6, 0, 1)
    fr = f * det * scoop * (1 + wob + vib * vfade * np.sin(TAU * rng.uniform(4.6, 5.4, (V, 1)) * t))
    s = saw(fr, rng.uniform(0, 1, (V, 1)))
    att = np.maximum(0.004, attack * rng.uniform(0.8, 1.25, (V, 1)))
    s *= np.sin(0.5 * np.pi * np.clip(t / att, 0, 1)) ** 2
    pan = np.clip(center + (spread * np.linspace(-1, 1, V) if V > 1 else np.zeros(1)), -1, 1)
    gl, gr = pan_gains(pan)
    st = np.stack([gl @ s, gr @ s], axis=1) / np.sqrt(V)
    e = np.ones(n)
    e[:min(n, int(attack * SR))] = fade_in(min(n, int(attack * SR)))
    if n > n_dur:
        e[n_dur:] = fade_out(n - n_dur)
    e = e * _swell(swell, n, n_dur)
    b = np.clip(e * vel, 0, 1) ** 1.25
    lo = lp(st, 320.0 + 1.6 * f, 2)
    hi = lp(st, min(900.0 + 4200.0 * bright, max(14.0 * f, 1500.0)), 2)
    y = lo * (1 - b)[:, None] + hi * b[:, None]
    y += reson(y, 520.0 if f < 180 else 900.0, 2.0, 0.35)
    y = hp(y, max(25.0, 0.6 * f), 1)
    y *= 0.13 / _rms(y[int(0.3 * n):max(int(0.8 * n), 1)])
    blat = 1.0 + 0.3 * np.exp(-t[0] / 0.07)
    nz = bp(noise(n, rng, 2), 700, 3500, 1) * (0.05 * np.exp(-t[0] / 0.045))[:, None] * 0.13
    return (y * blat[:, None] + nz) * (e * vel ** 1.2)[:, None]


_VOWELS = {  # (formant Hz, gain dB, bandwidth Hz)
    ('f', 'ah'): ((800, 1150, 2900, 3900, 4950), (0, -6, -32, -20, -50), (80, 90, 120, 130, 140)),
    ('f', 'oh'): ((450, 800, 2830, 3800, 4950), (0, -11, -22, -22, -50), (70, 80, 100, 130, 135)),
    ('f', 'oo'): ((325, 700, 2700, 3800, 4950), (0, -16, -35, -40, -60), (50, 60, 170, 180, 200)),
    ('f', 'eh'): ((400, 1600, 2700, 3300, 4950), (0, -24, -30, -35, -60), (60, 80, 120, 150, 200)),
    ('t', 'ah'): ((650, 1080, 2650, 2900, 3250), (0, -6, -7, -8, -22), (80, 90, 120, 130, 140)),
    ('t', 'oh'): ((400, 800, 2600, 2800, 3000), (0, -10, -12, -12, -26), (40, 80, 100, 120, 120)),
    ('t', 'oo'): ((350, 600, 2700, 2900, 3300), (0, -20, -17, -14, -26), (40, 60, 100, 120, 120)),
    ('t', 'eh'): ((400, 1700, 2600, 3200, 3580), (0, -14, -12, -14, -20), (70, 80, 100, 120, 120)),
    ('b', 'ah'): ((600, 1040, 2250, 2450, 2750), (0, -7, -9, -9, -20), (60, 70, 110, 120, 130)),
    ('b', 'oh'): ((400, 750, 2400, 2600, 2900), (0, -11, -21, -20, -40), (40, 80, 100, 120, 120)),
    ('b', 'oo'): ((350, 600, 2400, 2675, 2950), (0, -20, -32, -28, -36), (40, 80, 100, 120, 120)),
    ('b', 'eh'): ((400, 1620, 2400, 2800, 3100), (0, -12, -9, -12, -18), (40, 80, 100, 120, 120)),
}


def _formants(src, key):
    fs, gs, bws = _VOWELS[key]
    y = np.zeros_like(src)
    for fq, g, bw in zip(fs, gs, bws):
        y += reson(src, fq, fq / (bw * 1.6), db(g))
    return y


def choir(f, dur, vel=0.6, attack=0.5, release=0.7, voices=6, vowel='ah', vowel2=None,
          spread=0.8, center=0.0, seed=0, swell=None, breath=1.0, glide=None):
    """Choir pad: a detuned glottal-like source through vowel formants.
    `vowel2` morphs the vowel across the note. Stereo."""
    rng = np.random.default_rng(int(seed) * 31337 + int(f * 16) + 11)
    n_dur = int(dur * SR)
    n = n_dur + int(release * SR)
    t = tt(n)[None, :]
    V = voices
    sex = 'b' if f < 150 else 't' if f < 262 else 'f'
    det = 2.0 ** (rng.normal(0, 13.0, (V, 1)) / 1200.0)
    vfade = np.clip((t - rng.uniform(0.15, 0.6, (V, 1))) / 0.6, 0, 1)
    vibr = 0.007 * rng.uniform(0.6, 1.3, (V, 1)) * vfade * np.sin(
        TAU * rng.uniform(4.6, 5.8, (V, 1)) * t + rng.uniform(0, TAU, (V, 1)))
    jit = lp(rng.standard_normal((V, n)).T, 12.0, 1).T * 0.02
    fr = f * det * (1.0 + vibr + jit)
    if glide is not None:
        fr = fr * np.interp(np.arange(n), np.linspace(0, n, len(glide)), glide)[None, :]
    s = saw(fr, rng.uniform(0, 1, (V, 1)))
    att = np.maximum(0.02, attack * rng.uniform(0.7, 1.4, (V, 1)))
    s *= np.sin(0.5 * np.pi * np.clip(t / att, 0, 1)) ** 2
    pan = np.clip(center + spread * np.linspace(-1, 1, V)[rng.permutation(V)], -1, 1)
    gl, gr = pan_gains(pan)
    src = np.stack([gl @ s, gr @ s], axis=1) / np.sqrt(V)
    src = lp(src, 900.0 + 1400.0 * vel, 1)
    src = src + noise(n, rng, 2) * (0.05 * breath * np.sin(0.5 * np.pi * np.clip(t[0] / max(attack, 0.02), 0, 1)) ** 2)[:, None]
    y = _formants(src, (sex, vowel))
    if vowel2:
        m = np.clip(np.arange(n) / max(n_dur, 1), 0, 1)[:, None]
        y = y * (1 - m) + _formants(src, (sex, vowel2)) * m
    y = hp(y, max(60.0, 0.7 * f), 2)
    y *= 0.10 / _rms(y[int(0.3 * n):max(int(0.8 * n), 1)])
    e = np.ones(n)
    if n > n_dur:
        e[n_dur:] = fade_out(n - n_dur)
    e = e * _swell(swell, n, n_dur)
    return y * (e * vel ** 1.3)[:, None]


def piano(f, dur, vel=0.6, pedal=True, hard=1.0, seed=0, maxlen=10.0, felt=0.0):
    """Piano tone from inharmonic partials (stiff strings), each with its own
    two-stage decay, two slightly mistuned strings (slow beating, one a little
    to each side), and a soft hammer thump. `felt` 0..1 darkens and softens the
    hammer. With `pedal` the note rings on after `dur`. Stereo."""
    rng = np.random.default_rng(int(seed) * 15485863 + int(f * 16) + 5)
    B = 1.6e-4 * (f / 130.81) ** 1.05
    tau1 = float(np.clip(1.9 * (261.63 / f) ** 0.55, 0.25, 5.0))
    total = min(maxlen, tau1 * 7.5) if pedal else min(maxlen, dur + 0.5)
    total = max(total, min(dur, maxlen) if pedal else 0.1)
    n = int(total * SR)
    t = tt(n)
    L = np.zeros(n)
    R = np.zeros(n)
    k = np.arange(1, 64)
    fk = k * f * np.sqrt(1.0 + B * k * k)
    k = k[fk < 0.42 * SR]
    fk = fk[:len(k)]
    hammer_fc = (700.0 + 5200.0 * vel ** 1.7 * hard) * (1.0 - 0.6 * felt)
    amp = k ** -0.9 * (0.3 + 0.7 * np.abs(np.sin(np.pi * k / 7.7))) * np.exp(-fk / hammer_fc)
    amp *= fk * fk / (fk * fk + 85.0 ** 2)      # the soundboard radiates little below ~85 Hz
    amp /= amp.max()
    cents = rng.uniform(0.25, 0.75) * (1 if rng.random() < 0.5 else -1)
    d2 = 2.0 ** (cents / 1200.0)
    for kk, fq, a in zip(k, fk, amp):
        if a < 2e-4:
            continue
        rate = (1.0 / tau1) * (1.0 + 0.22 * (kk - 1) + 0.012 * (kk - 1) ** 2)
        m = min(n, int(SR * (np.log(a / 1e-4) / (rate * 0.4))) + 1)
        tk = t[:m]
        e = a * (0.72 * np.exp(-tk * rate * 1.7) + 0.28 * np.exp(-tk * rate * 0.4))
        p1, p2 = rng.uniform(0, TAU, 2)
        s1 = np.sin(TAU * fq * tk + p1) * e
        s2 = np.sin(TAU * fq * d2 * tk + p2) * e
        L[:m] += s1 + 0.3 * s2
        R[:m] += 0.55 * s1 + 0.7 * s2
    y = np.stack([L, R], axis=1)
    na = int(SR * (0.0015 + 0.004 * felt + 0.002 * (1 - vel)))
    y[:na] *= fade_in(na)[:, None]
    y *= 0.22 / (np.abs(y[:int(0.08 * SR)]).max() + 1e-9)
    # hammer: felt thump and a little key noise
    nh = min(n, int(0.12 * SR))
    th = t[:nh]
    thump = (lp(noise(nh, rng), 900.0 + 2200.0 * vel * (1 - 0.7 * felt), 2) * np.exp(-th / 0.014) * 0.9
             + np.sin(TAU * 95.0 * th) * np.exp(-th / 0.03) * 0.35
             + np.sin(TAU * 170.0 * th + 1.0) * np.exp(-th / 0.022) * 0.25)
    y[:nh] += (thump * 0.035 * (0.6 + 0.9 * felt))[:, None]
    if not pedal:
        nd = int(dur * SR)
        if nd < n:
            rel = 0.09 + 0.12 * min(1.0, 130.0 / f)
            damp = np.ones(n)
            damp[nd:] = np.exp(-(t[nd:] - t[nd]) / rel)
            y = (y * damp[:, None])[:min(n, nd + int(rel * 7 * SR))]
    nf = max(int(0.15 * len(y)), min(len(y), int(0.05 * SR)))
    y[-nf:] *= fade_out(nf)[:, None]
    return y * vel ** 1.5


def fm_bell(f, dur=4.0, vel=0.6, ratio=3.5, index=3.0, decay=1.6, seed=0, strike=1.0):
    """Glassy FM bell (the sound of Aetherite): an inharmonic FM pair whose
    modulation dies away faster than the tone, a detuned upper pair and a short
    high strike partial. Stereo (channels a few cents apart)."""
    rng = np.random.default_rng(int(seed) + int(f))
    n = int(dur * SR)
    t = tt(n)
    out = np.zeros((n, 2))
    if f * ratio > 8000.0:            # keep the sidebands of very high bells under Nyquist
        ratio = 1.41
    if f * 5.43 > 0.45 * SR:
        strike = 0.0
    for ch, dc in enumerate((-1.6, 1.6)):
        fc = f * 2.0 ** (dc / 1200.0)
        ind = index * (0.35 + 0.65 * vel) * (0.2 + 0.8 * np.exp(-t / (decay * 0.3)))
        y = np.sin(TAU * fc * t + ind * np.sin(TAU * fc * ratio * t)) * np.exp(-t / decay)
        y += 0.32 * np.sin(TAU * fc * 2.003 * t + 0.5 * ind * np.sin(TAU * fc * 2 * 1.41 * t + ch)) * np.exp(
            -t / (decay * 0.55))
        y += 0.25 * strike * np.sin(TAU * fc * 5.43 * t + rng.uniform(0, TAU)) * np.exp(-t / 0.07)
        y += 0.5 * np.sin(TAU * fc * 0.5 * t) * np.exp(-t / (decay * 1.2)) * 0.25
        out[:, ch] = y
    na = int(0.002 * SR)
    out[:na] *= fade_in(na)[:, None]
    nf = min(n, int(0.15 * SR))
    out[-nf:] *= fade_out(nf)[:, None]
    return out * 0.3 * vel ** 1.3


def glass_pad(f, dur, vel=0.5, attack=1.5, release=2.0, seed=0, shimmer=1.0, bright=1.0):
    """Sustained glass/Aether pad: slightly stretched partials, each breathing
    on its own slow LFO, differently left and right. Stereo."""
    rng = np.random.default_rng(int(seed) * 271 + int(f * 8) + 17)
    n = int((dur + release) * SR)
    t = tt(n)
    out = np.zeros((n, 2))
    ratios = np.array([1.0, 2.0, 3.004, 4.012, 5.03, 6.06, 8.1])
    amps = np.array([1.0, 0.5, 0.3, 0.2, 0.12, 0.08, 0.05]) * np.array([1, 1, bright, bright, bright, bright, bright])
    for r, a in zip(ratios, amps):
        if f * r > 0.4 * SR:
            continue
        for ch in range(2):
            for dc in (-3.5, 3.5):
                fq = f * r * 2.0 ** ((dc + rng.uniform(-1, 1)) / 1200.0)
                lfo = 0.55 + 0.45 * shimmer * np.sin(TAU * rng.uniform(0.07, 0.35) * t + rng.uniform(0, TAU))
                out[:, ch] += a * lfo * np.sin(TAU * fq * t + rng.uniform(0, TAU))
    e = ar(n, attack, release)
    return out * (e * 0.06 * vel ** 1.2)[:, None]


def timpani(f, vel=0.8, dur=2.6, seed=0):
    """Kettledrum: tuned membrane modes with a small pitch drop, a felt
    mallet thud and skin noise. Mono."""
    rng = np.random.default_rng(int(seed) + int(f * 4))
    n = int(dur * SR)
    t = tt(n)
    ratios = (1.0, 1.504, 1.742, 2.0, 2.245, 2.494, 2.8, 3.0)
    amps = (1.0, 0.7, 0.18, 0.45, 0.2, 0.22, 0.1, 0.1)
    decs = (1.0, 0.75, 0.45, 0.55, 0.35, 0.3, 0.22, 0.2)
    bend = 1.0 + 0.035 * vel * np.exp(-t / 0.05)
    ph = np.cumsum(bend) / SR
    y = np.zeros(n)
    for r, a, d in zip(ratios, amps, decs):
        bright = 1.0 if r < 1.6 else vel ** 1.2
        y += a * bright * np.sin(TAU * f * r * ph + rng.uniform(0, TAU)) * np.exp(-t / (d * dur * 0.36))
    thud = lp(noise(n, rng), 500 + 900 * vel, 2) * np.exp(-t / 0.02) * 1.6
    skin = bp(noise(n, rng), 150, 1200, 1) * np.exp(-t / 0.12) * 0.25 * vel
    y = y + thud + skin
    y[:60] *= fade_in(60)
    y[-2000:] *= fade_out(2000)
    return y * 0.28 * vel ** 1.4


def timp_roll(f, dur, vel=0.6, swell=(0.3, 1.0), rate=13.0, seed=0):
    """Timpani roll with a dynamic shape. Mono."""
    rng = np.random.default_rng(seed + 99)
    n = int((dur + 2.2) * SR)
    y = np.zeros(n)
    k = 0
    tpos = 0.0
    while tpos < dur:
        v = vel * np.interp(tpos / dur, np.linspace(0, 1, len(swell)), swell) * rng.uniform(0.85, 1.1)
        s = timpani(f, min(v, 1.0) * 0.8, 2.0, seed=seed + k % 5)
        i = int(tpos * SR)
        y[i:i + len(s)] += s[:n - i]
        tpos += (1.0 / rate) * rng.uniform(0.9, 1.1)
        k += 1
    return y


def taiko(f=62.0, vel=0.9, dur=1.4, seed=0, rim=0.0):
    """Big drum: membrane modes dropping in pitch, a noise body and a stick
    crack, pushed into soft saturation. Mono."""
    rng = np.random.default_rng(int(seed) + int(f * 4) + 1)
    n = int(dur * SR)
    t = tt(n)
    bend = 1.0 + (0.5 * vel) * np.exp(-t / 0.03) + 0.08 * np.exp(-t / 0.2)
    ph = np.cumsum(bend) / SR
    y = np.zeros(n)
    for r, a, d in ((1.0, 1.0, 0.36), (1.59, 0.5, 0.2), (2.14, 0.3, 0.14), (2.3, 0.22, 0.12), (2.65, 0.16, 0.1),
                    (3.16, 0.1, 0.07)):
        y += a * np.sin(TAU * f * r * ph + rng.uniform(0, 1)) * np.exp(-t / (d * dur / 1.4))
    body = bp(noise(n, rng), 90, 900, 1) * np.exp(-t / 0.07) * 0.9
    crack = bp(noise(n, rng), 1500, 6000, 1) * np.exp(-t / (0.006 + 0.01 * rim)) * (0.5 + 1.2 * rim) * vel
    y = np.tanh((y + body + crack) * (0.9 + 0.9 * vel)) / np.tanh(1.8)
    y[:40] *= fade_in(40)
    y[-2000:] *= fade_out(2000)
    return y * 0.42 * vel ** 1.3


def snare(vel=0.7, dur=0.35, seed=0, tone_f=185.0):
    """Field/snare drum: shell tone and snare-wire noise. Mono."""
    rng = np.random.default_rng(int(seed) + 77)
    n = int(dur * SR)
    t = tt(n)
    shell = (np.sin(TAU * tone_f * t * (1 + 0.08 * np.exp(-t / 0.02))) * np.exp(-t / 0.07)
             + 0.5 * np.sin(TAU * tone_f * 1.79 * t) * np.exp(-t / 0.045))
    wires = bp(noise(n, rng), 900, 7500, 1) * (0.7 * np.exp(-t / 0.035) + 0.3 * np.exp(-t / 0.13))
    y = shell * 0.55 + wires * 0.8
    y[:30] *= fade_in(30)
    y[-500:] *= fade_out(500)
    return y * 0.3 * vel ** 1.4


def cymbal(dur=4.0, vel=0.7, swell=0.0, seed=0, dark=0.0):
    """Suspended cymbal. swell=0 is a crash; swell>0 is a roll that grows for
    `swell` seconds and then rings. Stereo."""
    rng = np.random.default_rng(int(seed) + 4242)
    n = int(dur * SR)
    t = tt(n)
    nz = noise(n, rng, 2)
    y = hp(nz, 2500 - 1500 * dark, 2) * 0.5 + bp(nz, 4000 - 2000 * dark, 11000 - 4000 * dark, 2) * 0.8
    # metallic partial cluster
    for _ in range(26):
        fq = rng.uniform(350, 7000) * (1 - 0.5 * dark)
        y[:, rng.integers(0, 2)] += 0.06 * np.sin(TAU * fq * t + rng.uniform(0, TAU)) * np.exp(-t / rng.uniform(0.6, 2.5))
    if swell > 0:
        ns = int(swell * SR)
        e = np.concatenate([np.linspace(0, 1, ns) ** 2.4, np.exp(-(t[:n - ns]) / (0.28 * (dur - swell) + 0.2))])[:n]
        wob = 1 + 0.15 * np.sin(TAU * 9.0 * t)
        e = e * wob
    else:
        e = 0.6 * np.exp(-t / 0.15) + 0.4 * np.exp(-t / (0.3 * dur))
        y += hp(noise(n, rng, 2), 6000, 1) * (np.exp(-t / 0.012))[:, None] * 0.8
    y *= e[:, None]
    y[-3000:] *= fade_out(3000)[:, None]
    return y * 0.16 * vel ** 1.3


def tamtam(f=58.0, dur=7.0, vel=0.7, seed=0):
    """Large gong: dense inharmonic partials whose upper ones bloom after
    the stroke. Stereo."""
    rng = np.random.default_rng(int(seed) + 808)
    n = int(dur * SR)
    t = tt(n)
    y = np.zeros((n, 2))
    for i in range(46):
        r = rng.uniform(1.0, 1.0 + 0.55 * i)
        fq = f * r * rng.uniform(0.98, 1.02)
        if fq > 6000:
            continue
        bloom = 1.0 - np.exp(-t / (0.02 + 0.5 * min(1.0, fq / 2500.0)))
        e = bloom * np.exp(-t / (dur * rng.uniform(0.12, 0.3) / (1 + fq / 2500.0)))
        a = 1.0 / (1 + fq / 400.0)
        y[:, i % 2] += a * e * np.sin(TAU * fq * t + rng.uniform(0, TAU))
    y += lp(noise(n, rng, 2), 300, 2) * np.exp(-t / 0.05)[:, None] * 0.7
    y[-4000:] *= fade_out(4000)[:, None]
    return y * 0.09 * vel ** 1.3


def anvil(f=640.0, vel=0.8, dur=0.9, seed=0):
    """Struck metal bar: stiff-bar partials with a hard click. Mono."""
    rng = np.random.default_rng(int(seed) + int(f))
    n = int(dur * SR)
    t = tt(n)
    y = np.zeros(n)
    for r, a, d in ((1.0, 1.0, 0.3), (2.756, 0.8, 0.22), (5.404, 0.6, 0.14), (8.933, 0.4, 0.09), (13.34, 0.25, 0.05),
                    (1.48, 0.3, 0.12), (3.9, 0.3, 0.1)):
        if f * r < 0.45 * SR:
            y += a * np.sin(TAU * f * r * t + rng.uniform(0, TAU)) * np.exp(-t / (d * dur))
    y += hp(noise(n, rng), 3000, 1) * np.exp(-t / 0.004) * 1.5
    y[:20] *= fade_in(20)
    y[-800:] *= fade_out(800)
    return y * 0.17 * vel ** 1.3


def sub_bass(f, dur, vel=0.7, attack=0.01, release=0.12, drive=1.5, seed=0):
    """Synthetic bass: sine plus a filtered saw, lightly saturated. Mono."""
    n = int((dur + release) * SR)
    t = tt(n)
    fr = np.full(n, f)
    y = np.sin(TAU * f * t) + 0.5 * lp(saw(fr, 0.25), 3.2 * f + 150, 2)
    y = np.tanh(y * drive) / np.tanh(drive)
    e = ar(n, attack, release) * (0.75 + 0.25 * np.exp(-t / 0.15))
    return y * e * 0.3 * vel ** 1.2


def pluck_synth(f, dur, vel=0.6, bright=1.0, seed=0, release=0.15):
    """Synthetic pulse pluck (the Aether's edge): two detuned pulses, the
    filter closing quickly. Stereo."""
    n = int((dur + release) * SR)
    t = tt(n)
    out = np.zeros((n, 2))
    env = np.exp(-t / (0.09 + 0.25 * dur))
    for ch, dc in enumerate((-6.0, 6.0)):
        fr = np.full(n, f * 2.0 ** (dc / 1200.0))
        s = pulse(fr, 0.32, 0.1 + 0.3 * ch) + 0.4 * saw(fr * 2.0, 0.6)
        out[:, ch] = morph_lp(s, env ** 1.5, 300.0 + f, (1800.0 + 5000.0 * bright * vel) + f, steps=4)
    e = ar(n, 0.003, release) * env
    return out * (e * 0.16 * vel ** 1.2)[:, None]


def harp(f, dur=2.5, vel=0.6, seed=0):
    """Plucked string (harp-like): harmonic partials, bright pluck, fast
    upper decay. Stereo."""
    rng = np.random.default_rng(int(seed) + int(f * 8) + 23)
    n = int(min(dur, 6.0) * SR)
    t = tt(n)
    y = np.zeros(n)
    tau = np.clip(1.3 * (220.0 / f) ** 0.4, 0.3, 2.5)
    for k in range(1, 22):
        fq = f * k * (1 + 0.00006 * k * k)
        if fq > 0.4 * SR:
            break
        a = (1.0 / k) * abs(np.sin(np.pi * k * 0.23)) * np.exp(-fq / (2500 + 5000 * vel))
        y += a * np.sin(TAU * fq * t + rng.uniform(0, TAU)) * np.exp(-t * (1 + 0.35 * (k - 1)) / tau)
    y += lp(noise(n, rng), 3000, 1) * np.exp(-t / 0.004) * 0.3
    y[:40] *= fade_in(40)
    y[-min(n, 3000):] *= fade_out(min(n, 3000))
    y *= 0.25 / (np.abs(y).max() + 1e-9)
    return stereo(y * vel ** 1.4, rng.uniform(-0.15, 0.15))


# ---------------------------------------------------------------- reverb

_IR_CACHE = {}


def make_ir(rt60=2.6, predelay=0.02, damp=0.6, seed=7, er=0.6, lowcut=140.0):
    """Synthetic stereo hall impulse: band-wise decaying noise (highs die
    sooner), a diffuse onset, sparse early reflections and a pre-delay."""
    key = (rt60, predelay, damp, seed, er, lowcut)
    if key in _IR_CACHE:
        return _IR_CACHE[key]
    rng = np.random.default_rng(seed)
    n = int(SR * (rt60 * 1.35 + 0.2))
    t = tt(n)
    centers = np.array([90, 180, 360, 720, 1440, 2880, 5760, 11500], dtype=float)
    mult = np.array([1.1, 1.15, 1.1, 1.0, 0.86, 0.66, 0.44, 0.26])
    mult = 1.0 + (mult - 1.0) * (damp / 0.6)
    mult = np.clip(mult, 0.12, 1.3)
    out = np.zeros((n, 2))
    for c, m in zip(centers, mult):
        lo, hi = c / np.sqrt(2), min(c * np.sqrt(2), 0.47 * SR)
        sos = signal.butter(2, [lo / (SR / 2), hi / (SR / 2)], 'band', output='sos')
        b = signal.sosfilt(sos, rng.standard_normal((n, 2)), axis=0)
        out += b * np.exp(-6.908 * t / (rt60 * m))[:, None]
    out *= (1.0 - np.exp(-t / 0.035))[:, None]
    out /= np.sqrt(np.sum(out * out, axis=0, keepdims=True))
    early = np.zeros((n, 2))
    for i in range(16):
        d = rng.uniform(0.006, 0.085)
        g = er * 0.35 * np.exp(-d / 0.045) * rng.uniform(0.5, 1.0) * rng.choice([-1, 1])
        p = rng.uniform(-1, 1)
        gl, gr = pan_gains(p)
        j = int(d * SR)
        early[j:j + 3, 0] += g * gl * np.array([0.25, 0.5, 0.25])
        early[j:j + 3, 1] += g * gr * np.array([0.25, 0.5, 0.25])
    out = out + early
    out = hp(out, lowcut, 1)
    pd = int(predelay * SR)
    out = np.concatenate([np.zeros((pd, 2)), out])
    out /= np.sqrt(np.sum(out * out, axis=0, keepdims=True))
    _IR_CACHE[key] = out
    return out


def convolve_reverb(x, ir, cross=0.3):
    """Stereo in, stereo out; a little cross-feed widens the hall."""
    a = x[:, 0] + cross * x[:, 1]
    b = x[:, 1] + cross * x[:, 0]
    L = signal.fftconvolve(a, ir[:, 0])
    R = signal.fftconvolve(b, ir[:, 1])
    return np.stack([L, R], axis=1) / np.sqrt(1 + cross * cross)


def circ_reverb(x, ir, cross=0.3):
    """Reverb of a signal that loops: circular convolution, so the tail of
    the end is already in the beginning. Returns the wet signal only."""
    n = len(x)
    m = min(n, len(ir))
    a = x[:, 0] + cross * x[:, 1]
    b = x[:, 1] + cross * x[:, 0]
    out = np.zeros((n, 2))
    for ch, src in enumerate((a, b)):
        h = np.zeros(n)
        h[:m] = ir[:m, ch]
        out[:, ch] = np.fft.irfft(np.fft.rfft(src) * np.fft.rfft(h), n)
    return out / np.sqrt(1 + cross * cross)


def reverb(x, rt60=1.5, wet=0.25, predelay=0.01, damp=0.6, seed=3, tail=None):
    """Convenience for effects: returns dry + wet, extended by the tail."""
    if x.ndim == 1:
        x = stereo(x)
    ir = make_ir(rt60, predelay, damp, seed)
    w = convolve_reverb(x, ir)
    n = len(x) + int((tail if tail is not None else rt60) * SR)
    out = np.zeros((n, 2))
    out[:len(x)] = x
    m = min(n, len(w))
    out[:m] += wet * w[:m]
    return out


# ------------------------------------------------------------------ song

class Song:
    """A stereo timeline with a dry bus and named reverb sends."""

    def __init__(self, length, tail=8.0, reverbs=None, wrap=False):
        self.wrap = wrap                  # looping song: events before 0 land at the end
        self.length = float(length)
        self.n = int(round(length * SR))
        self.tail_n = int(tail * SR)
        self.N = self.n + self.tail_n
        self.dry = np.zeros((self.N, 2))
        self.reverbs = reverbs or {'hall': make_ir(2.6)}
        self.sends = {k: np.zeros((self.N, 2)) for k in self.reverbs}
        self.parts = {}

    def add(self, sig, t, gain=1.0, pan=0.0, send=0.25, part='misc', bus='hall', send2=0.0, bus2=None):
        if sig is None or len(sig) == 0:
            return
        if sig.ndim == 1:
            st = stereo(sig, pan)
        elif pan != 0.0:
            st = sig * np.array([min(1.0, 1.0 - pan), min(1.0, 1.0 + pan)])[None, :]
        else:
            st = sig
        i = int(round(t * SR))
        if i < 0 and self.wrap:
            i += self.n
        if i < 0:
            st = st[-i:]
            i = 0
        m = min(len(st), self.N - i)
        if m <= 0:
            return
        st = st[:m] * gain
        self.dry[i:i + m] += st
        if send > 0 and bus in self.sends:
            self.sends[bus][i:i + m] += st * send
        if send2 > 0 and bus2 in self.sends:
            self.sends[bus2][i:i + m] += st * send2
        acc = self.parts.setdefault(part, [0.0, 0.0])
        acc[0] += float(np.sum(st * st))
        acc[1] = max(acc[1], float(np.abs(st).max()))

    def mix(self, loop=False):
        out = self.dry.copy()
        for k, buf in self.sends.items():
            if np.any(buf):
                w = convolve_reverb(buf, self.reverbs[k])
                out += w[:self.N]
        if loop:
            m = min(self.tail_n, self.n)
            out[:m] += out[self.n:self.n + m]
        return out[:self.n]

    def report(self):
        rows = []
        for k, (e, pk) in sorted(self.parts.items(), key=lambda kv: -kv[1][0]):
            rows.append('    %-14s avg %6.1f dB  peak %6.1f dB' % (k, 10 * np.log10(e / (2 * self.n) + 1e-20), to_db(pk)))
        return '\n'.join(rows)


def crossfade_loop(x, n_loop, n_xf):
    """Make continuous material loop: x has n_loop + n_xf samples; the extra
    end is faded out over the faded-in beginning (equal power)."""
    y = x[:n_loop].copy()
    w = np.linspace(0, 1, n_xf)
    fi, fo = np.sin(0.5 * np.pi * w), np.cos(0.5 * np.pi * w)
    if x.ndim == 2:
        fi, fo = fi[:, None], fo[:, None]
    y[:n_xf] = x[:n_xf] * fi + x[n_loop:n_loop + n_xf] * fo
    return y


# ------------------------------------------------------- score utilities

class Grid:
    """Bars and beats to seconds at a fixed tempo. Bars count from 1, beats
    within a bar from 0."""

    def __init__(self, bpm, beats_per_bar=4, offset=0.0):
        self.bpm, self.bpb, self.offset = bpm, beats_per_bar, offset
        self.beat = 60.0 / bpm

    def t(self, bar, beat=0.0):
        return self.offset + ((bar - 1) * self.bpb + beat) * self.beat

    def d(self, beats):
        return beats * self.beat


def parse(line):
    """'A3:1.5 D4:.5 | E4:1.5 F4:.5 A4:2' -> [(pitches or None, beats, vel_scale)].
    'r:1' is a rest, 'D3+A3:2' a chord, 'A4:1@1.2' scales that note's velocity."""
    out = []
    for tok in line.split():
        if tok == '|':
            continue
        vs = 1.0
        if '@' in tok:
            tok, v = tok.split('@')
            vs = float(v)
        p, b = tok.split(':')
        out.append((None if p == 'r' else [midi(x) for x in p.split('+')], float(b), vs))
    return out


def play(song, grid, inst, line, bar, beat=0.0, vel=0.7, gain=1.0, pan=0.0, send=0.25, part='misc',
         legato=1.0, dyn=None, transpose=0, human=0.006, seed=0, bus='hall', send2=0.0, bus2=None,
         min_dur=0.05, **kw):
    """Play a parsed line. `dyn` is a list of (beat_offset, vel) points that
    shape the phrase; `legato` scales note lengths. Returns the beats used."""
    notes = parse(line) if isinstance(line, str) else line
    rng = np.random.default_rng(seed + 1234)
    pos = 0.0
    for i, (ps, beats, vs) in enumerate(notes):
        if ps is not None:
            v = vel if dyn is None else float(np.interp(pos, [d[0] for d in dyn], [d[1] for d in dyn]))
            v = min(1.0, v * vs * rng.uniform(0.96, 1.04))
            t0 = grid.t(bar, beat + pos) + rng.uniform(-human, human)
            dur = max(min_dur, grid.d(beats) * legato)
            for p in ps:
                sig = inst(hz(p + transpose), dur, v, seed=seed + i, **kw)
                song.add(sig, t0, gain, pan, send, part, bus, send2, bus2)
        pos += beats
    return pos


_CHORDS = {
    '': (0, 4, 7), 'm': (0, 3, 7), '5': (0, 7), 'sus4': (0, 5, 7), 'sus2': (0, 2, 7), '7': (0, 4, 7, 10),
    'm7': (0, 3, 7, 10), 'maj7': (0, 4, 7, 11), 'add9': (0, 4, 7, 2), 'madd9': (0, 3, 7, 2), 'dim': (0, 3, 6),
    'dim7': (0, 3, 6, 9), 'm7b5': (0, 3, 6, 10), 'aug': (0, 4, 8), '7sus4': (0, 5, 7, 10), 'm6': (0, 3, 7, 9),
    '6': (0, 4, 7, 9), 'maj9': (0, 4, 7, 11, 2), 'm9': (0, 3, 7, 10, 2), '7b9': (0, 4, 7, 10, 1),
    'maj7#11': (0, 4, 7, 11, 6), 'm11': (0, 3, 7, 10, 5),
}


def chord_pcs(sym):
    """'F/A' -> (pitch classes, bass pc). 'Bbadd9' -> ..."""
    bass = None
    if '/' in sym:
        sym, b = sym.split('/')
        bass = int(midi(b + '0')) % 12
    i = 2 if len(sym) > 1 and sym[1] in '#b' else 1
    root = int(midi(sym[:i] + '0')) % 12
    pcs = [(root + iv) % 12 for iv in _CHORDS[sym[i:]]]
    return pcs, (root if bass is None else bass)


def voice_lead(chords, start, lo=None, hi=None):
    """Voice a chord progression for len(start) voices with smooth voice
    leading: each voice moves to a near chord tone, every chord tone that fits
    is sounded, voices do not cross. `start` is a list of MIDI notes/names.
    Returns a list of voicings (lists of MIDI numbers, low to high)."""
    import itertools
    cur = [int(midi(x)) for x in start]
    lo = min(cur) - 5 if lo is None else int(midi(lo))
    hi = max(cur) + 5 if hi is None else int(midi(hi))
    out = []
    for sym in chords:
        pcs, _ = chord_pcs(sym)
        need = set(pcs[:min(len(pcs), len(cur))]) if len(pcs) <= len(cur) else set(
            [pcs[1]] + pcs[3:] + [pcs[0]][:max(0, len(cur) - len(pcs) + 2)])
        cands = []
        for v in cur:
            c = [p for p in range(v - 6, v + 7) if p % 12 in pcs and lo <= p <= hi]
            cands.append(c or [v])
        best, best_cost = None, 1e9
        for combo in itertools.product(*cands):
            if any(combo[i] >= combo[i + 1] for i in range(len(combo) - 1)):
                continue
            cost = sum(abs(a - b) for a, b in zip(combo, cur))
            cost += 8 * len(need - set(p % 12 for p in combo))
            cost += 3 * sum(1 for i in range(len(combo) - 1) if combo[i + 1] - combo[i] > 12)
            cost += 2 * sum(1 for i in range(len(combo) - 1) if combo[i + 1] - combo[i] < 3 and combo[i] < 55)
            cost += 3 * sum(1 for i in range(len(combo) - 1) if combo[i + 1] - combo[i] < 5 and combo[i] < 50)
            if cost < best_cost:
                best, best_cost = combo, cost
        cur = list(best) if best is not None else cur
        out.append(list(cur))
    return out


# ------------------------------------------------------------- mastering

def _kweight(x):
    f0, G, Q = 1681.974450955533, 3.999843853973347, 0.7071752369554196
    y = shelf(x, f0, G, True, Q)
    w = TAU * 38.13547087602444 / SR
    al = np.sin(w) / (2 * 0.5003270373238773)
    cs = np.cos(w)
    return _sos(y, _biquad([(1 + cs) / 2, -(1 + cs), (1 + cs) / 2], [1 + al, -2 * cs, 1 - al]))


def lufs(x):
    """Integrated loudness (ITU-R BS.1770 gating) of a mono/stereo signal."""
    if x.ndim == 1:
        x = x[:, None]
    z = _kweight(x)
    bl, hop = int(0.4 * SR), int(0.1 * SR)
    if len(z) < bl:
        return -0.691 + 10 * np.log10(np.sum(np.mean(z * z, axis=0)) + 1e-20)
    c = np.cumsum(np.concatenate([np.zeros((1, z.shape[1])), z * z]), axis=0)
    idx = np.arange(0, len(z) - bl + 1, hop)
    p = np.sum((c[idx + bl] - c[idx]) / bl, axis=1)
    l = -0.691 + 10 * np.log10(p + 1e-20)
    keep = l > -70
    if not keep.any():
        return -100.0
    rel = -0.691 + 10 * np.log10(np.mean(p[keep])) - 10
    keep = keep & (l > rel)
    return -0.691 + 10 * np.log10(np.mean(p[keep]) + 1e-20)


def short_lufs(x, win=3.0, hop=1.0):
    """Short-term loudness curve: (times, LUFS)."""
    if x.ndim == 1:
        x = x[:, None]
    z = _kweight(x)
    bl, hp_ = int(win * SR), int(hop * SR)
    c = np.cumsum(np.concatenate([np.zeros((1, z.shape[1])), z * z]), axis=0)
    idx = np.arange(0, max(1, len(z) - bl + 1), hp_)
    p = np.sum((c[np.minimum(idx + bl, len(z))] - c[idx]) / bl, axis=1)
    return idx / SR + win / 2, -0.691 + 10 * np.log10(p + 1e-20)


def compress(x, thresh_db=-20.0, ratio=2.0, win=0.05, smooth=0.15, knee=6.0):
    """Gentle RMS 'glue' compressor (linked stereo). Returns (y, gain curve)."""
    p = np.mean(x * x, axis=1) if x.ndim == 2 else x * x
    a = np.exp(-1.0 / (win * SR))
    env = signal.lfilter([1 - a], [1, -a], p)
    lev = 10 * np.log10(env + 1e-12)
    over = lev - thresh_db
    gr = np.where(over <= -knee / 2, 0.0,
                  np.where(over >= knee / 2, over, (over + knee / 2) ** 2 / (2 * knee)))
    gr_db = -gr * (1 - 1 / ratio)
    b = np.exp(-1.0 / (smooth * SR))
    gr_db = signal.lfilter([1 - b], [1, -b], gr_db)
    g = 10 ** (gr_db / 20.0)
    return (x * g[:, None] if x.ndim == 2 else x * g), g


def limit(x, ceiling_db=-1.5, look=0.004, release=0.06):
    """Look-ahead peak limiter (linked stereo)."""
    ceil = db(ceiling_db)
    pk = np.abs(x).max(axis=1) if x.ndim == 2 else np.abs(x)
    g = np.minimum(1.0, ceil / np.maximum(pk, 1e-9))
    if g.min() >= 1.0:
        return x
    w = int(look * SR) | 1
    wr = int(release * SR) | 1
    g = ndimage.minimum_filter1d(g, size=w, mode='nearest')
    # hold the reduction after a peak and let it recover smoothly (causal)
    h = ndimage.minimum_filter1d(g, size=wr, mode='nearest')
    h = ndimage.uniform_filter1d(h, size=wr, mode='nearest')
    sh = wr // 2
    h = np.concatenate([np.ones(sh), h[:-sh]])
    g = np.minimum(g, h)
    g = ndimage.uniform_filter1d(g, size=w, mode='nearest')
    return x * g[:, None] if x.ndim == 2 else x * g


def master(x, target_lufs=-16.0, ceiling_db=-1.5, comp=True, loop=False, ctx=3.0, hp_hz=24.0,
           comp_thresh=-2.0, comp_ratio=1.6, loop_start=0):
    """High-pass, glue compression, loudness normalisation and limiting.
    For loops the processing sees the loop's end before its start and its
    start after its end, so the result still wraps cleanly. `loop_start`
    (samples) marks where the looped part begins when x has an intro; the
    intro is then the context before it. `comp_thresh` is in dB relative to
    the programme's own loudness."""
    n = len(x)
    c0 = 0
    y = x
    if loop:
        c = min(int(ctx * SR), n - loop_start)
        y = np.concatenate([x, x[loop_start:loop_start + c]])
        if loop_start == 0:
            y = np.concatenate([x[-c:], y])
            c0 = c
    y = hp(y, hp_hz, 2)
    y = y * db(-18.0 - lufs(y[c0:c0 + n]))    # nominal level, so thresholds mean the same everywhere
    if comp:
        y, _ = compress(y, thresh_db=-18.0 + comp_thresh, ratio=comp_ratio)
    y = y * db(target_lufs - lufs(y[c0:c0 + n]))
    y = limit(y, ceiling_db)
    trim = target_lufs - lufs(y[c0:c0 + n])   # limiting pulls loudness down a little
    if abs(trim) > 0.1:
        y = limit(y * db(min(trim, 3.0)), ceiling_db)
    return y[c0:c0 + n]


def normalize_peak(x, peak_db=-1.5):
    return x * (db(peak_db) / (np.abs(x).max() + 1e-12))


# ------------------------------------------------------------------ files

def write_ogg(path, x, quality=0.5):
    """Write Ogg Vorbis in blocks (libsndfile can overflow its stack on long
    single writes) and return the decoded audio for verification."""
    import soundfile as sf
    x = np.asarray(x, dtype=np.float32)
    ch = 1 if x.ndim == 1 else x.shape[1]
    with sf.SoundFile(path, 'w', samplerate=SR, channels=ch, format='OGG', subtype='VORBIS',
                      compression_level=float(np.clip(1.0 - quality, 0, 1))) as f:
        for i in range(0, len(x), 16384):
            f.write(x[i:i + 16384])
    y, sr = sf.read(path, always_2d=False)
    assert sr == SR
    return y
