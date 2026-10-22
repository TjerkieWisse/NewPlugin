# KITNAP

**Steal the drum kit from any track.**

![KITNAP](docs/screenshot.png)

Drop a song into KITNAP and two AI models take the drums apart: the first pulls the drums out of the mix, the second splits them into kick, snare, toms, hi-hat and cymbals. Each of those becomes a pad, with single hits cut straight out of its part, exactly as they sound in the record. Play the kit with MIDI, flip through variations, drag any sound straight into your DAW, or export the whole kit as WAVs.

Runs as VST3 (any DAW), AU (Logic, GarageBand) and as a standalone app.

## Using it

1. Put KITNAP on an instrument track.
2. Drop an audio file on the big strip at the top, or click it to browse. WAV, AIFF, FLAC, MP3 and OGG all work.
3. Watch the drums get kidnapped: a scanner sweeps through the song while the AI separates the drums from the rest of the song, splits them into kick, snare, toms, hi-hat and cymbals, and cuts out the sounds. A full song takes a few minutes; the kit is saved with your project, so this only happens once.
4. Click a pad to hear it. Use ◀ ▶ to flip through variations, for example when a track has two different snares, or an open and a closed hi-hat. Each pad shows how many of the hits the AI heard sound like its current sound.
5. Drag a pad onto a track or into your sampler to get the WAV, or press **EXPORT KIT** for a folder with all of them.

The window has a film look (grain, flicker, dust, a faint colour fringe); **FILM** in the header turns it off, and the choice is saved with the project.

MIDI uses the General MIDI drum map, so any drum pattern plays it straight away:

| Pad      | Notes |
|----------|-------|
| Kick     | C1 (36), B0 (35) |
| Snare    | D1 (38), and 37, 39, 40 |
| Toms     | A1 (45), and 41, 43, 47, 48, 50 |
| Hi-hat   | F#1 (42), and 44, 46 |
| Cymbals  | C#2 (49), and 51, 52, 53, 55, 57, 59 |

The whole kit (every variation, the hit map and the waveform) is saved inside your project, so reopening a session is instant and still works if you move or delete the source file.

## How it works

KITNAP only uses what the two models give it. Nothing is classified, filtered, averaged or generated afterwards.

0. **Pick the sections with drums.** A kit needs one clean hit of each sound, and drum sounds repeat, so the AI doesn't have to hear the whole song. A quick scan (no AI) measures how many sudden attacks every stretch of the song has; the song is divided into five parts, and from each the 12 seconds with the most drum hits are taken (a part far quieter in hits than the busiest, like an intro without drums, is skipped). That's at most a minute of audio, joined with half a second of silence between the sections, so a 4-minute song takes about a quarter of the time. The editor doesn't show which sections were picked: its scanner sweeps evenly through the whole song. Songs under 75 seconds are used whole.

1. **Separate the drums (AI).** BS-RoFormer SW, a band-split transformer model, run with ONNX Runtime, removes everything that isn't drums. The song is processed in 4-second chunks that are cross-faded over half a second. (The model was trained on 13-second chunks, but its memory use grows with the square of the chunk length: over 14 GB at 13 s, about 3 GB at 4 s, with a result within −22 dB of the 13 s one.)
2. **Split the kit (AI).** A second model (DrumSep 5 stems, MDX23C) splits the drum stem into kick, snare, toms, hi-hat and cymbals, in 12-second chunks that overlap by half and are cross-faded. These five parts are the five pads: whatever the model puts in the snare part ends up on the snare pad, and so on.
3. **Find the hits in each part.** Spectral-flux onset detection, then each hit is pinned to where its attack rises most above what was sounding just before. Whatever the split leaves of other drums in a part is far quieter than the part's own hits, so only hits within 30 dB of the part's loud ones count (and a part that is more than 35 dB below the loudest one, like the toms part of a song without toms, stays empty).
4. **Group the hits that sound alike.** Every hit gets a fingerprint: its level in 24 frequency bands over its first 80 ms, minus whatever was still ringing from before it, relative to its own loudest band. Hits are grouped step by step, always merging the two groups that are closest on average, until the closest two are clearly further apart than hits of the same drum usually are in that part (a programmed sample barely changes, a live drummer does). So in a song whose beat switches (Sicko Mode), each beat's kick and snare stays its own sound. Each group is a variation on the pad, the most frequent first. A sound that occurs only once (or in under 2% of the part's hits) is left out.
5. **Pick the cleanest hit and cut it out.** Per group, among the loud hits, the one with the most room to ring out before the part's next hit and the least of the previous hit underneath it. It's cut straight out of the part, from 2 ms before the hit to where it has died away (45 dB down) or the next hit starts, with only a 2 ms fade-in and a 5 ms fade-out so it doesn't click. No level change.

### What to expect

