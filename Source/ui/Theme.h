#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include "BinaryData.h"

// FortyVolts look: black ink on light grey, thin black outlines with small rounded corners,
// Michroma for headings and labels, IBM Plex Sans for everything else.
namespace kitnap::theme
{
inline const juce::Colour background { 0xfff2f2f2 };
inline const juce::Colour panel { 0xffffffff };
inline const juce::Colour ink { 0xff000000 };
inline const juce::Colour inkDim { 0xff6e6e6e };
inline const juce::Colour line { 0xffd6d6d6 };   // quiet guides and tracks
inline const juce::Colour error { 0xffd0021b };

constexpr float outline = 1.5f; // outline width of panels, pads and buttons
constexpr float radius = 8.0f;  // corner radius, matching the 40V badge

namespace detail
{
inline juce::Typeface::Ptr load (const char* data, int size)
{
    return juce::Typeface::createSystemTypefaceFor (data, (size_t) size);
}
} // namespace detail

// Wide display face, for headings, labels and buttons. Set in capitals with a little extra spacing.
inline juce::Font display (float height)
{
    static auto face = detail::load (BinaryData::MichromaRegular_ttf, BinaryData::MichromaRegular_ttfSize);
    return juce::Font (juce::FontOptions (face).withHeight (height)).withExtraKerningFactor (0.08f);
}

// Text face.
inline juce::Font font (float height, bool medium = false)
{
    static auto regular = detail::load (BinaryData::IBMPlexSansRegular_ttf, BinaryData::IBMPlexSansRegular_ttfSize);
    static auto bold = detail::load (BinaryData::IBMPlexSansMedium_ttf, BinaryData::IBMPlexSansMedium_ttfSize);
    return juce::Font (juce::FontOptions (medium ? bold : regular).withHeight (height));
}

// The 40V FORTYVOLTS lockup, in black.
inline std::unique_ptr<juce::Drawable> logo()
{
    return juce::Drawable::createFromImageData (BinaryData::fortyvoltslockup_svg, BinaryData::fortyvoltslockup_svgSize);
}

// Middle dot separator, spelled out so it survives any source-file encoding.
inline juce::String dot() { return juce::String::charToString ((juce::juce_wchar) 0x00b7); }
} // namespace kitnap::theme
