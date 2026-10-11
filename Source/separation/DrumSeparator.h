#pragma once

#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_core/juce_core.h>
#include <functional>

namespace kitnap
{
// AI drum separation: pulls the drum stem out of a full mix with an MDX-Net style ONNX model,
// so the analyzer sees the drums without the music around them.
//
// ONNX Runtime and the model are looked up next to the plugin binary (in "Resources" or
// "../Resources", which is Contents/Resources inside a plugin bundle) or in the user's
// application data folder under "KITNAP". When either is missing, isAvailable() is false
// and KITNAP falls back to analysing the full mix.
class DrumSeparator
{
public:
    static constexpr double modelSampleRate = 44100.0;

    static bool isAvailable();

    // Why isAvailable() is false, for showing to the user.
    static juce::String unavailableReason();

    // Returns the drum stem at modelSampleRate (stereo), or an empty buffer on failure or cancel.
    // progress receives 0..1. Runs on the calling thread and can take a minute for a full song.
    static juce::AudioBuffer<float> separate (const juce::AudioBuffer<float>& source, double sampleRate,
                                              const std::function<void (float)>& progress,
                                              const std::function<bool()>& shouldCancel,
                                              juce::String& error);
};
} // namespace kitnap
