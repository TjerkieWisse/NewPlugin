#include "PadComponent.h"
#include "Theme.h"

using namespace kitnap;

PadComponent::PadComponent (int padIndex)
    : pad (padIndex)
{
    setMouseCursor (juce::MouseCursor::PointingHandCursor);
}

void PadComponent::setSound (SamplePtr newSample, int newVariation, int newCount)
{
    const bool changed = newSample != sample;
    sample = std::move (newSample);
    variation = newVariation;
    variationCount = newCount;
    if (changed)
        rebuildWaveform();
    repaint();
}

void PadComponent::setActivity (float level)
{
    if (std::abs (level - activity) > 0.002f)
    {
        activity = level;
        repaint();
    }
}

// The "‹ 1/3 ›" picker at the bottom of the tile.
juce::String PadComponent::pickerText() const
{
    return juce::String (variation + 1) + "/" + juce::String (variationCount);
}

juce::Rectangle<float> PadComponent::previousArea() const
{
    return { 10.0f, 244.0f, 22.0f, 28.0f };
}

juce::Rectangle<float> PadComponent::nextArea() const
{
    const float text = juce::GlyphArrangement::getStringWidth (theme::mono (12.0f), pickerText());
    return { 38.0f + text, 244.0f, 22.0f, 28.0f };
}

void PadComponent::rebuildWaveform()
{
    waveform.clear();
    if (sample == nullptr || sample->audio.getNumSamples() == 0)
        return;

    // Normalised 0..1 shape; scaled to the pad when painting.
    const auto& audio = sample->audio;
    const int points = 120;
    const int n = audio.getNumSamples();
    std::vector<float> peaks ((size_t) points, 0.0f);
    for (int p = 0; p < points; ++p)
    {
        const int start = p * n / points, end = std::max (start + 1, (p + 1) * n / points);
        for (int ch = 0; ch < audio.getNumChannels(); ++ch)
            peaks[(size_t) p] = std::max (peaks[(size_t) p], audio.getMagnitude (ch, start, end - start));
    }

    waveform.startNewSubPath (0.0f, 0.5f);
    for (int p = 0; p < points; ++p)
        waveform.lineTo ((float) p / (points - 1), 0.5f - 0.5f * peaks[(size_t) p]);
    for (int p = points - 1; p >= 0; --p)
        waveform.lineTo ((float) p / (points - 1), 0.5f + 0.5f * peaks[(size_t) p]);
    waveform.closeSubPath();
}

