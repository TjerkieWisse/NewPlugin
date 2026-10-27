# The mixing a drum part goes through in a real song, applied at random to the training mixes: EQ,
# compression, saturation, panning, a shared reverb, and a bus compressor/limiter and filters on the
# whole kit. Signals are float arrays shaped [channels, samples] at RATE.
#
# Anything on the bus is applied to every part the same way (a linear filter, or the gain curve the
# mix itself produces), so the parts still add up to the mix exactly.
import numpy as np
from scipy import signal

RATE = 44100


def db(x):
    return 10 ** (x / 20)


# --- EQ (RBJ cookbook biquads) --------------------------------------------------------------------

def _biquad(kind, freq, gain_db, q=0.7):
    a = db(gain_db / 2)
    w = 2 * np.pi * min(freq, RATE * 0.45) / RATE
    cw, alpha = np.cos(w), np.sin(w) / (2 * q)
    if kind == 'peak':
        b = [1 + alpha * a, -2 * cw, 1 - alpha * a]
        d = [1 + alpha / a, -2 * cw, 1 - alpha / a]
    else:
        s = 2 * np.sqrt(a) * alpha
        sign = 1 if kind == 'low' else -1
        b = [a * ((a + 1) - sign * (a - 1) * cw + s), 2 * sign * a * ((a - 1) - sign * (a + 1) * cw),
             a * ((a + 1) - sign * (a - 1) * cw - s)]
        d = [(a + 1) + sign * (a - 1) * cw + s, -2 * sign * ((a - 1) + sign * (a + 1) * cw),
             (a + 1) + sign * (a - 1) * cw - s]
    return np.array(b) / d[0], np.array(d) / d[0]


def random_eq(rng, x):
    for kind, lo, hi in (('low', 40, 300), ('peak', 200, 6000), ('high', 3000, 14000)):
        if rng.random() < 0.6:
            b, a = _biquad(kind, rng.uniform(lo, hi), rng.uniform(-8, 8), q=rng.uniform(0.5, 2.0))
            x = signal.lfilter(b, a, x, axis=-1)
    return x


# --- dynamics -------------------------------------------------------------------------------------

def gain_curve(x, threshold_db, ratio, time_s):
    """The gain a compressor would apply to x: an envelope follower (one time constant) on the
    louder channel, and `ratio`:1 above the threshold."""
    level = np.max(np.abs(x), axis=0)
    k = np.exp(-1 / (time_s * RATE))
    env = signal.lfilter([1 - k], [1, -k], level)
    over = 20 * np.log10(np.maximum(env, 1e-6)) - threshold_db
    return db(-np.maximum(over, 0) * (1 - 1 / ratio))


def random_compressor(rng, x):
    peak = np.max(np.abs(x)) + 1e-9
    threshold = 20 * np.log10(peak) - rng.uniform(3, 18)
    return x * gain_curve(x, threshold, rng.uniform(2, 8), rng.uniform(0.003, 0.08))


def saturate(rng, x):
    peak = np.max(np.abs(x)) + 1e-9
    drive = rng.uniform(1, 6)
    return np.tanh(x / peak * drive) / np.tanh(drive) * peak


# --- space ----------------------------------------------------------------------------------------

def pan(x, position):
    """Places a mono or stereo signal: position -1 (left) .. 1 (right), equal power."""
    if x.ndim == 1:
        x = np.stack([x, x])
    angle = (position + 1) * np.pi / 4
    return x * np.array([[np.cos(angle)], [np.sin(angle)]]) * np.sqrt(2)


def width(x, amount):
    mid, side = (x[0] + x[1]) / 2, (x[0] - x[1]) / 2 * amount
    return np.stack([mid + side, mid - side])


def random_reverb_ir(rng):
    """A room or plate: decaying stereo noise with a pre-delay, darker as it decays."""
    rt60 = rng.uniform(0.2, 2.0)
    n = int(rt60 * RATE)
    t = np.arange(n) / RATE
    ir = rng.standard_normal((2, n)) * np.exp(-6.9 * t / rt60)
    cutoff = rng.uniform(3000, 12000)
    ir = signal.sosfilt(signal.butter(1, cutoff / (RATE / 2), 'low', output='sos'), ir, axis=-1)
    ir = np.concatenate([np.zeros((2, int(rng.uniform(0, 0.04) * RATE))), ir], axis=1)
    return ir / np.sqrt(np.sum(ir ** 2) / 2)


def reverb(x, ir):
    return signal.fftconvolve(x, ir, axes=-1)[:, :x.shape[-1]]


# --- the whole kit --------------------------------------------------------------------------------

def bus(rng, stems):
    """Bus processing on stems [parts, 2, n]: the same filters and gain curve on every part."""
    if rng.random() < 0.5:                        # a highpass and/or a lowpass, like mastering or MP3
        lo = rng.uniform(20, 60)
        hi = rng.uniform(11000, 20000)
        sos = signal.butter(2, [lo / (RATE / 2), hi / (RATE / 2)], 'bandpass', output='sos')
        stems = signal.sosfilt(sos, stems, axis=-1)
    if rng.random() < 0.7:                        # bus compressor or limiter, driven by the mix
        mix = stems.sum(0)
        peak = np.max(np.abs(mix)) + 1e-9
        limiter = rng.random() < 0.5
        g = gain_curve(mix, 20 * np.log10(peak) - rng.uniform(1, 10),
                       20 if limiter else rng.uniform(1.5, 4), rng.uniform(0.001, 0.005) if limiter else rng.uniform(0.01, 0.1))
        stems = stems * g
    return stems
