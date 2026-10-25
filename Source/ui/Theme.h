#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include "BinaryData.h"

// FortyVolts "Orbit" look: white cards with hairline borders on a light ground, black ink, one
// electric-blue accent, Geist for text, Geist Mono for small details, Michroma for the name.
namespace kitnap::theme
{
inline const juce::Colour background { 0xfffbfbfb };
inline const juce::Colour glow { 0xffeef1ff };      // soft blue light behind the ring
inline const juce::Colour card { 0xffffffff };
inline const juce::Colour border { 0xffececec };
inline const juce::Colour ink { 0xff0a0a0a };
inline const juce::Colour inkDim { 0xff8a8a8a };
inline const juce::Colour line { 0xffededed };      // quiet tracks and rings
inline const juce::Colour fill { 0xfff2f2f2 };      // sound shapes
inline const juce::Colour accent { 0xff2f5bff };
inline const juce::Colour accentSoft { 0xff9db0ff };
inline const juce::Colour accentBright { 0xff4f74ff };
inline const juce::Colour activeRow { 0xfff6f8ff };
inline const juce::Colour activeBorder { 0xffc9d4ff };
inline const juce::Colour error { 0xffd0021b };

constexpr float cardRadius = 24.0f;
constexpr float rowRadius = 16.0f;

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

// White card with a hairline border and a soft, low shadow.
inline void paintCard (juce::Graphics& g, juce::Rectangle<float> r, float radius, bool shadow = true)
{
    if (shadow)
    {
        juce::Path p;
        p.addRoundedRectangle (r.reduced (12.0f, 0.0f).translated (0.0f, 14.0f), radius);
        juce::DropShadow (juce::Colour (0xff1e285a).withAlpha (0.10f), 36, {}).drawForPath (g, p);
    }
    g.setColour (card);
    g.fillRoundedRectangle (r, radius);
    g.setColour (border);
    g.drawRoundedRectangle (r.reduced (0.5f), radius, 1.0f);
}

// Middle dot separator, spelled out so it survives any source-file encoding.
inline juce::String dot() { return juce::String::charToString ((juce::juce_wchar) 0x00b7); }
} // namespace kitnap::theme
