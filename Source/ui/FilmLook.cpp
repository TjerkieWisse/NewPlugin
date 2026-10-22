#include "FilmLook.h"
#include "Theme.h"

using namespace kitnap;

namespace
{
constexpr int framesPerSecond = 24; // what the timecode counts
constexpr int updatesPerSecond = 12; // how often grain, flicker and dust change, as in the mockup
constexpr int grainFrames = 6;
constexpr float grainTileSize = 256.0f;

// Projector lamp flicker (white over everything) and gate weave positions (logical pixels).
constexpr float flickerLevels[] { 0.12f, 0.22f, 0.09f, 0.18f, 0.25f, 0.10f, 0.19f };
const juce::Point<float> weavePositions[] { { 0.0f, 0.0f }, { 0.4f, -0.3f }, { -0.3f, 0.35f }, { 0.2f, 0.15f } };

// Grading of the picture underneath.
constexpr float fringe = 0.35f;     // how much of the red and blue channels is shifted sideways
constexpr float fringeShift = 0.7f; // by how far (logical pixels)
constexpr float shadowLift = 0.12f; // blacks lifted towards navy
const juce::Colour navy { 0xff16304f };

juce::String emDash() { return juce::String::charToString ((juce::juce_wchar) 0x2014); }
} // namespace

//==============================================================================
FilmOverlay::FilmOverlay()
{
    setInterceptsMouseClicks (false, false);
    setVisible (false);
    scratches.push_back ({ 611.0f, 611.0f, 80 });
}

void FilmOverlay::setOn (bool shouldBeOn)
{
    on = shouldBeOn;
    setVisible (on);
    if (on)
        startTimerHz (updatesPerSecond);
    else
        stopTimer();
}

juce::Point<int> FilmOverlay::weaveOffset (float scale) const
{
    // Whole physical pixels only, so the cached picture never needs resampling: on a 1x display the
    // half-pixel weave rounds away, on a Retina display it moves by one pixel.
    return { juce::roundToInt (weave.x * scale), juce::roundToInt (weave.y * scale) };
}

void FilmOverlay::timerCallback()
{
    ++ticks;
    frame = ticks * framesPerSecond / updatesPerSecond;
    if (ticks % 2 == 0)
        flicker = flickerLevels[random.nextInt ((int) std::size (flickerLevels))];
    weave = weavePositions[ticks % (juce::int64) std::size (weavePositions)];

    grainFrame = random.nextInt (grainFrames);
    // Whole pixels, so the grain is copied rather than resampled.
    grainOffset = { (float) random.nextInt ((int) grainTileSize), (float) random.nextInt ((int) grainTileSize) };

    // Scratches: one that stays for a few seconds and jitters in place, now and then a short second one.
    const float width = (float) getWidth();
    for (auto& s : scratches)
    {
        s.x = s.home + (random.nextFloat() * 2.0f - 1.0f) * 0.9f;
        --s.framesLeft;
    }
    scratches.erase (std::remove_if (scratches.begin(), scratches.end(), [] (auto& s) { return s.framesLeft <= 0; }), scratches.end());
    if (scratches.empty() && random.nextFloat() < 0.08f)
    {
        const float x = 40.0f + random.nextFloat() * (width - 80.0f);
        scratches.push_back ({ x, x, 24 + random.nextInt (56) });
    }
    if (scratches.size() < 2 && random.nextFloat() < 0.016f)
    {
        const float x = 40.0f + random.nextFloat() * (width - 80.0f);
        scratches.push_back ({ x, x, 4 + random.nextInt (6) });
    }

    // Dust and hairs show for a single update.
    dust.clear();
    auto somewhere = [this] { return juce::Point<float> (random.nextFloat() * (float) getWidth(), random.nextFloat() * (float) getHeight()); };
    auto speck = [&]
    {
        Dust d { {}, false, random.nextFloat() < 0.2f };
        const float w = 0.8f + random.nextFloat() * 1.8f, h = w * (0.6f + random.nextFloat() * 0.5f);
        d.shape.addEllipse (-w / 2.0f, -h / 2.0f, w, h);
        const auto at = somewhere();
        d.shape.applyTransform (juce::AffineTransform::rotation (random.nextFloat() * juce::MathConstants<float>::twoPi).translated (at));
        dust.push_back (std::move (d));
    };
    if (random.nextFloat() < 0.45f) speck();
    if (random.nextFloat() < 0.15f) speck();
    if (random.nextFloat() < 0.06f)
    {
        Dust d { {}, true, false };
        auto offset = [this] (float range) { return juce::Point<float> ((random.nextFloat() * 2.0f - 1.0f) * range, (random.nextFloat() * 2.0f - 1.0f) * range); };
        const auto start = somewhere();
        d.shape.startNewSubPath (start);
        d.shape.cubicTo (start + offset (15.0f), start + offset (15.0f), start + offset (25.0f));
        dust.push_back (std::move (d));
    }

    repaint();
}

