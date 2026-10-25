#include "WaveformView.h"
#include "Theme.h"

using namespace kitnap;

namespace
{
constexpr float glowMargin = 40.0f;      // room around the waveform for its glow
constexpr float glowOpacity = 0.35f;

juce::String duration (double seconds)
{
    const int s = juce::roundToInt (seconds);
    return juce::String (s / 60) + ":" + juce::String (s % 60).paddedLeft ('0', 2);
}

// Three passes of a box blur over a single-channel image, close to a Gaussian blur.
void blur (juce::Image& image, int radius)
{
    if (radius < 1)
        return;
    const int w = image.getWidth(), h = image.getHeight();
    juce::Image::BitmapData data (image, juce::Image::BitmapData::readWrite);
    std::vector<int> line ((size_t) std::max (w, h));

    auto pass = [&] (int count, int length, auto pixel)
    {
        for (int i = 0; i < count; ++i)
        {
            for (int j = 0; j < length; ++j)
                line[(size_t) j] = *pixel (i, j);
            int sum = 0;
            for (int j = -radius; j <= radius; ++j)
                sum += line[(size_t) juce::jlimit (0, length - 1, j)];
            for (int j = 0; j < length; ++j)
            {
                *pixel (i, j) = (juce::uint8) (sum / (2 * radius + 1));
                sum += line[(size_t) juce::jlimit (0, length - 1, j + radius + 1)]
                     - line[(size_t) juce::jlimit (0, length - 1, j - radius)];
            }
        }
    };
    for (int k = 0; k < 3; ++k)
    {
        pass (h, w, [&] (int y, int x) { return data.getPixelPointer (x, y); });
        pass (w, h, [&] (int x, int y) { return data.getPixelPointer (x, y); });
    }
}
} // namespace

juce::Rectangle<float> WaveformView::waveArea() const
{
    return { 24.0f, 84.0f, (float) getWidth() - 48.0f, 116.0f };
}

// The hit count, or the analysis' percentage and step, at the top right.
juce::Rectangle<float> WaveformView::progressArea() const
{
    return { (float) getWidth() - 224.0f, 8.0f, 200.0f, 62.0f };
}

void WaveformView::kitChanged()
{
    const auto& kit = processor.getKit();
    if (kit.status == KitState::Status::analysing && lastStatus != KitState::Status::analysing)
        shownProgress = 0.0f;
    lastStatus = kit.status;

    // The waveform's outline: per pixel column the song's highest and lowest point, scaled so the
    // loudest moment fills the height.
    shape.clear();
    mask = glowMask = {};
    const int buckets = (int) kit.overview.size() / 2;
    const auto area = waveArea();
    const int columns = (int) area.getWidth();
    if (buckets > 0 && columns > 1)
    {
        std::vector<float> low ((size_t) columns), high ((size_t) columns);
        float top = 1.0e-6f;
        for (int c = 0; c < columns; ++c)
        {
            const int b0 = c * buckets / columns, b1 = std::max (b0 + 1, (c + 1) * buckets / columns);
            float lo = 0.0f, hi = 0.0f;
            for (int b = b0; b < b1; ++b)
            {
                lo = std::min (lo, kit.overview[(size_t) b * 2]);
                hi = std::max (hi, kit.overview[(size_t) b * 2 + 1]);
            }
            low[(size_t) c] = lo;
            high[(size_t) c] = hi;
            top = std::max ({ top, -lo, hi });
        }

        const float mid = area.getCentreY(), half = area.getHeight() / 2.0f;
        auto x = [&] (int c) { return area.getX() + area.getWidth() * (float) c / (float) (columns - 1); };
        shape.startNewSubPath (x (0), mid - std::max (0.6f, high[0] / top * half));
        for (int c = 1; c < columns; ++c)
            shape.lineTo (x (c), mid - std::max (0.6f, high[(size_t) c] / top * half));
        for (int c = columns - 1; c >= 0; --c)
            shape.lineTo (x (c), mid + std::max (0.6f, -low[(size_t) c] / top * half));
        shape.closeSubPath();
    }
    repaint();
}

