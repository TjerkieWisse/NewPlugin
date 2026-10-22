#include "PluginEditor.h"
#include "ui/Theme.h"

using namespace kitnap;

//==============================================================================
KitnapLookAndFeel::KitnapLookAndFeel()
{
    setColour (juce::TextButton::textColourOffId, theme::ink);
    setColour (juce::TextButton::textColourOnId, theme::ink);
    setColour (juce::Slider::textBoxTextColourId, theme::inkDim);
    setColour (juce::Slider::textBoxOutlineColourId, juce::Colours::transparentBlack);
    setColour (juce::TooltipWindow::backgroundColourId, theme::panel);
    setColour (juce::TooltipWindow::textColourId, theme::ink);
    setColour (juce::TooltipWindow::outlineColourId, theme::ink);
    setColour (juce::BubbleComponent::backgroundColourId, theme::panel);
    setColour (juce::BubbleComponent::outlineColourId, theme::ink);
}

// Outlined buttons; the primary one is solid black with white text.
void KitnapLookAndFeel::drawButtonBackground (juce::Graphics& g, juce::Button& button, const juce::Colour&,
                                              bool highlighted, bool down)
{
    auto r = button.getLocalBounds().toFloat().reduced (theme::outline / 2.0f);
    const bool primary = button.getProperties().getWithDefault ("primary", false);
    const float alpha = button.isEnabled() ? 1.0f : 0.35f;

    if (primary)
        g.setColour (theme::ink.withAlpha (alpha * (down ? 0.75f : highlighted ? 0.85f : 1.0f)));
    else
        g.setColour (theme::ink.withAlpha (down ? 0.12f : highlighted ? 0.06f : 0.0f));
    g.fillRoundedRectangle (r, theme::radius * 0.75f);
    g.setColour (theme::ink.withAlpha (alpha));
    g.drawRoundedRectangle (r, theme::radius * 0.75f, theme::outline);
}

void KitnapLookAndFeel::drawButtonText (juce::Graphics& g, juce::TextButton& button, bool, bool)
{
    const bool primary = button.getProperties().getWithDefault ("primary", false);
    g.setColour ((primary ? theme::panel : theme::ink).withAlpha (button.isEnabled() ? 1.0f : 0.35f));
    g.setFont (theme::display (10.5f));
    g.drawText (button.getButtonText(), button.getLocalBounds(), juce::Justification::centred);
}

void KitnapLookAndFeel::drawRotarySlider (juce::Graphics& g, int x, int y, int w, int h, float pos,
                                          float start, float end, juce::Slider&)
{
    auto r = juce::Rectangle<float> ((float) x, (float) y, (float) w, (float) h).reduced (4.0f);
    const float size = std::min (r.getWidth(), r.getHeight());
    r = r.withSizeKeepingCentre (size, size);
    const auto c = r.getCentre();
    const float arcRadius = size / 2.0f - 1.5f;
    const float angle = start + pos * (end - start);

    juce::Path track, value;
    track.addCentredArc (c.x, c.y, arcRadius, arcRadius, 0.0f, start, end, true);
    value.addCentredArc (c.x, c.y, arcRadius, arcRadius, 0.0f, start, angle, true);
    g.setColour (theme::line);
    g.strokePath (track, juce::PathStrokeType (2.0f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
    g.setColour (theme::ink);
    g.strokePath (value, juce::PathStrokeType (2.0f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));

    juce::Path pointer;
    pointer.addRoundedRectangle (-1.0f, -(arcRadius - 6.0f), 2.0f, arcRadius * 0.5f, 1.0f);
    g.fillPath (pointer, juce::AffineTransform::rotation (angle).translated (c.x, c.y));
}

//==============================================================================
KitnapAudioProcessorEditor::KitnapAudioProcessorEditor (KitnapAudioProcessor& p)
    : AudioProcessorEditor (&p), processor (p), waveform (p),
      volumeAttachment (p.apvts, "volume", volume), logo (theme::logo())
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
    g.fillAll (theme::background);

    // 40V FORTYVOLTS  |  KITNAP  steal the drum kit from any track
    auto header = getLocalBounds().reduced (24, 0).removeFromTop (80).toFloat();
    const auto logoArea = header.withSizeKeepingCentre (header.getWidth(), 22.0f).withWidth (174.0f);
    if (logo != nullptr)
        logo->drawWithin (g, logoArea, juce::RectanglePlacement::xLeft | juce::RectanglePlacement::yMid, 1.0f);

    const float divider = logoArea.getRight() + 18.0f;
    g.setColour (theme::ink);
    g.fillRect (juce::Rectangle<float> (divider, header.getCentreY() - 11.0f, theme::outline, 22.0f));

    g.setFont (theme::display (13.0f));
    const juce::String name ("KITNAP");
    const float nameX = divider + 18.0f;
    const float nameWidth = juce::GlyphArrangement::getStringWidth (g.getCurrentFont(), name);
    g.drawText (name, juce::Rectangle<float> (nameX, header.getY(), nameWidth + 4.0f, header.getHeight()),
                juce::Justification::centredLeft);

    g.setColour (theme::inkDim);
    g.setFont (theme::font (13.0f));
    g.drawText ("steal the drum kit from any track", juce::Rectangle<float> (nameX + nameWidth + 14.0f, header.getY(), 260.0f, header.getHeight()),
                juce::Justification::centredLeft);

    g.setFont (theme::display (9.0f));
    g.drawText ("VOL", volume.getBounds().translated (-38, 0).withWidth (34), juce::Justification::centredRight);
}

void KitnapAudioProcessorEditor::resized()
{
    auto area = getLocalBounds().reduced (24, 0);
    auto header = area.removeFromTop (80);

    auto controls = header.withSizeKeepingCentre (header.getWidth(), 36);
    exportButton.setBounds (controls.removeFromRight (128));
    controls.removeFromRight (10);
    loadButton.setBounds (controls.removeFromRight (84));
    controls.removeFromRight (16);
    volume.setBounds (controls.removeFromRight (36));

    waveform.setBounds (area.removeFromTop (210));
    area.removeFromTop (18);
    area.removeFromBottom (24);

    // Kick, snare and toms on top; hi-hat and cymbals below, a little wider.
    const int gap = 16;
    const int padHeight = (area.getHeight() - gap) / 2;
    auto layoutRow = [&] (juce::Rectangle<int> row, int first, int count)
    {
        const int width = (row.getWidth() - (count - 1) * gap) / count;
        for (int i = 0; i < count; ++i)
            pads[(size_t) (first + i)]->setBounds (row.getX() + i * (width + gap), row.getY(), width, row.getHeight());
    };
    layoutRow (area.removeFromTop (padHeight), 0, 3);
    area.removeFromTop (gap);
    layoutRow (area.removeFromTop (padHeight), 3, KitSampler::numPads - 3);
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
void KitnapAudioProcessorEditor::changeListenerCallback (juce::ChangeBroadcaster*)
{
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
