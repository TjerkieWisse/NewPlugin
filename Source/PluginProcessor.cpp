#include "PluginProcessor.h"
#include "PluginEditor.h"

OrbitAudioProcessor::OrbitAudioProcessor()
    : AudioProcessor (BusesProperties()
                          .withInput ("Input", juce::AudioChannelSet::stereo(), true)
                          .withOutput ("Output", juce::AudioChannelSet::stereo(), true)),
      apvts (*this, nullptr, "ORBIT", createLayout())
{
    xParam = apvts.getRawParameterValue (ParamIDs::x);
    yParam = apvts.getRawParameterValue (ParamIDs::y);
    mixParam = apvts.getRawParameterValue (ParamIDs::mix);
    motionParam = apvts.getRawParameterValue (ParamIDs::motion);
    speedParam = apvts.getRawParameterValue (ParamIDs::speed);
    freezeParam = apvts.getRawParameterValue (ParamIDs::freeze);
    outputParam = apvts.getRawParameterValue (ParamIDs::output);
}

juce::AudioProcessorValueTreeState::ParameterLayout OrbitAudioProcessor::createLayout()
{
    using namespace juce;
    auto percent = AudioParameterFloatAttributes().withStringFromValueFunction (
        [] (float v, int) { return String (juce::roundToInt (v * 100.0f)) + "%"; });

    std::vector<std::unique_ptr<RangedAudioParameter>> params;
    params.push_back (std::make_unique<AudioParameterFloat> (ParameterID { ParamIDs::x, 1 }, "Orb X",
                                                             NormalisableRange<float> (0.0f, 1.0f), 0.5f, percent));
    params.push_back (std::make_unique<AudioParameterFloat> (ParameterID { ParamIDs::y, 1 }, "Orb Y",
                                                             NormalisableRange<float> (0.0f, 1.0f), 0.5f, percent));
    params.push_back (std::make_unique<AudioParameterFloat> (ParameterID { ParamIDs::mix, 1 }, "Mix",
                                                             NormalisableRange<float> (0.0f, 1.0f), 0.6f, percent));
    params.push_back (std::make_unique<AudioParameterFloat> (ParameterID { ParamIDs::motion, 1 }, "Motion",
                                                             NormalisableRange<float> (0.0f, 1.0f), 0.0f, percent));
    params.push_back (std::make_unique<AudioParameterFloat> (
        ParameterID { ParamIDs::speed, 1 }, "Speed", NormalisableRange<float> (0.02f, 2.0f, 0.0f, 0.4f), 0.15f,
        AudioParameterFloatAttributes().withLabel ("Hz").withStringFromValueFunction (
            [] (float v, int) { return String (v, 2) + " Hz"; })));
    params.push_back (std::make_unique<AudioParameterBool> (ParameterID { ParamIDs::freeze, 1 }, "Freeze", false));
    params.push_back (std::make_unique<AudioParameterFloat> (
        ParameterID { ParamIDs::output, 1 }, "Output", NormalisableRange<float> (-24.0f, 12.0f), 0.0f,
        AudioParameterFloatAttributes().withLabel ("dB").withStringFromValueFunction (
            [] (float v, int) { return (v > 0.0f ? "+" : "") + String (v, 1) + " dB"; })));

    return { params.begin(), params.end() };
}

void OrbitAudioProcessor::prepareToPlay (double sampleRate, int)
{
    currentSampleRate = (float) sampleRate;

    dream.prepare (sampleRate);
    space.prepare (sampleRate);
    tape.prepare (sampleRate);
    glitch.prepare (sampleRate);
    freezer.prepare (sampleRate);

    smoothX.reset (sampleRate, 0.05);
    smoothY.reset (sampleRate, 0.05);
    smoothMix.reset (sampleRate, 0.05);
    smoothGain.reset (sampleRate, 0.05);
    smoothX.setCurrentAndTargetValue (xParam->load());
    smoothY.setCurrentAndTargetValue (yParam->load());
    smoothMix.setCurrentAndTargetValue (mixParam->load());
    smoothGain.setCurrentAndTargetValue (juce::Decibels::decibelsToGain (outputParam->load()));
    motionPhase = 0.0;
}

bool OrbitAudioProcessor::isBusesLayoutSupported (const BusesLayout& layouts) const
{
    const auto out = layouts.getMainOutputChannelSet();
    if (out != juce::AudioChannelSet::stereo() && out != juce::AudioChannelSet::mono())
        return false;
    return layouts.getMainInputChannelSet() == out;
}

