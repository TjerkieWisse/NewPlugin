# Turns a training run's best model into the files KITNAP uses: model.ckpt (the weights) and
# kitnap_drumsep5.onnx, the split model the plugin loads, checked against PyTorch by
# tools/export_drumsep.py.
#
# Usage: python export.py [--run runs/large]
import argparse, os, sys, torch
import model as M
import export_drumsep  # from tools/, via model.py


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--run', default='runs/large')
    args = ap.parse_args()
    state = torch.load(os.path.join(args.run, 'best.pt'), map_location='cpu', weights_only=True)
    ckpt = os.path.join(args.run, 'model.ckpt')
    torch.save({'state_dict': state['ema']}, ckpt)
    out = os.path.join(args.run, 'kitnap_drumsep5.onnx')
    sys.argv = ['export_drumsep.py', ckpt, state['config'], out]
    export_drumsep.main()
    print(f'Exported the model from update {state["step"]} to {out}')


if __name__ == '__main__':
    main()
