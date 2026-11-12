#!/usr/bin/env python3
"""Generate the Aetherbound soundtrack, ambiences and sound effects.

    python tools/aetherbound/audio/generate.py            # everything
    python tools/aetherbound/audio/generate.py last_light sfx:hit
    python tools/aetherbound/audio/generate.py --list
    python tools/aetherbound/audio/generate.py --table    # only rewrite audio.json
    python tools/aetherbound/audio/generate.py -j 4       # in parallel

Ids are those of the spec. `last_light` is both a track and an effect, so an
effect can be named `sfx:<id>`; a bare id that is both means both. Output:
samples/aetherbound/audio/*.ogg (effects as sfx_<id>.ogg) and
samples/aetherbound/data/audio.json, a flat table {id: {file, loop,
loop_start, loop_end, volume}} in which that effect is `last_light_sfx`.
Deterministic: the same ids give the same files.
"""
import json
import os
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
ROOT = os.path.normpath(os.path.join(HERE, '..', '..', '..', 'samples', 'aetherbound'))
OUT = os.path.join(ROOT, 'audio')

# id -> (group, volume)   volume is the suggested playback gain for the game
MUSIC = {'title': 0.9, 'fortress': 0.85, 'battle': 0.85, 'victory': 0.9, 'colossus': 0.9, 'overload': 0.9,
         'last_light': 1.0, 'remains': 1.0}
AMBIENCE = {'war': 0.6, 'wind': 0.55, 'hum': 0.5}
# loop points in seconds (start, end); everything else plays once
LOOPS = {'title': (0.0, 40.0), 'fortress': (0.0, 70.0), 'battle': (6.0, 60.0), 'colossus': (7.5, 90.0),
         'overload': (0.0, 60.0), 'war': (0.0, 45.0), 'wind': (0.0, 30.0), 'hum': (0.0, 20.0)}
SFX_VOLUME = {'text': 0.5, 'text_system': 0.5, 'step': 0.45, 'cursor': 0.6, 'confirm': 0.7, 'cancel': 0.7,
              'buzzer': 0.7, 'atb_ready': 0.7, 'star': 0.8}


def registry():
    import music
    import ambience
    import sfx
    reg = {}
    for k in MUSIC:
        if hasattr(music, k):
            reg['music:' + k] = getattr(music, k)
    for k in AMBIENCE:
        if hasattr(ambience, k):
            reg['ambience:' + k] = getattr(ambience, k)
    for k in sfx.EFFECTS:
        reg['sfx:' + k] = sfx.EFFECTS[k]
    return reg


def filename(key):
    group, name = key.split(':')
    return ('sfx_' + name if group == 'sfx' else name) + '.ogg'


def render(key):
    import numpy as np
    from synth import SR, write_ogg, to_db, lufs
    t0 = time.time()
    group, name = key.split(':')
    res = registry()[key]()
    if not isinstance(res, dict):
        res = {'audio': res, 'loop': False}
    y = res['audio']
    if group != 'sfx' and (name in LOOPS or res.get('loop')):
        want = LOOPS.get(name)
        got = (round(res.get('loop_start', 0.0), 4), round(res.get('loop_end', len(y) / SR), 4))
        assert res.get('loop') and want == got, 'loop points of %s changed: %s, table says %s' % (name, got, want)
    path = os.path.join(OUT, filename(key))
    quality = {'music': 0.65, 'ambience': 0.7, 'sfx': 0.6}[group]      # Vorbis quality 0..1
    for attempt in range(4):
        dec = write_ogg(path, y, quality)
        assert len(dec) == len(y), 'length changed in encoding: %d -> %d' % (len(y), len(dec))
        pk = to_db(np.abs(dec).max())
        if pk <= -1.0:
            break
        y = y * 10 ** ((-1.25 - pk) / 20.0)      # the codec overshot the ceiling: back off and re-encode
    info = {'key': key, 'file': 'audio/' + filename(key), 'seconds': len(y) / SR, 'loop': bool(res.get('loop')),
            'loop_start': float(res.get('loop_start', 0.0)), 'loop_end': float(res.get('loop_end', len(y) / SR)),
            'peak_db': float(pk), 'lufs': float(lufs(dec)), 'bytes': os.path.getsize(path),
            'render_s': time.time() - t0}
    print('%-22s %7.2f s  peak %6.2f dBFS  %6.1f LUFS  %7.1f kB  (%.0f s)' % (
        key, info['seconds'], info['peak_db'], info['lufs'], info['bytes'] / 1024, info['render_s']), flush=True)
    return info


def write_table():
    """data/audio.json from what is on disk (so a partial run keeps it whole)."""
    import soundfile as sf
    table = {}
    counts = {'music': 0, 'ambience': 0, 'sfx': 0}
    import sfx
    names = [('music', k, v) for k, v in MUSIC.items()] + [('ambience', k, v) for k, v in AMBIENCE.items()] + [
        ('sfx', k, SFX_VOLUME.get(k, 0.8)) for k in sfx.EFFECTS]
    for group, name, vol in names:
        key = group + ':' + name
        path = os.path.join(OUT, filename(key))
        if not os.path.exists(path):
            continue
        dur = sf.info(path).frames / 44100.0
        loop = LOOPS.get(name) if group != 'sfx' else None
        table[table_id(group, name)] = {'file': 'audio/' + filename(key), 'loop': loop is not None,
                                        'loop_start': loop[0] if loop else 0.0,
                                        'loop_end': loop[1] if loop else round(dur, 4), 'volume': vol}
        counts[group] += 1
    os.makedirs(os.path.join(ROOT, 'data'), exist_ok=True)
    with open(os.path.join(ROOT, 'data', 'audio.json'), 'w') as f:
        json.dump(table, f, indent=2)
        f.write('\n')
    return counts


def table_id(group, name):
    """Id in data/audio.json. The table is flat, and the spec uses `last_light`
    for both a track and an effect: the track keeps the name, the effect is
    `last_light_sfx`."""
    return name + '_sfx' if group == 'sfx' and name in MUSIC else name


def main(argv):
    jobs = 1
    args = []
    it = iter(argv)
    for a in it:
        if a == '-j':
            jobs = int(next(it))
        elif a == '--list':
            print('\n'.join(registry().keys()))
            return
        elif a == '--table':            # only rewrite data/audio.json from the files on disk
            print(write_table())
            return
        else:
            args.append(a)
    reg = registry()
    keys = []
    for a in args:
        if a in ('music', 'ambience', 'sfx'):
            keys += [k for k in reg if k.startswith(a + ':')]
        elif ':' in a:
            keys.append(a)
        else:
            found = [k for k in reg if k.split(':')[1] == a]
            if not found:
                sys.exit('unknown id: ' + a)
            keys += found
    if not args:
        keys = list(reg)
    os.makedirs(OUT, exist_ok=True)
    if jobs > 1 and len(keys) > 1:
        import multiprocessing as mp
        with mp.Pool(jobs) as pool:
            infos = pool.map(render, keys, chunksize=1)
    else:
        infos = [render(k) for k in keys]
    counts = write_table()
    total = sum(os.path.getsize(os.path.join(OUT, f)) for f in os.listdir(OUT))
    print('audio.json: %d music, %d ambience, %d sfx; audio folder %.2f MB' % (
        counts['music'], counts['ambience'], counts['sfx'], total / 1e6))


if __name__ == '__main__':
    main(sys.argv[1:])
