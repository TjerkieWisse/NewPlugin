# Your own music without drums, from my_music/: instrumentals, bass lines, synths, vocals, in any
# subfolders, as WAV, AIFF, FLAC, MP3 or OGG. It stands in for what a drum stem pulled out of a song
# still has of the other instruments. Only music you made or may use; the folder stays out of git.
import os, sys
import numpy as np, soundfile as sf
from scipy import signal

RATE = 44100
KINDS = ('.wav', '.aif', '.aiff', '.flac', '.mp3', '.ogg')


class Music:
    def __init__(self, folder):
        self.files = []                               # (path, frames, rate)
        for root, _, names in os.walk(folder):
            for name in sorted(names):
                if name.lower().endswith(KINDS) and not name.startswith('.'):
                    path = os.path.join(root, name)
                    try:
                        info = sf.info(path)
                    except Exception as e:
                        print(f'Skipping {path}: {e}', file=sys.stderr, flush=True)
                        continue
                    if info.frames > info.samplerate:         # a second at least
                        self.files.append((path, info.frames, info.samplerate))
        seconds = np.array([frames / rate for _, frames, rate in self.files])
        self.weights = seconds / seconds.sum() if len(seconds) else seconds
        self.hours = seconds.sum() / 3600

    def __len__(self):
        return len(self.files)

    def excerpt(self, rng, n):
        """n samples [2, n] from a random point in a random track (longer tracks more often), at 44.1 kHz;
        a stretch of silence is skipped for another one."""
        out = np.zeros((2, n))
        for _ in range(8):
            path, frames, rate = self.files[rng.choice(len(self.files), p=self.weights)]
            need = min(frames, int(np.ceil(n * rate / RATE)) + 8)
            try:
                audio, _ = sf.read(path, start=int(rng.integers(0, frames - need + 1)), frames=need,
                                   dtype='float64', always_2d=True)
            except Exception as e:
                print(f'Skipping {path}: {e}', file=sys.stderr, flush=True)
                continue
            audio = audio.T[:2] if audio.shape[1] > 1 else np.repeat(audio.T, 2, axis=0)
            if rate != RATE:
                audio = signal.resample_poly(audio, RATE, rate, axis=-1)
            audio = audio[:, :n]
            out = np.zeros((2, n))
            out[:, :audio.shape[1]] = audio
            if np.sqrt(np.mean(out ** 2)) > 1e-3:
                break
        return out
