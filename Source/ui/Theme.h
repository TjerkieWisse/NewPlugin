#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include "analysis/DrumAnalyzer.h"

namespace kitnap::theme
{
inline const juce::Colour background { 0xff0b0c11 };
inline const juce::Colour backgroundTop { 0xff12131b };
inline const juce::Colour panel { 0xff161822 };
inline const juce::Colour panelEdge { 0xff262938 };
inline const juce::Colour text { 0xfff2f3f7 };
inline const juce::Colour textDim { 0xff8a8fa3 };
inline const juce::Colour accent { 0xffff3d6e };

inline juce::Colour forClass (DrumClass c)
{
    switch (c)
    {
        case DrumClass::kick:    return juce::Colour (0xffff3d6e);
        case DrumClass::snare:   return juce::Colour (0xffff9f1c);
        case DrumClass::toms:    return juce::Colour (0xffa970ff);
        case DrumClass::hihat:   return juce::Colour (0xff2ee6c5);
        case DrumClass::cymbals: return juce::Colour (0xff3da9ff);
    }
    return accent;
}

// Middle dot separator, spelled out so it survives any source-file encoding.
inline juce::String dot() { return juce::String::charToString ((juce::juce_wchar) 0x00b7); }

inline juce::Font font (float height, bool bold = false)
{
    return juce::Font (juce::FontOptions (height, bold ? juce::Font::bold : juce::Font::plain));
}
} // namespace kitnap::theme
