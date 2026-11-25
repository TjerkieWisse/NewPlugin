#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include "licence/Licence.h"

// The licence, over the whole window: a card to paste the key into and activate (with Buy and Find
// my key), or, once activated, which key and computer it is, with Deactivate. Close, or a click
// beside the card, hides it.
class LicencePanel : public juce::Component,
                     private juce::ChangeListener
{
public:
    LicencePanel();
    ~LicencePanel() override;

    void paint (juce::Graphics&) override;
    void resized() override;
    void mouseUp (const juce::MouseEvent&) override;
    void visibilityChanged() override;

private:
    void changeListenerCallback (juce::ChangeBroadcaster*) override;
    void update();
    void activate();
    juce::Rectangle<int> cardBounds() const;
    juce::Rectangle<int> messageArea() const;

    juce::SharedResourcePointer<kitnap::LicenceService> licence;
    juce::TextEditor keyField;
    juce::TextButton closeButton { "Close" }, activateButton { "Activate" }, buyButton { "Buy KITNAP" },
        findKeyButton { "Find my key" }, checkButton { "Check now" }, deactivateButton { "Deactivate this computer" },
        manageButton { "Your computers" };
    kitnap::LicenceState shownState = kitnap::LicenceState::none;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (LicencePanel)
};
