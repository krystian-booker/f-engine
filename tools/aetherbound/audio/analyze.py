#!/usr/bin/env python3
"""Check the generated audio by measurement (nobody can listen in CI).

    python tools/aetherbound/audio/analyze.py [--png DIR] [id ...]

For every id of data/audio.json (or the named ids, or `sfx` for all the
effects): duration,
peak, integrated loudness, RMS, DC offset, clipped samples, silent gaps,
spectral centroid, level per octave band, and for loops the step across the
loop boundary compared with the largest sample-to-sample step inside the
file, the level of the last and first 50 ms, and `seam_click`: the
high-passed energy in the millisecond at the seam over the 95th percentile of
the second around it (about 1 or less: no click). With --png it writes a spectrogram (log frequency, with
the short-term loudness drawn under it) per file.
"""
import json
import os
import sys

import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
from synth import SR, lufs, short_lufs, to_db  # noqa: E402

ROOT = os.path.normpath(os.path.join(HERE, '..', '..', '..', 'samples', 'aetherbound'))


def centroid(x, n=8192):
    m = x.mean(axis=1) if x.ndim == 2 else x
    hop = max(n, len(m) // 60)
    out = []
    f = np.fft.rfftfreq(n, 1.0 / SR)
    w = np.hanning(n)
    for i in range(0, max(1, len(m) - n), hop):
        seg = m[i:i + n]
        if len(seg) < n:
            break
        X = np.abs(np.fft.rfft(seg * w)) ** 2
        out.append((i / SR, float((X * f).sum() / (X.sum() + 1e-20)), float(X.sum())))
    return out


def octave_bands(x):
    """Long-term level per octave band (dB relative to the loudest band)."""
    m = x.mean(axis=1) if x.ndim == 2 else x
    n = 16384
    if len(m) < n:
        m = np.concatenate([m, np.zeros(n - len(m))])
    acc = np.zeros(n // 2 + 1)
    w = np.hanning(n)
    for i in range(0, len(m) - n + 1, n // 2):
        acc += np.abs(np.fft.rfft(m[i:i + n] * w)) ** 2
    f = np.fft.rfftfreq(n, 1.0 / SR)
    out = []
    for c in (31.5, 63, 125, 250, 500, 1000, 2000, 4000, 8000, 16000):
        sel = (f >= c / np.sqrt(2)) & (f < c * np.sqrt(2))
        out.append(10 * np.log10(acc[sel].sum() + 1e-20))
    out = np.array(out)
    return [int(round(v)) for v in out - out.max()]


def spectrogram_png(x, path, title='', marks=(), width=1400, height=420):
    from PIL import Image, ImageDraw
    m = x.mean(axis=1) if x.ndim == 2 else x
    n = 4096
    hop = max(256, int(len(m) / width))
    if len(m) < n + hop:
        m = np.concatenate([m, np.zeros(n + hop - len(m))])
    idx = np.arange(0, len(m) - n, hop)
    w = np.hanning(n)
    fr = np.fft.rfftfreq(n, 1.0 / SR)
    fbins = np.geomspace(30.0, 18000.0, height)
    cols = []
    for i in idx:
        X = np.abs(np.fft.rfft(m[i:i + n] * w)) / (n / 4)
        cols.append(np.interp(fbins, fr, X))
    S = 20 * np.log10(np.array(cols).T + 1e-9)           # (height, time)
    S = np.clip((S - np.percentile(S, 99.9) + 66.0) / 66.0, 0, 1)[::-1] ** 1.8
    # resample the time axis to the image width
    xi = np.linspace(0, S.shape[1] - 1, width).astype(int)
    S = S[:, xi]
    r = np.clip(S * 3.0, 0, 1)
    g = np.clip(S * 3.0 - 1.0, 0, 1)
    b = np.clip(np.where(S < 0.33, S * 1.5, S * 3.0 - 2.0), 0, 1)
    img = np.stack([r, g, b], axis=2)
    strip = 90
    canvas = np.zeros((height + strip + 16, width, 3))
    canvas[:height] = img
    im = Image.fromarray((canvas * 255).astype(np.uint8))
    d = ImageDraw.Draw(im)
    dur = len(x) / SR
    tl, ll = short_lufs(x, win=min(1.0, dur / 4), hop=max(0.05, dur / 400))
    pts = [(int(t / dur * width), int(height + strip - np.clip((l + 60.0) / 55.0, 0, 1) * strip)) for t, l in zip(tl, ll)]
    for lv in (-50, -40, -30, -20, -10):
        yy = int(height + strip - (lv + 60.0) / 55.0 * strip)
        d.line([(0, yy), (width, yy)], fill=(50, 50, 60))
        d.text((2, yy - 10), '%d LUFS' % lv, fill=(120, 120, 140))
    if len(pts) > 1:
        d.line(pts, fill=(120, 255, 160), width=1)
    step = 1 if dur <= 12 else 5 if dur <= 60 else 10
    for sec in np.arange(0, dur + 0.01, step):
        xx = int(sec / dur * (width - 1))
        d.line([(xx, height), (xx, height + 6)], fill=(255, 255, 255))
        d.line([(xx, 0), (xx, height)], fill=(40, 40, 50))
        d.text((xx + 2, height + strip + 2), '%g' % sec, fill=(220, 220, 220))
    for hz_ in (50, 100, 200, 500, 1000, 2000, 5000, 10000):
        yy = int(height - 1 - np.log(hz_ / 30.0) / np.log(18000.0 / 30.0) * (height - 1))
        d.text((width - 44, yy - 5), '%d' % hz_, fill=(200, 200, 200))
        d.line([(width - 8, yy), (width, yy)], fill=(200, 200, 200))
    for sec, label in marks:
        xx = int(sec / dur * (width - 1))
        d.line([(xx, 0), (xx, height + strip)], fill=(90, 200, 255))
        d.text((xx + 3, 3), label, fill=(90, 200, 255))
    d.text((6, 4), title, fill=(255, 255, 255))
    im.save(path)


_NAMES = ['C', 'C#', 'D', 'Eb', 'E', 'F', 'F#', 'G', 'Ab', 'A', 'Bb', 'B']


def track(x, t0, t1, fmin, fmax, step=0.25, win=0.37):
    """Loudest pitch in fmin..fmax per `step` seconds between t0 and t1, as
    note names (a crude check that a tune is where the score says)."""
    m = x.mean(axis=1) if x.ndim == 2 else x
    n = int(win * SR)
    out = []
    t = t0
    nfft = 1 << 16
    f = np.fft.rfftfreq(nfft, 1.0 / SR)
    sel = (f >= fmin) & (f <= fmax)
    while t < t1:
        i = int(t * SR)
        seg = m[i:i + n]
        if len(seg) < n:
            break
        X = np.abs(np.fft.rfft(seg * np.hanning(n), nfft))
        k = np.argmax(X[sel])
        fr = f[sel][k]
        lev = 20 * np.log10(X[sel][k] / (n / 4) + 1e-12)
        midi = 69 + 12 * np.log2(fr / 440.0)
        name = '%s%d' % (_NAMES[int(round(midi)) % 12], int(round(midi)) // 12 - 1)
        out.append((round(t, 2), name if lev > -62 else '-', round(lev)))
        t += step
    return out


def analyze(path, loop=None, png=None, marks=()):
    import soundfile as sf
    x, sr = sf.read(path, always_2d=True)
    assert sr == SR
    n = len(x)
    dur = n / SR
    pk = to_db(np.abs(x).max())
    rms = to_db(np.sqrt(np.mean(x * x)))
    dc = float(np.abs(x.mean(axis=0)).max())
    clipped = int(np.sum(np.abs(x) >= 0.999))
    L = lufs(x)
    # silent gaps: 100 ms windows under -70 dBFS, excluding the outer edges of one-shots
    w = int(0.1 * SR)
    k = n // w
    lev = to_db(np.sqrt(np.mean(x[:k * w].reshape(k, w, -1) ** 2, axis=(1, 2)))) if k else np.array([])
    silent = lev < -70
    gaps = []
    i = 0
    while i < k:
        if silent[i]:
            j = i
            while j < k and silent[j]:
                j += 1
            gaps.append((i * 0.1, j * 0.1))
            i = j
        else:
            i += 1
    cen = centroid(x)
    cs = [c for _, c, e in cen if e > 1e-6]
    out = {'file': os.path.basename(path), 'seconds': round(dur, 3), 'channels': x.shape[1], 'peak_db': round(pk, 2),
           'lufs': round(L, 1), 'rms_db': round(rms, 1), 'dc': round(dc, 5), 'clipped': clipped,
           'centroid_hz': [int(min(cs)), int(np.median(cs)), int(max(cs))] if cs else None,
           'gaps': [(round(a, 1), round(b, 1)) for a, b in gaps], 'bands': octave_bands(x)}
    if loop and loop.get('loop'):
        a = int(round(loop.get('loop_start', 0.0) * SR))
        b = min(n, int(round(loop.get('loop_end', dur) * SR)))
        jump = float(np.abs(x[a] - x[b - 1]).max())
        inner = float(np.abs(np.diff(x[a:b], axis=0)).max())
        # typical step size near the boundary
        near = float(np.sqrt(np.mean(np.diff(np.concatenate([x[b - 2205:b], x[a:a + 2205]]), axis=0) ** 2)))
        w50 = int(0.05 * SR)
        out['loop'] = {'start': round(a / SR, 4), 'end': round(b / SR, 4), 'boundary_step': round(jump, 5),
                       'largest_step_inside': round(inner, 5), 'rms_step_near': round(near, 5),
                       'last_50ms_db': round(float(to_db(np.sqrt(np.mean(x[b - w50:b] ** 2)))), 1),
                       'first_50ms_db': round(float(to_db(np.sqrt(np.mean(x[a:a + w50] ** 2)))), 1)}
        # click test: play across the seam, high-pass, and compare the 1 ms at the seam with the rest
        from scipy import signal as _sig
        half = int(0.5 * SR)
        seam = np.concatenate([x[b - half:b], x[a:a + half]]).mean(axis=1)
        hpf = _sig.sosfilt(_sig.butter(4, 5000 / (SR / 2), 'high', output='sos'), seam)
        w1 = 44
        e = (hpf[:len(hpf) // w1 * w1].reshape(-1, w1) ** 2).mean(axis=1)
        mid = half // w1
        out['loop']['seam_click'] = round(float(e[mid - 1:mid + 2].max() / (np.percentile(e, 95) + 1e-15)), 2)
        # spectral continuity: compare the spectrum of the 50 ms before the end and after the start
        A = np.abs(np.fft.rfft(x[b - w50:b].mean(axis=1) * np.hanning(w50)))
        B = np.abs(np.fft.rfft(x[a:a + w50].mean(axis=1) * np.hanning(w50)))
        out['loop']['spectral_corr'] = round(float(np.corrcoef(np.log(A + 1e-6), np.log(B + 1e-6))[0, 1]), 3)
    if png:
        os.makedirs(png, exist_ok=True)
        spectrogram_png(x, os.path.join(png, os.path.basename(path)[:-4] + '.png'),
                        '%s  %.1f s  %.1f LUFS  peak %.1f dB' % (os.path.basename(path), dur, L, pk), marks)
    return out, x


def main(argv):
    png = None
    ids = []
    it = iter(argv)
    for a in it:
        if a == '--png':
            png = next(it)
        else:
            ids.append(a)
    table = json.load(open(os.path.join(ROOT, 'data', 'audio.json')))
    rows = []
    for name, rec in table.items():
        group = 'sfx' if os.path.basename(rec['file']).startswith('sfx_') else 'loop' if rec['loop'] else 'music'
        if ids and name not in ids and group not in ids:
            continue
        info, _ = analyze(os.path.join(ROOT, rec['file']), rec, png)
        info['id'] = name
        rows.append(info)
    for r in rows:
        print('%-22s %7.2fs %dch peak %6.2f  %6.1f LUFS  rms %6.1f  dc %.4f  clip %d  centroid %s' % (
            r['id'], r['seconds'], r['channels'], r['peak_db'], r['lufs'], r['rms_db'], r['dc'], r['clipped'],
            r['centroid_hz']))
        print('    octave bands 31..16k: %s' % r['bands'])
        if r['gaps']:
            print('    silent (<-70 dB) : %s' % r['gaps'])
        if 'loop' in r:
            print('    loop %s' % r['loop'])
    return rows


if __name__ == '__main__':
    main(sys.argv[1:])