void PadComponent::paint (juce::Graphics& g)
{
    const auto bounds = getLocalBounds().toFloat();
    const bool hasSound = sample != nullptr;
    const float live = hasSound ? juce::jlimit (0.0f, 1.0f, activity * 1.5f) : 0.0f;
    const double flow = juce::Time::getMillisecondCounterHiRes() / 1000.0 / theme::auroraSeconds;

    // White tile; while its sound plays the border turns into the flowing aurora.
    g.setColour (theme::card);
    g.fillRoundedRectangle (bounds, theme::tileRadius);
    g.setColour (hover && hasSound ? juce::Colour (0xffd9d9d9) : theme::border);
    g.drawRoundedRectangle (bounds.reduced (0.5f), theme::tileRadius, 1.0f);
    if (live > 0.0f)
    {
        auto aurora = theme::auroraGradient (0.0f, bounds.getWidth(), 0.0f, bounds.getWidth() / 2.0f, flow);
        g.setGradientFill (aurora);
        g.setOpacity (live);
        g.drawRoundedRectangle (bounds.reduced (0.75f), theme::tileRadius, 1.5f);
        g.setOpacity (1.0f);
    }

    // Name and note.
    const juce::String name (drumClassName ((DrumClass) pad));
    g.setColour (hasSound ? theme::ink : theme::inkDim);
    g.setFont (theme::font (14.0f, theme::Weight::medium));
    g.drawText (name.substring (0, 1) + name.substring (1).toLowerCase(), juce::Rectangle<float> (18.0f, 16.0f, 90.0f, 20.0f),
                juce::Justification::centredLeft);
    g.setColour (theme::inkDim);
    g.setFont (theme::mono (11.0f));
    g.drawText (juce::MidiMessage::getMidiNoteName (KitSampler::noteForPad (pad), true, true, 3),
                juce::Rectangle<float> (bounds.getWidth() - 78.0f, 17.0f, 60.0f, 18.0f), juce::Justification::centredRight);

    if (! hasSound)
    {
        g.drawText (placeholder, juce::Rectangle<float> (18.0f, 222.0f, bounds.getWidth() - 36.0f, 18.0f), juce::Justification::centredLeft);
        return;
    }

    // The sound's shape: grey with a black outline, in the aurora while it plays.
    auto shape = waveform;
    const auto box = juce::Rectangle<float> (18.0f, 92.0f, bounds.getWidth() - 36.0f, 72.0f);
    shape.applyTransform (juce::AffineTransform::scale (box.getWidth(), box.getHeight()).translated (box.getX(), box.getY()));
    const auto colours = theme::auroraShort (box.getTopLeft(), box.getTopRight());
    g.setColour (theme::fill);
    g.fillPath (shape);
    g.setColour (theme::ink);
    g.strokePath (shape, juce::PathStrokeType (1.0f));
    if (live > 0.0f)
    {
        // Covers the plain shape with the coloured one as it lights up.
        g.setColour (theme::card.withAlpha (live));
        g.fillPath (shape);
        g.setGradientFill (colours);
        g.setOpacity (0.18f * live);
        g.fillPath (shape);
        g.setOpacity (live);
        g.strokePath (shape, juce::PathStrokeType (1.25f));
        g.setOpacity (1.0f);
    }

    // How many of the song's hits sound like this one.
    g.setColour (theme::ink);
    g.setFont (theme::font (26.0f, theme::Weight::light));
    g.drawText (juce::String (sample->hitsStacked), juce::Rectangle<float> (18.0f, 190.0f, 100.0f, 34.0f), juce::Justification::centredLeft);
    g.setColour (theme::inkDim);
    g.setFont (theme::mono (11.0f));
    g.drawText ((sample->hitsStacked == 1 ? "hit  " : "hits  ") + theme::dot() + "  drag me",
                juce::Rectangle<float> (18.0f, 228.0f, bounds.getWidth() - 30.0f, 16.0f), juce::Justification::centredLeft);

    if (variationCount > 1)
    {
        g.setColour (theme::ink);
        g.setFont (theme::mono (12.0f));
        g.drawText (pickerText(), juce::Rectangle<float> (previousArea().getRight() + 4.0f, 250.0f, 60.0f, 16.0f),
                    juce::Justification::centredLeft);

        auto chevron = [&] (juce::Rectangle<float> r, bool left)
        {
            const auto c = r.getCentre();
            const float d = left ? 1.0f : -1.0f;
            juce::Path p;
            p.startNewSubPath (c.x + 2.0f * d, c.y - 4.0f);
            p.lineTo (c.x - 2.0f * d, c.y);
            p.lineTo (c.x + 2.0f * d, c.y + 4.0f);
            g.strokePath (p, juce::PathStrokeType (1.2f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
        };
        chevron (previousArea(), true);
        chevron (nextArea(), false);
    }
}

void PadComponent::mouseDown (const juce::MouseEvent& e)
{
    dragStarted = false;
    if (sample == nullptr)
        return;

    if (variationCount > 1 && previousArea().expanded (4.0f).contains (e.position))
    {
        if (onVariation) onVariation (-1);
        if (onPlay) onPlay();
        return;
    }
    if (variationCount > 1 && nextArea().expanded (4.0f).contains (e.position))
    {
        if (onVariation) onVariation (1);
        if (onPlay) onPlay();
        return;
    }
    if (onPlay)
        onPlay();
}

void PadComponent::mouseDrag (const juce::MouseEvent& e)
{
    if (sample != nullptr && ! dragStarted && e.getDistanceFromDragStart() > 8)
    {
        dragStarted = true;
        if (onDragOut)
            onDragOut();
    }
}

void PadComponent::mouseMove (const juce::MouseEvent&)
{
    if (! hover) { hover = true; repaint(); }
}

void PadComponent::mouseExit (const juce::MouseEvent&)
{
    hover = false;
    repaint();
}
