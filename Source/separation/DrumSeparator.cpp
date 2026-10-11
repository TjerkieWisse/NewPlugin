#include "DrumSeparator.h"
#include <juce_dsp/juce_dsp.h>

#if KITNAP_HAS_ONNX
 #include <onnxruntime_c_api.h>
#endif

namespace kitnap
{
namespace
{
const char* modelFileName = "kitnap_drums.onnx";

juce::Array<juce::File> searchFolders()
{
    const auto binaryFolder = juce::File::getSpecialLocation (juce::File::currentExecutableFile).getParentDirectory();
    return { binaryFolder.getChildFile ("Resources"),
             binaryFolder.getParentDirectory().getChildFile ("Resources"),
             binaryFolder,
             juce::File::getSpecialLocation (juce::File::userApplicationDataDirectory).getChildFile ("KITNAP") };
}

juce::File findFile (const juce::String& pattern)
{
    for (auto& folder : searchFolders())
        if (folder.isDirectory())
            for (auto& f : folder.findChildFiles (juce::File::findFiles, false, pattern))
                return f;
    return {};
}

juce::File findModel() { return findFile (modelFileName); }

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
// which is what MDX-Net models are trained with.
class Spectrogram
{
public:
    static constexpr int fftOrder = 12;
    static constexpr int fftSize = 1 << fftOrder; // 4096
    static constexpr int hop = 1024;
    static constexpr int numBins = fftSize / 2 + 1;

    Spectrogram (int numFreqBins, int numFrames)
        : dimF (numFreqBins), dimT (numFrames), binsF ((size_t) numFreqBins), framesT ((size_t) numFrames), fft (fftOrder)
    {
        window.resize ((size_t) fftSize);
        for (int i = 0; i < fftSize; ++i)
            window[(size_t) i] = 0.5f - 0.5f * std::cos (juce::MathConstants<float>::twoPi * (float) i / (float) fftSize);
        scratch.resize ((size_t) fftSize * 2);
    }

    int chunkLength() const { return hop * (dimT - 1); }

    // audio: chunkLength() samples per channel -> tensor [4][dimF][dimT] (ch0 re, ch0 im, ch1 re, ch1 im).
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

                const auto plane = binsF * framesT;
                float* re = tensor.data() + (size_t) (2 * ch) * plane;
                float* im = tensor.data() + (size_t) (2 * ch + 1) * plane;
                for (size_t f = 0; f < binsF; ++f)
                {
                    re[f * framesT + (size_t) t] = scratch[f * 2];
                    im[f * framesT + (size_t) t] = scratch[f * 2 + 1];
                }
            }
    }

