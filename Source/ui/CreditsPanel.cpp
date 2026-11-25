#include "CreditsPanel.h"
#include "Theme.h"

using namespace kitnap;

CreditsPanel::CreditsPanel()
{
    text.setMultiLine (true, false);
    text.setReadOnly (true);
    text.setCaretVisible (false);
    text.setScrollbarsShown (true);
    text.setColour (juce::TextEditor::backgroundColourId, juce::Colours::transparentBlack);
    text.setColour (juce::TextEditor::outlineColourId, juce::Colours::transparentBlack);
    text.setColour (juce::TextEditor::focusedOutlineColourId, juce::Colours::transparentBlack);
    text.setColour (juce::TextEditor::textColourId, theme::ink);
    text.setColour (juce::TextEditor::highlightColourId, theme::line);
    text.setFont (theme::mono (12.0f));
    text.setText (juce::String::createStringFromData (BinaryData::Credits_txt, BinaryData::Credits_txtSize), false);
    addAndMakeVisible (text);

    closeButton.onClick = [this] { setVisible (false); };
    addAndMakeVisible (closeButton);
}

juce::Rectangle<int> CreditsPanel::cardBounds() const
{
    return getLocalBounds().reduced (24).withTrimmedTop (52);
}

void CreditsPanel::paint (juce::Graphics& g)
{
    g.fillAll (theme::ink.withAlpha (0.18f));
    const auto card = cardBounds().toFloat();
    theme::paintCard (g, card, theme::cardRadius);
    g.setColour (theme::ink);
    g.setFont (theme::font (20.0f, theme::Weight::medium));
    g.drawText ("Credits & licences", card.reduced (32.0f, 0.0f).withY (card.getY() + 26.0f).withHeight (28.0f),
                juce::Justification::centredLeft);
}

void CreditsPanel::resized()
{
    const auto card = cardBounds();
    closeButton.setBounds (card.getRight() - 32 - 84, card.getY() + 22, 84, 36);
    text.setBounds (card.reduced (24, 0).withTrimmedTop (78).withTrimmedBottom (20));
}

void CreditsPanel::mouseUp (const juce::MouseEvent& e)
{
    if (! cardBounds().contains (e.getPosition()))
        setVisible (false);
}
