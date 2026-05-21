#pragma once

#include <windows.h>
#include <audioclient.h>
#include <wrl/client.h>

#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>

namespace corebs {

class AudioCapture {
public:
    struct StartOptions {
        DWORD targetPid = 0;
        bool verbose = false;
        int64_t baseQpc = 0;
        int64_t attachQpc = 0;
        std::function<void(const WAVEFORMATEX& format)> onFormat;
        std::function<void(const BYTE* data, size_t sizeBytes, uint64_t qpcPositionHns, bool discontinuity)> onChunk;
    };

    struct Stats {
        uint64_t bytesWritten = 0;
        uint64_t discontinuities = 0;
        uint32_t sampleRate = 0;
        uint16_t channels = 0;
        int64_t startQpc = 0;
    };

    struct CoTaskMemFreeDeleter {
        void operator()(WAVEFORMATEX* value) const;
    };

    AudioCapture();
    ~AudioCapture();

    void Start(const StartOptions& options);
    void Stop();
    Stats GetStats() const;

private:
    void CaptureLoop();
    void CaptureAvailablePackets();

    StartOptions m_options{};
    mutable std::mutex m_mutex;
    std::atomic<bool> m_running{false};
    Stats m_stats{};

    Microsoft::WRL::ComPtr<IAudioClient> m_audioClient;
    Microsoft::WRL::ComPtr<IAudioCaptureClient> m_captureClient;
    std::unique_ptr<WAVEFORMATEX, CoTaskMemFreeDeleter> m_mixFormat;

    HANDLE m_stopEvent = nullptr;
    HANDLE m_sampleReadyEvent = nullptr;
    HANDLE m_activationCompleteEvent = nullptr;
    std::thread m_captureThread;

    int64_t m_captureStartQpc = 0;
    uint64_t m_totalCapturedFrames = 0;
    uint64_t m_attachOffsetHns = 0;
    uint64_t m_firstPacketQpcHns = 0;
    uint64_t m_streamLatencyHns = 0;
};

}  // namespace corebs
