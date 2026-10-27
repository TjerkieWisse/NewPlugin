# Training KITNAP's drum split model

This trains our own model for KITNAP's second step: splitting a drum stem into kick, snare, toms,
hi-hat and cymbals. It's built only from material we're allowed to use commercially (see
[Data and licences](#data-and-licences)), so the trained model is ours.

The model is the same kind KITNAP already runs (MDX23C), in a smaller size that fits a laptop. A
trained model exports with `tools/export_drumsep.py` and drops into the plugin unchanged.

## How the training data is made

There are no recordings of songs with their drums split into parts that we could use, so the
examples are made on the fly, endlessly, each one different (`generator.py`):

1. **A groove:** a stretch of a real drummer's performance from the Groove MIDI Dataset, or a pattern
   programmed the way producers write them: trap (with hi-hat rolls), boom bap, house, pop/rock, R&B
   (`patterns.py`).
2. **A kit:** synthesised sounds (808s, punchy kicks, snares, claps, rims, toms, hats, crashes, rides;
   `synth.py`), a recorded acoustic kit, or a blend of both.
3. **Mixing:** every part gets its own level, EQ, compression, saturation, panning and reverb, and the
   whole kit a bus compressor or limiter (`fx.py`). Sometimes a little of other instruments is left
   in, like in a drum stem pulled out of a song.

The model hears the mix and learns to give back the five parts. `preview.py` writes examples to
listen to.

## On a Mac (Apple Silicon)

In Terminal, from this folder (`training/drumsplit` in the repository):

```bash
./setup_mac.sh                                   # once: Python and packages, in .venv here
.venv/bin/python fetch_data.py --groove-only     # the grooves (3 MB); enough for the speed test
.venv/bin/python bench.py                        # the speed test, about 10 minutes
.venv/bin/python preview.py                      # 10 example mixes with their parts, in previews/
```

`bench.py` saves its results to `bench_results.txt`. Send that file back.

While the speed test runs, the Mac gets warm and the fans spin up; that's normal. Keep it plugged in
and on a hard surface. If *Activity Monitor → Memory → Memory Pressure* turns red, stop it with
Ctrl+C and say so.

`fetch_data.py` without `--groove-only` also downloads the recorded acoustic kits: StemGMD's ten
kits (200 MB, quick) and three DrumGizmo kits (about 11 GB from a slow server, so this can take
hours), turned into a bank of 1–2 GB in `data/bank`. `--kits stemgmd` fetches only StemGMD. `--delete-downloads` deletes each download once it's converted. Without the kits, the
examples use synthesised sounds only.

## Data and licences

| What | Source | Licence |
|---|---|---|
| Grooves | [Groove MIDI Dataset](https://magenta.tensorflow.org/datasets/groove), Google Magenta | CC BY 4.0 |
| Acoustic kits | [StemGMD](https://zenodo.org/records/7882857) single hits: 10 kits (Politecnico di Milano). Its sounds come from Logic Pro's libraries: have a lawyer check this before release | CC BY 4.0 |
| Acoustic kits | [DRSKit](https://drumgizmo.org/wiki/doku.php?id=kits:drskit), [MuldjordKit](https://drumgizmo.org/wiki/doku.php?id=kits:muldjordkit), [CrocellKit](https://drumgizmo.org/wiki/doku.php?id=kits:crocellkit) from DrumGizmo | CC BY 4.0 |
| Synthesised sounds, patterns, effects | made by this code | ours |
| Model code | MDX23C / TFC-TDF v3 as in ZFTurbo's Music-Source-Separation-Training | MIT |

CC BY 4.0 allows commercial use with credit: KITNAP's credits must name these sources.
