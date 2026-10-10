#pragma once

#include "DspCommon.h"
#include <array>

namespace orbit
{
// FREEZE: grabs the last second of audio and turns it into an endless, evolving drone
// by layering overlapping, windowed grains read from random spots in the captured slice.
class FreezeFx
{
public:
    void prepare (double sampleRateIn)
    {
        sr = (float) sampleRateIn;
        captureLength = (int) (sr * 1.0f);
        grainLength = (int) (sr * 0.18f);
        for (auto& h : history)
            h.prepare (captureLength + 8);
        for (auto& f : frozen)
            f.assign ((size_t) captureLength, 0.0f);
        crossfadeStep = 1.0f / (0.06f * sr);
        reset();
    }

    void reset()
    {
        for (auto& h : history) h.clear();
        frozenMix = 0.0f;
        isFrozen = false;
        for (size_t g = 0; g < grains.size(); ++g)
            grains[g] = { 0, (int) g * grainLength / (int) grains.size() };
    }

    void setFrozen (bool shouldFreeze)
    {
        if (shouldFreeze && ! isFrozen)
            capture();
        isFrozen = shouldFreeze;
    }

    bool isActive() const { return isFrozen || frozenMix > 0.0f; }

    void process (float& l, float& r)
    {
        history[0].push (l);
        history[1].push (r);

        const float target = isFrozen ? 1.0f : 0.0f;
        if (frozenMix != target)
            frozenMix = std::clamp (frozenMix + (isFrozen ? crossfadeStep : -crossfadeStep), 0.0f, 1.0f);

        if (frozenMix <= 0.0f)
            return;

        float droneL = 0.0f, droneR = 0.0f;
        for (auto& g : grains)
        {
            if (g.age >= grainLength)
            {
                g.age = 0;
                g.start = (int) (random.nextFloat() * (float) (captureLength - grainLength - 1));
            }

            const float phase = (float) g.age / (float) grainLength;
            const float window = 0.5f - 0.5f * std::cos (twoPi * phase);
            const size_t idx = (size_t) (g.start + g.age);
            droneL += frozen[0][idx] * window;
            droneR += frozen[1][idx] * window;
            ++g.age;
        }

        // Four Hann grains at 25% offsets sum to a constant gain of 2.
        droneL *= 0.5f;
        droneR *= 0.5f;

        l += frozenMix * (droneL - l);
        r += frozenMix * (droneR - r);
    }

private:
    struct Grain { int start = 0; int age = 0; };

    void capture()
    {
        for (size_t ch = 0; ch < 2; ++ch)
            for (int i = 0; i < captureLength; ++i)
                frozen[ch][(size_t) i] = history[ch].readInt (captureLength - i);

        for (size_t g = 0; g < grains.size(); ++g)
            grains[g] = { (int) (random.nextFloat() * (float) (captureLength - grainLength - 1)),
                          (int) g * grainLength / (int) grains.size() };
    }

    float sr = 44100.0f, frozenMix = 0.0f, crossfadeStep = 0.001f;
    int captureLength = 1, grainLength = 1;
    bool isFrozen = false;
    std::array<DelayLine, 2> history;
    std::array<std::vector<float>, 2> frozen;
    std::array<Grain, 4> grains;
    FastRandom random { 0xF00Du };
};
} // namespace orbit
