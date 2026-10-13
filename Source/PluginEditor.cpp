#include "PluginEditor.h"
#include "ui/Theme.h"

using namespace kitnap;

//==============================================================================
KitnapLookAndFeel::KitnapLookAndFeel()
{
    setColour (juce::TextButton::textColourOffId, theme::text);
    setColour (juce::TextButton::textColourOnId, theme::text);
    setColour (juce::Slider::textBoxTextColourId, theme::textDim);
    setColour (juce::Slider::textBoxOutlineColourId, juce::Colours::transparentBlack);
    setColour (juce::TooltipWindow::backgroundColourId, theme::panel);
}

void KitnapLookAndFeel::drawButtonBackground (juce::Graphics& g, juce::Button& button, const juce::Colour&,
                                              bool highlighted, bool down)
{
    auto r = button.getLocalBounds().toFloat().reduced (1.0f);
    const bool primary = button.getProperties().getWithDefault ("primary", false);

    if (button.getProperties().getWithDefault ("ai", false))
    {
        // AI switch: glowing gradient when on, outlined when off.
        const bool on = button.getToggleState() && button.isEnabled();
        const auto violet = theme::forClass (DrumClass::perc), blue = theme::forClass (DrumClass::openHat);
        if (on)
        {
            juce::ColourGradient fill (violet, r.getX(), 0.0f, blue, r.getRight(), 0.0f, false);
            g.setGradientFill (fill);
            g.setOpacity (down ? 0.8f : highlighted ? 1.0f : 0.92f);
            g.fillRoundedRectangle (r, r.getHeight() / 2.0f);
        }
        else
        {
            g.setColour (theme::panel);
            g.fillRoundedRectangle (r, r.getHeight() / 2.0f);
            g.setColour ((button.isEnabled() ? violet : theme::panelEdge).withAlpha (highlighted ? 1.0f : 0.7f));
            g.drawRoundedRectangle (r, r.getHeight() / 2.0f, 1.2f);
        }
        return;
    }

    const auto base = primary ? theme::accent : theme::panel;

    g.setColour (base.withMultipliedBrightness (down ? 0.8f : highlighted ? 1.15f : 1.0f)
                     .withAlpha (button.isEnabled() ? 1.0f : 0.4f));
    g.fillRoundedRectangle (r, r.getHeight() / 2.0f);
    if (! primary)
    {
        g.setColour (theme::panelEdge.brighter (highlighted ? 0.4f : 0.0f));
        g.drawRoundedRectangle (r, r.getHeight() / 2.0f, 1.0f);
    }
}

juce::Font KitnapLookAndFeel::getTextButtonFont (juce::TextButton&, int)
{
    return theme::font (13.0f, true);
}

