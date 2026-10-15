# KITNAP

**Steal the drum kit from any track.**

![KITNAP](docs/screenshot.png)

Drop a song or drum loop into KITNAP and it uses AI to pull the drums out of the mix and split them into kick, snare and hi-hat, finds every drum hit, sorts them into kick, snare, clap, hat, open hat and perc, and turns each one into a clean, playable sample. Play the kit with MIDI, flip through variations, drag any sound straight into your DAW, or export the whole kit as WAVs.

Runs as VST3 (any DAW), AU (Logic, GarageBand) and as a standalone app.

## Using it

1. Put KITNAP on an instrument track.
2. Drop an audio file on the big strip at the top, or click it to browse. WAV, AIFF, FLAC, MP3 and OGG all work.
3. Watch the drums get kidnapped. First the AI separates the drums from the rest of the song (violet scanner), then splits them into kick, snare and hi-hat (cyan scanner), then every hit lights up in its drum's colour along the waveform. A full song takes a few minutes; the kit is saved with your project, so this only happens once.
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

The **AI** switch turns the AI steps on or off. Switching it re-runs the current track, so you can compare both versions. With it off, KITNAP analyses the full mix directly: much faster, and fine for drum loops and stems.

The whole kit (every variation, the hit map and the waveform) is saved inside your project, so reopening a session is instant and still works if you move or delete the source file.

## How it works

