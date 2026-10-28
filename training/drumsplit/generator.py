# Makes the training examples: a drum mix and its five parts (kick, snare, toms, hi-hat, cymbals),
# which add up to the mix apart from any residue of other instruments. Every example is new: a groove (a real drummer from Groove MIDI or a
# programmed pattern), a kit (synthesised sounds, a recorded acoustic kit from the bank, or a blend),
# each part mixed with its own EQ, compression, saturation, panning and reverb, the whole kit through
# a bus compressor or limiter, and sometimes a little of other instruments left in the mix, the way a
# drum stem pulled out of a song has some.
import json, os, sys, functools, numpy as np, soundfile as sf
import fx, patterns, synth

RATE = 44100
CLASSES = synth.CLASSES


class Bank:
    """The recorded acoustic kits fetch_data.py builds: bank/index.json lists, per kit and instrument,
    one or more sources (a crash 1 and a crash 2, say), each with its hits from soft to hard, each hit
    as a close-microphone file and a room file (None for kits recorded as one stereo mix)."""

    def __init__(self, folder):
        self.folder = folder
        self.kits = {}
        path = os.path.join(folder, 'index.json')
        if os.path.exists(path):
            self.kits = json.load(open(path))

    @functools.lru_cache(maxsize=512)          # about 0.5 GB per worker at most
    def load(self, relpath):
        """A hit's audio, or None if its file can't be read (it's then left out, with a warning)."""
        try:
            audio, rate = sf.read(os.path.join(self.folder, relpath), dtype='float32', always_2d=True)
        except Exception as e:
            print(f'Skipping {relpath}: {type(e).__name__} {e!r}', file=sys.stderr, flush=True)
            return None
        if rate != RATE or len(audio) == 0:
            return None
        return audio.T.copy() if audio.shape[1] > 1 else audio[:, 0].copy()   # mono hits get panned


def _choose_kit(rng, bank):
    """For every instrument, how to sound it: ('synth', one-shot) or ('acoustic', samples soft..hard).
    Mostly one coherent kit, sometimes with instruments swapped in from elsewhere."""
    made = synth.synth_kit(rng)
    kit_names = [k for k in bank.kits if k != 'freesound']
    # Freesound's one-shots (many of them electronic) a third of the time, a recorded kit or our own
    # synthesised sounds the rest.
    if 'freesound' in bank.kits and rng.random() < 0.3:
        base = 'freesound'
    else:
        base = kit_names[rng.integers(len(kit_names))] if kit_names and rng.random() < 0.5 else None
    kit = {}
    for inst in patterns.INSTRUMENT_CLASS:
        name = 'toms' if inst.startswith('tom') else inst
        source = base
        if base is None and kit_names and rng.random() < 0.15:
            source = kit_names[rng.integers(len(kit_names))]
        elif base is not None and rng.random() < 0.15:
            source = None
        options = bank.kits.get(source, {}).get(inst) if source else None
        if options:
            room = fx.db(rng.uniform(-18, 3)) if rng.random() < 0.8 else 0.0
            kit[inst] = ('acoustic', (options[rng.integers(len(options))], room))
        else:
            sound = made[name][int(inst[-1]) if inst.startswith('tom') else 0]
            kit[inst] = ('synth', sound)
    return kit


def _hit_audio(rng, bank, how, velocity):
    kind, data = how
    if kind == 'synth':
        return data * velocity ** 1.5                 # mono; panned later
    layers, room = data
    close_path, room_path = layers[int(np.clip(round(velocity * (len(layers) - 1) + rng.normal(0, 1.5)), 0, len(layers) - 1))]
    audio = bank.load(close_path)                     # recorded at its own loudness
    if audio is None:
        return None
    if len(layers) == 1:                              # a single one-shot: its loudness follows the velocity
        audio = audio * velocity ** 1.5
    room_audio = bank.load(room_path) if room and room_path else None
    return audio + room * room_audio if room_audio is not None and room_audio.shape == audio.shape else audio


