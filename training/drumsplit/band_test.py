# How much of each part's sound the model gives back, per frequency band, on examples whose parts are
# known: 100% is right, less is dull, more is too much. A kick that comes out dull next to jarredou's
# shows here as a low share at 5-12 kHz and above. Also how close each part is, in dB.
#
# Usage: python band_test.py [--run runs/xlarge] [--data data] [--examples 24]
import argparse, os
os.environ.setdefault('PYTORCH_ENABLE_MPS_FALLBACK', '1')
import numpy as np, torch
import model as M
import generator as G

BANDS = [(30, 150), (150, 600), (600, 2000), (2000, 5000), (5000, 12000), (12000, 20000)]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--run', default='runs/xlarge')
    ap.add_argument('--data', default='data')
    ap.add_argument('--examples', type=int, default=24)
    args = ap.parse_args()

    device = torch.device('mps' if torch.backends.mps.is_available() else 'cuda' if torch.cuda.is_available() else 'cpu')
    state = torch.load(os.path.join(args.run, 'best.pt'), map_location='cpu', weights_only=True)
    cfg = M.load_config(state['config'])
    net = M.Net(cfg)
    net.load_state_dict(state['ema'])
    net = net.to(device).eval()
    dim_f, n = cfg['audio']['dim_f'], M.frames_to_samples(cfg['audio']['dim_t'])
    freqs = np.arange(dim_f) * G.RATE / M.N_FFT
    masks = [torch.from_numpy((freqs >= lo) & (freqs < hi)).to(device) for lo, hi in BANDS]

    # The same examples every time (as in the training's check: real multitracks it never trained on,
    # and made-up kits), without music underneath, so the numbers compare between versions.
    examples = iter(G.Examples(args.data, n / G.RATE, seed=777, held_out=True, music_folder=None))
    given, true = np.zeros((5, len(BANDS))), np.zeros((5, len(BANDS)))
    err, ref = np.zeros(5), np.zeros(5)
    with torch.no_grad():
        for _ in range(args.examples):
            mix, parts = next(examples)
            spec = M.spectrogram(torch.from_numpy(mix[None]).to(device), dim_f)
            out = net(spec)[0]
            target = M.spectrogram(torch.from_numpy(parts).to(device), dim_f)
            power = lambda s: s[:, 0::2] ** 2 + s[:, 1::2] ** 2                  # [5, 2, dim_f, frames]
            P, T = power(out), power(target)
            for b, m in enumerate(masks):
                given[:, b] += P[:, :, m].sum(dim=(1, 2, 3)).cpu().numpy()
                true[:, b] += T[:, :, m].sum(dim=(1, 2, 3)).cpu().numpy()
            err += ((out - target) ** 2).sum(dim=(1, 2, 3)).cpu().numpy()
            ref += (target ** 2).sum(dim=(1, 2, 3)).cpu().numpy()

    print(f'Model from update {state["step"]}, {args.examples} examples.')
    print('How much of each part comes back per band (100% is right, less is dull):')
    print('  %-9s' % 'Hz' + ''.join('%12s' % f'{lo}-{hi}' for lo, hi in BANDS) + '   closeness')
    for i, name in enumerate(G.CLASSES):
        # bands holding under 0.5% of the part's sound (a hi-hat's lows) say nothing: left out
        share = ''.join('%11.0f%%' % (100 * g / t) if t > 0.005 * true[i].sum() else '%12s' % '-' for g, t in zip(given[i], true[i]))
        print('  %-9s' % name + share + '%9.1f dB' % (10 * np.log10(ref[i] / err[i])))


if __name__ == '__main__':
    main()
