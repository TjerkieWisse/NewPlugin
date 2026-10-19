// Tests the kit extraction (one pad per part of the AI drum split) and runs the AI models on files.
//
// Usage: KitnapAnalyzerTest [output-folder]
//            builds five synthetic drum parts (kick, snare, toms, hi-hat, cymbals), extracts the kit and
//            checks that every pad gets its own drum, cut out unchanged; writes the kit when a folder is given
//        KitnapAnalyzerTest --write-mix beat.wav [seconds]  writes a synthetic beat with bass and chords
//        KitnapAnalyzerTest --separate song.mp3 drums.wav   writes the AI-separated drum stem
//        KitnapAnalyzerTest --split drums.wav folder        writes the five parts of a drum stem (kick.wav, ...)
//        KitnapAnalyzerTest --file-ai song.mp3 [out]        the plugin's pipeline: pick sections, separate, split, extract
//                                                           (out also gets the sections' drum stem and parts)
//        KitnapAnalyzerTest --file-ai-full song.mp3 [out]   the same on the whole song
//        KitnapAnalyzerTest --sections song.mp3             lists the sections the AI would process
//        KitnapAnalyzerTest --file-parts folder [out]       the last step only, from the parts --split wrote
//        KitnapAnalyzerTest --hits folder                   lists the hits found in those parts
//        KitnapAnalyzerTest --compare a.wav b.wav [dB]      how far apart two stems are (e.g. GPU and CPU);
//                                                           fails if more than dB (default -40) below a

#include <juce_audio_formats/juce_audio_formats.h>
#include "analysis/DrumAnalyzer.h"
#include "analysis/SongSections.h"
#include "separation/DrumSeparator.h"
#include <cmath>
#include <iostream>
#include <random>

using namespace kitnap;

