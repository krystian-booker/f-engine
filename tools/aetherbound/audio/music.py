"""The Aetherbound score: one function per track.

Each function returns {'audio': stereo array, 'loop': bool, 'loop_start': s,
'loop_end': s, 'lufs': target}. The friendship motif (Cael and Rowan) is

    A  D  E  F  A  C        (in D minor: 5 1 2 b3 5 b7; rising, left hanging)
    1.5 .5 1.5 .5 2  4      beats, starting on the third beat of a bar

It is hinted by a lone horn in `fortress`, carried by the full strings in
`last_light` (where, at the climax, its last note finally resolves up to D),
and played by a solo piano in `remains`, where it stops on the fifth note.
"""
import numpy as np
from synth import (SR, TAU, Song, Grid, make_ir, hz, midi, db, tt, play, parse, voice_lead, chord_pcs,
                   strings, solo_string, brass, choir, piano, fm_bell, glass_pad, timpani, timp_roll, taiko,
                   snare, cymbal, tamtam, anvil, sub_bass, pluck_synth, harp, periodic_noise, crossfade_loop,
                   lp, hp, bp, reson, saw, noise, stereo, fade_in, fade_out, ar, master, pan_gains, morph_lp)

MOTIF = 'A3:1.5 D4:.5 | E4:1.5 F4:.5 A4:2 | C5:4'

# --------------------------------------------------------------- helpers

_cache = {}


def cached(fn, *args, seed=0, variants=3, **kw):
    """Short repeated notes: a few round-robin variants instead of hundreds
    of identical renders (and no machine-gun effect)."""
    key = (fn.__name__, args, tuple(sorted(kw.items())), seed % variants)
    if key not in _cache:
        _cache[key] = fn(*args, seed=seed % variants, **kw)
    return _cache[key]


def spic(p, vel, dur=0.16, voices=5, center=0.3, bright=1.1):
    """Short marcato stroke of a low string section."""
    return lambda seed: cached(strings, round(hz(p), 3), dur, round(vel, 2), attack=0.012, release=0.14,
                               voices=voices, marc=0.9, vib=0.0, spread=0.45, center=center, bright=bright,
                               seed=seed)


def vel_at(dyn, pos, default):
    if dyn is None:
        return default
    return float(np.interp(pos, [d[0] for d in dyn], [d[1] for d in dyn]))


def harmony(song, g, prog, bar, beat=0.0, start=('A3', 'D4', 'F4'), inst=strings, vel=0.5, dyn=None,
            gain=1.0, send=0.3, part='pad', lo=None, hi=None, overlap=0.12, seed=0, tie=True, **kw):
    """Sustain a progression [(chord, beats), ...] with voice leading; common
    tones are held across chords, and `dyn` [(beat, vel)...] shapes it."""
    syms = [p[0] for p in prog]
    beats = [p[1] for p in prog]
    vs = voice_lead(syms, start, lo, hi)
    for v in range(len(start)):
        pos, i = 0.0, 0
        while i < len(prog):
            p, b, j = vs[i][v], beats[i], i + 1
            while tie and j < len(prog) and vs[j][v] == p:
                b += beats[j]
                j += 1
            v0, v1 = vel_at(dyn, pos, vel), vel_at(dyn, pos + b, vel)
            vm = max(v0, v1, 1e-3)
            sig = inst(hz(p), g.d(b) + overlap, vm, swell=((v0 / vm) ** 1.3, (v1 / vm) ** 1.3),
                       seed=seed + 13 * v + i, **kw)
            song.add(sig, g.t(bar, beat + pos), gain, 0.0, send, part)
            pos += b
            i = j
    return vs


def bass_pitch(sym, ref):
    _, b = chord_pcs(sym)
    r = int(midi(ref))
    return [p for p in range(r - 6, r + 6) if p % 12 == b][0]


def bassline(song, g, prog, bar, beat=0.0, ref='D2', inst=strings, vel=0.6, dyn=None, gain=1.0, send=0.2,
             part='bass', octave_below=True, seed=0, overlap=0.1, tie=True, **kw):
    pos, i = 0.0, 0
    ps = [bass_pitch(sym, ref) for sym, _ in prog]
    while i < len(prog):
        p, b, j = ps[i], prog[i][1], i + 1
        while tie and j < len(prog) and ps[j] == p:
            b += prog[j][1]
            j += 1
        v0, v1 = vel_at(dyn, pos, vel), vel_at(dyn, pos + b, vel)
        vm = max(v0, v1, 1e-3)
        sw = ((v0 / vm) ** 1.3, (v1 / vm) ** 1.3)
        song.add(inst(hz(p), g.d(b) + overlap, vm, swell=sw, seed=seed + i, **kw), g.t(bar, beat + pos), gain,
                 0.0, send, part)
        if octave_below and p - 12 >= 24:
            song.add(inst(hz(p - 12), g.d(b) + overlap, vm, swell=sw, seed=seed + i + 50, **kw),
                     g.t(bar, beat + pos), gain * 0.8, 0.0, send, part)
        pos += b
        i = j


def chord_tones(sym, low, count):
    """Ascending chord tones starting at/above the bass near `low`."""
    pcs, b = chord_pcs(sym)
    p = bass_pitch(sym, low)
    out = [p]
    q = p + 1
    while len(out) < count:
        if q % 12 in pcs:
            out.append(q)
        q += 1
    return out


def arp(song, g, prog, bar, inst, pattern, step=0.5, low='D2', vel=0.45, dyn=None, gain=1.0, send=0.3,
        part='arp', pan=0.0, seed=0, human=0.008, skip_third_low=True, **kw):
    """Broken chords: `pattern` indexes the chord's ascending tones, one
    entry per `step` beats (None rests)."""
    rng = np.random.default_rng(seed + 5)
    pos = 0.0
    k = 0
    for sym, beats in prog:
        tones = chord_tones(sym, low, 12)
        if skip_third_low and len(tones) > 3 and tones[1] - tones[0] < 5:
            tones = [tones[0]] + tones[2:]         # open spacing at the bottom: root, fifth, ...
        nsteps = int(round(beats / step))
        for i in range(nsteps):
            idx = pattern[i % len(pattern)]
            if idx is not None:
                v = vel_at(dyn, pos + i * step, vel) * rng.uniform(0.9, 1.08) * (1.12 if i == 0 else 1.0)
                sig = inst(hz(tones[idx]), g.d(step) * 1.0, min(1.0, v), seed=seed + k, **kw)
                song.add(sig, g.t(bar, pos + i * step) + rng.uniform(-human, human), gain, pan, send, part)
            k += 1
        pos += beats


def hit(song, t, sig, gain=1.0, pan=0.0, send=0.2, part='perc', **kw):
    song.add(sig, t, gain, pan, send, part, **kw)


def snare_roll(song, t0, dur, v0, v1, rate=22.0, gain=1.0, pan=0.1, send=0.25, part='snare', seed=0):
    rng = np.random.default_rng(seed + 31)
    n = int(dur * rate)
    for i in range(n):
        v = v0 + (v1 - v0) * (i / max(1, n - 1)) ** 1.5
        hit(song, t0 + i / rate + rng.uniform(-0.004, 0.004),
            cached(snare, round(v * rng.uniform(0.85, 1.05), 2), 0.25, seed=i, variants=4), gain, pan, send, part)


def join_intro(intro, body, xf=0.03):
    """Intro followed by a looping body. The body's first samples already
    carry the tail of its own last bar; the intro ends with the same bar, so
    its last few milliseconds are cross-faded into the body's last ones and
    the first entry into the loop sounds like every later one."""
    n = int(xf * SR)
    w = np.linspace(0.0, 1.0, n)[:, None]
    intro = intro.copy()
    intro[-n:] = intro[-n:] * (1.0 - w) + body[-n:] * w
    return np.concatenate([intro, body])


def result(audio, loop, loop_start=0.0, target=-16.0, **kw):
    n = len(audio)
    y = master(audio, target_lufs=target, loop=loop, loop_start=int(round(loop_start * SR)), **kw)
    if not loop:
        nf = int(0.02 * SR)
        y[:nf] *= fade_in(nf)[:, None]
    return {'audio': y, 'loop': loop, 'loop_start': loop_start, 'loop_end': n / SR, 'lufs': target}


# ------------------------------------------------------------------ title

def title():
    """40 s loop. Still, cold: an open D-A-E that never moves, a bass that
    does (D, Bb, G, A), and a far bell that tolls the first three notes of
    the motif and stops."""
    L = 40.0
    s = Song(L, tail=14.0, reverbs={'hall': make_ir(5.0, 0.045, 0.5, 21)}, wrap=True)
    seg = 10.0
    bass = ['D2', 'Bb1', 'G1', 'A1']
    colour = ['A4', 'F4', 'G4', 'E4']
    for i in range(4):
        t0 = i * seg
        for p, v, c in (('D3', 0.36, 0.25), ('A3', 0.33, -0.2), ('E4', 0.26, 0.1)):
            s.add(strings(hz(p), seg + 1.0, v, attack=3.2, release=4.5, voices=6, vib=0.003, bright=0.45,
                          center=c, seed=i), t0 - 1.5, 1.0, 0.0, 0.55, 'strings')
        s.add(strings(hz(bass[i]), seg, 0.42, attack=2.5, release=4.0, voices=5, vib=0.002, bright=0.4,
                      center=0.2, seed=40 + i), t0 - 0.8, 1.0, 0.0, 0.35, 'bass')
        s.add(sub_bass(hz(bass[i]), seg, 0.35, attack=2.5, release=3.5, drive=0.6), t0 - 0.8, 0.5, 0.0, 0.1, 'bass')
        s.add(choir(hz(colour[i]), seg - 1.0, 0.3, attack=3.0, release=4.0, vowel='oo', voices=6, seed=i,
                    breath=1.6), t0 + 0.5, 0.8, 0.0, 0.7, 'choir')
    for i in range(2):
        for p, v in (('A4', 0.4), ('E5', 0.3), ('A5', 0.2)):
            s.add(glass_pad(hz(p), 20.0, v, attack=5.0, release=7.0, seed=i * 3 + int(midi(p))), i * 20.0 - 3.0,
                  0.9, 0.0, 0.5, 'glass')
    # the far bell: A . D . E . . (the motif begins and is not finished)
    for t, p, v in ((4.0, 'A5', 0.55), (14.0, 'D6', 0.5), (24.0, 'E6', 0.5), (33.0, 'A4', 0.32)):
        b = fm_bell(hz(p), 9.0, v, ratio=3.5, index=2.2, decay=3.2, seed=int(t))
        s.add(lp(b, 3800, 1), t, 0.28, -0.25, 1.4, 'bell')
    y = s.mix(loop=True)
    # cold air: a seamless band of high noise, breathing slowly
    n = len(y)
    rng = np.random.default_rng(5)
    t = tt(n)
    for ch in range(2):
        air = periodic_noise(n, rng, 3500, 12000, slope=-4.0)
        lfo = 0.6 + 0.4 * np.sin(TAU * (2 + ch) * t / L + ch * 1.7)
        y[:, ch] += air * lfo * 0.0035
    return result(y, True, target=-19.0, comp=False)


# --------------------------------------------------------------- fortress

