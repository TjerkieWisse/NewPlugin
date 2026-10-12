// Builds a synthetic beat (drums + bass + chords), runs the analyzer on it and checks that
// every drum is detected, classified and extracted cleanly.
//
// Usage: KitnapAnalyzerTest [output-folder]      writes the extracted kit as WAVs when a folder is given
//        KitnapAnalyzerTest --file song.wav [out] analyses a real file instead
//        KitnapAnalyzerTest --layered [output-folder]  the same with layered drums (snare + open hat, kick + hat)
//        KitnapAnalyzerTest --layered-always [out]     ...and the open hat never plays alone
//        KitnapAnalyzerTest --write-mix beat.wav     just writes the synthetic test beat
//        KitnapAnalyzerTest --separate song.mp3 drums.wav   writes the AI-separated drum stem

#include <juce_audio_formats/juce_audio_formats.h>
#include "analysis/DrumAnalyzer.h"
#include "separation/DrumSeparator.h"
#include <cmath>
#include <iostream>
#include <map>
#include <random>

using namespace kitnap;

namespace
{
constexpr double sr = 44100.0;
constexpr float twoPi = juce::MathConstants<float>::twoPi;

std::vector<float> noise (int n, unsigned seed)
{
    std::mt19937 rng (seed);
    std::uniform_real_distribution<float> dist (-1.0f, 1.0f);
    std::vector<float> v ((size_t) n);
    for (auto& x : v)
        x = dist (rng);
    return v;
}

void onePoleHighpass (std::vector<float>& v, float hz)
{
    const float a = std::exp (-twoPi * hz / (float) sr);
    float lp = 0.0f;
    for (auto& x : v)
    {
        lp = (1.0f - a) * x + a * lp;
        x -= lp;
    }
}

void onePoleLowpass (std::vector<float>& v, float hz)
{
    const float a = std::exp (-twoPi * hz / (float) sr);
    float lp = 0.0f;
    for (auto& x : v)
        x = lp = (1.0f - a) * x + a * lp;
}

std::vector<float> makeKick()
{
    const int n = (int) (0.45 * sr);
    std::vector<float> v ((size_t) n);
    float phase = 0.0f;
    for (int i = 0; i < n; ++i)
    {
        const float t = (float) i / (float) sr;
        const float freq = 48.0f + 110.0f * std::exp (-t * 30.0f);
        phase += twoPi * freq / (float) sr;
        v[(size_t) i] = std::sin (phase) * std::exp (-t * 7.0f) + (i < 60 ? 0.4f * (1.0f - (float) i / 60.0f) : 0.0f);
    }
    return v;
}

std::vector<float> makeSnare()
{
    const int n = (int) (0.3 * sr);
    auto nz = noise (n, 7);
    onePoleHighpass (nz, 900.0f);
    std::vector<float> v ((size_t) n);
    for (int i = 0; i < n; ++i)
    {
        const float t = (float) i / (float) sr;
        v[(size_t) i] = 0.6f * std::sin (twoPi * 185.0f * t) * std::exp (-t * 28.0f) + 0.7f * nz[(size_t) i] * std::exp (-t * 16.0f);
    }
    return v;
}

std::vector<float> makeClap()
{
    const int n = (int) (0.3 * sr);
    auto nz = noise (n, 11);
    onePoleHighpass (nz, 1000.0f);
    onePoleLowpass (nz, 3500.0f);
    std::vector<float> v ((size_t) n, 0.0f);
    const float bursts[] { 0.0f, 0.010f, 0.021f, 0.031f };
    for (int i = 0; i < n; ++i)
    {
        const float t = (float) i / (float) sr;
        float env = 0.0f;
        for (float b : bursts)
            if (t >= b && t < b + 0.0095f)
                env = std::max (env, std::exp (-(t - b) * 260.0f));
        if (t >= bursts[3])
            env = std::max (env, 0.8f * std::exp (-(t - bursts[3]) * 18.0f));
        v[(size_t) i] = 2.2f * nz[(size_t) i] * env;
    }
    return v;
}

std::vector<float> makeHat (bool open)
{
    const int n = (int) ((open ? 0.5 : 0.08) * sr);
    auto nz = noise (n, open ? 23u : 19u);
    onePoleHighpass (nz, 7000.0f);
    onePoleHighpass (nz, 7000.0f);
    std::vector<float> v ((size_t) n);
    for (int i = 0; i < n; ++i)
    {
        const float t = (float) i / (float) sr;
        v[(size_t) i] = 1.4f * nz[(size_t) i] * std::exp (-t * (open ? 7.0f : 70.0f));
    }
    return v;
}

void add (std::vector<float>& mix, const std::vector<float>& s, int at, float gain)
{
    // 5 ms fade at the end, like a properly trimmed sample.
    const size_t fade = (size_t) (0.005 * sr);
    for (size_t i = 0; i < s.size() && at + (int) i < (int) mix.size(); ++i)
    {
        const float f = i + fade > s.size() ? (float) (s.size() - i) / (float) fade : 1.0f;
        mix[(size_t) at + i] += gain * s[i] * f;
    }
}

struct Truth
{
    int position;
    DrumClass drumClass;
};

// From `skip` samples on (to judge only the tail, after the attack).
float bestCorrelation (const std::vector<float>& reference, const juce::AudioBuffer<float>& extracted, int skip = 0)
{
    const int n = std::min ((int) reference.size(), extracted.getNumSamples());
    float best = 0.0f;
    for (int lag = -100; lag <= 100; ++lag)
    {
        double cross = 0, ea = 0, eb = 0;
        for (int i = skip; i < n; ++i)
        {
            const int j = i + lag;
            if (j < 0 || j >= extracted.getNumSamples())
                continue;
            const float a = reference[(size_t) i], b = extracted.getSample (0, j);
            cross += a * b; ea += a * a; eb += b * b;
        }
        if (ea > 0 && eb > 0)
            best = std::max (best, (float) (cross / std::sqrt (ea * eb)));
    }
    return best;
}

void writeKit (const AnalysisResult& result, const juce::File& folder)
{
    folder.createDirectory();
    juce::WavAudioFormat wav;
    for (int c = 0; c < numDrumClasses; ++c)
        for (size_t v = 0; v < result.kit[(size_t) c].size(); ++v)
        {
            auto& s = result.kit[(size_t) c][v];
            auto file = folder.getChildFile (juce::String (drumClassName ((DrumClass) c)).replace (" ", "_")
                                             + "_" + juce::String ((int) v + 1) + ".wav");
            file.deleteFile();
            std::unique_ptr<juce::OutputStream> stream (file.createOutputStream().release());
            if (auto writer = std::unique_ptr<juce::AudioFormatWriter> (
                    wav.createWriterFor (stream.get(), s.sampleRate, 2, 24, {}, 0)))
            {
                stream.release();
                writer->writeFromAudioSampleBuffer (s.audio, 0, s.audio.getNumSamples());
            }
        }
}

void printKit (const AnalysisResult& result)
{
    for (int c = 0; c < numDrumClasses; ++c)
    {
        std::cout << "  " << drumClassName ((DrumClass) c) << ": " << result.kit[(size_t) c].size() << " variation(s)";
        for (auto& s : result.kit[(size_t) c])
            std::cout << " [" << s.hitsStacked << " hits, " << juce::roundToInt (1000.0 * s.audio.getNumSamples() / s.sampleRate) << " ms]";
        std::cout << "\n";
    }
}

int analyseFile (const juce::File& file, const juce::File& out)
{
    juce::AudioFormatManager formats;
    formats.registerBasicFormats();
    std::unique_ptr<juce::AudioFormatReader> reader (formats.createReaderFor (file));
    if (reader == nullptr)
    {
        std::cerr << "Cannot read " << file.getFullPathName() << "\n";
        return 1;
    }
    juce::AudioBuffer<float> audio ((int) reader->numChannels, (int) reader->lengthInSamples);
    reader->read (&audio, 0, audio.getNumSamples(), 0, true, true);

    const auto started = juce::Time::getMillisecondCounterHiRes();
    const auto result = DrumAnalyzer::analyse (audio, reader->sampleRate);
    std::cout << "Analysed in " << juce::roundToInt (juce::Time::getMillisecondCounterHiRes() - started) << " ms, "
              << result.hits.size() << " hits\n";
    printKit (result);
    if (out != juce::File())
        writeKit (result, out);
    return 0;
}
} // namespace

