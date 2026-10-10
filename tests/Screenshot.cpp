// Renders the editor to PNGs: empty, mid-analysis and with a finished kit.
// Usage: KitnapScreenshot <audio-file> <output-folder>

#include "PluginProcessor.h"
#include "PluginEditor.h"
#include <iostream>

namespace
{
void snap (juce::Component& c, const juce::File& file)
{
    const auto image = c.createComponentSnapshot (c.getLocalBounds(), true, 2.0f);
    file.deleteFile();
    juce::FileOutputStream out (file);
    juce::PNGImageFormat().writeImageToStream (image, out);
}

void pump (int ms)
{
    const auto end = juce::Time::getMillisecondCounter() + (juce::uint32) ms;
    while (juce::Time::getMillisecondCounter() < end)
        juce::MessageManager::getInstance()->runDispatchLoopUntil (10);
}
} // namespace

int main (int argc, char** argv)
{
    if (argc < 3)
        return 1;

    juce::ScopedJuceInitialiser_GUI gui;
    const auto cwd = juce::File::getCurrentWorkingDirectory();
    const auto audio = cwd.getChildFile (argv[1]);
    const auto out = cwd.getChildFile (argv[2]);
    out.createDirectory();

    auto processor = std::make_unique<KitnapAudioProcessor>();
    processor->prepareToPlay (44100.0, 512);
    std::unique_ptr<juce::AudioProcessorEditor> editor (processor->createEditor());
    pump (300);
    snap (*editor, out.getChildFile ("1-empty.png"));

    processor->loadFile (audio);
    pump (120);
    snap (*editor, out.getChildFile ("2-analysing.png"));

    while (processor->getKit().status == KitState::Status::analysing)
        pump (50);
    pump (2200); // let the reveal animation finish

    // Hit a couple of pads so their glow shows.
    juce::AudioBuffer<float> buffer (2, 512);
    juce::MidiBuffer midi;
    processor->triggerPad (0);
    processor->triggerPad (3);
    processor->processBlock (buffer, midi);
    std::cout << "Pad hit output peak: " << buffer.getMagnitude (0, buffer.getNumSamples()) << "\n";

    // Let the pad hits ring out, then a MIDI note (GM snare, D1) should play the snare pad.
    for (int i = 0; i < 200; ++i)
    {
        juce::MidiBuffer none;
        processor->processBlock (buffer, none);
    }
    juce::AudioBuffer<float> midiBuffer (2, 512);
    juce::MidiBuffer notes;
    notes.addEvent (juce::MidiMessage::noteOn (10, 38, 0.8f), 100);
    processor->processBlock (midiBuffer, notes);
    std::cout << "MIDI snare peak: " << midiBuffer.getMagnitude (0, 100) << " before note, "
              << midiBuffer.getMagnitude (100, 412) << " after\n";
    pump (60);
    snap (*editor, out.getChildFile ("3-kit.png"));

    // Round-trip the project state into a fresh instance.
    juce::MemoryBlock state;
    processor->getStateInformation (state);
    auto restored = std::make_unique<KitnapAudioProcessor>();
    restored->setStateInformation (state.getData(), (int) state.getSize());
    while (restored->getKit().status == KitState::Status::analysing)
        pump (50);
    int restoredPads = 0;
    for (auto& v : restored->getKit().variations)
        restoredPads += v.empty() ? 0 : 1;
    std::cout << "State size " << state.getSize() / 1024 << " KB, restored pads: " << restoredPads << "\n";

    editor.reset();
    return 0;
}