def fortress():
    """70 s loop, D minor, 96 BPM, 28 bars. A march: low strings and drums
    under a horn theme (bars 5-12), again with violins (13-20), then the
    drums fall away and a lone horn plays the motif (pickup in bar 21, to
    bar 24) before the march gathers again on the dominant."""
    g = Grid(96)
    L = g.t(29)
    s = Song(L, tail=8.0, reverbs={'hall': make_ir(2.7, 0.022, 0.6, 7)}, wrap=True)
    prog = (['Dm'] * 4 + ['Dm', 'Dm', 'Bb', 'C', 'Dm', 'Gm', 'Asus4', 'A']
            + ['Dm', 'Dm', 'Bb', 'C', 'Dm', 'Gm', 'A7sus4', 'A7']
            + ['Bbmaj7', 'Dm', 'Bbadd9', 'Gm7', 'Ebmaj7', 'Asus4', 'A7', 'A7'])
    lvl = [0.62, 0.62, 0.66, 0.7] + [0.68] * 8 + [0.82] * 8 + [0, 0, 0, 0, 0.3, 0.45, 0.62, 0.8]
    rng = np.random.default_rng(1)
    for b in range(28):
        bar, v = b + 1, lvl[b]
        root = bass_pitch(prog[b], 'D2')
        if root > 44:
            root -= 12
        fifth = root - 5 if root - 5 >= 31 else root + 7
        if v > 0:
            # low strings: | D . D-d D A a |
            for beat, p, a, d in ((0, root, 1.0, 0.24), (1, root, 0.7, 0.2), (1.75, root, 0.55, 0.1),
                                  (2, root, 0.85, 0.24), (3, fifth, 0.7, 0.18), (3.5, fifth, 0.62, 0.16)):
                t = g.t(bar, beat) + rng.uniform(-0.004, 0.004)
                s.add(spic(p, v * a, d)(b * 7 + int(beat * 4)), t, 0.95, 0.0, 0.22, 'ostinato')
                if beat in (0, 2):
                    s.add(spic(p - 12, v * a, d, center=0.45)(b + int(beat)), t, 0.7, 0.0, 0.2, 'ostinato')
            # field drum
            for beat, a in ((0, 0.8), (0.75, 0.35), (1, 0.62), (1.5, 0.4), (1.75, 0.36), (2, 0.78), (2.75, 0.36),
                            (3, 0.62), (3.25, 0.3), (3.5, 0.45), (3.75, 0.5)):
                hit(s, g.t(bar, beat) + rng.uniform(-0.003, 0.003),
                    cached(snare, round(v * a * 0.9, 2), 0.3, seed=int(beat * 4) + b, variants=5), 1.0, 0.12, 0.3,
                    'snare')
            hit(s, g.t(bar, 0), cached(timpani, round(hz(root + 12), 2), round(0.75 * v, 2), 2.2, seed=b), 0.9, -0.1,
                0.3, 'timpani')
            hit(s, g.t(bar, 2), cached(timpani, round(hz(root + 12), 2), round(0.5 * v, 2), 2.0, seed=b + 1), 0.8,
                -0.1, 0.3, 'timpani')
            hit(s, g.t(bar, 3), cached(timpani, round(hz(fifth + 12 if fifth < root else fifth), 2),
                                       round(0.55 * v, 2), 2.0, seed=b + 2), 0.8, -0.1, 0.3, 'timpani')
            if b % 4 == 0:
                hit(s, g.t(bar, 0), taiko(58.0, 0.85 * v + 0.1, 1.6, seed=b), 0.9, 0.0, 0.3, 'taiko')
        else:
            # the quiet: a heartbeat on the downbeat only
            hit(s, g.t(bar, 0), timpani(hz(root + 12), 0.3, 2.4, seed=b), 0.8, -0.1, 0.4, 'timpani')
    hit(s, 0.0, cymbal(4.5, 0.45, seed=1, dark=0.4), 0.6, 0.0, 0.4, 'cymbal')

    # sustained harmony: violas and second violins, three parts
    pr = [(c, 4) for c in prog]
    dyn_h = [(0, 0.0), (8, 0.0), (16, 0.38), (48, 0.45), (79, 0.6), (80, 0.3), (96, 0.3), (104, 0.5), (112, 0.75)]
    harmony(s, g, pr[4:], 5, start=('A3', 'D4', 'F4'), lo='F3', hi='A4', vel=0.45,
            dyn=[(x - 16, v) for x, v in dyn_h[2:]], gain=0.8, send=0.35, part='strings-harm', center=0.15,
            voices=6, seed=3)
    # long basses under the march
    bassline(s, g, pr[4:20], 5, ref='D2', vel=0.45, dyn=[(0, 0.4), (32, 0.5), (64, 0.6)], gain=0.7, part='bass',
             voices=5, center=0.4, bright=0.6, attack=0.1, seed=9)
    bassline(s, g, pr[20:], 21, ref='D2', vel=0.3, dyn=[(0, 0.32), (16, 0.3), (24, 0.5), (32, 0.7)], gain=0.8,
             part='bass', voices=5, center=0.4, bright=0.5, attack=0.4, seed=19)

    theme = ('D4:1.5 D4:.5 F4:1 A4:1 | G4:1.5 F4:.5 E4:1 D4:1 | F4:1.5 F4:.5 Bb4:1 D5:1 | C5:1.5 Bb4:.5 G4:2 | '
             'A4:1.5 A4:.5 D5:1 F5:1 | E5:1.5 D5:.5 Bb4:1 D5:1 | D5:2 E5:2 | ')
    end1 = 'C#5:3 r:1'
    end2 = 'C#5:2 E5:1 G5:1'
    hd = [(0, 0.62), (12, 0.7), (16, 0.66), (24, 0.78), (32, 0.7)]
    # horns, first time alone
    play(s, g, brass, theme + end1, 5, vel=0.66, dyn=hd, gain=1.0, send=0.4, part='horns', legato=0.94,
         voices=4, bright=0.42, center=-0.25, attack=0.05, seed=11)
    # second time: horns and violins in octaves, trombones underneath
    play(s, g, brass, theme + end2, 13, vel=0.75, dyn=[(x, v + 0.1) for x, v in hd], gain=1.0, send=0.4,
         part='horns', legato=0.94, voices=4, bright=0.5, center=-0.25, attack=0.05, seed=21)
    for tr, gn, c in ((0, 0.75, -0.3), (12, 0.6, -0.4)):
        play(s, g, strings, theme + end2, 13, vel=0.7, dyn=[(x, v + 0.08) for x, v in hd], gain=gn, send=0.35,
             part='violins', legato=0.97, transpose=tr, voices=7, center=c, attack=0.07, release=0.2, seed=31 + tr)
    harmony(s, g, pr[12:20], 13, start=('D3', 'A3', 'D4'), lo='A2', hi='F4', inst=brass, vel=0.5,
            dyn=[(0, 0.42), (24, 0.5), (32, 0.62)], gain=0.6, send=0.35, part='trombones', center=0.25,
            voices=3, bright=0.7, attack=0.09, seed=41)
    hit(s, g.t(13), cymbal(4.0, 0.5, seed=2), 0.6, 0.0, 0.35, 'cymbal')
    hit(s, g.t(13), taiko(55.0, 0.95, 1.8, seed=3), 1.0, 0.0, 0.3, 'taiko')

    # bars 21-24: the motif, quietly, on a lone horn; the violins keep one high A
    s.add(strings(hz('A5'), g.d(15), 0.3, attack=1.2, release=2.5, voices=6, bright=0.5, center=-0.3, vib=0.004,
                  swell=(1.0, 0.8, 0.5), seed=5), g.t(21), 0.55, 0.0, 0.5, 'violins')
    play(s, g, brass, 'A3:1.5 D4:.5 | E4:1.5 F4:.5 A4:2 | C5:6', 21, beat=2.0, vel=0.52,
         dyn=[(0, 0.46), (4, 0.56), (6, 0.56), (12, 0.34)], gain=1.15, send=0.6, part='motif-horn', legato=1.0,
         voices=1, bright=0.3, attack=0.09, release=0.4, vib=0.004, human=0.01, seed=71)
    hit(s, g.t(23), fm_bell(hz('C6'), 6.0, 0.3, decay=2.5, index=1.6), 0.18, 0.2, 1.0, 'bell')
    # bars 25-28: gather on the dominant
    for p, c in (('E4', 0.1), ('A4', -0.1), ('C#5', -0.3), ('E5', -0.35)):
        s.add(strings(hz(p), g.d(8), 0.8, attack=0.6, release=0.25, voices=6, trem=0.75, center=c,
                      swell=(0.12, 0.4, 1.0), seed=int(midi(p))), g.t(27), 0.5, 0.0, 0.35, 'violins')
    run = 'A3:.25 B3:.25 C#4:.25 D4:.25 E4:.25 F4:.25 G4:.25 A4:.25'
    for tr, gn in ((0, 0.8), (12, 0.6), (-12, 0.8)):
        play(s, g, strings, run, 28, beat=2.0, vel=0.8, dyn=[(0, 0.55), (2, 0.95)], gain=gn, send=0.3, part='run',
             transpose=tr, legato=0.9, voices=5, attack=0.012, release=0.08, marc=0.5, vib=0, seed=81 + tr)
    play(s, g, brass, 'A2:8', 27, vel=0.8, gain=0.7, send=0.35, part='trombones', voices=3, bright=0.9,
         swell=(0.2, 0.5, 1.0), center=0.25, seed=91)
    play(s, g, brass, 'E3:8', 27, vel=0.8, gain=0.6, send=0.35, part='trombones', voices=3, bright=0.9,
         swell=(0.2, 0.5, 1.0), center=0.2, seed=92)
    hit(s, g.t(28), timp_roll(hz('A2'), g.d(4) - 0.05, 0.8, swell=(0.15, 0.5, 1.0), seed=4), 0.9, -0.1, 0.3, 'timpani')
    snare_roll(s, g.t(28), g.d(4) - 0.03, 0.12, 0.8, gain=0.55, seed=5)
    hit(s, g.t(28, 1.0), cymbal(g.d(3) + 2.5, 0.5, swell=g.d(3), seed=6), 0.5, 0.0, 0.3, 'cymbal')
    print(s.report())
    return result(s.mix(loop=True), True, target=-16.0)


# ----------------------------------------------------------------- battle

def _battle_turn(s, g, bar, v=1.0, with_low=True):
    """Two bars on the dominant (E7): tutti hits in 3+3+2, then a run up to
    the downbeat. Used for the end of the intro and the end of the loop, so
    both arrive at the theme the same way."""
    rng = np.random.default_rng(bar)
    for bb in range(2):
        hits = (0, 1.5, 3) if bb == 0 else (0, 1.5)
        for beat in hits:
            t = g.t(bar + bb, beat)
            for p, c in (('E2', 0.3), ('E3', 0.3), ('B3', 0.2), ('E4', 0.1)):
                s.add(brass(hz(p), g.d(0.6), 0.95 * v, voices=3, bright=1.1, center=c, attack=0.02, release=0.12,
                            seed=int(beat * 2) + bb), t, 0.8, 0.0, 0.3, 'brass')
            for p, c in (('G#4', -0.2), ('D5', -0.3), ('E5', -0.35), ('B5', -0.4)):
                s.add(strings(hz(p), g.d(0.55), 0.9 * v, voices=6, attack=0.012, release=0.12, marc=0.8, center=c,
                              seed=int(beat * 2) + bb), t, 0.7, 0.0, 0.3, 'violins')
            hit(s, t, taiko(56.0, 1.0 * v, 1.2, seed=int(beat)), 1.0, 0.0, 0.25, 'taiko')
            hit(s, t, timpani(hz('E2'), 0.9 * v, 1.6, seed=int(beat)), 0.9, -0.1, 0.3, 'timpani')
    run = 'E4:.25 F#4:.25 G#4:.25 A4:.25 B4:.25 C5:.25 D5:.25 E5:.25'
    for tr, gn in ((0, 0.8), (12, 0.7), (-12, 0.7)):
        play(s, g, strings, run, bar + 1, beat=2.0, vel=0.9 * v, dyn=[(0, 0.6 * v), (2, 1.0 * v)], gain=gn, send=0.3,
             part='run', transpose=tr, legato=0.9, voices=5, attack=0.01, release=0.06, marc=0.5, vib=0, seed=81 + tr)
    snare_roll(s, g.t(bar + 1, 2.0), g.d(2) - 0.02, 0.3 * v, 0.9 * v, rate=24.0, gain=0.6)
    hit(s, g.t(bar, 2.0), cymbal(g.d(6) + 2.0, 0.6 * v, swell=g.d(6), seed=bar), 0.5, 0.0, 0.3, 'cymbal')


def _battle_rhythm(s, g, bar, sym, v=1.0, drums=True, arp_on=True, seed=0, sub=True, snr=True):
    """One bar of the engine: galloping low strings, synth bass, taiko and
    snare, a 16th pulse of synthetic plucks."""
    rng = np.random.default_rng(seed * 131 + bar)
    root = bass_pitch(sym, 'C2')
    tones = chord_tones(sym, 'A3', 6)
    for beat in range(4):
        for off, a, d in ((0.0, 1.0, 0.13), (0.5, 0.62, 0.08), (0.75, 0.7, 0.08)):
            t = g.t(bar, beat + off) + rng.uniform(-0.003, 0.003)
            s.add(spic(root + 12, 0.8 * v * a, d)(bar * 12 + beat * 3 + int(off * 4)), t, 0.72, 0.0, 0.18, 'ostinato')
            if off == 0.0:
                s.add(spic(root, 0.8 * v * a, d, center=0.45)(bar + beat), t, 0.5, 0.0, 0.15, 'ostinato')
        if sub:
            for off in (0.0, 0.5):
                s.add(cached(sub_bass, round(hz(root), 3), g.d(0.42), round(0.7 * v, 2), drive=2.0),
                      g.t(bar, beat + off), 0.42 if off == 0 else 0.32, 0.0, 0.0, 'sub')
    if drums:
        for beat, a, f in ((0, 1.0, 58.0), (1.5, 0.8, 58.0), (2.5, 0.55, 66.0), (3, 0.9, 58.0), (3.5, 0.5, 72.0)):
            hit(s, g.t(bar, beat), cached(taiko, f, round(0.9 * v * a, 2), 0.9, seed=bar + int(beat * 2)), 0.62,
                0.0, 0.2, 'taiko')
        if snr:
            for beat, a in ((1, 0.85), (3, 0.9), (2.75, 0.35), (3.75, 0.4), (0.75, 0.3)):
                hit(s, g.t(bar, beat), cached(snare, round(0.8 * v * a, 2), 0.28, seed=bar + int(beat * 4),
                                              variants=5), 0.95, 0.1, 0.25, 'snare')
    if arp_on:
        pat = (0, 2, 1, 2, 3, 2, 1, 2, 0, 2, 1, 2, 4, 2, 3, 2)
        for i in range(16):
            p = tones[pat[i]] + 12
            acc = 1.0 if i % 4 == 0 else 0.7 if i % 2 == 0 else 0.55
            s.add(cached(pluck_synth, round(hz(p), 3), 0.07, round(0.6 * v * acc, 2), bright=0.8, variants=1),
                  g.t(bar, i * 0.25), 0.95, 0.35 * (1 if i % 2 else -1), 0.3, 'pluck')


