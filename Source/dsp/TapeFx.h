#pragma once

#include "DspCommon.h"
#include <array>

namespace orbit
{
// "TAPE": a worn cassette. Wow and flutter pitch wobble, warm saturation,
// a rolled-off top end and a little bit-crush grit.
class TapeFx
{
public:
    void prepare (double sampleRateIn)
    {
        sr = (float) sampleRateIn;
        for (auto& l : lines)
            l.prepare ((int) (sr * 0.05f));
        for (auto& f : tone1) f.setCutoff (3800.0f, sr);
        for (auto& f : tone2) f.setCutoff (5200.0f, sr);
        for (auto& f : lowCut) f.setCutoff (60.0f, sr);
        wowPhase = flutterPhase = 0.0f;
        clear();
    }

    void clear()
    {
        for (auto& l : lines) l.clear();
        for (auto& f : tone1) f.reset();
        for (auto& f : tone2) f.reset();
        for (auto& f : lowCut) f.reset();
    }

    void process (float inL, float inR, float& outL, float& outR)
    {
        wowPhase += 0.55f / sr;
        flutterPhase += 6.3f / sr;
        wowPhase -= std::floor (wowPhase);
        flutterPhase -= std::floor (flutterPhase);

        // Wow drifts slowly, flutter shimmers quickly; the sum modulates a short delay.
        const float wow = std::sin (twoPi * wowPhase) + 0.35f * std::sin (twoPi * wowPhase * 2.7f);
        const float flutter = std::sin (twoPi * flutterPhase);
        const float delay = sr * (0.008f + 0.0025f * wow + 0.00012f * flutter);

        std::array<float, 2> in { inL, inR };
        std::array<float, 2> out {};
        for (size_t ch = 0; ch < 2; ++ch)
        {
            lines[ch].push (in[ch]);
            float x = lines[ch].read (delay + (ch == 0 ? 0.0f : 0.0004f * sr));

            x = std::tanh (drive * x) / std::tanh (drive);
            x = std::round (x * crushSteps) / crushSteps;
            x = tone2[ch].process (tone1[ch].process (x));
            out[ch] = lowCut[ch].process (x) * 1.15f;
        }

        outL = out[0];
        outR = out[1];
    }

private:
    static constexpr float drive = 2.2f;
    static constexpr float crushSteps = 512.0f;

    float sr = 44100.0f, wowPhase = 0.0f, flutterPhase = 0.0f;
    std::array<DelayLine, 2> lines;
    std::array<OnePoleLP, 2> tone1, tone2;
    std::array<OnePoleHP, 2> lowCut;
};
} // namespace orbit