    // tensor [4][dimF][dimT] -> chunkLength() samples per channel (bins above dimF are zero).
    void inverse (const float* tensor, std::vector<float> (&audio)[2])
    {
        const int length = chunkLength();
        const int total = fftSize + hop * (dimT - 1);
        std::vector<float> sum ((size_t) total), norm ((size_t) total);

        for (int ch = 0; ch < 2; ++ch)
        {
            std::fill (sum.begin(), sum.end(), 0.0f);
            std::fill (norm.begin(), norm.end(), 0.0f);
            const auto plane = binsF * framesT;
            const float* re = tensor + (size_t) (2 * ch) * plane;
            const float* im = tensor + (size_t) (2 * ch + 1) * plane;

            for (int t = 0; t < dimT; ++t)
            {
                std::fill (scratch.begin(), scratch.end(), 0.0f);
                for (size_t f = 0; f < binsF; ++f)
                {
                    scratch[f * 2] = re[f * framesT + (size_t) t];
                    scratch[f * 2 + 1] = im[f * framesT + (size_t) t];
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

private:
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
    int dimF = 0, dimT = 0;

    bool open (OrtEnv* env, const juce::File& model, juce::String& error)
    {
        if (failed (api, api->CreateSessionOptions (&options), error)) return false;
        if (failed (api, api->SetIntraOpNumThreads (options, std::max (1, juce::SystemStats::getNumCpus() - 1)), error)) return false;
        if (failed (api, api->SetSessionGraphOptimizationLevel (options, ORT_ENABLE_ALL), error)) return false;

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

        // Expect an input of [batch, 4, dimF, dimT].
        OrtTypeInfo* typeInfo = nullptr;
        if (failed (api, api->SessionGetInputTypeInfo (session, 0, &typeInfo), error)) return false;
        const OrtTensorTypeAndShapeInfo* tensorInfo = nullptr;
        size_t numDims = 0;
        int64_t dims[4] {};
        const bool ok = ! failed (api, api->CastTypeInfoToTensorInfo (typeInfo, &tensorInfo), error) && tensorInfo != nullptr
                     && ! failed (api, api->GetDimensionsCount (tensorInfo, &numDims), error) && numDims == 4
                     && ! failed (api, api->GetDimensions (tensorInfo, dims, 4), error);
        api->ReleaseTypeInfo (typeInfo);

        if (! ok || dims[1] != 4 || dims[2] <= 0 || dims[2] > Spectrogram::numBins || dims[3] <= 1)
        {
            error = "Unexpected separation model format";
            return false;
        }
        dimF = (int) dims[2];
        dimT = (int) dims[3];
        return true;
    }

    bool run (std::vector<float>& input, std::vector<float>& output, juce::String& error)
    {
        const int64_t shape[] { 1, 4, dimF, dimT };
        OrtValue* in = nullptr;
        OrtValue* out = nullptr;
        if (failed (api, api->CreateTensorWithDataAsOrtValue (memoryInfo, input.data(), input.size() * sizeof (float),
                                                              shape, 4, ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT, &in), error))
            return false;

        const char* inNames[] { inputName };
        const char* outNames[] { outputName };
        const bool ok = ! failed (api, api->Run (session, nullptr, inNames, &in, 1, outNames, 1, &out), error);
        api->ReleaseValue (in);
        if (! ok)
            return false;

        float* data = nullptr;
        const bool gotData = ! failed (api, api->GetTensorMutableData (out, (void**) &data), error) && data != nullptr;
        if (gotData)
            output.assign (data, data + input.size());
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
} // namespace

//==============================================================================
bool DrumSeparator::isAvailable()
{
    return unavailableReason().isEmpty();
}

juce::String DrumSeparator::unavailableReason()
{
   #if KITNAP_HAS_ONNX
    if (! findModel().existsAsFile())
        return "Separation model not found";
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
    if (! session.open (runtime.env, findModel(), error))
        return {};

    const auto mix = resample (source, sampleRate, modelSampleRate, 2);
    const int length = mix.getNumSamples();

    // MDX-Net chunking: each chunk is analysed whole, but only its middle part (minus `trim`
    // at both edges) is kept, so chunk borders never land in the output.
    Spectrogram spectrogram (session.dimF, session.dimT);
    const int chunk = spectrogram.chunkLength();
    const int trim = Spectrogram::fftSize / 2;
    const int keep = chunk - 2 * trim;
    const int padded = ((length + keep - 1) / keep) * keep;

    std::vector<float> paddedMix[2];
    for (int ch = 0; ch < 2; ++ch)
    {
        paddedMix[ch].assign ((size_t) (trim + padded + trim + chunk), 0.0f);
        std::copy (mix.getReadPointer (ch), mix.getReadPointer (ch) + length, paddedMix[ch].begin() + trim);
    }

    juce::AudioBuffer<float> drums (2, length);
    drums.clear();
    std::vector<float> input, output, stem[2];

    for (int start = 0; start < padded; start += keep)
    {
        if (shouldCancel && shouldCancel())
            return {};

        const float* chunkAudio[] { paddedMix[0].data() + start, paddedMix[1].data() + start };
        spectrogram.forward (chunkAudio, input);
        if (! session.run (input, output, error))
            return {};
        spectrogram.inverse (output.data(), stem);

        const int count = std::min (keep, length - start);
        for (int ch = 0; ch < 2 && count > 0; ++ch)
            drums.copyFrom (ch, start, stem[ch].data() + trim, count);

        if (progress)
            progress ((float) std::min (padded, start + keep) / (float) padded);
    }

    return drums;
   #else
    juce::ignoreUnused (source, sampleRate, progress, shouldCancel);
    error = unavailableReason();
    return {};
   #endif
}
} // namespace kitnap
