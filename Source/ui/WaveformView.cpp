#include "WaveformView.h"
#include "Theme.h"

using namespace kitnap;

namespace
{
constexpr float revealSeconds = 1.6f;
constexpr int laneCount = numDrumClasses;
}

void WaveformView::kitChanged()
{
    const auto status = processor.getKit().status;
    if (status == KitState::Status::ready && lastStatus != KitState::Status::ready)
        revealStart = time;
    if (status == KitState::Status::analysing)
        shownProgress = 0.0f;
    lastStatus = status;
    repaint();
}

void WaveformView::tick (double seconds)
{
    time += seconds;
    shownProgress += 0.15f * (processor.getProgress() - shownProgress);
    repaint();
}

void WaveformView::mouseUp (const juce::MouseEvent& e)
{
    if (e.mouseWasClicked() && processor.getKit().status != KitState::Status::analysing && onBrowse)
        onBrowse();
}

void WaveformView::paint (juce::Graphics& g)
{
    auto area = getLocalBounds().toFloat().reduced (1.0f);
    const auto& kit = processor.getKit();

    g.setColour (theme::panel);
    g.fillRoundedRectangle (area, 14.0f);

    const auto edge = dragHover ? theme::accent : theme::panelEdge;
    g.setColour (edge.withAlpha (dragHover ? 0.9f : 1.0f));
    g.drawRoundedRectangle (area, 14.0f, dragHover ? 2.0f : 1.0f);

    if (dragHover)
    {
        g.setColour (theme::accent.withAlpha (0.08f));
        g.fillRoundedRectangle (area, 14.0f);
    }

    auto inner = area.reduced (18.0f, 14.0f);

    if (kit.overview.empty() && kit.status != KitState::Status::analysing)
    {
        if (kit.status == KitState::Status::ready)
        {
            // Restored from a project without the source file: pads work, nothing to draw.
            g.setColour (theme::textDim);
            g.setFont (theme::font (15.0f));
            g.drawText ("Kit from \"" + kit.sourceName + "\"  " + theme::dot() + "  drop a new track to steal another kit",
                        inner, juce::Justification::centred);
            return;
        }
        paintEmpty (g, inner);
        return;
    }

    // Title row
    auto titleRow = inner.removeFromTop (22.0f);
    g.setFont (theme::font (15.0f, true));
    g.setColour (theme::text);
    g.drawText (kit.sourceName, titleRow, juce::Justification::centredLeft);

    g.setFont (theme::font (13.0f));
    g.setColour (kit.status == KitState::Status::error ? theme::accent : theme::textDim);
    juce::String status = kit.message;
    if (kit.status == KitState::Status::analysing)
        status = (processor.getStage() == KitState::Stage::separating ? "Separating drums with AI...  " : "Kidnapping drums...  ")
                 + juce::String (juce::roundToInt (shownProgress * 100.0f)) + "%";
    else if (kit.status == KitState::Status::ready)
        status = kit.message + "   " + theme::dot() + "   click to load another";
    g.drawText (status, titleRow, juce::Justification::centredRight);
    inner.removeFromTop (8.0f);

    auto lanes = inner.removeFromBottom (laneCount * 7.0f);
    inner.removeFromBottom (8.0f);

    const bool analysing = kit.status == KitState::Status::analysing;
    paintWaveform (g, inner, analysing ? 0.45f : 1.0f);

    if (analysing)
    {
        // Scanner sweeping across the song: violet while the AI separates, pink while kidnapping.
        g.reduceClipRegion (inner.withBottom (lanes.getBottom()).toNearestInt());
        const auto scanColour = processor.getStage() == KitState::Stage::separating ? theme::forClass (DrumClass::perc) : theme::accent;
        const float x = inner.getX() + inner.getWidth() * juce::jlimit (0.0f, 1.0f, shownProgress);
        juce::ColourGradient glow (scanColour.withAlpha (0.0f), x - 80.0f, 0.0f,
                                   scanColour.withAlpha (0.35f), x, 0.0f, false);
        g.setGradientFill (glow);
        g.fillRect (juce::Rectangle<float> (x - 80.0f, inner.getY(), 80.0f, inner.getHeight() + lanes.getHeight() + 8.0f));
        g.setColour (scanColour);
        g.fillRect (juce::Rectangle<float> (x - 1.0f, inner.getY(), 2.0f, inner.getHeight() + lanes.getHeight() + 8.0f));
        return;
    }

    paintHits (g, inner.withBottom (lanes.getBottom()));
}