void OrbitAudioProcessor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer&)
{
    juce::ScopedNoDenormals noDenormals;

    const int numSamples = buffer.getNumSamples();
    const int numChannels = buffer.getNumChannels();
    for (int ch = getTotalNumInputChannels(); ch < getTotalNumOutputChannels(); ++ch)
        buffer.clear (ch, 0, numSamples);

    if (numChannels == 0 || numSamples == 0)
        return;

    // Sync tempo-based effects to the host when it tells us the BPM.
    double bpm = 0.0;
    if (auto* playHead = getPlayHead())
        if (auto position = playHead->getPosition())
            if (auto hostBpm = position->getBpm())
                bpm = *hostBpm;
    space.setTempo (bpm);
    glitch.setTempo (bpm);

    freezer.setFrozen (freezeParam->load() > 0.5f);

    // MOTION: the orb drifts on a Lissajous path around the position the user set.
    const float motion = motionParam->load();
    const double blockSeconds = (double) numSamples / currentSampleRate;
    motionPhase = std::fmod (motionPhase + blockSeconds * speedParam->load(), 1000.0);
    const float radius = 0.38f * motion;
    const float targetX = juce::jlimit (0.0f, 1.0f, xParam->load() + radius * (float) std::sin (juce::MathConstants<double>::twoPi * motionPhase));
    const float targetY = juce::jlimit (0.0f, 1.0f, yParam->load() + radius * (float) std::sin (juce::MathConstants<double>::twoPi * motionPhase * 1.5 + 1.3));

    smoothX.setTargetValue (targetX);
    smoothY.setTargetValue (targetY);
    smoothMix.setTargetValue (mixParam->load());
    smoothGain.setTargetValue (juce::Decibels::decibelsToGain (outputParam->load()));

    float* left = buffer.getWritePointer (0);
    float* right = numChannels > 1 ? buffer.getWritePointer (1) : nullptr;
    float sumSquares = 0.0f;

    for (int i = 0; i < numSamples; ++i)
    {
        float inL = left[i];
        float inR = right != nullptr ? right[i] : inL;
        freezer.process (inL, inR);

        // Bilinear weights: each corner of the pad is one effect, y = 1 is the top.
        const float x = smoothX.getNextValue();
        const float y = smoothY.getNextValue();
        const float wDream = (1.0f - x) * y;
        const float wSpace = x * y;
        const float wTape = (1.0f - x) * (1.0f - y);
        const float wGlitch = x * (1.0f - y);

        float wetL = 0.0f, wetR = 0.0f, l, r;
        dream.process (inL, inR, l, r);
        wetL += wDream * l; wetR += wDream * r;
        space.process (inL, inR, l, r);
        wetL += wSpace * l; wetR += wSpace * r;
        tape.process (inL, inR, l, r);
        wetL += wTape * l; wetR += wTape * r;
        glitch.process (inL, inR, l, r);
        wetL += wGlitch * l; wetR += wGlitch * r;

        const float mix = smoothMix.getNextValue();
        const float gain = smoothGain.getNextValue();
        const float outL = orbit::softClip (gain * (inL + mix * (wetL - inL)));
        const float outR = orbit::softClip (gain * (inR + mix * (wetR - inR)));

        left[i] = outL;
        if (right != nullptr)
            right[i] = outR;
        else
            left[i] = 0.5f * (outL + outR);

        sumSquares += 0.5f * (outL * outL + outR * outR);
    }

    orbX.store (smoothX.getCurrentValue());
    orbY.store (smoothY.getCurrentValue());
    level.store (std::sqrt (sumSquares / (float) numSamples));
    frozenActive.store (freezer.isActive());
}

juce::AudioProcessorEditor* OrbitAudioProcessor::createEditor()
{
    return new OrbitAudioProcessorEditor (*this);
}

void OrbitAudioProcessor::getStateInformation (juce::MemoryBlock& destData)
{
    if (auto xml = apvts.copyState().createXml())
        copyXmlToBinary (*xml, destData);
}

void OrbitAudioProcessor::setStateInformation (const void* data, int sizeInBytes)
{
    if (auto xml = getXmlFromBinary (data, sizeInBytes))
        if (xml->hasTagName (apvts.state.getType()))
            apvts.replaceState (juce::ValueTree::fromXml (*xml));
}

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new OrbitAudioProcessor();
}
