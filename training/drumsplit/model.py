# The network KITNAP runs to split drums (MDX23C, TFC-TDF v3), the same code tools/export_drumsep.py
# exports, so a model trained here exports and drops into the plugin unchanged. Plus the spectrogram in
# the plugin's layout and the training loss.
import os, sys
import torch

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..', 'tools'))
from export_drumsep import Net, load_config  # noqa: E402

N_FFT, HOP = 2048, 512


def frames_to_samples(dim_t):
    """The length of audio whose spectrogram (centred frames) has dim_t frames."""
    return HOP * (dim_t - 1)


def spectrogram(audio, dim_f):
    """audio [batch, 2, samples] -> [batch, 4, dim_f, frames]: left re, left im, right re, right im,
    as the plugin computes it (periodic Hann window, centred frames, the lowest dim_f bins)."""
    b, c, n = audio.shape
    window = torch.hann_window(N_FFT, device=audio.device)
    spec = torch.stft(audio.reshape(b * c, n), N_FFT, HOP, window=window, center=True, return_complex=True)
    spec = torch.view_as_real(spec[:, :dim_f])                    # [b*c, dim_f, frames, 2]
    return spec.reshape(b, c, dim_f, -1, 2).permute(0, 1, 4, 2, 3).reshape(b, c * 2, dim_f, -1)


def loss_fn(predicted, mix_spec, parts, dim_f):
    """L1 between the predicted and true part spectrograms, on the complex values and on the
    magnitudes. predicted [b, parts, 4, dim_f, frames]; parts [b, parts, 2, samples]."""
    b, p, c, n = parts.shape
    target = spectrogram(parts.reshape(b * p, c, n), dim_f).reshape(predicted.shape)
    complex_l1 = (predicted - target).abs().mean()
    mag = lambda s: torch.sqrt(s[:, :, 0::2] ** 2 + s[:, :, 1::2] ** 2 + 1e-8)
    return complex_l1 + (mag(predicted) - mag(target)).abs().mean()
