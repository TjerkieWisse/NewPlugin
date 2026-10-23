#include "WaveformView.h"
#include "Theme.h"

using namespace kitnap;

namespace
{
constexpr int stripeCount = 120;
constexpr float innerRadius = 88.0f;   // where the stripes start
constexpr float coreRadius = 84.0f;    // the thin ring around the centre text
constexpr float maxLength = 70.0f;
constexpr double lapSeconds = 3.4;     // one wave of light around the ring
constexpr double breathSeconds = 6.8;  // the centre ring's slow breath
const juce::Point<float> ringCentre { 210.0f, 252.0f };

juce::String duration (double seconds)
{
    const int s = juce::roundToInt (seconds);
    return juce::String (s / 60) + ":" + juce::String (s % 60).paddedLeft ('0', 2);
}

// How brightly a stripe shines, `age` (0..1 of a lap) after the wave passed it: a quick rise,
// then an easing fade over `fade`.
float shine (float age, float rise, float fade)
{
    if (age < rise)
        return age / rise;
    const float x = (age - rise) / (fade - rise);
    return x >= 1.0f ? 0.0f : std::pow (1.0f - x, 2.4f);
}
} // namespace

WaveformView::WaveformView (KitnapAudioProcessor& p) : processor (p)
{
    // Everything the stripes can reach, glow and pop included; it stays inside the white card.
    const float reach = (innerRadius + 4.0f + maxLength) * 1.08f + 8.0f;
    ring.setBounds (juce::Rectangle<float> (reach * 2.0f, reach * 2.0f).withCentre (ringCentre).getSmallestIntegerContainer());
    addAndMakeVisible (ring);
}

void WaveformView::Ring::paint (juce::Graphics& g)
{
    // With a finished kit the stripes rest in black and only a few are lit at any moment, so the
    // black ring is drawn once into a picture and the lit stripes go on top.
    const bool ready = owner.processor.getKit().status == KitState::Status::ready && ! owner.stripes.empty();
    const float scale = g.getInternalContext().getPhysicalPixelScaleFactor();
    if (ready && (! restingRing.isValid() || ! juce::approximatelyEqual (restingScale, scale)))
    {
        restingScale = scale;
        restingRing = juce::Image (juce::Image::RGB, juce::roundToInt ((float) getWidth() * scale),
                                   juce::roundToInt ((float) getHeight() * scale), false);
        juce::Graphics rg (restingRing);
        rg.addTransform (juce::AffineTransform::scale (scale));
        rg.fillAll (theme::card);
        rg.setOrigin (-getPosition());
        owner.paintRing (rg, ringCentre, true);
    }

    if (ready)
        g.drawImage (restingRing, getLocalBounds().toFloat());
    else
        g.fillAll (theme::card);
    g.setOrigin (-getPosition());
    owner.paintRing (g, ringCentre, false);
    owner.paintCentre (g, ringCentre);
}

void WaveformView::kitChanged()
{
    const auto& kit = processor.getKit();
    if (kit.status == KitState::Status::analysing && lastStatus != KitState::Status::analysing)
        shownProgress = 0.0f;
    lastStatus = kit.status;

    // One stripe per 1/120 of the song: its loudest moment. The loud ones that rise above their
    // neighbour are the hits.
    stripes.clear();
    ring.restingRing = {};
    const int buckets = (int) kit.overview.size() / 2;
    if (buckets > 0)
    {
        stripes.resize (stripeCount);
        float top = 0.0f;
        for (int i = 0; i < stripeCount; ++i)
        {
            const int b0 = i * buckets / stripeCount, b1 = std::max (b0 + 1, (i + 1) * buckets / stripeCount);
            for (int b = b0; b < b1; ++b)
                stripes[(size_t) i].level = std::max ({ stripes[(size_t) i].level, -kit.overview[(size_t) b * 2], kit.overview[(size_t) b * 2 + 1] });
            top = std::max (top, stripes[(size_t) i].level);
        }
        for (auto& s : stripes)
            s.level = top > 0.0f ? s.level / top : 0.0f;
        for (int i = 0; i < stripeCount; ++i)
        {
            auto& s = stripes[(size_t) i];
            const float before = stripes[(size_t) ((i + stripeCount - 1) % stripeCount)].level;
            const float after = stripes[(size_t) ((i + 1) % stripeCount)].level;
            s.loud = s.level > 0.72f && s.level >= before && s.level >= after;
            s.soft = s.level < 0.35f;
        }
    }
    repaint();
}

void WaveformView::tick (double seconds)
{
    time += seconds;
    shownProgress += 0.15f * (processor.getProgress() - shownProgress);

    ring.repaint();
}

void WaveformView::mouseUp (const juce::MouseEvent& e)
{
    // The ring area loads a track; the volume row below it doesn't.
    if (e.mouseWasClicked() && e.y < 440 && processor.getKit().status != KitState::Status::analysing && onBrowse)
        onBrowse();
}