void WaveformView::buildMasks (float scale)
{
    maskScale = scale;
    const auto area = waveArea();
    auto render = [&] (juce::Rectangle<float> r, juce::Image::PixelFormat format)
    {
        juce::Image image (format, juce::roundToInt (r.getWidth() * scale), juce::roundToInt (r.getHeight() * scale), true);
        juce::Graphics g (image);
        g.addTransform (juce::AffineTransform::translation (-r.getX(), -r.getY()).scaled (scale));
        g.setColour (juce::Colours::white);
        g.fillPath (shape);
        return image;
    };
    glowArea = area.expanded (glowMargin);
    mask = render (glowArea, juce::Image::SingleChannel);
    glowMask = mask.createCopy();
    blur (glowMask, juce::roundToInt (13.0f * scale));
    frame = juce::Image (juce::Image::ARGB, mask.getWidth(), mask.getHeight(), true);
}

void WaveformView::tick (double seconds)
{
    time += seconds;
    shownProgress += 0.15f * (processor.getProgress() - shownProgress);

    // Only the waveform moves, plus the percentage while analysing and the card's edge while a file
    // hovers over it.
    if (dragHover)
        repaint();
    else
        repaint (waveArea().expanded (0.0f, glowMargin).getSmallestIntegerContainer().getIntersection (getLocalBounds().reduced (2)));
    if (processor.getKit().status == KitState::Status::analysing)
        repaint (progressArea().getSmallestIntegerContainer());
}

void WaveformView::mouseUp (const juce::MouseEvent& e)
{
    if (e.mouseWasClicked() && processor.getKit().status != KitState::Status::analysing && onBrowse)
        onBrowse();
}

void WaveformView::paint (juce::Graphics& g)
{
    const auto bounds = getLocalBounds().toFloat();
    const auto& kit = processor.getKit();
    const bool analysing = kit.status == KitState::Status::analysing;
    const float w = bounds.getWidth();

    g.setColour (theme::card);
    g.fillRoundedRectangle (bounds, theme::cardRadius);
    if (dragHover)
    {
        g.setGradientFill (theme::auroraGradient (0.0f, w, 0.0f, w / 2.0f, time / theme::auroraSeconds));
        g.drawRoundedRectangle (bounds.reduced (0.75f), theme::cardRadius, 1.5f);
    }
    else
    {
        g.setColour (theme::border);
        g.drawRoundedRectangle (bounds.reduced (0.5f), theme::cardRadius, 1.0f);
    }

    // Name and details on the left.
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
    g.drawText (title, juce::Rectangle<float> (24.0f, 16.0f, w - 260.0f, 22.0f), juce::Justification::centredLeft, true);
    g.setColour (kit.status == KitState::Status::error ? theme::error : theme::inkDim);
    g.setFont (theme::mono (11.0f));
    g.drawText (details, juce::Rectangle<float> (24.0f, 40.0f, w - 260.0f, 18.0f), juce::Justification::centredLeft, true);

    // How many hits, or how far the analysis is, on the right.
    juce::String big, small;
    if (analysing)
    {
        const auto stage = processor.getStage();
        big = juce::String (juce::roundToInt (juce::jlimit (0.0f, 1.0f, shownProgress) * 100.0f)) + "%";
        small = stage == KitState::Stage::separating ? "separating drums"
              : stage == KitState::Stage::splitting  ? "splitting the kit"
              : stage == KitState::Stage::reading    ? "reading"
                                                     : "cutting the sounds";
    }
    else if (kit.status == KitState::Status::ready)
    {
        big = juce::String ((int) kit.hits.size());
        small = kit.hits.size() == 1 ? "hit found" : "hits found";
    }
    if (big.isNotEmpty())
    {
        g.setColour (theme::ink);
        g.setFont (theme::font (34.0f, theme::Weight::light));
        g.drawText (big, progressArea().withHeight (42.0f), juce::Justification::centredRight);
        g.setColour (theme::inkDim);
        g.setFont (theme::mono (11.0f));
        g.drawText (small, progressArea().withTrimmedTop (44.0f), juce::Justification::centredRight);
    }

    paintWave (g);
    paintRuler (g);
}

