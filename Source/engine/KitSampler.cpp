#include "KitSampler.h"

namespace kitnap
{
int KitSampler::noteForPad (int pad)
{
    // GM drum map: kick, snare, low tom, closed hat, crash.
    static constexpr int notes[] { 36, 38, 45, 42, 49 };
    return notes[pad];
}

int KitSampler::padForNote (int note)
{
    // The other GM notes of each kind of drum play the same pad, so any drum pattern sounds complete.
    switch (note)
    {
        case 35: case 36:                                         return (int) DrumClass::kick;
        case 37: case 38: case 39: case 40:                       return (int) DrumClass::snare;
        case 41: case 43: case 45: case 47: case 48: case 50:     return (int) DrumClass::toms;
        case 42: case 44: case 46:                                return (int) DrumClass::hihat;
        case 49: case 51: case 52: case 53: case 55: case 57: case 59: return (int) DrumClass::cymbals;
        default:                                                  return -1;
    }
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

        if (const int pad = padForNote (message.getNoteNumber()); pad >= 0)
            startVoice (pad, message.getFloatVelocity());
    }
    renderVoices (output, position, output.getNumSamples() - position);
}

void KitSampler::renderVoices (juce::AudioBuffer<float>& output, int start, int numSamples)
{
    if (numSamples <= 0)
        return;

    const int outChannels = output.getNumChannels();

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
            if (index + 1 >= length)
            {
                v.sample = nullptr;
                v.pad = -1;
                break;
            }

            const float frac = (float) (v.position - index);
            for (int ch = 0; ch < outChannels; ++ch)
            {
                const float* src = audio.getReadPointer (std::min (ch, srcChannels - 1));
                const float sample = src[index] + frac * (src[index + 1] - src[index]);
                output.addSample (ch, start + i, v.gain * sample);
            }
            v.position += v.increment;
        }
    }
}
} // namespace kitnap
