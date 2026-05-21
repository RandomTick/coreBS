#pragma once

#include <windows.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <wrl/client.h>

#include <cstddef>
#include <filesystem>
#include <mutex>
#include <vector>

namespace corebs {

class VideoCapture {
public:
    struct StartOptions {
        std::filesystem::path outputPath;
        uint32_t width = 1920;
        uint32_t height = 1080;
        int fps = 60;
        int64_t baseQpc = 0;
    };

    struct Stats {
        uint32_t width = 0;
        uint32_t height = 0;
        uint64_t writtenFrames = 0;
    };

    VideoCapture();
    ~VideoCapture();

    void Start(const StartOptions& options);
    void WriteFrame(const std::vector<std::byte>& frameData, int64_t frameQpc);
    void Stop();
    Stats GetStats() const;

private:
    void CreateSinkWriter();

    StartOptions m_options{};
    Stats m_stats{};
    Microsoft::WRL::ComPtr<IMFSinkWriter> m_sinkWriter;
    DWORD m_streamIndex = 0;
    mutable std::mutex m_mutex;
    bool m_running = false;
};

}  // namespace corebs