void WaveformView::paint (juce::Graphics& g)
{
    const auto bounds = getLocalBounds().toFloat();
    const auto& kit = processor.getKit();
    const bool analysing = kit.status == KitState::Status::analysing;

    g.setColour (theme::card);
    g.fillRoundedRectangle (bounds, theme::cardRadius);
    g.setColour (dragHover ? theme::accent : theme::border);
    g.drawRoundedRectangle (bounds.reduced (dragHover ? 1.0f : 0.5f), theme::cardRadius, dragHover ? 2.0f : 1.0f);

    // Title, details and how many sounds were found.
    juce::String title = kit.sourceName, details;
    if (kit.sourceName.isEmpty())
    {
        title = "No song yet";
        details = "WAV  MP3  AIFF  FLAC  OGG";
    }
    else if (kit.status == KitState::Status::error)
        details = kit.message;
    else if (analysing)
        details = kit.sourceLengthSeconds > 0.0 ? duration (kit.sourceLengthSeconds) : juce::String ("reading the file");
    else
        details = (kit.sourceLengthSeconds > 0.0 ? duration (kit.sourceLengthSeconds) + "  " + theme::dot() + "  " : juce::String())
                  + "click to load another";

    g.setColour (theme::ink);
    g.setFont (theme::font (15.0f, theme::Weight::medium));
    g.drawText (title, juce::Rectangle<float> (24.0f, 16.0f, bounds.getWidth() - 140.0f, 22.0f), juce::Justification::centredLeft, true);
    g.setColour (kit.status == KitState::Status::error ? theme::error : theme::inkDim);
    g.setFont (theme::mono (11.0f));
    g.drawText (details, juce::Rectangle<float> (24.0f, 40.0f, bounds.getWidth() - 48.0f, 18.0f), juce::Justification::centredLeft, true);

    if (kit.status == KitState::Status::ready)
    {
        int sounds = 0;
        for (auto& v : kit.variations)
            sounds += v.empty() ? 0 : 1;
        g.setColour (theme::inkDim);
        g.drawText (juce::String (sounds) + (sounds == 1 ? " sound" : " sounds"),
                    juce::Rectangle<float> (bounds.getWidth() - 124.0f, 18.0f, 100.0f, 18.0f), juce::Justification::centredRight);
    }

    paintVolume (g);
}