def battle():
    """60 s: a 4-bar intro (6 s) and a 36-bar loop (54 s), A minor, 160 BPM.
    A (8) theme, A' (8) theme in octaves with brass, B (8) long-breathed
    strings and choir over the same engine, C (8) rising sequence over a
    chromatic bass, and a 4-bar dominant turn back to the top."""
    g = Grid(160)
    hall = {'hall': make_ir(2.2, 0.018, 0.65, 9)}
    n_intro = 4

    def theme_parts(s, b0):
        A = ('E5:1.5 D5:.5 C5:1 D5:1 | E5:1.5 G5:.5 A5:2 | A5:1.5 G5:.5 F5:1 G5:1 | G5:1.5 D5:.5 B4:1 D5:1 | '
             'E5:1.5 D5:.5 C5:1 D5:1 | E5:1.5 G5:.5 A5:1 B5:1 | C6:1.5 B5:.5 A5:1 F5:1 | E5:1.5 G#5:.5 B5:2')
        A2 = ('E5:1.5 D5:.5 C5:1 D5:1 | E5:1.5 G5:.5 A5:2 | A5:1.5 G5:.5 F5:1 G5:1 | G5:1.5 D5:.5 B4:1 D5:1 | '
              'E5:1.5 G5:.5 C6:1 B5:1 | D6:1.5 B5:.5 G5:1 B5:1 | C6:1.5 A5:.5 F5:1 A5:1 | B5:2 G#5:1 B5:1')
        B = ('C6:3 A5:1 | B5:3 G5:1 | G5:3 E5:1 | A5:4 | C6:3 D6:1 | D6:3 B5:1 | B5:2 E6:2 | '
             'E6:2 D6:.5 C6:.5 B5:.5 G#5:.5')
        C = ('D5:.5 F5:.5 A5:1 D5:.5 F5:.5 A5:1 | F5:.5 A5:.5 D6:1 F5:.5 A5:.5 D6:1 | '
             'E5:.5 G#5:.5 B5:1 E5:.5 G#5:.5 B5:1 | G#5:.5 B5:.5 E6:1 G#5:.5 B5:.5 E6:1 | '
             'F5:.5 A5:.5 C6:1 A5:.5 C6:.5 F6:1 | F#5:.5 A5:.5 C6:1 A5:.5 C6:.5 Eb6:1 | '
             'G5:.5 B5:.5 D6:1 B5:.5 D6:.5 G6:1 | G#5:.5 B5:.5 D6:1 B5:.5 D6:.5 F6:1')
        chA = ['Am', 'Am', 'F', 'G', 'Am', 'Am', 'F', 'E']
        chA2 = ['Am', 'Am', 'F', 'G', 'C', 'G/B', 'F', 'E']
        chB = ['F', 'G', 'Em', 'Am', 'F', 'G', 'E', 'E']
        chC = ['Dm', 'Dm', 'E', 'E', 'F', 'F#dim7', 'G', 'G#dim7']
        chT = ['Am/E', 'E7sus4']
        prog = chA + chA2 + chB + chC + chT
        for i, sym in enumerate(prog):
            sec = i // 8
            v = (0.85, 0.95, 0.8, 0.9 + 0.012 * (i - 24), 1.0)[sec]
            _battle_rhythm(s, g, b0 + i, sym, v=v, drums=True, arp_on=(sec != 4), seed=1, snr=(sec != 2 or i % 2 == 1))
        _battle_turn(s, g, b0 + 34)
        for i in (34, 35):   # low engine keeps going under the last two bars
            _battle_rhythm(s, g, b0 + i, 'E7', v=1.0, drums=False, arp_on=False, seed=2)
        # A: violins, with violas an octave below
        mv = dict(voices=7, attack=0.03, release=0.12, marc=0.35, vib=0.006, vib_delay=0.12)
        play(s, g, strings, A, b0, vel=0.82, gain=1.0, send=0.3, part='melody', legato=0.92, center=-0.3, seed=1, **mv)
        play(s, g, strings, A, b0, vel=0.78, gain=0.75, send=0.3, part='melody', legato=0.92, center=0.15,
             transpose=-12, seed=2, **mv)
        # A': add the octave above and trumpets
        play(s, g, strings, A2, b0 + 8, vel=0.88, gain=1.0, send=0.3, part='melody', legato=0.92, center=-0.3,
             seed=3, **mv)
        play(s, g, strings, A2, b0 + 8, vel=0.85, gain=0.7, send=0.3, part='melody', legato=0.92, center=-0.35,
             transpose=12, seed=4, **mv)
        play(s, g, brass, A2, b0 + 8, vel=0.8, gain=0.8, send=0.35, part='trumpets', legato=0.9, transpose=-12,
             voices=3, bright=1.2, attack=0.025, release=0.1, center=0.1, seed=5)
        # brass punctuation through A and A' (3+3+2) and held roots
        for i in range(16):
            sym = (chA + chA2)[i]
            tones = chord_tones(sym, 'A2', 6)
            tones = [tones[0], tones[2], tones[3], tones[4]]      # open: root, fifth, octave, tenth
            for beat, a in ((0, 1.0), (1.5, 0.75), (3, 0.8)) if i % 2 == 1 else ((0, 1.0),):
                for k, p in enumerate(tones):
                    s.add(brass(hz(p), g.d(0.7 if beat else 1.2), (0.7 + 0.1 * (i >= 8)) * a, voices=3,
                                bright=0.95, center=0.3 - 0.1 * k, attack=0.02, release=0.12, seed=i * 3 + k),
                          g.t(b0 + i, beat), 0.62, 0.0, 0.3, 'brass')
        harmony(s, g, [(c, 4) for c in chA + chA2], b0, start=('E4', 'A4', 'C5'), lo='C4', hi='E5', vel=0.55,
                gain=0.55, send=0.3, part='strings-harm', center=0.1, voices=6, trem=0.5, trem_rate=10.67, seed=7)
        # B: long line, choir, horns answering
        play(s, g, strings, B, b0 + 16, vel=0.85, dyn=[(0, 0.8), (16, 0.86), (24, 0.95), (32, 0.9)], gain=1.0,
             send=0.35, part='melody', legato=1.0, center=-0.3, voices=8, attack=0.08, release=0.25, seed=11)
        play(s, g, strings, B, b0 + 16, vel=0.8, gain=0.75, send=0.35, part='melody', legato=1.0, center=-0.35,
             transpose=12, voices=7, attack=0.08, release=0.25, seed=12)
        harmony(s, g, [(c, 4) for c in chB], b0 + 16, start=('C4', 'F4', 'A4', 'C5'), lo='A3', hi='E5', inst=choir,
                vel=0.7, gain=0.8, send=0.45, part='choir', vowel='ah', voices=6, attack=0.15, release=0.3, seed=13)
        harmony(s, g, [(c, 4) for c in chB], b0 + 16, start=('F3', 'A3', 'C4'), lo='C3', hi='E4', inst=brass,
                vel=0.6, gain=0.6, send=0.35, part='horns', voices=4, bright=0.5, center=-0.2, attack=0.08, seed=14)
        play(s, g, brass, 'r:2 F4:1 A4:1 | G4:2 r:2 | r:2 E4:1 G4:1 | E4:2 r:2 | r:2 A4:1 C5:1 | B4:2 r:2 | G#4:2 B4:2 | '
                          'E5:4', b0 + 16, vel=0.75, gain=0.7, send=0.35, part='horns', voices=4, bright=0.6,
             center=-0.25, legato=0.95, seed=15)
        hit(s, g.t(b0 + 16), cymbal(4.0, 0.7, seed=3), 0.7, 0.0, 0.3, 'cymbal')
        # C: the sequence climbs; everything in octaves, brass on the bass line
        dC = [(0, 0.7), (16, 0.85), (32, 1.0)]
        for tr, gn, c in ((0, 1.0, -0.3), (-12, 0.8, 0.1)):
            play(s, g, strings, C, b0 + 24, vel=0.85, dyn=dC, gain=gn, send=0.3, part='melody', legato=0.85,
                 transpose=tr, center=c, voices=7, attack=0.015, release=0.08, marc=0.5, vib=0, seed=21 + tr)
        play(s, g, brass, C, b0 + 24, vel=0.8, dyn=dC, gain=0.6, send=0.3, part='trumpets', legato=0.8, transpose=-12,
             voices=3, bright=1.2, attack=0.015, release=0.08, center=0.1, seed=22)
        bassline(s, g, [(c, 4) for c in chC], b0 + 24, ref='E2', inst=brass, vel=0.8, dyn=dC, gain=0.8, send=0.3,
                 part='brass', voices=3, bright=1.0, center=0.3, attack=0.03, seed=23, octave_below=False)
        harmony(s, g, [(c, 4) for c in chC], b0 + 24, start=('D4', 'F4', 'A4'), lo='A3', hi='D5', inst=brass,
                vel=0.7, dyn=dC, gain=0.5, send=0.3, part='brass', voices=3, bright=0.9, center=0.2, attack=0.03,
                seed=24)
        hit(s, g.t(b0 + 24), cymbal(3.5, 0.6, seed=4), 0.6, 0.0, 0.3, 'cymbal')
        hit(s, g.t(b0 + 28), timp_roll(hz('F2'), g.d(16) - 0.1, 0.8, swell=(0.25, 0.5, 1.0), seed=6), 0.7, -0.1,
            0.3, 'timpani')
        # turn: violins hold the dominant above
        for p, c in (('E6', -0.35), ('B5', -0.3), ('E5', -0.2)):
            s.add(strings(hz(p), g.d(6), 0.9, voices=6, attack=0.05, release=0.15, trem=0.7, center=c,
                          swell=(0.7, 1.0), seed=5), g.t(b0 + 32), 0.6, 0.0, 0.3, 'violins')
        for bb in range(2):
            for beat in (0, 1.5, 3):
                t = g.t(b0 + 32 + bb, beat)
                for p, c in (('E2', 0.3), ('E3', 0.3), ('A3' if bb == 0 else 'B3', 0.2), ('E4', 0.1)):
                    s.add(brass(hz(p), g.d(0.6), 0.9, voices=3, bright=1.0, center=c, attack=0.02, release=0.12,
                                seed=int(beat * 2) + bb), t, 0.75, 0.0, 0.3, 'brass')
        # downbeats of the sections
        hit(s, g.t(b0), cymbal(4.0, 0.8, seed=1), 0.75, 0.0, 0.3, 'cymbal')
        hit(s, g.t(b0), taiko(50.0, 1.0, 1.8, seed=9), 1.1, 0.0, 0.3, 'taiko')
        hit(s, g.t(b0 + 8), cymbal(4.0, 0.7, seed=2), 0.65, 0.0, 0.3, 'cymbal')

    # intro: two bars of gathering on E, then the same turn that ends the loop
    si = Song(g.t(n_intro + 1), tail=4.0, reverbs=hall)
    for p, c in (('E2', 0.3), ('B2', 0.3), ('E3', 0.2)):
        si.add(brass(hz(p), g.d(8), 0.9, voices=3, bright=1.0, center=c, attack=0.3, swell=(0.15, 0.45, 1.0)),
               0.0, 0.8, 0.0, 0.3, 'brass')
    for p, c in (('E4', 0.1), ('B4', -0.2), ('E5', -0.3), ('G#5', -0.35)):
        si.add(strings(hz(p), g.d(8), 0.9, voices=6, attack=0.3, release=0.1, trem=0.8, center=c,
                       swell=(0.1, 0.4, 1.0), seed=3), 0.0, 0.6, 0.0, 0.3, 'violins')
    hit(si, 0.0, timp_roll(hz('E2'), g.d(8) - 0.1, 0.85, swell=(0.15, 0.5, 1.0)), 0.9, -0.1, 0.3, 'timpani')
    hit(si, 0.0, tamtam(52.0, 5.0, 0.5), 0.6, 0.0, 0.3, 'tamtam')
    for i, beat in enumerate((0, 2, 4, 5, 6, 6.5, 7, 7.5)):
        hit(si, g.t(1, beat), taiko(56.0, 0.5 + 0.06 * i, 1.0, seed=i), 0.9, 0.0, 0.25, 'taiko')
    _battle_turn(si, g, 3)
    for i in (3, 4):
        _battle_rhythm(si, g, i, 'E7', v=1.0, drums=False, arp_on=False, seed=2)
    intro = si.mix(loop=False)

    sb = Song(g.t(37), tail=6.0, reverbs=hall, wrap=True)
    theme_parts(sb, 1)
    print(sb.report())
    body = sb.mix(loop=True)
    y = join_intro(intro, body)
    return result(y, True, loop_start=len(intro) / SR, target=-15.0, comp_thresh=-3.0)


# ---------------------------------------------------------------- victory

