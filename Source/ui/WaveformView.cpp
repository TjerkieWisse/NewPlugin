#include "WaveformView.h"
#include "Theme.h"

using namespace kitnap;

void WaveformView::kitChanged()
{
    if (processor.getKit().status == KitState::Status::analysing && lastStatus != KitState::Status::analysing)
        shownProgress = 0.0f;
    lastStatus = processor.getKit().status;
    repaint();
}

void WaveformView::tick (double seconds)
{
    time += seconds;
    shownProgress += 0.15f * (processor.getProgress() - shownProgress);

    // Only redraw while something moves: the scanner, or the pulsing drop zone (a slow pulse, so
    // every other frame is plenty).
    const auto& kit = processor.getKit();
    ++ticks;
    if (kit.status == KitState::Status::analysing || (kit.overview.empty() && kit.status != KitState::Status::ready && ticks % 2 == 0))
        repaint();
}

void WaveformView::mouseUp (const juce::MouseEvent& e)
{
    if (e.mouseWasClicked() && processor.getKit().status != KitState::Status::analysing && onBrowse)
        onBrowse();
}

void WaveformView::paint (juce::Graphics& g)
{
    auto area = getLocalBounds().toFloat().reduced (theme::outline / 2.0f);
    const auto& kit = processor.getKit();

    g.setColour (dragHover ? theme::background : theme::panel);
    g.fillRoundedRectangle (area, theme::radius);
    g.setColour (theme::ink);
    g.drawRoundedRectangle (area, theme::radius, dragHover ? theme::outline * 2.0f : theme::outline);

    auto inner = area.reduced (20.0f, 16.0f);

    if (kit.overview.empty() && kit.status != KitState::Status::analysing)
    {
        if (kit.status == KitState::Status::ready)
        {
            // Restored from a project without the source file: pads work, nothing to draw.
            g.setColour (theme::inkDim);
            g.setFont (theme::font (14.0f));
            g.drawText ("Kit from \"" + kit.sourceName + "\"  " + theme::dot() + "  drop a new track to steal another kit",
                        inner, juce::Justification::centred);
            return;
        }
        paintEmpty (g, inner);
        return;
    }

    // Title row: song name on the left, what's happening on the right.
    auto titleRow = inner.removeFromTop (20.0f);
    g.setFont (theme::font (14.0f, true));
    g.setColour (theme::ink);
    g.drawText (kit.sourceName, titleRow, juce::Justification::centredLeft);

    const auto stage = processor.getStage();
    const bool analysing = kit.status == KitState::Status::analysing;
    juce::String status = kit.message;
    if (analysing)
        status = juce::String (stage == KitState::Stage::separating ? "SEPARATING DRUMS"
                             : stage == KitState::Stage::splitting  ? "SPLITTING THE KIT"
                                                                    : "CUTTING THE SOUNDS")
                 + "   " + juce::String (juce::roundToInt (shownProgress * 100.0f)) + "%";
    if (analysing)
    {
        g.setFont (theme::display (9.5f));
        g.setColour (theme::ink);
    }
    else
    {
        if (kit.status == KitState::Status::ready)
            status = kit.message + "   " + theme::dot() + "   click to load another";
        g.setFont (theme::font (13.0f));
        g.setColour (kit.status == KitState::Status::error ? theme::error : theme::inkDim);
    }
    g.drawText (status, titleRow, juce::Justification::centredRight);
    inner.removeFromTop (14.0f);

    // While analysing, a scanner sweeps through the song: the part it has passed turns black.
    const float done = analysing ? juce::jlimit (0.0f, 1.0f, shownProgress) : 1.0f;
    const float x = inner.getX() + inner.getWidth() * done;
    {
        juce::Graphics::ScopedSaveState s (g);
        g.reduceClipRegion (inner.withLeft (x).toNearestInt());
        paintWaveform (g, inner, theme::line);
    }
    {
        juce::Graphics::ScopedSaveState s (g);
        g.reduceClipRegion (inner.withRight (x).toNearestInt());
        paintWaveform (g, inner, theme::ink);
    }
    if (done < 1.0f)
    {
        g.setColour (theme::ink);
        g.fillRect (juce::Rectangle<float> (x - 0.75f, inner.getY() - 4.0f, 1.5f, inner.getHeight() + 8.0f));
    }
}

