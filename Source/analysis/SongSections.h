#pragma once

#include <juce_audio_basics/juce_audio_basics.h>
#include <vector>

namespace kitnap
{
// The parts of a song where the drums are, so the AI only has to process those instead of the whole
// song. A kit needs one clean hit of each sound, and drum sounds repeat throughout a song.
//
// The song is divided into maxSections stretches; from each, the sectionSeconds with the most drum
// hits (the most sudden new energy, measured by spectral flux, which also follows how loud it is)
// are taken. Spreading them over the song lets the verse, chorus and bridge each bring their own
// sounds. A stretch with far fewer hits than the busiest one (an intro without drums, say) is
// skipped. Songs shorter than maxSections * sectionSeconds * 1.25 are used whole.
struct SongSections
{
    static constexpr double sectionSeconds = 12.0; // one chunk of the drum split model
    static constexpr int maxSections = 5;
    static constexpr double gapSeconds = 0.5;      // silence between sections when they're joined

    std::vector<juce::Range<int>> sections; // in samples of the song, in time order
    double sampleRate = 44100.0;

    static SongSections pick (const juce::AudioBuffer<float>& song, double sampleRate);

    // The sections joined into one buffer, gapSeconds of silence between them, with 10 ms fades at
    // each section's edges so the cuts don't sound like hits.
    juce::AudioBuffer<float> join (const juce::AudioBuffer<float>& song) const;

    // Where each section lies in the joined audio once it's at `rate` (the AI works at 44.1 kHz).
    std::vector<juce::Range<int>> joinedRanges (double rate) const;

    // A position in the joined audio at `rate` -> the same moment in the song, also at `rate`.
    int toSong (int joinedPosition, double rate) const;

    // The sections in seconds, for the waveform.
    std::vector<juce::Range<double>> seconds() const;
};
} // namespace kitnap
