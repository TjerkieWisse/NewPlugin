# Synthesised drum one-shots for training: 808 and punchy kicks, snares, claps, rims, toms, closed and
# open hi-hats, crashes and rides. Every parameter is drawn at random, so each training mix gets new
# sounds. All of it is generated here, so the sounds belong to us outright.
#
# Each function takes a numpy Generator and returns a mono float32 one-shot at RATE, peaking at 1.
# synth_kit() picks one sound family per drum class, the way a producer picks a kit.
import numpy as np
from scipy import signal

RATE = 44100


def _t(seconds):
    return np.arange(int(seconds * RATE)) / RATE


def _env(seconds, decay, attack=0.0005):
    """Exponential decay to -60 dB after `decay` seconds, with a short linear attack."""
    t = _t(seconds)
    env = np.exp(-6.9 * t / max(decay, 1e-3))
    a = max(1, int(attack * RATE))
    env[:a] *= np.linspace(0, 1, a)
    return env


def _filter(x, kind, freq, order=2):
    nyq = RATE / 2
    if kind == 'band':
        lo, hi = max(20.0, freq[0]), min(freq[1], nyq * 0.95)
        sos = signal.butter(order, [lo / nyq, hi / nyq], 'bandpass', output='sos')
    else:
        sos = signal.butter(order, min(freq, nyq * 0.95) / nyq, kind, output='sos')
    return signal.sosfilt(sos, x)


def _sweep(f_start, f_end, sweep_time, seconds):
    """Phase of a sine whose pitch falls exponentially from f_start to f_end."""
    t = _t(seconds)
    f = f_end + (f_start - f_end) * np.exp(-t / max(sweep_time, 1e-3))
    return 2 * np.pi * np.cumsum(f) / RATE


def _metal(rng, seconds, base):
    """The 808 recipe for hats and cymbals: six detuned square waves, inharmonic to each other."""
    ratios = np.array([1.0, 1.4471, 1.6170, 1.9265, 2.5028, 2.6637]) * rng.uniform(0.97, 1.03, 6)
    t = _t(seconds)
    return sum(signal.square(2 * np.pi * base * r * t + rng.uniform(0, 6.28)) for r in ratios) / 6


def _finish(x, drive=0.0):
    if drive > 0:
        x = np.tanh(x * (1 + drive)) / np.tanh(1 + drive)
    tail = int(0.003 * RATE)                      # no click where the sound is cut off
    x[-tail:] *= np.linspace(1, 0, tail)
    peak = np.max(np.abs(x))
    return (x / peak if peak > 0 else x).astype(np.float32)


# --- kick -----------------------------------------------------------------------------------------

def kick_808(rng):
    decay = rng.uniform(0.35, 2.5)
    seconds = decay * 1.1
    f0 = rng.uniform(35, 65)
    x = np.sin(_sweep(f0 * rng.uniform(2, 6), f0, rng.uniform(0.01, 0.06), seconds)) * _env(seconds, decay)
    if rng.random() < 0.6:                        # the click of the beater
        x += 0.3 * rng.random() * _filter(rng.standard_normal(len(x)), 'high', 1500) * _env(seconds, 0.008)
    return _finish(x, drive=rng.choice([0, rng.uniform(0.5, 6)]))


def kick_punch(rng):
    decay = rng.uniform(0.12, 0.5)
    seconds = decay * 1.2
    f0 = rng.uniform(45, 90)
    x = np.sin(_sweep(f0 * rng.uniform(3, 8), f0, rng.uniform(0.005, 0.03), seconds)) * _env(seconds, decay)
    noise = _filter(rng.standard_normal(len(x)), 'band', (rng.uniform(1000, 3000), rng.uniform(4000, 9000)))
    x += rng.uniform(0.1, 0.5) * noise * _env(seconds, rng.uniform(0.004, 0.015))
    return _finish(x, drive=rng.uniform(0, 3))


# --- snare, clap, rim -----------------------------------------------------------------------------

def snare(rng):
    seconds = rng.uniform(0.25, 0.6)
    f0 = rng.uniform(150, 260)
    tone = (np.sin(_sweep(f0 * 1.5, f0, 0.01, seconds)) + 0.5 * np.sin(_sweep(f0 * 2.6, f0 * 1.8, 0.01, seconds)))
    tone *= _env(seconds, rng.uniform(0.04, 0.15))
    noise = _filter(rng.standard_normal(len(tone)), 'band', (rng.uniform(800, 2500), rng.uniform(6000, 14000)))
    noise *= _env(seconds, rng.uniform(0.1, seconds * 0.9))
    mix = rng.uniform(0.2, 0.8)
    return _finish(mix * tone + (1 - mix) * 2 * noise, drive=rng.uniform(0, 2))


