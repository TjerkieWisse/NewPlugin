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

// The "‹ 1/3 ›" picker at the right edge of the row.
juce::Rectangle<float> PadComponent::previousArea() const
{
    return { (float) getWidth() - 74.0f, (float) getHeight() / 2.0f - 16.0f, 18.0f, 32.0f };
}

juce::Rectangle<float> PadComponent::nextArea() const
{
    return { (float) getWidth() - 36.0f, (float) getHeight() / 2.0f - 16.0f, 18.0f, 32.0f };
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
    const float glow = hasSound ? juce::jlimit (0.0f, 1.0f, activity) : 0.0f;
    const float mid = bounds.getHeight() / 2.0f;

    // White row; while its sound plays it turns pale blue and its play button and shape go blue.
    g.setColour (theme::card.interpolatedWith (theme::activeRow, glow));
    g.fillRoundedRectangle (bounds, theme::rowRadius);
    const auto edge = hover && hasSound ? juce::Colour (0xffd9d9d9) : theme::border;
    g.setColour (edge.interpolatedWith (theme::activeBorder, glow));
    g.drawRoundedRectangle (bounds.reduced (0.5f), theme::rowRadius, 1.0f);

    // Play button, with a ring that spreads out from it as the hit fades.
    const auto play = juce::Rectangle<float> (18.0f, mid - 17.0f, 34.0f, 34.0f);
    if (glow > 0.01f)
    {
        g.setColour (theme::accent.withAlpha (0.45f * glow));
        g.drawEllipse (play.expanded (10.0f * (1.0f - glow)), 1.5f + 3.0f * glow);
    }
    g.setColour (theme::card.interpolatedWith (theme::accent, glow));
    g.fillEllipse (play);
    g.setColour (juce::Colour (0xffe2e2e2).interpolatedWith (theme::accent, glow));
    g.drawEllipse (play.reduced (0.5f), 1.0f);
    juce::Path triangle;
    const auto c = play.getCentre();
    triangle.addTriangle (c.x - 2.5f, c.y - 4.5f, c.x + 5.0f, c.y, c.x - 2.5f, c.y + 4.5f);
    g.setColour ((hasSound ? theme::ink : theme::inkDim.withAlpha (0.5f)).interpolatedWith (theme::card, glow));
    g.fillPath (triangle);

    // Name and note.
    g.setColour (hasSound ? theme::ink : theme::inkDim);
    g.setFont (theme::font (16.0f, theme::Weight::medium));
    const juce::String name (drumClassName ((DrumClass) pad));
    g.drawText (name.substring (0, 1) + name.substring (1).toLowerCase(), juce::Rectangle<float> (68.0f, mid - 22.0f, 100.0f, 22.0f), juce::Justification::centredLeft);
    g.setColour (theme::inkDim);
    g.setFont (theme::mono (11.0f));
    const auto note = juce::MidiMessage::getMidiNoteName (KitSampler::noteForPad (pad), true, true, 3);
    g.drawText (hasSound ? note + "  " + theme::dot() + "  drag me" : note, juce::Rectangle<float> (68.0f, mid + 2.0f, 100.0f, 16.0f),
                juce::Justification::centredLeft);

    if (! hasSound)
    {
        g.drawText (placeholder, juce::Rectangle<float> (170.0f, 0.0f, bounds.getWidth() - 188.0f, bounds.getHeight()),
                    juce::Justification::centredLeft);
        return;
    }

    // Sound shape.
    auto shape = waveform;
    shape.applyTransform (juce::AffineTransform::scale (170.0f, 46.0f).translated (170.0f, mid - 23.0f));
    g.setColour (theme::fill.interpolatedWith (theme::accent.withAlpha (0.14f), glow));
    g.fillPath (shape);
    g.setColour (theme::ink.interpolatedWith (theme::accent, glow));
    g.strokePath (shape, juce::PathStrokeType (1.0f + 0.25f * glow));

    // How many of the song's hits sound like this one.
    g.setColour (theme::ink);
    g.setFont (theme::font (24.0f, theme::Weight::light));
    g.drawText (juce::String (sample->hitsStacked), juce::Rectangle<float> (360.0f, mid - 28.0f, 40.0f, 30.0f), juce::Justification::centredLeft);
    g.setColour (theme::inkDim);
    g.setFont (theme::mono (11.0f));
    g.drawText (sample->hitsStacked == 1 ? "hit" : "hits", juce::Rectangle<float> (360.0f, mid + 5.0f, 40.0f, 16.0f), juce::Justification::centredLeft);

    if (variationCount > 1)
    {
        g.setColour (theme::ink);
        g.setFont (theme::mono (12.0f));
        g.drawText (juce::String (variation + 1) + "/" + juce::String (variationCount),
                    juce::Rectangle<float> (previousArea().getRight(), mid - 8.0f, nextArea().getX() - previousArea().getRight(), 16.0f),
                    juce::Justification::centred);

        auto chevron = [&] (juce::Rectangle<float> r, bool left)
        {
            const auto p0 = r.getCentre();
            const float d = left ? 1.0f : -1.0f;
            juce::Path p;
            p.startNewSubPath (p0.x + 2.0f * d, p0.y - 4.0f);
            p.lineTo (p0.x - 2.0f * d, p0.y);
            p.lineTo (p0.x + 2.0f * d, p0.y + 4.0f);
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
