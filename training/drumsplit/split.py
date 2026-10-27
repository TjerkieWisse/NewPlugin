# Splits a drum stem into its five parts with a training run's best model, to listen to how it's
# doing. Works like the plugin: chunks of the training length, overlapping by half and cross-faded.
#
# Usage: python split.py <drums.wav> [--run runs/large] [--out <folder>]
import argparse, os
os.environ.setdefault('PYTORCH_ENABLE_MPS_FALLBACK', '1')
import numpy as np, soundfile as sf, torch
import model as M
import generator as G


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('drums')
    ap.add_argument('--run', default='runs/large')
    ap.add_argument('--out', default=None)
    args = ap.parse_args()

    device = torch.device('mps' if torch.backends.mps.is_available() else 'cpu')
    state = torch.load(os.path.join(args.run, 'best.pt'), map_location='cpu', weights_only=True)
    cfg = M.load_config(state['config'])
    net = M.Net(cfg)
    net.load_state_dict(state['ema'])
    net = net.to(device).eval()
    dim_f, dim_t = cfg['audio']['dim_f'], cfg['audio']['dim_t']

    audio, rate = sf.read(args.drums, dtype='float32', always_2d=True)
    if rate != G.RATE:
        raise SystemExit(f'{args.drums} is at {rate} Hz; it needs to be 44.1 kHz.')
    audio = audio[:, :2] if audio.shape[1] > 1 else np.repeat(audio, 2, axis=1)
    chunk = M.frames_to_samples(dim_t)
    hop = chunk // 2
    n = len(audio)
    padded = np.pad(audio.T, ((0, 0), (hop, hop + chunk)))
    fade = np.concatenate([np.linspace(0, 1, hop), np.linspace(1, 0, chunk - hop)])
    parts = np.zeros((5, 2, padded.shape[1]), dtype=np.float32)
    weight = np.zeros(padded.shape[1], dtype=np.float32)
    window = torch.hann_window(M.N_FFT, device=device)
    with torch.no_grad():
        for start in range(0, padded.shape[1] - chunk + 1, hop):
            x = torch.from_numpy(padded[None, :, start:start + chunk].copy()).to(device)
            out = net(M.spectrogram(x, dim_f))[0]                          # [5, 4, dim_f, dim_t]
            spec = torch.complex(out[:, 0::2], out[:, 1::2])               # [5, 2, dim_f, dim_t]
            spec = torch.nn.functional.pad(spec, (0, 0, 0, M.N_FFT // 2 + 1 - dim_f))
            y = torch.istft(spec.reshape(10, -1, dim_t), M.N_FFT, M.HOP, window=window, length=chunk)
            parts[:, :, start:start + chunk] += y.reshape(5, 2, chunk).cpu().numpy() * fade
            weight[start:start + chunk] += fade
    parts = parts[:, :, hop:hop + n] / np.maximum(weight[hop:hop + n], 1e-6)

    out = args.out or os.path.splitext(args.drums)[0] + ' - parts'
    os.makedirs(out, exist_ok=True)
    for i, name in enumerate(G.CLASSES):
        sf.write(os.path.join(out, f'{i + 1} {name}.wav'), parts[i].T, G.RATE, subtype='PCM_24')
    print('Parts written to', out)


if __name__ == '__main__':
    main()
