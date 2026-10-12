#include "DrumAnalyzer.h"
#include <juce_dsp/juce_dsp.h>
#include <algorithm>
#include <cmath>
#include <numeric>

namespace kitnap
{
namespace
{
constexpr float pi = juce::MathConstants<float>::pi;

//==============================================================================
// RBJ biquad, used for the analysis side-chains and the cleanup filters.
struct Biquad
{
    float b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0;

    static Biquad lowpass (double sr, double hz, double q = 0.7071) { return make (sr, hz, q, false); }
    static Biquad highpass (double sr, double hz, double q = 0.7071) { return make (sr, hz, q, true); }

    void process (float* data, int n) const
    {
        float z1 = 0.0f, z2 = 0.0f;
        for (int i = 0; i < n; ++i)
        {
            const float x = data[i];
            const float y = b0 * x + z1;
            z1 = b1 * x - a1 * y + z2;
            z2 = b2 * x - a2 * y;
            data[i] = y;
        }
    }

    void process (std::vector<float>& v) const { process (v.data(), (int) v.size()); }

    // Forward-backward filtering: no phase shift, so transients keep their shape.
    void processZeroPhase (float* data, int n) const
    {
        process (data, n);
        std::reverse (data, data + n);
        process (data, n);
        std::reverse (data, data + n);
    }

private:
    static Biquad make (double sr, double hz, double q, bool high)
    {
        const double w0 = 2.0 * juce::MathConstants<double>::pi * std::min (hz, sr * 0.45) / sr;
        const double alpha = std::sin (w0) / (2.0 * q);
        const double c = std::cos (w0);
        const double a0 = 1.0 + alpha;
        Biquad f;
        if (high)
        {
            f.b0 = (float) ((1.0 + c) / 2.0 / a0);
            f.b1 = (float) (-(1.0 + c) / a0);
        }
        else
        {
            f.b0 = (float) ((1.0 - c) / 2.0 / a0);
            f.b1 = (float) ((1.0 - c) / a0);
        }
        f.b2 = f.b0;
        f.a1 = (float) (-2.0 * c / a0);
        f.a2 = (float) ((1.0 - alpha) / a0);
        return f;
    }
};

//==============================================================================
struct Onset
{
    int position = 0;
    float strength = 0.0f;
};

struct HitInfo
{
    Onset onset;
    DrumClass drumClass = DrumClass::perc;
    int secondaryClass = -1; // e.g. a hat played together with a kick or bass note
    int nextOnsetDistance = 0;
};

struct ClassSettings
{
    float extractSeconds;   // length cut out of the song for each hit
    float compareSeconds;   // length used to judge whether two hits are the same sound
    float fadeOutSeconds;
};

ClassSettings settingsFor (DrumClass c)
{
    switch (c)
    {
        case DrumClass::kick:    return { 0.70f, 0.060f, 0.020f };
        case DrumClass::snare:   return { 0.60f, 0.050f, 0.020f };
        case DrumClass::clap:    return { 0.60f, 0.050f, 0.020f };
        case DrumClass::hat:     return { 0.25f, 0.025f, 0.010f };
        case DrumClass::openHat: return { 0.90f, 0.040f, 0.030f };
        case DrumClass::perc:    return { 0.50f, 0.040f, 0.020f };
    }
    return { 0.5f, 0.04f, 0.02f };
}

// Moving average of |x| via prefix sums, so any window length is O(1).
class Envelope
{
public:
    explicit Envelope (const std::vector<float>& x)
    {
        prefix.resize (x.size() + 1, 0.0);
        for (size_t i = 0; i < x.size(); ++i)
            prefix[i + 1] = prefix[i] + std::abs ((double) x[i]);
    }

    float average (int centre, int halfWidth) const
    {
        const int n = (int) prefix.size() - 1;
        const int a = std::clamp (centre - halfWidth, 0, n);
        const int b = std::clamp (centre + halfWidth + 1, 0, n);
        return b > a ? (float) ((prefix[(size_t) b] - prefix[(size_t) a]) / (b - a)) : 0.0f;
    }

private:
    std::vector<double> prefix;
};

//==============================================================================
std::vector<Onset> detectOnsets (const std::vector<float>& mono, double sr,
                                 const std::function<void (float)>& progress,
                                 const std::function<bool()>& cancelled)
{
    const int order = sr > 60000.0 ? 11 : 10;
    const int fftSize = 1 << order;
    const int hop = fftSize / 8;
    const int numBins = fftSize / 2 + 1;

    // Silence in front so a hit at the very start of the file is still an onset.
    const int padding = fftSize;
    std::vector<float> input ((size_t) padding, 0.0f);
    input.insert (input.end(), mono.begin(), mono.end());
    const int length = (int) input.size();
    if (length < fftSize)
        return {};

    juce::dsp::FFT fft (order);
    std::vector<float> window ((size_t) fftSize);
    for (int i = 0; i < fftSize; ++i)
        window[(size_t) i] = 0.5f - 0.5f * std::cos (2.0f * pi * (float) i / (float) fftSize);

    const int numFrames = (length - fftSize) / hop + 1;
    std::vector<float> flux ((size_t) numFrames, 0.0f);
    std::vector<float> frame ((size_t) fftSize * 2);
    std::vector<float> previous ((size_t) numBins, 0.0f);

    for (int f = 0; f < numFrames; ++f)
    {
        if ((f & 1023) == 0)
        {
            if (cancelled && cancelled())
                return {};
            if (progress)
                progress (0.35f * (float) f / (float) numFrames);
        }

        std::fill (frame.begin(), frame.end(), 0.0f);
        for (int i = 0; i < fftSize; ++i)
            frame[(size_t) i] = input[(size_t) (f * hop + i)] * window[(size_t) i];
        fft.performFrequencyOnlyForwardTransform (frame.data(), true);

        // Log-compressed spectral flux: quiet hats register as well as loud kicks.
        float sum = 0.0f;
        for (int b = 1; b < numBins; ++b)
        {
            const float mag = std::log1p (100.0f * frame[(size_t) b]);
            sum += std::max (0.0f, mag - previous[(size_t) b]);
            previous[(size_t) b] = mag;
        }
        flux[(size_t) f] = f == 0 ? 0.0f : sum;
    }

    const float maxFlux = *std::max_element (flux.begin(), flux.end());
    if (maxFlux <= 0.0f)
        return {};
    for (auto& v : flux)
        v /= maxFlux;

    // Adaptive threshold: local mean of the flux plus a small absolute floor.
    std::vector<double> prefix ((size_t) numFrames + 1, 0.0);
    for (int f = 0; f < numFrames; ++f)
        prefix[(size_t) f + 1] = prefix[(size_t) f] + flux[(size_t) f];

    const int meanHalf = std::max (4, (int) (0.05 * sr / hop));
    const int peakHalf = 3;
    // Energy prefix sums in three bands, used to pin each onset to the sample. Looking per band
    // lets a quiet hat on top of a loud bass line be located by its own rise.
    auto bandPrefix = [&] (std::initializer_list<Biquad> filters)
    {
        std::vector<float> band (input);
        for (auto& filter : filters)
            filter.process (band);
        std::vector<double> p (band.size() + 1, 0.0);
        for (size_t i = 0; i < band.size(); ++i)
            p[i + 1] = p[i] + (double) band[i] * band[i];
        return p;
    };
    const std::vector<double> risePrefixes[] {
        bandPrefix ({ Biquad::lowpass (sr, 300.0), Biquad::lowpass (sr, 300.0) }),
        bandPrefix ({ Biquad::highpass (sr, 300.0), Biquad::highpass (sr, 300.0), Biquad::lowpass (sr, 4000.0) }),
        bandPrefix ({ Biquad::highpass (sr, 4000.0), Biquad::highpass (sr, 4000.0) })
    };
    const int postWindow = std::max (8, (int) (0.004 * sr));
    const int preWindow = std::max (16, (int) (0.008 * sr));
    const int refineStep = std::max (1, (int) (0.0002 * sr));

    std::vector<Onset> onsets;
    for (int f = 1; f < numFrames - 1; ++f)
    {
        const float v = flux[(size_t) f];
        const int a = std::max (0, f - meanHalf), b = std::min (numFrames, f + meanHalf + 1);
        const float localMean = (float) ((prefix[(size_t) b] - prefix[(size_t) a]) / (b - a));
        if (v < 0.025f + 1.35f * localMean)
            continue;

        bool isPeak = true;
        for (int k = std::max (0, f - peakHalf); k <= std::min (numFrames - 1, f + peakHalf) && isPeak; ++k)
            if (flux[(size_t) k] > v || (k < f && ! (flux[(size_t) k] < v)))
                isPeak = false;
        if (! isPeak)
            continue;

        // Refine to the sample: the hit starts where the energy above the bass jumps the most
        // (energy in the next 4 ms compared with the 8 ms before).
        const int searchStart = std::max (preWindow, f * hop - hop);
        const int searchEnd = std::min (length - postWindow - 1, f * hop + fftSize - hop);
        int start = -1;
        float bestRise = 0.0f;
        for (auto& risePrefix : risePrefixes)
        {
            for (int i = searchStart; i <= searchEnd; i += refineStep)
            {
                const double post = (risePrefix[(size_t) (i + postWindow)] - risePrefix[(size_t) i]) / postWindow;
                const double pre = (risePrefix[(size_t) i] - risePrefix[(size_t) (i - preWindow)]) / preWindow;
                const float rise = (float) ((post + 1e-10) / (pre + 1e-10));
                if (rise > bestRise)
                {
                    bestRise = rise;
                    start = i;
                }
            }
        }
        if (start < 0)
            continue;
        start = std::max (0, start - padding);

        onsets.push_back ({ start, v });
    }

    // Merge onsets that landed on the same hit.
    std::sort (onsets.begin(), onsets.end(), [] (auto& x, auto& y) { return x.position < y.position; });
    std::vector<Onset> merged;
    const int minSpacing = (int) (0.03 * sr);
    for (auto& o : onsets)
    {
        if (! merged.empty() && o.position - merged.back().position < minSpacing)
        {
            if (o.strength > merged.back().strength)
                merged.back().strength = o.strength;
            continue;
        }
        merged.push_back (o);
    }
    return merged;
}

//==============================================================================
struct Features
{
    float sub = 0, low = 0, mid = 0, presence = 0, air = 0;
    float flatness = 0;
    int flamPeaks = 0;
    float highDecaySeconds = 0;
    float airRise = 0; // how much the band above 6 kHz grows at the hit
};

class FeatureExtractor
{
public:
    FeatureExtractor (const std::vector<float>& monoIn, const std::vector<float>& clapBandIn,
                      const std::vector<float>& highBandIn, double sampleRate)
        : mono (monoIn), clapEnv (clapBandIn), highEnv (highBandIn), sr (sampleRate),
          order (sampleRate > 60000.0 ? 13 : 12), fft (order)
    {
        buffer.resize ((size_t) (1 << order) * 2);
    }

