# Writes a few training examples to listen to: each as the mix plus its five parts, the answers the
# model learns to give.
#
# Usage: python preview.py [--data data] [--count 10] [--out previews]
import argparse, os
import numpy as np, soundfile as sf
import generator


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--data', default='data')
    ap.add_argument('--count', type=int, default=10)
    ap.add_argument('--seconds', type=float, default=8.0)
    ap.add_argument('--out', default='previews')
    ap.add_argument('--seed', type=int, default=1)
    args = ap.parse_args()

    examples = iter(generator.Examples(args.data, args.seconds, seed=args.seed))
    for i in range(args.count):
        mix, parts = next(examples)
        folder = os.path.join(args.out, f'{i + 1:02d}')
        os.makedirs(folder, exist_ok=True)
        sf.write(os.path.join(folder, '0 mix.wav'), mix.T, generator.RATE, subtype='PCM_16')
        for c, name in enumerate(generator.CLASSES):
            if np.any(parts[c]):
                sf.write(os.path.join(folder, f'{c + 1} {name}.wav'), parts[c].T, generator.RATE, subtype='PCM_16')
        print('wrote', folder)


if __name__ == '__main__':
    main()
