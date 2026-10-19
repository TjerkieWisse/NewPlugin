#include "PluginProcessor.h"
#include "PluginEditor.h"
#include "analysis/SongSections.h"
#include "separation/DrumSeparator.h"

using namespace kitnap;

namespace
{
constexpr double maxSourceSeconds = 12.0 * 60.0;
constexpr int overviewBuckets = 2000;
constexpr int kitFormat = 2; // 2: the five pads of the AI drum split

juce::MemoryBlock encodeWav (const DrumSample& sample)
{
    juce::MemoryBlock block;
    juce::WavAudioFormat wav;
    auto stream = std::make_unique<juce::MemoryOutputStream> (block, false);
    if (auto writer = std::unique_ptr<juce::AudioFormatWriter> (
            wav.createWriterFor (stream.get(), sample.sampleRate, (unsigned int) sample.audio.getNumChannels(), 24, {}, 0)))
    {
        stream.release(); // the writer owns it now
        writer->writeFromAudioSampleBuffer (sample.audio, 0, sample.audio.getNumSamples());
    }
    return block;
}

// Sounds are stored in the project as FLAC: lossless and about half the size of WAV.
juce::MemoryBlock encodeFlac (const DrumSample& sample)
{
    juce::MemoryBlock block;
    juce::FlacAudioFormat flac;
    auto stream = std::make_unique<juce::MemoryOutputStream> (block, false);
    if (auto writer = std::unique_ptr<juce::AudioFormatWriter> (
            flac.createWriterFor (stream.get(), sample.sampleRate, (unsigned int) sample.audio.getNumChannels(), 24, {}, 0)))
    {
        stream.release();
        writer->writeFromAudioSampleBuffer (sample.audio, 0, sample.audio.getNumSamples());
    }
    return block;
}

SamplePtr decodeFlac (const juce::MemoryBlock& block, int hitsStacked)
{
    juce::FlacAudioFormat flac;
    std::unique_ptr<juce::AudioFormatReader> reader (
        flac.createReaderFor (new juce::MemoryInputStream (block, false), true));
    if (reader == nullptr || reader->lengthInSamples <= 0)
        return nullptr;

    auto sample = std::make_shared<DrumSample>();
    sample->sampleRate = reader->sampleRate;
    sample->hitsStacked = hitsStacked;
    sample->audio.setSize ((int) reader->numChannels, (int) reader->lengthInSamples);
    reader->read (&sample->audio, 0, sample->audio.getNumSamples(), 0, true, true);
    return sample;
}

// The hit map, packed as fixed-size records.
struct PackedHit
{
    juce::int32 position;
    juce::int8 drumClass, reserved;
    juce::int16 strength; // 0..32767
};

juce::String titleCase (const juce::String& s)
{
    juce::StringArray words;
    words.addTokens (s.toLowerCase(), " ", {});
    for (auto& w : words)
        w = w.substring (0, 1).toUpperCase() + w.substring (1);
    return words.joinIntoString (" ");
}
} // namespace

//==============================================================================
struct KitnapAudioProcessor::AnalysisOutput
{
    juce::File file;
    bool ok = false;
    juce::String error;
    double sampleRate = 44100.0, lengthSeconds = 0.0;
    std::vector<float> overview;
    std::vector<juce::Range<double>> scanned;
    AnalysisResult result;
    std::array<int, numDrumClasses> selection {};
    double analysisSampleRate = 44100.0;
};

// Reads the dropped file and runs both AI models and the kit extraction on a background thread.
class KitnapAudioProcessor::AnalysisJob : public juce::Thread
{
public:
    AnalysisJob (juce::AudioFormatManager& fm, juce::File f, std::atomic<float>& p, std::atomic<int>& st,
                 std::array<int, numDrumClasses> selection)
        : Thread ("KITNAP analysis"), formats (fm), progress (p), stage (st)
    {
        output = std::make_unique<AnalysisOutput>();
        output->file = std::move (f);
        output->selection = selection;
    }