// Film grain: dark and light grains, some of them tinted, at the display's real pixel size.
const juce::Image& FilmOverlay::grainTile (float scale)
{
    if (tiles.empty() || ! juce::approximatelyEqual (tileScale, scale))
    {
        tiles.clear();
        tileScale = scale;
        const int size = juce::roundToInt (grainTileSize * scale);
        juce::Random r (1234);
        for (int i = 0; i < grainFrames; ++i)
        {
            juce::Image tile (juce::Image::ARGB, size, size, true);
            juce::Image::BitmapData data (tile, juce::Image::BitmapData::writeOnly);
            for (int y = 0; y < size; ++y)
                for (int x = 0; x < size; ++x)
                {
                    const float v = (r.nextFloat() + r.nextFloat() + r.nextFloat()) / 3.0f - 0.5f;
                    auto colour = v < 0.0f ? juce::Colours::black : juce::Colours::white;
                    float alpha = v < 0.0f ? std::min (1.0f, -v * 2.2f) * 0.10f : std::min (1.0f, v * 2.2f) * 0.22f;
                    if (r.nextFloat() < 0.35f)
                    {
                        colour = colour.interpolatedWith (juce::Colour::fromHSV (r.nextFloat(), 0.9f, v < 0.0f ? 0.35f : 1.0f, 1.0f), 0.6f);
                        alpha *= 1.3f;
                    }
                    juce::PixelARGB p ((juce::uint8) juce::roundToInt (alpha * 255.0f), colour.getRed(), colour.getGreen(), colour.getBlue());
                    p.premultiply();
                    *reinterpret_cast<juce::PixelARGB*> (data.getPixelPointer (x, y)) = p;
                }
            tiles.push_back (tile);
        }
    }
    return tiles[(size_t) grainFrame];
}

void FilmOverlay::paint (juce::Graphics& g)
{
    const float scale = g.getInternalContext().getPhysicalPixelScaleFactor();

    // Tiled by hand: whole-pixel image copies are much cheaper than a tiled image fill.
    const auto& tile = grainTile (scale);
    const int step = (int) grainTileSize;
    for (int y = (int) grainOffset.y - step; y < getHeight(); y += step)
        for (int x = (int) grainOffset.x - step; x < getWidth(); x += step)
            g.drawImageTransformed (tile, juce::AffineTransform::scale (1.0f / scale).translated ((float) x, (float) y));

    g.setColour (juce::Colours::white.withAlpha (flicker));
    g.fillAll();

    for (auto& s : scratches)
    {
        g.setColour (juce::Colours::black.withAlpha (0.14f * (0.3f + 0.4f * random.nextFloat())));
        g.fillRect (juce::Rectangle<float> (s.x, 0.0f, 0.6f, (float) getHeight()));
    }

    for (auto& d : dust)
    {
        if (d.hair)
        {
            g.setColour (juce::Colours::black.withAlpha (0.6f));
            g.strokePath (d.shape, juce::PathStrokeType (0.55f));
        }
        else
        {
            g.setColour (d.light ? juce::Colours::white.withAlpha (0.8f) : juce::Colours::black.withAlpha (0.75f));
            g.fillPath (d.shape);
        }
    }

    paintDetails (g);
}