void WaveformView::paintWave (juce::Graphics& g)
{
    const auto& kit = processor.getKit();
    const auto area = waveArea();

    if (shape.isEmpty())
    {
        // Drop zone: a row of short ticks with the invitation in the middle.
        const auto text = juce::Rectangle<float> (300.0f, area.getHeight()).withCentre (area.getCentre());
        g.setColour (juce::Colour (0xffe4e4e4));
        for (float x = area.getX(); x <= area.getRight(); x += 6.0f)
            if (x < text.getX() || x > text.getRight())
                g.fillRect (juce::Rectangle<float> (x, area.getCentreY() - 2.0f, 1.5f, 4.0f));
        g.setColour (theme::ink);
        g.setFont (theme::font (19.0f, theme::Weight::light));
        g.drawText ("Drop a song", text.withHeight (26.0f).withCentre (area.getCentre().translated (0.0f, -10.0f)), juce::Justification::centred);
        g.setColour (theme::inkDim);
        g.setFont (theme::mono (11.0f));
        g.drawText ("or click to browse", text.withHeight (18.0f).withCentre (area.getCentre().translated (0.0f, 16.0f)), juce::Justification::centred);
        return;
    }

    const float scale = g.getInternalContext().getPhysicalPixelScaleFactor();
    if (! mask.isValid() || ! juce::approximatelyEqual (maskScale, scale))
        buildMasks (scale);

    const bool analysing = kit.status == KitState::Status::analysing;
    const float done = analysing ? juce::jlimit (0.0f, 1.0f, shownProgress) : 1.0f;
    const float litRight = area.getX() + area.getWidth() * done;
    composeFrame (scale, litRight, analysing ? juce::jmin (1.0f, done * 20.0f) : 1.0f);
    g.drawImage (frame, glowArea);
}

// The waveform and its glow in the flowing aurora (up to `litRight`; plain grey after it), blended
// straight into one picture: every pixel column has one aurora colour, and the sharp outline sits
// on top of the blurred one.
void WaveformView::composeFrame (float scale, float litRight, float glowStrength)
{
    const auto area = waveArea();
    const int w = frame.getWidth(), h = frame.getHeight();
    const auto aurora = theme::auroraGradient (glowArea.getX(), glowArea.getRight(), 0.0f,
                                               area.getWidth() / 2.0f, time / theme::auroraSeconds);
    const int litColumns = juce::jlimit (0, w, juce::roundToInt ((litRight - glowArea.getX()) * scale));
    const auto grey = theme::line;

    columnColours.resize ((size_t) w);
    for (int x = 0; x < w; ++x)
    {
        const float pos = (glowArea.getX() + (float) x / scale - aurora.point1.x) / (aurora.point2.x - aurora.point1.x);
        columnColours[(size_t) x] = x < litColumns ? aurora.getColourAtPosition (juce::jlimit (0.0, 1.0, (double) pos)) : grey;
    }

    const juce::Image::BitmapData sharp (mask, juce::Image::BitmapData::readOnly);
    const juce::Image::BitmapData soft (glowMask, juce::Image::BitmapData::readOnly);
    juce::Image::BitmapData out (frame, juce::Image::BitmapData::writeOnly);
    const int glow = juce::roundToInt (glowOpacity * glowStrength * 256.0f);
    for (int y = 0; y < h; ++y)
    {
        const auto* s = sharp.getLinePointer (y);
        const auto* b = soft.getLinePointer (y);
        auto* o = reinterpret_cast<juce::PixelARGB*> (out.getLinePointer (y));
        for (int x = 0; x < w; ++x)
        {
            const int a = s[x], glowAlpha = x < litColumns ? (b[x] * glow) >> 8 : 0;
            const int alpha = a + ((glowAlpha * (255 - a)) >> 8);
            const auto c = columnColours[(size_t) x];
            o[x].setARGB ((juce::uint8) alpha, (juce::uint8) ((c.getRed() * alpha) / 255),
                          (juce::uint8) ((c.getGreen() * alpha) / 255), (juce::uint8) ((c.getBlue() * alpha) / 255));
        }
    }
}

void WaveformView::paintRuler (juce::Graphics& g)
{
    const double length = processor.getKit().sourceLengthSeconds;
    if (shape.isEmpty() || length <= 0.0)
        return;

    // Four times along the song: its start, its end, and two in between.
    const auto area = waveArea();
    g.setColour (theme::inkFaint);
    g.setFont (theme::mono (10.0f));
    for (int k = 0; k <= 3; ++k)
    {
        const float x = area.getX() + area.getWidth() * (float) k / 3.0f;
        const auto label = duration (length * k / 3.0);
        const auto justification = k == 0 ? juce::Justification::centredLeft
                                 : k == 3 ? juce::Justification::centredRight : juce::Justification::centred;
        const auto box = k == 0 ? juce::Rectangle<float> (x, 0.0f, 60.0f, 14.0f)
                       : k == 3 ? juce::Rectangle<float> (x - 60.0f, 0.0f, 60.0f, 14.0f)
                                : juce::Rectangle<float> (60.0f, 14.0f).withCentre ({ x, 7.0f });
        g.drawText (label, box.withY (area.getBottom() + 9.0f), justification);
    }
}