    void run() override
    {
        std::unique_ptr<juce::AudioFormatReader> reader (formats.createReaderFor (output->file));
        if (reader == nullptr || reader->lengthInSamples <= 0 || reader->sampleRate <= 0.0)
        {
            output->error = "Can't read this file. Try WAV, AIFF, FLAC, MP3 or OGG.";
            return;
        }

        const auto length = (int) std::min<juce::int64> (reader->lengthInSamples,
                                                          (juce::int64) (maxSourceSeconds * reader->sampleRate));
        juce::AudioBuffer<float> audio ((int) std::min (2u, reader->numChannels), length);
        reader->read (&audio, 0, length, 0, true, audio.getNumChannels() > 1);
        if (threadShouldExit())
            return;

        output->sampleRate = reader->sampleRate;
        output->lengthSeconds = length / reader->sampleRate;

        // Waveform overview for the editor.
        output->overview.resize (overviewBuckets * 2);
        for (int b = 0; b < overviewBuckets; ++b)
        {
            const int start = (int) ((juce::int64) b * length / overviewBuckets);
            const int end = std::max (start + 1, (int) ((juce::int64) (b + 1) * length / overviewBuckets));
            float lo = 0.0f, hi = 0.0f;
            for (int ch = 0; ch < audio.getNumChannels(); ++ch)
            {
                auto range = juce::FloatVectorOperations::findMinAndMax (audio.getReadPointer (ch, start), std::min (end, length) - start);
                lo = std::min (lo, range.getStart());
                hi = std::max (hi, range.getEnd());
            }
            output->overview[(size_t) b * 2] = lo;
            output->overview[(size_t) b * 2 + 1] = hi;
        }
        overviewReady.store (true); // the editor can draw the waveform while the analysis runs

        // Only the parts of the song with the most drum hits go through the AI (at most a minute).
        const auto sections = SongSections::pick (audio, reader->sampleRate);
        output->scanned = sections.seconds();
        sectionsReady.store (true);
        const auto joined = sections.join (audio);

        // 1. The first model pulls the drums out of the mix. 2. The second splits them into kick, snare,
        // toms, hi-hat and cymbals. 3. Each pad gets single hits cut straight out of its own part.
        // The two AI steps take nearly all of the time, so they get nearly all of the progress bar.
        constexpr float separateEnd = 0.6f, splitEnd = 0.98f;
        auto cancel = [this] { return threadShouldExit(); };

        stage.store ((int) KitState::Stage::separating);
        juce::String error;
        const auto drums = DrumSeparator::separate (joined, reader->sampleRate,
                                                    [this] (float p) { progress.store (separateEnd * p); },
                                                    cancel, error);
        if (threadShouldExit())
            return;
        if (drums.getNumSamples() == 0)
        {
            output->error = "AI drum separation failed: " + error;
            return;
        }

        stage.store ((int) KitState::Stage::splitting);
        const auto parts = DrumSeparator::split (drums, { DrumSeparator::DrumPart::kick, DrumSeparator::DrumPart::snare,
                                                          DrumSeparator::DrumPart::toms, DrumSeparator::DrumPart::hihat,
                                                          DrumSeparator::DrumPart::cymbals },
                                                 [this] (float p) { progress.store (separateEnd + (splitEnd - separateEnd) * p); },
                                                 cancel, error);
        if (threadShouldExit())
            return;
        if ((int) parts.size() != numDrumClasses)
        {
            output->error = "AI drum split failed: " + error;
            return;
        }

        stage.store ((int) KitState::Stage::analysing);
        output->analysisSampleRate = DrumSeparator::modelSampleRate;
        output->result = DrumAnalyzer::extract (parts, DrumSeparator::modelSampleRate,
                                                [this] (float p) { progress.store (splitEnd + (1.0f - splitEnd) * p); },
                                                cancel, sections.joinedRanges (DrumSeparator::modelSampleRate));
        for (auto& hit : output->result.hits)
            hit.position = sections.toSong (hit.position, DrumSeparator::modelSampleRate);
        output->ok = ! threadShouldExit();
    }

