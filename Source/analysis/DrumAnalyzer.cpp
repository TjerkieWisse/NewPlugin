#include "DrumAnalyzer.h"
#include <juce_dsp/juce_dsp.h>
#include <algorithm>
#include <cmath>
#include <limits>
#include <numeric>

namespace kitnap
{
namespace
{
constexpr float pi = juce::MathConstants<float>::pi;

// The longest a pad's sound may be: long enough for a crash to ring out.
double maxSecondsFor (DrumClass c)
{
    switch (c)
    {
        case DrumClass::kick:    return 1.5;
        case DrumClass::snare:   return 1.5;
        case DrumClass::toms:    return 2.0;
        case DrumClass::hihat:   return 1.5;
        case DrumClass::cymbals: return 4.0;
    }
    return 1.5;
}

std::vector<float> toMono (const juce::AudioBuffer<float>& audio)
{
    const int n = audio.getNumSamples(), channels = audio.getNumChannels();
    std::vector<float> mono ((size_t) n, 0.0f);
    for (int ch = 0; ch < channels; ++ch)
        juce::FloatVectorOperations::addWithMultiply (mono.data(), audio.getReadPointer (ch), 1.0f / (float) channels, n);
    return mono;
}

std::vector<float> hannWindow (int size)
{
    std::vector<float> w ((size_t) size);
    for (int i = 0; i < size; ++i)
        w[(size_t) i] = 0.5f - 0.5f * std::cos (2.0f * pi * (float) i / (float) size);
    return w;
}

//==============================================================================
// Spectral flux on log-compressed magnitudes: how much new energy appears in each frame.
// Returns approximate onset positions (samples).
std::vector<int> detectOnsets (const std::vector<float>& signal, double sr)
{
    constexpr int order = 10, size = 1 << order, hop = size / 4, bins = size / 2;
    // Silence in front, so a hit right at the start is seen arriving too.
    std::vector<float> x ((size_t) size, 0.0f);
    x.insert (x.end(), signal.begin(), signal.end());
    const int n = (int) x.size();
    if (n < size * 2)
        return {};
    const int frames = (n - size) / hop + 1;
    juce::dsp::FFT fft (order);
    const auto window = hannWindow (size);
    std::vector<float> buffer ((size_t) size * 2);

    auto magnitudes = [&] (int frame, std::vector<float>& out)
    {
        std::fill (buffer.begin(), buffer.end(), 0.0f);
        for (int i = 0; i < size; ++i)
            buffer[(size_t) i] = x[(size_t) (frame * hop + i)] * window[(size_t) i];
        fft.performFrequencyOnlyForwardTransform (buffer.data(), true);
        out.assign (buffer.begin(), buffer.begin() + bins + 1);
    };

    // The compression is relative to the loudest bin anywhere in the part, so it doesn't matter how
    // loud the part is.
    std::vector<float> mag;
    float reference = 0.0f;
    for (int f = 0; f < frames; ++f)
    {
        magnitudes (f, mag);
        reference = std::max (reference, *std::max_element (mag.begin(), mag.end()));
    }
    if (reference <= 0.0f)
        return {};
    const float scale = 1000.0f / reference;

    std::vector<float> flux ((size_t) frames, 0.0f), previous ((size_t) bins + 1, 0.0f), current ((size_t) bins + 1);
    for (int f = 0; f < frames; ++f)
    {
        magnitudes (f, mag);
        for (int b = 1; b <= bins; ++b)
            current[(size_t) b] = std::log1p (mag[(size_t) b] * scale);
        if (f > 0)
            for (int b = 1; b <= bins; ++b)
                flux[(size_t) f] += std::max (0.0f, current[(size_t) b] - previous[(size_t) b]);
        std::swap (previous, current);
    }

    // Peaks that stand out from their surroundings (+-90 ms) and from the part as a whole.
    const float maxFlux = *std::max_element (flux.begin(), flux.end());
    const int localRadius = 3, averageRadius = std::max (1, (int) (0.09 * sr / hop));
    std::vector<int> onsets;
    for (int f = 1; f < frames; ++f)
    {
        bool isPeak = true;
        for (int k = std::max (0, f - localRadius); k <= std::min (frames - 1, f + localRadius) && isPeak; ++k)
            isPeak = flux[(size_t) k] <= flux[(size_t) f];
        if (! isPeak)
            continue;
        double sum = 0.0;
        int count = 0;
        for (int k = std::max (0, f - averageRadius); k <= std::min (frames - 1, f + averageRadius); ++k, ++count)
            sum += flux[(size_t) k];
        if (flux[(size_t) f] > 1.5f * (float) (sum / count) + 0.05f * maxFlux)
            onsets.push_back (f * hop + size / 2 - size);
    }
    return onsets;
}

// Where exactly the hit starts: the point where the power of the next 3 ms rises most above that of the
// 12 ms before (so a previous hit still ringing doesn't count), searched around the frame-based
// estimate. energy holds a running sum of squares.
int refineOnset (const std::vector<double>& energy, int approx, double sr)
{
    const int n = (int) energy.size() - 1;
    const int after = (int) (0.003 * sr), before = (int) (0.012 * sr), step = 8, reach = 768;
    auto power = [&] (int from, int to)
    {
        from = juce::jlimit (0, n, from);
        to = juce::jlimit (0, n, to);
        return to > from ? (energy[(size_t) to] - energy[(size_t) from]) / (to - from) : 0.0;
    };
    const double floor = 1e-12;
    int best = juce::jlimit (0, n, approx);
    double bestRatio = 0.0;
    for (int t = std::max (0, approx - reach); t <= std::min (n - 1, approx + reach); t += step)
        if (const double ratio = (power (t, t + after) + floor) / (power (t - before, t) + floor); ratio > bestRatio)
        {
            bestRatio = ratio;
            best = t;
        }
    return best;
}

//==============================================================================
// What a hit sounds like: band levels (dB) in its first 80 ms, 24 bands from 40 Hz to 16 kHz in four
// 20 ms slices, with the overall level taken out so a quieter hit of the same drum still matches.
class Fingerprinter
{
public:
    static constexpr int numBands = 24, numSlices = 4;
    static constexpr float floorDb = 40.0f;
    using Print = std::vector<float>; // numBands per slice, as many slices as the hit has room for