def clap(rng):
    seconds = rng.uniform(0.2, 0.5)
    n = len(_t(seconds))
    noise = _filter(rng.standard_normal(n), 'band', (rng.uniform(700, 1500), rng.uniform(2500, 6000)))
    env = np.zeros(n)
    gap = rng.uniform(0.005, 0.015)
    for k in range(rng.integers(3, 6)):            # the separate hands, then the room tail
        start = int(k * gap * RATE)
        burst = _env(seconds, 0.012)[:n - start]
        env[start:] += burst
    start = int(rng.integers(3, 6) * gap * RATE)
    env[start:] += rng.uniform(0.3, 0.8) * _env(seconds, rng.uniform(0.08, 0.3))[:n - start]
    return _finish(noise * env, drive=rng.uniform(0, 1.5))


def rim(rng):
    seconds = rng.uniform(0.06, 0.15)
    f0 = rng.uniform(400, 1800)
    x = np.sin(2 * np.pi * f0 * _t(seconds)) * _env(seconds, rng.uniform(0.01, 0.04))
    x += 0.5 * _filter(rng.standard_normal(len(x)), 'high', 2000) * _env(seconds, 0.006)
    return _finish(x)


# --- toms -----------------------------------------------------------------------------------------

def tom(rng, f0=None):
    """A drum tom, not an 808: a short pitch bend, the drum's second mode (1.5x, the "boing" a real
    tom has and a sine kick lacks), and a stick attack of filtered noise."""
    decay = rng.uniform(0.25, 0.9)
    seconds = decay * 1.1
    f0 = f0 or rng.uniform(80, 280)
    bend = rng.uniform(1.05, 1.4)
    x = np.sin(_sweep(f0 * bend, f0, rng.uniform(0.02, 0.08), seconds)) * _env(seconds, decay)
    x += rng.uniform(0.3, 0.7) * np.sin(_sweep(f0 * 1.5 * bend, f0 * 1.5, 0.03, seconds)) * _env(seconds, decay * 0.5)
    attack = _filter(rng.standard_normal(len(x)), 'band', (rng.uniform(500, 1500), rng.uniform(3000, 8000)))
    x += rng.uniform(0.3, 0.8) * attack * _env(seconds, rng.uniform(0.01, 0.04))
    return _finish(x, drive=rng.uniform(0, 1.0))


# --- hi-hat ---------------------------------------------------------------------------------------

def _hat(rng, decay):
    seconds = max(0.05, decay * 1.3)
    if rng.random() < 0.6:
        src = _metal(rng, seconds, rng.uniform(250, 450))
    else:
        src = rng.standard_normal(len(_t(seconds)))
    x = _filter(src, 'band', (rng.uniform(5000, 8000), rng.uniform(11000, 18000)), order=3)
    x = _filter(x, 'high', rng.uniform(4000, 7000))
    return _finish(x * _env(seconds, decay, attack=0.0003))


def hihat_closed(rng):
    return _hat(rng, rng.uniform(0.02, 0.09))


def hihat_open(rng):
    return _hat(rng, rng.uniform(0.2, 0.7))


# --- cymbals --------------------------------------------------------------------------------------

def crash(rng):
    decay = rng.uniform(1.0, 3.0)
    seconds = decay * 1.1
    x = 0.6 * _metal(rng, seconds, rng.uniform(300, 600)) + 0.4 * rng.standard_normal(len(_t(seconds)))
    x = _filter(x, 'high', rng.uniform(2500, 5000))
    x = _filter(x, 'low', rng.uniform(12000, 18000))
    return _finish(x * _env(seconds, decay, attack=rng.uniform(0.001, 0.01)))


def ride(rng):
    decay = rng.uniform(0.8, 2.0)
    seconds = decay * 1.1
    t = _t(seconds)
    bell = sum(np.sin(2 * np.pi * f * t) * rng.uniform(0.3, 1)
               for f in rng.uniform(300, 900) * np.array([1, 2.4, 3.9, 5.4]))
    bell *= _env(seconds, rng.uniform(0.3, decay))
    wash = _filter(_metal(rng, seconds, rng.uniform(400, 700)), 'high', rng.uniform(3000, 6000)) * _env(seconds, decay)
    return _finish(rng.uniform(0.1, 0.5) * bell + wash)


# --- kits -----------------------------------------------------------------------------------------

CLASSES = ['kick', 'snare', 'toms', 'hihat', 'cymbals']

# What each class can be built from; the names are the instruments a groove asks for (see patterns.py).
FAMILIES = {
    'kick': [kick_808, kick_punch],
    'snare': [snare, clap],
    'rim': [rim],
    'toms': [tom],
    'hihat_closed': [hihat_closed],
    'hihat_open': [hihat_open],
    'crash': [crash],
    'ride': [ride],
}


def synth_kit(rng):
    """A kit: one sound per instrument, as {instrument: [one-shot, ...]}. Toms get three, tuned from
    high to low."""
    kit = {}
    for name, makers in FAMILIES.items():
        make = makers[rng.integers(len(makers))]
        if name == 'toms':
            kit[name] = [make(rng, f0) for f0 in sorted(rng.uniform(70, 300, 3), reverse=True)]
        else:
            kit[name] = [make(rng)]
    return kit