    std::unique_ptr<AnalysisOutput> output;
    std::atomic<bool> overviewReady { false }, sectionsReady { false };

private:
    juce::AudioFormatManager& formats;
    std::atomic<float>& progress;
    std::atomic<int>& stage;
};

//==============================================================================
KitnapAudioProcessor::KitnapAudioProcessor()
    : AudioProcessor (BusesProperties().withOutput ("Output", juce::AudioChannelSet::stereo(), true)),
      apvts (*this, nullptr, "PARAMS", createLayout())
{
    formatManager.registerBasicFormats();
    volumeParam = apvts.getRawParameterValue ("volume");
    startTimerHz (20);
}

KitnapAudioProcessor::~KitnapAudioProcessor()
{
    stopTimer();
    if (job != nullptr)
        job->stopThread (4000);
}

juce::AudioProcessorValueTreeState::ParameterLayout KitnapAudioProcessor::createLayout()
{
    using namespace juce;
    std::vector<std::unique_ptr<RangedAudioParameter>> params;
    params.push_back (std::make_unique<AudioParameterFloat> (
        ParameterID { "volume", 1 }, "Volume", NormalisableRange<float> (-30.0f, 6.0f), -6.0f,
        AudioParameterFloatAttributes().withLabel ("dB").withStringFromValueFunction (
            [] (float v, int) { return (v > 0.0f ? "+" : "") + String (v, 1) + " dB"; })));
    return { params.begin(), params.end() };
}

void KitnapAudioProcessor::prepareToPlay (double sampleRate, int)
{
    sampler.prepare (sampleRate);
    volume.reset (sampleRate, 0.05);
    volume.setCurrentAndTargetValue (juce::Decibels::decibelsToGain (volumeParam->load()));
}

bool KitnapAudioProcessor::isBusesLayoutSupported (const BusesLayout& layouts) const
{
    const auto out = layouts.getMainOutputChannelSet();
    return out == juce::AudioChannelSet::stereo() || out == juce::AudioChannelSet::mono();
}

void KitnapAudioProcessor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midi)
{
    juce::ScopedNoDenormals noDenormals;
    sampler.render (buffer, midi);

    volume.setTargetValue (juce::Decibels::decibelsToGain (volumeParam->load()));
    volume.applyGain (buffer, buffer.getNumSamples());
}

//==============================================================================
bool KitnapAudioProcessor::canLoad (const juce::File& file) const
{
    return file.existsAsFile()
        && const_cast<juce::AudioFormatManager&> (formatManager).findFormatForFileExtension (file.getFileExtension()) != nullptr;
}

void KitnapAudioProcessor::loadFile (const juce::File& file)
{
    startAnalysis (file, {});
}

void KitnapAudioProcessor::startAnalysis (const juce::File& file, std::array<int, numDrumClasses> selection)
{
    if (job != nullptr)
        job->stopThread (4000);

    progress.store (0.0f);
    stage.store ((int) KitState::Stage::reading);
    kit.status = KitState::Status::analysing;
    kit.sourceName = file.getFileNameWithoutExtension();
    kit.sourceFile = file;
    kit.message = {};
    kit.hits.clear();
    kit.overview.clear();
    kit.scanned.clear();

    if (! DrumSeparator::isAvailable())
    {
        kit.status = KitState::Status::error;
        kit.message = "The AI models aren't installed (" + DrumSeparator::unavailableReason()
                    + "). Run the KITNAP installer, then load the track again.";
        sendChangeMessage();
        return;
    }

    job = std::make_unique<AnalysisJob> (formatManager, file, progress, stage, selection);
    job->startThread();
    sendChangeMessage();
}

