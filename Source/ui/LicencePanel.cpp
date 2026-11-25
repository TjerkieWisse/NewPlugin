#include "LicencePanel.h"
#include "Theme.h"

using namespace kitnap;

namespace
{
constexpr int pad = 32;

void drawWrapped (juce::Graphics& g, const juce::String& text, juce::Font font, juce::Colour colour, juce::Rectangle<float> area)
{
    juce::AttributedString s;
    s.append (text, font, colour);
    s.setWordWrap (juce::AttributedString::byWord);
    s.setLineSpacing (3.0f);
    juce::TextLayout layout;
    layout.createLayout (s, area.getWidth());
    layout.draw (g, area);
}

juce::String daysAgo (juce::Time t)
{
    const int days = (int) std::floor ((juce::Time::getCurrentTime() - t).inDays());
    return days <= 0 ? juce::String ("today") : days == 1 ? juce::String ("yesterday") : juce::String (days) + " days ago";
}
} // namespace

LicencePanel::LicencePanel()
{
    keyField.setFont (theme::mono (14.0f));
    keyField.setJustification (juce::Justification::centredLeft);
    keyField.setIndents (16, 0);
    keyField.setTextToShowWhenEmpty ("Licence key", theme::inkFaint);
    // The field's rounded box is painted by the panel, behind it.
    keyField.setColour (juce::TextEditor::backgroundColourId, juce::Colours::transparentBlack);
    keyField.setColour (juce::TextEditor::outlineColourId, juce::Colours::transparentBlack);
    keyField.setColour (juce::TextEditor::focusedOutlineColourId, juce::Colours::transparentBlack);
    keyField.setColour (juce::TextEditor::textColourId, theme::ink);
    keyField.setColour (juce::TextEditor::highlightColourId, theme::line);
    keyField.setColour (juce::CaretComponent::caretColourId, theme::ink);
    keyField.onReturnKey = [this] { activate(); };
    addChildComponent (keyField);

    activateButton.getProperties().set ("primary", true);
    checkButton.getProperties().set ("primary", true);
    activateButton.onClick = [this] { activate(); };
    checkButton.onClick = [this] { licence->checkNow(); };
    deactivateButton.onClick = [this] { licence->deactivate(); };
    buyButton.onClick = [this] { juce::URL (licence->config().buyUrl).launchInDefaultBrowser(); };
    findKeyButton.onClick = [this] { juce::URL (licence->config().accountUrl).launchInDefaultBrowser(); };
    manageButton.onClick = [this] { juce::URL (licence->config().accountUrl).launchInDefaultBrowser(); };
    closeButton.onClick = [this] { setVisible (false); };
    for (auto* b : { &activateButton, &checkButton, &deactivateButton, &buyButton, &findKeyButton, &manageButton })
        addChildComponent (b);
    addAndMakeVisible (closeButton);

    licence->addChangeListener (this);
    update();
}

LicencePanel::~LicencePanel()
{
    licence->removeChangeListener (this);
}

void LicencePanel::changeListenerCallback (juce::ChangeBroadcaster*)
{
    update();
}

void LicencePanel::visibilityChanged()
{
    update();
    if (isShowing() && keyField.isVisible() && keyField.isEnabled())
        keyField.grabKeyboardFocus();
}

void LicencePanel::activate()
{
    if (keyField.getText().trim().isNotEmpty() && ! licence->isBusy())
        licence->activate (keyField.getText());
}

void LicencePanel::update()
{
    shownState = licence->state();
    const bool busy = licence->isBusy();
    const bool active = shownState == LicenceState::active;
    const bool needsCheck = shownState == LicenceState::needsCheck;
    const bool none = ! active && ! needsCheck;
    const auto& cfg = licence->config();

    keyField.setVisible (none);
    activateButton.setVisible (none);
    buyButton.setVisible (none && cfg.buyUrl.isNotEmpty());
    findKeyButton.setVisible (none && cfg.accountUrl.isNotEmpty());
    checkButton.setVisible (needsCheck);
    deactivateButton.setVisible (active || needsCheck);
    manageButton.setVisible ((active || needsCheck) && cfg.accountUrl.isNotEmpty());

    keyField.setEnabled (! busy);
    for (auto* b : { &activateButton, &checkButton, &deactivateButton })
        b->setEnabled (! busy);
    if (active)
        keyField.clear();

    resized();
    repaint();
}

