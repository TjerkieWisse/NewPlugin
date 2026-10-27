# The grooves the training mixes are played from: real drummers from the Groove MIDI Dataset (Google
# Magenta, CC BY 4.0), and patterns programmed here the way producers write them (trap, boom bap,
# house, pop/rock, R&B), with their rolls, swing and open hats.
#
# A groove is a list of hits (time in seconds, instrument, velocity 0..1). Instruments:
#   kick, snare, rim, tom0..tom2 (high to low), hihat_closed, hihat_open, crash, ride
# and INSTRUMENT_CLASS says which of KITNAP's five parts each one belongs to.
import os, struct, numpy as np

INSTRUMENT_CLASS = {
    'kick': 'kick', 'snare': 'snare', 'rim': 'snare',
    'tom0': 'toms', 'tom1': 'toms', 'tom2': 'toms',
    'hihat_closed': 'hihat', 'hihat_open': 'hihat',
    'crash': 'cymbals', 'ride': 'cymbals',
}

# Groove MIDI's Roland TD-11 notes (see the dataset's documentation).
GMD_NOTES = {
    36: 'kick', 38: 'snare', 40: 'snare', 37: 'rim',
    48: 'tom0', 50: 'tom0', 45: 'tom1', 47: 'tom1', 43: 'tom2', 58: 'tom2',
    42: 'hihat_closed', 22: 'hihat_closed', 44: 'hihat_closed', 46: 'hihat_open', 26: 'hihat_open',
    49: 'crash', 55: 'crash', 57: 'crash', 52: 'crash', 51: 'ride', 59: 'ride', 53: 'ride',
}


# --- reading MIDI ---------------------------------------------------------------------------------

def _varlen(data, i):
    value = 0
    while True:
        byte = data[i]; i += 1
        value = (value << 7) | (byte & 0x7F)
        if not byte & 0x80:
            return value, i


def read_midi_hits(path):
    """Note-ons of a standard MIDI file as (seconds, note, velocity 1..127), following its tempo map."""
    data = open(path, 'rb').read()
    if data[:4] != b'MThd':
        raise ValueError('not a MIDI file')
    _, ntracks, division = struct.unpack('>HHH', data[8:14])
    pos, events, tempos = 14, [], [(0, 500000)]
    for _ in range(ntracks):
        assert data[pos:pos + 4] == b'MTrk'
        length = struct.unpack('>I', data[pos + 4:pos + 8])[0]
        i, end, tick, status = pos + 8, pos + 8 + length, 0, 0
        while i < end:
            delta, i = _varlen(data, i); tick += delta
            if data[i] & 0x80:
                status = data[i]; i += 1
            if status == 0xFF:
                kind = data[i]; size, i = _varlen(data, i + 1)
                if kind == 0x51:
                    tempos.append((tick, int.from_bytes(data[i:i + 3], 'big')))
                i += size
            elif status in (0xF0, 0xF7):
                size, i = _varlen(data, i); i += size
            else:
                kind = status & 0xF0
                n = 1 if kind in (0xC0, 0xD0) else 2
                if kind == 0x90 and data[i + 1] > 0:
                    events.append((tick, data[i], data[i + 1]))
                i += n
        pos = end
    tempos.sort()
    hits = []
    for tick, note, vel in sorted(events):
        seconds, last_tick, last_tempo = 0.0, 0, tempos[0][1]
        for t_tick, t_tempo in tempos:
            if t_tick >= tick:
                break
            seconds += (t_tick - last_tick) * last_tempo / division / 1e6
            last_tick, last_tempo = t_tick, t_tempo
        hits.append((seconds + (tick - last_tick) * last_tempo / division / 1e6, note, vel))
    return hits


