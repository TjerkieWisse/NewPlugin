#include "PluginProcessor.h"
#include "PluginEditor.h"

using namespace kitnap;

namespace
{
constexpr double maxSourceSeconds = 12.0 * 60.0;
constexpr int overviewBuckets = 2000;

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

SamplePtr decodeWav (const juce::MemoryBlock& block, int hitsStacked)
{
    juce::WavAudioFormat wav;
    std::unique_ptr<juce::AudioFormatReader> reader (
        wav.createReaderFor (new juce::MemoryInputStream (block, false), true));
    if (reader == nullptr || reader->lengthInSamples <= 0)
        return nullptr;

    auto sample = std::make_shared<DrumSample>();
    sample->sampleRate = reader->sampleRate;
    sample->hitsStacked = hitsStacked;
    sample->audio.setSize ((int) reader->numChannels, (int) reader->lengthInSamples);
    reader->read (&sample->audio, 0, sample->audio.getNumSamples(), 0, true, true);
    return sample;
}

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
    AnalysisResult result;
    std::array<int, numDrumClasses> selection {};
    bool keepCurrentPads = false;
};

// Reads the dropped file and runs the analyzer on a background thread.
class KitnapAudioProcessor::AnalysisJob : public juce::Thread
{
public:
    AnalysisJob (juce::AudioFormatManager& fm, juce::File f, std::atomic<float>& p,
                 std::array<int, numDrumClasses> selection, bool keepCurrentPads)
        : Thread ("KITNAP analysis"), formats (fm), progress (p)
    {
        output = std::make_unique<AnalysisOutput>();
        output->file = std::move (f);
        output->selection = selection;
        output->keepCurrentPads = keepCurrentPads;
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

        output->result = DrumAnalyzer::analyse (audio, reader->sampleRate,
                                                [this] (float p) { progress.store (p); },
                                                [this] { return threadShouldExit(); });
        output->ok = ! threadShouldExit();
    }

    std::unique_ptr<AnalysisOutput> output;
    std::atomic<bool> overviewReady { false };

private:
    juce::AudioFormatManager& formats;
    std::atomic<float>& progress;
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
    startAnalysis (file, {}, false);
}

void KitnapAudioProcessor::startAnalysis (const juce::File& file, std::array<int, numDrumClasses> selection, bool keepCurrentPads)
{
    if (job != nullptr)
        job->stopThread (4000);

    progress.store (0.0f);
    kit.status = KitState::Status::analysing;
    kit.sourceName = file.getFileNameWithoutExtension();
    kit.sourceFile = file;
    kit.message = {};
    if (! keepCurrentPads)
    {
        kit.hits.clear();
        kit.overview.clear();
    }

    job = std::make_unique<AnalysisJob> (formatManager, file, progress, selection, keepCurrentPads);
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
        kit.status = output->keepCurrentPads ? KitState::Status::ready : KitState::Status::error;
        kit.message = output->keepCurrentPads ? juce::String ("Source file couldn't be re-read, kit restored as saved.")
                                              : output->error;
        sendChangeMessage();
        return;
    }

    kit.hits = std::move (output->result.hits);
    kit.hitSampleRate = output->sampleRate;
    kit.overview = std::move (output->overview);
    kit.sourceLengthSeconds = output->lengthSeconds;

    int found = 0;
    for (int pad = 0; pad < numDrumClasses; ++pad)
    {
        auto& fresh = output->result.kit[(size_t) pad];
        if (fresh.empty() && output->keepCurrentPads)
            continue; // keep the sound restored from the saved project

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
    kit.message = found == 0 ? juce::String ("No drums found. Try a drum loop or a track with clear drums.")
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

    // The chosen sounds are stored in the project, so it still plays if the source file is gone.
    juce::ValueTree kitTree ("Kit");
    kitTree.setProperty ("source", kit.sourceFile.getFullPathName(), nullptr);
    kitTree.setProperty ("name", kit.sourceName, nullptr);
    for (int pad = 0; pad < numDrumClasses; ++pad)
    {
        juce::ValueTree padTree ("Pad");
        padTree.setProperty ("index", pad, nullptr);
        padTree.setProperty ("selected", kit.selected[(size_t) pad], nullptr);
        if (auto sample = sampler.getPadSample (pad))
        {
            padTree.setProperty ("stacked", sample->hitsStacked, nullptr);
            padTree.setProperty ("wav", encodeWav (*sample).toBase64Encoding(), nullptr);
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

    const auto kitTree = root.getChildWithName ("Kit");
    if (! kitTree.isValid())
        return;

    auto restore = [this, kitTree]
    {
        std::array<int, numDrumClasses> selection {};
        bool anySound = false;
        for (const auto& padTree : kitTree)
        {
            const int pad = padTree.getProperty ("index", -1);
            if (! juce::isPositiveAndBelow (pad, numDrumClasses))
                continue;
            selection[(size_t) pad] = padTree.getProperty ("selected", 0);

            for (auto& old : kit.variations[(size_t) pad])
                retire (old);
            kit.variations[(size_t) pad].clear();
            kit.selected[(size_t) pad] = 0;

            juce::MemoryBlock block;
            if (block.fromBase64Encoding (padTree.getProperty ("wav").toString()))
                if (auto sample = decodeWav (block, padTree.getProperty ("stacked", 1)))
                {
                    kit.variations[(size_t) pad].push_back (sample);
                    anySound = true;
                }
        }
        applySelectionToSampler();

        kit.sourceName = kitTree.getProperty ("name").toString();
        kit.sourceFile = juce::File (kitTree.getProperty ("source").toString());
        kit.status = anySound ? KitState::Status::ready : KitState::Status::empty;
        kit.hits.clear();
        kit.overview.clear();
        sendChangeMessage();

        // Re-analyse the original file in the background to bring back the waveform and all variations.
        if (anySound && kit.sourceFile.existsAsFile())
            startAnalysis (kit.sourceFile, selection, true);
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
