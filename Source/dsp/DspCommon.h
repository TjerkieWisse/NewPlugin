#pragma once

#include <cmath>
#include <vector>
#include <algorithm>

namespace orbit
{
constexpr float twoPi = 6.28318530717958647692f;

// Circular buffer with a fractional (linearly interpolated) read.
class DelayLine
{
public:
    void prepare (int maxSamples)
    {
        buffer.assign ((size_t) std::max (4, maxSamples + 2), 0.0f);
        writePos = 0;
    }

    void clear() { std::fill (buffer.begin(), buffer.end(), 0.0f); }

    void push (float x)
    {
        buffer[(size_t) writePos] = x;
        if (++writePos >= (int) buffer.size())
            writePos = 0;
    }

    // delaySamples = 1 returns the most recently pushed sample.
    float read (float delaySamples) const
    {
        const int size = (int) buffer.size();
        delaySamples = std::clamp (delaySamples, 1.0f, (float) (size - 2));
        float pos = (float) writePos - delaySamples;
        while (pos < 0.0f)
            pos += (float) size;

        const int i0 = (int) pos;
        const int i1 = (i0 + 1) % size;
        const float frac = pos - (float) i0;
        return buffer[(size_t) i0] + frac * (buffer[(size_t) i1] - buffer[(size_t) i0]);
    }

    float readInt (int delaySamples) const
    {
        const int size = (int) buffer.size();
        int pos = writePos - std::clamp (delaySamples, 1, size - 1);
        if (pos < 0)
            pos += size;
        return buffer[(size_t) pos];
    }

    int size() const { return (int) buffer.size(); }

private:
    std::vector<float> buffer;
    int writePos = 0;
};

class OnePoleLP
{
public:
    void setCutoff (float hz, float sampleRate)
    {
        coeff = 1.0f - std::exp (-twoPi * hz / sampleRate);
    }
    float process (float x)
    {
        state += coeff * (x - state);
        return state;
    }
    void reset() { state = 0.0f; }

private:
    float coeff = 1.0f, state = 0.0f;
};

class OnePoleHP
{
public:
    void setCutoff (float hz, float sampleRate) { lp.setCutoff (hz, sampleRate); }
    float process (float x) { return x - lp.process (x); }
    void reset() { lp.reset(); }

private:
    OnePoleLP lp;
};

// Schroeder allpass used for diffusion.
class Allpass
{
public:
    void prepare (int samples) { line.prepare (samples); length = samples; }
    void clear() { line.clear(); }
    float process (float x, float g = 0.6f)
    {
        const float delayed = line.readInt (length);
        const float v = x + g * delayed;
        line.push (v);
        return delayed - g * v;
    }

private:
    DelayLine line;
    int length = 1;
};

// Simple xorshift RNG that is safe to call from the audio thread.
class FastRandom
{
public:
    explicit FastRandom (unsigned int seed = 0x9E3779B9u) : state (seed ? seed : 1u) {}
    unsigned int next()
    {
        state ^= state << 13;
        state ^= state >> 17;
        state ^= state << 5;
        return state;
    }
    float nextFloat() { return (float) (next() >> 8) * (1.0f / 16777216.0f); }

private:
    unsigned int state;
};

inline float softClip (float x)
{
    constexpr float knee = 0.8f;
    const float a = std::abs (x);
    if (a <= knee)
        return x;
    const float shaped = knee + (1.0f - knee) * std::tanh ((a - knee) / (1.0f - knee));
    return x < 0.0f ? -shaped : shaped;
}
} // namespace orbit