void KitnapLookAndFeel::drawRotarySlider (juce::Graphics& g, int x, int y, int w, int h, float pos,
                                          float start, float end, juce::Slider&)
{
    auto r = juce::Rectangle<float> ((float) x, (float) y, (float) w, (float) h).reduced (4.0f);
    const float size = std::min (r.getWidth(), r.getHeight());
    r = r.withSizeKeepingCentre (size, size);
    const auto c = r.getCentre();
    const float radius = size / 2.0f;
    const float angle = start + pos * (end - start);

    juce::Path track, value;
    track.addCentredArc (c.x, c.y, radius - 2.0f, radius - 2.0f, 0.0f, start, end, true);
    value.addCentredArc (c.x, c.y, radius - 2.0f, radius - 2.0f, 0.0f, start, angle, true);
    g.setColour (theme::panelEdge);
    g.strokePath (track, juce::PathStrokeType (3.0f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
    g.setColour (theme::accent);
    g.strokePath (value, juce::PathStrokeType (3.0f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));

    g.setColour (theme::panel);
    g.fillEllipse (r.reduced (7.0f));
    juce::Path pointer;
    pointer.addRoundedRectangle (-1.5f, -(radius - 9.0f), 3.0f, radius * 0.45f, 1.5f);
    g.setColour (theme::text);
    g.fillPath (pointer, juce::AffineTransform::rotation (angle).translated (c.x, c.y));
}

//==============================================================================
KitnapAudioProcessorEditor::KitnapAudioProcessorEditor (KitnapAudioProcessor& p)
    : AudioProcessorEditor (&p), processor (p), waveform (p),
      volumeAttachment (p.apvts, "volume", volume)
{
    setLookAndFeel (&lookAndFeel);

    addAndMakeVisible (waveform);
    waveform.onBrowse = [this] { browseForFile(); };

    for (int i = 0; i < KitSampler::numPads; ++i)
    {
        auto pad = std::make_unique<PadComponent> (i);
        pad->onPlay = [this, i] { processor.triggerPad (i); };
        pad->onVariation = [this, i] (int delta) { processor.selectVariation (i, processor.getKit().selected[(size_t) i] + delta); };
        pad->onDragOut = [this, i] { dragPadOut (i); };
        addAndMakeVisible (*pad);
        pads.push_back (std::move (pad));
    }

    aiButton.getProperties().set ("ai", true);
    aiButton.onClick = [this] { processor.setAIEnabled (! processor.isAIEnabled()); };
    addAndMakeVisible (aiButton);

    loadButton.onClick = [this] { browseForFile(); };
    exportButton.onClick = [this] { exportKit(); };
    exportButton.getProperties().set ("primary", true);
    addAndMakeVisible (loadButton);
    addAndMakeVisible (exportButton);

    volume.setSliderStyle (juce::Slider::RotaryHorizontalVerticalDrag);
    volume.setTextBoxStyle (juce::Slider::NoTextBox, false, 0, 0);
    volume.setTooltip ("Volume");
    volume.setPopupDisplayEnabled (true, true, this);
    addAndMakeVisible (volume);

    processor.addChangeListener (this);
    setSize (960, 640);
    refreshAIButton();
    refreshPads();
    waveform.kitChanged();
    lastTick = juce::Time::getMillisecondCounterHiRes();
    startTimerHz (60);
}

KitnapAudioProcessorEditor::~KitnapAudioProcessorEditor()
{
    processor.removeChangeListener (this);
    setLookAndFeel (nullptr);
}

void KitnapAudioProcessorEditor::paint (juce::Graphics& g)
{
    juce::ColourGradient bg (theme::backgroundTop, 0.0f, 0.0f, theme::background, 0.0f, (float) getHeight(), false);
    g.setGradientFill (bg);
    g.fillAll();

    // Logo
    auto header = getLocalBounds().reduced (24, 0).removeFromTop (80).toFloat();
    juce::GlyphArrangement logo;
    logo.addLineOfText (theme::font (30.0f, true).withExtraKerningFactor (0.12f), "KITNAP", header.getX(), header.getCentreY() + 10.0f);
    juce::Path logoPath;
    logo.createPath (logoPath);
    const auto logoBounds = logoPath.getBounds();
    juce::ColourGradient logoGradient (theme::forClass (DrumClass::kick), logoBounds.getX(), 0.0f,
                                       theme::forClass (DrumClass::snare), logoBounds.getRight(), 0.0f, false);
    g.setGradientFill (logoGradient);
    g.fillPath (logoPath);

    g.setColour (theme::textDim);
    g.setFont (theme::font (14.0f));
    g.setFont (theme::font (11.0f, true));
    g.drawText ("VOL", volume.getBounds().translated (-38, 0).withWidth (34), juce::Justification::centredRight);

    g.setFont (theme::font (14.0f));
    g.drawText ("steal the drum kit from any track", juce::Rectangle<float> (logoBounds.getRight() + 16.0f, header.getY(), 300.0f, header.getHeight()).translated (0.0f, 2.0f),
                juce::Justification::centredLeft);
}

void KitnapAudioProcessorEditor::resized()
{
    auto area = getLocalBounds().reduced (24, 0);
    auto header = area.removeFromTop (80);

    auto controls = header.withSizeKeepingCentre (header.getWidth(), 40);
    exportButton.setBounds (controls.removeFromRight (130));
    controls.removeFromRight (10);
    loadButton.setBounds (controls.removeFromRight (90));
    controls.removeFromRight (10);
    aiButton.setBounds (controls.removeFromRight (96));
    controls.removeFromRight (14);
    volume.setBounds (controls.removeFromRight (40));

    waveform.setBounds (area.removeFromTop (210));
    area.removeFromTop (18);
    area.removeFromBottom (24);

    const int gap = 16;
    const int padWidth = (area.getWidth() - 2 * gap) / 3;
    const int padHeight = (area.getHeight() - gap) / 2;
    for (int i = 0; i < (int) pads.size(); ++i)
    {
        const int row = i / 3, col = i % 3;
        pads[(size_t) i]->setBounds (area.getX() + col * (padWidth + gap), area.getY() + row * (padHeight + gap), padWidth, padHeight);
    }
}

//==============================================================================
bool KitnapAudioProcessorEditor::isInterestedInFileDrag (const juce::StringArray& files)
{
    for (auto& f : files)
        if (processor.canLoad (juce::File (f)))
            return true;
    return false;
}

void KitnapAudioProcessorEditor::fileDragEnter (const juce::StringArray&, int, int) { waveform.setDragHover (true); }
void KitnapAudioProcessorEditor::fileDragExit (const juce::StringArray&) { waveform.setDragHover (false); }

void KitnapAudioProcessorEditor::filesDropped (const juce::StringArray& files, int, int)
{
    waveform.setDragHover (false);
    for (auto& f : files)
        if (processor.canLoad (juce::File (f)))
        {
            processor.loadFile (juce::File (f));
            return;
        }
}

void KitnapAudioProcessorEditor::browseForFile()
{
    chooser = std::make_unique<juce::FileChooser> ("Pick a song or drum loop", juce::File(), "*.wav;*.aif;*.aiff;*.flac;*.mp3;*.ogg");
    chooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
                          [this] (const juce::FileChooser& fc)
                          {
                              if (auto file = fc.getResult(); processor.canLoad (file))
                                  processor.loadFile (file);
                          });
}

void KitnapAudioProcessorEditor::exportKit()
{
    chooser = std::make_unique<juce::FileChooser> ("Where should the kit go?",
                                                   juce::File::getSpecialLocation (juce::File::userMusicDirectory));
    chooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectDirectories,
                          [this] (const juce::FileChooser& fc)
                          {
                              const auto folder = fc.getResult();
                              if (folder == juce::File())
                                  return;
                              const auto written = processor.exportKit (folder);
                              if (! written.isEmpty())
                                  written.getFirst().revealToUser();
                          });
}

