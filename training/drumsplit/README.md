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
hours), turned into a bank of 1–2 GB in `data/bank`. `--kits stemgmd` fetches only StemGMD.
`--kits freesound` fetches CC0 one-shots from Freesound: it asks once for your free API key
(freesound.org/apiv2/apply) and keeps it in `data/freesound_key.txt`. `--delete-downloads` deletes each download once it's converted. Without the kits, the
examples use synthesised sounds only.

## Training

```bash
.venv/bin/python train.py                         # the large model, about 15 hours on an M5
```

It prints its progress every 50 updates and checks itself every 1000 (how close each part comes to
the truth, in dB: higher is better). Press **Ctrl+C** to stop: it saves first. Run the same command
again to carry on. Everything goes into `runs/large/`; the best version so far is `best.pt`.

To hear how it's doing at any point, split a drum stem with the best version so far:

```bash
.venv/bin/python split.py "path/to/drums.wav"     # writes the five parts next to it
```

When it's done, `export.py` makes `runs/large/kitnap_drumsep5.onnx`, the file the plugin loads in
place of the current split model.

## Data and licences

| What | Source | Licence |
|---|---|---|
| Grooves | [Groove MIDI Dataset](https://magenta.tensorflow.org/datasets/groove), Google Magenta | CC BY 4.0 |
| Acoustic kits | [StemGMD](https://zenodo.org/records/7882857) single hits: 10 kits (Politecnico di Milano). Its sounds come from Logic Pro's libraries: have a lawyer check this before release | CC BY 4.0 |
| One-shots, many electronic | [Freesound](https://freesound.org), only sounds their makers released as CC0; `data/bank/freesound/credits.csv` lists each one and its maker | CC0 |
| Acoustic kits | [DRSKit](https://drumgizmo.org/wiki/doku.php?id=kits:drskit), [MuldjordKit](https://drumgizmo.org/wiki/doku.php?id=kits:muldjordkit), [CrocellKit](https://drumgizmo.org/wiki/doku.php?id=kits:crocellkit) from DrumGizmo | CC BY 4.0 |
| Synthesised sounds, patterns, effects | made by this code | ours |
| Model code | MDX23C / TFC-TDF v3 as in ZFTurbo's Music-Source-Separation-Training | MIT |

CC BY 4.0 allows commercial use with credit: KITNAP's credits must name these sources. CC0 needs no
credit, but the list is kept anyway.