    explicit Fingerprinter (double sampleRate)
        : sr (sampleRate), sliceLength ((int) (0.02 * sampleRate)), fft (order), window (hannWindow (sliceLength))
    {
        const double binHz = sr / size;
        bandOfBin.assign ((size_t) size / 2 + 1, -1);
        for (int b = 1; b <= size / 2; ++b)
        {
            const double hz = b * binHz;
            if (hz >= 40.0 && hz < 16000.0)
                bandOfBin[(size_t) b] = juce::jlimit (0, numBands - 1, (int) (numBands * std::log (hz / 40.0) / std::log (16000.0 / 40.0)));
        }
        buffer.resize ((size_t) size * 2);
    }

    Print make (const std::vector<float>& x, int onset, int available)
    {
        // What is still sounding from before the hit (the previous hit ringing on) is taken out of
        // every slice, so only what the hit itself adds is compared.
        const auto before = bandEnergies (x, onset - sliceLength);
        const int slices = juce::jlimit (1, numSlices, available / sliceLength);
        Print print ((size_t) (slices * numBands), 0.0f);
        for (int s = 0; s < slices; ++s)
        {
            const auto energy = bandEnergies (x, onset + s * sliceLength);
            for (int band = 0; band < numBands; ++band)
                print[(size_t) (s * numBands + band)] = (float) (10.0 * std::log10 (std::max (0.0, energy[(size_t) band] - before[(size_t) band]) + 1e-20));
        }
        // Relative to the loudest band, so the level doesn't matter. Bands more than 40 dB below it
        // don't shape the sound (and are mostly other drums' bleed): they all count as -40 dB.
        const float top = *std::max_element (print.begin(), print.end());
        for (auto& v : print)
            v = std::max (v - top, -floorDb);
        return print;
    }

