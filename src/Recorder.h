#pragma once

#include "AudioCapture.h"
#include "ProcessFinder.h"
#include "VideoCapture.h"
#include "WindowFinder.h"

#include <atomic>
#include <condition_variable>
#include <filesystem>
#include <mutex>
#include <optional>
#include <thread>

namespace corebs {

class Recorder {
public:
    struct Options {
        std::optional<std::wstring> exeName;
        std::optional<DWORD> pid;
        std::optional<std::wstring> windowTitle;
        std::optional<std::filesystem::path> outputPath;
        int fps = 60;
        bool captureCursor = true;
        bool audioEnabled = true;
        bool verbose = false;
    };

    enum class StopReason {
        None,
        CtrlC,
        TargetExited,
        CaptureEnded,
    };

    int Run(const Options& options);

private:
    struct ResolvedTarget {
        ProcessInfo process;
        WindowInfo window;
    };

    struct OutputPlan {
        std::filesystem::path finalOutputPath;
        std::filesystem::path videoPath;
        std::optional<std::filesystem::path> audioPath;
        bool needsMux = false;
    };

    ResolvedTarget ResolveTarget(const Options& options) const;
    OutputPlan ResolveOutputPlan(const Options& options) const;
    void FinalizeOutputs(int64_t baseQpc, const AudioCapture::Stats& audioStats) const;
    void RequestStop(StopReason reason);
    void StartProcessMonitor();
    void StopProcessMonitor();

    Options m_options{};
    ResolvedTarget m_target{};
    OutputPlan m_outputPlan{};

    HANDLE m_processHandle = nullptr;
    HANDLE m_monitorStopEvent = nullptr;
    std::thread m_processMonitorThread;

    std::mutex m_stopMutex;
    std::condition_variable m_stopCondition;
    std::atomic<bool> m_stopRequested{false};
    StopReason m_stopReason = StopReason::None;

    VideoCapture m_videoCapture;
    AudioCapture m_audioCapture;
};

}  // namespace corebs