void KitnapAudioProcessor::timerCallback()
{
    // Show the waveform as soon as the file is read. The job doesn't touch it after setting the flag.
    if (job != nullptr && job->overviewReady.load() && kit.overview.empty() && job->output != nullptr)
    {
        kit.overview = job->output->overview;
        kit.sourceLengthSeconds = job->output->lengthSeconds;
        sendChangeMessage();
    }
    if (job != nullptr && job->sectionsReady.load() && kit.scanned.empty() && job->output != nullptr)
    {
        kit.scanned = job->output->scanned;
        sendChangeMessage();
    }

    if (job != nullptr && ! job->isThreadRunning())
    {
        auto output = std::move (job->output);
        job.reset();
        if (output != nullptr)
            applyAnalysis (std::move (output));
    }

    retired.erase (std::remove_if (retired.begin(), retired.end(), [] (auto& s) { return s.use_count() <= 1; }),
                   retired.end());
}

void KitnapAudioProcessor::retire (const SamplePtr& sample)
{
    if (sample != nullptr)
        retired.push_back (sample);
}

void KitnapAudioProcessor::applyAnalysis (std::unique_ptr<AnalysisOutput> output)
{
    if (! output->ok)
    {
        kit.status = KitState::Status::error;
        kit.message = output->error;
        sendChangeMessage();
        return;
    }

    kit.hits = std::move (output->result.hits);
    kit.hitSampleRate = output->analysisSampleRate;
    kit.overview = std::move (output->overview);
    kit.sourceLengthSeconds = output->lengthSeconds;
    kit.scanned = std::move (output->scanned);

    int found = 0;
    for (int pad = 0; pad < numDrumClasses; ++pad)
    {
        auto& fresh = output->result.kit[(size_t) pad];
        for (auto& old : kit.variations[(size_t) pad])
            retire (old);
        kit.variations[(size_t) pad].clear();
        for (auto& s : fresh)
            kit.variations[(size_t) pad].push_back (std::make_shared<const DrumSample> (std::move (s)));

        const int count = (int) kit.variations[(size_t) pad].size();
        kit.selected[(size_t) pad] = count > 0 ? juce::jlimit (0, count - 1, output->selection[(size_t) pad]) : 0;
        found += count > 0 ? 1 : 0;
    }

    applySelectionToSampler();
    kit.status = KitState::Status::ready;
    kit.message = found == 0 ? juce::String ("No drums found. Try a track with clear drums.")
                             : juce::String (kit.hits.size()) + " hits found";
    sendChangeMessage();
}

void KitnapAudioProcessor::applySelectionToSampler()
{
    for (int pad = 0; pad < numDrumClasses; ++pad)
    {
        auto& vars = kit.variations[(size_t) pad];
        const int sel = kit.selected[(size_t) pad];
        sampler.setPadSample (pad, juce::isPositiveAndBelow (sel, (int) vars.size()) ? vars[(size_t) sel] : nullptr);
    }
}

bool KitnapAudioProcessor::isAIAvailable() const
{
    return DrumSeparator::isAvailable();
}

juce::String KitnapAudioProcessor::aiUnavailableReason() const
{
    return DrumSeparator::unavailableReason();
}

void KitnapAudioProcessor::selectVariation (int pad, int index)
{
    auto& vars = kit.variations[(size_t) pad];
    if (vars.empty())
        return;
    kit.selected[(size_t) pad] = (index % (int) vars.size() + (int) vars.size()) % (int) vars.size();
    applySelectionToSampler();
    sendChangeMessage();
}

//==============================================================================
juce::String KitnapAudioProcessor::padFileName (int pad) const
{
    const auto base = kit.sourceName.isNotEmpty() ? kit.sourceName : juce::String ("KITNAP");
    return juce::File::createLegalFileName (base + " " + titleCase (drumClassName ((DrumClass) pad)) + ".wav");
}

