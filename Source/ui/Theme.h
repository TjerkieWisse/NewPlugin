#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include "BinaryData.h"

// FortyVolts "Halo" look: white cards with hairline borders on a light ground, black ink, and one
// flowing aurora gradient for whatever is alive (the song, the pad that plays, the volume). Geist
// for text, Geist Mono for small details, Michroma for the name.
namespace kitnap::theme
{
inline const juce::Colour background { 0xfffafafa };
inline const juce::Colour card { 0xffffffff };
inline const juce::Colour border { 0xffececec };
inline const juce::Colour ink { 0xff0a0a0a };
inline const juce::Colour inkDim { 0xff8a8a8a };
inline const juce::Colour inkFaint { 0xffababab }; // time ruler
inline const juce::Colour line { 0xffefefef };     // knob track, quiet waveform
inline const juce::Colour fill { 0xfff2f2f2 };     // sound shapes
inline const juce::Colour error { 0xffd0021b };

// The aurora's colours, repeating: violet, sky, mint, amber, pink.
inline const juce::Colour aurora[] { juce::Colour (0xff7b61ff), juce::Colour (0xff00b7ff), juce::Colour (0xff00d9a0),
                                    juce::Colour (0xffffb547), juce::Colour (0xffff5c8a) };
constexpr double auroraSeconds = 14.0 / 6.0; // time for the flow to move one colour along

constexpr float cardRadius = 20.0f;
constexpr float tileRadius = 18.0f;

// A horizontal aurora gradient across [x0, x1] at height y, one colour every `spacing` pixels,
// flowed `flow` colours to the left (it repeats, so any flow value works).
inline juce::ColourGradient auroraGradient (float x0, float x1, float y, float spacing, double flow)
{
    const int n = (int) std::size (aurora);
    const double shift = std::fmod (flow, (double) n);
    const float first = x0 - (float) (shift - std::floor (shift)) * spacing;
    int index = (int) std::floor (shift);
    juce::ColourGradient g (aurora[index % n], first, y, aurora[index % n], x1 + spacing, y, false);
    g.clearColours();
    const float length = x1 + spacing - first;
    for (float x = first; x <= x1 + spacing + 0.5f; x += spacing, ++index)
        g.addColour (juce::jlimit (0.0, 1.0, (double) ((x - first) / length)), aurora[index % n]);
    return g;
}

// The three-colour version used for small things (a sound's shape, the volume arc).
inline juce::ColourGradient auroraShort (juce::Point<float> from, juce::Point<float> to)
{
    juce::ColourGradient g (aurora[0], from, aurora[2], to, false);
    g.addColour (0.5, aurora[1]);
    return g;
}

namespace detail
{
inline juce::Typeface::Ptr load (const char* data, int size)
{
    return juce::Typeface::createSystemTypefaceFor (data, (size_t) size);
}
} // namespace detail

// Wide display face, for the product name.
inline juce::Font display (float height)
{
    static auto face = detail::load (BinaryData::MichromaRegular_ttf, BinaryData::MichromaRegular_ttfSize);
    return juce::Font (juce::FontOptions (face).withHeight (height)).withExtraKerningFactor (0.18f);
}

enum class Weight { light, medium };

// Text face.
inline juce::Font font (float height, Weight weight)
{
    static auto light = detail::load (BinaryData::GeistLight_ttf, BinaryData::GeistLight_ttfSize);
    static auto medium = detail::load (BinaryData::GeistMedium_ttf, BinaryData::GeistMedium_ttfSize);
    auto face = weight == Weight::light ? light : medium;
    return juce::Font (juce::FontOptions (face).withHeight (height));
}

// Small details: timings, notes, counts.
inline juce::Font mono (float height = 11.0f)
{
    static auto face = detail::load (BinaryData::GeistMonoRegular_ttf, BinaryData::GeistMonoRegular_ttfSize);
    return juce::Font (juce::FontOptions (face).withHeight (height)).withExtraKerningFactor (0.02f);
}

// The 40V FORTYVOLTS lockup, in black.
inline std::unique_ptr<juce::Drawable> logo()
{
    return juce::Drawable::createFromImageData (BinaryData::fortyvoltslockup_svg, BinaryData::fortyvoltslockup_svgSize);
}

// White card with a hairline border and, optionally, a soft low shadow.
inline void paintCard (juce::Graphics& g, juce::Rectangle<float> r, float radius, bool shadow = true)
{
    if (shadow)
    {
        juce::Path p;
        p.addRoundedRectangle (r.reduced (16.0f, 4.0f).translated (0.0f, 14.0f), radius);
        juce::DropShadow (juce::Colours::black.withAlpha (0.08f), 30, {}).drawForPath (g, p);
    }
    g.setColour (card);
    g.fillRoundedRectangle (r, radius);
    g.setColour (border);
    g.drawRoundedRectangle (r.reduced (0.5f), radius, 1.0f);
}

// Middle dot separator, spelled out so it survives any source-file encoding.
inline juce::String dot() { return juce::String::charToString ((juce::juce_wchar) 0x00b7); }
} // namespace kitnap::theme
