#include "DrumSeparator.h"
#include <juce_dsp/juce_dsp.h>

#if KITNAP_HAS_ONNX
 #include <onnxruntime_c_api.h>
#endif
#if JUCE_MAC
 #include <sys/sysctl.h>
#endif

namespace kitnap
{
namespace
{
// The two models: the drum stem out of a full mix, and the drum stem into its parts.
struct ModelSpec
{
    const char* fileName;
    int fftOrder, hop;
};
const ModelSpec drumsModel { "kitnap_drums_sw.onnx", 11, 512 }; // BS-RoFormer SW, drums only
const ModelSpec splitModel { "kitnap_drumsep5.onnx", 11, 512 }; // DrumSep 5 stems (MDX23C)

juce::Array<juce::File> searchFolders()
{
    const auto binaryFolder = juce::File::getSpecialLocation (juce::File::currentExecutableFile).getParentDirectory();
    const auto user = juce::File::getSpecialLocation (juce::File::userApplicationDataDirectory);
    const auto common = juce::File::getSpecialLocation (juce::File::commonApplicationDataDirectory);
    // Next to the binary (inside the plugin bundle), then the shared folder the installer fills
    // (C:\ProgramData\KITNAP, /Library/Application Support/KITNAP, ~/.config/KITNAP).
    return { binaryFolder.getChildFile ("Resources"),
             binaryFolder.getParentDirectory().getChildFile ("Resources"),
             binaryFolder,
             common.getChildFile ("KITNAP"),
             common.getChildFile ("Application Support/KITNAP"),
             user.getChildFile ("KITNAP"),
             user.getChildFile ("Application Support/KITNAP") };
}

juce::File findFile (const juce::String& pattern)
{
    for (auto& folder : searchFolders())
        if (folder.isDirectory())
            for (auto& f : folder.findChildFiles (juce::File::findFiles, false, pattern))
                return f;
    return {};
}

juce::File findModel (const ModelSpec& spec) { return findFile (spec.fileName); }

juce::File findRuntimeLibrary()
{
   #if JUCE_WINDOWS
    return findFile ("onnxruntime.dll");
   #elif JUCE_MAC
    return findFile ("libonnxruntime*.dylib");
   #else
    return findFile ("libonnxruntime.so*");
   #endif
}

#if KITNAP_HAS_ONNX
// Loads ONNX Runtime once per process and keeps it loaded (unloading it while a host runs is asking for trouble).
class Runtime
{
public:
    static Runtime& get()
    {
        static Runtime instance;
        return instance;
    }

    const OrtApi* api = nullptr;
    OrtEnv* env = nullptr;
    juce::String error;

private:
    Runtime()
    {
        const auto libraryFile = findRuntimeLibrary();
        if (! libraryFile.existsAsFile())
        {
            error = "ONNX Runtime not found";
            return;
        }
        if (! library.open (libraryFile.getFullPathName()))
        {
            error = "ONNX Runtime couldn't be loaded";
            return;
        }

        using GetApiBase = const OrtApiBase* (ORT_API_CALL*) ();
        auto getApiBase = (GetApiBase) library.getFunction ("OrtGetApiBase");
        if (getApiBase == nullptr || (api = getApiBase()->GetApi (ORT_API_VERSION)) == nullptr)
        {
            error = "ONNX Runtime version is too old";
            api = nullptr;
            return;
        }

        if (auto* status = api->CreateEnv (ORT_LOGGING_LEVEL_WARNING, "KITNAP", &env))
        {
            error = api->GetErrorMessage (status);
            api->ReleaseStatus (status);
            api = nullptr;
        }
    }