void KitnapAudioProcessorEditor::dragPadOut (int pad)
{
    const auto folder = juce::File::getSpecialLocation (juce::File::tempDirectory).getChildFile ("KITNAP");
    const auto file = processor.writePadToFile (pad, folder);
    if (file.existsAsFile())
        juce::DragAndDropContainer::performExternalDragDropOfFiles ({ file.getFullPathName() }, false, this);
}

//==============================================================================
void KitnapAudioProcessorEditor::refreshAIButton()
{
    const bool available = processor.isAIAvailable();
    const bool on = available && processor.isAIEnabled();
    aiButton.setEnabled (available);
    aiButton.setToggleState (on, juce::dontSendNotification);
    aiButton.setButtonText (available ? (on ? "AI  ON" : "AI  OFF") : "AI  N/A");
    aiButton.setTooltip (! available ? "AI separation unavailable: " + processor.aiUnavailableReason()
                         : processor.canSplitKit()
                             ? juce::String ("AI: pulls the drums out of the mix, then splits them into kick, snare and hi-hat, "
                                             "so every pad is built from its own drum. Much cleaner on full songs; "
                                             "takes a few minutes per song.")
                             : juce::String ("AI drum separation: pulls the drums out of the mix before the kit is extracted. "
                                             "Much cleaner on full songs, takes a few minutes. (The drum split model "
                                             "isn't installed, so layered drums are separated less cleanly.)"));
}

void KitnapAudioProcessorEditor::changeListenerCallback (juce::ChangeBroadcaster*)
{
    refreshAIButton();
    refreshPads();
    waveform.kitChanged();
}

void KitnapAudioProcessorEditor::refreshPads()
{
    const auto& kit = processor.getKit();
    bool any = false;
    for (int i = 0; i < (int) pads.size(); ++i)
    {
        const auto& vars = kit.variations[(size_t) i];
        const int sel = kit.selected[(size_t) i];
        const bool valid = juce::isPositiveAndBelow (sel, (int) vars.size());
        pads[(size_t) i]->setSound (valid ? vars[(size_t) sel] : nullptr, sel, (int) vars.size());
        any = any || valid;
    }
    exportButton.setEnabled (any);
}

void KitnapAudioProcessorEditor::timerCallback()
{
    const double now = juce::Time::getMillisecondCounterHiRes();
    const double seconds = juce::jlimit (0.0, 0.1, (now - lastTick) / 1000.0);
    lastTick = now;

    waveform.tick (seconds);
    for (int i = 0; i < (int) pads.size(); ++i)
        pads[(size_t) i]->setActivity (processor.getPadActivity (i));
    processor.decayPadActivity ((float) std::exp (-seconds * 5.0));
}