    std::array<double, numBands> bandEnergies (const std::vector<float>& x, int start)
    {
        std::fill (buffer.begin(), buffer.end(), 0.0f);
        for (int i = 0; i < sliceLength; ++i)
            if (const int idx = start + i; idx >= 0 && idx < (int) x.size())
                buffer[(size_t) i] = x[(size_t) idx] * window[(size_t) i];
        fft.performFrequencyOnlyForwardTransform (buffer.data(), true);
        std::array<double, numBands> energy {};
        for (int b = 1; b <= size / 2; ++b)
            if (const int band = bandOfBin[(size_t) b]; band >= 0)
                energy[(size_t) band] += (double) buffer[(size_t) b] * buffer[(size_t) b];
        return energy;
    }

    // RMS difference in dB over the bands that are audible in either hit, in the slices both have.
    static float distance (const Print& a, const Print& b)
    {
        const size_t n = std::min (a.size(), b.size());
        double sum = 0.0;
        int count = 0;
        for (size_t i = 0; i < n; ++i)
            if (a[i] > -floorDb || b[i] > -floorDb)
            {
                const double d = a[i] - b[i];
                sum += d * d;
                ++count;
            }
        return count > 0 ? (float) std::sqrt (sum / count) : 0.0f;
    }

private:
    static constexpr int order = 11, size = 1 << order;
    double sr;
    int sliceLength;
    juce::dsp::FFT fft;
    std::vector<float> window, buffer;
    std::vector<int> bandOfBin;
};

//==============================================================================
// Hits that differ by less than sameSoundFactor times the part's typical distance between a hit and
// its closest match (kept between minSameSound and maxSameSound dB) are the same sound.
constexpr float minSameSound = 2.0f, maxSameSound = 12.0f, sameSoundFactor = 3.5f;

struct Hit
{
    int onset = 0;      // where the hit starts
    int end = 0;        // where its sound ends: the next hit, the pad's maximum length, or silence
    float peak = 0.0f;  // loudest sample in its first 50 ms
    float before = 0.0f; // level just before it (the previous hit's tail), relative to its own level
    Fingerprinter::Print print;
};

float rms (const std::vector<float>& x, int from, int to)
{
    from = std::max (0, from);
    to = std::min ((int) x.size(), to);
    if (to <= from)
        return 0.0f;
    double sum = 0.0;
    for (int i = from; i < to; ++i)
        sum += (double) x[(size_t) i] * x[(size_t) i];
    return (float) std::sqrt (sum / (to - from));
}

// Finds the hits in one part, with where each one's sound ends.
// Only hits inside `regions` count (all of x when empty), from 30 ms after a region's start on (its
// first moments may be the middle of a sound), and a sound ends at its region's end at the latest.
std::vector<Hit> findHits (const std::vector<float>& x, double sr, DrumClass drumClass, float loudestPart,
                           std::vector<juce::Range<int>> regions)
{
    const int n = (int) x.size();
    const bool wholePart = regions.empty();
    if (wholePart)
        regions.push_back ({ 0, n });
    auto regionOf = [&] (int position) -> const juce::Range<int>*
    {
        for (auto& r : regions)
            if (r.contains (position))
                return &r;
        return nullptr;
    };
    const int settle = (int) (0.03 * sr);
    // Of the first difference, which favours the attack's high end over low notes still ringing.
    std::vector<double> energy ((size_t) n + 1, 0.0);
    for (int i = 0; i < n; ++i)
    {
        const double d = x[(size_t) i] - (i > 0 ? x[(size_t) i - 1] : 0.0f);
        energy[(size_t) i + 1] = energy[(size_t) i] + d * d;
    }

    std::vector<int> refined, onsets;
    for (int approx : detectOnsets (x, sr))
        refined.push_back (refineOnset (energy, approx, sr));
    std::sort (refined.begin(), refined.end());
    for (int onset : refined)
        // Two estimates of the same hit (25 ms apart at most): keep the first.
        if (onsets.empty() || onset - onsets.back() >= (int) (0.025 * sr))
            onsets.push_back (onset);

    std::vector<Hit> hits;
    for (int onset : onsets)
    {
        const auto* region = regionOf (onset);
        if (region == nullptr || (! wholePart && onset < region->getStart() + settle))
            continue;
        Hit h;
        h.onset = onset;
        for (int i = onset; i < std::min (n, onset + (int) (0.05 * sr)); ++i)
            h.peak = std::max (h.peak, std::abs (x[(size_t) i]));
        hits.push_back (std::move (h));
    }
    if (hits.empty())
        return hits;

    // What the split leaves of other drums in this part is far quieter than the part's own hits:
    // keep hits within 30 dB of the part's typical loud one. A part that only holds such leftovers
    // (a song without toms, say) is far below the drums as a whole: nothing more than 35 dB under
    // the loudest part counts.
    std::vector<float> peaks;
    for (auto& h : hits)
        peaks.push_back (h.peak);
    std::sort (peaks.begin(), peaks.end());
    const float typical = peaks[(size_t) ((peaks.size() - 1) * 9 / 10)];
    const float gate = std::max ({ typical * juce::Decibels::decibelsToGain (-30.0f), loudestPart * juce::Decibels::decibelsToGain (-35.0f),
                                   juce::Decibels::decibelsToGain (-80.0f) });
    hits.erase (std::remove_if (hits.begin(), hits.end(), [gate] (auto& h) { return h.peak < gate; }), hits.end());

    const int preroll = (int) (0.002 * sr);
    const int maxLength = (int) (maxSecondsFor (drumClass) * sr);
    const int window = std::max (1, (int) (0.01 * sr));
    for (auto& h : hits)
    {
        // The part's next hit, counting the ones too quiet to keep.
        const auto following = std::upper_bound (onsets.begin(), onsets.end(), h.onset);
        const int next = following != onsets.end() ? *following - preroll : n;
        h.end = std::min ({ next, h.onset + maxLength, regionOf (h.onset)->getEnd() });

        // Where it has died away, if that comes sooner: 45 dB below its loudest 10 ms, and staying
        // there for 100 ms (so the sample doesn't carry on with faint bleed from other drums). And
        // where something new starts in the part (the level jumps 10 dB from where it had got down
        // to), even when that was too quiet to count as a hit.
        std::vector<float> levels;
        for (int i = h.onset; i + window <= h.end; i += window)
            levels.push_back (rms (x, i, i + window));
        if (! levels.empty())
        {
            // Its own loudest moment, in its first 50 ms (later on, it may be the next hit's).
            const auto loudest = std::max_element (levels.begin(), levels.begin() + std::min<std::ptrdiff_t> ((std::ptrdiff_t) levels.size(), 5));
            const float quiet = *loudest * juce::Decibels::decibelsToGain (-45.0f);
            const auto hold = (std::ptrdiff_t) std::max (1, (int) (0.1 * sr) / window);
            auto lowest = loudest;
            for (auto it = loudest; it != levels.end(); ++it)
            {
                if (*it > *lowest * juce::Decibels::decibelsToGain (10.0f) && *it > quiet)
                {
                    h.end = std::min (h.end, h.onset + (int) (lowest - levels.begin() + 1) * window);
                    break;
                }
                if (*it < *lowest)
                    lowest = it;
                if (std::all_of (it, std::min (levels.end(), it + hold), [quiet] (float l) { return l < quiet; }))
                {
                    h.end = std::min (h.end, h.onset + (int) (it - levels.begin() + 1) * window);
                    break;
                }
            }
        }

        h.before = h.peak > 0.0f ? rms (x, h.onset - (int) (0.02 * sr), h.onset - preroll) / h.peak : 0.0f;
    }
    return hits;
}

// Groups the hits that sound alike. How much hits of the same drum differ depends on the song (a
// programmed sample barely changes, a live drummer does, and other drums bleed into every part), so
// the threshold follows the part: a few times the typical distance between a hit and its closest
// match. The hits with the most close matches lead the groups, so each group forms around its
// most typical sound; every other hit joins the closest group within the threshold, or starts one.
std::vector<std::vector<size_t>> groupHits (const std::vector<Hit>& hits)
{
    const size_t n = hits.size();
    if (n < 2)
        return n == 1 ? std::vector<std::vector<size_t>> { { 0 } } : std::vector<std::vector<size_t>> {};

    std::vector<float> distances (n * n, 0.0f), closest (n, std::numeric_limits<float>::max());
    for (size_t i = 0; i < n; ++i)
        for (size_t j = i + 1; j < n; ++j)
        {
            const float d = Fingerprinter::distance (hits[i].print, hits[j].print);
            distances[i * n + j] = distances[j * n + i] = d;
            closest[i] = std::min (closest[i], d);
            closest[j] = std::min (closest[j], d);
        }
    auto typical = closest;
    std::nth_element (typical.begin(), typical.begin() + (long) n / 2, typical.end());
    const float threshold = juce::jlimit (minSameSound, maxSameSound, sameSoundFactor * typical[n / 2]);

    std::vector<int> matches (n, 0);
    for (size_t i = 0; i < n; ++i)
        for (size_t j = 0; j < n; ++j)
            matches[i] += j != i && distances[i * n + j] < threshold ? 1 : 0;
    std::vector<size_t> order (n);
    std::iota (order.begin(), order.end(), size_t { 0 });
    std::stable_sort (order.begin(), order.end(), [&] (size_t a, size_t b)
                      { return matches[a] != matches[b] ? matches[a] > matches[b] : hits[a].peak > hits[b].peak; });

    std::vector<std::vector<size_t>> groups;
    for (size_t k : order)
    {
        int best = -1;
        float bestDistance = threshold;
        for (size_t g = 0; g < groups.size(); ++g)
            if (const float d = distances[k * n + groups[g].front()]; d < bestDistance)
            {
                bestDistance = d;
                best = (int) g;
            }
        if (best >= 0)
            groups[(size_t) best].push_back (k);
        else
            groups.push_back ({ k });
    }
    return groups;
}
} // namespace

//==============================================================================
AnalysisResult DrumAnalyzer::extract (const std::vector<juce::AudioBuffer<float>>& parts, double sr,
                                      const std::function<void (float)>& progress,
                                      const std::function<bool()>& shouldCancel,
                                      const std::vector<juce::Range<int>>& regions)
{
    AnalysisResult result;
    if ((int) parts.size() != numDrumClasses || sr <= 0.0)
        return result;

    const int preroll = (int) (0.002 * sr), fadeOut = (int) (0.005 * sr);
    Fingerprinter fingerprinter (sr);
    float loudestPart = 0.0f;
    for (auto& part : parts)
        for (int ch = 0; ch < part.getNumChannels(); ++ch)
            loudestPart = std::max (loudestPart, part.getMagnitude (ch, 0, part.getNumSamples()));

    for (int c = 0; c < numDrumClasses; ++c)
    {
        if (shouldCancel && shouldCancel())
            return {};

        const auto drumClass = (DrumClass) c;
        const auto& part = parts[(size_t) c];
        if (part.getNumSamples() == 0 || part.getNumChannels() == 0)
            continue;
        const auto x = toMono (part);
        auto hits = findHits (x, sr, drumClass, loudestPart, regions);
        for (auto& h : hits)
            h.print = fingerprinter.make (x, h.onset, h.end - h.onset);

        // The hits of this part on the waveform.
        const auto firstIndex = (int) result.hits.size();
        for (auto& h : hits)
            result.hits.push_back ({ h.onset, drumClass, h.peak });

        auto groups = groupHits (hits);

        // A sound that occurs only once (or in under 2% of the part's hits) is usually a leftover of
        // something else; keep it only if nothing in the part repeats. Most frequent first.
        std::stable_sort (groups.begin(), groups.end(), [] (auto& a, auto& b) { return a.size() > b.size(); });
        const size_t minHits = std::max<size_t> (2, (hits.size() + 49) / 50);
        if (! groups.empty() && groups.front().size() >= minHits)
            groups.erase (std::remove_if (groups.begin(), groups.end(), [minHits] (auto& g) { return g.size() < minHits; }), groups.end());
        else if (groups.size() > 1)
            groups.resize (1);
        if (groups.size() > (size_t) maxVariationsPerClass)
            groups.resize ((size_t) maxVariationsPerClass);

        for (auto& group : groups)
        {
            // The cleanest hit: among the loud ones (within 6 dB of the loudest), the one with the most
            // room to ring out, then the least of the previous hit's tail underneath it.
            const float loudest = hits[group.front()].peak;
            int longest = 0;
            for (size_t k : group)
                if (hits[k].peak >= loudest * 0.5f)
                    longest = std::max (longest, hits[k].end - hits[k].onset);
            size_t best = group.front();
            float bestScore = -1e9f;
            for (size_t k : group)
                if (hits[k].peak >= loudest * 0.5f)
                {
                    const float room = (float) (hits[k].end - hits[k].onset) / (float) std::max (1, longest);
                    const float score = room - 2.0f * hits[k].before;
                    if (score > bestScore)
                    {
                        bestScore = score;
                        best = k;
                    }
                }

            // Cut it out as it is: from 2 ms before the hit (faded in) to where its sound ends (5 ms fade-out).
            const auto& h = hits[best];
            const int start = std::max (0, h.onset - preroll);
            const int length = h.end - start;
            if (length <= preroll + fadeOut)
                continue;
            DrumSample sample;
            sample.sampleRate = sr;
            sample.audio.setSize (part.getNumChannels(), length);
            for (int ch = 0; ch < part.getNumChannels(); ++ch)
                sample.audio.copyFrom (ch, 0, part, ch, start, length);
            const int fadeIn = h.onset - start;
            if (fadeIn > 0)
                sample.audio.applyGainRamp (0, fadeIn, 0.0f, 1.0f);
            sample.audio.applyGainRamp (length - fadeOut, fadeOut, 1.0f, 0.0f);

            sample.hitsStacked = (int) group.size();
            for (size_t k : group)
                sample.sourceHits.push_back (firstIndex + (int) k);
            result.kit[(size_t) c].push_back (std::move (sample));
        }

        if (progress)
            progress ((float) (c + 1) / (float) numDrumClasses);
    }

    // All parts' hits in time order for the waveform, with the sounds' hit indices remapped.
    std::vector<int> order (result.hits.size());
    std::iota (order.begin(), order.end(), 0);
    std::stable_sort (order.begin(), order.end(), [&] (int a, int b) { return result.hits[(size_t) a].position < result.hits[(size_t) b].position; });
    std::vector<int> newIndex (order.size());
    std::vector<DetectedHit> sorted;
    sorted.reserve (order.size());
    for (size_t k = 0; k < order.size(); ++k)
    {
        newIndex[(size_t) order[k]] = (int) k;
        sorted.push_back (result.hits[(size_t) order[k]]);
    }
    result.hits = std::move (sorted);
    for (auto& variations : result.kit)
        for (auto& sample : variations)
            for (int& h : sample.sourceHits)
                h = newIndex[(size_t) h];
    return result;
}
} // namespace kitnap
