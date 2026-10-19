#include "SongSections.h"
#include <juce_dsp/juce_dsp.h>
#include <algorithm>
#include <cmath>

namespace kitnap
{
namespace
{
// Spectral flux per frame: the new energy in each frame, on log-compressed magnitudes relative to
// the loudest bin in the song (so a louder passage counts for more, but not overwhelmingly).
std::vector<float> spectralFlux (const juce::AudioBuffer<float>& song, int hop)
{
    constexpr int order = 10, size = 1 << order, bins = size / 2;
    const int n = song.getNumSamples(), channels = song.getNumChannels();
    if (n < size || channels == 0)
        return {};

    std::vector<float> mono ((size_t) n, 0.0f);
    for (int ch = 0; ch < channels; ++ch)
        juce::FloatVectorOperations::addWithMultiply (mono.data(), song.getReadPointer (ch), 1.0f / (float) channels, n);

    const int frames = (n - size) / hop + 1;
    juce::dsp::FFT fft (order);
    std::vector<float> window ((size_t) size), buffer ((size_t) size * 2);
    for (int i = 0; i < size; ++i)
        window[(size_t) i] = 0.5f - 0.5f * std::cos (juce::MathConstants<float>::twoPi * (float) i / (float) size);

    std::vector<float> magnitude ((size_t) bins);
    auto analyse = [&] (int frame)
    {
        std::fill (buffer.begin(), buffer.end(), 0.0f);
        for (int i = 0; i < size; ++i)
            buffer[(size_t) i] = mono[(size_t) (frame * hop + i)] * window[(size_t) i];
        fft.performFrequencyOnlyForwardTransform (buffer.data(), true);
        std::copy (buffer.begin() + 1, buffer.begin() + bins + 1, magnitude.begin());
    };

    // Two passes (the FFTs are cheap): the loudest bin first, then the flux.
    float reference = 0.0f;
    for (int f = 0; f < frames; ++f)
    {
        analyse (f);
        reference = std::max (reference, *std::max_element (magnitude.begin(), magnitude.end()));
    }
    std::vector<float> flux ((size_t) frames, 0.0f);
    if (reference <= 0.0f)
        return flux;

    const float scale = 1000.0f / reference;
    std::vector<float> previous ((size_t) bins, 0.0f), current ((size_t) bins);
    for (int f = 0; f < frames; ++f)
    {
        analyse (f);
        for (int b = 0; b < bins; ++b)
            current[(size_t) b] = std::log1p (magnitude[(size_t) b] * scale);
        if (f > 0)
            for (int b = 0; b < bins; ++b)
                flux[(size_t) f] += std::max (0.0f, current[(size_t) b] - previous[(size_t) b]);
        std::swap (previous, current);
    }
    return flux;
}
} // namespace

SongSections SongSections::pick (const juce::AudioBuffer<float>& song, double sampleRate)
{
    SongSections result;
    result.sampleRate = sampleRate;
    const int length = song.getNumSamples();
    const int section = (int) (sectionSeconds * sampleRate);
    if (length <= (int) (maxSections * sectionSeconds * 1.25 * sampleRate))
    {
        result.sections.push_back ({ 0, length });
        return result;
    }

    // How busy every window of `section` samples is: the flux summed over it.
    const int hop = 512;
    const auto flux = spectralFlux (song, hop);
    std::vector<double> sum (flux.size() + 1, 0.0);
    for (size_t f = 0; f < flux.size(); ++f)
        sum[f + 1] = sum[f] + flux[f];
    auto score = [&] (int start)
    {
        const auto from = (size_t) juce::jlimit (0, (int) flux.size(), start / hop);
        const auto to = (size_t) juce::jlimit (0, (int) flux.size(), (start + section) / hop);
        return sum[to] - sum[from];
    };

    // The busiest window centred in each of maxSections equal stretches of the song, without
    // overlapping one already taken (the busiest stretches choose first).
    struct Candidate { int start; double score; };
    const int step = (int) (0.25 * sampleRate);
    auto bestIn = [&] (int stretch, const std::vector<Candidate>& taken)
    {
        const int from = (int) ((juce::int64) length * stretch / maxSections) - section / 2;
        const int to = (int) ((juce::int64) length * (stretch + 1) / maxSections) - section / 2;
        Candidate best { -1, -1.0 };
        for (int start = std::max (0, from); start <= std::min (to, length - section); start += step)
        {
            const bool free = std::none_of (taken.begin(), taken.end(), [&] (auto& t) { return std::abs (t.start - start) < section; });
            if (const double sc = score (start); free && sc > best.score)
                best = { start, sc };
        }
        return best;
    };

    std::vector<std::pair<double, int>> order; // each stretch's busiest window on its own
    for (int s = 0; s < maxSections; ++s)
        order.push_back ({ bestIn (s, {}).score, s });
    std::sort (order.begin(), order.end(), std::greater<>());

    std::vector<Candidate> taken;
    for (auto& [ignored, stretch] : order)
        if (const auto c = bestIn (stretch, taken); c.start >= 0)
            taken.push_back (c);

    // Skip windows with far fewer hits than the busiest one: probably no drums there.
    const double busiest = order.front().first;
    std::sort (taken.begin(), taken.end(), [] (auto& a, auto& b) { return a.start < b.start; });
    for (auto& c : taken)
        if (c.score >= 0.35 * busiest)
            result.sections.push_back ({ c.start, c.start + section });
    return result;
}

juce::AudioBuffer<float> SongSections::join (const juce::AudioBuffer<float>& song) const
{
    const int gap = (int) (gapSeconds * sampleRate), fade = (int) (0.01 * sampleRate);
    int total = 0;
    for (auto& s : sections)
        total += s.getLength() + gap;
    juce::AudioBuffer<float> joined (song.getNumChannels(), std::max (0, total - gap));
    joined.clear();

    int at = 0;
    for (auto& s : sections)
    {
        for (int ch = 0; ch < song.getNumChannels(); ++ch)
            joined.copyFrom (ch, at, song, ch, s.getStart(), s.getLength());
        const int edge = std::min (fade, s.getLength() / 2);
        if (s.getStart() > 0)
            joined.applyGainRamp (at, edge, 0.0f, 1.0f);
        if (s.getEnd() < song.getNumSamples())
            joined.applyGainRamp (at + s.getLength() - edge, edge, 1.0f, 0.0f);
        at += s.getLength() + gap;
    }
    return joined;
}

std::vector<juce::Range<int>> SongSections::joinedRanges (double rate) const
{
    const double ratio = rate / sampleRate;
    const int gap = (int) (gapSeconds * sampleRate);
    std::vector<juce::Range<int>> ranges;
    int at = 0;
    for (auto& s : sections)
    {
        ranges.push_back ({ (int) std::lround (at * ratio), (int) std::lround ((at + s.getLength()) * ratio) });
        at += s.getLength() + gap;
    }
    return ranges;
}

int SongSections::toSong (int joinedPosition, double rate) const
{
    const auto ranges = joinedRanges (rate);
    const double ratio = rate / sampleRate;
    for (size_t k = 0; k < ranges.size(); ++k)
        if (joinedPosition < ranges[k].getEnd() || k + 1 == ranges.size())
            return (int) std::lround (sections[k].getStart() * ratio) + joinedPosition - ranges[k].getStart();
    return joinedPosition;
}

std::vector<juce::Range<double>> SongSections::seconds() const
{
    std::vector<juce::Range<double>> result;
    for (auto& s : sections)
        result.push_back ({ s.getStart() / sampleRate, s.getEnd() / sampleRate });
    return result;
}
} // namespace kitnap
