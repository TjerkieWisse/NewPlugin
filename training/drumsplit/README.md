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

Once `multitrack.py` has fetched them, a third of the examples start instead from **real drum
multitracks**: whole performances recorded part by part (StemGMD), sped up or slowed down a little,
sometimes with one part swapped for another recording's or left out, then mixed the same way.

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

## Your own sounds

Put your own drum one-shots in `my_sounds/` next to this README, one folder per collection, with
subfolders as you like (`my_sounds/trap/808s/...`, `my_sounds/house/Hats/Open/...`). Then:

```bash
.venv/bin/python fetch_data.py --kits mine
```

It reads which instrument each sound is from its folder and file names (kick, 808, snare, clap, rim,
tom, hat, open hat, crash, ride), checks that it's one clear hit of that kind, and lists what it
skipped. Run it again after adding sounds. Only use sounds you made or recorded yourself, or have
the maker's permission to train on: many sample packs forbid it.

## Real drum multitracks

StemGMD has the Groove MIDI drummers' performances played on ten acoustic kits, every drum on its own
track: 143 hours, 1.13 TB in seven 31 GB archive files. `multitrack.py` reads the archive's table of
contents and downloads only the recordings it picks, spread evenly over the drummers and kits, and
keeps them as our five parts:

```bash
.venv/bin/python fetch_data.py --multitrack 20     # 20 hours of drums: about 30 GB download, 10 GB on disk
```

It says up front how much it will download and how much disk it needs. Stop it with Ctrl+C and run it
again to carry on; ask for more hours later and it adds to what's there. Half an hour of performances
the model never trains on is kept apart, for the check. It can run while training does.

To add them to a training run that's already going: once the fetch is done, stop the training with
Ctrl+C and start it again with the same command. It then trains on them too. The check now includes
real recordings, so its scores drop and the best score starts afresh; that's expected.

## Training

```bash
.venv/bin/python train.py --config configs/large2.yaml      # round 2: about the same size as round 1
.venv/bin/python train.py --config configs/xlarge.yaml      # round 2, about twice as big and slower
```

Each run goes into `runs/<config name>/`. The round-2 configs (`large2`, `xlarge`) level the input
inside the model, so fp16 stays stable, and train on 6-second pieces; `bench.py` times them all.

It prints its progress every 50 updates and checks itself every 1000 (how close each part comes to
the truth, in dB: higher is better). Press **Ctrl+C** to stop: it saves first. Run the same command
again to carry on. Everything goes into `runs/<config name>/`; the best version so far is `best.pt`.

To hear how it's doing at any point, split a drum stem with the best version so far:

```bash
.venv/bin/python split.py "path/to/drums.wav" --run runs/large2     # writes the five parts next to it
```

When it's done, `export.py --run runs/<config name>` makes `kitnap_drumsep5.onnx` there, the file the plugin loads in
place of the current split model.

If the GPU runs short of memory during a long run, training clears it and carries on by itself.

### At jarredou's size, on a rented GPU

`configs/xxl.yaml` is the size of jarredou's DrumSep: 128 channels, 109M parameters. It doesn't fit
a 16 GB Mac, and it needs far more training than a laptop can do in reasonable time, so it's for a
rented computer with an NVIDIA GPU (an RTX 4090 or better, under Linux; RunPod, Vast.ai and Lambda
rent them by the hour). On that computer:

```bash
git clone -b claude/friendly-planck-k1gyiw https://github.com/TjerkieWisse/NewPlugin.git
cd NewPlugin/training/drumsplit
./setup_mac.sh                                       # works on Linux too
.venv/bin/python fetch_data.py --kits stemgmd,drs,muldjord,crocell
.venv/bin/python fetch_data.py --multitrack 40
.venv/bin/python bench.py --sizes xxl --batches 2,4,8
.venv/bin/python train.py --config configs/xxl.yaml --batch 4 --accum 2 --workers 12 --examples 600000
```

Your own sounds and the Freesound ones: copy `my_sounds/` and `data/freesound_key.txt` over and run
`fetch_data.py --kits freesound,mine` as well. `--examples 600000` is about 1000 hours of drums, four
times a laptop run; `bench.py` tells how long that takes there. Copy `runs/xxl/best.pt` back when it's
done.

## Data and licences

| What | Source | Licence |
|---|---|---|
| Grooves | [Groove MIDI Dataset](https://magenta.tensorflow.org/datasets/groove), Google Magenta | CC BY 4.0 |
| Acoustic kits | [StemGMD](https://zenodo.org/records/7882857) single hits: 10 kits (Politecnico di Milano). Its sounds come from Logic Pro's libraries: have a lawyer check this before release | CC BY 4.0 |
| Drum multitracks | [StemGMD](https://zenodo.org/records/7860223) performances, the same kits; same lawyer check | CC BY 4.0 |
| One-shots, many electronic | [Freesound](https://freesound.org), only sounds their makers released as CC0; `data/bank/freesound/credits.csv` lists each one and its maker | CC0 |
| Acoustic kits | [DRSKit](https://drumgizmo.org/wiki/doku.php?id=kits:drskit), [MuldjordKit](https://drumgizmo.org/wiki/doku.php?id=kits:muldjordkit), [CrocellKit](https://drumgizmo.org/wiki/doku.php?id=kits:crocellkit) from DrumGizmo | CC BY 4.0 |
| Your own one-shots (`my_sounds/`) | you | yours |
| Synthesised sounds, patterns, effects | made by this code | ours |
| Model code | MDX23C / TFC-TDF v3 as in ZFTurbo's Music-Source-Separation-Training | MIT |

CC BY 4.0 allows commercial use with credit: KITNAP's credits must name these sources. CC0 needs no
credit, but the list is kept anyway.
