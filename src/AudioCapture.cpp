#include "AudioCapture.h"

#include "Utils.h"

#include <Audioclientactivationparams.h>
#include <Mmdeviceapi.h>
#include <wrl/client.h>
#include <wrl/implements.h>

#include <algorithm>
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

std::unique_ptr<WAVEFORMATEX, AudioCapture::CoTaskMemFreeDeleter> CreatePcmFormat(WORD channels, DWORD sampleRate, WORD bitsPerSample)
{
    auto* raw = reinterpret_cast<WAVEFORMATEX*>(CoTaskMemAlloc(sizeof(WAVEFORMATEX)));
    if (raw == nullptr) {
        utils::Fail(L"CoTaskMemAlloc failed for audio capture format.");
    }

    ZeroMemory(raw, sizeof(WAVEFORMATEX));
    raw->wFormatTag = WAVE_FORMAT_PCM;
    raw->nChannels = channels;
    raw->nSamplesPerSec = sampleRate;
    raw->wBitsPerSample = bitsPerSample;
    raw->nBlockAlign = static_cast<WORD>((channels * bitsPerSample) / 8);
    raw->nAvgBytesPerSec = raw->nSamplesPerSec * raw->nBlockAlign;
    raw->cbSize = 0;
    return std::unique_ptr<WAVEFORMATEX, AudioCapture::CoTaskMemFreeDeleter>(raw);
}

}  // namespace

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

    auto cleanupOnFailure = [this]() {
        if (m_stopEvent != nullptr) {
            SetEvent(m_stopEvent);
        }
        if (m_captureThread.joinable()) {
            m_captureThread.join();
        }
        if (m_audioClient) {
            m_audioClient->Stop();
        }
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
    };

    try {
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

        m_mixFormat = CreatePcmFormat(2, 44100, 16);

        const DWORD streamFlags = AUDCLNT_STREAMFLAGS_LOOPBACK | AUDCLNT_STREAMFLAGS_EVENTCALLBACK;
        utils::ThrowIfFailed(
            m_audioClient->Initialize(AUDCLNT_SHAREMODE_SHARED, streamFlags, 0, 0, m_mixFormat.get(), nullptr),
            L"IAudioClient::Initialize failed");
        utils::ThrowIfFailed(m_audioClient->SetEventHandle(m_sampleReadyEvent), L"IAudioClient::SetEventHandle failed");
        utils::ThrowIfFailed(m_audioClient->GetService(IID_PPV_ARGS(&m_captureClient)), L"IAudioClient::GetService(IAudioCaptureClient) failed");

        REFERENCE_TIME streamLatency = 0;
        const HRESULT streamLatencyHr = m_audioClient->GetStreamLatency(&streamLatency);
        if (SUCCEEDED(streamLatencyHr) && streamLatency > 0) {
            m_streamLatencyHns = static_cast<uint64_t>(streamLatency);
        } else {
            m_streamLatencyHns = 0;
            if (m_options.verbose) {
                utils::LogVerbose(L"IAudioClient::GetStreamLatency unavailable. Using zero latency compensation.");
            }
        }

        m_stats.sampleRate = m_mixFormat->nSamplesPerSec;
        m_stats.channels = m_mixFormat->nChannels;
        if (m_options.onFormat) {
            m_options.onFormat(*m_mixFormat);
        }

        m_captureStartQpc = utils::QueryPerformanceCounterValue();
        m_totalCapturedFrames = 0;
        const int64_t attachAnchorQpc = m_options.attachQpc != 0 ? m_options.attachQpc : m_captureStartQpc;
        m_attachOffsetHns = static_cast<uint64_t>(utils::QpcTicksToHundredsOfNanoseconds(std::max<int64_t>(0, attachAnchorQpc - m_options.baseQpc)));
        m_firstPacketQpcHns = 0;
        m_stats.startQpc = m_captureStartQpc;

        m_running.store(true);
        m_captureThread = std::thread(&AudioCapture::CaptureLoop, this);
        utils::ThrowIfFailed(m_audioClient->Start(), L"IAudioClient::Start failed");
    } catch (...) {
        cleanupOnFailure();
        throw;
    }
}