    juce::DynamicLibrary library;
};

// How many threads the models run on: one per performance core. Efficiency cores (Apple Silicon)
// and hyper-threads only slow it down, since every thread waits for the slowest one at each step.
// The number of threads doesn't change the result.
int inferenceThreads()
{
   #if JUCE_MAC
    int performanceCores = 0;
    size_t size = sizeof (performanceCores);
    if (sysctlbyname ("hw.perflevel0.physicalcpu", &performanceCores, &size, nullptr, 0) == 0 && performanceCores > 0)
        return performanceCores;
   #endif
    return std::max (1, juce::SystemStats::getNumPhysicalCpus());
}

// Turns an OrtStatus into an error string, releasing it.
bool failed (const OrtApi* api, OrtStatus* status, juce::String& error)
{
    if (status == nullptr)
        return false;
    error = api->GetErrorMessage (status);
    api->ReleaseStatus (status);
    return true;
}

//==============================================================================
// STFT matching torch.stft / torch.istft with center=True and a periodic Hann window,
// which is what MDX-Net and RoFormer models are trained with.
//
// Tensor layouts: MDX-Net style [4][dimF][dimT] (ch0 re, ch0 im, ch1 re, ch1 im), or with
// `complexLast` RoFormer style [2][dimF][dimT][2] (channel, bin, frame, re/im).
class Spectrogram
{
public:
    Spectrogram (int fftOrder, int hopSize, int numFreqBins, int numFrames, bool complexLastLayout = false)
        : fftSize (1 << fftOrder), hop (hopSize), complexLast (complexLastLayout),
          dimF (numFreqBins), dimT (numFrames), binsF ((size_t) numFreqBins), framesT ((size_t) numFrames), fft (fftOrder)
    {
        window.resize ((size_t) fftSize);
        for (int i = 0; i < fftSize; ++i)
            window[(size_t) i] = 0.5f - 0.5f * std::cos (juce::MathConstants<float>::twoPi * (float) i / (float) fftSize);
        scratch.resize ((size_t) fftSize * 2);
    }

    int chunkLength() const { return hop * (dimT - 1); }

    // Where the real part of (channel, bin, frame) lives in a tensor; the imaginary part is at + imagOffset().
    size_t index (int ch, size_t f, int t) const
    {
        return complexLast ? (((size_t) ch * binsF + f) * framesT + (size_t) t) * 2
                           : (size_t) (2 * ch) * binsF * framesT + f * framesT + (size_t) t;
    }
    size_t imagOffset() const { return complexLast ? 1 : binsF * framesT; }

    // audio: chunkLength() samples per channel -> tensor (see the layouts above).
    void forward (const float* const* audio, std::vector<float>& tensor)
    {
        const int length = chunkLength();
        const int half = fftSize / 2;
        tensor.assign (4 * binsF * framesT, 0.0f);

        for (int ch = 0; ch < 2; ++ch)
            for (int t = 0; t < dimT; ++t)
            {
                std::fill (scratch.begin(), scratch.end(), 0.0f);
                for (int i = 0; i < fftSize; ++i)
                {
                    // Reflect padding at both ends, like torch's center=True.
                    int idx = t * hop + i - half;
                    if (idx < 0) idx = -idx;
                    if (idx >= length) idx = 2 * (length - 1) - idx;
                    scratch[(size_t) i] = audio[ch][juce::jlimit (0, length - 1, idx)] * window[(size_t) i];
                }
                fft.performRealOnlyForwardTransform (scratch.data(), true);

                for (size_t f = 0; f < binsF; ++f)
                {
                    const auto i = index (ch, f, t);
                    tensor[i] = scratch[f * 2];
                    tensor[i + imagOffset()] = scratch[f * 2 + 1];
                }
            }
    }

    // tensor (see the layouts above) -> chunkLength() samples per channel (bins above dimF are zero).
    void inverse (const float* tensor, std::vector<float> (&audio)[2])
    {
        const int length = chunkLength();
        const int total = fftSize + hop * (dimT - 1);
        std::vector<float> sum ((size_t) total), norm ((size_t) total);

        for (int ch = 0; ch < 2; ++ch)
        {
            std::fill (sum.begin(), sum.end(), 0.0f);
            std::fill (norm.begin(), norm.end(), 0.0f);
            for (int t = 0; t < dimT; ++t)
            {
                std::fill (scratch.begin(), scratch.end(), 0.0f);
                for (size_t f = 0; f < binsF; ++f)
                {
                    const auto i = index (ch, f, t);
                    scratch[f * 2] = tensor[i];
                    scratch[f * 2 + 1] = tensor[i + imagOffset()];
                }
                fft.performRealOnlyInverseTransform (scratch.data());

                for (int i = 0; i < fftSize; ++i)
                {
                    sum[(size_t) (t * hop + i)] += scratch[(size_t) i] * window[(size_t) i];
                    norm[(size_t) (t * hop + i)] += window[(size_t) i] * window[(size_t) i];
                }
            }

            audio[ch].resize ((size_t) length);
            for (int i = 0; i < length; ++i)
            {
                const size_t j = (size_t) (i + fftSize / 2);
                audio[ch][(size_t) i] = norm[j] > 1e-8f ? sum[j] / norm[j] : 0.0f;
            }
        }
    }

