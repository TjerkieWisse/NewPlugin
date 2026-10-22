#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

// The "film" look: everything the editor draws is graded like film stock (a faint red/cyan
// colour fringe, warm whites, slightly navy blacks), and an animated layer on top adds grain,
// flicker, dust, scratches, a gentle gate weave and viewfinder details.

// Animated layer drawn over the whole editor. Ignores the mouse.
class FilmOverlay : public juce::Component,
                    private juce::Timer
{
public:
    FilmOverlay();

    void setOn (bool shouldBeOn);
    bool isOn() const { return on; }

    // Gate weave: how far the picture is shifted this frame, in physical pixels.
    juce::Point<int> weaveOffset (float scale) const;

    void paint (juce::Graphics&) override;

private:
    struct Scratch { float x, home; int framesLeft; };
    struct Dust { juce::Path shape; bool hair, light; };

    void timerCallback() override;
    const juce::Image& grainTile (float scale);
    void paintDetails (juce::Graphics&);

    bool on = false;
    juce::Random random;
    juce::int64 ticks = 0, frame = 0;
    float flicker = 0.0f;
    juce::Point<float> weave, grainOffset;
    int grainFrame = 0;
    std::vector<juce::Image> tiles;
    float tileScale = 0.0f;
    std::vector<Scratch> scratches;
    std::vector<Dust> dust;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (FilmOverlay)
};

// Caches what a component draws (like setBufferedToImage) and, while the film look is on,
// grades the cached picture. Only the parts that changed are drawn and graded again, so the
// overlay's animation doesn't make the editor redraw everything.
class FilmCache : public juce::CachedComponentImage
{
public:
    FilmCache (juce::Component& component, const FilmOverlay& filmOverlay) : owner (component), film (filmOverlay) {}

    void paint (juce::Graphics&) override;
    bool invalidateAll() override { valid.clear(); return true; }
    bool invalidate (const juce::Rectangle<int>& area) override { valid.subtract (area); return true; }
    void releaseResources() override { raw = graded = {}; }

private:
    void grade (juce::Rectangle<int> physicalArea);

    juce::Component& owner;
    const FilmOverlay& film;
    juce::Image raw, graded;
    juce::RectangleList<int> valid;
    bool gradedAll = false;
    float scale = 1.0f;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (FilmCache)
};
