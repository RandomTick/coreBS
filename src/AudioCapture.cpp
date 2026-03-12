#include "AudioCapture.h"

#include "Utils.h"

#include <Audioclientactivationparams.h>
#include <Mmdeviceapi.h>
#include <wrl/client.h>
#include <wrl/implements.h>

#include <fstream>
#include <vector>

namespace corebs {

namespace {

class AudioInterfaceActivator final
    : public Microsoft::WRL::RuntimeClass<
          Microsoft::WRL::RuntimeClassFlags<Microsoft::WRL::ClassicCom>,
          Microsoft::WRL::FtmBase,
          IActivateAudioInterfaceCompletionHandler> {
public:
    explicit AudioInterfaceActivator(HANDLE completionEvent)
        : m_completionEvent(completionEvent)
    {
    }

    STDMETHODIMP ActivateCompleted(IActivateAudioInterfaceAsyncOperation* operation) override
    {
        HRESULT activateResult = E_FAIL;
        Microsoft::WRL::ComPtr<IUnknown> audioClientUnknown;
        const HRESULT hr = operation->GetActivateResult(&activateResult, &audioClientUnknown);
        if (SUCCEEDED(hr) && SUCCEEDED(activateResult)) {
            audioClientUnknown.As(&m_audioClient);
        }
        m_result = FAILED(hr) ? hr : activateResult;
        SetEvent(m_completionEvent);
        return S_OK;
    }

    HRESULT Result() const
    {
        return m_result;
    }

    Microsoft::WRL::ComPtr<IAudioClient> GetAudioClient() const
    {
        return m_audioClient;
    }

private:
    HANDLE m_completionEvent = nullptr;
    HRESULT m_result = E_FAIL;
    Microsoft::WRL::ComPtr<IAudioClient> m_audioClient;
};

void WriteFourCc(std::ofstream& stream, const char (&text)[5])
{
    stream.write(text, 4);
}

template <typename T>
void WriteBinary(std::ofstream& stream, const T& value)
{
    stream.write(reinterpret_cast<const char*>(&value), sizeof(value));
}

}  // namespace

struct AudioCapture::WaveFileSession {
    explicit WaveFileSession(const std::filesystem::path& path)
        : stream(path, std::ios::binary | std::ios::trunc)
    {
        if (!stream) {
            utils::Fail(L"Failed to open WAV output file: " + path.wstring());
        }
    }

    void WriteHeader(const WAVEFORMATEX& format)
    {
        const uint32_t formatSize = static_cast<uint32_t>(sizeof(WAVEFORMATEX) + format.cbSize);
        WriteFourCc(stream, "RIFF");
        WriteBinary<uint32_t>(stream, 0);
        WriteFourCc(stream, "WAVE");
        WriteFourCc(stream, "fmt ");
        WriteBinary<uint32_t>(stream, formatSize);
        stream.write(reinterpret_cast<const char*>(&format), formatSize);
        WriteFourCc(stream, "data");
        dataSizeOffset = static_cast<std::streamoff>(stream.tellp());
        WriteBinary<uint32_t>(stream, 0);
    }

    void Finalize(uint64_t dataBytes)
    {
        stream.flush();
        const auto endPosition = static_cast<uint64_t>(stream.tellp());
        const auto riffSize = static_cast<uint32_t>(endPosition - 8);
        const auto wavDataSize = static_cast<uint32_t>(std::min<uint64_t>(dataBytes, 0xFFFFFFFFULL));

        stream.seekp(4, std::ios::beg);
        WriteBinary<uint32_t>(stream, riffSize);
        stream.seekp(dataSizeOffset, std::ios::beg);
        WriteBinary<uint32_t>(stream, wavDataSize);
        stream.flush();
    }

