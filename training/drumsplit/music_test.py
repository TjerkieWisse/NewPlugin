# Does real music under the drums throw the model off? A drum stem pulled out of a song still has a
# little of the other instruments in it; in training, the model only ever heard simple tones in their
# place. This splits the same drums three ways with a trained model: on their own, with those tones
# underneath, and with your own drumless music (my_music/) underneath at the same level. It prints
# how close each part comes to the truth each time, and how much of the added sound ends up in the
# parts. If your music does clearly more harm than the tones, training with it should help.
#
# Usage: python music_test.py [--music my_music] [--run runs/xlarge] [--data data] [--examples 40] [--level -20]
import argparse, os
os.environ.setdefault('PYTORCH_ENABLE_MPS_FALLBACK', '1')
import numpy as np, torch
import model as M
import generator as G
import music


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--music', default='my_music')
    ap.add_argument('--run', default='runs/xlarge')
    ap.add_argument('--data', default='data')
    ap.add_argument('--examples', type=int, default=40)
    ap.add_argument('--level', type=float, default=-20, help='the added sound, in dB relative to the drums')
    args = ap.parse_args()

    tracks = music.Music(args.music)
    if not len(tracks):
        raise SystemExit(f'No music in {args.music}/ yet: put a few of your own tracks without drums there '
                         '(WAV, AIFF, FLAC, MP3).')
    if not os.path.exists(os.path.join(args.run, 'best.pt')):
        raise SystemExit(f'No trained model in {args.run}.')
    device = torch.device('mps' if torch.backends.mps.is_available() else 'cuda' if torch.cuda.is_available() else 'cpu')
    state = torch.load(os.path.join(args.run, 'best.pt'), map_location='cpu', weights_only=True)
    cfg = M.load_config(state['config'])
    net = M.Net(cfg)
    net.load_state_dict(state['ema'])
    net = net.to(device).eval()
    dim_f, n = cfg['audio']['dim_f'], M.frames_to_samples(cfg['audio']['dim_t'])
    print(f'Model from update {state["step"]}; {len(tracks)} tracks, {tracks.hours * 60:.0f} minutes of music; '
          f'added at {args.level:g} dB under the drums.', flush=True)

    def split(audio):
        """The model's five parts for audio [2, n], and the audio's own spectrogram."""
        spec = M.spectrogram(torch.from_numpy(audio[None].astype(np.float32)).to(device), dim_f)
        out = net(spec)[0]
        return torch.complex(out[:, 0::2], out[:, 1::2]).cpu(), torch.complex(spec[0, 0::2], spec[0, 1::2]).cpu()

    energy = lambda s: (s.abs() ** 2).sum(dim=(1, 2, 3))          # per part, of a spectrogram [parts, 2, f, t]
    rng = np.random.default_rng(13579)
    examples = iter(G.Examples(args.data, n / G.RATE, seed=2468, held_out=True))
    kinds = ['drums only', 'with test tones', 'with your music']
    accuracy = {k: np.zeros(len(G.CLASSES)) for k in kinds}
    landed = {k: np.zeros(len(G.CLASSES)) for k in kinds[1:]}
    counted = np.zeros(len(G.CLASSES))
    with torch.no_grad():
        for i in range(args.examples):
            _, parts = next(examples)
            drums = parts.sum(0).astype(np.float64)               # without the residue the generator may add
            rms = np.sqrt(np.mean(drums ** 2)) + 1e-9
            added = {}
            for kind, sound in (('with test tones', G._residue(rng, n)), ('with your music', tracks.excerpt(rng, n))):
                added[kind] = sound / (np.sqrt(np.mean(sound ** 2)) + 1e-9) * rms * 10 ** (args.level / 20)
            truth = M.spectrogram(torch.from_numpy(parts), dim_f)
            truth = torch.complex(truth[:, 0::2], truth[:, 1::2])
            present = (energy(truth) > 1e-5 * energy(truth).sum()).numpy()
            counted += present
            clean, _ = split(drums)
            for kind in kinds:
                if kind == 'drums only':
                    out = clean
                else:
                    out, _ = split(drums + added[kind])
                    _, sound_spec = split(added[kind])
                    landed[kind] += (energy(out - clean) / (energy(sound_spec[None]) + 1e-12)).numpy()
                snr = 10 * torch.log10(energy(truth) / (energy(out - truth) + 1e-12) + 1e-12).numpy()
                accuracy[kind] += np.where(present, snr, 0)
            if (i + 1) % 10 == 0:
                print(f'  {i + 1}/{args.examples}', flush=True)

    print('\nHow close each part is to the truth (dB, higher is better):')
    print(f'  {"":18}' + ''.join(f'{c:>9}' for c in G.CLASSES) + '  average')
    for kind in kinds:
        row = accuracy[kind] / np.maximum(counted, 1)
        print(f'  {kind:18}' + ''.join(f'{v:9.1f}' for v in row) + f'{row.mean():9.1f}')
    print('\nHow much of the added sound ends up in each part (% of its energy; 0 would be ideal):')
    print(f'  {"":18}' + ''.join(f'{c:>9}' for c in G.CLASSES) + '    total')
    for kind in kinds[1:]:
        row = 100 * landed[kind] / args.examples
        print(f'  {kind[5:]:18}' + ''.join(f'{v:8.1f}%' for v in row) + f'{row.sum():8.1f}%')
    cost = {k: np.mean((accuracy['drums only'] - accuracy[k]) / np.maximum(counted, 1)) for k in kinds[1:]}
    print(f'\nThe test tones cost {cost["with test tones"]:.1f} dB on average, your music {cost["with your music"]:.1f} dB.')


if __name__ == '__main__':
    main()