void WaveformView::paintEmpty (juce::Graphics& g, juce::Rectangle<float> area)
{
    const float pulse = 0.5f + 0.5f * std::sin ((float) time * 2.4f);

    juce::Path dashed;
    dashed.addRoundedRectangle (area.reduced (2.0f), theme::radius * 0.75f);
    juce::Path stroke;
    const float dashes[] { 6.0f, 5.0f };
    juce::PathStrokeType (1.0f).createDashedStroke (stroke, dashed, dashes, 2);
    g.setColour (theme::ink.withAlpha (dragHover ? 0.9f : 0.25f + 0.15f * pulse));
    g.fillPath (stroke);

    // Arrow dropping into a tray.
    auto icon = area.withSizeKeepingCentre (40.0f, 40.0f).translated (0.0f, -28.0f);
    const float bounce = 4.0f * pulse;
    juce::Path arrow;
    arrow.startNewSubPath (icon.getCentreX(), icon.getY() + bounce);
    arrow.lineTo (icon.getCentreX(), icon.getY() + 22.0f + bounce);
    arrow.startNewSubPath (icon.getCentreX() - 8.0f, icon.getY() + 14.0f + bounce);
    arrow.lineTo (icon.getCentreX(), icon.getY() + 22.0f + bounce);
    arrow.lineTo (icon.getCentreX() + 8.0f, icon.getY() + 14.0f + bounce);
    arrow.startNewSubPath (icon.getX() + 4.0f, icon.getY() + 24.0f);
    arrow.lineTo (icon.getX() + 4.0f, icon.getBottom());
    arrow.lineTo (icon.getRight() - 4.0f, icon.getBottom());
    arrow.lineTo (icon.getRight() - 4.0f, icon.getY() + 24.0f);
    g.setColour (theme::ink);
    g.strokePath (arrow, juce::PathStrokeType (theme::outline, juce::PathStrokeType::mitered, juce::PathStrokeType::square));

    g.setFont (theme::display (15.0f));
    g.drawText ("DROP A SONG OR DRUM LOOP", juce::Rectangle<float> (area.getX(), area.getCentreY() + 8.0f, area.getWidth(), 28.0f),
                juce::Justification::centred);
    g.setColour (theme::inkDim);
    g.setFont (theme::font (13.0f));
    g.drawText ("or click to browse  " + theme::dot() + "  WAV  MP3  AIFF  FLAC  OGG",
                juce::Rectangle<float> (area.getX(), area.getCentreY() + 40.0f, area.getWidth(), 22.0f),
                juce::Justification::centred);
}

void WaveformView::paintWaveform (juce::Graphics& g, juce::Rectangle<float> area, juce::Colour colour)
{
    const auto& overview = processor.getKit().overview;
    const int buckets = (int) overview.size() / 2;
    if (buckets == 0)
        return;

    // Thin bars with gaps, mirrored around the centre line.
    constexpr float step = 3.0f, bar = 2.0f;
    const int bars = (int) (area.getWidth() / step);
    const float mid = area.getCentreY(), half = area.getHeight() * 0.5f;
    g.setColour (colour);

    for (int i = 0; i < bars; ++i)
    {
        const int b0 = i * buckets / bars, b1 = std::max (b0 + 1, (i + 1) * buckets / bars);
        float peak = 0.0f;
        for (int b = b0; b < b1 && b < buckets; ++b)
            peak = std::max ({ peak, -overview[(size_t) b * 2], overview[(size_t) b * 2 + 1] });
        const float h = std::max (1.0f, juce::jlimit (0.0f, 1.0f, peak) * half);
        g.fillRect (area.getX() + (float) i * step, mid - h, bar, h * 2.0f);
    }
}