    std::ofstream stream;
    std::streamoff dataSizeOffset = 0;
};

AudioCapture::AudioCapture() = default;

AudioCapture::~AudioCapture()
{
    Stop();
}

void AudioCapture::CoTaskMemFreeDeleter::operator()(WAVEFORMATEX* value) const
{
    if (value != nullptr) {
        CoTaskMemFree(value);
    }
}

void AudioCapture::Start(const StartOptions& options)
{
    std::lock_guard<std::mutex> lock(m_mutex);

    if (m_running.load()) {
        utils::Fail(L"Audio capture has already started.");
    }

    m_options = options;
    m_stats = {};

    m_stopEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    m_sampleReadyEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    m_activationCompleteEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (m_stopEvent == nullptr || m_sampleReadyEvent == nullptr || m_activationCompleteEvent == nullptr) {
        utils::Fail(L"Failed to create audio capture event handles: " + utils::FormatWindowsError(GetLastError()));
    }

    AUDIOCLIENT_ACTIVATION_PARAMS activationParams{};
    activationParams.ActivationType = AUDIOCLIENT_ACTIVATION_TYPE_PROCESS_LOOPBACK;
    activationParams.ProcessLoopbackParams.TargetProcessId = options.targetPid;
    activationParams.ProcessLoopbackParams.ProcessLoopbackMode = PROCESS_LOOPBACK_MODE_INCLUDE_TARGET_PROCESS_TREE;

    PROPVARIANT activationPropVariant{};
    activationPropVariant.vt = VT_BLOB;
    activationPropVariant.blob.cbSize = sizeof(activationParams);
    activationPropVariant.blob.pBlobData = reinterpret_cast<BYTE*>(&activationParams);

    Microsoft::WRL::ComPtr<AudioInterfaceActivator> activator = Microsoft::WRL::Make<AudioInterfaceActivator>(m_activationCompleteEvent);
    Microsoft::WRL::ComPtr<IActivateAudioInterfaceAsyncOperation> asyncOperation;

    utils::ThrowIfFailed(
        ActivateAudioInterfaceAsync(
            VIRTUAL_AUDIO_DEVICE_PROCESS_LOOPBACK,
            __uuidof(IAudioClient),
            &activationPropVariant,
            activator.Get(),
            &asyncOperation),
        L"ActivateAudioInterfaceAsync failed");

    const auto activationWait = WaitForSingleObject(m_activationCompleteEvent, INFINITE);
    if (activationWait != WAIT_OBJECT_0) {
        utils::Fail(L"Waiting for ActivateAudioInterfaceAsync failed.");
    }

    utils::ThrowIfFailed(activator->Result(), L"Application loopback activation failed");
    m_audioClient = activator->GetAudioClient();

    WAVEFORMATEX* rawMixFormat = nullptr;
    utils::ThrowIfFailed(m_audioClient->GetMixFormat(&rawMixFormat), L"IAudioClient::GetMixFormat failed");
    m_mixFormat.reset(rawMixFormat);

    const DWORD streamFlags = AUDCLNT_STREAMFLAGS_LOOPBACK | AUDCLNT_STREAMFLAGS_EVENTCALLBACK;
    utils::ThrowIfFailed(
        m_audioClient->Initialize(AUDCLNT_SHAREMODE_SHARED, streamFlags, 0, 0, m_mixFormat.get(), nullptr),
        L"IAudioClient::Initialize failed");
    utils::ThrowIfFailed(m_audioClient->SetEventHandle(m_sampleReadyEvent), L"IAudioClient::SetEventHandle failed");
    utils::ThrowIfFailed(m_audioClient->GetService(IID_PPV_ARGS(&m_captureClient)), L"IAudioClient::GetService(IAudioCaptureClient) failed");

    m_stats.sampleRate = m_mixFormat->nSamplesPerSec;
    m_stats.channels = m_mixFormat->nChannels;
    OpenWaveFile();

    m_running.store(true);
    m_captureThread = std::thread(&AudioCapture::CaptureLoop, this);
    m_stats.startQpc = utils::QueryPerformanceCounterValue();
    utils::ThrowIfFailed(m_audioClient->Start(), L"IAudioClient::Start failed");
}

void AudioCapture::Stop()
{
    std::lock_guard<std::mutex> lock(m_mutex);

    if (!m_running.exchange(false)) {
        return;
    }

    if (m_audioClient) {
        m_audioClient->Stop();
    }
    if (m_stopEvent != nullptr) {
        SetEvent(m_stopEvent);
    }
    if (m_captureThread.joinable()) {
        m_captureThread.join();
    }

    FinalizeWaveFile();

    m_captureClient.Reset();
    m_audioClient.Reset();
    m_mixFormat.reset();

    if (m_activationCompleteEvent != nullptr) {
        CloseHandle(m_activationCompleteEvent);
        m_activationCompleteEvent = nullptr;
    }
    if (m_sampleReadyEvent != nullptr) {
        CloseHandle(m_sampleReadyEvent);
        m_sampleReadyEvent = nullptr;
    }
    if (m_stopEvent != nullptr) {
        CloseHandle(m_stopEvent);
        m_stopEvent = nullptr;
    }
}

AudioCapture::Stats AudioCapture::GetStats() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_stats;
}