def victory():
    """6 s fanfare: the motif's first steps turned to the major and allowed
    to arrive (A D E | F G A over Bb - C - D), with a roll and a crash."""
    s = Song(6.0, tail=0.5, reverbs={'hall': make_ir(2.4, 0.02, 0.6, 5)})
    tp = dict(voices=3, bright=1.25, attack=0.02, release=0.1, center=-0.1)
    hn = dict(voices=4, bright=0.6, attack=0.03, release=0.12, center=-0.3)
    tb = dict(voices=3, bright=0.95, attack=0.03, release=0.12, center=0.3)
    t = 0.0
    for i, p in enumerate(('A4', 'D5', 'E5')):
        s.add(brass(hz(p), 0.12, 0.8 + 0.05 * i, seed=i, **tp), t + 0.14 * i, 0.9, 0.0, 0.3, 'trumpets')
        s.add(brass(hz(p) / 2, 0.12, 0.75, seed=i + 5, **hn), t + 0.14 * i, 0.8, 0.0, 0.3, 'horns')
        hit(s, t + 0.14 * i, snare(0.5 + 0.12 * i, 0.25, seed=i), 0.5, 0.1, 0.25, 'snare')
    t1, t2, t3, tend = 0.42, 1.22, 2.02, 4.45
    chords = ((t1, t2 - t1, 'F5', ('Bb3', 'D4', 'F4'), 'Bb2', 0.85),
              (t2, t3 - t2, 'G5', ('C4', 'E4', 'G4'), 'C3', 0.9),
              (t3, tend - t3, 'A5', ('D4', 'F#4', 'A4'), 'D3', 1.0))
    for (t0, d, top, mid, bs, v) in chords:
        last = t0 == t3
        sw = (0.85, 0.75, 1.0) if last else None
        s.add(brass(hz(top), d * 0.97, v, swell=sw, seed=1, **tp), t0, 1.0, 0.0, 0.3, 'trumpets')
        s.add(brass(hz(top) / 2, d * 0.97, v * 0.9, swell=sw, seed=2, **hn), t0, 0.7, 0.0, 0.3, 'horns')
        for k, p in enumerate(mid):
            s.add(brass(hz(p), d * 0.97, v * 0.85, swell=sw, seed=3 + k, **(hn if k else tb)), t0, 0.62, 0.0, 0.3,
                  'brass')
            s.add(strings(hz(p) * 2, d * 0.97, v * 0.85, voices=7, attack=0.03, release=0.2, trem=0.6 if last else 0,
                          center=-0.25, swell=sw, seed=k), t0, 0.55, 0.0, 0.3, 'strings')
        s.add(brass(hz(bs), d * 0.97, v, swell=sw, seed=7, **tb), t0, 0.8, 0.0, 0.25, 'brass')
        s.add(brass(hz(bs) / 2, d * 0.97, v, swell=sw, seed=8, voices=2, bright=0.6, attack=0.03, center=0.3), t0,
              0.7, 0.0, 0.2, 'brass')
        s.add(strings(hz(bs), d * 0.97, v, voices=5, attack=0.02, release=0.2, marc=0.6, center=0.35, seed=9), t0,
              0.8, 0.0, 0.25, 'strings')
        hit(s, t0, timpani(hz(bs), 0.85 * v, 2.0, seed=int(t0 * 10)), 1.0, -0.1, 0.3, 'timpani')
        hit(s, t0, taiko(56.0, 0.8 * v, 1.2, seed=int(t0 * 10)), 0.8, 0.0, 0.25, 'taiko')
    hit(s, t3, cymbal(3.8, 0.85, seed=1), 0.8, 0.0, 0.35, 'cymbal')
    hit(s, t3 + 0.3, timp_roll(hz('D3'), tend - t3 - 0.35, 0.75, swell=(0.5, 0.6, 1.0), seed=2), 0.8, -0.1, 0.3,
        'timpani')
    # the trumpets climb to the top D; glass bells run up the scale
    s.add(brass(hz('D6'), tend - 3.25, 0.95, swell=(0.8, 1.0), seed=4, **tp), 3.25, 0.8, 0.0, 0.35, 'trumpets')
    s.add(brass(hz('F#5'), tend - 3.25, 0.9, swell=(0.8, 1.0), seed=5, **tp), 3.25, 0.6, 0.0, 0.35, 'trumpets')
    for i, p in enumerate(('D5', 'E5', 'F#5', 'A5', 'B5', 'D6', 'E6', 'F#6', 'A6', 'D7')):
        hit(s, t3 + 0.05 + 0.085 * i, fm_bell(hz(p), 2.5, 0.5, decay=0.9, index=2.0, seed=i), 0.35,
            -0.5 + 0.1 * i, 0.5, 'bell')
        hit(s, t3 + 0.05 + 0.085 * i, harp(hz(p) / 2, 2.0, 0.7, seed=i), 0.7, 0.0, 0.4, 'harp')
    y = s.mix(loop=False)[:int(6.0 * SR)]
    nf = int(0.6 * SR)
    y[-nf:] *= fade_out(nf)[:, None]
    return result(y, False, target=-15.0, comp_thresh=0.0)


# --------------------------------------------------------------- colossus

def _steam(vel, seed):
    rng = np.random.default_rng(seed + 900)
    n = int(0.22 * SR)
    t = tt(n)
    y = bp(noise(n, rng), 2500, 8000, 1) * np.exp(-t / 0.05) * np.minimum(1.0, t / 0.004)
    return y * 0.12 * vel


def _colossus_engine(s, g, bar, root, v=1.0, full=True, seed=0):
    """One 7/4 bar (2+2+3) of the machine: low strings and a driven synth
    bass hammering eighths, the last two climbing a minor third and a
    tritone; taiko and struck iron on the three accents, steam between."""
    rng = np.random.default_rng(seed * 17 + bar)
    hits = (0, 2, 3, 4, 6, 7, 8, 10, 11, 12, 13) if full else (0, 4, 8, 12, 13)
    for e in hits:
        beat = e / 2.0
        p = root + (3 if e == 12 else 6 if e == 13 else 0)
        acc = 1.0 if e in (0, 4, 8) else 0.88 if e in (12, 13) else 0.62
        t = g.t(bar, beat) + rng.uniform(-0.003, 0.003)
        s.add(spic(p, 0.85 * v * acc, 0.17 if acc == 1.0 else 0.11)(bar * 14 + e), t, 0.8, 0.0, 0.2, 'engine')
        s.add(spic(p + 12, 0.8 * v * acc, 0.12, center=0.2)(bar * 14 + e + 1), t, 0.45, 0.0, 0.2, 'engine')
        s.add(cached(sub_bass, round(hz(p), 3), 0.16, round(0.8 * v * acc, 2), drive=3.0, variants=1), t, 0.5, 0.0,
              0.05, 'synth-bass')
        if e in (0, 4, 8):
            s.add(spic(p - 12, 0.9 * v, 0.2, center=0.45)(bar + e), t, 0.6, 0.0, 0.2, 'engine')
            hit(s, t, cached(taiko, 48.0, round(0.95 * v, 2), 1.5, seed=bar + e), 0.62, 0.0, 0.28, 'taiko')
            hit(s, t, cached(anvil, 587.0 if e != 4 else 440.0, round(0.6 * v, 2), 0.8, seed=e), 0.5,
                -0.4 if e != 4 else 0.4, 0.5, 'iron')
        elif e in (12, 13):
            hit(s, t, cached(taiko, 62.0, round(0.8 * v, 2), 1.0, seed=bar + e), 0.7, 0.0, 0.28, 'taiko')
        elif full:
            hit(s, t, cached(taiko, 92.0, round(0.4 * v, 2), 0.5, seed=bar + e, rim=1.0), 0.45, 0.25, 0.25, 'taiko')
    for e in (1, 5, 9):
        hit(s, g.t(bar, e / 2.0), _steam(0.7 * v, bar + e), 0.8, 0.5 if e == 5 else -0.5, 0.4, 'steam')


def _colossus_turn(s, g, bar, v=1.0):
    """The bar that throws the music back to the top: tutti on Gb (the
    tritone) twice, then G, and the two-eighth fill."""
    for beat, sym, dur in ((0, ('Gb1', 'Gb2', 'Db3', 'Gb3', 'Bb3'), 1.6), (2, ('Gb1', 'Gb2', 'Db3', 'Gb3', 'Bb3'), 1.6),
                           (4, ('G1', 'G2', 'D3', 'G3', 'B3'), 1.9)):
        t = g.t(bar, beat)
        for k, p in enumerate(sym):
            s.add(brass(hz(p), g.d(dur), 0.95 * v, voices=3, bright=1.1, center=0.3 - 0.12 * k, attack=0.03,
                        release=0.15, seed=k + beat), t, 0.8, 0.0, 0.3, 'brass')
        for k, p in enumerate(sym[2:]):
            s.add(strings(hz(p) * 4, g.d(dur), 0.9 * v, voices=6, attack=0.02, release=0.15, trem=0.7,
                          center=-0.3, seed=k + beat), t, 0.5, 0.0, 0.3, 'violins')
            s.add(choir(hz(p) * 2, g.d(dur), 0.85 * v, voices=5, attack=0.05, release=0.25, vowel='ah',
                        seed=k + beat), t, 0.6, 0.0, 0.4, 'choir')
        hit(s, t, taiko(46.0, 1.0 * v, 1.6, seed=beat), 1.0, 0.0, 0.3, 'taiko')
        hit(s, t, timpani(hz(sym[1]), 0.9 * v, 2.0, seed=beat), 0.9, -0.1, 0.3, 'timpani')
        hit(s, t, anvil(392.0, 0.8 * v, 1.0, seed=beat), 0.6, 0.3, 0.5, 'iron')
    for i, beat in enumerate((6, 6.5)):
        hit(s, g.t(bar, beat), taiko(60.0, 0.95 * v, 1.2, seed=i), 1.0, 0.0, 0.3, 'taiko')
        hit(s, g.t(bar, beat), timpani(hz('G2'), 0.85 * v, 1.5, seed=i), 0.9, -0.1, 0.3, 'timpani')
        s.add(brass(hz('G1'), g.d(0.4), 0.95 * v, voices=3, bright=1.1, center=0.3, attack=0.02), g.t(bar, beat), 0.8,
              0.0, 0.3, 'brass')
        s.add(brass(hz('G2'), g.d(0.4), 0.95 * v, voices=3, bright=1.1, center=0.25, attack=0.02), g.t(bar, beat),
              0.8, 0.0, 0.3, 'brass')
    hit(s, g.t(bar, 3.0), cymbal(g.d(4) + 2.5, 0.6 * v, swell=g.d(4), seed=bar, dark=0.3), 0.5, 0.0, 0.3, 'cymbal')


