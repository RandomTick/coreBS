#pragma once

#include <windows.h>
#include <audioclient.h>
#include <wrl/client.h>

#include <atomic>
#include <filesystem>
#include <memory>
#include <mutex>
#include <thread>

namespace corebs {

class AudioCapture {
public:
    struct StartOptions {
        DWORD targetPid = 0;
        std::filesystem::path outputPath;
        bool verbose = false;
        int64_t baseQpc = 0;
    };

    struct Stats {
        uint64_t bytesWritten = 0;
        uint64_t discontinuities = 0;
        uint32_t sampleRate = 0;
        uint16_t channels = 0;
        int64_t startQpc = 0;
    };

    AudioCapture();
    ~AudioCapture();

    void Start(const StartOptions& options);
    void Stop();
    Stats GetStats() const;

private:
    struct CoTaskMemFreeDeleter {
        void operator()(WAVEFORMATEX* value) const;
    };

    struct WaveFileSession;

    void OpenWaveFile();
    void FinalizeWaveFile();
    void CaptureLoop();
    void CaptureAvailablePackets();

    StartOptions m_options{};
    mutable std::mutex m_mutex;
    std::atomic<bool> m_running{false};
    Stats m_stats{};

    Microsoft::WRL::ComPtr<IAudioClient> m_audioClient;
    Microsoft::WRL::ComPtr<IAudioCaptureClient> m_captureClient;
    std::unique_ptr<WAVEFORMATEX, CoTaskMemFreeDeleter> m_mixFormat;
    std::unique_ptr<WaveFileSession> m_waveFile;

    HANDLE m_stopEvent = nullptr;
    HANDLE m_sampleReadyEvent = nullptr;
    HANDLE m_activationCompleteEvent = nullptr;
    std::thread m_captureThread;
};

}  // namespace corebs