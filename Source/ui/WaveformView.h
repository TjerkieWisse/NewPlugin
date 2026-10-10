#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include "PluginProcessor.h"

// The big strip at the top: a drop zone when empty, a scanning animation while analysing,
// and the song's waveform with every detected hit lighting up in its drum colour when done.
class WaveformView : public juce::Component
{
public:
    explicit WaveformView (KitnapAudioProcessor& p) : processor (p) {}

    void paint (juce::Graphics&) override;
    void mouseUp (const juce::MouseEvent&) override;

    // Called by the editor every frame.
    void tick (double seconds);
    void kitChanged();
    void setDragHover (bool hovering) { dragHover = hovering; }

    std::function<void()> onBrowse;

private:
    void paintEmpty (juce::Graphics&, juce::Rectangle<float> area);
    void paintWaveform (juce::Graphics&, juce::Rectangle<float> area, float brightness);
    void paintHits (juce::Graphics&, juce::Rectangle<float> area);

    KitnapAudioProcessor& processor;
    double time = 0.0, revealStart = -10.0;
    float shownProgress = 0.0f;
    bool dragHover = false;
    KitState::Status lastStatus = KitState::Status::empty;
};
