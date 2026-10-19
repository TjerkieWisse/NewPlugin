#pragma once

#include <juce_audio_basics/juce_audio_basics.h>
#include <array>
#include <functional>
#include <vector>

namespace kitnap
{
// The five pads: one per part of the AI drum split, in the same order as DrumSeparator::DrumPart.
enum class DrumClass { kick, snare, toms, hihat, cymbals };
constexpr int numDrumClasses = 5;

inline const char* drumClassName (DrumClass c)
{
    static constexpr const char* names[] { "KICK", "SNARE", "TOMS", "HI-HAT", "CYMBALS" };
    return names[(int) c];
}

// One drum sound: a single hit cut straight out of its part, as it sounds in the song.
struct DrumSample
{
    juce::AudioBuffer<float> audio;
    double sampleRate = 44100.0;
    int hitsStacked = 1; // how many hits of the song sound like this one
    std::vector<int> sourceHits; // indices into AnalysisResult::hits of every hit that sounds like this one
};

// A detected hit in the song (used for the waveform markers).
struct DetectedHit
{
    int position = 0; // sample index in the source
    DrumClass drumClass = DrumClass::kick;
    float strength = 0.0f;
};

struct AnalysisResult
{
    std::vector<DetectedHit> hits;
    // For each pad: the different sounds found in its part, the most frequent first.
    std::array<std::vector<DrumSample>, numDrumClasses> kit;
};

// Builds the kit from the five parts of the AI drum split (kick, snare, toms, hi-hat, cymbals):
// every pad comes from its own part and nothing else.
//   1. find the hits in the part (spectral flux), ignoring leftovers far below its real hits
//   2. group hits that sound alike; each group is one variation on the pad
//   3. per group, pick the cleanest single hit: loud, with room to ring out before the next hit,
//      and little sound from the previous hit underneath it
//   4. cut it out as it is, with only a 2 ms fade-in and a 5 ms fade-out so it doesn't click
// No averaging, no filters, no generated tails and no level change.
class DrumAnalyzer
{
public:
    // parts: numDrumClasses stereo buffers, indexed by DrumClass. progress receives values 0..1;
    // shouldCancel is polled regularly. With regions (the song sections joined into the parts, see
    // SongSections), only hits inside them count, and no sound runs past its region's end.
    static AnalysisResult extract (const std::vector<juce::AudioBuffer<float>>& parts, double sampleRate,
                                   const std::function<void (float)>& progress = {},
                                   const std::function<bool()>& shouldCancel = {},
                                   const std::vector<juce::Range<int>>& regions = {});

    static constexpr int maxVariationsPerClass = 8;
};
} // namespace kitnap
