#pragma once

#include "DspCommon.h"
#include <array>

namespace orbit
{
// "GLITCH": a tempo-synced beat repeater. Every eighth note it rolls the dice and
// either passes audio through, stutters a 1/16 or 1/32 slice, plays it backwards,
// or repeats it an octave up.
class GlitchFx
{
public:
    void prepare (double sampleRateIn)
    {
        sr = (float) sampleRateIn;
        for (auto& b : history)
            b.prepare ((int) (sr * 4.0f));
        fadeSamples = std::max (16, (int) (0.002f * sr));
        setTempo (0.0);
        clear();
    }

    void clear()
    {
        for (auto& b : history) b.clear();
        mode = nextMode = Mode::pass;
        eventRemaining = 0;
    }

    void setTempo (double bpm)
    {
        const float beat = bpm > 20.0 ? (float) (60.0 / bpm) : 0.5f;
        stepSamples = std::max (64, (int) (beat * 0.5f * sr)); // eighth note
    }

    void process (float inL, float inR, float& outL, float& outR)
    {
        history[0].push (inL);
        history[1].push (inR);

        if (eventRemaining <= 0)
            startEvent();

        const int elapsed = eventLength - eventRemaining;
        --eventRemaining;

        if (mode == Mode::pass)
        {
            // Only fade when the neighbouring event is a glitch, so straight playback stays seamless.
            float gain = 1.0f;
            if (previousMode != Mode::pass)
                gain = std::min (gain, (float) elapsed / (float) fadeSamples);
            if (nextMode != Mode::pass)
                gain = std::min (gain, (float) eventRemaining / (float) fadeSamples);
            outL = inL * gain;
            outR = inR * gain;
            return;
        }

        // Position inside the current slice repetition.
        const float pos = std::fmod ((float) elapsed * rate, (float) sliceLength);
        const float readPos = mode == Mode::reverse ? (float) sliceLength - pos : pos;

        // The slice was captured when the event started; it now lives `elapsed` samples further back.
        const float delay = (float) (sliceLength + elapsed) - readPos;

        // Short fades at slice boundaries and at the event edges avoid clicks.
        const float edge = std::min ({ pos, (float) sliceLength - pos,
                                       (float) elapsed, (float) eventRemaining });
        const float gain = std::min (1.0f, edge / (float) fadeSamples);

        outL = history[0].read (delay) * gain;
        outR = history[1].read (delay) * gain;
    }

private:
    enum class Mode { pass, repeat16, repeat32, reverse, pitchUp };

    void startEvent()
    {
        eventLength = eventRemaining = stepSamples;
        previousMode = mode;
        mode = nextMode;
        nextMode = pickMode();

        rate = 1.0f;
        switch (mode)
        {
            case Mode::pass:     sliceLength = stepSamples; break;
            case Mode::repeat16: sliceLength = stepSamples / 2; break;
            case Mode::repeat32: sliceLength = stepSamples / 4; break;
            case Mode::reverse:  sliceLength = stepSamples; break;
            case Mode::pitchUp:  sliceLength = stepSamples / 2; rate = 2.0f; break;
        }
    }

    Mode pickMode()
    {
        const float r = random.nextFloat();
        if (r < 0.28f) return Mode::pass;
        if (r < 0.55f) return Mode::repeat16;
        if (r < 0.75f) return Mode::repeat32;
        if (r < 0.90f) return Mode::reverse;
        return Mode::pitchUp;
    }

    float sr = 44100.0f, rate = 1.0f;
    int stepSamples = 1, sliceLength = 1, eventLength = 1, eventRemaining = 0, fadeSamples = 64;
    Mode mode = Mode::pass, previousMode = Mode::pass, nextMode = Mode::pass;
    std::array<DelayLine, 2> history;
    FastRandom random { 0xC0FFEEu };
};
} // namespace orbit