def colossus():
    """90 s: a 2-bar intro (7.5 s) and a 22-bar loop (82.5 s), C minor,
    7/4 (2+2+3) at 112 BPM. A (4) the machine and its theme in low brass:
    C G | Ab Gb | F Eb Db | C; A2 (4) with men's choir, horns and iron
    bells; B (6) the engine idles under a choir chant (Ab, Fm, Db, G);
    C (4) everything climbs by semitones; D (3) the theme tutti, and one
    bar on the tritone that throws it back."""
    g = Grid(112, 7)
    hall = {'hall': make_ir(3.0, 0.025, 0.6, 15)}
    TH = 'C3:4 G3:3 | Ab3:4 Gb3:3 | F3:2 Eb3:2 Db3:3 | '
    C_ = int(midi('C2'))

    sb = Song(g.t(23), tail=7.0, reverbs=hall, wrap=True)
    s = sb
    # --- A (1-4), A2 (5-8)
    for bar in range(1, 9):
        _colossus_engine(s, g, bar, C_, v=0.88 if bar <= 4 else 0.95, seed=1)
    tb = dict(voices=3, bright=1.0, attack=0.05, release=0.2, center=0.3)
    for b0, last, v in ((1, 'C3:7', 0.85), (5, 'C3:4 G3:3', 0.92)):
        play(s, g, brass, TH + last, b0, vel=v, gain=1.0, send=0.35, part='theme-brass', legato=0.96, seed=b0, **tb)
        play(s, g, brass, TH + last, b0, vel=v, gain=0.9, send=0.3, part='theme-brass', legato=0.96, transpose=-12,
             voices=2, bright=0.7, attack=0.06, release=0.2, center=0.35, seed=b0 + 1)
    play(s, g, brass, TH + 'C3:4 G3:3', 5, vel=0.85, gain=0.7, send=0.35, part='horns', legato=0.96, transpose=12,
         voices=4, bright=0.55, attack=0.05, center=-0.25, seed=7)
    for tr, gn in ((0, 0.9), (12, 0.75)):
        play(s, g, choir, TH + 'C3:4 G3:3', 5, vel=0.8, gain=gn, send=0.45, part='choir', legato=0.98, transpose=tr,
             voices=6, attack=0.12, release=0.3, vowel='oh', seed=9 + tr)
    for p, c in (('G5', -0.3), ('C6', -0.35)):
        s.add(strings(hz(p), g.d(28), 0.75, voices=6, attack=1.0, release=0.4, trem=0.8, center=c,
                      swell=(0.3, 0.6, 1.0), seed=3), g.t(5), 0.4, 0.0, 0.35, 'violins')
    for bar in range(5, 9):
        for beat, p in ((0, 'C6'), (2, 'Gb6'), (4, 'Eb6')):
            hit(s, g.t(bar, beat), fm_bell(hz(p), 2.5, 0.5, ratio=2.76, index=2.6, decay=0.8, seed=bar), 0.3,
                -0.4 + 0.2 * beat, 0.6, 'bell')
    hit(s, g.t(1), tamtam(50.0, 8.0, 0.8, seed=1), 0.9, 0.0, 0.35, 'tamtam')
    hit(s, g.t(5), cymbal(4.0, 0.6, seed=1, dark=0.4), 0.6, 0.0, 0.3, 'cymbal')
    # --- B (9-14): chant
    chB = ['Ab', 'Ab', 'Fm', 'Fm', 'Db', 'G']
    for i, sym in enumerate(chB):
        root = bass_pitch(sym, 'C2')
        _colossus_engine(s, g, 9 + i, root, v=0.6 + 0.04 * i, full=(i == 5), seed=2)
        hit(s, g.t(9 + i), timpani(hz(root + 12), 0.6, 2.4, seed=i), 0.8, -0.1, 0.3, 'timpani')
    dB = [(0, 0.6), (14, 0.7), (28, 0.78), (35, 0.8), (42, 0.95)]
    harmony(s, g, [(c, 7) for c in chB], 9, start=('Ab3', 'C4', 'Eb4'), lo='F3', hi='Ab4', inst=choir, vel=0.7,
            dyn=dB, gain=0.9, send=0.5, part='choir', vowel='oh', vowel2='ah', voices=6, attack=0.25, release=0.5,
            seed=21)
    chant = 'Eb5:7 | Eb5:4 F5:3 | Ab5:7 | G5:4 F5:3 | F5:4 Ab5:3 | G5:7'
    play(s, g, choir, chant, 9, vel=0.75, dyn=dB, gain=1.0, send=0.5, part='choir', legato=1.0, voices=7, attack=0.2,
         release=0.5, vowel='ah', seed=23)
    play(s, g, choir, chant, 9, vel=0.7, dyn=dB, gain=0.8, send=0.5, part='choir', legato=1.0, transpose=-12, voices=6,
         attack=0.2, release=0.5, vowel='oh', seed=24)
    play(s, g, strings, chant, 9, vel=0.6, dyn=dB, gain=0.6, send=0.4, part='violins', legato=1.0, voices=7,
         attack=0.25, release=0.4, center=-0.3, seed=25)
    bassline(s, g, [(c, 7) for c in chB], 9, ref='C2', inst=brass, vel=0.65, dyn=dB, gain=0.8, send=0.3,
             part='low-brass', voices=3, bright=0.7, center=0.3, attack=0.12, seed=26, octave_below=False)
    harmony(s, g, [(c, 7) for c in chB], 9, start=('Eb3', 'Ab3', 'C4'), lo='C3', hi='Eb4', vel=0.5, dyn=dB, gain=0.7,
            send=0.35, part='strings-harm', voices=6, center=0.2, attack=0.3, seed=27)
    for i, sym in enumerate(chB):
        tones = chord_tones(sym, 'C5', 5)
        pat = (0, 2, 1, 3, 2, 4, 3, 2, 1, 3, 2, 1, 0, 2)
        for e in range(14):
            hit(s, g.t(9 + i, e / 2.0), cached(fm_bell, round(hz(tones[pat[e]] + 12), 2), 1.2, 0.4, ratio=2.76,
                                               index=1.8, decay=0.35, variants=1), 0.22, 0.5 * np.sin(e * 0.9), 0.5,
                'bell')
    hit(s, g.t(14, 3.0), cymbal(g.d(4) + 2.0, 0.5, swell=g.d(4), seed=3, dark=0.3), 0.5, 0.0, 0.3, 'cymbal')
    # --- C (15-18): climb by semitones
    for i in range(4):
        bar = 15 + i
        root = C_ + i
        v = 0.85 + 0.05 * i
        _colossus_engine(s, g, bar, root, v=v, seed=3)
        tri = (root + 12, root + 19, root + 24, root + 27)
        for beat in (0, 2, 4):
            for k, p in enumerate(tri):
                s.add(brass(hz(p), g.d(1.2 if beat < 4 else 2.4), v, voices=3, bright=1.05, center=0.3 - 0.15 * k,
                            attack=0.025, release=0.15, seed=bar + k + beat), g.t(bar, beat), 0.62, 0.0, 0.3, 'brass')
        for k, p in enumerate((root + 31, root + 36)):
            s.add(choir(hz(p), g.d(7), v * 0.85, voices=6, attack=0.2, release=0.3, vowel='ah',
                        swell=(0.6, 1.0), seed=bar + k), g.t(bar), 0.6, 0.0, 0.45, 'choir')
    climb = 'C5:4 Eb5:3 | Db5:4 E5:3 | D5:4 F5:3 | Eb5:4 Gb5:3'
    for tr, gn, c in ((0, 0.8, -0.2), (12, 0.7, -0.35)):
        play(s, g, strings, climb, 15, vel=0.85, dyn=[(0, 0.7), (28, 1.0)], gain=gn, send=0.35, part='violins',
             legato=1.0, transpose=tr, voices=7, attack=0.08, release=0.2, trem=0.6, center=c, seed=31 + tr)
    hit(s, g.t(15), cymbal(3.5, 0.6, seed=4, dark=0.3), 0.6, 0.0, 0.3, 'cymbal')
    hit(s, g.t(17), timp_roll(hz('D2'), g.d(14) - 0.1, 0.8, swell=(0.2, 0.5, 1.0), seed=5), 0.8, -0.1, 0.3,
        'timpani')
    hit(s, g.t(18, 3.0), cymbal(g.d(4) + 3.0, 0.7, swell=g.d(4), seed=5), 0.6, 0.0, 0.3, 'cymbal')
    # --- D (19-21): the theme, everyone
    for bar in range(19, 22):
        _colossus_engine(s, g, bar, C_, v=1.0, seed=4)
    for tr, gn, kw in ((0, 1.0, tb), (-12, 0.9, dict(voices=2, bright=0.8, attack=0.06, center=0.35)),
                       (12, 0.85, dict(voices=4, bright=0.75, attack=0.05, center=-0.25)),
                       (24, 0.6, dict(voices=3, bright=1.2, attack=0.03, center=-0.05))):
        play(s, g, brass, TH, 19, vel=1.0, gain=gn, send=0.35, part='theme-brass', legato=0.96, transpose=tr,
             seed=40 + tr, **kw)
    for tr, gn, vw in ((0, 0.9, 'oh'), (12, 0.9, 'ah'), (24, 0.7, 'ah')):
        play(s, g, choir, TH, 19, vel=0.95, gain=gn, send=0.45, part='choir', legato=0.98, transpose=tr, voices=6,
             attack=0.1, release=0.3, vowel=vw, seed=50 + tr)
    for tr, gn, c in ((24, 0.7, -0.3), (36, 0.55, -0.35)):
        play(s, g, strings, TH, 19, vel=0.95, gain=gn, send=0.35, part='violins', legato=1.0, transpose=tr, voices=7,
             attack=0.05, release=0.2, trem=0.7, center=c, seed=60 + tr)
    hit(s, g.t(19), tamtam(50.0, 8.0, 0.9, seed=2), 0.9, 0.0, 0.35, 'tamtam')
    hit(s, g.t(19), cymbal(5.0, 0.85, seed=6), 0.7, 0.0, 0.3, 'cymbal')
    for bar in range(19, 22):
        for beat, p in ((0, 'C7'), (2, 'Gb6'), (4, 'Eb6')):
            hit(s, g.t(bar, beat), fm_bell(hz(p), 2.5, 0.6, ratio=2.76, index=2.6, decay=0.8, seed=bar), 0.3,
                -0.4 + 0.2 * beat, 0.6, 'bell')
    _colossus_turn(s, g, 22)
    print(s.report())
    body = sb.mix(loop=True)

    # --- intro: it wakes (one bar of gathering), then the turn
    si = Song(g.t(3), tail=5.0, reverbs=hall)
    hit(si, 0.0, tamtam(46.0, 9.0, 0.7, seed=3), 0.9, 0.0, 0.4, 'tamtam')
    for p, vw in (('C2', 'oo'), ('G2', 'oo'), ('Db3', 'oh'), ('C3', 'oh')):
        si.add(choir(hz(p), g.d(7), 0.8, voices=6, attack=1.0, release=0.4, vowel=vw, swell=(0.15, 0.45, 1.0),
                     seed=int(midi(p))), 0.0, 0.8, 0.0, 0.5, 'choir')
    for p in ('C1', 'C2'):
        si.add(sub_bass(hz(p), g.d(7), 0.8, attack=1.5, release=0.3, drive=1.2) * np.linspace(0.3, 1.0, int(
            (g.d(7) + 0.3) * SR)), 0.0, 0.6, 0.0, 0.1, 'synth-bass')
    for p, c in (('C2', 0.3), ('G2', 0.3), ('C3', 0.2)):
        si.add(brass(hz(p), g.d(7), 0.9, voices=3, bright=0.9, center=c, attack=0.8, swell=(0.05, 0.3, 1.0)), 0.0,
               0.7, 0.0, 0.3, 'brass')
    _colossus_engine(si, g, 1, C_, v=0.55, full=False, seed=9)
    for i, e in enumerate((6, 7, 10, 11)):
        hit(si, g.t(1, e / 2.0), taiko(92.0, 0.3 + 0.08 * i, 0.5, seed=i, rim=1.0), 0.5, 0.25, 0.25, 'taiko')
    _colossus_turn(si, g, 2)
    intro = si.mix(loop=False)
    y = join_intro(intro, body)
    return result(y, True, loop_start=len(intro) / SR, target=-15.0, comp_thresh=-3.0)


# --------------------------------------------------------------- overload

def _shepard_strings(L, xf, rng):
    """Endlessly rising string cluster: four pitch classes (C, C#, F#, G: two
    tritones a semitone apart), each sounding in every octave under a fixed
    bell-shaped spectrum, all gliding up two octaves per loop. Because the
    spectrum two octaves up is the same spectrum, the loop is seamless and
    the rise never arrives."""
    n, nx = int(L * SR), int(xf * SR)
    N = n + nx
    t = tt(N)
    glide = 4.0 ** (t / L)
    out = np.zeros((N, 2))
    for base in (32.703, 34.648, 46.249, 48.999):
        for k in range(-1, 9):
            f = base * (2.0 ** k) * glide
            lf = np.log2(f / 520.0)
            amp = np.exp(-0.5 * (lf / 1.45) ** 2) * np.clip((9500.0 - f) / 3500.0, 0, 1) * np.clip((f - 22.0) / 20.0, 0, 1)
            if amp.max() < 1e-3:
                continue
            for v in range(2):
                det = 2.0 ** (rng.normal(0, 7.0) / 1200.0)
                y = saw(f * det, rng.uniform(0, 1))
                tr = 1.0 - 0.75 * (0.5 + 0.5 * np.sin(TAU * rng.uniform(10.5, 15.0) * t + rng.uniform(0, TAU))) ** 2
                gl, gr = pan_gains(rng.uniform(-0.9, 0.9))
                y = y * amp * tr
                out[:, 0] += y * gl
                out[:, 1] += y * gr
    out = lp(lp(out, 5200, 2), 8000, 2)
    out = hp(out, 35, 2)
    out = crossfade_loop(out, n, nx)
    return out / (np.sqrt(np.mean(out * out)) + 1e-9)


def _alarm(vel, seed=0):
    """One stroke of the slow alarm: a low horn-like blare that sags in
    pitch, and a high pure tone over it."""
    rng = np.random.default_rng(seed)
    n = int(1.7 * SR)
    t = tt(n)
    f0 = hz('C#3')
    sag = 2.0 ** (-0.35 * np.clip(t / 1.2, 0, 1) / 12.0)
    y = np.zeros((n, 2))
    for ch, dc in enumerate((-7.0, 7.0)):
        fr = f0 * sag * 2.0 ** (dc / 1200.0)
        y[:, ch] = saw(fr, 0.2 + 0.3 * ch) + 0.7 * saw(fr * 0.5, 0.6) + 0.35 * saw(fr * 1.498, 0.1)
    env = np.minimum(1.0, t / 0.03) * (0.25 * np.exp(-t / 0.9) + 0.75 * np.exp(-t / 0.33))
    y = morph_lp(y, env ** 0.7 * (0.55 + 0.45 * vel), 250.0, 4200.0, steps=5)
    y = y + reson(y, 760.0, 3.0, 0.7)
    beep = np.sin(TAU * hz('G5') * t) + 0.5 * np.sin(TAU * hz('G6') * 1.003 * t)
    benv = np.minimum(1.0, t / 0.008) * np.exp(-t / 0.22)
    y = y * env[:, None] * 0.22 + (beep * benv * 0.09)[:, None]
    y[-2000:] *= fade_out(2000)[:, None]
    return y * vel ** 1.2


def overload():
    """60 s loop. No key, no bar lines: a slow alarm every two seconds
    (C#), a heartbeat between its strokes, and strings that only ever rise
    (a Shepard cluster on C, C#, F#, G). Low brass swell and break off four
    times, each time with more notes; a women's cluster slides upward through
    the second half; the last eight seconds accelerate into a blow that is
    also the first beat of the next time round."""
    L = 60.0
    rng = np.random.default_rng(66)
    hall = make_ir(3.4, 0.03, 0.55, 23)
    s = Song(L, tail=9.0, reverbs={'hall': hall}, wrap=True)
    n = int(L * SR)
    t = tt(n)
    rise = np.where(t < 2.5, 1.0 - 0.72 * (t / 2.5) ** 0.6,
                    0.28 + 0.72 * (np.clip(t - 2.5, 0, None) / (L - 2.5)) ** 1.5)

    # the blow at the top
    hit(s, 0.0, taiko(44.0, 1.0, 2.6, seed=1), 1.2, 0.0, 0.35, 'impact')
    hit(s, 0.0, tamtam(44.0, 9.0, 0.9, seed=5), 0.9, 0.0, 0.4, 'impact')
    nd = int(1.8 * SR)
    td = tt(nd)
    drop = np.sin(TAU * np.cumsum(30.0 + 60.0 * np.exp(-td / 0.25)) / SR) * np.exp(-td / 0.5)
    hit(s, 0.0, drop * 0.5, 1.0, 0.0, 0.1, 'impact')
    hit(s, 0.0, anvil(311.0, 0.9, 1.4, seed=2), 0.5, -0.3, 0.6, 'impact')
    hit(s, 0.0, anvil(233.0, 0.9, 1.6, seed=3), 0.5, 0.3, 0.6, 'impact')

    # alarm and heartbeat
    for i in range(30):
        tp = 2.0 * i
        v = 0.6 + 0.4 * (tp / L)
        if i > 0:
            hit(s, tp, _alarm(v, seed=i % 4), 1.35, 0.0, 0.35, 'alarm')
        hv = 0.35 + 0.45 * (tp / L)
        hit(s, tp + 1.0, cached(taiko, 43.0, round(hv, 2), 1.3, seed=i), 0.8, 0.0, 0.3, 'heart')
        hit(s, tp + 1.27, cached(taiko, 41.0, round(hv * 0.7, 2), 1.3, seed=i + 1), 0.8, 0.0, 0.3, 'heart')

    # low brass: four swells, each wider, each cut off
    segs = ((('C2', 'F#2'), 0.6), (('C2', 'F#2', 'C#3'), 0.72), (('C2', 'G2', 'C#3', 'F#3'), 0.86),
            (('C1', 'C2', 'F#2', 'C#3', 'G3', 'C4', 'F#4'), 1.0))
    for i, (ps, v) in enumerate(segs):
        for k, p in enumerate(ps):
            s.add(brass(hz(p), 13.6, v, voices=3, bright=0.8 + 0.1 * i, attack=1.5, release=0.25,
                        swell=(0.04, 0.12, 0.35, 1.0), center=0.3 - 0.1 * k, seed=i * 10 + k), 15.0 * i + 1.2, 0.6,
                  0.0, 0.35, 'brass')
    # men's drone, the whole loop
    for p, vw in (('C#2', 'oo'), ('G#2', 'oo')):
        s.add(choir(hz(p), L, 0.55, voices=6, attack=3.0, release=3.0, vowel=vw, seed=int(midi(p)), breath=1.5), -3.0,
              0.7, 0.0, 0.5, 'choir-men')
    # women's cluster through the second half, sliding up a quarter tone
    gl = np.linspace(1.0, 2.0 ** (0.5 / 12.0), 64)
    for k, p in enumerate(('C5', 'C#5', 'F#5', 'G5')):
        s.add(choir(hz(p), 31.5, 0.85, voices=6, attack=6.0, release=0.5, vowel='oo', vowel2='ah', glide=gl,
                    swell=(0.1, 0.3, 0.6, 1.0), seed=k), 28.0, 0.62, 0.0, 0.55, 'choir-women')
    # high strings: glassy tremolo seconds, entering at 15 s
    for k, (p, t0) in enumerate((('F#6', 15.0), ('G6', 22.0), ('C7', 30.0), ('C#7', 38.0))):
        s.add(strings(hz(p), L - t0 - 0.3, 0.7, voices=5, attack=5.0, release=0.4, trem=0.85, vib=0.002,
                      bright=1.2, center=-0.5 + 0.3 * k, swell=(0.15, 0.5, 1.0), seed=k), t0, 0.3, 0.0, 0.45,
              'violins')
    # the end: roll, swell, accelerating blows
    hit(s, 49.0, timp_roll(hz('C#2'), 10.9, 0.9, swell=(0.08, 0.3, 1.0), seed=3), 0.9, -0.1, 0.3, 'timpani')
    hit(s, 54.0, cymbal(6.0 + 5.0, 0.8, swell=6.0, seed=7, dark=0.2), 0.7, 0.0, 0.35, 'cymbal')
    for i, tp in enumerate((52.0, 54.0, 56.0, 57.0, 58.0, 58.5, 59.0, 59.25, 59.5, 59.75)):
        hit(s, tp, taiko(52.0, 0.6 + 0.04 * i, 1.4, seed=i), 0.95, 0.0, 0.3, 'taiko')
    # Aetherite: unstable glass, more of it as time runs out
    pcs = ('F#6', 'G6', 'C7', 'C#7', 'A#6', 'E7')
    for i in range(46):
        tp = L * rng.uniform(0.02, 1.0) ** 0.6
        p = hz(pcs[rng.integers(0, len(pcs))]) * 2.0 ** (rng.uniform(-0.4, 0.4) / 12.0)
        b = fm_bell(p, 1.8, rng.uniform(0.3, 0.6) * (0.5 + 0.5 * tp / L), ratio=rng.choice([2.76, 3.5, 1.41]),
                    index=rng.uniform(1.5, 4.0), decay=rng.uniform(0.25, 0.7), seed=i)
        hit(s, tp, b, 0.3, rng.uniform(-0.8, 0.8), 0.7, 'glass')
    for i in range(140):
        tp = L * rng.uniform(0.0, 1.0) ** 0.5
        nn = int(rng.uniform(0.008, 0.03) * SR)
        c = bp(noise(nn, rng), 2500, 9000, 1) * np.exp(-tt(nn) / 0.006) * rng.uniform(0.3, 1.0) * (0.3 + 0.7 * tp / L)
        hit(s, tp, c * 0.12, 1.0, rng.uniform(-0.9, 0.9), 0.5, 'crackle')
    print(s.report())
    y = s.mix(loop=True)

    # layers that are exact loops by construction
    sh = _shepard_strings(L, 4.0, rng)
    wet = np.zeros_like(sh)
    from synth import circ_reverb
    wet = circ_reverb(sh, hall)
    y += (sh + 0.45 * wet) * (rise * 0.085)[:, None]
    for f, a in ((4158 / 60.0, 1.0), (4173 / 60.0, 0.8), (2079 / 60.0, 0.4)):   # C#2 beating, exact cycles per loop
        y += (np.sin(TAU * f * t) * a * 0.022 * (0.5 + 0.5 * rise))[:, None]
    return result(y, True, target=-16.0, comp_thresh=-1.0, comp_ratio=1.4)