// Writes the AI-separated drum stem of `in` to `out` (24-bit WAV).
int separateFile (const juce::File& in, const juce::File& out)
{
    if (! DrumSeparator::isAvailable())
    {
        std::cerr << "AI separation unavailable: " << DrumSeparator::unavailableReason() << "\n";
        return 1;
    }

    juce::AudioFormatManager formats;
    formats.registerBasicFormats();
    std::unique_ptr<juce::AudioFormatReader> reader (formats.createReaderFor (in));
    if (reader == nullptr)
        return 1;
    juce::AudioBuffer<float> audio ((int) reader->numChannels, (int) reader->lengthInSamples);
    reader->read (&audio, 0, audio.getNumSamples(), 0, true, true);

    const auto started = juce::Time::getMillisecondCounterHiRes();
    juce::String error;
    const auto drums = DrumSeparator::separate (audio, reader->sampleRate,
                                                [] (float p) { std::cout << "\r" << juce::roundToInt (p * 100.0f) << "%" << std::flush; },
                                                {}, error);
    std::cout << "\nSeparated in " << juce::roundToInt (juce::Time::getMillisecondCounterHiRes() - started) << " ms\n";
    if (drums.getNumSamples() == 0)
    {
        std::cerr << "Separation failed: " << error << "\n";
        return 1;
    }

    out.deleteFile();
    juce::WavAudioFormat wav;
    std::unique_ptr<juce::OutputStream> stream (out.createOutputStream().release());
    if (auto writer = std::unique_ptr<juce::AudioFormatWriter> (wav.createWriterFor (stream.get(), DrumSeparator::modelSampleRate, 2, 24, {}, 0)))
    {
        stream.release();
        writer->writeFromAudioSampleBuffer (drums, 0, drums.getNumSamples());
    }
    return 0;
}