def load_groove_midi(folder):
    """Every Groove MIDI file under `folder` as a list of hits; files without drums are skipped."""
    grooves = []
    for root, _, files in os.walk(folder):
        for f in sorted(files):
            if f.endswith('.mid'):
                hits = [(t, GMD_NOTES[n], v / 127) for t, n, v in read_midi_hits(os.path.join(root, f)) if n in GMD_NOTES]
                if len(hits) > 8:
                    grooves.append(hits)
    return grooves


def groove_excerpt(rng, grooves, seconds):
    """A random stretch of a real drummer's performance, played up to 25% faster or slower. Mostly
    from the longer grooves; one time in six from a short fill, repeated to fill the time."""
    speed = rng.uniform(0.8, 1.25)
    long = [g for g in grooves if g[-1][0] / speed > seconds]
    pool = grooves if not long or rng.random() < 1 / 6 else long
    hits = pool[rng.integers(len(pool))]
    length = hits[-1][0] / speed + 60 / 120 / 4              # end on a sixteenth after the last hit
    repeats = int(np.ceil(seconds / length)) + 1
    hits = [(t / speed + k * length, n, v) for k in range(repeats) for t, n, v in hits]
    start = rng.uniform(0, max(0.0, repeats * length - seconds)) if len(pool) and pool is long else 0.0
    return [(t - start, n, v) for t, n, v in hits if 0 <= t - start < seconds]


# --- programmed patterns --------------------------------------------------------------------------

def _grid(rng, bpm, bars, steps_per_beat, pattern, name, vel=(0.7, 1.0), swing=0.0, prob=1.0):
    """Hits on a step grid: `pattern` lists step indexes within a bar of 4 beats."""
    step = 60 / bpm / steps_per_beat
    hits = []
    for bar in range(bars):
        for s in pattern:
            if rng.random() > prob:
                continue
            t = (bar * 4 * steps_per_beat + s) * step
            if swing and s % 2 == 1:
                t += swing * step
            hits.append((t + rng.normal(0, 0.003), name, float(np.clip(rng.uniform(*vel), 0.05, 1))))
    return hits


def _trap(rng, bars):
    bpm = rng.uniform(130, 160)
    hits = _grid(rng, bpm, bars, 4, sorted(set(rng.choice(16, rng.integers(3, 7), replace=False)) | {0}), 'kick')
    hits += _grid(rng, bpm, bars, 4, [8], 'snare')        # half-time: the snare/clap on beat 3
    step = 60 / bpm / 4
    for bar in range(bars):                               # hats: eighths or sixteenths with rolls
        s = 0
        while s < 16:
            t0 = (bar * 16 + s) * step
            if rng.random() < 0.15:                       # a roll: 3, 4 or 6 hits in the step
                n = rng.choice([3, 4, 6])
                for k in range(n):
                    hits.append((t0 + k * step / n * rng.choice([1, 2]), 'hihat_closed', rng.uniform(0.4, 0.9)))
                s += 2
            else:
                hits.append((t0, 'hihat_closed', rng.uniform(0.5, 1.0)))
                s += rng.choice([1, 2])
        if rng.random() < 0.3:
            hits.append(((bar * 16 + rng.choice([6, 14])) * step, 'hihat_open', rng.uniform(0.5, 0.9)))
    return hits


def _boom_bap(rng, bars):
    bpm = rng.uniform(80, 98)
    swing = rng.uniform(0, 0.35)
    hits = _grid(rng, bpm, bars, 4, [0, 7, 10] if rng.random() < 0.5 else [0, 3, 10], 'kick', swing=swing)
    hits += _grid(rng, bpm, bars, 4, [4, 12], 'snare', swing=swing)
    hits += _grid(rng, bpm, bars, 4, list(range(0, 16, 2)) if rng.random() < 0.5 else list(range(16)),
                  'hihat_closed', vel=(0.4, 0.9), swing=swing, prob=0.95)
    return hits


