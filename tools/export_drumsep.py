# Converts a DrumSep MDX23C checkpoint (jarredou's 5-stem v2025 model: kick, snare, toms, hi-hat,
# cymbals) into the ONNX model KITNAP runs to split a drum stem into its parts. The STFT is left out
# of the model: the plugin computes it (n_fft 2048, hop 512, the first dim_f bins, dim_t frames per
# chunk, both from the config).
#
# Usage: python export_drumsep.py <model.ckpt> <model.yaml> <out.onnx>
# Needs: torch, onnx, pyyaml, numpy (pip install torch onnx pyyaml numpy)
import sys, yaml, numpy as np, torch, torch.nn as nn

def norm(c):
    return nn.InstanceNorm2d(c, affine=True)

class TFC_TDF(nn.Module):
    def __init__(self, in_c, c, l, f, bn):
        super().__init__()
        self.blocks = nn.ModuleList()
        for _ in range(l):
            b = nn.Module()
            b.tfc1 = nn.Sequential(norm(in_c), nn.GELU(), nn.Conv2d(in_c, c, 3, 1, 1, bias=False))
            b.tdf = nn.Sequential(norm(c), nn.GELU(), nn.Linear(f, f // bn, bias=False),
                                  norm(c), nn.GELU(), nn.Linear(f // bn, f, bias=False))
            b.tfc2 = nn.Sequential(norm(c), nn.GELU(), nn.Conv2d(c, c, 3, 1, 1, bias=False))
            b.shortcut = nn.Conv2d(in_c, c, 1, 1, 0, bias=False)
            self.blocks.append(b)
            in_c = c

    def forward(self, x):
        for b in self.blocks:
            s = b.shortcut(x)
            x = b.tfc1(x)
            x = x + b.tdf(x)
            x = b.tfc2(x)
            x = x + s
        return x

class Scale(nn.Module):
    def __init__(self, in_c, out_c, scale, transpose):
        super().__init__()
        layer = nn.ConvTranspose2d if transpose else nn.Conv2d
        self.conv = nn.Sequential(norm(in_c), nn.GELU(), layer(in_c, out_c, scale, scale, bias=False))

    def forward(self, x):
        return self.conv(x)

class Net(nn.Module):
    def __init__(self, cfg):
        super().__init__()
        m, a = cfg['model'], cfg['audio']
        self.instruments = cfg['training']['instruments']
        self.k = m['num_subbands']
        # 'mask': the network gives each part a complex mask that is applied to the input (KITNAP's own
        # models). Otherwise it gives the parts' spectrograms directly (jarredou's DrumSep).
        self.mask = m.get('output') == 'mask'
        # normalize: true scales the input to a fixed level before the network (the masks still apply
        # to the input as it is), which keeps the network's numbers in a stable range at any loudness.
        self.normalize = str(m.get('normalize', '')).lower() == 'true'
        dim_c = self.k * a['num_channels'] * 2
        n, scale, l, c, g, bn = m['num_scales'], m['scale'], m['num_blocks_per_scale'], m['num_channels'], m['growth'], m['bottleneck_factor']
        f = a['dim_f'] // self.k
        self.first_conv = nn.Conv2d(dim_c, c, 1, 1, 0, bias=False)
        self.encoder_blocks = nn.ModuleList()
        for _ in range(n):
            b = nn.Module()
            b.tfc_tdf = TFC_TDF(c, c, l, f, bn)
            b.downscale = Scale(c, c + g, scale, False)
            f //= scale[1]; c += g
            self.encoder_blocks.append(b)
        self.bottleneck_block = TFC_TDF(c, c, l, f, bn)
        self.decoder_blocks = nn.ModuleList()
        for _ in range(n):
            b = nn.Module()
            b.upscale = Scale(c, c - g, scale, True)
            f *= scale[1]; c -= g
            b.tfc_tdf = TFC_TDF(2 * c, c, l, f, bn)
            self.decoder_blocks.append(b)
        self.final_conv = nn.Sequential(nn.Conv2d(c + dim_c, c, 1, 1, 0, bias=False), nn.GELU(),
                                        nn.Conv2d(c, len(self.instruments) * dim_c, 1, 1, 0, bias=False))

    # x: [1, 4, dim_f, dim_t] spectrogram (left re, left im, right re, right im), as torch.stft gives
    # it with center=True and a periodic Hann window. Returns [1, stems, 4, dim_f, dim_t].
    def forward(self, x):
        spec = x
        if self.normalize:
            x = x / (torch.sqrt(torch.mean(x * x, dim=(1, 2, 3), keepdim=True)) + 1e-5)
        b, c, f, t = x.shape
        mix = x = x.reshape(b, c, self.k, f // self.k, t).reshape(b, c * self.k, f // self.k, t)
        first = x = self.first_conv(x)
        x = x.transpose(-1, -2)
        skips = []
        for blk in self.encoder_blocks:
            x = blk.tfc_tdf(x); skips.append(x); x = blk.downscale(x)
        x = self.bottleneck_block(x)
        for blk in self.decoder_blocks:
            x = blk.upscale(x); x = torch.cat([x, skips.pop()], 1); x = blk.tfc_tdf(x)
        x = x.transpose(-1, -2) * first
        x = self.final_conv(torch.cat([mix, x], 1))
        b, c, f, t = x.shape
        x = x.reshape(b, c // self.k, self.k, f, t).reshape(b, c // self.k, f * self.k, t)
        x = x.reshape(b, len(self.instruments), -1, f * self.k, t)
        if self.mask:
            # Complex multiplication per channel; every part starts out as an equal share of the input.
            m_re, m_im = x[:, :, 0::2] + 1.0 / len(self.instruments), x[:, :, 1::2]
            s_re, s_im = spec[:, None, 0::2], spec[:, None, 1::2]
            x = torch.stack([m_re * s_re - m_im * s_im, m_re * s_im + m_im * s_re], dim=3)
            x = x.reshape(b, len(self.instruments), -1, f * self.k, t)
        return x


def load_config(path):
    # BaseLoader: the config holds Python-specific tags that safe loaders reject; the numbers we need
    # are converted by hand.
    cfg = yaml.load(open(path), Loader=yaml.BaseLoader)
    num = lambda v: int(v) if str(v).lstrip('-').isdigit() else v
    for sec in ('audio', 'model'):
        cfg[sec] = {k: ([num(x) for x in v] if isinstance(v, list) else num(v)) for k, v in cfg[sec].items()}
    return cfg

def main():
    ckpt, config, out = sys.argv[1:4]
    cfg = load_config(config)
    net = Net(cfg)
    state = torch.load(ckpt, map_location='cpu', weights_only=True)
    net.load_state_dict(state.get('state_dict', state), strict=True)
    net.eval()

    dummy = torch.randn(1, 4, cfg['audio']['dim_f'], cfg['audio']['dim_t'])
    with torch.no_grad():
        torch.onnx.export(net, (dummy,), out, input_names=['spectrogram'], output_names=['stems'],
                          opset_version=17, dynamo=False)

    # Check the exported model against PyTorch.
    import onnxruntime
    with torch.inference_mode():
        expected = net(dummy).numpy()
    got = onnxruntime.InferenceSession(out, providers=['CPUExecutionProvider']).run(None, {'spectrogram': dummy.numpy()})[0]
    error = float(np.abs(got - expected).max() / (np.abs(expected).max() + 1e-9))
    print('stems:', ', '.join(net.instruments), '- relative error', error)
    if error > 1e-3:
        sys.exit('exported model does not match')

if __name__ == '__main__':
    main()