    Features compute (int start, int nextOnsetDistance)
    {
        Features f;
        const int fftSize = 1 << order;
        const int length = std::min ({ fftSize, (int) (0.08 * sr), (int) mono.size() - start });
        if (length < 64)
            return f;

        // Compare the spectrum right after the hit with the one just before it, so only the
        // energy the hit adds counts (bass notes and chords ringing underneath are ignored).
        spectrum (start, length, true, after);
        spectrum (start - length, length, false, before);

        const int numBins = fftSize / 2 + 1;
        const double binHz = sr / fftSize;
        double bands[5] {};
        double airAfter = 0.0, airBefore = 0.0;
        // Third-octave bands from 300 Hz to 12 kHz, for a noisiness measure that survives the
        // background subtraction (individual bins are often zero, whole bands rarely are).
        constexpr int numFlatBands = 16;
        double flatBands[numFlatBands] {};
        int flatBins[numFlatBands] {};
        for (int b = 1; b < numBins; ++b)
        {
            const double hz = b * binHz;
            if (hz >= 6000.0)
            {
                airAfter += after[(size_t) b];
                airBefore += before[(size_t) b];
            }
            const double power = std::max (0.0, (double) after[(size_t) b] - 1.5 * (double) before[(size_t) b]);
            const int band = hz < 120 ? 0 : hz < 300 ? 1 : hz < 2000 ? 2 : hz < 6000 ? 3 : 4;
            if (hz >= 20.0)
                bands[band] += power;

            if (hz >= 300.0 && hz < std::min (12000.0, sr * 0.45))
            {
                const int fb = std::min (numFlatBands - 1, (int) (3.0 * std::log2 (hz / 300.0)));
                flatBands[fb] += power;
                ++flatBins[fb];
            }
        }

        const double total = bands[0] + bands[1] + bands[2] + bands[3] + bands[4] + 1e-12;
        f.sub = (float) (bands[0] / total);
        f.low = (float) (bands[1] / total);
        f.mid = (float) (bands[2] / total);
        f.presence = (float) (bands[3] / total);
        f.air = (float) (bands[4] / total);

        double densities[numFlatBands] {};
        double maxDensity = 0.0;
        int usedBands = 0;
        for (int fb = 0; fb < numFlatBands; ++fb)
            if (flatBins[fb] > 0)
            {
                densities[usedBands] = flatBands[fb] / flatBins[fb];
                maxDensity = std::max (maxDensity, densities[usedBands]);
                ++usedBands;
            }
        if (usedBands > 0 && maxDensity > 0.0)
        {
            double logSum = 0.0, linSum = 0.0;
            for (int i = 0; i < usedBands; ++i)
            {
                const double d = std::max (densities[i], maxDensity * 1e-4);
                logSum += std::log (d);
                linSum += d;
            }
            f.flatness = (float) (std::exp (logSum / usedBands) / (linSum / usedBands));
        }
        f.airRise = (float) ((airAfter + 1e-9) / (airBefore + 1e-9));

        f.flamPeaks = countFlams (start);
        f.highDecaySeconds = highDecay (start, nextOnsetDistance);
        return f;
    }

private:
    // Power spectrum of `length` samples from `start` (zero outside the file), normalised by window energy.
    // The hit window keeps the attack (1 ms fade-in) and fades over its second half; the
    // background window is a plain Hann.
    void spectrum (int start, int length, bool hitWindow, std::vector<float>& power)
    {
        const int fftSize = 1 << order;
        std::fill (buffer.begin(), buffer.end(), 0.0f);
        const int fadeIn = std::max (1, (int) (0.001 * sr));
        double windowEnergy = 0.0;
        for (int i = 0; i < length; ++i)
        {
            float w;
            if (! hitWindow)
                w = 0.5f - 0.5f * std::cos (2.0f * pi * (float) i / (float) length);
            else if (i < fadeIn)
                w = (float) i / (float) fadeIn;
            else if (i > length / 2)
                w = 0.5f + 0.5f * std::cos (pi * (float) (i - length / 2) / (float) (length - length / 2));
            else
                w = 1.0f;
            windowEnergy += (double) w * w;
            const int idx = start + i;
            if (idx >= 0 && idx < (int) mono.size())
                buffer[(size_t) i] = mono[(size_t) idx] * w;
        }
        fft.performFrequencyOnlyForwardTransform (buffer.data(), true);

        power.resize ((size_t) fftSize / 2 + 1);
        const float norm = windowEnergy > 0.0 ? (float) (1.0 / windowEnergy) : 0.0f;
        for (size_t b = 0; b < power.size(); ++b)
            power[b] = buffer[b] * buffer[b] * norm;
    }

    // Claps are several noise bursts a few milliseconds apart: count them.
    int countFlams (int start) const
    {
        const int half = std::max (1, (int) (0.001 * sr));
        const int span = (int) (0.045 * sr);
        const int step = std::max (1, (int) (0.0002 * sr));
        std::vector<float> e;
        for (int i = 0; i < span; i += step)
            e.push_back (clapEnv.average (start + i, half));
        if (e.empty())
            return 0;

        const float maxE = *std::max_element (e.begin(), e.end());
        if (maxE <= 0.0f)
            return 0;

        // A burst counts when the envelope rises to a peak after dipping well below the previous one.
        int count = 0;
        float lastPeak = 0.0f, dip = maxE;
        for (size_t i = 1; i + 1 < e.size(); ++i)
        {
            dip = std::min (dip, e[i]);
            const bool isPeak = e[i] >= e[i - 1] && e[i] > e[i + 1] && e[i] > 0.3f * maxE;
            if (! isPeak)
                continue;
            if (count == 0 || dip < 0.5f * std::min (lastPeak, e[i]))
            {
                ++count;
                lastPeak = e[i];
                dip = e[i];
            }
            else
            {
                lastPeak = std::max (lastPeak, e[i]);
            }
        }
        return count;
    }

    // How long the high band rings: separates open from closed hats.
    float highDecay (int start, int nextOnsetDistance) const
    {
        const int half = std::max (1, (int) (0.003 * sr));
        const int step = std::max (1, (int) (0.001 * sr));
        const int peakSpan = (int) (0.015 * sr);
        float peak = 0.0f;
        int peakPos = start;
        for (int i = start; i < start + peakSpan; i += step)
        {
            const float e = highEnv.average (i, half);
            if (e > peak) { peak = e; peakPos = i; }
        }
        if (peak <= 0.0f)
            return 0.0f;

        const int limit = start + std::min (nextOnsetDistance, (int) (1.0 * sr));
        for (int i = peakPos; i < limit; i += step)
            if (highEnv.average (i, half) < 0.2f * peak)
                return (float) ((i - start) / sr);
        return (float) ((limit - start) / sr);
    }

    const std::vector<float>& mono;
    Envelope clapEnv, highEnv;
    double sr;
    int order;
    juce::dsp::FFT fft;
    std::vector<float> buffer, before, after;
};

DrumClass classify (const Features& f)
{
    const float lowEnd = f.sub + f.low;
    const float top = f.presence + f.air;

    if (f.sub > 0.35f && f.sub > top && f.sub + 0.5f * f.low > f.mid)
        return DrumClass::kick;

    if (f.air > 0.5f && lowEnd < 0.15f)
        return f.highDecaySeconds > 0.15f ? DrumClass::openHat : DrumClass::hat;

    if (f.flatness > 0.2f && f.mid + top > 0.4f)
        return f.flamPeaks >= 3 ? DrumClass::clap : DrumClass::snare;

    if (top > 0.5f && lowEnd < 0.25f)
        return f.highDecaySeconds > 0.15f ? DrumClass::openHat : DrumClass::hat;

    return DrumClass::perc;
}

// A second drum played at the same moment as the main one, or -1.
int detectLayer (DrumClass primary, const Features& f)
{
    switch (primary)
    {
        case DrumClass::kick:
            // A snare or clap on the kick adds noisy mids; a hat adds a fresh, ringing top end.
            if (f.mid + f.presence > 0.25f && f.flatness > 0.25f)
                return (int) (f.flamPeaks >= 3 ? DrumClass::clap : DrumClass::snare);
            if (f.airRise > 8.0f && f.highDecaySeconds > 0.02f)
                return (int) (f.highDecaySeconds > 0.15f ? DrumClass::openHat : DrumClass::hat);
            return -1;

        case DrumClass::snare:
        case DrumClass::clap:
            // A snare's own noise reaches the top end too, so only a top end that rings on
            // well past the snare body counts as an open hat on top.
            return f.highDecaySeconds > 0.18f ? (int) DrumClass::openHat : -1;

        case DrumClass::perc:
            if (f.airRise > 8.0f && f.highDecaySeconds > 0.02f)
                return (int) (f.highDecaySeconds > 0.15f ? DrumClass::openHat : DrumClass::hat);
            return -1;

        case DrumClass::hat:
        case DrumClass::openHat:
            return f.sub > 0.25f ? (int) DrumClass::kick : -1;
    }
    return -1;
}

//==============================================================================
// Finds whether a drum (the "layer", e.g. an open hat) is inside another drum's sound
// (the "target", e.g. a snare that was always played together with it).
class LayerDetector
{
public:
    // Only content above highPassHz is compared: the layer's own range (e.g. above 2 kHz for hats).
    LayerDetector (const juce::AudioBuffer<float>& target, const juce::AudioBuffer<float>& layer, double sr, double highPassHz)
        : fft (order)
    {
        window.resize ((size_t) size);
        for (int i = 0; i < size; ++i)
            window[(size_t) i] = 0.5f - 0.5f * std::cos (2.0f * pi * (float) i / (float) size);

        auto filtered = [&] (const juce::AudioBuffer<float>& audio)
        {
            auto m = mono (audio);
            Biquad::highpass (sr, highPassHz).processZeroPhase (m.data(), (int) m.size());
            return m;
        };
        targetMag = magnitudes (filtered (target));
        layerMag = magnitudes (filtered (layer));
        targetFrames = std::max (0, (target.getNumSamples() - size / 2) / hop);
    }