int main (int argc, char** argv)
{
    if (argc >= 4 && juce::String (argv[1]) == "--separate")
        return separateFile (juce::File::getCurrentWorkingDirectory().getChildFile (argv[2]),
                             juce::File::getCurrentWorkingDirectory().getChildFile (argv[3]));

    if (argc >= 3 && juce::String (argv[1]) == "--file")
        return analyseFile (juce::File::getCurrentWorkingDirectory().getChildFile (argv[2]),
                            argc >= 4 ? juce::File::getCurrentWorkingDirectory().getChildFile (argv[3]) : juce::File());

    // --layered: every snare has an open hat on top and kicks land together with closed hats;
    // the open and closed hats also play alone elsewhere, as in most real beats.
    // --layered-always: the same, but the open hat never plays alone (so no clean copy of it exists).
    const bool alwaysLayered = argc >= 2 && juce::String (argv[1]) == "--layered-always";
    const bool layered = alwaysLayered || (argc >= 2 && juce::String (argv[1]) == "--layered");
    if (layered)
    {
        --argc;
        ++argv;
    }

    const double bpm = 120.0;
    const int step = (int) (sr * 60.0 / bpm / 4.0); // sixteenth note
    const int bars = 8;
    std::vector<float> mix ((size_t) (step * 16 * bars + sr), 0.0f);
    std::vector<Truth> truth;

    const auto kick = makeKick(), snare = makeSnare(), clap = makeClap();
    const auto hat = makeHat (false), openHat = makeHat (true);

    for (int bar = 0; bar < bars; ++bar)
        for (int s = 0; s < 16; ++s)
        {
            const int at = (bar * 16 + s) * step;
            const float humanise = 0.85f + 0.15f * (float) ((bar * 7 + s * 3) % 5) / 4.0f;
            if (layered)
            {
                auto hit = [&] (const std::vector<float>& sound, float gain, DrumClass c)
                {
                    add (mix, sound, at, gain * humanise);
                    truth.push_back ({ at, c });
                };
                if (s == 0 || s == 10) hit (kick, 0.9f, DrumClass::kick);
                if (s == 0 || s == 2 || s == 8 || s == 10) hit (hat, 0.25f, DrumClass::hat);
                if (s == 4 || s == 12) hit (snare, 0.7f, DrumClass::snare);
                if (s == 4 || s == 12 || (s == 6 && ! alwaysLayered)) hit (openHat, 0.2f, DrumClass::openHat);
                continue;
            }
            if (s == 0 || s == 10)
            {
                add (mix, kick, at, 0.9f * humanise);
                truth.push_back ({ at, DrumClass::kick });
            }
            if (s == 4 || s == 12)
            {
                const bool useClap = bar % 2 == 1;
                add (mix, useClap ? clap : snare, at, 0.7f * humanise);
                truth.push_back ({ at, useClap ? DrumClass::clap : DrumClass::snare });
            }
            if (s % 2 == 0 && s != 0 && s != 4 && s != 10 && s != 12 && s != 14)
            {
                add (mix, hat, at, 0.25f * humanise);
                truth.push_back ({ at, DrumClass::hat });
            }
            if (s == 14)
            {
                add (mix, openHat, at, 0.2f * humanise);
                truth.push_back ({ at, DrumClass::openHat });
            }
        }

    // Bass line and soft chords on top, so the drums are not alone.
    const float bassNotes[] { 55.0f, 65.41f, 49.0f, 73.42f };
    const float chordRoots[] { 220.0f, 261.63f, 196.0f, 293.66f };
    for (int i = 0; i < (int) mix.size(); ++i)
    {
        const float t = (float) i / (float) sr;
        const int stepIndex = i / step;
        const int bar = (stepIndex / 16) % 4;
        const int posInBar = stepIndex % 16;
        const float bassFreq = bassNotes[(bar + (posInBar >= 8 ? 1 : 0)) % 4];
        const float bassEnv = std::exp (-(float) ((i % (step * 8)) / sr) * 2.0f);
        mix[(size_t) i] += 0.25f * bassEnv * std::tanh (2.0f * std::sin (twoPi * bassFreq * t));

        const float root = chordRoots[bar];
        mix[(size_t) i] += 0.05f * (std::sin (twoPi * root * t) + std::sin (twoPi * root * 1.26f * t) + std::sin (twoPi * root * 1.5f * t));
    }

    juce::AudioBuffer<float> audio (2, (int) mix.size());
    for (int ch = 0; ch < 2; ++ch)
        audio.copyFrom (ch, 0, mix.data(), (int) mix.size());

    if (argc >= 3 && juce::String (argv[1]) == "--write-mix")
    {
        const auto file = juce::File::getCurrentWorkingDirectory().getChildFile (argv[2]);
        file.deleteFile();
        juce::WavAudioFormat wav;
        std::unique_ptr<juce::OutputStream> stream (file.createOutputStream().release());
        if (auto writer = std::unique_ptr<juce::AudioFormatWriter> (wav.createWriterFor (stream.get(), sr, 2, 24, {}, 0)))
        {
            stream.release();
            writer->writeFromAudioSampleBuffer (audio, 0, audio.getNumSamples());
        }
        return 0;
    }

    const auto started = juce::Time::getMillisecondCounterHiRes();
    const auto result = DrumAnalyzer::analyse (audio, sr);
    std::cout << "Analysed " << bars << " bars in " << juce::roundToInt (juce::Time::getMillisecondCounterHiRes() - started)
              << " ms: " << result.hits.size() << " hits detected, " << truth.size() << " expected\n";

    // Detection and classification accuracy against the ground truth.
    std::map<std::pair<int, int>, int> confusion;
    int detected = 0;
    for (auto& t : truth)
    {
        const DetectedHit* nearest = nullptr;
        for (auto& h : result.hits)
            if (std::abs (h.position - t.position) < (int) (0.012 * sr))
                nearest = &h;
        if (nearest != nullptr)
        {
            ++detected;
            const bool secondaryMatch = nearest->secondaryClass == (int) t.drumClass;
            ++confusion[{ (int) t.drumClass, secondaryMatch ? (int) t.drumClass : (int) nearest->drumClass }];
        }
    }

    std::cout << "Detected " << detected << "/" << truth.size() << " true hits\nConfusion (truth -> found):\n";
    int correct = 0;
    for (auto& [key, count] : confusion)
    {
        std::cout << "  " << drumClassName ((DrumClass) key.first) << " -> " << drumClassName ((DrumClass) key.second) << ": " << count << "\n";
        if (key.first == key.second)
            correct += count;
    }
    printKit (result);

    // Each extracted sound should closely match the drum that was put in.
    bool ok = detected >= (int) truth.size() * 9 / 10 && correct >= detected * 8 / 10;
    const std::pair<DrumClass, const std::vector<float>*> references[] {
        { DrumClass::kick, &kick }, { DrumClass::snare, &snare }, { DrumClass::clap, &clap },
        { DrumClass::hat, &hat }, { DrumClass::openHat, &openHat }
    };
    for (auto& [drumClass, reference] : references)
    {
        if (layered && drumClass == DrumClass::clap)
            continue;
        auto& variations = result.kit[(size_t) drumClass];
        const float corr = variations.empty() ? 0.0f : bestCorrelation (*reference, variations.front().audio);
        std::cout << "Similarity " << drumClassName (drumClass) << ": " << juce::String (corr, 3);

        // Layering: how much of the drum it was layered with is still inside the extracted sound.
        const std::vector<float>* other = nullptr;
        if (layered)
            switch (drumClass)
            {
                case DrumClass::kick:    other = &hat; break;
                case DrumClass::hat:     other = &kick; break;
                case DrumClass::snare:   other = &openHat; break;
                case DrumClass::openHat: other = &snare; break;
                case DrumClass::clap:
                case DrumClass::perc:    break;
            }
        if (alwaysLayered && drumClass == DrumClass::openHat)
            other = nullptr; // no open hat of its own to judge
        if (other != nullptr && ! variations.empty())
        {
            // With no clean copy of the layered drum anywhere, only its ringing on can be removed (its
            // attack lands on, and is masked by, the main drum's attack), so judge the tail.
            const int skip = alwaysLayered ? (int) (0.05 * sr) : 0;
            const float leak = bestCorrelation (*other, variations.front().audio, skip);
            const float leakDb = juce::Decibels::gainToDecibels (leak, -60.0f);
            std::cout << "  (layered drum still in " << (skip > 0 ? "its tail: " : "it: ") << juce::String (leakDb, 1) << " dB)";
            // With a clean copy elsewhere the other drum must be 20 dB down. Without one, only its
            // ringing on can be told apart from the main drum, so the bar is lower.
            if (leakDb > (alwaysLayered ? -13.0f : -20.0f))
                ok = false;
        }
        std::cout << "\n";
        if (corr < 0.8f && ! (alwaysLayered && drumClass == DrumClass::openHat))
            ok = false;
    }

    if (argc == 2)
        writeKit (result, juce::File::getCurrentWorkingDirectory().getChildFile (argv[1]));

    std::cout << (ok ? "PASS" : "FAIL") << "\n";
    return ok ? 0 : 1;
}