void WaveformView::paintEmpty (juce::Graphics& g, juce::Rectangle<float> area)
{
    const float pulse = 0.5f + 0.5f * std::sin ((float) time * 2.4f);

    juce::Path dashed;
    dashed.addRoundedRectangle (area.reduced (4.0f), 10.0f);
    juce::Path stroke;
    const float dashes[] { 8.0f, 6.0f };
    juce::PathStrokeType (1.5f).createDashedStroke (stroke, dashed, dashes, 2);
    g.setColour ((dragHover ? theme::accent : theme::textDim).withAlpha (0.35f + 0.25f * pulse));
    g.fillPath (stroke);

    // Arrow dropping into a tray.
    auto icon = area.withSizeKeepingCentre (44.0f, 44.0f).translated (0.0f, -26.0f);
    const float bounce = 4.0f * pulse;
    juce::Path arrow;
    arrow.startNewSubPath (icon.getCentreX(), icon.getY() + bounce);
    arrow.lineTo (icon.getCentreX(), icon.getY() + 24.0f + bounce);
    arrow.startNewSubPath (icon.getCentreX() - 9.0f, icon.getY() + 15.0f + bounce);
    arrow.lineTo (icon.getCentreX(), icon.getY() + 24.0f + bounce);
    arrow.lineTo (icon.getCentreX() + 9.0f, icon.getY() + 15.0f + bounce);
    arrow.startNewSubPath (icon.getX() + 4.0f, icon.getY() + 26.0f);
    arrow.lineTo (icon.getX() + 4.0f, icon.getBottom());
    arrow.lineTo (icon.getRight() - 4.0f, icon.getBottom());
    arrow.lineTo (icon.getRight() - 4.0f, icon.getY() + 26.0f);
    g.setColour (theme::accent);
    g.strokePath (arrow, juce::PathStrokeType (3.0f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));

    g.setColour (theme::text);
    g.setFont (theme::font (22.0f, true));
    g.drawText ("Drop a song or drum loop", juce::Rectangle<float> (area.getX(), area.getCentreY() + 8.0f, area.getWidth(), 30.0f),
                juce::Justification::centred);
    g.setColour (theme::textDim);
    g.setFont (theme::font (14.0f));
    g.drawText ("or click to browse  " + theme::dot() + "  WAV  MP3  AIFF  FLAC  OGG",
                juce::Rectangle<float> (area.getX(), area.getCentreY() + 40.0f, area.getWidth(), 22.0f),
                juce::Justification::centred);
}

void WaveformView::paintWaveform (juce::Graphics& g, juce::Rectangle<float> area, float brightness)
{
    const auto& overview = processor.getKit().overview;
    const int buckets = (int) overview.size() / 2;
    if (buckets == 0)
        return;

    const int width = (int) area.getWidth();
    const float mid = area.getCentreY(), half = area.getHeight() * 0.5f;
    juce::ColourGradient gradient (juce::Colour (0xff6d7390).withMultipliedBrightness (brightness), area.getX(), area.getY(),
                                   juce::Colour (0xff3b4057).withMultipliedBrightness (brightness), area.getX(), area.getBottom(), false);
    g.setGradientFill (gradient);

    for (int x = 0; x < width; ++x)
    {
        const int b0 = x * buckets / width, b1 = std::max (b0 + 1, (x + 1) * buckets / width);
        float lo = 0.0f, hi = 0.0f;
        for (int b = b0; b < b1 && b < buckets; ++b)
        {
            lo = std::min (lo, overview[(size_t) b * 2]);
            hi = std::max (hi, overview[(size_t) b * 2 + 1]);
        }
        const float top = mid - juce::jlimit (0.0f, 1.0f, hi) * half;
        const float bottom = mid - juce::jlimit (-1.0f, 0.0f, lo) * half;
        g.fillRect (area.getX() + (float) x, top, 1.0f, std::max (1.0f, bottom - top));
    }
}

void WaveformView::paintHits (juce::Graphics& g, juce::Rectangle<float> area)
{
    const auto& kit = processor.getKit();
    if (kit.sourceLengthSeconds <= 0.0)
        return;

    auto lanes = area.removeFromBottom (laneCount * 7.0f);
    const float elapsed = (float) (time - revealStart);
    const float revealX = area.getX() + area.getWidth() * juce::jlimit (0.0f, 1.0f, elapsed / revealSeconds);

    // Lane guides
    for (int lane = 0; lane < laneCount; ++lane)
    {
        g.setColour (theme::forClass ((DrumClass) lane).withAlpha (0.08f));
        g.fillRoundedRectangle (lanes.getX(), lanes.getY() + lane * 7.0f + 1.0f, lanes.getWidth(), 5.0f, 2.5f);
    }

    for (const auto& hit : kit.hits)
    {
        const float fraction = (float) (hit.position / kit.hitSampleRate / kit.sourceLengthSeconds);
        const float x = area.getX() + area.getWidth() * fraction;
        if (x > revealX)
            continue;

        // Pop in as the reveal sweep passes, then settle.
        const float age = (revealX - x) / std::max (1.0f, area.getWidth()) * revealSeconds;
        const float pop = 1.0f + 0.8f * std::exp (-age * 9.0f);

        auto drawHit = [&] (int drumClass, float alpha)
        {
            const auto colour = theme::forClass ((DrumClass) drumClass);
            g.setColour (colour.withAlpha (0.18f * alpha * std::min (1.0f, 0.4f + hit.strength)));
            g.fillRect (x - 0.5f, area.getY(), 1.0f, area.getHeight());
            g.setColour (colour.withAlpha (alpha));
            const float size = 4.0f * pop;
            g.fillEllipse (x - size / 2.0f, lanes.getY() + drumClass * 7.0f + 3.5f - size / 2.0f, size, size);
        };

        drawHit ((int) hit.drumClass, 1.0f);
        if (hit.secondaryClass >= 0)
            drawHit (hit.secondaryClass, 0.7f);
    }

    if (elapsed < revealSeconds)
    {
        g.setColour (juce::Colours::white.withAlpha (0.7f));
        g.fillRect (revealX - 1.0f, area.getY(), 2.0f, area.getHeight() + lanes.getHeight());
    }
}
