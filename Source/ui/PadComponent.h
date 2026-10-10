#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include "engine/KitSampler.h"

// One drum pad: click to play, arrows to flip through variations, drag it out to get the WAV.
class PadComponent : public juce::Component
{
public:
    explicit PadComponent (int padIndex);

    void setSound (kitnap::SamplePtr sample, int variation, int variationCount);
    void setActivity (float level);

    void paint (juce::Graphics&) override;
    void mouseDown (const juce::MouseEvent&) override;
    void mouseDrag (const juce::MouseEvent&) override;
    void mouseMove (const juce::MouseEvent&) override;
    void mouseExit (const juce::MouseEvent&) override;

    std::function<void()> onPlay;
    std::function<void (int delta)> onVariation;
    std::function<void()> onDragOut;

private:
    juce::Rectangle<float> previousArea() const;
    juce::Rectangle<float> nextArea() const;
    void rebuildWaveform();

    int pad;
    juce::Colour colour;
    kitnap::SamplePtr sample;
    int variation = 0, variationCount = 0;
    float activity = 0.0f;
    bool hover = false, dragStarted = false;
    juce::Path waveform;
};
