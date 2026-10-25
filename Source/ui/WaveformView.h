#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include "PluginProcessor.h"

// The wide card at the top: the song's name and details, how many hits were found, and the song's
// waveform filled with the flowing aurora, with a soft glow under it.
// Empty, it's the drop zone; while analysing, the aurora fills the waveform up to the progress.
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
    juce::Rectangle<float> waveArea() const;
    juce::Rectangle<float> progressArea() const;
    void paintWave (juce::Graphics&);
    void paintRuler (juce::Graphics&);
    void buildMasks (float scale);
    void composeFrame (float scale, float litRight, float glowStrength);

    KitnapAudioProcessor& processor;
    juce::Path shape;                  // the waveform's outline, in this component's coordinates
    juce::Image mask, glowMask;        // the outline as an alpha mask, sharp and blurred
    juce::Image frame;                 // both in colour, redrawn every frame
    std::vector<juce::Colour> columnColours;
    juce::Rectangle<float> glowArea;   // where the masks and the frame go
    float maskScale = 0.0f;
    double time = 0.0;
    float shownProgress = 0.0f;
    bool dragHover = false;
    KitState::Status lastStatus = KitState::Status::empty;
};