void WaveformView::paintRing (juce::Graphics& g, juce::Point<float> c, bool resting)
{
    const auto& kit = processor.getKit();
    const bool analysing = kit.status == KitState::Status::analysing;
    const bool ready = kit.status == KitState::Status::ready && ! stripes.empty();
    const float done = juce::jlimit (0.0f, 1.0f, shownProgress);
    const float lap = (float) std::fmod (time / lapSeconds, 1.0);
    const juce::PathStrokeType stroke (2.0f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded);

    juce::Path quiet; // everything drawn in one plain colour goes into one path
    const auto quietColour = ready || analysing ? theme::ink : (dragHover ? theme::accentSoft : juce::Colour (0xffe1e1e1));

    for (int i = 0; i < stripeCount; ++i)
    {
        const float a = juce::MathConstants<float>::twoPi * (float) i / stripeCount;
        const juce::Point<float> dir (std::sin (a), -std::cos (a));
        const Stripe s = stripes.empty() ? Stripe {} : stripes[(size_t) i];
        const float length = stripes.empty() ? 12.0f : 4.0f + std::pow (s.level, 0.9f) * maxLength; // short ticks with no song

        juce::Colour colour = quietColour;
        float glow = 0.0f, scale = 1.0f;

        if (ready && ! resting)
        {
            // The wave of light passes stripe i at i/120 of each lap.
            float age = lap - (float) i / stripeCount;
            if (age < 0.0f)
                age += 1.0f;
            if (s.loud)
            {
                const float b = shine (age, 0.015f, 0.34f);
                colour = theme::ink.interpolatedWith (theme::accentBright, b);
                glow = b * 0.9f;
                scale = 1.0f + 0.08f * shine (age, 0.015f, 0.12f);
            }
            else if (s.soft)
                colour = theme::ink.interpolatedWith (theme::accentSoft, shine (age, 0.02f, 0.14f));
            else
            {
                const float b = shine (age, 0.02f, 0.24f);
                colour = theme::ink.interpolatedWith (theme::accent, b);
                glow = b * 0.5f;
            }
        }
        else if (analysing)
        {
            // The ring fills up with the progress; a short blue scanner leads it.
            const float pos = (float) i / stripeCount;
            if (pos > done)
                colour = juce::Colour (0xffe1e1e1);
            const float behind = done - pos;
            if (behind >= 0.0f && behind < 0.04f)
            {
                const float b = 1.0f - behind / 0.04f;
                colour = theme::ink.interpolatedWith (theme::accent, b);
                glow = b * 0.6f;
            }
        }

        // Loud stripes stretch outwards when the light hits them.
        const auto from = c + dir * innerRadius;
        const auto to = c + dir * (innerRadius + length * scale);
        if (colour == quietColour && glow <= 0.0f)
        {
            if (ready && ! resting)
                continue; // already in the resting picture
            quiet.startNewSubPath (from);
            quiet.lineTo (to);
            continue;
        }

        juce::Path p;
        p.startNewSubPath (from);
        p.lineTo (to);
        if (glow > 0.01f)
        {
            g.setColour (theme::accent.withAlpha (0.10f * glow));
            g.strokePath (p, juce::PathStrokeType (10.0f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
            g.setColour (theme::accent.withAlpha (0.22f * glow));
            g.strokePath (p, juce::PathStrokeType (5.0f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
        }
        g.setColour (colour);
        g.strokePath (p, stroke);
    }

    g.setColour (quietColour);
    g.strokePath (quiet, stroke);
}

void WaveformView::paintCentre (juce::Graphics& g, juce::Point<float> c)
{
    const auto& kit = processor.getKit();

    // The thin centre ring breathes slowly: light grey to a pale blue with a faint glow and back.
    const float breath = 0.5f - 0.5f * std::cos ((float) (time / breathSeconds) * juce::MathConstants<float>::twoPi);
    const auto circle = juce::Rectangle<float> (coreRadius * 2.0f, coreRadius * 2.0f).withCentre (c);
    if (breath > 0.02f)
    {
        juce::Path halo;
        halo.addEllipse (circle.expanded (16.0f));
        halo.addEllipse (circle.reduced (6.0f));
        halo.setUsingNonZeroWinding (false);
        const float outer = coreRadius + 16.0f;
        juce::ColourGradient light (theme::accent.withAlpha (0.0f), c, theme::accent.withAlpha (0.0f), c.translated (outer, 0.0f), true);
        light.addColour ((coreRadius - 6.0f) / outer, theme::accent.withAlpha (0.0f));
        light.addColour (coreRadius / outer, theme::accent.withAlpha (0.10f * breath));
        light.addColour ((coreRadius + 5.0f) / outer, theme::accent.withAlpha (0.035f * breath));
        g.setGradientFill (light);
        g.fillPath (halo);
    }
    g.setColour (theme::line.interpolatedWith (juce::Colour (0xffd3dcff), breath));
    g.drawEllipse (circle, 1.0f);

    juce::String big, small;
    switch (kit.status)
    {
        case KitState::Status::analysing:
        {
            const auto stage = processor.getStage();
            big = juce::String (juce::roundToInt (juce::jlimit (0.0f, 1.0f, shownProgress) * 100.0f)) + "%";
            small = stage == KitState::Stage::separating ? "separating drums"
                  : stage == KitState::Stage::splitting  ? "splitting the kit"
                  : stage == KitState::Stage::reading    ? "reading"
                                                         : "cutting the sounds";
            break;
        }
        case KitState::Status::ready:
            big = juce::String ((int) kit.hits.size());
            small = kit.hits.size() == 1 ? "hit found" : "hits found";
            break;
        case KitState::Status::error:
            big = "-";
            small = "try another track";
            break;
        case KitState::Status::empty:
            break;
    }

    if (big.isEmpty())
    {
        g.setColour (theme::ink);
        g.setFont (theme::font (19.0f, theme::Weight::light));
        g.drawText ("Drop a song", juce::Rectangle<float> (200.0f, 26.0f).withCentre (c.translated (0.0f, -8.0f)), juce::Justification::centred);
        g.setColour (theme::inkDim);
        g.setFont (theme::mono (11.0f));
        g.drawText ("or click to browse", juce::Rectangle<float> (200.0f, 18.0f).withCentre (c.translated (0.0f, 18.0f)), juce::Justification::centred);
        return;
    }

    g.setColour (theme::ink);
    g.setFont (theme::font (46.0f, theme::Weight::light));
    g.drawText (big, juce::Rectangle<float> (220.0f, 52.0f).withCentre (c.translated (0.0f, -8.0f)), juce::Justification::centred);
    g.setColour (theme::inkDim);
    g.setFont (theme::mono (11.0f));
    g.drawText (small, juce::Rectangle<float> (200.0f, 18.0f).withCentre (c.translated (0.0f, 28.0f)), juce::Justification::centred);
}

void WaveformView::paintVolume (juce::Graphics& g)
{
    const float w = (float) getWidth();
    g.setColour (juce::Colour (0xfff0f0f0));
    g.fillRect (juce::Rectangle<float> (24.0f, 446.0f, w - 48.0f, 1.0f));

    g.setColour (theme::ink);
    g.setFont (theme::font (13.0f, theme::Weight::medium));
    g.drawText ("Volume", juce::Rectangle<float> (24.0f, 462.0f, 120.0f, 20.0f), juce::Justification::centredLeft);

    const float db = processor.apvts.getRawParameterValue ("volume")->load();
    g.setFont (theme::mono (11.0f));
    g.drawText ((db > 0.05f ? "+" : "") + juce::String (db, 1) + " dB", juce::Rectangle<float> (w - 144.0f, 462.0f, 120.0f, 20.0f),
                juce::Justification::centredRight);
}