- A sound that only occurs outside the picked sections (a crash only at the very start, say) isn't in the kit.
- How clean a pad sounds is down to the two models: what's left of other drums in a part (a hat under the snare, say) stays in its sound, and a drum the model puts in the wrong part ends up on the wrong pad.
- Each sound is a single hit from the song, so it carries what was playing at that moment in its part. When the next hit of the same part comes quickly (fast hats), the sound is that short.
- **On a Mac the AI can run on the GPU** (through Core ML, in 32-bit floats like on the CPU). The first time, KITNAP prepares each model for the GPU and times one chunk on the GPU and one on the CPU, then remembers which is faster on that Mac (in `~/Library/Caches/KITNAP`, together with the prepared models), so the first track takes a minute or two longer. If the GPU can't be used, KITNAP quietly runs everything on the CPU instead.
- On the CPU (Windows and Linux, or a Mac without Core ML) the AI steps take a few minutes for a full song: on 4 cores about 1.8 times the song's length to separate, plus about 1.3 times to split; faster with more cores. They run on one thread per performance core (Apple Silicon's efficiency cores would only hold the others up), and need about 3 GB of memory while separating.

> Sounds pulled from commercial songs are fine for practice and personal use, but clear samples before releasing music with them.

## Getting the plugin

Download the installer for your system from the repository's **Releases** page:

- **Windows:** `KITNAP-x.y.z-Windows-Setup.exe` installs the VST3 plugin (into `C:\Program Files\Common Files\VST3`, where FL Studio, Ableton and other DAWs look) and the standalone app, plus the AI drum split model, and adds an uninstaller.
- **macOS:** `KITNAP-x.y.z-macOS.pkg` installs the VST3, the Audio Unit (Logic, GarageBand), the standalone app and the AI drum split model. Click *Customize* to choose.
- **Linux:** unpack `KITNAP-x.y.z-Linux.tar.gz` and run `./install.sh` (installs to `~/.vst3`, `~/.lv2`, `~/.local/bin` and `~/.config/KITNAP`).

Then rescan plugins in your DAW. In FL Studio: *Options → Manage plugins → Find installed plugins*, then add KITNAP from the Channel rack.

The installers aren't code-signed yet, so the first launch shows a warning:
- **Windows** (SmartScreen): click *More info → Run anyway*.
- **macOS:** if the installer is blocked, open *System Settings → Privacy & Security* and click *Open Anyway* (on older macOS, right-click the `.pkg` and choose *Open*).

Every push builds the Mac installer and plugins (Windows and Linux are switched off in `.github/workflows/build.yml` to save GitHub Actions minutes; uncomment them there to build those installers too); they're under *Artifacts* on each run in the **Actions** tab (GitHub login required, kept for 90 days). Pushing a version tag (`git tag v0.2.0 && git push origin v0.2.0`) publishes a Release with the three installers attached.

### The AI files

ONNX Runtime ships inside the plugin (`Contents/Resources` in the bundle, or `Resources/` next to the binary). The models (`kitnap_drums_sw.onnx`, 210 MB, and `kitnap_drumsep5.onnx`, 440 MB) are installed once in a shared folder: `C:\ProgramData\KITNAP` on Windows, `/Library/Application Support/KITNAP` on macOS, `~/.config/KITNAP` on Linux. KITNAP loads the runtime only when needed and never links against it. The separation model is exported with its attention, normalisation and rotary embeddings written out as plain matrix and element-wise operations (the same math), which runs about a third faster on the CPU and lets Core ML take on the whole model. Both models are needed to make a kit: if a file is missing or can't load (e.g. on a very old macOS), loading a track shows why, and kits saved in a project still open and play.

The models:
- **Separation:** BS-RoFormer SW (6 stems: bass, drums, other, vocals, guitar, piano; weights released by jarredou), downloaded from the [mixdown-models](https://github.com/mogonzal/mixdown-models/releases/tag/bs-roformer-sw-v1) mirror (checksum verified) and converted by `tools/export_sw.py`, drums only. Input and output `[1, 2, 1025, 345, 2]` (channel, bin, frame, real/imaginary; 2048-point FFT, hop 512).
- **Drum split:** DrumSep 5 stems v2025 (MDX23C) by jarredou. Its original release is gone; it's downloaded from a copy in the [DrumToMIDI](https://github.com/EverlastEngineering/DrumToMIDI) project (pinned by checksum, though there's no official one to compare with) and converted to ONNX by `tools/export_drumsep.py`. Input `[1, 4, 1024, 1024]` (2048-point FFT, hop 512), output `[1, 5, 4, 1024, 1024]`: kick, snare, toms, hi-hat, cymbals.

**Before selling KITNAP, sort out the model licences.** Neither BS-RoFormer SW's nor DrumSep's authors published licence terms for their weights, so treat them as non-commercial until they give permission. Alternatively, swap in models with a commercial licence.

## Building it yourself