namespace
{
constexpr double sr = 44100.0;
constexpr float twoPi = juce::MathConstants<float>::twoPi;
const char* partFileNames[] { "kick", "snare", "toms", "hihat", "cymbals" };

juce::File arg (const char* path) { return juce::File::getCurrentWorkingDirectory().getChildFile (path); }

//==============================================================================
// Synthetic drums.
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

// A sine whose pitch falls from start + drop to start, with a click on top.
std::vector<float> makeDrum (double seconds, float start, float drop, float pitchDecay, float decay, float click)
{
    const int n = (int) (seconds * sr);
    std::vector<float> v ((size_t) n);
    float phase = 0.0f;
    for (int i = 0; i < n; ++i)
    {
        const float t = (float) i / (float) sr;
        phase += twoPi * (start + drop * std::exp (-t * pitchDecay)) / (float) sr;
        v[(size_t) i] = std::sin (phase) * std::exp (-t * decay) + (i < 60 ? click * (1.0f - (float) i / 60.0f) : 0.0f);
    }
    return v;
}

std::vector<float> makeKick()      { return makeDrum (0.45, 48.0f, 110.0f, 30.0f, 7.0f, 0.4f); }
std::vector<float> makeShortKick() { return makeDrum (0.25, 70.0f, 160.0f, 50.0f, 16.0f, 0.9f); }
std::vector<float> makeTom (bool high) { return makeDrum (0.45, high ? 180.0f : 95.0f, high ? 40.0f : 25.0f, 12.0f, 12.0f, 0.15f); }

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

// Noise above `highpass`, decaying at `decay` per second.
std::vector<float> makeMetal (double seconds, float highpass, float decay, unsigned seed)
{
    const int n = (int) (seconds * sr);
    auto nz = noise (n, seed);
    onePoleHighpass (nz, highpass);
    onePoleHighpass (nz, highpass);
    std::vector<float> v ((size_t) n);
    for (int i = 0; i < n; ++i)
        v[(size_t) i] = 1.4f * nz[(size_t) i] * std::exp (-(float) i / (float) sr * decay);
    return v;
}

std::vector<float> makeHat()   { return makeMetal (0.08, 7000.0f, 70.0f, 19); }
std::vector<float> makeCrash() { return makeMetal (2.5, 3000.0f, 1.8f, 31); }

void add (std::vector<float>& mix, const std::vector<float>& s, int at, float gain)
{
    const size_t fade = (size_t) (0.005 * sr); // like a properly trimmed sample
    for (size_t i = 0; i < s.size() && at + (int) i < (int) mix.size(); ++i)
    {
        const float f = i + fade > s.size() ? (float) (s.size() - i) / (float) fade : 1.0f;
        mix[(size_t) at + i] += gain * s[i] * f;
    }
}

juce::AudioBuffer<float> toStereo (const std::vector<float>& mono)
{
    juce::AudioBuffer<float> audio (2, (int) mono.size());
    for (int ch = 0; ch < 2; ++ch)
        audio.copyFrom (ch, 0, mono.data(), (int) mono.size());
    return audio;
}

struct Beat
{
    std::array<std::vector<float>, numDrumClasses> parts;
    std::array<std::vector<int>, numDrumClasses> hits; // where each part's own hits are
};

// Eight bars at 120 BPM, with two different kicks; the toms play a fill twice and the crash comes twice.
Beat makeBeat()
{
    const int step = (int) (sr * 60.0 / 120.0 / 4.0), bars = 8;
    const size_t length = (size_t) (step * 16 * bars + 3 * sr);
    Beat beat;
    for (auto& p : beat.parts)
        p.assign (length, 0.0f);

    const auto kick = makeKick(), shortKick = makeShortKick(), snare = makeSnare(), hat = makeHat(), crash = makeCrash();
    const auto highTom = makeTom (true), lowTom = makeTom (false);
    auto hit = [&] (DrumClass c, const std::vector<float>& sound, int at, float gain)
    {
        add (beat.parts[(size_t) c], sound, at, gain);
        beat.hits[(size_t) c].push_back (at);
    };

    for (int bar = 0; bar < bars; ++bar)
        for (int s = 0; s < 16; ++s)
        {
            const int at = (bar * 16 + s) * step;
            const float humanise = 0.85f + 0.15f * (float) ((bar * 7 + s * 3) % 5) / 4.0f;
            const bool fill = bar % 4 == 3 && s >= 12;
            if (s == 0 || s == 10)
                hit (DrumClass::kick, kick, at, 0.9f * humanise);
            if (s == 7)
                hit (DrumClass::kick, shortKick, at, 0.7f * humanise);
            if ((s == 4 || s == 12) && ! fill)
                hit (DrumClass::snare, snare, at, 0.7f * humanise);
            if (fill && s % 2 == 0)
                hit (DrumClass::toms, s < 14 ? highTom : lowTom, at, 0.8f * humanise);
            if (s % 2 == 0 && ! fill)
                hit (DrumClass::hihat, hat, at, 0.25f * humanise);
            if (bar % 4 == 0 && s == 0)
                hit (DrumClass::cymbals, crash, at, 0.3f);
        }
    return beat;
}

//==============================================================================
float bestCorrelation (const std::vector<float>& reference, const juce::AudioBuffer<float>& extracted)
{
    const int n = std::min ((int) reference.size(), extracted.getNumSamples());
    float best = 0.0f;
    for (int lag = -200; lag <= 200; ++lag)
    {
        double cross = 0, ea = 0, eb = 0;
        for (int i = 0; i < n; ++i)
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

void writeWav (const juce::AudioBuffer<float>& audio, double rate, const juce::File& file)
{
    file.deleteFile();
    juce::WavAudioFormat wav;
    std::unique_ptr<juce::OutputStream> stream (file.createOutputStream().release());
    if (auto writer = std::unique_ptr<juce::AudioFormatWriter> (
            wav.createWriterFor (stream.get(), rate, (unsigned int) audio.getNumChannels(), 24, {}, 0)))
    {
        stream.release();
        writer->writeFromAudioSampleBuffer (audio, 0, audio.getNumSamples());
    }
}

juce::AudioBuffer<float> readFile (const juce::File& file, double& sampleRate)
{
    juce::AudioFormatManager formats;
    formats.registerBasicFormats();
    std::unique_ptr<juce::AudioFormatReader> reader (formats.createReaderFor (file));
    if (reader == nullptr)
        return {};
    juce::AudioBuffer<float> audio ((int) reader->numChannels, (int) reader->lengthInSamples);
    reader->read (&audio, 0, audio.getNumSamples(), 0, true, true);
    sampleRate = reader->sampleRate;
    return audio;
}

void writeKit (const AnalysisResult& result, const juce::File& folder)
{
    folder.createDirectory();
    for (int c = 0; c < numDrumClasses; ++c)
        for (size_t v = 0; v < result.kit[(size_t) c].size(); ++v)
        {
            auto& s = result.kit[(size_t) c][v];
            writeWav (s.audio, s.sampleRate, folder.getChildFile (juce::String (drumClassName ((DrumClass) c)).replace ("-", "")
                                                                  + "_" + juce::String ((int) v + 1) + ".wav"));
        }
}

void printKit (const AnalysisResult& result)
{
    std::array<int, numDrumClasses> hits {};
    for (auto& h : result.hits)
        ++hits[(size_t) h.drumClass];
    for (int c = 0; c < numDrumClasses; ++c)
    {
        std::cout << "  " << drumClassName ((DrumClass) c) << ": " << hits[(size_t) c] << " hits, "
                  << result.kit[(size_t) c].size() << " sound(s)";
        for (auto& s : result.kit[(size_t) c])
            std::cout << " [" << s.hitsStacked << "x, " << juce::roundToInt (1000.0 * s.audio.getNumSamples() / s.sampleRate) << " ms]";
        std::cout << "\n";
    }
}

AnalysisResult extractTimed (const std::vector<juce::AudioBuffer<float>>& parts, double rate)
{
    const auto started = juce::Time::getMillisecondCounterHiRes();
    auto result = DrumAnalyzer::extract (parts, rate);
    std::cout << "Extracted in " << juce::roundToInt (juce::Time::getMillisecondCounterHiRes() - started) << " ms, "
              << result.hits.size() << " hits\n";
    printKit (result);
    return result;
}

void printProgress (float p) { std::cout << "\r" << juce::roundToInt (p * 100.0f) << "%" << std::flush; }

juce::AudioBuffer<float> separate (const juce::AudioBuffer<float>& audio, double rate)
{
    const auto started = juce::Time::getMillisecondCounterHiRes();
    juce::String error;
    auto drums = DrumSeparator::separate (audio, rate, printProgress, {}, error);
    std::cout << "\nSeparated in " << juce::roundToInt (juce::Time::getMillisecondCounterHiRes() - started) << " ms on "
              << DrumSeparator::backend() << "\n";
    if (drums.getNumSamples() == 0)
        std::cerr << "Separation failed: " << error << "\n";
    return drums;
}

std::vector<juce::AudioBuffer<float>> split (const juce::AudioBuffer<float>& drums)
{
    using Part = DrumSeparator::DrumPart;
    const auto started = juce::Time::getMillisecondCounterHiRes();
    juce::String error;
    auto parts = DrumSeparator::split (drums, { Part::kick, Part::snare, Part::toms, Part::hihat, Part::cymbals },
                                       printProgress, {}, error);
    std::cout << "\nSplit in " << juce::roundToInt (juce::Time::getMillisecondCounterHiRes() - started) << " ms on "
              << DrumSeparator::backend() << "\n";
    if (parts.empty())
        std::cerr << "Split failed: " << error << "\n";
    return parts;
}

bool checkAI()
{
    if (DrumSeparator::isAvailable())
        return true;
    std::cerr << "AI models unavailable: " << DrumSeparator::unavailableReason() << "\n";
    return false;
}

//==============================================================================
// --separate song drums.wav
int separateFile (const juce::File& in, const juce::File& out)
{
    double rate = 0.0;
    const auto audio = readFile (in, rate);
    if (! checkAI() || audio.getNumSamples() == 0)
        return 1;
    const auto drums = separate (audio, rate);
    if (drums.getNumSamples() == 0)
        return 1;
    writeWav (drums, DrumSeparator::modelSampleRate, out);
    return 0;
}

// --split drums.wav folder
int splitFile (const juce::File& in, const juce::File& folder)
{
    double rate = 0.0;
    const auto drums = readFile (in, rate);
    if (! checkAI())
        return 1;
    if (drums.getNumSamples() == 0 || std::abs (rate - DrumSeparator::modelSampleRate) > 0.5)
    {
        std::cerr << "Needs a 44.1 kHz drum stem\n";
        return 1;
    }
    const auto parts = split (drums);
    if (parts.empty())
        return 1;
    folder.createDirectory();
    for (int part = 0; part < DrumSeparator::numDrumParts; ++part)
        writeWav (parts[(size_t) part], DrumSeparator::modelSampleRate, folder.getChildFile (juce::String (partFileNames[part]) + ".wav"));
    return 0;
}

void printSections (const SongSections& sections, double songSeconds)
{
    double total = 0.0;
    for (auto& r : sections.seconds())
    {
        std::cout << "  " << juce::String (r.getStart(), 1) << " - " << juce::String (r.getEnd(), 1) << " s\n";
        total += r.getLength();
    }
    std::cout << "Sections: " << juce::String (total, 1) << " of " << juce::String (songSeconds, 1) << " s\n";
}

// --file-ai song [out]: what the plugin does (--file-ai-full: on the whole song).
int extractWithAI (const juce::File& in, const juce::File& out, bool wholeSong)
{
    double rate = 0.0;
    const auto audio = readFile (in, rate);
    if (! checkAI() || audio.getNumSamples() == 0)
        return 1;
    SongSections sections;
    sections.sampleRate = rate;
    sections.sections.push_back ({ 0, audio.getNumSamples() });
    if (! wholeSong)
        sections = SongSections::pick (audio, rate);
    printSections (sections, audio.getNumSamples() / rate);

    const auto started = juce::Time::getMillisecondCounterHiRes();
    const auto drums = separate (sections.join (audio), rate);
    if (drums.getNumSamples() == 0)
        return 1;
    const auto parts = split (drums);
    if (parts.empty())
        return 1;
    auto result = DrumAnalyzer::extract (parts, DrumSeparator::modelSampleRate, {}, {},
                                         sections.joinedRanges (DrumSeparator::modelSampleRate));
    for (auto& hit : result.hits)
        hit.position = sections.toSong (hit.position, DrumSeparator::modelSampleRate);
    std::cout << "Total " << juce::roundToInt ((juce::Time::getMillisecondCounterHiRes() - started) / 1000.0) << " s, "
              << result.hits.size() << " hits\n";
    printKit (result);
    if (out != juce::File())
    {
        // The kit, plus what the AI made of the sections (joined), for listening to.
        writeKit (result, out);
        out.getChildFile ("parts").createDirectory();
        writeWav (drums, DrumSeparator::modelSampleRate, out.getChildFile ("parts/drums.wav"));
        for (int part = 0; part < DrumSeparator::numDrumParts; ++part)
            writeWav (parts[(size_t) part], DrumSeparator::modelSampleRate,
                      out.getChildFile ("parts/" + juce::String (partFileNames[part]) + ".wav"));
    }

    // Any beat has a kick, a snare and hats.
    auto found = [&] (DrumClass c) { return ! result.kit[(size_t) c].empty(); };
    const bool ok = found (DrumClass::kick) && found (DrumClass::snare) && found (DrumClass::hihat);
    std::cout << (ok ? "PASS\n" : "FAIL: kick, snare or hi-hat missing\n");
    return ok ? 0 : 1;
}

std::vector<juce::AudioBuffer<float>> readParts (const juce::File& folder, double& rate)
{
    std::vector<juce::AudioBuffer<float>> parts;
    for (auto* name : partFileNames)
    {
        parts.push_back (readFile (folder.getChildFile (juce::String (name) + ".wav"), rate));
        if (parts.back().getNumSamples() == 0)
        {
            std::cerr << "Needs kick.wav, snare.wav, toms.wav, hihat.wav and cymbals.wav (from --split)\n";
            return {};
        }
    }
    return parts;
}

// --file-parts folder [out]
int extractParts (const juce::File& folder, const juce::File& out)
{
    double rate = 0.0;
    const auto parts = readParts (folder, rate);
    if (parts.empty())
        return 1;
    const auto result = extractTimed (parts, rate);
    if (out != juce::File())
        writeKit (result, out);
    return 0;
}

// --hits folder: every hit as "<sample position> <drum>".
int listHits (const juce::File& folder)
{
    double rate = 0.0;
    const auto parts = readParts (folder, rate);
    if (parts.empty())
        return 1;
    for (auto& hit : DrumAnalyzer::extract (parts, rate).hits)
        std::cout << hit.position << " " << drumClassName (hit.drumClass) << "\n";
    return 0;
}

// --compare a.wav b.wav [dB]: the difference between two renderings of the same stem, relative to
// the first (RMS, dB). Fails when it's louder than the limit.
int compareFiles (const juce::File& a, const juce::File& b, float limitDb)
{
    double rateA = 0.0, rateB = 0.0;
    const auto x = readFile (a, rateA), y = readFile (b, rateB);
    if (x.getNumSamples() == 0 || x.getNumSamples() != y.getNumSamples() || x.getNumChannels() != y.getNumChannels())
    {
        std::cerr << "Can't compare: missing files or different lengths\n";
        return 1;
    }
    double signal = 0.0, difference = 0.0;
    for (int ch = 0; ch < x.getNumChannels(); ++ch)
        for (int i = 0; i < x.getNumSamples(); ++i)
        {
            const double d = x.getSample (ch, i) - y.getSample (ch, i);
            signal += (double) x.getSample (ch, i) * x.getSample (ch, i);
            difference += d * d;
        }
    const auto db = (float) (10.0 * std::log10 ((difference + 1e-30) / (signal + 1e-30)));
    const bool ok = db <= limitDb;
    std::cout << "Difference " << juce::String (db, 1) << " dB (limit " << juce::String (limitDb, 1) << " dB): "
              << (ok ? "PASS" : "FAIL") << "\n";
    return ok ? 0 : 1;
}

// The synthetic beat with a bass line and chords on top (or only the bass and chords).
std::vector<float> makeMix (bool withDrums)
{
    const auto beat = makeBeat();
    std::vector<float> mix (beat.parts[0].size(), 0.0f);
    if (withDrums)
        for (auto& part : beat.parts)
            for (size_t i = 0; i < mix.size(); ++i)
                mix[i] += part[i];
    const int step = (int) (sr * 60.0 / 120.0 / 4.0);
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
    return mix;
}

// --write-mix beat.wav [seconds]: the synthetic beat for the AI tests (only the first seconds when given).
int writeMix (const juce::File& file, double seconds)
{
    auto mix = makeMix (true);
    if (seconds > 0.0)
        mix.resize (std::min (mix.size(), (size_t) (seconds * sr)));
    writeWav (toStereo (mix), sr, file);
    return 0;
}

// --sections song: the sections the AI would process.
int listSections (const juce::File& file)
{
    double rate = 0.0;
    const auto audio = readFile (file, rate);
    if (audio.getNumSamples() == 0)
        return 1;
    printSections (SongSections::pick (audio, rate), audio.getNumSamples() / rate);
    return 0;
}

// A 3-minute song: 30 s without drums, 60 s with, 30 s without, 60 s with. The sections must all
// have drums in them, from both stretches with drums, and map back to the right place in the song.
bool sectionTest()
{
    const auto drums = makeMix (true), music = makeMix (false);
    std::vector<float> song;
    std::vector<juce::Range<double>> withDrums;
    auto append = [&] (const std::vector<float>& source, double seconds)
    {
        const auto start = song.size();
        for (size_t i = 0; i < (size_t) (seconds * sr); ++i)
            song.push_back (source[i % source.size()]);
        return juce::Range<double> (start / sr, song.size() / sr);
    };
    append (music, 30.0);
    withDrums.push_back (append (drums, 60.0));
    append (music, 30.0);
    withDrums.push_back (append (drums, 60.0));

    const auto audio = toStereo (song);
    const auto sections = SongSections::pick (audio, sr);
    std::cout << "Section picker on a song with drums at 30-90 s and 120-180 s:\n";
    printSections (sections, song.size() / sr);

    bool ok = ! sections.sections.empty();
    std::array<bool, 2> covered {};
    for (auto& r : sections.seconds())
    {
        double inDrums = 0.0;
        for (size_t k = 0; k < withDrums.size(); ++k)
            if (const auto overlap = r.getIntersectionWith (withDrums[k]).getLength(); overlap > 0.0)
            {
                inDrums += overlap;
                covered[k] = true;
            }
        if (inDrums < 0.5 * r.getLength())
            ok = false;
    }
    ok = ok && covered[0] && covered[1];

    // The joined audio maps back onto the song.
    const auto ranges = sections.joinedRanges (DrumSeparator::modelSampleRate);
    for (size_t k = 0; k < ranges.size(); ++k)
    {
        const int expected = (int) std::lround (sections.sections[k].getStart() * DrumSeparator::modelSampleRate / sr);
        if (std::abs (sections.toSong (ranges[k].getStart() + 1000, DrumSeparator::modelSampleRate) - (expected + 1000)) > 1)
            ok = false;
    }
    std::cout << (ok ? "Sections OK\n" : "  FAIL: sections without drums, or a stretch with drums missed\n");
    return ok;
}

//==============================================================================
// The synthetic test: five clean parts, so the right answer is known exactly.
int syntheticTest (const juce::File& out)
{
    auto beat = makeBeat();
    // A faint copy of the snare in the kick part (-48 dB), like the bleed the AI split leaves behind.
    const auto snare = makeSnare();
    for (int at : beat.hits[(size_t) DrumClass::snare])
        add (beat.parts[(size_t) DrumClass::kick], snare, at, 0.004f);
    std::vector<juce::AudioBuffer<float>> parts;
    for (auto& p : beat.parts)
        parts.push_back (toStereo (p));
    const auto result = extractTimed (parts, sr);
    bool ok = true;
    auto fail = [&ok] (const juce::String& why) { std::cout << "  FAIL: " << why << "\n"; ok = false; };

    // 1. Every hit of every part is found, at the right place, and nothing else (not the bleed).
    for (int c = 0; c < numDrumClasses; ++c)
    {
        std::vector<int> found;
        for (auto& h : result.hits)
            if ((int) h.drumClass == c)
                found.push_back (h.position);
        const auto& truth = beat.hits[(size_t) c];
        int matched = 0;
        for (int t : truth)
            for (int f : found)
                if (std::abs (f - t) <= (int) (0.003 * sr))
                {
                    ++matched;
                    break;
                }
        std::cout << drumClassName ((DrumClass) c) << ": " << matched << "/" << truth.size() << " hits found, "
                  << found.size() << " reported\n";
        if (matched != (int) truth.size() || found.size() != truth.size())
        {
            fail ("wrong hits");
            for (int f : found)
                std::cout << " " << f;
            std::cout << "\n";
        }
    }

    // 2. Each pad's sound is its own drum.
    struct Expected { DrumClass pad; std::vector<float> sound; const char* name; };
    const Expected expected[] { { DrumClass::kick, makeKick(), "kick" }, { DrumClass::kick, makeShortKick(), "short kick" },
                                { DrumClass::snare, makeSnare(), "snare" }, { DrumClass::toms, makeTom (true), "high tom" },
                                { DrumClass::toms, makeTom (false), "low tom" }, { DrumClass::hihat, makeHat(), "hat" },
                                { DrumClass::cymbals, makeCrash(), "crash" } };
    for (auto& e : expected)
    {
        float best = 0.0f;
        for (auto& s : result.kit[(size_t) e.pad])
            best = std::max (best, bestCorrelation (e.sound, s.audio));
        std::cout << "Similarity " << e.name << ": " << juce::String (best, 4) << "\n";
        if (best < 0.99f)
            fail (juce::String (e.name) + " not on its pad");
    }
    if (result.kit[(size_t) DrumClass::kick].size() != 2 || result.kit[(size_t) DrumClass::toms].size() != 2)
        fail ("expected two kicks and two toms");

    // 3. Every sound is a hit of its part, unchanged: the same samples apart from the 2 ms fade-in and
    // the 5 ms fade-out.
    const int fadeIn = (int) (0.002 * sr), fadeOut = (int) (0.005 * sr);
    for (int c = 0; c < numDrumClasses; ++c)
        for (auto& s : result.kit[(size_t) c])
        {
            const int n = s.audio.getNumSamples();
            float bestDiff = 1.0f;
            for (int at : beat.hits[(size_t) c])
                for (int start = std::max (0, at - fadeIn - 132); start <= std::max (0, at - fadeIn + 132); ++start)
                {
                    if (start < 0 || start + n > parts[(size_t) c].getNumSamples())
                        continue;
                    float diff = 0.0f;
                    for (int ch = 0; ch < 2 && diff < bestDiff; ++ch)
                        for (int i = fadeIn; i < n - fadeOut && diff < bestDiff; ++i)
                            diff = std::max (diff, std::abs (s.audio.getSample (ch, i) - parts[(size_t) c].getSample (ch, start + i)));
                    bestDiff = std::min (bestDiff, diff);
                }
            if (bestDiff > 1e-6f)
                fail (juce::String (drumClassName ((DrumClass) c)) + " sound was changed (" + juce::String (bestDiff) + ")");
        }

    if (out != juce::File())
        writeKit (result, out);
    ok = sectionTest() && ok;
    std::cout << (ok ? "PASS" : "FAIL") << "\n";
    return ok ? 0 : 1;
}
} // namespace

int main (int argc, char** argv)
{
    const juce::String mode = argc >= 2 ? argv[1] : "";
    if (mode == "--write-mix" && argc >= 3)  return writeMix (arg (argv[2]), argc >= 4 ? juce::String (argv[3]).getDoubleValue() : 0.0);
    if (mode == "--separate" && argc >= 4)   return separateFile (arg (argv[2]), arg (argv[3]));
    if (mode == "--split" && argc >= 4)      return splitFile (arg (argv[2]), arg (argv[3]));
    if (mode == "--file-ai" && argc >= 3)    return extractWithAI (arg (argv[2]), argc >= 4 ? arg (argv[3]) : juce::File(), false);
    if (mode == "--file-ai-full" && argc >= 3) return extractWithAI (arg (argv[2]), argc >= 4 ? arg (argv[3]) : juce::File(), true);
    if (mode == "--sections" && argc >= 3)   return listSections (arg (argv[2]));
    if (mode == "--file-parts" && argc >= 3) return extractParts (arg (argv[2]), argc >= 4 ? arg (argv[3]) : juce::File());
    if (mode == "--hits" && argc >= 3)       return listHits (arg (argv[2]));
    if (mode == "--compare" && argc >= 4)    return compareFiles (arg (argv[2]), arg (argv[3]), argc >= 5 ? juce::String (argv[4]).getFloatValue() : -40.0f);
    if (mode.startsWith ("--"))
    {
        std::cerr << "Unknown or incomplete option " << mode << "\n";
        return 1;
    }
    return syntheticTest (argc >= 2 ? arg (argv[1]) : juce::File());
}
