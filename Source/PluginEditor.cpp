#include "PluginEditor.h"
#include "ui/Theme.h"

using namespace kitnap;

//==============================================================================
KitnapLookAndFeel::KitnapLookAndFeel()
{
    setColour (juce::TextButton::textColourOffId, theme::ink);
    setColour (juce::TextButton::textColourOnId, theme::ink);
    setColour (juce::TooltipWindow::backgroundColourId, theme::card);
    setColour (juce::TooltipWindow::textColourId, theme::ink);
    setColour (juce::TooltipWindow::outlineColourId, theme::border);
}

// Pill buttons: white with a hairline, or solid black for the primary one.
void KitnapLookAndFeel::drawButtonBackground (juce::Graphics& g, juce::Button& button, const juce::Colour&,
                                              bool highlighted, bool down)
{
    auto r = button.getLocalBounds().toFloat().reduced (0.5f);
    const float radius = r.getHeight() / 2.0f;
    const bool primary = (bool) button.getProperties().getWithDefault ("primary", false);
    const float alpha = button.isEnabled() ? 1.0f : 0.3f;

    if (primary)
    {
        g.setColour (theme::ink.brighter (down ? 0.0f : highlighted ? 0.25f : 0.0f).withAlpha (alpha * (down ? 0.85f : 1.0f)));
        g.fillRoundedRectangle (r, radius);
        return;
    }
    g.setColour (theme::card.darker (down ? 0.06f : highlighted ? 0.025f : 0.0f));
    g.fillRoundedRectangle (r, radius);
    g.setColour (juce::Colour (0xffe4e4e4).darker (highlighted ? 0.08f : 0.0f).withMultipliedAlpha (alpha));
    g.drawRoundedRectangle (r, radius, 1.0f);
}

void KitnapLookAndFeel::drawButtonText (juce::Graphics& g, juce::TextButton& button, bool, bool)
{
    const bool primary = (bool) button.getProperties().getWithDefault ("primary", false);
    g.setColour ((primary ? theme::card : theme::ink).withAlpha (button.isEnabled() || primary ? 1.0f : 0.35f));
    g.setFont (theme::font (13.0f, theme::Weight::medium));
    g.drawText (button.getButtonText(), button.getLocalBounds(), juce::Justification::centred);
}

// The volume knob: a grey arc, filled with the aurora up to the value, and the value in the middle.
void KitnapLookAndFeel::drawRotarySlider (juce::Graphics& g, int x, int y, int w, int h, float pos,
                                          float start, float end, juce::Slider& slider)
{
    const auto box = juce::Rectangle<float> ((float) x, (float) y, (float) w, (float) h);
    const auto c = box.getCentre();
    const float radius = std::min (box.getWidth(), box.getHeight()) / 2.0f - 5.5f;
    const juce::PathStrokeType stroke (5.0f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded);

    juce::Path track, value;
    track.addCentredArc (c.x, c.y, radius, radius, 0.0f, start, end, true);
    g.setColour (theme::line);
    g.strokePath (track, stroke);
    if (pos > 0.001f)
    {
        value.addCentredArc (c.x, c.y, radius, radius, 0.0f, start, start + pos * (end - start), true);
        g.setGradientFill (theme::auroraShort (box.getBottomLeft(), box.getTopRight()));
        g.strokePath (value, stroke);
    }

    const double db = slider.getValue();
    g.setColour (theme::ink);
    g.setFont (theme::font (24.0f, theme::Weight::light));
    g.drawText ((db > 0.05 ? "+" : "") + juce::String (db, 1), box.withY (c.y - 18.0f).withHeight (30.0f), juce::Justification::centred);
    g.setColour (theme::inkDim);
    g.setFont (theme::mono (11.0f));
    g.drawText ("dB", box.withY (c.y + 14.0f).withHeight (16.0f), juce::Justification::centred);
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

    // The volume knob, in the last tile; drag up or down, double-click to reset.
    volume.setSliderStyle (juce::Slider::RotaryHorizontalVerticalDrag);
    volume.setRotaryParameters (juce::MathConstants<float>::pi * 1.25f, juce::MathConstants<float>::pi * 2.75f, true);
    volume.setTextBoxStyle (juce::Slider::NoTextBox, false, 0, 0);
    if (auto* param = processor.apvts.getParameter ("volume"))
        volume.setDoubleClickReturnValue (true, param->convertFrom0to1 (param->getDefaultValue()));
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
    // The song card repaints every frame, so the shadow behind it is drawn once into a picture at
    // the screen's pixel density and reused.
    const float scale = g.getInternalContext().getPhysicalPixelScaleFactor();
    if (! background.isValid() || ! juce::approximatelyEqual (backgroundScale, scale))
    {
        backgroundScale = scale;
        background = juce::Image (juce::Image::RGB, juce::roundToInt ((float) getWidth() * scale),
                                  juce::roundToInt ((float) getHeight() * scale), false);
        juce::Graphics bg (background);
        bg.addTransform (juce::AffineTransform::scale (scale));
        paintBackground (bg);
    }
    g.drawImage (background, getLocalBounds().toFloat());
}

void KitnapAudioProcessorEditor::paintBackground (juce::Graphics& g)
{
    g.fillAll (theme::background);
    theme::paintCard (g, waveform.getBounds().toFloat(), theme::cardRadius);

    // The volume tile around the knob.
    const auto tile = volumeTile().toFloat();
    theme::paintCard (g, tile, theme::tileRadius, false);
    g.setColour (theme::ink);
    g.setFont (theme::font (14.0f, theme::Weight::medium));
    g.drawText ("Volume", tile.withTrimmedLeft (18.0f).withY (tile.getY() + 16.0f).withHeight (20.0f), juce::Justification::centredLeft);

    // 40V FORTYVOLTS  |  KITNAP
    if (logo != nullptr)
        logo->drawWithin (g, { 28.0f, 30.0f, 158.0f, 20.0f }, juce::RectanglePlacement::xLeft | juce::RectanglePlacement::yMid, 1.0f);
    g.setColour (juce::Colour (0xffd4d4d4));
    g.fillRect (juce::Rectangle<float> (204.0f, 29.0f, 1.0f, 22.0f));
    g.setColour (theme::ink);
    g.setFont (theme::display (12.0f));
    g.drawText ("KITNAP", juce::Rectangle<float> (222.0f, 30.0f, 120.0f, 20.0f), juce::Justification::centredLeft);
}

void KitnapAudioProcessorEditor::resized()
{
    background = {};
    loadButton.setBounds (728, 22, 84, 36);
    exportButton.setBounds (820, 22, 116, 36);
    waveform.setBounds (24, 76, 912, 236);

    // The pad tiles in a row under the song, then the volume tile.
    for (int i = 0; i < (int) pads.size(); ++i)
        pads[(size_t) i]->setBounds (24 + i * 153, 332, 140, 284);
    volume.setBounds (volumeTile().getX() + 20, volumeTile().getY() + 84, 104, 104);
}

juce::Rectangle<int> KitnapAudioProcessorEditor::volumeTile() const
{
    return { 792, 332, 144, 284 };
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
        pads[(size_t) i]->setPlaceholder (kit.status == KitState::Status::ready ? "not in this song" : "");
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