    const int fftSize, hop;

private:
    bool complexLast;
    int dimF, dimT;
    size_t binsF, framesT; // the same, for indexing
    juce::dsp::FFT fft;
    std::vector<float> window, scratch;
};

// Everything for one separation run; releases all ONNX objects on destruction.
struct Session
{
    explicit Session (const OrtApi* a) : api (a) {}
    ~Session()
    {
        if (inputName != nullptr) ignore (api->AllocatorFree (allocator, inputName));
        if (outputName != nullptr) ignore (api->AllocatorFree (allocator, outputName));
        if (memoryInfo != nullptr) api->ReleaseMemoryInfo (memoryInfo);
        if (session != nullptr) api->ReleaseSession (session);
        if (options != nullptr) api->ReleaseSessionOptions (options);
    }

    void ignore (OrtStatus* status) const
    {
        if (status != nullptr)
            api->ReleaseStatus (status);
    }

    const OrtApi* api;
    OrtSessionOptions* options = nullptr;
    OrtSession* session = nullptr;
    OrtMemoryInfo* memoryInfo = nullptr;
    OrtAllocator* allocator = nullptr;
    char* inputName = nullptr;
    char* outputName = nullptr;
    std::vector<int64_t> inputDims, outputDims;
    size_t inputSize = 0, outputSize = 0; // floats per run

    bool open (OrtEnv* env, const juce::File& model, juce::String& error)
    {
        if (failed (api, api->CreateSessionOptions (&options), error)) return false;
        if (failed (api, api->SetIntraOpNumThreads (options, inferenceThreads()), error)) return false;
        if (failed (api, api->SetSessionGraphOptimizationLevel (options, ORT_ENABLE_ALL), error)) return false;
        // Planning all buffers ahead keeps every intermediate tensor alive at once; for attention
        // models that alone is gigabytes.
        if (failed (api, api->DisableMemPattern (options), error)) return false;

       #if JUCE_WINDOWS
        const auto path = model.getFullPathName().toWideCharPointer();
       #else
        const auto path = model.getFullPathName().toRawUTF8();
       #endif
        if (failed (api, api->CreateSession (env, path, options, &session), error)) return false;
        if (failed (api, api->GetAllocatorWithDefaultOptions (&allocator), error)) return false;
        if (failed (api, api->SessionGetInputName (session, 0, allocator, &inputName), error)) return false;
        if (failed (api, api->SessionGetOutputName (session, 0, allocator, &outputName), error)) return false;
        if (failed (api, api->CreateCpuMemoryInfo (OrtArenaAllocator, OrtMemTypeDefault, &memoryInfo), error)) return false;

        auto shapeOf = [&] (bool input, std::vector<int64_t>& dims, size_t& size)
        {
            OrtTypeInfo* typeInfo = nullptr;
            if (failed (api, input ? api->SessionGetInputTypeInfo (session, 0, &typeInfo)
                                   : api->SessionGetOutputTypeInfo (session, 0, &typeInfo), error))
                return false;
            const OrtTensorTypeAndShapeInfo* tensorInfo = nullptr;
            size_t numDims = 0;
            bool ok = ! failed (api, api->CastTypeInfoToTensorInfo (typeInfo, &tensorInfo), error) && tensorInfo != nullptr
                   && ! failed (api, api->GetDimensionsCount (tensorInfo, &numDims), error) && numDims > 0 && numDims <= 8;
            if (ok)
            {
                dims.assign (numDims, 0);
                ok = ! failed (api, api->GetDimensions (tensorInfo, dims.data(), numDims), error);
            }
            api->ReleaseTypeInfo (typeInfo);
            if (! ok)
                return false;
            dims[0] = 1; // batch
            size = 1;
            for (auto d : dims)
            {
                if (d <= 0)
                    return false;
                size *= (size_t) d;
            }
            return true;
        };
        if (! shapeOf (true, inputDims, inputSize) || ! shapeOf (false, outputDims, outputSize))
        {
            if (error.isEmpty())
                error = "Unexpected separation model format";
            return false;
        }
        return true;
    }