0. **Separate the drums (AI).** BS-RoFormer SW, a band-split transformer model, run on the CPU with ONNX Runtime, removes everything that isn't drums. The song is processed in 4-second chunks that are cross-faded over half a second. (The model was trained on 13-second chunks, but its memory use grows with the square of the chunk length: over 14 GB at 13 s, about 3 GB at 4 s, with a result within −22 dB of the 13 s one.)
0. **Split the kit (AI).** A second model (DrumSep 5 stems, MDX23C) splits the drum stem into kick, snare, toms, hi-hat and cymbals, in 12-second chunks that overlap by half and are cross-faded. Every step below runs on the whole drum stem and on each part. The whole drum stem decides what the drums are (which pads, which variations); each sound is then swapped for the same drum from a part when the two clearly match (their first 120 ms correlate at 0.8 or more), which is that sound without whatever was layered on it. So a hat ringing on top of a clap, or a hat on every kick, no longer ends up in the other drum. The parts only fill in what the drum stem lacks, such as an open hat that only ever rings on top of a clap. Going by the drum stem first matters for electronic and heavily processed drums, where the split model, trained mostly on acoustic kits, can put a drum in the wrong part.
1. **Find the hits.** Spectral-flux onset detection, then each hit is pinned to the sample by looking for the sharpest energy rise in three frequency bands. That way a quiet hat over a loud bass line is still placed exactly.
2. **Name them.** For every hit it measures the energy the hit *adds* compared with just before it (so bass and chords ringing underneath are ignored): low end, mids, top, noisiness, how long the top end rings, and whether there are several quick bursts (a clap). A kick and a hat landing together counts as both.
3. **Group them.** Hits of the same drum are compared waveform-to-waveform and grouped, so a song with two different snares gives you two variations.
4. **Peel off layers.** Hits where two drums land together (snare + open hat, kick + hat) are detected; each sound is preferably built only from hits where its drum plays alone. If a drum only ever occurs layered, the other drum's clean copy is checked against it by its spectral "fingerprint" (the exact pattern of peaks and dips of that sample) and, if present, its waveform is lined up to the sample and subtracted.
5. **Stack them.** Every hit in a group is aligned to the sample and averaged. The drum is identical every time, but the music around it isn't, so averaging cancels out much of the rest of the song. This is where the clean sound comes from: the more often a drum repeats, the cleaner it gets.
6. **Let it ring.** In a busy groove the next drum usually comes within 1/16 or 1/8, which used to chop sounds short. Now: drums that follow at the same distance every time are subtracted from the tail when they cancel cleanly; the real tail is grafted on from hits of the same drum that have room to ring out (e.g. before a break); and noisy drums (snare, clap, hats) that still had to be cut are continued with noise shaped like their own last moments, decaying at their own rate.
7. **Clean up.** Each drum type gets its own filters (the kick's tail is low-passed to remove hats, hats are high-passed to remove the kick and bass), then the sound is trimmed where it dies away or where a new hit starts (judged above 300 Hz, so a deep kick's wobble isn't mistaken for one), faded and normalised.

### What to expect

- **Full songs** come out clean thanks to the AI separation, even sample-based tracks where the music loops behind every drum hit.
- **Drum loops, drum stems and beats with clear drums** come out very clean with the AI on or off.
- The AI steps take several minutes for a full song on a laptop CPU (on 4 cores roughly 2 to 3 times the song's length to separate, plus about 2 times to split; faster with more cores), and need about 3 GB of memory while separating.
- **Layered drums** (a snare with an open hat on top, a kick with a hat) are mostly separated by the AI split. On top of that (and without the split model): KITNAP builds each sound from the hits where that drum plays alone, and if a drum is *always* layered, it subtracts a clean copy of the other drum (taken from elsewhere in the track) sample-accurately. When the other drum never plays alone (e.g. an open hat that only ever lands on the clap), KITNAP models the top end as two decays: the clap's own, which fades with its body, and the slower ring of the hat. Once the clap's body has died away it fits the hat's ring, extends it back to the start and removes it from under the clap. Past that point the hat is all that's left, so the sound ends there and the clap's own tail is continued with noise shaped like the clap itself. That way the sizzle that kept going after the clap is gone. The hat that was taken out isn't thrown away: if the track has no open hat of its own, it becomes the open hat pad. The very start of the hat stays, since it lands on (and is masked by) the clap's attack. Sounds that are layered by design (a snare and clap stacked into one sample) stay as you hear them.

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

ONNX Runtime ships inside the plugin (`Contents/Resources` in the bundle, or `Resources/` next to the binary). The models (`kitnap_drums_sw.onnx`, 210 MB, and `kitnap_drumsep5.onnx`, 440 MB) are installed once in a shared folder: `C:\ProgramData\KITNAP` on Windows, `/Library/Application Support/KITNAP` on macOS, `~/.config/KITNAP` on Linux. KITNAP loads the runtime only when needed and never links against it, so if the files are missing or can't load (e.g. on a very old macOS), the AI switch shows **N/A** and everything else keeps working. Without the split model, the AI only separates the drums from the song and the kit is built from the whole drum stem.

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

The plugins end up in `build/KITNAP_artefacts/Release/`. The build downloads ONNX Runtime 1.20.1 and the models (checksums verified), copies the runtime into each plugin, and converts the models to ONNX, which needs Python with PyTorch (`pip install torch onnx onnxruntime pyyaml numpy safetensors BS-RoFormer==0.4.1`); without it the build continues without AI models. The converted models land in `build/kitnap-ai/`: copy them into the shared folder above to use them with a self-built plugin. Pass `-DKITNAP_WITH_AI=OFF` to build without any AI, or `-DKITNAP_WITH_DRUM_SPLIT=OFF` to skip only the split.

### Tests

`KitnapAnalyzerTest` builds a synthetic beat (kick, snare, clap, hats, open hat) with a bass line and chords on top, runs the analyzer, and checks that every drum is detected, classified and extracted cleanly (each extracted sound must match the original with a correlation of 0.8 or higher; currently 0.97–1.0). `--layered` runs the same with every snare layered with an open hat and the kicks layered with hats; there the other drum must be at least 20 dB down in every extracted sound (currently 30–49 dB). `--layered-always` makes the open hat play only together with the snare (no clean copy exists), and checks the hat ringing on in the snare's tail.

```bash
./build/KitnapAnalyzerTest_artefacts/Release/KitnapAnalyzerTest            # run the test
./build/KitnapAnalyzerTest_artefacts/Release/KitnapAnalyzerTest kit-out    # ...and write the extracted kit as WAVs
./build/KitnapAnalyzerTest_artefacts/Release/KitnapAnalyzerTest --file song.wav kit-out   # analyse your own file
./build/KitnapAnalyzerTest_artefacts/Release/KitnapAnalyzerTest --separate song.mp3 drums.wav   # write the AI drum stem
./build/KitnapAnalyzerTest_artefacts/Release/KitnapAnalyzerTest --split drums.wav parts      # write its five parts (kick, snare, toms, hi-hat, cymbals)
./build/KitnapAnalyzerTest_artefacts/Release/KitnapAnalyzerTest --file-ai song.mp3 kit-out   # the plugin's full AI pipeline
./build/KitnapAnalyzerTest_artefacts/Release/KitnapAnalyzerTest --file-parts drums.wav parts kit-out   # only the last step
./build/KitnapAnalyzerTest_artefacts/Release/KitnapAnalyzerTest --hits drums.wav            # list the detected hits and their drums
```

On Linux, `-DKITNAP_BUILD_SCREENSHOT=ON` also builds `KitnapScreenshot`, which renders the UI to PNGs (run it under `xvfb-run`).

## Project layout

```
Source/
  analysis/DrumAnalyzer.*   onset detection, classification, grouping, stacking, cleanup
  separation/DrumSeparator.* AI drum separation and drum split (ONNX Runtime, loaded at run time)
  engine/KitSampler.*       the 6-pad sampler (MIDI, choke, voice handling)
  ui/                       waveform/drop zone, pads, colours
  PluginProcessor.*         loading, background analysis, saving with the project, export
  PluginEditor.*            the window
tests/                      analyzer test and screenshot tool
installer/                  Windows (Inno Setup), macOS (pkg) and Linux installers
cmake/KitnapAI.cmake        downloads ONNX Runtime + models, converts the split model, bundles them
tools/export_drumsep.py     converts the DrumSep checkpoint to ONNX
```

## License note

KITNAP is built with [JUCE](https://juce.com). JUCE is dual-licensed: AGPLv3, or a commercial JUCE licence (including a free tier for small developers). Check the current terms at juce.com before selling KITNAP or releasing it closed-source.
