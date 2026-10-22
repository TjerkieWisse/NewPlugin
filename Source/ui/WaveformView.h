#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include "PluginProcessor.h"

// The big strip at the top: a drop zone when empty, a scanner sweeping through the song's waveform
// while analysing, and the waveform in black when the kit is ready.
class WaveformView : public juce::Component
{
public:
    explicit WaveformView (KitnapAudioProcessor& p) : processor (p) {}

    void paint (juce::Graphics&) override;
    void mouseUp (const juce::MouseEvent&) override;

    // Called by the editor every frame.
    void tick (double seconds);
    void kitChanged();
    void setDragHover (bool hovering) { dragHover = hovering; repaint(); }

    std::function<void()> onBrowse;

private:
    void paintEmpty (juce::Graphics&, juce::Rectangle<float> area);
    void paintWaveform (juce::Graphics&, juce::Rectangle<float> area, juce::Colour);

    KitnapAudioProcessor& processor;
    double time = 0.0;
    float shownProgress = 0.0f;
    bool dragHover = false;
    KitState::Status lastStatus = KitState::Status::empty;
};