    bool run (std::vector<float>& input, std::vector<float>& output, juce::String& error)
    {
        jassert (input.size() == inputSize);
        OrtValue* in = nullptr;
        OrtValue* out = nullptr;
        if (failed (api, api->CreateTensorWithDataAsOrtValue (memoryInfo, input.data(), input.size() * sizeof (float),
                                                              inputDims.data(), inputDims.size(),
                                                              ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT, &in), error))
            return false;

        const char* inNames[] { inputName };
        const char* outNames[] { outputName };
        const bool ok = ! failed (api, api->Run (session, nullptr, inNames, &in, 1, outNames, 1, &out), error);
        api->ReleaseValue (in);
        if (! ok)
            return false;

        float* data = nullptr;
        OrtTensorTypeAndShapeInfo* info = nullptr;
        size_t count = 0;
        const bool gotData = ! failed (api, api->GetTensorMutableData (out, (void**) &data), error) && data != nullptr
                          && ! failed (api, api->GetTensorTypeAndShape (out, &info), error)
                          && ! failed (api, api->GetTensorShapeElementCount (info, &count), error) && count == outputSize;
        if (info != nullptr)
            api->ReleaseTensorTypeAndShapeInfo (info);
        if (gotData)
            output.assign (data, data + count);
        else if (error.isEmpty())
            error = "Unexpected separation model output";
        api->ReleaseValue (out);
        return gotData;
    }
};
#endif

juce::AudioBuffer<float> resample (const juce::AudioBuffer<float>& in, double fromRate, double toRate, int channels)
{
    const double ratio = fromRate / toRate;
    const int outLength = (int) std::ceil (in.getNumSamples() / ratio);
    juce::AudioBuffer<float> out (channels, outLength);
    for (int ch = 0; ch < channels; ++ch)
    {
        const int srcChannel = std::min (ch, in.getNumChannels() - 1);
        if (std::abs (ratio - 1.0) < 1e-9)
        {
            out.copyFrom (ch, 0, in, srcChannel, 0, std::min (outLength, in.getNumSamples()));
            continue;
        }
        juce::WindowedSincInterpolator interpolator;
        interpolator.process (ratio, in.getReadPointer (srcChannel), out.getWritePointer (ch), outLength,
                              in.getNumSamples(), 0);
    }
    return out;
}

#if KITNAP_HAS_ONNX
// Runs a spectrogram model over `audio` (stereo, at the model's rate) in chunks that overlap by
// `overlap` samples and are cross-faded there (a chunk's edges are its least reliable part).
// The song is padded with silence so its start and end are covered like the rest. Returns
// numStems buffers where only the wanted ones are filled (a full song's stems take a lot of
// memory), or an empty vector on failure or cancel.
std::vector<juce::AudioBuffer<float>> runChunked (Session& session, Spectrogram& spectrogram, int numStems,
                                                  const juce::AudioBuffer<float>& audio, int overlap,
                                                  const std::vector<int>& wanted,
                                                  const std::function<void (float)>& progress,
                                                  const std::function<bool()>& shouldCancel,
                                                  juce::String& error)
{
    const int chunk = spectrogram.chunkLength();
    const int length = audio.getNumSamples();
    overlap = juce::jlimit (1, chunk / 2, overlap);
    const int step = chunk - overlap;
    const int border = overlap;
    const int total = border + length + border + chunk;

    std::vector<float> padded[2];
    for (int ch = 0; ch < 2; ++ch)
    {
        const float* src = audio.getReadPointer (std::min (ch, audio.getNumChannels() - 1));
        padded[ch].assign ((size_t) total, 0.0f);
        std::copy (src, src + length, padded[ch].begin() + border);
    }
    std::vector<float> fade ((size_t) chunk, 1.0f);
    for (int i = 0; i < overlap; ++i)
    {
        fade[(size_t) i] = (float) i / (float) std::max (1, overlap - 1);
        fade[(size_t) (chunk - 1 - i)] = fade[(size_t) i];
    }

    std::vector<juce::AudioBuffer<float>> stems ((size_t) numStems);
    for (int s : wanted)
    {
        stems[(size_t) s].setSize (2, length);
        stems[(size_t) s].clear();
    }
    std::vector<float> weight ((size_t) total, 0.0f), input, output, stem[2];
    const size_t stemSize = session.outputSize / (size_t) numStems;
    const int lastStart = total - chunk;

    // Chunks from the end of the song on would only cover padding: skip them.
    const int songEnd = border + length;
    for (int start = 0; start <= lastStart && start < songEnd; start += step)
    {
        if (shouldCancel && shouldCancel())
            return {};

        const float* chunkAudio[] { padded[0].data() + start, padded[1].data() + start };
        spectrogram.forward (chunkAudio, input);
        if (! session.run (input, output, error))
            return {};

        // The part of this chunk that lies inside the song; summed straight into the output and
        // divided by the total fade weight at the end.
        const int from = std::max (0, border - start), to = std::min (chunk, border + length - start);
        for (int s = 0; s < numStems; ++s)
        {
            auto& buffer = stems[(size_t) s];
            if (buffer.getNumSamples() == 0 || from >= to)
                continue;
            spectrogram.inverse (output.data() + (size_t) s * stemSize, stem);
            for (int ch = 0; ch < 2; ++ch)
            {
                float* out = buffer.getWritePointer (ch, start - border);
                for (int i = from; i < to; ++i)
                    out[i] += stem[ch][(size_t) i] * fade[(size_t) i];
            }
        }
        for (int i = 0; i < chunk; ++i)
            weight[(size_t) (start + i)] += fade[(size_t) i];

        if (progress)
            progress ((float) std::min (start + step, songEnd) / (float) songEnd);
    }

    for (auto& buffer : stems)
        for (int ch = 0; ch < buffer.getNumChannels(); ++ch)
        {
            float* out = buffer.getWritePointer (ch);
            for (int i = 0; i < length; ++i)
            {
                const float w = weight[(size_t) (border + i)];
                out[i] = w > 1e-6f ? out[i] / w : 0.0f;
            }
        }
    return stems;
}
#endif
} // namespace

//==============================================================================
bool DrumSeparator::isAvailable()
{
    return unavailableReason().isEmpty();
}

juce::String DrumSeparator::unavailableReason()
{
   #if KITNAP_HAS_ONNX
    if (! findModel (drumsModel).existsAsFile())
        return "Separation model not found";
    if (! findModel (splitModel).existsAsFile())
        return "Drum split model not found";
    auto& runtime = Runtime::get();
    return runtime.api == nullptr ? runtime.error : juce::String();
   #else
    return "Built without AI separation";
   #endif
}

juce::AudioBuffer<float> DrumSeparator::separate (const juce::AudioBuffer<float>& source, double sampleRate,
                                                  const std::function<void (float)>& progress,
                                                  const std::function<bool()>& shouldCancel,
                                                  juce::String& error)
{
   #if KITNAP_HAS_ONNX
    error = unavailableReason();
    if (error.isNotEmpty() || source.getNumSamples() == 0 || source.getNumChannels() == 0)
        return {};

    auto& runtime = Runtime::get();
    Session session (runtime.api);
    if (! session.open (runtime.env, findModel (drumsModel), error))
        return {};
    // BS-RoFormer SW, drums only: [1, 2 channels, 1025 bins, frames, re/im] in and out.
    const auto& dims = session.inputDims;
    if (dims.size() != 5 || dims[1] != 2 || dims[2] != (1 << drumsModel.fftOrder) / 2 + 1 || dims[4] != 2
        || session.outputSize != session.inputSize)
    {
        error = "Unexpected separation model format";
        return {};
    }

    const auto mix = resample (source, sampleRate, modelSampleRate, 2);
    Spectrogram spectrogram (drumsModel.fftOrder, drumsModel.hop, (int) dims[2], (int) dims[3], true);

    // The model works on short chunks (its attention over time needs memory that grows with the
    // square of the chunk length); half a second of each chunk's edge is cross-faded with the next.
    auto stems = runChunked (session, spectrogram, 1, mix, (int) (0.5 * modelSampleRate), { 0 },
                             progress, shouldCancel, error);
    if (stems.empty())
        return {};
    auto drums = std::move (stems[0]);
    return drums;
   #else
    juce::ignoreUnused (source, sampleRate, progress, shouldCancel);
    error = unavailableReason();
    return {};
   #endif
}

std::vector<juce::AudioBuffer<float>> DrumSeparator::split (const juce::AudioBuffer<float>& drums,
                                                            const std::vector<DrumPart>& wanted,
                                                            const std::function<void (float)>& progress,
                                                            const std::function<bool()>& shouldCancel,
                                                            juce::String& error)
{
   #if KITNAP_HAS_ONNX
    error = unavailableReason();
    if (error.isNotEmpty() || drums.getNumSamples() == 0 || drums.getNumChannels() == 0)
        return {};

    auto& runtime = Runtime::get();
    Session session (runtime.api);
    if (! session.open (runtime.env, findModel (splitModel), error))
        return {};
    // DrumSep (MDX23C): [1, 4, bins, frames] in, [1, parts, 4, bins, frames] out.
    const auto& dims = session.inputDims;
    if (dims.size() != 4 || dims[1] != 4 || dims[2] > (1 << splitModel.fftOrder) / 2 + 1
        || session.outputSize != session.inputSize * (size_t) numDrumParts)
    {
        error = "Unexpected drum split model format";
        return {};
    }

    Spectrogram spectrogram (splitModel.fftOrder, splitModel.hop, (int) dims[2], (int) dims[3]);
    std::vector<int> parts;
    for (auto part : wanted)
        parts.push_back ((int) part);
    // Chunks overlap by half: every sample is covered twice.
    auto result = runChunked (session, spectrogram, numDrumParts, drums, spectrogram.chunkLength() / 2, parts,
                              progress, shouldCancel, error);
    return result;
   #else
    juce::ignoreUnused (drums, wanted, progress, shouldCancel);
    error = "Built without AI separation";
    return {};
   #endif
}
} // namespace kitnap
