#include "KitSampler.h"

namespace kitnap
{
int KitSampler::noteForPad (int pad)
{
    // GM drum map: kick, snare, clap, closed hat, open hat, low tom.
    static constexpr int notes[] { 36, 38, 39, 42, 46, 45 };
    return notes[pad];
}

void KitSampler::prepare (double newSampleRate)
{
    sampleRate = newSampleRate;
    for (auto& v : voices)
        v = {};
}

void KitSampler::setPadSample (int pad, SamplePtr sample)
{
    const juce::SpinLock::ScopedLockType lock (padLock);
    pads[(size_t) pad] = std::move (sample);
}

SamplePtr KitSampler::getPadSample (int pad) const
{
    const juce::SpinLock::ScopedLockType lock (padLock);
    return pads[(size_t) pad];
}

void KitSampler::decayActivity (float factor)
{
    for (auto& a : activity)
        a.store (a.load() * factor);
}

void KitSampler::startVoice (int pad, float velocity)
{
    SamplePtr sample;
    {
        // Never block the audio thread: if the editor is swapping a sample right now, skip this hit.
        const juce::SpinLock::ScopedTryLockType lock (padLock);
        if (! lock.isLocked())
            return;
        sample = pads[(size_t) pad];
    }
    if (sample == nullptr || sample->audio.getNumSamples() == 0)
        return;

    // Closed hat chokes the open hat, like on a real kit.
    if (pad == (int) DrumClass::hat)
        for (auto& v : voices)
            if (v.pad == (int) DrumClass::openHat && v.sample != nullptr && v.fadeOutRemaining < 0)
                v.fadeOutRemaining = (int) (0.006 * sampleRate);

    // Take a free voice, or steal the one that has played the longest.
    Voice* target = nullptr;
    for (auto& v : voices)
        if (v.sample == nullptr) { target = &v; break; }
    if (target == nullptr)
    {
        target = &voices[0];
        for (auto& v : voices)
            if (v.position > target->position)
                target = &v;
    }

    target->sample = std::move (sample);
    target->pad = pad;
    target->position = 0.0;
    target->increment = target->sample->sampleRate / sampleRate;
    target->gain = velocity;
    target->fadeOutRemaining = -1;

    activity[(size_t) pad].store (std::max (activity[(size_t) pad].load(), 0.4f + 0.6f * velocity));
}

void KitSampler::render (juce::AudioBuffer<float>& output, const juce::MidiBuffer& midi)
{
    output.clear();

    const int queued = uiTriggers.exchange (0);
    for (int pad = 0; pad < numPads; ++pad)
        if ((queued & (1 << pad)) != 0)
            startVoice (pad, 0.9f);

    int position = 0;
    for (const auto metadata : midi)
    {
        const auto message = metadata.getMessage();
        const int eventTime = juce::jlimit (0, output.getNumSamples(), metadata.samplePosition);
        if (! message.isNoteOn())
            continue;

        renderVoices (output, position, eventTime - position);
        position = eventTime;

        for (int pad = 0; pad < numPads; ++pad)
            if (noteForPad (pad) == message.getNoteNumber())
                startVoice (pad, message.getFloatVelocity());
    }
    renderVoices (output, position, output.getNumSamples() - position);
}

void KitSampler::renderVoices (juce::AudioBuffer<float>& output, int start, int numSamples)
{
    if (numSamples <= 0)
        return;

    const int outChannels = output.getNumChannels();
    const int fadeLength = std::max (1, (int) (0.006 * sampleRate));

    for (auto& v : voices)
    {
        if (v.sample == nullptr)
            continue;

        const auto& audio = v.sample->audio;
        const int length = audio.getNumSamples();
        const int srcChannels = audio.getNumChannels();

        for (int i = 0; i < numSamples; ++i)
        {
            const int index = (int) v.position;
            if (index + 1 >= length || v.fadeOutRemaining == 0)
            {
                v.sample = nullptr;
                v.pad = -1;
                break;
            }

            float gain = v.gain;
            if (v.fadeOutRemaining > 0)
                gain *= (float) v.fadeOutRemaining-- / (float) fadeLength;

            const float frac = (float) (v.position - index);
            for (int ch = 0; ch < outChannels; ++ch)
            {
                const float* src = audio.getReadPointer (std::min (ch, srcChannels - 1));
                const float sample = src[index] + frac * (src[index + 1] - src[index]);
                output.addSample (ch, start + i, gain * sample);
            }
            v.position += v.increment;
        }
    }
}
} // namespace kitnap