// Viewfinder corners, edge print along the sides and a running timecode.
void FilmOverlay::paintDetails (juce::Graphics& g)
{
    const float w = (float) getWidth(), h = (float) getHeight();
    constexpr float inset = 9.0f, arm = 14.0f;

    g.setColour (juce::Colours::black.withAlpha (0.45f));
    auto bar = [&] (float x1, float y1, float x2, float y2)
    {
        g.fillRect (juce::Rectangle<float>::leftTopRightBottom (std::min (x1, x2), std::min (y1, y2), std::max (x1, x2), std::max (y1, y2)));
    };
    for (auto corner : { juce::Point<float> (inset, inset), { w - inset, inset }, { inset, h - inset }, { w - inset, h - inset } })
    {
        // Each arm is one pixel thick and points into the window.
        const float dx = corner.x < w / 2.0f ? 1.0f : -1.0f, dy = corner.y < h / 2.0f ? 1.0f : -1.0f;
        bar (corner.x, corner.y, corner.x + arm * dx, corner.y + dy);
        bar (corner.x, corner.y, corner.x + dx, corner.y + arm * dy);
    }

    // Edge print, running down the sides like the markings on a film strip.
    g.setColour (juce::Colour (0xff9c9c9c));
    g.setFont (theme::display (5.5f).withExtraKerningFactor (0.35f));
    const auto dash = "  " + emDash() + "  ";
    auto edgeText = [&] (const juce::String& text, float x, float y)
    {
        juce::Graphics::ScopedSaveState state (g);
        g.addTransform (juce::AffineTransform::rotation (juce::MathConstants<float>::halfPi).translated (x, y));
        g.drawSingleLineText (text, 0, 0);
    };
    edgeText ("FORTYVOLTS  40V" + dash + "KITNAP" + dash + "5219" + dash + "017", 8.0f, 120.0f);
    edgeText ("018" + dash + "019" + dash + "020" + dash + "021", w - 14.0f, 300.0f);

    // Recording light, frame rate and the timecode since the window opened.
    const auto font = theme::display (7.0f).withExtraKerningFactor (0.18f);
    const auto seconds = frame / framesPerSecond;
    const auto tc = juce::String::formatted ("TC %02d:%02d:%02d:%02d", (int) (seconds / 3600), (int) (seconds / 60 % 60),
                                             (int) (seconds % 60), (int) (frame % framesPerSecond));
    const juce::String fps ("24 FPS");
    const float tcWidth = juce::GlyphArrangement::getStringWidth (font, tc);
    const float fpsWidth = juce::GlyphArrangement::getStringWidth (font, fps);
    float x = w - 28.0f - tcWidth;
    g.setFont (font);
    g.setColour (theme::inkDim);
    g.drawText (tc, juce::Rectangle<float> (x, 6.0f, tcWidth + 2.0f, 12.0f), juce::Justification::centredLeft);
    x -= 8.0f + fpsWidth;
    g.drawText (fps, juce::Rectangle<float> (x, 6.0f, fpsWidth + 2.0f, 12.0f), juce::Justification::centredLeft);
    x -= 8.0f + 5.0f;
    g.setColour (theme::ink.withAlpha (frame % framesPerSecond < framesPerSecond / 2 ? 1.0f : 0.15f));
    g.fillEllipse (x, 9.5f, 5.0f, 5.0f);
}

