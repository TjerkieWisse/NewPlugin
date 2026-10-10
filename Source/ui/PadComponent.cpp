#include "PadComponent.h"
#include "Theme.h"

using namespace kitnap;

PadComponent::PadComponent (int padIndex)
    : pad (padIndex), colour (theme::forClass ((DrumClass) padIndex))
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

juce::Rectangle<float> PadComponent::previousArea() const
{
    return getLocalBounds().toFloat().removeFromBottom (34.0f).removeFromRight (96.0f).removeFromLeft (30.0f).translated (-10.0f, -6.0f);
}

juce::Rectangle<float> PadComponent::nextArea() const
{
    return getLocalBounds().toFloat().removeFromBottom (34.0f).removeFromRight (40.0f).translated (-10.0f, -6.0f);
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
    auto area = getLocalBounds().toFloat().reduced (1.0f);
    const bool hasSound = sample != nullptr;
    const float glow = hasSound ? activity : 0.0f;

    // Body with a glow that flares when the pad is hit.
    juce::ColourGradient body (theme::panel.interpolatedWith (colour, 0.05f + 0.35f * glow), area.getX(), area.getY(),
                               theme::panel.darker (0.2f), area.getX(), area.getBottom(), false);
    g.setGradientFill (body);
    g.fillRoundedRectangle (area, 14.0f);

    g.setColour (hasSound ? colour.withAlpha (0.35f + 0.65f * glow + (hover ? 0.2f : 0.0f)) : theme::panelEdge);
    g.drawRoundedRectangle (area, 14.0f, 1.0f + 1.5f * glow);

    auto content = area.reduced (16.0f, 12.0f);
    auto header = content.removeFromTop (22.0f);

    g.setColour (hasSound ? colour : theme::textDim);
    g.fillEllipse (header.getX(), header.getCentreY() - 4.0f, 8.0f, 8.0f);
    g.setFont (theme::font (16.0f, true));
    g.setColour (hasSound ? theme::text : theme::textDim);
    g.drawText (drumClassName ((DrumClass) pad), header.withTrimmedLeft (16.0f), juce::Justification::centredLeft);

    g.setFont (theme::font (12.0f));
    g.setColour (theme::textDim);
    g.drawText (juce::MidiMessage::getMidiNoteName (KitSampler::noteForPad (pad), true, true, 3), header,
                juce::Justification::centredRight);

    auto footer = content.removeFromBottom (22.0f);
    content.removeFromBottom (6.0f);

    if (! hasSound)
    {
        g.setFont (theme::font (13.0f));
        g.drawText ("not found yet", content, juce::Justification::centred);
        return;
    }

    // Sound shape
    auto wave = content.reduced (0.0f, 4.0f);
    auto shape = waveform;
    shape.applyTransform (juce::AffineTransform::scale (wave.getWidth(), wave.getHeight()).translated (wave.getX(), wave.getY()));
    g.setColour (colour.withAlpha (0.25f + 0.5f * glow));
    g.fillPath (shape);
    g.setColour (colour.withAlpha (0.8f));
    g.strokePath (shape, juce::PathStrokeType (1.0f));

    // Footer: how many hits were stacked, and the variation picker.
    g.setFont (theme::font (12.0f));
    g.setColour (theme::textDim);
    const auto stacked = sample->hitsStacked > 1 ? juce::String (sample->hitsStacked) + " hits stacked" : juce::String ("single hit");
    g.drawText (stacked + "  " + theme::dot() + "  drag me", footer, juce::Justification::centredLeft);

    if (variationCount > 1)
    {
        g.setColour (theme::text);
        g.setFont (theme::font (13.0f, true));
        g.drawText (juce::String (variation + 1) + "/" + juce::String (variationCount),
                    footer.removeFromRight (86.0f).withTrimmedLeft (26.0f).withTrimmedRight (30.0f), juce::Justification::centred);

        auto drawArrow = [&] (juce::Rectangle<float> r, bool left)
        {
            r = r.withSizeKeepingCentre (22.0f, 22.0f);
            g.setColour (colour.withAlpha (0.18f));
            g.fillEllipse (r);
            juce::Path p;
            const auto c = r.getCentre();
            const float d = left ? -1.0f : 1.0f;
            p.addTriangle (c.x + 4.0f * d, c.y, c.x - 3.0f * d, c.y - 5.0f, c.x - 3.0f * d, c.y + 5.0f);
            g.setColour (colour);
            g.fillPath (p);
        };
        drawArrow (previousArea(), true);
        drawArrow (nextArea(), false);
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
