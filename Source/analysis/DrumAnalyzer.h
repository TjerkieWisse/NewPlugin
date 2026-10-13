#pragma once

#include <juce_audio_basics/juce_audio_basics.h>
#include <array>
#include <atomic>
#include <functional>
#include <vector>

namespace kitnap
{
enum class DrumClass { kick, snare, clap, hat, openHat, perc };
constexpr int numDrumClasses = 6;

inline const char* drumClassName (DrumClass c)
{
    static constexpr const char* names[] { "KICK", "SNARE", "CLAP", "HAT", "OPEN HAT", "PERC" };
    return names[(int) c];
}

// One extracted, cleaned drum sound.
struct DrumSample
{
    juce::AudioBuffer<float> audio;
    double sampleRate = 44100.0;
    int hitsStacked = 1; // how many hits of the song were averaged into this sound
    std::vector<int> sourceHits; // indices into AnalysisResult::hits of every hit in this group
    bool fromLayer = false; // reconstructed from a drum ringing on top of another (no clean hits of it)
};

// A detected hit in the source audio (used for the waveform markers).
struct DetectedHit
{
    int position = 0; // sample index in the source
    DrumClass drumClass = DrumClass::perc;
    int secondaryClass = -1; // another drum class heard in the same hit, or -1
    float strength = 0.0f;
};

struct AnalysisResult
{
    std::vector<DetectedHit> hits;
    // For each drum class: the variations found, best first.
    std::array<std::vector<DrumSample>, numDrumClasses> kit;
};

// Offline analysis that turns a drum loop or sparse beat into a cleaned-up drum kit:
//   1. spectral-flux onset detection
//   2. per-hit features (band energies, noisiness, decay, clap flams) -> drum class
//   3. hits of a class are grouped by waveform similarity
//   4. each group is aligned and averaged ("stacked"), cancelling the rest of the mix
//   5. class-specific cleanup filters, trimming, fades and normalisation
class DrumAnalyzer
{
public:
    // progress receives values 0..1; shouldCancel is polled regularly.
    static AnalysisResult analyse (const juce::AudioBuffer<float>& source, double sampleRate,
                                   const std::function<void (float)>& progress = {},
                                   const std::function<bool()>& shouldCancel = {});

    // Builds the kit from a drum stem that the AI has also split into kick, snare and hi-hat stems.
    // The whole drum stem decides what the drums are (classes, variations, hits); every sound is
    // then swapped for the same drum taken from a split stem when one clearly matches it, which is
    // the same sound without what was layered on it. The split only fills in what the drum stem
    // lacks (e.g. an open hat that only ever rang on top of a clap), since on electronic or heavily
    // processed drums it can put a drum in the wrong stem.
    static AnalysisResult analyseParts (const juce::AudioBuffer<float>& drums, const juce::AudioBuffer<float>& kick,
                                        const juce::AudioBuffer<float>& snare, const juce::AudioBuffer<float>& hihat,
                                        double sampleRate,
                                        const std::function<void (float)>& progress = {},
                                        const std::function<bool()>& shouldCancel = {});

    static constexpr int maxVariationsPerClass = 8;
};
} // namespace kitnap
