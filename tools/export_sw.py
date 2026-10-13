# Converts the BS-RoFormer SW checkpoint (6 stems, released by jarredou) into the ONNX model KITNAP
# runs to pull the drums out of a song. Only the drums are exported, and the STFT is left out of
# the model: the plugin computes it (n_fft 2048, hop 512, periodic Hann window, center=True).
#
#   input  "spectrogram": [1, 2, 1025, frames, 2]  (channel, frequency bin, frame, real/imaginary)
#   output "drums":       [1, 2, 1025, frames, 2]  the drums' spectrogram, same layout
#
# frames is fixed at 345 (176128 samples, 4 s). The model was trained on 13 s chunks, but its
# attention over time needs memory that grows with the square of the chunk length (over 14 GB at
# 13 s, about 3 GB at 4 s); at 4 s the result is within -22 dB of the 13 s one.
#
# Usage: python export_sw.py <bs_roformer_sw.safetensors> <out.onnx>
# Needs: torch, onnx, onnxruntime, safetensors, numpy, and BS-RoFormer==0.4.1 (later versions of
# that package changed the architecture and can't load these weights).
import sys, numpy as np, torch
from torch import nn
from einops import rearrange, pack, unpack
from safetensors.torch import load_file
from bs_roformer import BSRoformer

STEMS = ['bass', 'drums', 'other', 'vocals', 'guitar', 'piano']
FRAMES = 345

def build(weights):
    freqs = tuple([2] * 24 + [4] * 12 + [12] * 8 + [24] * 8 + [48] * 8 + [128, 129])
    model = BSRoformer(dim=256, depth=12, stereo=True, num_stems=6, time_transformer_depth=1, freq_transformer_depth=1,
                       freqs_per_bands=freqs, dim_head=64, heads=8, attn_dropout=0.0, ff_dropout=0.0, flash_attn=False,
                       dim_freqs_in=1025, stft_n_fft=2048, stft_hop_length=512, stft_win_length=2048,
                       stft_normalized=False, mask_estimator_depth=2)
    model.load_state_dict(load_file(weights), strict=True)
    return model.eval()

class DrumsFromSpectrogram(nn.Module):
    """BSRoformer.forward between its STFT and its inverse STFT, for the drums stem only."""

    def __init__(self, model):
        super().__init__()
        self.model = model
        self.drums = STEMS.index('drums')

    def forward(self, spec):
        m = self.model
        stft_repr = rearrange(spec, 'b s f t c -> b (f s) t c')
        x = rearrange(stft_repr, 'b f t c -> b t (f c)')
        x = m.band_split(x)
        for time_transformer, freq_transformer in m.layers:
            x = rearrange(x, 'b t f d -> b f t d')
            x, ps = pack([x], '* t d')
            x = time_transformer(x)
            x, = unpack(x, ps, '* t d')
            x = rearrange(x, 'b f t d -> b t f d')
            x, ps = pack([x], '* f d')
            x = freq_transformer(x)
            x, = unpack(x, ps, '* f d')
        x = m.final_norm(x)
        mask = rearrange(m.mask_estimators[self.drums](x), 'b t (f c) -> b f t c', c=2)
        re = stft_repr[..., 0] * mask[..., 0] - stft_repr[..., 1] * mask[..., 1]
        im = stft_repr[..., 0] * mask[..., 1] + stft_repr[..., 1] * mask[..., 0]
        return rearrange(torch.stack([re, im], -1), 'b (f s) t c -> b s f t c', s=2)

def main():
    weights, out = sys.argv[1:3]
    model = build(weights)
    wrapper = DrumsFromSpectrogram(model).eval()

    # A real-looking input: the STFT of noise bursts, so the check covers the whole value range.
    torch.manual_seed(0)
    audio = torch.randn(2, (FRAMES - 1) * 512) * torch.rand(2, 1) * 0.3
    window = torch.hann_window(2048, periodic=True)
    spec = torch.view_as_real(torch.stft(audio, 2048, 512, window=window, center=True, return_complex=True))[None]

    with torch.no_grad():
        torch.onnx.export(wrapper, (spec,), out, input_names=['spectrogram'], output_names=['drums'],
                          opset_version=17, dynamo=False)

    # Check against the original model's own drums output (with its own STFT and inverse STFT).
    import onnxruntime
    with torch.inference_mode():
        expected = model(audio[None])[0, STEMS.index('drums')].numpy()
    got = onnxruntime.InferenceSession(out, providers=['CPUExecutionProvider']).run(None, {'spectrogram': spec.numpy()})[0]
    got = torch.view_as_complex(torch.from_numpy(got[0]).contiguous())
    got = torch.istft(got, 2048, 512, window=window, center=True, length=audio.shape[-1]).numpy()
    n = min(got.shape[-1], expected.shape[-1])
    error = float(np.abs(got[..., :n] - expected[..., :n]).max() / (np.abs(expected).max() + 1e-9))
    print('drums - relative error', error)
    if error > 1e-3:
        sys.exit('exported model does not match')

main()