def _residue(rng, n):
    """A quiet stand-in for what the drum separation leaves of the other instruments: bass notes and
    a filtered chord, sometimes breathy noise."""
    t = np.arange(n) / RATE
    out = np.zeros(n)
    note_len = rng.uniform(0.2, 1.0)
    for start in np.arange(0, n / RATE, note_len):
        f = 40 * 2 ** (rng.integers(0, 24) / 12)
        seg = (t >= start) & (t < start + note_len)
        out[seg] += np.sin(2 * np.pi * f * t[seg]) + 0.3 * np.sin(4 * np.pi * f * t[seg])
    for f in 200 * 2 ** (rng.choice(24, 3, replace=False) / 12):
        out += 0.3 * np.sign(np.sin(2 * np.pi * f * t))
    if rng.random() < 0.4:
        out += rng.standard_normal(n) * 0.5
    out = fx.signal.sosfilt(fx.signal.butter(2, rng.uniform(800, 6000) / (RATE / 2), 'low', output='sos'), out)
    return fx.pan(out / (np.max(np.abs(out)) + 1e-9), rng.uniform(-0.3, 0.3))


def make_example(rng, grooves, bank, seconds):
    """One training example: mix [2, n] and parts [5, 2, n], float32."""
    n = int(round(seconds * RATE))
    if grooves and rng.random() < 0.5:
        hits = patterns.groove_excerpt(rng, grooves, seconds)
    else:
        hits = patterns.programmed(rng, seconds)
    kit = _choose_kit(rng, bank)
    positions = {inst: float(np.clip(rng.normal(0, {'kick': 0.03, 'snare': 0.05, 'rim': 0.1}.get(inst, 0.4)), -0.9, 0.9))
                 for inst in patterns.INSTRUMENT_CLASS}

    stems = np.zeros((len(CLASSES), 2, n))
    hits = sorted(hits)
    hat_times = [t for t, inst, _ in hits if inst.startswith('hihat')]
    for t, inst, velocity in hits:
        audio = _hit_audio(rng, bank, kit[inst], velocity)
        if audio is None:
            continue
        if inst == 'hihat_open':                      # the next hi-hat hit chokes an open hat
            later = [h for h in hat_times if h > t + 0.01]
            if later:
                keep = int((later[0] - t + 0.015) * RATE)
                if keep < audio.shape[-1]:
                    audio = audio[..., :keep].copy()
                    audio[..., -int(0.015 * RATE):] *= np.linspace(1, 0, int(0.015 * RATE))
        audio = fx.pan(audio, positions[inst]) if audio.ndim == 1 else audio
        start = int(max(t, 0) * RATE)
        length = min(audio.shape[-1], n - start)
        if length > 0:
            stems[CLASSES.index(patterns.INSTRUMENT_CLASS[inst]), :, start:start + length] += audio[:, :length]

    room = fx.random_reverb_ir(rng) if rng.random() < 0.6 else None
    for c in range(len(CLASSES)):
        x = stems[c]
        if not np.any(x):
            continue
        x = x / (np.max(np.abs(x)) + 1e-9) * fx.db(rng.uniform(-10, 0))
        if rng.random() < 0.7:
            x = fx.random_eq(rng, x)
        if rng.random() < 0.4:
            x = fx.random_compressor(rng, x)
        if rng.random() < 0.15:
            x = fx.saturate(rng, x)
        if rng.random() < 0.3:
            x = fx.width(x, rng.uniform(0.5, 1.5))
        if room is not None:
            x = x + fx.reverb(x, room) * fx.db(rng.uniform(-30, -8) if CLASSES[c] != 'kick' else rng.uniform(-40, -18))
        stems[c] = x

    stems = fx.bus(rng, stems)
    mix = stems.sum(0)
    if rng.random() < 0.4:
        rms = np.sqrt(np.mean(mix ** 2)) + 1e-9
        mix = mix + _residue(rng, n) * rms * fx.db(rng.uniform(-35, -15))
    # As loud as drums pulled out of a mastered song: an average level of -24 to -10 dBFS, peaks no
    # higher than 0 dBFS.
    scale = min(fx.db(rng.uniform(-24, -10)) / (np.sqrt(np.mean(mix ** 2)) + 1e-9), 1 / (np.max(np.abs(mix)) + 1e-9))
    return (mix * scale).astype(np.float32), (stems * scale).astype(np.float32)


class Examples:
    """Endless examples for training. Pass a torch worker's id as `stream` so every worker draws
    different ones; the same seed and stream always give the same examples."""

    def __init__(self, data_folder, seconds, seed=0, stream=0):
        self.grooves = patterns.load_groove_midi(os.path.join(data_folder, 'groove'))
        self.bank = Bank(os.path.join(data_folder, 'bank'))
        self.seconds = seconds
        self.rng = np.random.default_rng([seed, stream])

    def __iter__(self):
        while True:
            yield make_example(self.rng, self.grooves, self.bank, self.seconds)
