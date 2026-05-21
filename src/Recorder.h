#pragma once

#include "AudioCapture.h"
#include "ProcessFinder.h"
#include "VideoCapture.h"
#include "WindowCaptureSource.h"
#include "WindowFinder.h"

#include <atomic>
#include <chrono>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>

namespace corebs {

class SessionAudioWriter;

class Recorder {
public:
    Recorder();
    ~Recorder();

    struct Options {
        std::optional<std::wstring> exeName;
        std::optional<DWORD> pid;
        std::optional<std::wstring> windowTitle;
        std::optional<std::filesystem::path> outputPath;
        std::wstring liveSplitTitle = L"LiveSplit";
        int fps = 60;
        bool captureCursor = true;
        int audioSyncMs = 1000;
        bool audioEnabled = true;
        bool waitForTarget = true;
        bool verbose = false;
    };

    enum class State {
        WaitingForTarget,
        WaitingForWindow,
        Recording,
        Finalizing,
        Stopped,
    };

    enum class StopReason {
        None,
        CtrlC,
        TargetExited,
        WindowLost,
    };

    int Run(const Options& options);

private:
    struct ResolvedTarget {
        ProcessInfo process;
        WindowInfo window;
    };

    struct SessionOutputPlan {
        std::filesystem::path finalOutputPath;
        std::filesystem::path tempVideoPath;
        std::filesystem::path tempAudioPath;
    };

    void ValidateOptions() const;
    bool UsesStableTargetIdentity() const;
    std::optional<ResolvedTarget> TryResolveCurrentTarget(bool& sawMatchingProcess) const;
    SessionOutputPlan ResolveSessionOutputPlan(const Options& options) const;

    void StartSession();
    void FinalizeSession(int64_t stopQpc);
    void TransitionTo(State state, const std::wstring& message);
    void RequestStop(StopReason reason);
    void SleepWithStopCheck(std::chrono::milliseconds duration) const;

    void UpdateGameCapture(int64_t nowQpc);
    void UpdateLiveSplitCapture(int64_t nowQpc);
    void AttachGameTarget(const ResolvedTarget& target, bool isReattach);
    void DetachGameTarget(StopReason reason, bool keepWaiting);
    void ReleaseActiveGameResources();
    void ReleaseLiveSplitResources();

    void OnAudioFormat(const WAVEFORMATEX& format);
    void OnAudioChunk(const BYTE* data, size_t sizeBytes, uint64_t qpcPositionHns, bool discontinuity);

    void WritePendingFrames(int64_t nowQpc);
    void WriteFrameAt(int64_t frameQpc);

    Options m_options{};
    SessionOutputPlan m_sessionOutputPlan{};

    State m_state = State::Stopped;
    std::wstring m_lastStateMessage;
    std::atomic<bool> m_stopRequested{false};
    StopReason m_stopReason = StopReason::None;
    int64_t m_sessionStartQpc = 0;
    int64_t m_stopRequestedQpc = 0;
    int64_t m_frameIntervalQpc = 0;
    int64_t m_nextFrameQpc = 0;
    int64_t m_lastGamePollQpc = 0;
    int64_t m_lastLiveSplitPollQpc = 0;
    uint64_t m_droppedFrameCount = 0;
    uint64_t m_currentAudioAttachHns = 0;

    bool m_sessionStarted = false;
    bool m_hasEverAttachedGame = false;
    bool m_liveSplitWaitingLogged = false;

    std::optional<ResolvedTarget> m_activeTarget;
    HANDLE m_activeProcessHandle = nullptr;
    bool m_gameAudioStarted = false;

    std::optional<WindowInfo> m_liveSplitWindow;

    VideoCapture m_videoCapture;
    AudioCapture m_audioCapture;
    WindowCaptureSource m_gameCapture;
    WindowCaptureSource m_liveSplitCapture;
    std::unique_ptr<SessionAudioWriter> m_audioWriter;
};

}  // namespace corebs
