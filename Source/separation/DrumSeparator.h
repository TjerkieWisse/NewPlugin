#pragma once

#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_core/juce_core.h>
#include <functional>
#include <vector>

namespace kitnap
{
// The two AI models KITNAP runs: BS-RoFormer SW pulls the drum stem out of a full mix, then the
// 5-stem drum split model splits that stem into kick, snare, toms, hi-hat and cymbals.
//
// ONNX Runtime and the models are looked up next to the plugin binary (in "Resources" or
// "../Resources", which is Contents/Resources inside a plugin bundle), then in the shared
// "KITNAP" folder the installer fills (C:\ProgramData, /Library/Application Support, ~/.config).
// Both models are needed: when the runtime or either model is missing, isAvailable() is false and
// KITNAP can't make a kit.
class DrumSeparator
{
public:
    static constexpr double modelSampleRate = 44100.0;

    static bool isAvailable();

    // Why isAvailable() is false, for showing to the user.
    static juce::String unavailableReason();

    // Where the last model run happened: "Core ML (GPU)" on a Mac, otherwise "CPU".
    static juce::String backend();

    // Returns the drum stem at modelSampleRate (stereo), or an empty buffer on failure or cancel.
    // progress receives 0..1. Runs on the calling thread and takes a few minutes for a full song.
    static juce::AudioBuffer<float> separate (const juce::AudioBuffer<float>& source, double sampleRate,
                                              const std::function<void (float)>& progress,
                                              const std::function<bool()>& shouldCancel,
                                              juce::String& error);

    // The second model splits the drum stem into its parts, in this order.
    enum class DrumPart { kick, snare, toms, hihat, cymbals };
    static constexpr int numDrumParts = 5;

    // Splits a drum stem (stereo, at modelSampleRate) into its parts. Returns numDrumParts buffers,
    // indexed by DrumPart, where only the wanted ones are filled; an empty vector on failure or cancel.
    // Takes about as long as the song itself on a laptop.
    static std::vector<juce::AudioBuffer<float>> split (const juce::AudioBuffer<float>& drums,
                                                        const std::vector<DrumPart>& wanted,
                                                        const std::function<void (float)>& progress,
                                                        const std::function<bool()>& shouldCancel,
                                                        juce::String& error);
};
} // namespace kitnap
