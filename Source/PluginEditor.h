#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include "PluginProcessor.h"
#include "ui/PadComponent.h"
#include "ui/WaveformView.h"

class KitnapLookAndFeel : public juce::LookAndFeel_V4
{
public:
    KitnapLookAndFeel();
    void drawButtonBackground (juce::Graphics&, juce::Button&, const juce::Colour&, bool highlighted, bool down) override;
    juce::Font getTextButtonFont (juce::TextButton&, int) override;
    void drawRotarySlider (juce::Graphics&, int x, int y, int w, int h, float pos, float start, float end, juce::Slider&) override;
};

class KitnapAudioProcessorEditor : public juce::AudioProcessorEditor,
                                   public juce::FileDragAndDropTarget,
                                   private juce::ChangeListener,
                                   private juce::Timer
{
public:
    explicit KitnapAudioProcessorEditor (KitnapAudioProcessor&);
    ~KitnapAudioProcessorEditor() override;

    void paint (juce::Graphics&) override;
    void resized() override;

    bool isInterestedInFileDrag (const juce::StringArray& files) override;
    void fileDragEnter (const juce::StringArray&, int, int) override;
    void fileDragExit (const juce::StringArray&) override;
    void filesDropped (const juce::StringArray& files, int, int) override;

private:
    void changeListenerCallback (juce::ChangeBroadcaster*) override;
    void timerCallback() override;
    void refreshPads();
    void browseForFile();
    void exportKit();
    void dragPadOut (int pad);

    KitnapAudioProcessor& processor;
    KitnapLookAndFeel lookAndFeel;

    WaveformView waveform;
    std::vector<std::unique_ptr<PadComponent>> pads;
    juce::TextButton loadButton { "LOAD" }, exportButton { "EXPORT KIT" };
    juce::TooltipWindow tooltips { this };
    juce::Slider volume;
    juce::AudioProcessorValueTreeState::SliderAttachment volumeAttachment;
    std::unique_ptr<juce::FileChooser> chooser;
    double lastTick = 0.0;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (KitnapAudioProcessorEditor)
};
