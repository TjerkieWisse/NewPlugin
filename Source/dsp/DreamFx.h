#pragma once

#include "DspCommon.h"
#include <array>

namespace orbit
{
// Delay-line pitch shifter: two taps sweep through a short window, crossfaded with a sine window.
class PitchShifter
{
public:
    void prepare (float sampleRate, float ratio)
    {
        windowSamples = 0.06f * sampleRate;
        line.prepare ((int) windowSamples + 8);
        phaseInc = (1.0f - ratio) / windowSamples;
        phase = 0.0f;
    }

    void clear() { line.clear(); }

    float process (float x)
    {
        line.push (x);
        phase += phaseInc;
        phase -= std::floor (phase);

        const float p2 = std::fmod (phase + 0.5f, 1.0f);
        const float a = line.read (1.0f + phase * windowSamples) * std::sin (phase * twoPi * 0.5f);
        const float b = line.read (1.0f + p2 * windowSamples) * std::sin (p2 * twoPi * 0.5f);
        return a + b;
    }

private:
    DelayLine line;
    float windowSamples = 1.0f, phaseInc = 0.0f, phase = 0.0f;
};

// "DREAM": a big shimmering reverb. An 8-line feedback delay network whose
// output is pitched up an octave and fed back in, so tails bloom upward.
class DreamFx
{
public:
    void prepare (double sampleRateIn)
    {
        sr = (float) sampleRateIn;
        const float scale = sr / 44100.0f;

        static constexpr std::array<int, numLines> baseLengths { 1557, 1617, 1491, 1422, 1277, 1356, 1188, 1116 };
        for (int i = 0; i < numLines; ++i)
        {
            lengths[(size_t) i] = (float) baseLengths[(size_t) i] * scale * 1.9f;
            lines[(size_t) i].prepare ((int) (lengths[(size_t) i] * 1.1f) + 64);
            damping[(size_t) i].setCutoff (6500.0f, sr);
        }

        static constexpr std::array<int, 4> diffLengths { 142, 107, 379, 277 };
        for (int ch = 0; ch < 2; ++ch)
            for (size_t i = 0; i < diffLengths.size(); ++i)
                diffusers[(size_t) ch][i].prepare ((int) ((float) (diffLengths[i] + ch * 23) * scale));

        for (auto& s : shifters)
            s.prepare (sr, 2.0f);
        for (auto& f : shimmerTone)
            f.setCutoff (5000.0f, sr);
        for (auto& f : shimmerLow)
            f.setCutoff (250.0f, sr);

        modPhase = 0.0f;
        clear();
    }

    void clear()
    {
        for (auto& l : lines) l.clear();
        for (auto& f : damping) f.reset();
        for (auto& ch : diffusers)
            for (auto& d : ch) d.clear();
        for (auto& s : shifters) s.clear();
        lastOut = { 0.0f, 0.0f };
    }

    void process (float inL, float inR, float& outL, float& outR)
    {
        // Shimmer: pitch the previous output up an octave and feed it back in.
        std::array<float, 2> shimmer {};
        for (size_t ch = 0; ch < 2; ++ch)
            shimmer[ch] = shimmerLow[ch].process (shimmerTone[ch].process (shifters[ch].process (lastOut[ch])));

        std::array<float, 2> in { inL + shimmerAmount * shimmer[0], inR + shimmerAmount * shimmer[1] };
        for (size_t ch = 0; ch < 2; ++ch)
            for (auto& d : diffusers[ch])
                in[ch] = d.process (in[ch], 0.65f);

        modPhase += 0.35f / sr;
        if (modPhase >= 1.0f)
            modPhase -= 1.0f;

        std::array<float, numLines> taps {};
        for (size_t i = 0; i < (size_t) numLines; ++i)
        {
            const float mod = 12.0f * std::sin (twoPi * (modPhase + (float) i / numLines));
            taps[i] = damping[i].process (lines[i].read (lengths[i] + mod));
        }

        // Householder mixing keeps the network lossless; decay sets the tail length.
        float sum = 0.0f;
        for (auto t : taps)
            sum += t;
        sum *= 2.0f / numLines;

        for (size_t i = 0; i < (size_t) numLines; ++i)
        {
            const float input = (i % 2 == 0) ? in[0] : in[1];
            lines[i].push (input * 0.5f + decay * (taps[i] - sum));
        }

        outL = 0.6f * (taps[0] - taps[2] + taps[4] - taps[6] + taps[1]);
        outR = 0.6f * (taps[1] - taps[3] + taps[5] - taps[7] + taps[2]);
        lastOut = { outL, outR };
    }

private:
    static constexpr int numLines = 8;
    static constexpr float decay = 0.93f;
    static constexpr float shimmerAmount = 0.32f;

    float sr = 44100.0f, modPhase = 0.0f;
    std::array<DelayLine, numLines> lines;
    std::array<float, numLines> lengths {};
    std::array<OnePoleLP, numLines> damping;
    std::array<std::array<Allpass, 4>, 2> diffusers;
    std::array<PitchShifter, 2> shifters;
    std::array<OnePoleLP, 2> shimmerTone;
    std::array<OnePoleHP, 2> shimmerLow;
    std::array<float, 2> lastOut {};
};
} // namespace orbit
