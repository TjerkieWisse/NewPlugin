#pragma once

#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_core/juce_core.h>
#include <functional>
#include <vector>

namespace kitnap
{
// AI drum separation: pulls the drum stem out of a full mix with a BS-RoFormer ONNX model,
// so the analyzer sees the drums without the music around them.
//
// ONNX Runtime and the models are looked up next to the plugin binary (in "Resources" or
// "../Resources", which is Contents/Resources inside a plugin bundle), then in the shared
// "KITNAP" folder the installer fills (C:\ProgramData, /Library/Application Support, ~/.config).
// When the runtime or the separation model is missing, isAvailable() is false and KITNAP falls
// back to analysing the full mix; without the split model, canSplit() is false and the kit is
// built from the whole drum stem.
class DrumSeparator
{
public:
    static constexpr double modelSampleRate = 44100.0;

    static bool isAvailable();

    // Why isAvailable() is false, for showing to the user.
    static juce::String unavailableReason();

    // Returns the drum stem at modelSampleRate (stereo), or an empty buffer on failure or cancel.
    // progress receives 0..1. Runs on the calling thread and takes a few minutes for a full song.
    static juce::AudioBuffer<float> separate (const juce::AudioBuffer<float>& source, double sampleRate,
                                              const std::function<void (float)>& progress,
                                              const std::function<bool()>& shouldCancel,
                                              juce::String& error);

    // Phase 3: a second model splits the drum stem into its parts, in this order.
    enum class DrumPart { kick, snare, toms, hihat, ride, crash };
    static constexpr int numDrumParts = 6;

    // True when the drum split model ("kitnap_drumsep.onnx") is installed too.
    static bool canSplit();

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