# ------------------------------------------------------------- last_light

def last_light():
    """150 s, no loop. D minor, 64 BPM (a bar is 3.75 s), so that the full
    theme falls on 75 s (bar 21) and the climax on 120 s (bar 33).

      0.0   bar 1   one held A (solo violin and glass), almost nothing
      9.4   bar 3   the motif on solo cello (A D | E F A | C), unanswered
     20.6   bar 6   solo violin takes it and finishes the sentence; cello under it
     37.5   bar 11  Rowan's farewell: a falling strain (F E D | C D A | Bb C D F | E);
                    the section strings and a piano come in under the two soloists
     52.5   bar 15  the strain again, higher, on the violins; choir, horns
     67.5   bar 19  dominant: roll, the horns call the motif's first notes
     75.0   bar 21  the full theme: the motif in all the strings (Dm, Bb, F, C)
     90.0   bar 25  second phrase, in the relative major, reaching higher
    105.0   bar 29  third phrase: the motif twice more, Dm - Am - Bb - C
    120.0   bar 33  climax: its last note C finally rises to D, in D major
    127     -       the orchestra is gone; one A is left ringing, and fades
    """
    g = Grid(64)
    rev = {'hall': make_ir(3.3, 0.03, 0.55, 13), 'long': make_ir(7.0, 0.05, 0.42, 17)}
    s = Song(150.0, tail=1.0, reverbs=rev)
    VN = dict(center=-0.3)
    # ---------------- the held A (the light): bars 1-20, under everything
    for i, t0 in enumerate((0.0, 17.0, 34.0, 51.0)):
        s.add(glass_pad(hz('A5'), 19.0, 0.42 - 0.04 * i, attack=4.0, release=5.0, seed=i, shimmer=0.7, bright=0.6),
              t0, 1.6 if i == 0 else 1.0, 0.0, 0.3, 'light', send2=0.5, bus2='long')
    for i, (t0, d) in enumerate(((0.0, 10.5), (10.0, 11.5))):
        s.add(solo_string(hz('A5'), d, 0.3, attack=3.5 if i == 0 else 1.2, release=2.0, vib=0.0035, vib_delay=1.5,
                          bright=0.6, seed=i), t0, 1.0, -0.25, 0.6, 'light')
    # ---------------- bars 3-5: solo cello, the motif
    cello = dict(attack=0.14, release=0.35, vib=0.008, vib_delay=0.3, bright=0.8)
    play(s, g, solo_string, 'A2:1.5 D3:.5 | E3:1.5 F3:.5 A3:2', 3, beat=2.0, vel=0.5,
         dyn=[(0, 0.44), (2, 0.5), (4, 0.62)], gain=1.0, pan=0.25, send=0.55, part='solo-cello', legato=1.02,
         human=0.012, seed=1, **cello)
    s.add(solo_string(hz('C4'), g.d(4.6), 0.6, swell=(0.85, 1.0, 0.7, 0.25), seed=7, **cello), g.t(5), 1.0, 0.25,
          0.55, 'solo-cello')
    pk = dict(pedal=True, felt=0.5, maxlen=11.0)
    s.add(piano(hz('D2'), 6.0, 0.26, seed=1, **pk), g.t(4), 1.0, 0.0, 0.5, 'piano')
    s.add(piano(hz('Bb1'), 6.0, 0.26, seed=2, **pk), g.t(5), 1.0, 0.0, 0.5, 'piano')
    s.add(piano(hz('F3'), 6.0, 0.2, seed=3, **pk), g.t(5, 0.06), 1.0, 0.0, 0.5, 'piano')
    # ---------------- bars 6-10: solo violin answers, cello accompanies
    violin = dict(attack=0.1, release=0.3, vib=0.009, vib_delay=0.22, bright=0.9)
    play(s, g, solo_string, 'A4:1.5 D5:.5 | E5:1.5 F5:.5 A5:2 | C6:3 Bb5:.5 A5:.5 | G5:1.5 A5:.5 F5:2', 6, beat=2.0,
         vel=0.55, dyn=[(0, 0.46), (6, 0.62), (9, 0.6), (14, 0.5)], gain=0.95, pan=-0.3, send=0.55,
         part='solo-violin', legato=1.02, human=0.012, seed=11, **violin)
    s.add(solo_string(hz('E5'), g.d(4.3), 0.5, swell=(0.9, 1.0, 0.5), seed=17, **violin), g.t(10), 0.95, -0.3, 0.55,
          'solo-violin')
    play(s, g, solo_string, 'D3:2 F3:2 | Bb2:2 D3:2 | A2:2 C3:1 A2:1 | C3:2 A2:2', 7, vel=0.42, gain=0.9, pan=0.25,
         send=0.5, part='solo-cello', legato=1.02, human=0.012, seed=21, **cello)
    for bar, notes in ((7, ('D2', 'A2', 'F3')), (8, ('Bb1', 'F2', 'D3')), (9, ('A1', 'F2', 'C3')), (10, ('C2', 'G2', 'E3'))):
        for k, p in enumerate(notes):
            s.add(piano(hz(p), 5.0, 0.3 - 0.03 * k, seed=bar + k, **pk), g.t(bar) + 0.07 * k, 1.0, 0.0, 0.45, 'piano')
    # ---------------- bars 11-14: the farewell strain, soloists over the section
    chF1 = [('Bbmaj7', 4), ('F/A', 4), ('Gm7', 4), ('Asus4', 2), ('A', 2)]
    play(s, g, solo_string, 'F5:1.5 E5:.5 D5:2 | C5:1.5 D5:.5 A4:2 | Bb4:1.5 C5:.5 D5:1 F5:1', 11, vel=0.58,
         dyn=[(0, 0.55), (8, 0.52), (12, 0.66)], gain=0.95, pan=-0.3, send=0.55, part='solo-violin', legato=1.02,
         human=0.012, seed=31, **violin)
    s.add(solo_string(hz('E5'), g.d(4.2), 0.62, swell=(0.9, 1.0, 0.7), seed=37, **violin), g.t(14), 0.95, -0.3, 0.55,
          'solo-violin')
    play(s, g, solo_string, 'D4:2 C4:2 | A3:2 F3:2 | G3:2 Bb3:2 | D4:2 C#4:2', 11, vel=0.48, gain=0.9, pan=0.25,
         send=0.5, part='solo-cello', legato=1.02, human=0.012, seed=41, **cello)
    dF1 = [(0, 0.3), (12, 0.36), (16, 0.42)]
    harmony(s, g, chF1, 11, start=('D4', 'F4', 'A4'), lo='A3', hi='D5', vel=0.33, dyn=dF1, gain=0.85, send=0.45,
            part='strings', voices=7, attack=0.6, release=0.6, center=0.1, bright=0.6, seed=51)
    bassline(s, g, chF1, 11, ref='A2', vel=0.33, dyn=dF1, gain=0.8, send=0.35, part='bass', voices=5, center=0.4,
             bright=0.5, attack=0.5, release=0.5, seed=52)
    arp(s, g, chF1, 11, lambda f, d, v, seed=0: piano(f, 3.0, v, seed=seed, **pk), (0, 1, 2, 3, 4, 3, 2, 3),
        step=0.5, low='A2', vel=0.3, dyn=[(0, 0.27), (16, 0.36)], gain=1.0, send=0.4, part='piano', seed=53)
    # ---------------- bars 15-18: the strain again, higher, with the violins
    chF2 = [('Bb', 4), ('C', 4), ('Dm', 4), ('Gm/Bb', 4)]
    strain2 = 'A5:1.5 G5:.5 F5:2 | G5:1.5 F5:.5 E5:2 | F5:1.5 E5:.5 D5:1 F5:1 | G5:2 Bb5:1 A5:1'
    dF2 = [(0, 0.4), (8, 0.46), (12, 0.52), (16, 0.6)]
    play(s, g, strings, strain2, 15, vel=0.5, dyn=dF2, gain=1.0, send=0.45, part='violins', legato=1.0, voices=8,
         attack=0.16, release=0.35, seed=61, **VN)
    play(s, g, strings, strain2, 15, vel=0.45, dyn=dF2, gain=0.7, send=0.45, part='violins', legato=1.0, voices=7,
         attack=0.16, release=0.35, transpose=-12, center=-0.1, seed=62)
    play(s, g, solo_string, strain2, 15, vel=0.5, dyn=dF2, gain=0.5, pan=-0.3, send=0.55, part='solo-violin',
         legato=1.02, transpose=12, seed=63, **violin)
    play(s, g, strings, 'D4:2 F4:2 | E4:2 C4:2 | A3:2 D4:2 | D4:2 G4:1 E4:1', 15, vel=0.5, dyn=dF2, gain=0.75,
         send=0.4, part='cellos', legato=1.0, voices=6, attack=0.14, release=0.3, center=0.35, seed=64)
    harmony(s, g, chF2, 15, start=('D4', 'F4', 'Bb4'), lo='A3', hi='D5', vel=0.45, dyn=dF2, gain=0.8, send=0.45,
            part='strings', voices=7, attack=0.4, release=0.5, center=0.1, bright=0.8, seed=65)
    bassline(s, g, chF2, 15, ref='A2', vel=0.45, dyn=dF2, gain=0.85, send=0.35, part='bass', voices=5, center=0.4,
             bright=0.6, attack=0.3, release=0.4, seed=66)
    harmony(s, g, chF2, 15, start=('F3', 'Bb3', 'D4'), lo='C3', hi='F4', inst=brass, vel=0.42, dyn=dF2, gain=0.5,
            send=0.45, part='horns', voices=4, bright=0.4, attack=0.3, release=0.4, center=-0.2, seed=67)
    harmony(s, g, chF2, 15, start=('F4', 'Bb4', 'D5'), lo='C4', hi='F5', inst=choir, vel=0.42,
            dyn=[(0, 0.3), (16, 0.55)], gain=0.7, send=0.55, part='choir', vowel='oo', voices=6, attack=1.0,
            release=0.8, seed=68)
    arp(s, g, chF2, 15, lambda f, d, v, seed=0: piano(f, 3.0, v, seed=seed, **pk), (0, 1, 2, 3, 4, 5, 4, 3),
        step=0.5, low='A2', vel=0.4, dyn=[(0, 0.36), (16, 0.5)], gain=1.0, send=0.4, part='piano', seed=69)
    for bar in (17, 18):
        hit(s, g.t(bar), timpani(hz('D2' if bar == 17 else 'Bb2'), 0.4, 2.6, seed=bar), 0.8, -0.1, 0.4, 'timpani')
    # ---------------- bars 19-20: the dominant
    chD = [('Asus4', 4), ('A', 2), ('A7sus4', 2)]
    dD = [(0, 0.5), (4, 0.58), (6.5, 0.72), (8, 1.0)]
    for tr, gn, c in ((0, 1.0, -0.3), (-12, 0.7, -0.1)):
        s.add(strings(hz('A5') * 2.0 ** (tr / 12.0), g.d(6), 0.85, voices=8, attack=0.2, release=0.3, center=c,
                      swell=(0.4, 0.5, 0.65, 1.0), seed=71 + tr), g.t(19), gn, 0.0, 0.45, 'violins')
    harmony(s, g, chD, 19, start=('D4', 'E4', 'A4'), lo='A3', hi='D5', vel=0.7, dyn=dD, gain=0.85, send=0.45,
            part='strings', voices=7, attack=0.25, release=0.3, center=0.1, seed=72)
    harmony(s, g, chD, 19, start=('A2', 'E3', 'A3'), lo='E2', hi='D4', inst=brass, vel=0.6, dyn=dD, gain=0.6,
            send=0.4, part='trombones', voices=3, bright=0.8, attack=0.2, center=0.25, seed=73)
    harmony(s, g, chD, 19, start=('E4', 'A4', 'D5'), lo='C4', hi='F5', inst=choir, vel=0.6, dyn=dD, gain=0.75,
            send=0.55, part='choir', vowel='oo', vowel2='ah', voices=6, attack=0.5, release=0.4, seed=74)
    bassline(s, g, [('A', 4)], 19, ref='A1', vel=0.7, dyn=[(0, 0.6), (4, 0.75)], gain=0.9, send=0.35, part='bass',
             voices=5, center=0.4, attack=0.2, seed=75)
    for tr, gn in ((0, 0.9), (-12, 0.8)):
        play(s, g, strings, 'A2:.5 B2:.5 C#3:.5 D3:.5 E3:.5 F3:.5 G3:.5 A3:.5', 20, vel=0.8,
             dyn=[(0, 0.7), (4, 1.0)], gain=gn, send=0.35, part='bass', legato=1.0, transpose=tr, voices=6,
             attack=0.05, release=0.15, marc=0.3, center=0.4, seed=76 + tr)
    play(s, g, brass, 'A3:1.5 D4:.5 E4:2 | E4:2', 19, vel=0.7, dyn=[(0, 0.6), (4, 0.8)], gain=0.9, send=0.5,
         part='horns', legato=0.98, voices=4, bright=0.5, attack=0.06, center=-0.25, seed=77)
    hit(s, g.t(19), timp_roll(hz('A2'), g.d(8) - 0.05, 0.9, swell=(0.05, 0.12, 0.3, 1.0), seed=3), 0.9, -0.1, 0.35, 'timpani')
    hit(s, g.t(20, 0.0), cymbal(g.d(4) + 3.0, 0.7, swell=g.d(4), seed=4), 0.6, 0.0, 0.35, 'cymbal')
    arp(s, g, [('Asus4', 4), ('A7sus4', 4)], 19, lambda f, d, v, seed=0: piano(f, 3.0, v, seed=seed, **pk),
        (0, 1, 2, 3, 4, 5, 4, 3), step=0.5, low='A2', vel=0.5, dyn=[(0, 0.5), (8, 0.7)], gain=1.0, send=0.4,
        part='piano', seed=78)

    # ---------------- bars 21-32: the full theme
    chT = ['Dm', 'Bbadd9', 'F/A', 'C', 'F', 'Am7', 'Bb', 'A', 'Dm', 'Am7', 'Bbmaj7', 'C']
    prT = [(c, 4) for c in chT]
    prT[7:8] = [('A', 2), ('A7sus4', 2)]
    pick1 = 'A4:1.5 D5:.5 | '
    m1 = 'E5:1.5 F5:.5 A5:2 | C6:3 Bb5:.5 A5:.5 | G5:1.5 A5:.5 F5:2 | E5:2 C5:1.5 F5:.5 | '
    m2 = 'G5:1.5 A5:.5 C6:2 | E6:3 D6:.5 C6:.5 | D6:1.5 C6:.5 Bb5:1 D6:1 | E6:2 A5:1.5 D6:.5 | '
    m3 = 'E6:1.5 F6:.5 A6:2 | C7:2 A5:1.5 D6:.5 | E6:1.5 F6:.5 A6:2 | C7:4'
    mel = pick1 + m1 + m2 + m3            # starts on beat 2 of bar 20
    dT = [(0, 0.85), (2, 0.9), (18, 0.84), (20, 0.74), (30, 0.86), (34, 0.9), (36, 0.95), (46, 1.0), (50, 1.0)]
    mk = dict(voices=9, attack=0.07, release=0.3, vib=0.006, vib_delay=0.15)
    play(s, g, strings, mel, 20, beat=2.0, vel=0.85, dyn=dT, gain=1.5, send=0.45, part='melody', legato=1.0,
         seed=101, **VN, **mk)
    play(s, g, strings, mel, 20, beat=2.0, vel=0.85, dyn=dT, gain=1.2, send=0.45, part='melody', legato=1.0,
         transpose=-12, center=-0.1, seed=102, **mk)
    # violas and horns join the tune two octaves down for the last phrase
    play(s, g, strings, 'A4:1.5 D5:.5 | ' + m3, 28, beat=2.0, vel=0.95, gain=0.7, send=0.45, part='melody',
         legato=1.0, transpose=-24, center=0.15, seed=103, **mk)
    play(s, g, brass, 'A4:1.5 D5:.5 | ' + m3, 28, beat=2.0, vel=0.92, gain=0.75, send=0.45, part='horns', legato=0.98,
         transpose=-24, voices=4, bright=0.75, attack=0.05, center=-0.25, seed=104)
    play(s, g, brass, 'A4:1.5 D5:.5 | ' + m3, 28, beat=2.0, vel=0.9, gain=0.5, send=0.45, part='trumpets',
         legato=0.98, transpose=-12, voices=3, bright=1.15, attack=0.04, center=0.0, seed=105)
    # horns: the tune in bars 21-24, then rising answers under the held notes
    play(s, g, brass, pick1 + m1, 20, beat=2.0, vel=0.78, gain=0.6, send=0.45, part='horns', legato=0.98,
         transpose=-12, voices=4, bright=0.55, attack=0.06, center=-0.25, seed=106)
    for bar, line in ((22, 'r:1 F4:1 Bb4:1 D5:1'), (26, 'r:1 E4:1 A4:1 C5:1'), (27, 'D5:2 Bb4:2'),
                      (28, 'r:1 E4:1 A4:1 C#5:1')):
        play(s, g, brass, line, bar, vel=0.8, gain=0.7, send=0.45, part='horns', legato=0.98, voices=4, bright=0.6,
             attack=0.06, center=-0.25, seed=107 + bar)
    # cellos: a tenor counter-line in contrary motion
    play(s, g, strings, 'D4:2 C4:2 | Bb3:2 D4:2 | C4:2 A3:2 | G3:2 Bb3:2 | A3:2 C4:2 | C4:2 E4:2 | F4:2 D4:2 | '
                        'E4:2 E4:2 | F4:2 D4:2 | E4:2 C4:2 | D4:2 F4:2 | G4:2 E4:2', 21, vel=0.8, dyn=dT[1:], gain=0.7,
         send=0.4, part='cellos', legato=1.0, voices=7, attack=0.1, release=0.3, center=0.35, seed=110)
    harmony(s, g, prT, 21, start=('A3', 'D4', 'F4'), lo='F3', hi='A4', vel=0.8, dyn=dT[1:], gain=0.6, send=0.45,
            part='strings', voices=8, attack=0.12, release=0.3, center=0.1, seed=111)
    harmony(s, g, prT, 21, start=('D3', 'A3', 'D4'), lo='A2', hi='F4', inst=brass, vel=0.7,
            dyn=[(0, 0.66), (16, 0.6), (32, 0.82), (48, 1.0)], gain=0.62, send=0.4, part='trombones', voices=3,
            bright=0.85, attack=0.08, center=0.25, seed=112)
    harmony(s, g, prT, 21, start=('F4', 'A4', 'D5', 'F5'), lo='C4', hi='A5', inst=choir, vel=0.75,
            dyn=[(0, 0.72), (16, 0.66), (32, 0.86), (48, 1.0)], gain=0.58, send=0.55, part='choir', vowel='ah',
            voices=7, attack=0.2, release=0.5, seed=113)
    harmony(s, g, prT, 21, start=('D3', 'A3', 'D4'), lo='A2', hi='F4', inst=choir, vel=0.7,
            dyn=[(0, 0.66), (16, 0.6), (32, 0.82), (48, 1.0)], gain=0.5, send=0.5, part='choir', vowel='oh',
            voices=6, attack=0.2, release=0.5, seed=114)
    bassline(s, g, prT, 21, ref='C2', vel=0.85, dyn=dT[1:], gain=0.8, send=0.3, part='bass', voices=6, center=0.4,
             attack=0.08, release=0.3, tie=False, seed=115)
    bassline(s, g, prT, 21, ref='C2', inst=brass, vel=0.75, dyn=[(0, 0.7), (32, 0.8), (48, 1.0)], gain=0.6, send=0.3,
             part='tuba', voices=2, bright=0.6, center=0.3, attack=0.08, octave_below=False, tie=False, seed=116)
    arp(s, g, prT, 21, lambda f, d, v, seed=0: harp(f, 2.6, v, seed=seed), (0, 1, 2, 3, 4, 5, 6, 5), step=0.5,
        low='D2', vel=0.75, gain=1.1, send=0.4, part='harp', pan=-0.35, seed=117)
    arp(s, g, prT, 21, lambda f, d, v, seed=0: piano(f, 3.0, v, seed=seed, pedal=True, maxlen=6.0),
        (0, 2, 3, 4, 5, 4, 3, 2), step=0.5, low='D2', vel=0.6, dyn=[(0, 0.6), (48, 0.8)], gain=0.9, send=0.35,
        part='piano', seed=118)
    for i, (sym, _) in enumerate([(c, 4) for c in chT]):
        bar = 21 + i
        bp_ = bass_pitch(sym, 'C2')
        v = 0.75 + 0.02 * i
        hit(s, g.t(bar), timpani(hz(bp_ + 12 if bp_ < 36 else bp_), v, 2.8, seed=i), 0.9, -0.1, 0.35, 'timpani')
        if i % 4 != 3:
            hit(s, g.t(bar, 2.0), timpani(hz(bp_ + 12 if bp_ < 36 else bp_), v * 0.55, 2.4, seed=i + 1), 0.8, -0.1,
                0.35, 'timpani')
    hit(s, g.t(21), cymbal(6.0, 0.9, seed=5), 0.8, 0.0, 0.4, 'cymbal')
    hit(s, g.t(21), taiko(50.0, 0.9, 2.0, seed=1), 0.9, 0.0, 0.35, 'taiko')
    hit(s, g.t(24, 2.0), timp_roll(hz('C3'), g.d(2) - 0.05, 0.7, swell=(0.3, 1.0), seed=5), 0.8, -0.1, 0.35, 'timpani')
    hit(s, g.t(25), cymbal(5.0, 0.6, seed=6), 0.6, 0.0, 0.4, 'cymbal')
    hit(s, g.t(28), timp_roll(hz('A2'), g.d(4) - 0.05, 0.85, swell=(0.2, 0.5, 1.0), seed=6), 0.9, -0.1, 0.35, 'timpani')
    hit(s, g.t(28), cymbal(g.d(4) + 3.5, 0.8, swell=g.d(4), seed=7), 0.6, 0.0, 0.4, 'cymbal')
    hit(s, g.t(29), cymbal(6.0, 0.95, seed=8), 0.8, 0.0, 0.4, 'cymbal')
    hit(s, g.t(29), taiko(50.0, 1.0, 2.0, seed=2), 1.0, 0.0, 0.35, 'taiko')
    hit(s, g.t(31), taiko(50.0, 0.9, 2.0, seed=3), 0.9, 0.0, 0.35, 'taiko')
    hit(s, g.t(32), timp_roll(hz('C3'), g.d(4) - 0.04, 1.0, swell=(0.35, 0.6, 1.0), seed=8), 1.0, -0.1, 0.35, 'timpani')
    hit(s, g.t(32), cymbal(g.d(4) + 4.0, 0.9, swell=g.d(4), seed=9), 0.7, 0.0, 0.4, 'cymbal')
    for i, beat in enumerate((2.0, 3.0, 3.5)):
        hit(s, g.t(32, beat), taiko(54.0, 0.8 + 0.08 * i, 1.6, seed=i), 0.9, 0.0, 0.35, 'taiko')
    for bar, p, v in ((22, 'C7', 0.5), (26, 'E7', 0.45), (30, 'C7', 0.55), (32, 'C7', 0.6)):
        hit(s, g.t(bar), fm_bell(hz(p), 6.0, v, decay=2.2, index=2.0, seed=bar), 0.3, 0.2, 0.5, 'bell', send2=0.4,
            bus2='long')

    # ---------------- bar 33 (120.0 s): D major. The C has risen to D.
    t0 = g.t(33)
    sw = (1.0, 0.95, 0.55, 0.0)
    for p, c, gn in (('D2', 0.4, 1.0), ('D1', 0.45, 0.8), ('A2', 0.4, 0.8), ('D3', 0.3, 0.8), ('A3', 0.15, 0.75),
                     ('F#4', 0.1, 0.75), ('D5', -0.1, 0.8), ('F#5', -0.15, 0.75), ('A5', -0.2, 0.8),
                     ('D6', -0.3, 1.0), ('A6', -0.3, 0.7), ('D7', -0.35, 0.8)):
        s.add(strings(hz(p), g.d(8), 1.0, voices=8, attack=0.05, release=0.5, center=c, swell=sw, marc=0.3,
                      seed=int(midi(p))), t0, gn * 0.9, 0.0, 0.45, 'final-chord')
    for p, c, kw in (('D2', 0.3, dict(voices=2, bright=0.7)), ('D3', 0.3, dict(voices=3, bright=1.0)),
                     ('A3', 0.25, dict(voices=3, bright=1.0)), ('D4', -0.25, dict(voices=4, bright=0.8)),
                     ('F#4', -0.25, dict(voices=4, bright=0.8)), ('A4', -0.2, dict(voices=4, bright=0.8)),
                     ('D5', 0.0, dict(voices=3, bright=1.2)), ('F#5', 0.0, dict(voices=3, bright=1.2)),
                     ('A5', 0.0, dict(voices=3, bright=1.2))):
        s.add(brass(hz(p), g.d(5.5), 1.0, attack=0.04, release=0.6, center=c, swell=(1.0, 0.9, 0.6, 0.0),
                    seed=int(midi(p)), **kw), t0, 0.7, 0.0, 0.45, 'final-chord')
    for p, vw in (('D3', 'oh'), ('A3', 'oh'), ('D4', 'ah'), ('F#4', 'ah'), ('A4', 'ah'), ('D5', 'ah'), ('F#5', 'ah'),
                  ('A5', 'ah')):
        s.add(choir(hz(p), g.d(7), 1.0, voices=7, attack=0.08, release=0.8, vowel=vw, swell=(1.0, 0.9, 0.5, 0.0),
                    seed=int(midi(p))), t0, 0.72, 0.0, 0.55, 'final-chord')
    hit(s, t0, timpani(hz('D2'), 1.0, 3.5, seed=9), 1.1, -0.1, 0.35, 'timpani')
    hit(s, t0 + 0.02, timp_roll(hz('D2'), 3.2, 0.8, swell=(1.0, 0.6, 0.15), seed=10), 0.8, -0.1, 0.35, 'timpani')
    hit(s, t0, taiko(46.0, 1.0, 2.6, seed=4), 1.1, 0.0, 0.35, 'taiko')
    hit(s, t0, cymbal(8.0, 1.0, seed=10), 0.9, 0.0, 0.45, 'cymbal')
    hit(s, t0, tamtam(55.0, 10.0, 0.9, seed=4), 0.8, 0.0, 0.45, 'tamtam')
    for i, p in enumerate(('D4', 'F#4', 'A4', 'D5', 'F#5', 'A5', 'D6', 'F#6', 'A6', 'D7')):
        hit(s, t0 + 0.06 * i, harp(hz(p), 3.0, 0.8, seed=i), 0.9, -0.35, 0.45, 'harp')
    for p, v in (('D7', 0.6), ('A6', 0.7)):
        hit(s, t0, fm_bell(hz(p), 9.0, v, decay=3.5, index=2.2, seed=3), 0.45, 0.1, 0.5, 'bell', send2=0.5, bus2='long')
    # ---------------- the one tone that is left: A, from 120 s, alone by 127 s, gone by 149 s
    n = int(29.5 * SR)
    t = tt(n)
    env = np.minimum(1.0, t / 2.5) * np.where(t < 7.0, 1.0, np.exp(-(t - 7.0) / 6.5)) * np.clip((29.0 - t) / 6.0, 0, 1)
    ring = np.zeros((n, 2))
    for f, a, ph in ((880.0, 1.0, 0.0), (880.9, 0.45, 1.0), (1760.0, 0.6, 2.0), (1761.3, 0.3, 0.5), (2640.6, 0.12, 0.3),
                     (440.0, 0.25, 1.7)):
        for ch in range(2):
            lfo = 1.0 + 0.12 * np.sin(TAU * (0.11 + 0.05 * ch) * t + ph + ch)
            ring[:, ch] += a * lfo * np.sin(TAU * f * (1.0 + 0.0004 * ch) * t + ph * (1 + ch))
    s.add(ring * (env * 0.05)[:, None], t0, 1.0, 0.0, 0.2, 'ring', send2=0.6, bus2='long')
    print(s.report())
    y = s.mix(loop=False)
    nf = int(1.2 * SR)
    y[-nf:] *= fade_out(nf)[:, None]
    return result(y, False, target=-16.0, comp_thresh=4.0, comp_ratio=1.5)


