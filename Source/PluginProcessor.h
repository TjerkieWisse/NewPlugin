#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_audio_formats/juce_audio_formats.h>
#include "analysis/DrumAnalyzer.h"
#include "engine/KitSampler.h"

// Everything the editor shows about the current kit. Only touched on the message thread.
struct KitState
{
    enum class Status { empty, analysing, ready, error };

    Status status = Status::empty;
    juce::String sourceName, message;
    juce::File sourceFile;
    double sourceLengthSeconds = 0.0;

    std::vector<kitnap::DetectedHit> hits;
    double hitSampleRate = 44100.0;
    std::vector<float> overview; // min/max pairs, for drawing the waveform

    std::array<std::vector<kitnap::SamplePtr>, kitnap::numDrumClasses> variations;
    std::array<int, kitnap::numDrumClasses> selected {};
};

class KitnapAudioProcessor : public juce::AudioProcessor,
                             public juce::ChangeBroadcaster,
                             private juce::Timer
{
public:
    KitnapAudioProcessor();
    ~KitnapAudioProcessor() override;

    void prepareToPlay (double sampleRate, int samplesPerBlock) override;
    void releaseResources() override {}
    bool isBusesLayoutSupported (const BusesLayout& layouts) const override;
    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;
    using AudioProcessor::processBlock;

    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override { return true; }

    const juce::String getName() const override { return JucePlugin_Name; }
    bool acceptsMidi() const override { return true; }
    bool producesMidi() const override { return false; }
    bool isMidiEffect() const override { return false; }
    double getTailLengthSeconds() const override { return 1.0; }

    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram (int) override {}
    const juce::String getProgramName (int) override { return {}; }
    void changeProgramName (int, const juce::String&) override {}

    void getStateInformation (juce::MemoryBlock& destData) override;
    void setStateInformation (const void* data, int sizeInBytes) override;

    //==============================================================================
    // Message thread API used by the editor.
    void loadFile (const juce::File& file);
    bool canLoad (const juce::File& file) const;
    const KitState& getKit() const { return kit; }
    float getProgress() const { return progress.load(); }

    void selectVariation (int pad, int index);
    void triggerPad (int pad) { sampler.triggerFromUi (pad); }
    float getPadActivity (int pad) const { return sampler.getPadActivity (pad); }
    void decayPadActivity (float factor) { sampler.decayActivity (factor); }

    // Writes one pad, or the whole kit, as 24-bit WAV files. Returns the written file(s).
    juce::File writePadToFile (int pad, const juce::File& folder) const;
    juce::Array<juce::File> exportKit (const juce::File& parentFolder) const;
    juce::String padFileName (int pad) const;

    juce::AudioProcessorValueTreeState apvts;

private:
    class AnalysisJob;
    struct AnalysisOutput;

    static juce::AudioProcessorValueTreeState::ParameterLayout createLayout();
    void timerCallback() override;
    void applyAnalysis (std::unique_ptr<AnalysisOutput> output);
    void startAnalysis (const juce::File& file, std::array<int, kitnap::numDrumClasses> selection, bool keepCurrentPads);
    void applySelectionToSampler();
    void retire (const kitnap::SamplePtr& sample);

    juce::AudioFormatManager formatManager;
    kitnap::KitSampler sampler;
    KitState kit;
    std::unique_ptr<AnalysisJob> job;
    std::atomic<float> progress { 0.0f };

    // Samples that a voice might still be playing; released once nobody else holds them.
    std::vector<kitnap::SamplePtr> retired;
    std::atomic<float>* volumeParam = nullptr;
    juce::LinearSmoothedValue<float> volume;

    JUCE_DECLARE_WEAK_REFERENCEABLE (KitnapAudioProcessor)
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (KitnapAudioProcessor)
};
