# A short report on what a training run's best model does with each part, to paste back: how loud its
# output is next to the true part, how close it gets, and, for the low end (below 150 Hz, where the
# kick lives), which output that sound ends up in. Runs in full precision and in fp16, the way
# training checks itself.
#
# Usage: python diagnose.py [--run runs/large] [--data data]
import argparse, os
os.environ.setdefault('PYTORCH_ENABLE_MPS_FALLBACK', '1')
import numpy as np, torch
import model as M
import generator as G


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--run', default='runs/large')
    ap.add_argument('--data', default='data')
    args = ap.parse_args()
    device = torch.device('mps' if torch.backends.mps.is_available() else 'cuda' if torch.cuda.is_available() else 'cpu')
    state = torch.load(os.path.join(args.run, 'best.pt'), map_location='cpu', weights_only=True)
    cfg = M.load_config(state['config'])
    net = M.Net(cfg)
    net.load_state_dict(state['ema'])
    net = net.to(device).eval()
    dim_f, dim_t = cfg['audio']['dim_f'], cfg['audio']['dim_t']
    low = int(150 / (G.RATE / M.N_FFT)) + 1                  # the bins below 150 Hz

    examples = iter(G.Examples(args.data, M.frames_to_samples(dim_t) / G.RATE, seed=424242))
    batch = [next(examples) for _ in range(32)]
    print(f'Model from update {state["step"]}, on {device}')
    for half in (False, True):
        sums = {k: np.zeros(5) for k in ('target', 'out', 'err', 'low_target', 'low_out')}
        with torch.no_grad():
            for i in range(0, 32, 4):
                mix = torch.from_numpy(np.stack([b[0] for b in batch[i:i + 4]])).to(device)
                parts = torch.from_numpy(np.stack([b[1] for b in batch[i:i + 4]])).to(device)
                spec = M.spectrogram(mix, dim_f)
                with torch.autocast(device.type, dtype=torch.float16, enabled=half and device.type != 'cpu'):
                    out = net(spec)
                out = out.float()
                if not torch.isfinite(out).all():
                    print(f'  {"fp16" if half else "fp32"}: the output has inf/NaN values!')
                b, p, c, n = parts.shape
                target = M.spectrogram(parts.reshape(b * p, c, n), dim_f).reshape(out.shape)
                e = lambda x: (x ** 2).sum(dim=(0, 2, 3, 4)).cpu().numpy()
                sums['target'] += e(target)
                sums['out'] += e(out)
                sums['err'] += e(out - target)
                sums['low_target'] += e(target[:, :, :, :low])
                sums['low_out'] += e(out[:, :, :, :low])
        db = lambda x: 10 * np.log10(x + 1e-9)
        print(f'\n{"fp16" if half else "fp32"}:            ' + ''.join(f'{p:>9}' for p in G.CLASSES))
        print('  true part (dB)   ' + ''.join(f'{v:9.1f}' for v in db(sums['target'])))
        print('  model output (dB)' + ''.join(f'{v:9.1f}' for v in db(sums['out'])))
        print('  closeness (dB)   ' + ''.join(f'{v:9.1f}' for v in db(sums['target']) - db(sums['err'])))
        lt, lo = sums['low_target'], sums['low_out']
        print('  below 150 Hz, share of the true parts (%) ' + ' '.join(f'{v:5.0f}' for v in 100 * lt / lt.sum()))
        print('  below 150 Hz, share of the outputs (%)    ' + ' '.join(f'{v:5.0f}' for v in 100 * lo / max(lo.sum(), 1e-9)))
        print(f'  below 150 Hz, output level next to the truth: {db(lo.sum()) - db(lt.sum()):+.1f} dB')


if __name__ == '__main__':
    main()