void AudioCapture::Stop()
{
    HANDLE stopEvent = nullptr;
    Microsoft::WRL::ComPtr<IAudioClient> audioClient;
    std::thread captureThread;
    HANDLE activationCompleteEvent = nullptr;
    HANDLE sampleReadyEvent = nullptr;

    {
        std::lock_guard<std::mutex> lock(m_mutex);

        stopEvent = m_stopEvent;
        sampleReadyEvent = m_sampleReadyEvent;
        activationCompleteEvent = m_activationCompleteEvent;
        audioClient = m_audioClient;
        if (m_captureThread.joinable()) {
            captureThread = std::move(m_captureThread);
        }
        m_running.store(false);
    }

    if (stopEvent != nullptr) {
        SetEvent(stopEvent);
    }
    if (audioClient) {
        audioClient->Stop();
    }
    if (captureThread.joinable()) {
        captureThread.join();
    }

    std::lock_guard<std::mutex> lock(m_mutex);
    m_captureClient.Reset();
    m_audioClient.Reset();
    m_mixFormat.reset();

    if (activationCompleteEvent != nullptr) {
        CloseHandle(activationCompleteEvent);
        m_activationCompleteEvent = nullptr;
    }
    if (sampleReadyEvent != nullptr) {
        CloseHandle(sampleReadyEvent);
        m_sampleReadyEvent = nullptr;
    }
    if (stopEvent != nullptr) {
        CloseHandle(stopEvent);
        m_stopEvent = nullptr;
    }
}

AudioCapture::Stats AudioCapture::GetStats() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_stats;
}

void AudioCapture::CaptureLoop()
{
    try {
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
    } catch (const std::exception& ex) {
        utils::LogWarning(L"Audio capture thread stopped: " + utils::Utf8ToWide(ex.what()));
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
        UINT64 devicePosition = 0;
        UINT64 qpcPositionHns = 0;
        utils::ThrowIfFailed(
            m_captureClient->GetBuffer(&data, &frameCount, &flags, &devicePosition, &qpcPositionHns),
            L"IAudioCaptureClient::GetBuffer failed");

        const uint64_t packetStartFrames = m_totalCapturedFrames;
        m_totalCapturedFrames += frameCount;

        uint64_t relativePositionHns = 0;
        if (qpcPositionHns > 0) {
            if (m_firstPacketQpcHns == 0) {
                m_firstPacketQpcHns = qpcPositionHns;
            }
            uint64_t packetTimelineHns = qpcPositionHns >= m_firstPacketQpcHns ? (qpcPositionHns - m_firstPacketQpcHns) : 0ULL;
            packetTimelineHns = packetTimelineHns > m_streamLatencyHns ? (packetTimelineHns - m_streamLatencyHns) : 0ULL;
            relativePositionHns = m_attachOffsetHns + packetTimelineHns;
        } else {
            const uint64_t packetOffsetHns = (static_cast<uint64_t>(packetStartFrames) * 10000000ULL) /
                static_cast<uint64_t>(m_mixFormat->nSamplesPerSec);
            relativePositionHns = m_attachOffsetHns + packetOffsetHns;
        }

        const bool discontinuity = (flags & AUDCLNT_BUFFERFLAGS_DATA_DISCONTINUITY) != 0;
        if (discontinuity) {
            ++m_stats.discontinuities;
            if (m_options.verbose) {
                utils::LogVerbose(L"Audio discontinuity detected.");
            }
        }

        const auto bytesToWrite = static_cast<size_t>(frameCount) * static_cast<size_t>(m_mixFormat->nBlockAlign);
        if (bytesToWrite > 0 && m_options.onChunk) {
            if ((flags & AUDCLNT_BUFFERFLAGS_SILENT) != 0 || data == nullptr) {
                silenceBuffer.assign(bytesToWrite, 0);
                m_options.onChunk(silenceBuffer.data(), silenceBuffer.size(), relativePositionHns, discontinuity);
            } else {
                m_options.onChunk(data, bytesToWrite, relativePositionHns, discontinuity);
            }
            m_stats.bytesWritten += bytesToWrite;
        }

        utils::ThrowIfFailed(m_captureClient->ReleaseBuffer(frameCount), L"IAudioCaptureClient::ReleaseBuffer failed");
        utils::ThrowIfFailed(m_captureClient->GetNextPacketSize(&packetLength), L"IAudioCaptureClient::GetNextPacketSize failed");
    }
}

}  // namespace corebs
