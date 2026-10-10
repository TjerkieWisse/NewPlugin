#pragma once

#include "DspCommon.h"
#include <array>

namespace orbit
{
// "SPACE": tempo-synced ping-pong delay with a darkening, slowly drifting feedback path.
class SpaceFx
{
public:
    void prepare (double sampleRateIn)
    {
        sr = (float) sampleRateIn;
        for (auto& l : lines)
            l.prepare ((int) (sr * 2.1f));
        for (auto& f : lowpass)
            f.setCutoff (4200.0f, sr);
        for (auto& f : highpass)
            f.setCutoff (180.0f, sr);
        smoothedDelay = targetDelay = 0.375f * sr;
        modPhase = 0.0f;
        clear();
    }

    void clear()
    {
        for (auto& l : lines) l.clear();
        for (auto& f : lowpass) f.reset();
        for (auto& f : highpass) f.reset();
    }

    // Dotted eighth notes at the host tempo; falls back to 375 ms.
    void setTempo (double bpm)
    {
        const float seconds = bpm > 20.0 ? (float) (60.0 / bpm) * 0.75f : 0.375f;
        targetDelay = std::clamp (seconds, 0.05f, 2.0f) * sr;
    }

    void process (float inL, float inR, float& outL, float& outR)
    {
        smoothedDelay += 0.0005f * (targetDelay - smoothedDelay);

        modPhase += 0.2f / sr;
        if (modPhase >= 1.0f)
            modPhase -= 1.0f;
        const float drift = 0.002f * sr * std::sin (twoPi * modPhase);

        const float tapL = lines[0].read (smoothedDelay + drift);
        const float tapR = lines[1].read (smoothedDelay - drift);

        const float fbL = highpass[0].process (lowpass[0].process (tapR));
        const float fbR = highpass[1].process (lowpass[1].process (tapL));

        // Input enters on the left only, so echoes bounce L -> R -> L.
        const float mono = 0.5f * (inL + inR);
        lines[0].push (softClip (mono + feedback * fbL));
        lines[1].push (softClip (feedback * fbR));

        outL = tapL + 0.15f * mono;
        outR = tapR + 0.15f * mono;
    }

private:
    static constexpr float feedback = 0.62f;
    float sr = 44100.0f, targetDelay = 1.0f, smoothedDelay = 1.0f, modPhase = 0.0f;
    std::array<DelayLine, 2> lines;
    std::array<OnePoleLP, 2> lowpass;
    std::array<OnePoleHP, 2> highpass;
};
} // namespace orbit