juce::File KitnapAudioProcessor::writePadToFile (int pad, const juce::File& folder) const
{
    auto& vars = kit.variations[(size_t) pad];
    const int sel = kit.selected[(size_t) pad];
    if (! juce::isPositiveAndBelow (sel, (int) vars.size()))
        return {};

    folder.createDirectory();
    auto file = folder.getChildFile (padFileName (pad));
    const auto block = encodeWav (*vars[(size_t) sel]);
    return block.getSize() > 0 && file.replaceWithData (block.getData(), block.getSize()) ? file : juce::File();
}

juce::Array<juce::File> KitnapAudioProcessor::exportKit (const juce::File& parentFolder) const
{
    auto folder = parentFolder.getChildFile (juce::File::createLegalFileName (
        (kit.sourceName.isNotEmpty() ? kit.sourceName : juce::String ("KITNAP")) + " Kit"));
    if (folder.exists())
        folder = folder.getNonexistentSibling();

    juce::Array<juce::File> written;
    for (int pad = 0; pad < numDrumClasses; ++pad)
        if (auto f = writePadToFile (pad, folder); f.existsAsFile())
            written.add (f);
    return written;
}

//==============================================================================
void KitnapAudioProcessor::getStateInformation (juce::MemoryBlock& destData)
{
    juce::ValueTree root ("KITNAP");
    root.appendChild (apvts.copyState(), nullptr);

    // The whole analysis is stored in the project: every variation, the hit map and the waveform.
    // Reopening a session is instant and never needs the source file or the AI again.
    juce::ValueTree kitTree ("Kit");
    kitTree.setProperty ("version", kitFormat, nullptr);
    kitTree.setProperty ("source", kit.sourceFile.getFullPathName(), nullptr);
    kitTree.setProperty ("name", kit.sourceName, nullptr);
    kitTree.setProperty ("message", kit.message, nullptr);
    kitTree.setProperty ("hitRate", kit.hitSampleRate, nullptr);
    kitTree.setProperty ("length", kit.sourceLengthSeconds, nullptr);
    juce::StringArray scanned;
    for (auto& r : kit.scanned)
        scanned.add (juce::String (r.getStart(), 3) + " " + juce::String (r.getEnd(), 3));
    kitTree.setProperty ("scanned", scanned.joinIntoString (" "), nullptr);

    if (! kit.overview.empty())
        kitTree.setProperty ("overview", juce::MemoryBlock (kit.overview.data(), kit.overview.size() * sizeof (float)).toBase64Encoding(), nullptr);

    if (! kit.hits.empty())
    {
        std::vector<PackedHit> packed;
        for (auto& h : kit.hits)
            packed.push_back ({ (juce::int32) h.position, (juce::int8) h.drumClass, 0,
                                (juce::int16) juce::jlimit (0, 32767, juce::roundToInt (h.strength * 32767.0f)) });
        kitTree.setProperty ("hits", juce::MemoryBlock (packed.data(), packed.size() * sizeof (PackedHit)).toBase64Encoding(), nullptr);
    }

    for (int pad = 0; pad < numDrumClasses; ++pad)
    {
        juce::ValueTree padTree ("Pad");
        padTree.setProperty ("index", pad, nullptr);
        padTree.setProperty ("selected", kit.selected[(size_t) pad], nullptr);
        for (auto& sample : kit.variations[(size_t) pad])
        {
            juce::ValueTree sound ("Sound");
            sound.setProperty ("stacked", sample->hitsStacked, nullptr);
            sound.setProperty ("flac", encodeFlac (*sample).toBase64Encoding(), nullptr);
            padTree.appendChild (sound, nullptr);
        }
        kitTree.appendChild (padTree, nullptr);
    }
    root.appendChild (kitTree, nullptr);

    if (auto xml = root.createXml())
        copyXmlToBinary (*xml, destData);
}