    // How much of the layer is in the target, and at what level. A drum on its own fades in its own
    // way, so its level relative to the layer keeps changing; with the layer on top, the ratio
    // levels off at the layer's level once the target drum has died away.
    void estimate()
    {
        float peak = 0.0f;
        for (auto& frame : layerMag)
            for (float m : frame)
                peak = std::max (peak, m);
        if (peak <= 0.0f)
            return;

        std::vector<float> frameRatios;
        for (int f = 0; f < std::min (targetFrames, (int) layerMag.size()); ++f)
        {
            std::vector<float> ratios;
            for (int b = 1; b < bins; ++b)
                if (layerMag[(size_t) f][(size_t) b] > 0.1f * peak)
                    ratios.push_back (targetMag[(size_t) f][(size_t) b] / layerMag[(size_t) f][(size_t) b]);
            if (ratios.size() < 4)
                continue;
            std::nth_element (ratios.begin(), ratios.begin() + (std::ptrdiff_t) ratios.size() / 2, ratios.end());
            frameRatios.push_back (ratios[ratios.size() / 2]);
        }
        if (frameRatios.size() < 3)
            return;

        // A low percentile over time: the plateau, ignoring the frames where the target dominates.
        std::nth_element (frameRatios.begin(), frameRatios.begin() + (std::ptrdiff_t) frameRatios.size() / 4, frameRatios.end());
        gain = frameRatios[frameRatios.size() / 4];

        // Presence: does the layer's fine spectral detail (its exact pattern of peaks and dips, the
        // fingerprint of that particular sample) show up in the target? Each spectrum is compared
        // after removing its smooth overall shape, so two merely similar-sounding drums don't match.
        double weightedCorrelation = 0.0, weightSum = 0.0;
        for (int f = 0; f < std::min (targetFrames, (int) layerMag.size()); ++f)
        {
            // Weight each frame by how much of it the layer could account for: where the target drum
            // is much louder, the layer's detail is buried and says little either way.
            double layerEnergy = 0.0, targetEnergy = 0.0;
            std::vector<int> strong;
            for (int b = 1; b < bins; ++b)
                if (layerMag[(size_t) f][(size_t) b] > 0.05f * peak)
                {
                    strong.push_back (b);
                    const double l = gain * layerMag[(size_t) f][(size_t) b], t = targetMag[(size_t) f][(size_t) b];
                    layerEnergy += l * l;
                    targetEnergy += t * t;
                }
            if (strong.size() < 16 || targetEnergy <= 0.0)
                continue;
            const double frameEnergy = std::min (1.0, layerEnergy / targetEnergy);
            const auto t = detail (targetMag[(size_t) f], strong);
            const auto l = detail (layerMag[(size_t) f], strong);
            double tl = 0.0, tt = 0.0, ll = 0.0;
            for (size_t i = 0; i < t.size(); ++i)
            {
                tl += t[i] * l[i];
                tt += t[i] * t[i];
                ll += l[i] * l[i];
            }
            if (tt > 0.0 && ll > 0.0)
            {
                weightedCorrelation += frameEnergy * tl / std::sqrt (tt * ll);
                weightSum += frameEnergy;
            }
        }
        presence = weightSum > 0.0 ? (float) (weightedCorrelation / weightSum) : 0.0f;
    }

    float getGain() const { return gain; }
    float getPresence() const { return presence; }

private:
    static constexpr int order = 10, size = 1 << order, hop = size / 4, bins = size / 2 + 1;

    // Log magnitude minus its local average (+-6 bins), at the given bins.
    static std::vector<double> detail (const std::vector<float>& mag, const std::vector<int>& at)
    {
        std::vector<double> out;
        out.reserve (at.size());
        for (int b : at)
        {
            double local = 0.0;
            int count = 0;
            for (int k = std::max (1, b - 6); k <= std::min (bins - 1, b + 6); ++k, ++count)
                local += std::log (mag[(size_t) k] + 1e-9);
            out.push_back (std::log (mag[(size_t) b] + 1e-9) - local / count);
        }
        return out;
    }

    static std::vector<float> mono (const juce::AudioBuffer<float>& audio)
    {
        std::vector<float> m ((size_t) audio.getNumSamples(), 0.0f);
        for (int ch = 0; ch < audio.getNumChannels(); ++ch)
            for (int i = 0; i < audio.getNumSamples(); ++i)
                m[(size_t) i] += audio.getSample (ch, i);
        return m;
    }

    void transform (const float* x, int n, int frame, std::vector<float>& out) const
    {
        out.assign ((size_t) size * 2, 0.0f);
        for (int i = 0; i < size; ++i)
        {
            const int idx = frame * hop + i - size / 2;
            if (idx >= 0 && idx < n)
                out[(size_t) i] = x[idx] * window[(size_t) i];
        }
        fft.performRealOnlyForwardTransform (out.data(), true);
    }

    std::vector<std::vector<float>> magnitudes (const std::vector<float>& x) const
    {
        const int frames = ((int) x.size() + size) / hop;
        std::vector<std::vector<float>> result ((size_t) frames, std::vector<float> ((size_t) bins));
        std::vector<float> spectrum;
        for (int f = 0; f < frames; ++f)
        {
            transform (x.data(), (int) x.size(), f, spectrum);
            for (int b = 0; b < bins; ++b)
                result[(size_t) f][(size_t) b] = std::hypot (spectrum[(size_t) b * 2], spectrum[(size_t) b * 2 + 1]);
        }
        return result;
    }

