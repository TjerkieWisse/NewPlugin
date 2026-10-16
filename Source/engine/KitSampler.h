#pragma once

#include <juce_audio_basics/juce_audio_basics.h>
#include "analysis/DrumAnalyzer.h"
#include <array>
#include <atomic>
#include <memory>

namespace kitnap
{
using SamplePtr = std::shared_ptr<const DrumSample>;

// Plays the extracted kit: one pad per drum class, triggered by MIDI or by clicking a pad.
// MIDI follows the General MIDI drum map, so any drum pattern or drum rack plays it straight away.
class KitSampler
{
public:
    static constexpr int numPads = numDrumClasses;
    static int noteForPad (int pad);  // the main note of each pad
    static int padForNote (int note); // -1 when no pad plays this note

    void prepare (double sampleRate);

    // Message thread. The caller must keep the previous sample alive until isInUse() is false.
    void setPadSample (int pad, SamplePtr sample);
    SamplePtr getPadSample (int pad) const;

    // Thread safe: queues a hit for the next audio block (used when a pad is clicked).
    void triggerFromUi (int pad) { uiTriggers.fetch_or (1 << pad); }

    // Audio thread.
    void render (juce::AudioBuffer<float>& output, const juce::MidiBuffer& midi);

    // 0..1, decays after each hit; the editor uses it to flash the pads.
    float getPadActivity (int pad) const { return activity[(size_t) pad].load(); }
    void decayActivity (float factor);

private:
    struct Voice
    {
        SamplePtr sample;
        int pad = -1;
        double position = 0.0, increment = 1.0;
        float gain = 1.0f;
    };

    void startVoice (int pad, float velocity);
    void renderVoices (juce::AudioBuffer<float>& output, int start, int numSamples);

    double sampleRate = 44100.0;
    std::array<SamplePtr, numPads> pads;
    mutable juce::SpinLock padLock;
    std::array<Voice, 32> voices;
    std::array<std::atomic<float>, numPads> activity {};
    std::atomic<int> uiTriggers { 0 };
};
} // namespace kitnap