# ---------------------------------------------------------------- remains

def remains():
    """110 s, no loop. D minor, 48 BPM (a bar is 5 s), solo piano with a
    violin, a viola and a cello.

      1.2   one A, alone (the tone that was left)
      5     bars 2-5: four slow chords under that A (Dm, Bb, Gm, Asus4)
     22.5   the motif, first time (A D | E F A): it stops on the fifth
            note, over a Bb chord; the C does not come (30 s). The strings
            enter there.
     35     the second half of the sentence, alone (G A F | E)
     45     bars 10-13: the farewell strain from `last_light`, on the three
            strings, the piano only marking the harmony
     62.5   the motif again, an octave higher, with the strings swelling
            under it; again it stops on A (70 s) and they hold the chord
     75     the answer, low; the strings go out by 85 s
     85     bars 18-20: once more, slower, one note at a time, no harmony:
            A (85.0)  D (87.5)  E (90.0)  F (92.8)  A (95.6)
     98.75  where the C would be: nothing
    102.5   one glass A, very far; silence from about 108 s
    """
    g = Grid(48)
    rev = {'hall': make_ir(3.6, 0.03, 0.5, 31), 'long': make_ir(7.0, 0.05, 0.42, 17)}
    s = Song(110.0, tail=1.0, reverbs=rev)
    rng = np.random.default_rng(12)

    def pn(bar, beat, p, vel, hold, gain=1.0):
        """A piano note held (pedal) for `hold` beats."""
        d = g.d(hold)
        y = piano(hz(p), d, vel * rng.uniform(0.94, 1.06), pedal=False, felt=0.55, maxlen=d + 1.5,
                  seed=int(bar * 100 + beat * 10 + midi(p)))
        s.add(y, g.t(bar, beat) + rng.uniform(-0.012, 0.012), gain * 3.2, 0.0, 0.42, 'piano', send2=0.15, bus2='long')

    def chord(bar, beat, ps, vel, hold, roll=0.045):
        for k, p in enumerate(ps):
            pn(bar, beat + k * roll / g.beat, p, vel * (1.0 - 0.05 * k), hold - k * roll / g.beat)

    # the memory of the ringing tone
    s.add(glass_pad(hz('A5'), 6.0, 0.3, attack=0.5, release=6.0, seed=1, shimmer=0.6, bright=0.5), 0.0, 0.8, 0.0, 0.3,
          'light', send2=0.6, bus2='long')
    pn(1, 1.0, 'A4', 0.3, 3.0)
    # bars 2-5: chords under a held A
    for bar, lh, rh in ((2, ('D2', 'A2'), ('E4', 'F4')), (3, ('Bb1', 'F2'), ('D4', 'F4')),
                        (4, ('G1', 'D2'), ('Bb3', 'D4')), (5, ('A1', 'E2'), ('D4', 'E4'))):
        chord(bar, 0.0, lh, 0.26, 4.0)
        chord(bar, 1.0, rh, 0.26, 3.0, roll=0.09)
        if bar < 5:
            pn(bar, 2.0, 'A4', 0.33, 2.0)
    # the motif, first time: A D | E F A | (no C)
    pn(5, 2.0, 'A3', 0.36, 2.0)
    pn(5, 3.5, 'D4', 0.36, 0.5)
    chord(6, 0.0, ('D2', 'A2'), 0.3, 4.0)
    pn(6, 0.0, 'E4', 0.4, 1.5)
    pn(6, 1.5, 'F4', 0.38, 2.5)
    pn(6, 2.0, 'A4', 0.44, 2.0)
    chord(7, 0.0, ('Bb1', 'F2'), 0.27, 4.0)
    pn(7, 0.0, 'A4', 0.2, 4.0)          # the A is still there; nothing above it
    pn(7, 1.5, 'D3', 0.2, 2.5)
    # the answer
    chord(8, 0.0, ('G1', 'D2'), 0.28, 4.0)
    pn(8, 0.0, 'G4', 0.38, 1.5)
    pn(8, 1.5, 'A4', 0.34, 0.5)
    pn(8, 2.0, 'F4', 0.36, 2.0)
    chord(9, 0.0, ('A1', 'E2'), 0.27, 4.0)
    pn(9, 0.0, 'E4', 0.34, 4.0)
    pn(9, 2.0, 'D4', 0.22, 2.0)
    # strings: enter where the C is missing, hold through the answer
    vn = dict(attack=0.5, release=0.8, vib=0.007, vib_delay=0.5, bright=0.6)

    def st(line, bar, beat, vel, dyn, pan, gain, seed, transpose=0):
        play(s, g, solo_string, line, bar, beat=beat, vel=vel, dyn=dyn, gain=gain * 0.5, pan=pan, send=0.6, part='strings',
             legato=1.03, human=0.015, seed=seed, transpose=transpose, send2=0.2, bus2='long', **vn)

    d1 = [(0, 0.3), (4, 0.4), (10, 0.34), (14, 0.22)]
    st('D4:6 D4:4 D4:4', 6, 2.0, 0.34, d1, 0.1, 0.8, 1)
    st('Bb2:4 G2:4 A2:4', 7, 0.0, 0.34, [(0, 0.3), (6, 0.36), (12, 0.2)], 0.3, 0.9, 2)
    st('A4:6 Bb4:4 A4:4', 6, 2.0, 0.3, d1, -0.3, 0.7, 3)
    # bars 10-13: the farewell strain on three strings
    d2 = [(0, 0.4), (6, 0.46), (12, 0.52), (16, 0.4)]
    fv = dict(vn, attack=0.14, release=0.4, bright=0.75, vib=0.009, vib_delay=0.3)
    play(s, g, solo_string, 'F5:1.5 E5:.5 D5:2 | C5:1.5 D5:.5 A4:2 | Bb4:1.5 C5:.5 D5:1 F5:1 | E5:4', 10, vel=0.45,
         dyn=d2, gain=0.62, pan=-0.3, send=0.6, part='strings', legato=1.03, human=0.015, seed=11, send2=0.2,
         bus2='long', **fv)
    st('D4:4 | C4:4 | Bb3:2 D4:2 | D4:2 E4:2', 10, 0.0, 0.38, d2, 0.1, 0.8, 12)
    st('A4:4 | A4:4 | G4:2 F4:2 | E4:2 D4:2', 10, 0.0, 0.34, d2, -0.1, 0.6, 13)
    st('Bb2:4 | A2:4 | G2:4 | A2:4', 10, 0.0, 0.4, d2, 0.3, 0.9, 14)
    for bar, lh, rh in ((10, 'Bb1', ('D4', 'F4')), (11, 'A1', ('C4', 'F4')), (12, 'G1', ('Bb3', 'D4')),
                        (13, 'A1', ('D4', 'E4'))):
        pn(bar, 0.0, lh, 0.26, 4.0)
        chord(bar, 2.0, rh, 0.22, 2.0, roll=0.08)
    # the motif again, an octave up, the strings under it
    pn(13, 2.0, 'A4', 0.4, 2.0)
    pn(13, 3.5, 'D5', 0.4, 0.5)
    chord(14, 0.0, ('D2', 'A2', 'F3'), 0.3, 4.0)
    pn(14, 0.0, 'E5', 0.41, 1.5)
    pn(14, 1.5, 'F5', 0.39, 2.5)
    pn(14, 2.0, 'A5', 0.45, 2.0)
    chord(15, 0.0, ('Bb1', 'F2', 'D3'), 0.3, 4.0)
    pn(15, 0.0, 'A5', 0.2, 4.0)
    d3 = [(0, 0.38), (4, 0.48), (8, 0.44), (12, 0.32), (16, 0.12)]
    st('A3:4 | Bb3:4 | Bb3:4 | A3:4', 14, 0.0, 0.4, d3, 0.1, 0.85, 21)
    st('F4:4 | F4:4 | D4:4 | D4:4', 14, 0.0, 0.4, d3, -0.1, 0.75, 22)
    st('A4:4 | A4:4 | G4:4 | E4:4', 14, 0.0, 0.4, d3, -0.3, 0.75, 23)     # A over Bb: it wants to rise, and waits
    st('D3:4 | Bb2:4 | G2:4 | A2:4', 14, 0.0, 0.44, d3, 0.3, 0.95, 24)
    # the answer, low
    chord(16, 0.0, ('G1', 'D2'), 0.28, 4.0)
    pn(16, 0.0, 'G3', 0.36, 1.5)
    pn(16, 1.5, 'A3', 0.32, 0.5)
    pn(16, 2.0, 'F3', 0.34, 2.0)
    chord(17, 0.0, ('A1', 'E2'), 0.25, 4.0)
    pn(17, 0.0, 'E3', 0.3, 4.0)
    # bars 18-20: once more, alone, slower, and no last note
    pn(18, 0.0, 'A3', 0.34, 4.0)
    pn(18, 2.0, 'D4', 0.33, 3.0)
    pn(19, 0.0, 'E4', 0.33, 3.0)
    pn(19, 2.25, 'F4', 0.3, 3.0)
    chord(20, 0.5, ('Bb1', 'D3'), 0.2, 9.0)
    pn(20, 0.5, 'A4', 0.3, 9.0)
    s.add(solo_string(hz('A5'), 14.0, 0.26, attack=4.0, release=5.0, vib=0.003, vib_delay=2.0, bright=0.5,
                      swell=(0.6, 1.0, 0.7, 0.3), seed=9), g.t(19), 0.3, -0.2, 0.7, 'strings', send2=0.3, bus2='long')
    b = fm_bell(hz('A6'), 7.0, 0.3, decay=2.6, index=1.4, strike=0.5, seed=4)
    s.add(lp(b, 5000, 1), g.t(21, 2.0), 0.22, 0.15, 0.9, 'light', send2=0.9, bus2='long')
    print(s.report())
    y = s.mix(loop=False)
    nf = int(2.5 * SR)
    y[-nf:] *= fade_out(nf)[:, None]
    return result(y, False, target=-18.5, comp=False)