//==============================================================================
void FilmCache::paint (juce::Graphics& g)
{
    scale = g.getInternalContext().getPhysicalPixelScaleFactor();
    const auto bounds = owner.getLocalBounds();
    const auto imageBounds = bounds * scale;

    if (raw.isNull() || raw.getBounds() != imageBounds)
    {
        raw = juce::Image (juce::Image::ARGB, std::max (1, imageBounds.getWidth()), std::max (1, imageBounds.getHeight()), false);
        graded = juce::Image (juce::Image::ARGB, raw.getWidth(), raw.getHeight(), false);
        valid.clear();
        gradedAll = false;
    }

    juce::RectangleList<int> dirty (bounds);
    dirty.subtract (valid);
    if (! dirty.isEmpty())
    {
        juce::Graphics imageGraphics (raw);
        auto& context = imageGraphics.getInternalContext();
        context.addTransform (juce::AffineTransform::scale (scale));
        for (auto& r : valid)
            context.excludeClipRectangle (r);
        owner.paintEntireComponent (imageGraphics, true);
    }
    valid = bounds;

    const bool on = film.isOn();
    if (on && ! gradedAll)
    {
        grade (raw.getBounds());
        gradedAll = true;
    }
    else if (on)
    {
        for (auto& r : dirty)
            grade (r.expanded (2) * scale);
    }
    else
    {
        gradedAll = false;
    }

    const auto offset = on ? film.weaveOffset (scale) : juce::Point<int>();
    g.setColour (juce::Colours::black);
    g.drawImageTransformed (on ? graded : raw,
                            juce::AffineTransform::translation ((float) offset.x, (float) offset.y).scaled (1.0f / scale), false);
}

// Colour fringe (red pulled from the left, blue from the right), warm whites and navy blacks.
// Integer maths with tables indexed by the pixel's brightness (r + g + b), as this runs on every
// pixel that changes.
void FilmCache::grade (juce::Rectangle<int> area)
{
    area = area.getIntersection (raw.getBounds());
    if (area.isEmpty())
        return;

    struct Tables
    {
        int liftR[766], liftG[766], liftB[766], keepG[766], keepB[766]; // keep: out of 256
        Tables()
        {
            for (int sum = 0; sum <= 765; ++sum)
            {
                const float light = (float) sum / 765.0f;
                liftR[sum] = juce::roundToInt (navy.getRed() * shadowLift * (1.0f - light));
                liftG[sum] = juce::roundToInt (navy.getGreen() * shadowLift * (1.0f - light));
                liftB[sum] = juce::roundToInt (navy.getBlue() * shadowLift * (1.0f - light));
                keepG[sum] = juce::roundToInt (256.0f * (1.0f - 0.035f * light));
                keepB[sum] = juce::roundToInt (256.0f * (1.0f - 0.065f * light));
            }
        }
    };
    static const Tables t;
    constexpr int fringe256 = (int) (fringe * 256.0f);

    const int shift = std::max (1, juce::roundToInt (fringeShift * scale));
    const int width = raw.getWidth();
    const juce::Image::BitmapData src (raw, juce::Image::BitmapData::readOnly);
    juce::Image::BitmapData dst (graded, area.getX(), area.getY(), area.getWidth(), area.getHeight(), juce::Image::BitmapData::writeOnly);
    jassert (src.pixelStride == (int) sizeof (juce::PixelARGB));

    for (int y = 0; y < area.getHeight(); ++y)
    {
        const auto* line = reinterpret_cast<const juce::PixelARGB*> (src.getLinePointer (area.getY() + y));
        auto* out = reinterpret_cast<juce::PixelARGB*> (dst.getLinePointer (y));
        for (int x = 0; x < area.getWidth(); ++x)
        {
            const int sx = area.getX() + x;
            const auto& c = line[sx];
            const int left = line[std::max (0, sx - shift)].getRed(), right = line[std::min (width - 1, sx + shift)].getBlue();
            const int red = c.getRed() + (((left - c.getRed()) * fringe256) >> 8);
            const int green = c.getGreen();
            const int blue = c.getBlue() + (((right - c.getBlue()) * fringe256) >> 8);
            const int sum = juce::jlimit (0, 765, red + green + blue);
            out[x].setARGB (255,
                            (juce::uint8) std::min (255, red + t.liftR[sum]),
                            (juce::uint8) std::min (255, ((green + t.liftG[sum]) * t.keepG[sum]) >> 8),
                            (juce::uint8) std::min (255, ((blue + t.liftB[sum]) * t.keepB[sum]) >> 8));
        }
    }
}