juce::Rectangle<int> LicencePanel::cardBounds() const
{
    return getLocalBounds().withSizeKeepingCentre (600, 404);
}

juce::Rectangle<int> LicencePanel::messageArea() const
{
    const auto card = cardBounds();
    return { card.getX() + pad, card.getBottom() - 92, card.getWidth() - 2 * pad, 64 };
}

void LicencePanel::paint (juce::Graphics& g)
{
    g.fillAll (theme::ink.withAlpha (0.18f));
    const auto card = cardBounds().toFloat();
    theme::paintCard (g, card, theme::cardRadius);

    const bool active = shownState == LicenceState::active;
    const bool needsCheck = shownState == LicenceState::needsCheck;
    const auto inner = card.reduced ((float) pad, 0.0f);

    g.setColour (theme::ink);
    g.setFont (theme::font (20.0f, theme::Weight::medium));
    g.drawText (active || needsCheck ? "Licence" : "Activate KITNAP", inner.withY (card.getY() + 26.0f).withHeight (28.0f),
                juce::Justification::centredLeft);

    if (keyField.isVisible())
    {
        const auto box = keyField.getBounds().toFloat().reduced (0.5f);
        g.setColour (theme::background);
        g.fillRoundedRectangle (box, 12.0f);
        g.setColour (juce::Colour (0xffe4e4e4));
        g.drawRoundedRectangle (box, 12.0f, 1.0f);
    }

    const juce::String intro = active ? "KITNAP is activated on this computer."
                             : needsCheck ? "KITNAP checks your licence online about once a month, and hasn't been able to for "
                                            "a while. Connect to the internet, then press Check now."
                                          : "Paste the licence key from your purchase email. One key works on two computers.";
    drawWrapped (g, intro, theme::font (14.0f, theme::Weight::light), theme::ink, inner.withY (card.getY() + 80.0f).withHeight (44.0f));

    if (active || needsCheck)
    {
        const auto rec = licence->record();
        const std::pair<const char*, juce::String> rows[] { { "KEY", rec.displayKey },
                                                            { "COMPUTER", rec.label },
                                                            { "CHECKED", daysAgo (rec.checked) } };
        float y = card.getY() + 138.0f;
        for (auto& [name, value] : rows)
        {
            g.setColour (theme::inkDim);
            g.setFont (theme::mono (11.0f));
            g.drawText (name, inner.withY (y).withHeight (22.0f), juce::Justification::centredLeft);
            g.setColour (theme::ink);
            g.setFont (theme::font (14.0f, theme::Weight::medium));
            g.drawText (value, inner.withTrimmedLeft (110.0f).withY (y).withHeight (22.0f), juce::Justification::centredLeft);
            y += 28.0f;
        }
    }

    // What just happened: progress, the result, or what went wrong.
    juce::String message = licence->message();
    auto colour = licence->lastActionFailed() ? theme::error : theme::inkDim;
    if (licence->isBusy())
    {
        message = active || needsCheck ? "Talking to the licence server..." : "Checking your key...";
        colour = theme::inkDim;
    }
    else if (message.isEmpty() && active)
    {
        message = "Moving to a new computer? Deactivate here first, or in your account.";
    }
    drawWrapped (g, message, theme::font (13.0f, theme::Weight::light), colour, messageArea().toFloat());
}

void LicencePanel::resized()
{
    const auto card = cardBounds();
    closeButton.setBounds (card.getRight() - pad - 84, card.getY() + 22, 84, 36);
    keyField.setBounds (card.getX() + pad, card.getY() + 140, card.getWidth() - 2 * pad, 44);

    // One row of buttons, left to right, whichever are showing.
    auto row = juce::Rectangle<int> (card.getX() + pad, card.getY() + (keyField.isVisible() ? 204 : 236), card.getWidth() - 2 * pad, 36);
    for (auto* b : { &activateButton, &checkButton, &deactivateButton, &buyButton, &findKeyButton, &manageButton })
        if (b->isVisible())
        {
            const int width = b == &deactivateButton ? 196 : b == &manageButton ? 140 : 120;
            b->setBounds (row.removeFromLeft (width));
            row.removeFromLeft (10);
        }
}

void LicencePanel::mouseUp (const juce::MouseEvent& e)
{
    if (! cardBounds().contains (e.getPosition()))
        setVisible (false);
}
