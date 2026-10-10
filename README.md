# KITNAP

**Steal the drum kit from any track.**

![KITNAP](docs/screenshot.png)

Drop a song or drum loop into KITNAP and it finds every drum hit, sorts them into kick, snare, clap, hat, open hat and perc, and turns each one into a clean, playable sample. Play the kit with MIDI, flip through variations, drag any sound straight into your DAW, or export the whole kit as WAVs.

Runs as VST3 (any DAW), AU (Logic, GarageBand) and as a standalone app.

## Using it

1. Put KITNAP on an instrument track.
2. Drop an audio file on the big strip at the top, or click it to browse. WAV, AIFF, FLAC, MP3 and OGG all work.
3. Watch the drums get kidnapped. Every hit lights up in its drum's colour along the waveform.
4. Click a pad to hear it. Use ◀ ▶ to flip through variations, for example when a track has two different snares.
5. Drag a pad onto a track or into your sampler to get the WAV, or press **EXPORT KIT** for a folder with all of them.

MIDI uses the General MIDI drum map, so any drum pattern plays it straight away:

| Pad      | Note |
|----------|------|
| Kick     | C1 (36) |
| Snare    | D1 (38) |
| Clap     | D#1 (39) |
| Hat      | F#1 (42) — chokes the open hat |
| Open hat | A#1 (46) |
| Perc     | A1 (45) |

The chosen sounds are saved inside your project, so it still plays if you move or delete the source file.

## How it works

1. **Find the hits.** Spectral-flux onset detection, then each hit is pinned to the sample by looking for the sharpest energy rise in three frequency bands. That way a quiet hat over a loud bass line is still placed exactly.
2. **Name them.** For every hit it measures the energy the hit *adds* compared with just before it (so bass and chords ringing underneath are ignored): low end, mids, top, noisiness, how long the top end rings, and whether there are several quick bursts (a clap). A kick and a hat landing together counts as both.
3. **Group them.** Hits of the same drum are compared waveform-to-waveform and grouped, so a song with two different snares gives you two variations.
4. **Stack them.** Every hit in a group is aligned to the sample and averaged. The drum is identical every time, but the music around it isn't, so averaging cancels out much of the rest of the song. This is where the clean sound comes from: the more often a drum repeats, the cleaner it gets.
5. **Clean up.** Each drum type gets its own filters (the kick's tail is low-passed to remove hats, hats are high-passed to remove the kick and bass), then the sound is trimmed before the next drum bleeds in, faded and normalised.

### What to expect

- **Drum loops, drum stems and beats with clear drums** (trap, house, lo-fi, boom bap) come out very clean.
- **Dense, fully mixed songs** work best when the drums repeat a lot. The more repeats, the cleaner the result. Some bleed from other instruments is normal.
- Layered drums (e.g. a snare stacked with a clap) come out as the layer you hear.
- **Coming next (phase 2):** AI drum separation before the analysis, so dense full songs come out as clean as loops.

> Sounds pulled from commercial songs are fine for practice and personal use, but clear samples before releasing music with them.

## Getting the plugin

Every push builds macOS, Windows and Linux versions automatically. Open the repository's **Actions** tab, pick the latest run, and download `KITNAP-macOS`, `KITNAP-Windows` or `KITNAP-Linux` under *Artifacts*.

Install by copying the plugin into your plugin folder:

| OS | VST3 | AU |
|----|------|----|
| macOS | `~/Library/Audio/Plug-Ins/VST3` | `~/Library/Audio/Plug-Ins/Components` |
| Windows | `C:\Program Files\Common Files\VST3` | — |
| Linux | `~/.vst3` | — |

On macOS the build isn't signed yet. If the DAW refuses to load it, run `xattr -cr ~/Library/Audio/Plug-Ins/VST3/KITNAP.vst3` (and the same for `Components/KITNAP.component`).

## Building it yourself

You need CMake 3.22+ and a C++17 compiler (Xcode on macOS, Visual Studio 2022 on Windows). JUCE is downloaded automatically.

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release --parallel
```

The plugins end up in `build/KITNAP_artefacts/Release/`.

### Tests

`KitnapAnalyzerTest` builds a synthetic beat (kick, snare, clap, hats, open hat) with a bass line and chords on top, runs the analyzer, and checks that every drum is detected, classified and extracted cleanly (each extracted sound must match the original with a correlation of 0.8 or higher; currently 0.97–1.0).

```bash
./build/KitnapAnalyzerTest_artefacts/Release/KitnapAnalyzerTest            # run the test
./build/KitnapAnalyzerTest_artefacts/Release/KitnapAnalyzerTest kit-out    # ...and write the extracted kit as WAVs
./build/KitnapAnalyzerTest_artefacts/Release/KitnapAnalyzerTest --file song.wav kit-out   # analyse your own file
```

On Linux, `-DKITNAP_BUILD_SCREENSHOT=ON` also builds `KitnapScreenshot`, which renders the UI to PNGs (run it under `xvfb-run`).

## Project layout

```
Source/
  analysis/DrumAnalyzer.*   onset detection, classification, grouping, stacking, cleanup
  engine/KitSampler.*       the 6-pad sampler (MIDI, choke, voice handling)
  ui/                       waveform/drop zone, pads, colours
  PluginProcessor.*         loading, background analysis, saving with the project, export
  PluginEditor.*            the window
tests/                      analyzer test and screenshot tool
```

## License note

KITNAP is built with [JUCE](https://juce.com). JUCE is dual-licensed: AGPLv3, or a commercial JUCE licence (including a free tier for small developers). Check the current terms at juce.com before selling KITNAP or releasing it closed-source.
