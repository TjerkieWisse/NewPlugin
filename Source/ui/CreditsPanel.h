#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

// The credits and licences (Resources/Credits.txt), over the whole window: a card with the text to
// scroll through. Close, or a click beside the card, hides it again.
class CreditsPanel : public juce::Component
{
public:
    CreditsPanel();

    void paint (juce::Graphics&) override;
    void resized() override;
    void mouseUp (const juce::MouseEvent&) override;

private:
    juce::Rectangle<int> cardBounds() const;

    juce::TextEditor text;
    juce::TextButton closeButton { "Close" };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (CreditsPanel)
};