void KitnapAudioProcessor::setStateInformation (const void* data, int sizeInBytes)
{
    auto xml = getXmlFromBinary (data, sizeInBytes);
    if (xml == nullptr || ! xml->hasTagName ("KITNAP"))
        return;

    const auto root = juce::ValueTree::fromXml (*xml);
    if (auto params = root.getChildWithName (apvts.state.getType()); params.isValid())
        apvts.replaceState (params);

    // Kits from before the five pads (kick, snare, toms, hi-hat, cymbals) don't fit and aren't restored.
    auto kitTree = root.getChildWithName ("Kit");
    if ((int) kitTree.getProperty ("version", 1) != kitFormat)
        kitTree = {};

    auto restore = [this, kitTree]
    {
        if (! kitTree.isValid())
        {
            sendChangeMessage();
            return;
        }

        if (job != nullptr)
            job->stopThread (4000);
        job.reset();

        bool anySound = false;
        for (const auto& padTree : kitTree)
        {
            const int pad = padTree.getProperty ("index", -1);
            if (! juce::isPositiveAndBelow (pad, numDrumClasses))
                continue;

            for (auto& old : kit.variations[(size_t) pad])
                retire (old);
            kit.variations[(size_t) pad].clear();

            for (const auto& sound : padTree)
            {
                juce::MemoryBlock block;
                if (block.fromBase64Encoding (sound.getProperty ("flac").toString()))
                    if (auto sample = decodeFlac (block, sound.getProperty ("stacked", 1)))
                        kit.variations[(size_t) pad].push_back (sample);
            }

            const int count = (int) kit.variations[(size_t) pad].size();
            kit.selected[(size_t) pad] = count > 0 ? juce::jlimit (0, count - 1, (int) padTree.getProperty ("selected", 0)) : 0;
            anySound = anySound || count > 0;
        }
        applySelectionToSampler();

        kit.sourceName = kitTree.getProperty ("name").toString();
        kit.sourceFile = juce::File (kitTree.getProperty ("source").toString());
        kit.message = kitTree.getProperty ("message").toString();
        kit.hitSampleRate = kitTree.getProperty ("hitRate", 44100.0);
        kit.sourceLengthSeconds = kitTree.getProperty ("length", 0.0);
        kit.scanned.clear();
        const auto scanned = juce::StringArray::fromTokens (kitTree.getProperty ("scanned").toString(), true);
        for (int i = 0; i + 1 < scanned.size(); i += 2)
            kit.scanned.push_back ({ scanned[i].getDoubleValue(), scanned[i + 1].getDoubleValue() });

        kit.overview.clear();
        juce::MemoryBlock block;
        if (block.fromBase64Encoding (kitTree.getProperty ("overview").toString()) && block.getSize() % sizeof (float) == 0)
        {
            kit.overview.resize (block.getSize() / sizeof (float));
            block.copyTo (kit.overview.data(), 0, block.getSize());
        }

        kit.hits.clear();
        if (block.fromBase64Encoding (kitTree.getProperty ("hits").toString()) && block.getSize() % sizeof (PackedHit) == 0)
        {
            std::vector<PackedHit> packed (block.getSize() / sizeof (PackedHit));
            block.copyTo (packed.data(), 0, block.getSize());
            for (auto& p : packed)
                if (juce::isPositiveAndBelow ((int) p.drumClass, numDrumClasses))
                    kit.hits.push_back ({ p.position, (DrumClass) p.drumClass, (float) p.strength / 32767.0f });
        }

        kit.status = anySound ? KitState::Status::ready : KitState::Status::empty;
        sendChangeMessage();
    };

    if (juce::MessageManager::getInstance()->isThisTheMessageThread())
        restore();
    else
        juce::MessageManager::callAsync ([safeThis = juce::WeakReference<KitnapAudioProcessor> (this), restore]
        {
            if (safeThis != nullptr)
                restore();
        });
}

juce::AudioProcessorEditor* KitnapAudioProcessor::createEditor()
{
    return new KitnapAudioProcessorEditor (*this);
}

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new KitnapAudioProcessor();
}
