#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include "PluginProcessor.h"

// The big card on the left: the song as a circular waveform. A drop zone when empty, a ring that
// fills up while analysing, and when the kit is ready its stripes light up one after another, the
// loudest ones (the hits) brightest. Also shows the volume label above the slider the editor places
// at the bottom.
class WaveformView : public juce::Component
{
public:
    explicit WaveformView (KitnapAudioProcessor& p);

    void paint (juce::Graphics&) override;
    void mouseUp (const juce::MouseEvent&) override;

    // Called by the editor every frame.
    void tick (double seconds);
    void kitChanged();
    void setDragHover (bool hovering) { dragHover = hovering; repaint(); }

    // Where the editor puts the volume slider, in this component's coordinates.
    static juce::Rectangle<int> volumeSliderArea() { return { 24, 492, 372, 24 }; }

    std::function<void()> onBrowse;

private:
    struct Stripe { float level = 0.0f; bool loud = false, soft = false; };

    // The animated part, on its own opaque layer so a frame only redraws the ring.
    struct Ring : public juce::Component
    {
        explicit Ring (WaveformView& o) : owner (o) { setOpaque (true); setInterceptsMouseClicks (false, false); }
        void paint (juce::Graphics&) override;
        WaveformView& owner;
        juce::Image restingRing;
        float restingScale = 0.0f;
    };

    void paintRing (juce::Graphics&, juce::Point<float> centre, bool resting);
    void paintCentre (juce::Graphics&, juce::Point<float> centre);
    void paintVolume (juce::Graphics&);

    KitnapAudioProcessor& processor;
    std::vector<Stripe> stripes;
    double time = 0.0;
    float shownProgress = 0.0f;
    bool dragHover = false;
    Ring ring { *this };
    KitState::Status lastStatus = KitState::Status::empty;
};