void AudioCapture::OpenWaveFile()
{
    m_waveFile = std::make_unique<WaveFileSession>(m_options.outputPath);
    m_waveFile->WriteHeader(*m_mixFormat);
}

void AudioCapture::FinalizeWaveFile()
{
    if (!m_waveFile) {
        return;
    }

    m_waveFile->Finalize(m_stats.bytesWritten);
    m_waveFile.reset();
}

void AudioCapture::CaptureLoop()
{
    HANDLE waitHandles[2] = {m_stopEvent, m_sampleReadyEvent};

    while (true) {
        const DWORD waitResult = WaitForMultipleObjects(2, waitHandles, FALSE, INFINITE);
        if (waitResult == WAIT_OBJECT_0) {
            CaptureAvailablePackets();
            return;
        }
        if (waitResult == WAIT_OBJECT_0 + 1) {
            CaptureAvailablePackets();
        }
    }
}

void AudioCapture::CaptureAvailablePackets()
{
    UINT32 packetLength = 0;
    std::vector<BYTE> silenceBuffer;

    utils::ThrowIfFailed(m_captureClient->GetNextPacketSize(&packetLength), L"IAudioCaptureClient::GetNextPacketSize failed");
    while (packetLength > 0) {
        BYTE* data = nullptr;
        UINT32 frameCount = 0;
        DWORD flags = 0;

        utils::ThrowIfFailed(
            m_captureClient->GetBuffer(&data, &frameCount, &flags, nullptr, nullptr),
            L"IAudioCaptureClient::GetBuffer failed");

        if ((flags & AUDCLNT_BUFFERFLAGS_DATA_DISCONTINUITY) != 0) {
            ++m_stats.discontinuities;
            if (m_options.verbose) {
                utils::LogVerbose(L"Audio discontinuity detected.");
            }
        }

        const auto bytesToWrite = static_cast<size_t>(frameCount) * static_cast<size_t>(m_mixFormat->nBlockAlign);
        if (bytesToWrite > 0) {
            if ((flags & AUDCLNT_BUFFERFLAGS_SILENT) != 0 || data == nullptr) {
                silenceBuffer.assign(bytesToWrite, 0);
                m_waveFile->stream.write(reinterpret_cast<const char*>(silenceBuffer.data()), static_cast<std::streamsize>(silenceBuffer.size()));
            } else {
                m_waveFile->stream.write(reinterpret_cast<const char*>(data), static_cast<std::streamsize>(bytesToWrite));
            }
            m_stats.bytesWritten += bytesToWrite;
        }

        utils::ThrowIfFailed(m_captureClient->ReleaseBuffer(frameCount), L"IAudioCaptureClient::ReleaseBuffer failed");
        utils::ThrowIfFailed(m_captureClient->GetNextPacketSize(&packetLength), L"IAudioCaptureClient::GetNextPacketSize failed");
    }
}

}  // namespace corebs