You need CMake 3.22+ and a C++17 compiler (Xcode on macOS, Visual Studio 2022 on Windows). JUCE is downloaded automatically.

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release --parallel
```

The plugins end up in `build/KITNAP_artefacts/Release/`. The build downloads ONNX Runtime 1.23.2 and the models (checksums verified), copies the runtime into each plugin, and converts the models to ONNX, which needs Python with PyTorch (`pip install torch onnx onnxruntime pyyaml numpy safetensors BS-RoFormer==0.4.1`); without it the build continues without AI models. The converted models land in `build/kitnap-ai/`: copy them into the shared folder above to use them with a self-built plugin. Pass `-DKITNAP_WITH_AI=OFF` to build without any AI, or `-DKITNAP_WITH_DRUM_SPLIT=OFF` to skip converting the split model (then copy a converted one into the shared folder yourself).

### Tests

`KitnapAnalyzerTest` builds five synthetic drum parts (two different kicks, a snare, a tom fill, hats and a crash, with a faint copy of the snare in the kick part like the bleed the split leaves), extracts the kit, and checks that every hit is found at the right place and nothing else, that each drum ends up on its own pad (two kicks and two toms as variations), and that every sound is a hit of its part, unchanged apart from the fades. It also builds a 3-minute song with and without drums and checks the section picker only picks stretches with drums, from both of them.

```bash
./build/KitnapAnalyzerTest_artefacts/Release/KitnapAnalyzerTest            # run the test
./build/KitnapAnalyzerTest_artefacts/Release/KitnapAnalyzerTest kit-out    # ...and write the extracted kit as WAVs
./build/KitnapAnalyzerTest_artefacts/Release/KitnapAnalyzerTest --file-ai song.mp3 kit-out   # the plugin's pipeline
./build/KitnapAnalyzerTest_artefacts/Release/KitnapAnalyzerTest --file-ai-full song.mp3 kit-out   # the same on the whole song
./build/KitnapAnalyzerTest_artefacts/Release/KitnapAnalyzerTest --sections song.mp3          # which sections the AI would process
./build/KitnapAnalyzerTest_artefacts/Release/KitnapAnalyzerTest --separate song.mp3 drums.wav   # write the AI drum stem
./build/KitnapAnalyzerTest_artefacts/Release/KitnapAnalyzerTest --split drums.wav parts      # write its five parts (kick, snare, toms, hi-hat, cymbals)
./build/KitnapAnalyzerTest_artefacts/Release/KitnapAnalyzerTest --file-parts parts kit-out   # only the last step, from those parts
./build/KitnapAnalyzerTest_artefacts/Release/KitnapAnalyzerTest --hits parts                 # list the hits found in each part
./build/KitnapAnalyzerTest_artefacts/Release/KitnapAnalyzerTest --write-mix beat.wav         # the synthetic drums with bass and chords, for trying the AI
./build/KitnapAnalyzerTest_artefacts/Release/KitnapAnalyzerTest --compare a.wav b.wav -40     # how far apart two stems are (fails above -40 dB)
```

On a Mac, `KITNAP_CPU_ONLY=1` runs the models on the CPU, `KITNAP_FORCE_GPU=1` on the GPU (without timing which is faster), and `KITNAP_ORT_LOG=1` shows ONNX Runtime's details (such as how much of each model Core ML takes on). CI separates and splits a few seconds of the synthetic beat both ways and checks the results match (GitHub's Mac runners have no real GPU, so there Core ML is slow and KITNAP picks the CPU by itself).

On Linux, `-DKITNAP_BUILD_SCREENSHOT=ON` also builds `KitnapScreenshot`, which renders the UI to PNGs (run it under `xvfb-run`).

## Project layout

```
Source/
  analysis/DrumAnalyzer.*   finds the hits in each part, groups them, cuts out the sounds
  separation/DrumSeparator.* AI drum separation and drum split (ONNX Runtime, loaded at run time)
  engine/KitSampler.*       the 5-pad sampler (MIDI, voice handling)
  ui/                       waveform/drop zone, pads, the FortyVolts theme (fonts, colours, logo), the film look
  PluginProcessor.*         loading, background analysis, saving with the project, export
  PluginEditor.*            the window
Resources/                  fonts (Michroma, IBM Plex Sans; SIL Open Font License) and the FortyVolts logo
tests/                      analyzer test and screenshot tool
installer/                  Windows (Inno Setup), macOS (pkg) and Linux installers
cmake/KitnapAI.cmake        downloads ONNX Runtime + models, converts them, bundles the runtime
tools/export_sw.py          converts the BS-RoFormer SW checkpoint to ONNX
tools/export_drumsep.py     converts the DrumSep checkpoint to ONNX
```

## License note

KITNAP is built with [JUCE](https://juce.com). JUCE is dual-licensed: AGPLv3, or a commercial JUCE licence (including a free tier for small developers). Check the current terms at juce.com before selling KITNAP or releasing it closed-source.
