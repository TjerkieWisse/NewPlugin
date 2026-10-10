#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include <atomic>
#include "dsp/DreamFx.h"
#include "dsp/SpaceFx.h"
#include "dsp/TapeFx.h"
#include "dsp/GlitchFx.h"
#include "dsp/FreezeFx.h"

namespace ParamIDs
{
inline constexpr const char* x = "x";
inline constexpr const char* y = "y";
inline constexpr const char* mix = "mix";
inline constexpr const char* motion = "motion";
inline constexpr const char* speed = "speed";
inline constexpr const char* freeze = "freeze";
inline constexpr const char* output = "output";
}

class OrbitAudioProcessor : public juce::AudioProcessor
{
public:
    OrbitAudioProcessor();

    void prepareToPlay (double sampleRate, int samplesPerBlock) override;
    void releaseResources() override {}
    bool isBusesLayoutSupported (const BusesLayout& layouts) const override;
    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;
    using AudioProcessor::processBlock;

    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override { return true; }

    const juce::String getName() const override { return JucePlugin_Name; }
    bool acceptsMidi() const override { return false; }
    bool producesMidi() const override { return false; }
    bool isMidiEffect() const override { return false; }
    double getTailLengthSeconds() const override { return 8.0; }

    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram (int) override {}
    const juce::String getProgramName (int) override { return {}; }
    void changeProgramName (int, const juce::String&) override {}

    void getStateInformation (juce::MemoryBlock& destData) override;
    void setStateInformation (const void* data, int sizeInBytes) override;

    juce::AudioProcessorValueTreeState apvts;

    // Read by the editor for visuals; written once per block by the audio thread.
    std::atomic<float> orbX { 0.5f }, orbY { 0.5f }, level { 0.0f };
    std::atomic<bool> frozenActive { false };

private:
    static juce::AudioProcessorValueTreeState::ParameterLayout createLayout();

    orbit::DreamFx dream;
    orbit::SpaceFx space;
    orbit::TapeFx tape;
    orbit::GlitchFx glitch;
    orbit::FreezeFx freezer;

    juce::LinearSmoothedValue<float> smoothX, smoothY, smoothMix, smoothGain;
    double motionPhase = 0.0;
    float currentSampleRate = 44100.0f;

    std::atomic<float>* xParam = nullptr;
    std::atomic<float>* yParam = nullptr;
    std::atomic<float>* mixParam = nullptr;
    std::atomic<float>* motionParam = nullptr;
    std::atomic<float>* speedParam = nullptr;
    std::atomic<float>* freezeParam = nullptr;
    std::atomic<float>* outputParam = nullptr;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (OrbitAudioProcessor)
};