def _house(rng, bars):
    bpm = rng.uniform(118, 128)
    hits = _grid(rng, bpm, bars, 4, [0, 4, 8, 12], 'kick')
    hits += _grid(rng, bpm, bars, 4, [4, 12], 'snare')
    hits += _grid(rng, bpm, bars, 4, [2, 6, 10, 14], 'hihat_open', vel=(0.5, 0.8))
    if rng.random() < 0.6:
        hits += _grid(rng, bpm, bars, 4, list(range(16)), 'hihat_closed', vel=(0.3, 0.7), prob=0.8)
    if rng.random() < 0.3:
        hits += _grid(rng, bpm, bars, 4, [0, 2, 4, 6, 8, 10, 12, 14], 'ride', vel=(0.4, 0.7))
    return hits


def _rock(rng, bars):
    bpm = rng.uniform(90, 150)
    top = 'ride' if rng.random() < 0.25 else 'hihat_closed'
    hits = _grid(rng, bpm, bars, 2, [0, 4, 5] if rng.random() < 0.5 else [0, 3, 4], 'kick')
    hits += _grid(rng, bpm, bars, 2, [2, 6], 'snare')
    hits += _grid(rng, bpm, bars, 2, list(range(8)), top, vel=(0.5, 0.9))
    beat = 60 / bpm
    for bar in range(bars):
        if rng.random() < 0.4:                            # crash on the one
            hits.append((bar * 4 * beat, 'crash', rng.uniform(0.7, 1.0)))
        if rng.random() < 0.3:                            # a tom fill at the end of the bar
            for k in range(8):
                hits.append(((bar * 4 + 3 + k / 8) * beat, ['tom0', 'tom1', 'tom2'][min(2, k // 3)], rng.uniform(0.6, 1)))
    return hits


def _rnb(rng, bars):
    bpm = rng.uniform(65, 95)
    hits = _grid(rng, bpm, bars, 4, sorted(set(rng.choice(16, rng.integers(2, 5), replace=False)) | {0}), 'kick')
    hits += _grid(rng, bpm, bars, 4, [4, 12], 'snare' if rng.random() < 0.7 else 'rim')
    hits += _grid(rng, bpm, bars, 4, list(range(0, 16, 2)), 'hihat_closed', vel=(0.3, 0.8), prob=0.9,
                  swing=rng.uniform(0, 0.25))
    return hits


STYLES = [_trap, _boom_bap, _house, _rock, _rnb]


def _extras(rng, hits):
    """Crashes at the start of some bars and tom fills at the end of some, in any style, so toms and
    cymbals turn up often enough to learn."""
    end = max(t for t, _, _ in hits)
    beat = 60 / rng.uniform(70, 160)
    bar = 4 * beat
    extra = []
    if rng.random() < 0.5:
        for b in np.arange(0, end, bar):
            if rng.random() < 0.5:
                extra.append((b, 'crash', rng.uniform(0.6, 1.0)))
    if rng.random() < 0.45:
        for b in np.arange(0, end, bar):
            if rng.random() < 0.4:
                n = rng.choice([4, 6, 8])
                toms = sorted(rng.choice(3, n), reverse=False)
                extra += [(b + 3 * beat + k * beat / n, f'tom{toms[k]}', rng.uniform(0.5, 1)) for k in range(n)]
    if rng.random() < 0.15:
        extra += [(t, 'ride', rng.uniform(0.4, 0.8)) for t in np.arange(0, end, beat / 2)]
    return hits + extra


def programmed(rng, seconds):
    """A stretch of `seconds` from a programmed pattern in a random style, starting anywhere in it."""
    style = STYLES[rng.integers(len(STYLES))]
    bars = int(np.ceil(seconds * 160 / 240)) + 2              # enough at the fastest tempo (160 BPM)
    hits = sorted(_extras(rng, style(rng, bars)))
    start = rng.uniform(0, max(0.0, hits[-1][0] - seconds))
    return [(t - start, n, v) for t, n, v in hits if 0 <= t - start < seconds]