    juce::dsp::FFT fft;
    std::vector<float> window;
    std::vector<std::vector<float>> targetMag, layerMag;
    int targetFrames = 0;
    float gain = 0.0f, presence = 0.0f;
};

// Removes the layer by subtracting its waveform. In a produced track a layered drum is the same
// sample every time, so once lined up to the sample it cancels almost completely, while the
// target drum (a different waveform) is left alone. Works on the raw stacks, before any filtering.
// `offset` places the layer later in the target (a drum that follows, e.g. a hat 1/16 after the
// snare). Returns how much the target's energy in the layer's band dropped where the layer sits, in dB.
float subtractLayer (juce::AudioBuffer<float>& target, int targetLength, const juce::AudioBuffer<float>& rawLayer,
                     double sr, double highPassHz, int offset = 0)
{
    const int n = std::min ({ targetLength, target.getNumSamples(), offset + rawLayer.getNumSamples() });
    const int maxLag = (int) (0.003 * sr);
    if (n - offset <= 2 * maxLag)
        return 0.0f;

    // Only the layer's own range is subtracted (the stacks also hold bass and other music below it),
    // and the level is fitted in that range only.
    juce::AudioBuffer<float> layer (rawLayer), band (target);
    for (int ch = 0; ch < layer.getNumChannels(); ++ch)
        Biquad::highpass (sr, highPassHz).processZeroPhase (layer.getWritePointer (ch), layer.getNumSamples());
    for (int ch = 0; ch < band.getNumChannels(); ++ch)
        Biquad::highpass (sr, highPassHz).processZeroPhase (band.getWritePointer (ch), band.getNumSamples());

    // Best alignment on the channel sums.
    auto sum = [] (const juce::AudioBuffer<float>& b, int i)
    {
        float v = 0.0f;
        for (int ch = 0; ch < b.getNumChannels(); ++ch)
            v += b.getSample (ch, i);
        return v;
    };
    int bestLag = 0;
    double bestScore = 0.0;
    const int from = std::max (maxLag, offset + maxLag);
    for (int lag = -maxLag; lag <= maxLag; ++lag)
    {
        double cross = 0.0, energy = 0.0;
        for (int i = from; i < n - maxLag; ++i)
        {
            const int j = i - offset - lag;
            if (j < 0 || j >= layer.getNumSamples())
                continue;
            const float l = sum (layer, j);
            cross += (double) sum (band, i) * l;
            energy += (double) l * l;
        }
        const double score = energy > 0.0 ? cross / std::sqrt (energy) : 0.0;
        if (score > bestScore)
        {
            bestScore = score;
            bestLag = lag;
        }
    }

    // Least-squares level per channel, then subtract.
    double before = 0.0, after = 0.0;
    for (int ch = 0; ch < target.getNumChannels(); ++ch)
    {
        const float* l = layer.getReadPointer (std::min (ch, layer.getNumChannels() - 1));
        const float* b = band.getReadPointer (ch);
        float* t = target.getWritePointer (ch);
        double cross = 0.0, energy = 0.0;
        for (int i = std::max (0, offset); i < n; ++i)
            if (const int j = i - offset - bestLag; j >= 0 && j < layer.getNumSamples())
            {
                cross += (double) b[i] * l[j];
                energy += (double) l[j] * l[j];
            }
        const float gain = energy > 0.0 ? (float) std::max (0.0, cross / energy) : 0.0f;
        for (int i = std::max (0, offset); i < n; ++i)
            if (const int j = i - offset - bestLag; j >= 0 && j < layer.getNumSamples())
            {
                t[i] -= gain * l[j];
                const double residual = b[i] - gain * l[j];
                before += (double) b[i] * b[i];
                after += residual * residual;
            }
    }
    return before > 0.0 && after > 0.0 ? (float) (10.0 * std::log10 (before / after)) : 0.0f;
}

// Continues `sound` beyond `length` with `tail` (a stack of the same drum from hits that could ring
// out), level-matched just before the splice and crossfaded. Updates `length`.
void graftTail (juce::AudioBuffer<float>& sound, int& length, const juce::AudioBuffer<float>& tail, double sr)
{
    const int fade = (int) (0.012 * sr);
    const int match = (int) (0.03 * sr);
    const int spliceEnd = length;
    const int spliceStart = spliceEnd - fade;
    if (tail.getNumSamples() <= length || spliceStart - match < 0 || sound.getNumSamples() < tail.getNumSamples())
        return;

    for (int ch = 0; ch < sound.getNumChannels(); ++ch)
    {
        const float* t = tail.getReadPointer (std::min (ch, tail.getNumChannels() - 1));
        float* y = sound.getWritePointer (ch);

        // Level of the tail stack relative to the main stack, just before the splice.
        double cross = 0.0, energy = 0.0;
        for (int i = spliceStart - match; i < spliceStart; ++i)
        {
            cross += (double) y[i] * t[i];
            energy += (double) t[i] * t[i];
        }
        const float gain = energy > 0.0 ? (float) juce::jlimit (0.0, 4.0, cross / energy) : 0.0f;

        for (int i = spliceStart; i < tail.getNumSamples(); ++i)
        {
            const float mix = i < spliceEnd ? 0.5f - 0.5f * std::cos (pi * (float) (i - spliceStart) / (float) fade) : 1.0f;
            y[i] = (1.0f - mix) * y[i] + mix * gain * t[i];
        }
    }
    length = tail.getNumSamples();
}

// For noisy drums (snare, clap, hats) that had to be cut while still ringing: continue the sound with
// noise shaped like its last moments, decaying at the rate it was already decaying. Their tails are
// essentially shaped noise, so the continuation sounds natural instead of chopped.
void extendTail (juce::AudioBuffer<float>& sound, double sr, int seed, double minDecayDbPerSecond, int maxLength)
{
    const int n = sound.getNumSamples();
    const int order = n >= 2048 ? 11 : 10, size = 1 << order, hop = size / 4;
    const int span = std::min ((int) (0.03 * sr), n / 4);
    if (n < size)
        return;

    auto rms = [&] (int from, int count)
    {
        double sum = 0.0;
        for (int ch = 0; ch < sound.getNumChannels(); ++ch)
            for (int i = from; i < from + count; ++i)
                sum += (double) sound.getSample (ch, i) * sound.getSample (ch, i);
        return std::sqrt (sum / (count * sound.getNumChannels()) + 1e-20);
    };

    // Still ringing? (the end is within 30 dB of the loudest part)
    double peak = 0.0;
    for (int from = 0; from + span <= n; from += span / 2)
        peak = std::max (peak, rms (from, span));
    const double endLevel = rms (n - span, span);
    if (peak <= 0.0 || endLevel < peak * 0.03)
        return;

    // Decay rate from the last two windows, kept within what drums actually do.
    const double previous = rms (n - 3 * span, span);
    double decayDbPerSecond = 20.0 * std::log10 (previous / endLevel) / (2.0 * span / sr);
    decayDbPerSecond = juce::jlimit (minDecayDbPerSecond, 400.0, decayDbPerSecond);
    const int extra = std::min (maxLength - n, (int) (sr * 50.0 / decayDbPerSecond)); // down to about -50 dB
    if (extra < hop)
        return;

    juce::dsp::FFT fft (order);
    std::vector<float> window ((size_t) size), spectrum ((size_t) size * 2);
    for (int i = 0; i < size; ++i)
        window[(size_t) i] = 0.5f - 0.5f * std::cos (2.0f * pi * (float) i / (float) size);

    juce::Random random (seed);
    const int fade = (int) (0.01 * sr);
    const int total = n + extra;
    juce::AudioBuffer<float> out (sound.getNumChannels(), total);
    out.clear();

    for (int ch = 0; ch < sound.getNumChannels(); ++ch)
    {
        // The magnitude spectrum of the last moments.
        std::fill (spectrum.begin(), spectrum.end(), 0.0f);
        for (int i = 0; i < size; ++i)
            spectrum[(size_t) i] = sound.getSample (ch, n - size + i) * window[(size_t) i];
        fft.performFrequencyOnlyForwardTransform (spectrum.data(), true);
        std::vector<float> magnitude (spectrum.begin(), spectrum.begin() + size / 2 + 1);

        // Random-phase resynthesis with overlap-add, starting just before the cut for the crossfade.
        std::vector<float> noise ((size_t) (extra + fade + size), 0.0f), norm (noise.size(), 0.0f);
        for (int start = 0; start + size <= (int) noise.size(); start += hop)
        {
            std::fill (spectrum.begin(), spectrum.end(), 0.0f);
            for (int b = 1; b < size / 2; ++b)
            {
                const float phase = random.nextFloat() * 2.0f * pi;
                spectrum[(size_t) b * 2] = magnitude[(size_t) b] * std::cos (phase);
                spectrum[(size_t) b * 2 + 1] = magnitude[(size_t) b] * std::sin (phase);
            }
            fft.performRealOnlyInverseTransform (spectrum.data());
            for (int i = 0; i < size; ++i)
            {
                noise[(size_t) (start + i)] += spectrum[(size_t) i] * window[(size_t) i];
                norm[(size_t) (start + i)] += window[(size_t) i] * window[(size_t) i];
            }
        }

        // Match the level at the cut, decay from there, crossfade in.
        double noiseLevel = 0.0;
        for (int i = size / 2; i < size / 2 + span; ++i)
        {
            const float v = norm[(size_t) i] > 1e-6f ? noise[(size_t) i] / norm[(size_t) i] : 0.0f;
            noiseLevel += (double) v * v;
        }
        noiseLevel = std::sqrt (noiseLevel / span + 1e-20);
        double channelEnd = 0.0;
        for (int i = n - span; i < n; ++i)
            channelEnd += (double) sound.getSample (ch, i) * sound.getSample (ch, i);
        channelEnd = std::sqrt (channelEnd / span);
        const float scale = (float) (channelEnd / noiseLevel);

        float* y = out.getWritePointer (ch);
        std::copy (sound.getReadPointer (ch), sound.getReadPointer (ch) + n, y);
        for (int i = n - fade; i < total; ++i)
        {
            const size_t j = (size_t) (i - (n - fade) + size / 2);
            const float v = j < noise.size() && norm[j] > 1e-6f ? noise[j] / norm[j] : 0.0f;
            const float seconds = (float) std::max (0, i - n) / (float) sr;
            const float decay = std::pow (10.0f, -(float) decayDbPerSecond * seconds / 20.0f);
            const float mix = i < n ? 0.5f - 0.5f * std::cos (pi * (float) (i - (n - fade)) / (float) fade) : 1.0f;
            y[i] = (i < n ? (1.0f - mix) * y[i] : 0.0f) + mix * scale * v * decay;
        }
    }
    sound = std::move (out);
}

// A snare or clap's top end fades along with its body. When the top end keeps ringing well above
// that (an open hat or shaker played on top, which no clean copy exists of), remove the ringing from
// under the drum, and end the sound where the drum's body has died away: past that point the layer
// is all there is (the tail is then continued by extendTail). Returns the new length. If `layerOut`
// is given and the sound was cut, the removed layer is written there (the top end only).
int tameRingingTop (juce::AudioBuffer<float>& sound, double sr, juce::AudioBuffer<float>* layerOut = nullptr)
{
    constexpr int order = 10, size = 1 << order, hop = size / 4, bins = size / 2 + 1;
    const int n = sound.getNumSamples();
    const int frames = (n + size) / hop;
    if (n < size)
        return n;

    juce::dsp::FFT fft (order);
    std::vector<float> window ((size_t) size);
    for (int i = 0; i < size; ++i)
        window[(size_t) i] = 0.5f - 0.5f * std::cos (2.0f * pi * (float) i / (float) size);

    auto transform = [&] (const float* x, int frame, std::vector<float>& out)
    {
        out.assign ((size_t) size * 2, 0.0f);
        for (int i = 0; i < size; ++i)
            if (const int idx = frame * hop + i - size / 2; idx >= 0 && idx < n)
                out[(size_t) i] = x[idx] * window[(size_t) i];
        fft.performRealOnlyForwardTransform (out.data(), true);
    };

    // Power per frame: the body (1.2 - 3 kHz: the snare or clap's own range, above most bass and
    // chords that may still be under it, below most of a hat) and four top-end bands from 4 kHz up.
    const double binHz = sr / size;
    constexpr int topBands = 4;
    auto topBandOf = [&] (int b) { return juce::jlimit (0, topBands - 1, (int) (std::log2 (b * binHz / 4000.0) * 2.0)); };
    std::vector<float> mono ((size_t) n, 0.0f);
    for (int ch = 0; ch < sound.getNumChannels(); ++ch)
        for (int i = 0; i < n; ++i)
            mono[(size_t) i] += sound.getSample (ch, i);

    std::vector<double> body ((size_t) frames, 1e-20);
    std::vector<std::array<double, topBands>> top ((size_t) frames);
    std::vector<float> spectrum;
    for (int f = 0; f < frames; ++f)
    {
        transform (mono.data(), f, spectrum);
        top[(size_t) f].fill (1e-20);
        for (int b = 1; b < bins; ++b)
        {
            const double hz = b * binHz;
            const double power = (double) spectrum[(size_t) b * 2] * spectrum[(size_t) b * 2] + (double) spectrum[(size_t) b * 2 + 1] * spectrum[(size_t) b * 2 + 1];
            if (hz >= 1200.0 && hz < 3000.0)
                body[(size_t) f] += power;
            else if (hz >= 4000.0)
                top[(size_t) f][(size_t) topBandOf (b)] += power;
        }
    }

    // Model each top band as two decays added together: a fast one (the drum itself, fading like its
    // body) and a slow one (the ringing layer). Once the body has died away (20 dB down), what is left
    // in the top end is the layer: fit its decay there, extend it back to the start, and remove it.
    double bodyPeak = 0.0;
    int bodyPeakFrame = 0;
    for (int f = 0; f < frames; ++f)
        if (body[(size_t) f] > bodyPeak)
        {
            bodyPeak = body[(size_t) f];
            bodyPeakFrame = f;
        }
    int quietFrame = frames;
    for (int f = bodyPeakFrame; f < frames; ++f)
        if (body[(size_t) f] < bodyPeak * 0.01)
        {
            quietFrame = f;
            break;
        }
    const int lastFrame = (n - size / 2) / hop; // frames past the end only see the fade
    if (quietFrame >= lastFrame - 3)
        return n; // the body rings to the end: nothing can be told apart

    const double bodyDecayPerFrame = 10.0 * std::log10 (bodyPeak / body[(size_t) quietFrame]) / std::max (1, quietFrame - bodyPeakFrame);
    std::vector<std::array<float, topBands>> gains ((size_t) frames);
    for (auto& g : gains)
        g.fill (1.0f);
    int ringingBands = 0;

    for (int band = 0; band < topBands; ++band)
    {
        double topPeakPower = 1e-20;
        for (auto& t : top)
            topPeakPower = std::max (topPeakPower, t[(size_t) band]);
        const double topPeak = 10.0 * std::log10 (topPeakPower);

        // Straight-line fit of the band's level (dB) over the quiet part, while still clearly audible.
        double sumT = 0.0, sumY = 0.0, sumTT = 0.0, sumTY = 0.0;
        int count = 0;
        for (int f = quietFrame; f < lastFrame; ++f)
        {
            const double y = 10.0 * std::log10 (top[(size_t) f][(size_t) band]);
            if (y < topPeak - 45.0)
                break;
            sumT += f; sumY += y; sumTT += (double) f * f; sumTY += f * y;
            ++count;
        }
        if (count < 4)
            continue;
        const double slope = std::min (0.0, (count * sumTY - sumT * sumY) / (count * sumTT - sumT * sumT));
        const double intercept = (sumY - slope * sumT) / count;

        // A ringing layer decays clearly slower than the body (an open hat or shaker; a snare or clap's
        // own sizzle fades about as fast as its body), never faster than 300 dB per second, and is
        // still clearly audible.
        const double layerAtQuiet = intercept + slope * quietFrame;
        const double maxLayerDecayPerFrame = std::min (0.75 * bodyDecayPerFrame, 300.0 * hop / sr);
        if (-slope > maxLayerDecayPerFrame || layerAtQuiet < topPeak - 30.0)
            continue;

        for (int f = 0; f < frames; ++f)
        {
            const double layer = std::pow (10.0, (intercept + slope * std::max (f, bodyPeakFrame)) / 10.0);
            const double total = top[(size_t) f][(size_t) band];
            gains[(size_t) f][(size_t) band] = (float) std::sqrt (std::max (0.01, 1.0 - layer / total));
        }
        ++ringingBands;
    }
    if (ringingBands == 0)
        return n;

    // Clearly ringing across the top end: past the body, the layer is all there is.
    const bool cut = ringingBands >= 2;
    const bool keepLayer = layerOut != nullptr && cut;
    if (layerOut != nullptr)
        layerOut->setSize (keepLayer ? sound.getNumChannels() : 0, keepLayer ? n : 0);

    for (int ch = 0; ch < sound.getNumChannels(); ++ch)
    {
        std::vector<float> out ((size_t) n, 0.0f), layer ((size_t) n, 0.0f), norm ((size_t) n, 0.0f), removed;
        const float* x = sound.getReadPointer (ch);
        for (int f = 0; f < frames; ++f)
        {
            transform (x, f, spectrum);
            removed.assign ((size_t) size * 2, 0.0f);
            for (int b = 1; b < bins; ++b)
                if (b * binHz >= 4000.0)
                {
                    // Everything the drum keeps is taken off the layer (and vice versa).
                    const float g = gains[(size_t) f][(size_t) topBandOf (b)];
                    const float rest = std::sqrt (std::max (0.0f, 1.0f - g * g));
                    removed[(size_t) b * 2] = spectrum[(size_t) b * 2] * rest;
                    removed[(size_t) b * 2 + 1] = spectrum[(size_t) b * 2 + 1] * rest;
                    spectrum[(size_t) b * 2] *= g;
                    spectrum[(size_t) b * 2 + 1] *= g;
                }
            fft.performRealOnlyInverseTransform (spectrum.data());
            fft.performRealOnlyInverseTransform (removed.data());
            for (int i = 0; i < size; ++i)
                if (const int idx = f * hop + i - size / 2; idx >= 0 && idx < n)
                {
                    out[(size_t) idx] += spectrum[(size_t) i] * window[(size_t) i];
                    layer[(size_t) idx] += removed[(size_t) i] * window[(size_t) i];
                    norm[(size_t) idx] += window[(size_t) i] * window[(size_t) i];
                }
        }
        float* y = sound.getWritePointer (ch);
        for (int i = 0; i < n; ++i)
        {
            const bool covered = norm[(size_t) i] > 1e-6f;
            if (keepLayer)
                layerOut->setSample (ch, i, covered ? layer[(size_t) i] / norm[(size_t) i] : 0.0f);
            y[i] = covered ? out[(size_t) i] / norm[(size_t) i] : y[i];
        }
    }
    return cut ? std::min (n, quietFrame * hop) : n;
}

// How much the energy jumps at `position` (next 10 ms vs previous 10 ms), in dB, above highPassHz.
float attackAt (const juce::AudioBuffer<float>& audio, int position, double sr, double highPassHz)
{
    const int window = (int) (0.01 * sr);
    const int start = std::max (0, position - 4 * window);
    const int end = std::min (audio.getNumSamples(), position + 4 * window);
    if (position - window < start || position + window > end)
        return 0.0f;

    std::vector<float> m ((size_t) (end - start), 0.0f);
    for (int ch = 0; ch < audio.getNumChannels(); ++ch)
        for (int i = start; i < end; ++i)
            m[(size_t) (i - start)] += audio.getSample (ch, i);
    Biquad::highpass (sr, highPassHz).processZeroPhase (m.data(), (int) m.size());

    // The strongest rise within +-5 ms of the expected position (the hit may sit slightly off).
    float best = 0.0f;
    const int at = position - start;
    for (int c = at - window / 2; c <= at + window / 2; c += std::max (1, window / 20))
    {
        double before = 1e-12, after = 1e-12;
        for (int i = std::max (0, c - window); i < c; ++i)
            before += (double) m[(size_t) i] * m[(size_t) i];
        for (int i = c; i < std::min ((int) m.size(), c + window); ++i)
            after += (double) m[(size_t) i] * m[(size_t) i];
        best = std::max (best, (float) (10.0 * std::log10 (after / before)));
    }
    return best;
}

//==============================================================================
// Normalised cross-correlation of two hits on a class-specific side-chain, with a small lag search.
struct Match
{
    float correlation = 0.0f;
    int lag = 0;
};

Match compare (const std::vector<float>& signal, int a, int b, int length, int maxLag)
{
    const int n = (int) signal.size();
    Match best;
    double energyA = 0.0;
    for (int i = 0; i < length; ++i)
        if (a + i < n)
            energyA += (double) signal[(size_t) (a + i)] * signal[(size_t) (a + i)];
    if (energyA <= 0.0)
        return best;

    for (int lag = -maxLag; lag <= maxLag; ++lag)
    {
        const int bStart = b + lag;
        if (bStart < 0 || bStart + length >= n || a + length >= n)
            continue;
        double cross = 0.0, energyB = 0.0;
        for (int i = 0; i < length; ++i)
        {
            const float y = signal[(size_t) (bStart + i)];
            cross += (double) signal[(size_t) (a + i)] * y;
            energyB += (double) y * y;
        }
        if (energyB <= 0.0)
            continue;
        const float c = (float) (cross / std::sqrt (energyA * energyB));
        if (c > best.correlation)
            best = { c, lag };
    }
    return best;
}

struct Cluster
{
    std::vector<int> members; // indices into the class candidate list
    std::vector<int> lags;
    float score = 0.0f;
};

//==============================================================================
void applyFades (juce::AudioBuffer<float>& audio, int fadeIn, int fadeOut)
{
    const int n = audio.getNumSamples();
    for (int ch = 0; ch < audio.getNumChannels(); ++ch)
    {
        auto* d = audio.getWritePointer (ch);
        for (int i = 0; i < std::min (fadeIn, n); ++i)
            d[i] *= (float) i / (float) fadeIn;
        for (int i = 0; i < std::min (fadeOut, n); ++i)
            d[n - 1 - i] *= 0.5f - 0.5f * std::cos (pi * (float) i / (float) fadeOut);
    }
}

void cleanUp (juce::AudioBuffer<float>& audio, DrumClass c, double sr)
{
    const int n = audio.getNumSamples();
    for (int ch = 0; ch < audio.getNumChannels(); ++ch)
    {
        auto* d = audio.getWritePointer (ch);
        Biquad::highpass (sr, 25.0).processZeroPhase (d, n);

        switch (c)
        {
            case DrumClass::kick:
            {
                // After the click, a kick is all low end: fade to a low-passed copy to remove hat and synth bleed.
                std::vector<float> dark (d, d + n);
                Biquad::lowpass (sr, 1800.0).processZeroPhase (dark.data(), n);
                const int from = (int) (0.025 * sr), to = (int) (0.06 * sr);
                for (int i = from; i < n; ++i)
                {
                    const float t = std::min (1.0f, (float) (i - from) / (float) (to - from));
                    d[i] += t * (dark[(size_t) i] - d[i]);
                }
                break;
            }
            case DrumClass::snare:
            case DrumClass::clap:
                Biquad::highpass (sr, 110.0).processZeroPhase (d, n);
                break;
            case DrumClass::hat:
            case DrumClass::openHat:
                Biquad::highpass (sr, 2500.0).processZeroPhase (d, n);
                break;
            case DrumClass::perc:
                Biquad::highpass (sr, 80.0).processZeroPhase (d, n);
                break;
        }
    }
}

// Cut the tail where the sound has died away, or where the next hit of the song bleeds in.
int findEnd (const juce::AudioBuffer<float>& audio, double sr)
{
    const int n = audio.getNumSamples();
    std::vector<float> mono ((size_t) n, 0.0f);
    for (int ch = 0; ch < audio.getNumChannels(); ++ch)
        for (int i = 0; i < n; ++i)
            mono[(size_t) i] += audio.getSample (ch, i);

    // New hits are looked for above 300 Hz: every drum attack has energy there, while the slow
    // beating of a deep kick or 808 (which looks like new attacks in the full band) doesn't.
    std::vector<float> upper (mono);
    Biquad::highpass (sr, 300.0).processZeroPhase (upper.data(), n);

    // Silence is judged on a smoothed full-band envelope (longer than one bass cycle).
    Envelope env (mono), upperEnv (upper);
    const int smooth = std::max (1, (int) (0.012 * sr));
    const int half = std::max (1, (int) (0.004 * sr));
    const int step = std::max (1, (int) (0.001 * sr));

    float peak = 0.0f, upperPeak = 0.0f;
    int upperPeakPos = 0;
    for (int i = 0; i < std::min (n, (int) (0.03 * sr)); i += step)
    {
        peak = std::max (peak, env.average (i, smooth));
        if (upperEnv.average (i, half) > upperPeak)
        {
            upperPeak = upperEnv.average (i, half);
            upperPeakPos = i;
        }
    }
    if (peak <= 0.0f)
        return n;

    const float silence = peak * juce::Decibels::decibelsToGain (-50.0f);
    float minSince = upperPeak;
    const int lookAhead = (int) (0.012 * sr);
    for (int i = upperPeakPos; i < n; i += step)
    {
        if (env.average (i, smooth) < silence)
            return std::min (n, i + (int) (0.01 * sr));

        // A new attack: the upper band decayed, then jumps back up quickly.
        const float e = upperEnv.average (i, half);
        minSince = std::min (minSince, e);
        // It has to be a real hit: reaching at least 15% of this drum's own attack in that band, and
        // standing out against the whole sound there (not just bass harmonics wobbling under a kick).
        if (upperPeak > 0.0f && minSince < 0.35f * upperPeak && e < 0.5f * upperPeak && i + lookAhead < n)
        {
            const float ahead = upperEnv.average (i + lookAhead, half);
            if (ahead > 2.2f * e && ahead > 0.15f * upperPeak && ahead > 0.1f * env.average (i + lookAhead, half))
                return i + lookAhead / 2;
        }
    }
    return n;
}

void normalise (juce::AudioBuffer<float>& audio)
{
    const float peak = audio.getMagnitude (0, audio.getNumSamples());
    if (peak > 0.0f)
        audio.applyGain (juce::Decibels::decibelsToGain (-0.5f) / peak);
}
} // namespace

//==============================================================================
AnalysisResult DrumAnalyzer::analyse (const juce::AudioBuffer<float>& source, double sr,
                                      const std::function<void (float)>& progress,
                                      const std::function<bool()>& shouldCancel)
{
    AnalysisResult result;
    const int length = source.getNumSamples();
    const int numChannels = std::min (2, source.getNumChannels());
    if (length == 0 || numChannels == 0 || sr <= 0.0)
        return result;

    auto cancelled = [&] { return shouldCancel && shouldCancel(); };
    auto report = [&] (float p) { if (progress) progress (p); };

    std::vector<float> mono ((size_t) length, 0.0f);
    for (int ch = 0; ch < numChannels; ++ch)
    {
        const float* d = source.getReadPointer (ch);
        for (int i = 0; i < length; ++i)
            mono[(size_t) i] += d[i] / (float) numChannels;
    }

    // 1. Onsets
    const auto onsets = detectOnsets (mono, sr, report, shouldCancel);
    if (onsets.empty() || cancelled())
        return result;

    // Side-chains: band-limited copies used to judge and compare specific drums.
    auto filtered = [&] (std::initializer_list<Biquad> filters)
    {
        std::vector<float> v (mono);
        for (auto& f : filters)
            f.process (v);
        return v;
    };
    const auto lowBand = filtered ({ Biquad::lowpass (sr, 1200.0), Biquad::lowpass (sr, 1200.0) });
    const auto midBand = filtered ({ Biquad::highpass (sr, 200.0), Biquad::highpass (sr, 200.0) });
    const auto highBand = filtered ({ Biquad::highpass (sr, 5000.0), Biquad::highpass (sr, 5000.0) });
    const auto clapBand = filtered ({ Biquad::highpass (sr, 800.0), Biquad::lowpass (sr, 4000.0) });
    report (0.45f);

    // 2. Classify every hit
    FeatureExtractor extractor (mono, clapBand, highBand, sr);
    std::vector<HitInfo> hits;
    hits.reserve (onsets.size());
    for (size_t i = 0; i < onsets.size(); ++i)
    {
        if ((i & 63) == 0)
        {
            if (cancelled())
                return result;
            report (0.45f + 0.15f * (float) i / (float) onsets.size());
        }
        const int next = i + 1 < onsets.size() ? onsets[i + 1].position - onsets[i].position
                                               : length - onsets[i].position;
        HitInfo h;
        h.onset = onsets[i];
        h.nextOnsetDistance = next;
        const auto features = extractor.compute (onsets[i].position, next);
        h.drumClass = classify (features);
        h.secondaryClass = detectLayer (h.drumClass, features);
        hits.push_back (h);
        result.hits.push_back ({ h.onset.position, h.drumClass, h.secondaryClass, h.onset.strength });
    }

    // Every variation is first stacked raw (unfiltered, full length); layers are removed from the
    // raw stacks, and only then is each one cleaned up, trimmed and normalised.
    struct Pending
    {
        juce::AudioBuffer<float> raw;
        int length = 0;              // where the sound will be cut
        int used = 0;                // hits stacked
        int layer = -1;              // the drum it is layered with in every hit used, or -1 if clean
        int templateLength = 0;      // up to the first other drum: the part that is safe to use as a template
        std::vector<std::pair<int, int>> followers; // (drum class, offset): drums that follow it in the groove
        juce::AudioBuffer<float> tail;  // stacked from the hits that have room to ring out, or empty
        std::vector<int> sourceHits;
    };
    std::array<std::vector<Pending>, numDrumClasses> pending;

    // 3 + 4 + 5. Group, stack and clean each class
    for (int c = 0; c < numDrumClasses; ++c)
    {
        const auto drumClass = (DrumClass) c;
        const auto settings = settingsFor (drumClass);
        const auto& sideChain = drumClass == DrumClass::kick ? lowBand
                              : (drumClass == DrumClass::hat || drumClass == DrumClass::openHat) ? highBand
                                                                                                 : midBand;

        // Hits only secondarily of this class can join a group but never start one.
        std::vector<const HitInfo*> candidates;
        for (auto& h : hits)
            if (h.drumClass == drumClass || h.secondaryClass == c)
                candidates.push_back (&h);
        std::sort (candidates.begin(), candidates.end(), [drumClass] (auto* a, auto* b)
        {
            const bool pa = a->drumClass == drumClass, pb = b->drumClass == drumClass;
            return pa != pb ? pa : a->onset.strength > b->onset.strength;
        });
        if (candidates.size() > 64)
            candidates.resize (64);
        if (candidates.empty())
            continue;

        const int compareLength = (int) (settings.compareSeconds * sr);
        const int maxLag = (int) (0.0015 * sr);
        std::vector<bool> assigned (candidates.size(), false);
        std::vector<Cluster> clusters;

        for (size_t seed = 0; seed < candidates.size(); ++seed)
        {
            if (assigned[seed] || candidates[seed]->drumClass != drumClass)
                continue;
            if (cancelled())
                return result;

            Cluster cluster;
            cluster.members.push_back ((int) seed);
            cluster.lags.push_back (0);
            assigned[seed] = true;

            for (size_t other = seed + 1; other < candidates.size(); ++other)
            {
                if (assigned[other])
                    continue;
                const auto m = compare (sideChain, candidates[seed]->onset.position,
                                        candidates[other]->onset.position, compareLength, maxLag);
                if (m.correlation > 0.8f)
                {
                    cluster.members.push_back ((int) other);
                    cluster.lags.push_back (m.lag);
                    assigned[other] = true;
                }
            }

            clusters.push_back (std::move (cluster));
        }

        // Second pass: groups whose averages match are the same drum heard over different
        // parts of the song (e.g. a different bass note underneath), so merge them.
        auto centroid = [&] (const Cluster& cl)
        {
            std::vector<float> avg ((size_t) compareLength, 0.0f);
            for (size_t k = 0; k < cl.members.size(); ++k)
            {
                const int start = candidates[(size_t) cl.members[k]]->onset.position + cl.lags[k];
                for (int i = 0; i < compareLength; ++i)
                    if (start + i >= 0 && start + i < (int) sideChain.size())
                        avg[(size_t) i] += sideChain[(size_t) (start + i)];
            }
            // Pad on both sides so `compare` can search lags around it.
            std::vector<float> padded ((size_t) maxLag, 0.0f);
            padded.insert (padded.end(), avg.begin(), avg.end());
            padded.insert (padded.end(), (size_t) maxLag * 2 + 1, 0.0f);
            return padded;
        };

        for (size_t i = 0; i < clusters.size(); ++i)
        {
            if (cancelled())
                return result;
            for (size_t j = i + 1; j < clusters.size();)
            {
                auto a = centroid (clusters[i]);
                const auto b = centroid (clusters[j]);
                const size_t offset = a.size();
                a.insert (a.end(), b.begin(), b.end());
                const auto m = compare (a, maxLag, (int) offset + maxLag, compareLength, maxLag);
                if (m.correlation > 0.9f)
                {
                    for (size_t k = 0; k < clusters[j].members.size(); ++k)
                    {
                        clusters[i].members.push_back (clusters[j].members[k]);
                        clusters[i].lags.push_back (clusters[j].lags[k] + m.lag);
                    }
                    clusters.erase (clusters.begin() + (std::ptrdiff_t) j);
                }
                else
                {
                    ++j;
                }
            }
        }

        for (auto& cl : clusters)
        {
            float strength = 0.0f;
            for (int m : cl.members)
                strength += candidates[(size_t) m]->onset.strength;
            strength /= (float) cl.members.size();
            cl.score = std::log2 (1.0f + (float) cl.members.size()) + 0.5f * strength;
        }

        std::sort (clusters.begin(), clusters.end(), [] (auto& a, auto& b) { return a.score > b.score; });

        // One-off hits are usually something else in the mix; only keep them if nothing repeats.
        // Percussion is the catch-all class, so it only counts when the same sound repeats.
        if (drumClass == DrumClass::perc)
            clusters.erase (std::remove_if (clusters.begin(), clusters.end(),
                                            [] (auto& cl) { return cl.members.size() < 2; }),
                            clusters.end());
        if (clusters.empty())
            continue;

        if (clusters.front().members.size() >= 3)
            clusters.erase (std::remove_if (clusters.begin(), clusters.end(),
                                            [] (auto& cl) { return cl.members.size() < 2; }),
                            clusters.end());
        else if (clusters.size() > 3)
            clusters.resize (3);
        if (clusters.size() > (size_t) maxVariationsPerClass)
            clusters.resize ((size_t) maxVariationsPerClass);

        const int extractLength = (int) (settings.extractSeconds * sr);
        const int preRoll = (int) (0.001 * sr);

        for (auto& cluster : clusters)
        {
            juce::AudioBuffer<float> stacked (2, extractLength);
            stacked.clear();

            const bool single = cluster.members.size() == 1;
            // Which other drum is layered on each member. If enough members are clean, stack only those.
            auto layerOf = [&] (int member)
            {
                const auto* h = candidates[(size_t) member];
                return h->drumClass == drumClass ? h->secondaryClass : (int) h->drumClass;
            };
            std::vector<size_t> chosen;
            for (size_t k = 0; k < cluster.members.size(); ++k)
                if (layerOf (cluster.members[k]) < 0)
                    chosen.push_back (k);
            const bool cleanOnly = chosen.size() >= 3 && chosen.size() * 4 >= cluster.members.size();
            if (! cleanOnly)
            {
                chosen.clear();
                for (size_t k = 0; k < cluster.members.size(); ++k)
                    chosen.push_back (k);
            }

            int layer = -1;
            if (! cleanOnly)
            {
                std::array<int, numDrumClasses> counts {};
                for (size_t k : chosen)
                    if (const int l = layerOf (cluster.members[k]); l >= 0)
                        ++counts[(size_t) l];
                const auto most = std::max_element (counts.begin(), counts.end());
                if (*most * 2 >= (int) chosen.size())
                    layer = (int) (most - counts.begin());
            }

            // Drums that follow this one at the same distance in most hits (e.g. a hat exactly 1/16 after
            // every snare). They are part of the groove, so they can be removed from the tail with their
            // own clean sound instead of cutting the sound short before them.
            const int tolerance = (int) (0.015 * sr);
            std::vector<std::pair<int, int>> followers;
            {
                std::array<std::vector<int>, numDrumClasses> offsets;
                for (size_t k : chosen)
                {
                    const auto index = (size_t) (candidates[(size_t) cluster.members[k]] - hits.data());
                    for (size_t next = index + 1; next < hits.size(); ++next)
                    {
                        const int offset = hits[next].onset.position - hits[index].onset.position;
                        if (offset >= extractLength)
                            break;
                        if (offset > (int) (0.02 * sr))
                            offsets[(size_t) hits[next].drumClass].push_back (offset);
                    }
                }
                for (int d = 0; d < numDrumClasses; ++d)
                {
                    auto& o = offsets[(size_t) d];
                    std::sort (o.begin(), o.end());
                    for (size_t a = 0; a < o.size();)
                    {
                        size_t b = a;
                        while (b + 1 < o.size() && o[b + 1] - o[a] <= 2 * tolerance)
                            ++b;
                        if ((b - a + 1) * 2 >= chosen.size() && chosen.size() >= 2)
                            followers.push_back ({ d, o[(a + b) / 2] });
                        a = b + 1;
                    }
                }
                std::sort (followers.begin(), followers.end(), [] (auto& x, auto& y) { return x.second < y.second; });
            }

            // Cut before the next hit that would bleed through: one this class's cleanup filter doesn't
            // remove (the kick's low-pass removes hats) and that isn't a groove follower removed above.
            // Use the typical gap across the group. The template length stops at any other drum.
            auto memberGaps = [&] (bool allowFollowers)
            {
                std::vector<int> gaps;
                for (size_t k : chosen)
                {
                    const auto index = (size_t) (candidates[(size_t) cluster.members[k]] - hits.data());
                    int gap = length - hits[index].onset.position;
                    for (size_t next = index + 1; next < hits.size(); ++next)
                    {
                        const auto nextClass = hits[next].drumClass;
                        const int offset = hits[next].onset.position - hits[index].onset.position;
                        const bool removedByCleanup = drumClass == DrumClass::kick
                                                   && (nextClass == DrumClass::hat || nextClass == DrumClass::openHat);
                        const bool follower = allowFollowers
                            && std::any_of (followers.begin(), followers.end(), [&] (auto& f)
                                            { return f.first == (int) nextClass && std::abs (f.second - offset) <= tolerance; });
                        if (! removedByCleanup && ! follower)
                        {
                            gap = offset;
                            break;
                        }
                    }
                    gaps.push_back (gap);
                }
                return gaps;
            };
            auto typicalGap = [&] (std::vector<int> gaps)
            {
                std::nth_element (gaps.begin(), gaps.begin() + (std::ptrdiff_t) gaps.size() / 2, gaps.end());
                return std::max ((int) (0.02 * sr), std::min (extractLength, gaps[gaps.size() / 2] + preRoll));
            };
            const auto gapsWithFollowers = memberGaps (true);
            const int hitLength = typicalGap (gapsWithFollowers);
            const int templateLength = typicalGap (memberGaps (false));

            const int seedPos = candidates[(size_t) cluster.members[chosen.front()]]->onset.position + cluster.lags[chosen.front()];
            const int refLength = std::min (compareLength, hitLength);

            // Averages the given members (aligned, level-matched to the seed) into `out`.
            auto stack = [&] (const std::vector<size_t>& members, int minLength, juce::AudioBuffer<float>& out)
            {
                out.setSize (2, extractLength);
                out.clear();
                int count = 0;
                for (size_t k : members)
                {
                    if (count >= 32)
                        break;
                    const int start = candidates[(size_t) cluster.members[k]]->onset.position + cluster.lags[k] - preRoll;
                    if (start < 0 || start >= source.getNumSamples())
                        continue;
                    if (! single && start + minLength > source.getNumSamples())
                        continue;
                    const int available = std::min (extractLength, source.getNumSamples() - start);

                    // Least-squares gain so quieter repeats line up with the loudest one.
                    double cross = 0.0, energy = 0.0;
                    for (int i = 0; i < refLength; ++i)
                    {
                        const int a = seedPos - preRoll + i, b = start + i;
                        if (a < 0 || b >= (int) sideChain.size() || a >= (int) sideChain.size())
                            continue;
                        cross += (double) sideChain[(size_t) a] * sideChain[(size_t) b];
                        energy += (double) sideChain[(size_t) b] * sideChain[(size_t) b];
                    }
                    const float gain = energy <= 0.0 ? 1.0f : (float) juce::jlimit (0.25, 4.0, cross / energy);

                    // The whole extract length is kept: the sound is cut later, but as a template for
                    // removing this drum from others, its full ring is useful.
                    for (int ch = 0; ch < 2; ++ch)
                    {
                        const float* src = source.getReadPointer (std::min (ch, numChannels - 1));
                        float* dst = out.getWritePointer (ch);
                        for (int i = 0; i < available; ++i)
                            dst[i] += gain * src[start + i];
                    }
                    ++count;
                }
                if (count > 0)
                    out.applyGain (1.0f / (float) count);
                return count;
            };

            const int used = stack (chosen, hitLength, stacked);
            if (used == 0)
                continue;

            // The tail: hits of this drum that have room to ring out (the next drum comes much later,
            // e.g. before a break). Their stack supplies the natural decay beyond the cut.
            juce::AudioBuffer<float> tail;
            {
                std::vector<std::pair<int, size_t>> roomy;
                for (size_t i = 0; i < chosen.size(); ++i)
                    if (gapsWithFollowers[i] >= std::max (hitLength * 8 / 5, hitLength + (int) (0.08 * sr)))
                        roomy.push_back ({ gapsWithFollowers[i], chosen[i] });
                if (! roomy.empty())
                {
                    std::sort (roomy.begin(), roomy.end(), [] (auto& a, auto& b) { return a.first > b.first; });
                    if (roomy.size() > 8)
                        roomy.resize (8);
                    std::vector<size_t> members;
                    for (auto& r : roomy)
                        members.push_back (r.second);
                    const int tailLength = std::min (extractLength, roomy.back().first + preRoll);
                    if (stack (members, tailLength, tail) > 0)
                        tail.setSize (2, tailLength, true);
                    else
                        tail.setSize (0, 0);
                }
            }

            Pending p;
            p.raw = std::move (stacked);
            p.length = hitLength;
            p.templateLength = templateLength;
            p.followers = std::move (followers);
            p.tail = std::move (tail);
            p.used = used;
            p.layer = layer;
            for (int m : cluster.members)
                p.sourceHits.push_back ((int) (candidates[(size_t) m] - hits.data()));
            pending[(size_t) c].push_back (std::move (p));
        }

        report (0.6f + 0.4f * (float) (c + 1) / (float) numDrumClasses);
    }

    // Sounds that only ever occur layered (e.g. a snare that always has an open hat on top):
    // subtract a clean copy of the other drum, found elsewhere in the track. The layer is either
    // known from the hits themselves, or found by testing the clean hats and snares against it.
    auto cleanTemplate = [&] (int layerClass) -> const Pending*
    {
        for (auto& p : pending[(size_t) layerClass])
            if (p.layer < 0)
                return &p;
        return nullptr;
    };

    // Templates for removing followers: each drum's raw stack up to the first other drum, faded out,
    // taken before anything is subtracted.
    std::array<juce::AudioBuffer<float>, numDrumClasses> followerTemplates;
    for (int d = 0; d < numDrumClasses; ++d)
        if (const auto* t = cleanTemplate (d) != nullptr ? cleanTemplate (d)
                          : (pending[(size_t) d].empty() ? nullptr : &pending[(size_t) d].front()))
        {
            auto& buffer = followerTemplates[(size_t) d];
            buffer.makeCopyOf (t->raw);
            buffer.setSize (buffer.getNumChannels(), std::min (t->templateLength, buffer.getNumSamples()), true);
            applyFades (buffer, 1, std::min (buffer.getNumSamples() / 2, (int) (0.005 * sr)));
        }

    for (int c = 0; c < numDrumClasses; ++c)
    {
        const auto drumClass = (DrumClass) c;
        if (drumClass == DrumClass::hat || drumClass == DrumClass::openHat)
            continue; // hats are high-passed already; their best groups are the clean ones

        for (auto& p : pending[(size_t) c])
        {
            std::vector<int> layers;
            if (p.layer >= 0)
                layers.push_back (p.layer);
            for (auto candidate : { DrumClass::openHat, DrumClass::hat })
                if ((int) candidate != p.layer)
                    layers.push_back ((int) candidate);
            if (drumClass == DrumClass::kick)
                for (auto candidate : { DrumClass::snare, DrumClass::clap })
                    layers.push_back ((int) candidate);

            for (int layer : layers)
            {
                const auto* layerSound = cleanTemplate (layer);
                if (layerSound == nullptr)
                    continue;

                juce::AudioBuffer<float> target (p.raw.getNumChannels(), std::min (p.length, p.raw.getNumSamples()));
                for (int ch = 0; ch < target.getNumChannels(); ++ch)
                    target.copyFrom (ch, 0, p.raw, ch, 0, target.getNumSamples());
                const bool hatLayer = layer == (int) DrumClass::hat || layer == (int) DrumClass::openHat;
                const double highPassHz = hatLayer ? 2000.0 : 150.0;
                LayerDetector detector (target, layerSound->raw, sr, highPassHz);
                detector.estimate();
                // The template may itself contain some of this drum (e.g. claps that often land on kicks).
                // Subtracting it then also removes gain x (amount of this drum in the template) of the drum
                // itself, which is only a level change, undone by normalising later, unless it gets large.
                LayerDetector reverse (layerSound->raw, target, sr, drumClass == DrumClass::kick ? 40.0 : 150.0);
                reverse.estimate();
                const bool contaminated = reverse.getPresence() > 0.12f && detector.getGain() * reverse.getGain() > 0.5f;

                if (! contaminated && detector.getGain() > 0.0f && (layer == p.layer || detector.getPresence() > 0.12f))
                {
                    subtractLayer (p.raw, p.length, layerSound->raw, sr, highPassHz);

                    // Now known to be layered: show both drums for these hits.
                    for (int index : p.sourceHits)
                        if (result.hits[(size_t) index].secondaryClass < 0 && (int) result.hits[(size_t) index].drumClass == c)
                            result.hits[(size_t) index].secondaryClass = layer;
                }
            }
        }
    }

    // Remove the groove followers from each sound's tail, so it can ring out instead of being cut
    // short. If a follower doesn't cancel well (not the same sample every time), cut before it.
    for (int c = 0; c < numDrumClasses; ++c)
        for (auto& p : pending[(size_t) c])
            for (auto& [followerClass, offset] : p.followers)
            {
                if (offset >= p.length)
                    break;
                const auto& t = followerTemplates[(size_t) followerClass];
                const bool hatFollower = followerClass == (int) DrumClass::hat || followerClass == (int) DrumClass::openHat;
                const double band = hatFollower ? 2000.0 : 40.0;
                if (t.getNumSamples() > 0)
                    subtractLayer (p.raw, p.length, t, sr, band, offset);
                const float attackAfter = attackAt (p.raw, offset, sr, band);

                // Still an audible new hit there (energy jumps by more than 3 dB): cut before it.
                if (attackAfter > 3.0f)
                {
                    p.length = std::max ((int) (0.02 * sr), offset);
                    break;
                }
            }

    // Graft the natural tail on where the sound would otherwise be cut short.
    for (int c = 0; c < numDrumClasses; ++c)
        for (auto& p : pending[(size_t) c])
            graftTail (p.raw, p.length, p.tail, sr);

    // Finishes a sound: continues a tail that was cut while still ringing, fades and normalises.
    auto finish = [&] (juce::AudioBuffer<float>& sound, DrumClass drumClass, int seed)
    {
        // How fast each kind of drum dies away at the least (dB per second), so a tail measured over
        // leftover music can't ring on unnaturally.
        if (drumClass != DrumClass::kick)
        {
            const double minDecay = drumClass == DrumClass::hat ? 350.0 : drumClass == DrumClass::openHat ? 60.0 : 120.0;
            extendTail (sound, sr, seed, minDecay, (int) (settingsFor (drumClass).extractSeconds * sr));
        }
        applyFades (sound, std::max (1, (int) (0.0003 * sr)),
                    std::min (sound.getNumSamples() / 2, (int) (settingsFor (drumClass).fadeOutSeconds * sr)));
        normalise (sound);
    };

    // Open hats that only ever ring on top of a snare or clap, taken out of them.
    struct RingingLayer
    {
        juce::AudioBuffer<float> audio;
        int used = 0;
        std::vector<int> sourceHits;
    };
    std::vector<RingingLayer> ringingLayers;

    // Clean up, trim and normalise every sound.
    for (int c = 0; c < numDrumClasses; ++c)
    {
        const auto drumClass = (DrumClass) c;
        for (auto& p : pending[(size_t) c])
        {
            auto stacked = std::move (p.raw);
            stacked.setSize (2, std::min (p.length, stacked.getNumSamples()), true);
            cleanUp (stacked, drumClass, sr);
            const int end = std::max ((int) (0.02 * sr), findEnd (stacked, sr));
            stacked.setSize (2, std::min (end, stacked.getNumSamples()), true);
            if (drumClass == DrumClass::snare || drumClass == DrumClass::clap)
            {
                RingingLayer layer;
                stacked.setSize (2, std::max ((int) (0.02 * sr), tameRingingTop (stacked, sr, &layer.audio)), true);
                if (layer.audio.getNumSamples() > 0)
                {
                    layer.used = p.used;
                    layer.sourceHits = p.sourceHits;
                    ringingLayers.push_back (std::move (layer));
                }
            }
            finish (stacked, drumClass, c * 100 + (int) result.kit[(size_t) c].size());

            DrumSample sample;
            sample.audio = std::move (stacked);
            sample.sampleRate = sr;
            sample.hitsStacked = p.used;
            sample.sourceHits = std::move (p.sourceHits);
            result.kit[(size_t) c].push_back (std::move (sample));
        }
    }

    // In a full track every real drum repeats, so a sound made from one hit is something else.
    if (result.hits.size() >= 50)
        for (auto& variations : result.kit)
            variations.erase (std::remove_if (variations.begin(), variations.end(),
                                              [] (auto& v) { return v.sourceHits.size() < 2; }),
                              variations.end());

    // Fragments too short to be a usable drum sound (the cut landed right after the attack).
    for (int c = 0; c < numDrumClasses; ++c)
    {
        if ((DrumClass) c == DrumClass::hat)
            continue;
        auto& variations = result.kit[(size_t) c];
        variations.erase (std::remove_if (variations.begin(), variations.end(),
                                          [sr] (auto& v) { return v.audio.getNumSamples() < (int) (0.04 * sr); }),
                          variations.end());
    }

    // No open hat of its own: use the one ringing on top of the snares and claps. The longest layer
    // first (it holds most of the hat's own ring), then the best stacked.
    if (auto& openHats = result.kit[(size_t) DrumClass::openHat]; openHats.empty())
    {
        std::stable_sort (ringingLayers.begin(), ringingLayers.end(), [] (auto& a, auto& b)
        {
            return std::make_pair (a.audio.getNumSamples(), a.used) > std::make_pair (b.audio.getNumSamples(), b.used);
        });
        for (auto& layer : ringingLayers)
        {
            if (openHats.size() >= 2)
                break;
            cleanUp (layer.audio, DrumClass::openHat, sr);
            finish (layer.audio, DrumClass::openHat, 400 + (int) openHats.size());
            for (int index : layer.sourceHits)
                if (result.hits[(size_t) index].secondaryClass < 0)
                    result.hits[(size_t) index].secondaryClass = (int) DrumClass::openHat;

            DrumSample sample;
            sample.audio = std::move (layer.audio);
            sample.sampleRate = sr;
            sample.hitsStacked = layer.used;
            sample.sourceHits = std::move (layer.sourceHits);
            openHats.push_back (std::move (sample));
        }
    }

    // Nearly every track has a snare or clap. If neither repeats but percussion has a big
    // repeating group, that group is the snare (often a dark or heavily processed one).
    auto biggest = [&] (DrumClass dc)
    {
        size_t most = 0;
        for (auto& v : result.kit[(size_t) dc])
            most = std::max (most, v.sourceHits.size());
        return most;
    };
    auto& percs = result.kit[(size_t) DrumClass::perc];
    if (std::max (biggest (DrumClass::snare), biggest (DrumClass::clap)) < 4 && ! percs.empty()
        && percs.front().sourceHits.size() >= 4)
    {
        for (int index : percs.front().sourceHits)
            result.hits[(size_t) index].drumClass = DrumClass::snare;
        auto& snares = result.kit[(size_t) DrumClass::snare];
        snares.insert (snares.begin(), std::move (percs.front()));
        percs.erase (percs.begin());
    }

    report (1.0f);
    return result;
}
} // namespace kitnap
