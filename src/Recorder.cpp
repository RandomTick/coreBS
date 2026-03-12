#include "Recorder.h"

#include "SignalHandler.h"
#include "Utils.h"

#include <sstream>

namespace corebs {

namespace {

std::wstring StopReasonToString(Recorder::StopReason reason)
{
    switch (reason) {
    case Recorder::StopReason::CtrlC:
        return L"Ctrl+C";
    case Recorder::StopReason::TargetExited:
        return L"target process exit";
    case Recorder::StopReason::CaptureEnded:
        return L"capture ended";
    case Recorder::StopReason::None:
    default:
        return L"normal shutdown";
    }
}

std::wstring ExtensionLower(const std::filesystem::path& path)
{
    return utils::ToLower(path.extension().wstring());
}

}  // namespace

int Recorder::Run(const Options& options)
{
    m_options = options;
    m_target = ResolveTarget(options);
    m_outputPath = ResolveOutputPath(options);
    m_processHandle = ProcessFinder::OpenProcessForMonitoring(m_target.process.pid);
    m_monitorStopEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (m_monitorStopEvent == nullptr) {
        utils::Fail(L"CreateEventW failed for process monitor shutdown: " + utils::FormatWindowsError(GetLastError()));
    }

    utils::LogInfo(L"Chosen PID: " + std::to_wstring(m_target.process.pid) + L" (" + m_target.process.exeName + L")");
    utils::LogInfo(L"Chosen HWND: " + utils::FormatPointer(m_target.window.hwnd));
    utils::LogInfo(L"Chosen output path: " + m_outputPath.wstring());
    utils::LogInfo(L"Video size: " + utils::FormatSize(
        static_cast<unsigned int>(m_target.window.bounds.right - m_target.window.bounds.left),
        static_cast<unsigned int>(m_target.window.bounds.bottom - m_target.window.bounds.top)));
    utils::LogInfo(L"FPS: " + std::to_wstring(options.fps));
    utils::LogInfo(L"Audio enabled: no (video-only MVP build)");

    if (options.audioEnabled) {
        utils::LogWarning(L"Audio capture is not in this first commit yet. Continuing with video only.");
    }

    const auto baseQpc = utils::QueryPerformanceCounterValue();

    SignalHandler::Install([this] { RequestStop(StopReason::CtrlC); });
    StartProcessMonitor();

    try {
        m_videoCapture.Start(VideoCapture::StartOptions{
            m_target.window.hwnd,
            m_outputPath,
            options.fps,
            options.captureCursor,
            options.verbose,
            baseQpc,
        });
    } catch (...) {
        StopProcessMonitor();
        SignalHandler::Uninstall();
        if (m_monitorStopEvent != nullptr) {
            CloseHandle(m_monitorStopEvent);
            m_monitorStopEvent = nullptr;
        }
        if (m_processHandle != nullptr) {
            CloseHandle(m_processHandle);
            m_processHandle = nullptr;
        }
        throw;
    }

    utils::LogInfo(L"Start reason: target window capture started.");

    {
        std::unique_lock<std::mutex> lock(m_stopMutex);
        m_stopCondition.wait(lock, [this] { return m_stopRequested.load(); });
    }

    utils::LogInfo(L"Stop reason: " + StopReasonToString(m_stopReason));
    m_videoCapture.Stop();
    StopProcessMonitor();
    SignalHandler::Uninstall();

    if (m_monitorStopEvent != nullptr) {
        CloseHandle(m_monitorStopEvent);
        m_monitorStopEvent = nullptr;
    }
    if (m_processHandle != nullptr) {
        CloseHandle(m_processHandle);
        m_processHandle = nullptr;
    }

    const auto stats = m_videoCapture.GetStats();
    std::wstringstream stream;
    stream << L"Finalized video-only output at " << m_outputPath.wstring()
           << L" (" << stats.writtenFrames << L" frames";
    if (options.verbose) {
        stream << L", dropped=" << stats.droppedFrames;
    }
    stream << L")";
    utils::LogInfo(stream.str());

    return 0;
}

Recorder::ResolvedTarget Recorder::ResolveTarget(const Options& options) const
{
    int selectorCount = 0;
    selectorCount += options.exeName.has_value() ? 1 : 0;
    selectorCount += options.pid.has_value() ? 1 : 0;
    selectorCount += options.windowTitle.has_value() ? 1 : 0;

    if (selectorCount != 1) {
        utils::Fail(L"Exactly one of --exe, --pid, or --title must be provided.");
    }

    if (options.pid.has_value()) {
        const auto process = ProcessFinder::FindByPid(*options.pid);
        if (!process) {
            utils::Fail(L"Could not find process with PID " + std::to_wstring(*options.pid) + L".");
        }

        const auto window = WindowFinder::FindBestWindowForPid(*options.pid);
        if (!window) {
            utils::Fail(L"Could not find a visible, non-minimized top-level window for PID " + std::to_wstring(*options.pid) + L".");
        }

        return {*process, *window};
    }

    if (options.exeName.has_value()) {
        const auto processes = ProcessFinder::FindByExeName(*options.exeName);
        if (processes.empty()) {
            utils::Fail(L"Could not find a running process named " + *options.exeName + L".");
        }

        std::optional<ResolvedTarget> bestMatch;
        long long bestScore = -1;

        for (const auto& process : processes) {
            const auto window = WindowFinder::FindBestWindowForPid(process.pid);
            if (!window) {
                continue;
            }

            const long long score =
                static_cast<long long>(window->bounds.right - window->bounds.left) *
                static_cast<long long>(window->bounds.bottom - window->bounds.top);

            if (score > bestScore) {
                bestScore = score;
                bestMatch = ResolvedTarget{process, *window};
            }
        }

        if (!bestMatch) {
            utils::Fail(L"Found matching processes for " + *options.exeName + L", but none had a visible, non-minimized top-level window.");
        }

        return *bestMatch;
    }

    const auto windows = WindowFinder::FindWindowsByTitle(*options.windowTitle);
    if (windows.empty()) {
        utils::Fail(L"Could not find a visible top-level window matching title " + *options.windowTitle + L".");
    }

    const auto process = ProcessFinder::FindByPid(windows.front().pid);
    if (!process) {
        utils::Fail(L"Resolved a window for title " + *options.windowTitle + L", but its process no longer exists.");
    }

    return {*process, windows.front()};
}

std::filesystem::path Recorder::ResolveOutputPath(const Options& options) const
{
    auto output = options.outputPath.value_or(utils::DefaultOutputPath(false));
    if (!output.has_extension()) {
        output += L".mp4";
    }

    const auto extension = ExtensionLower(output);
    if (extension != L".mp4") {
        utils::LogWarning(L"Video-only MVP output uses MP4. Replacing requested extension with .mp4 for this commit.");
        output.replace_extension(L".mp4");
    }

    return output;
}

void Recorder::RequestStop(StopReason reason)
{
    bool expected = false;
    if (m_stopRequested.compare_exchange_strong(expected, true)) {
        {
            std::lock_guard<std::mutex> lock(m_stopMutex);
            m_stopReason = reason;
        }
        m_stopCondition.notify_all();
    }
}

void Recorder::StartProcessMonitor()
{
    m_processMonitorThread = std::thread([this] {
        if (m_processHandle == nullptr || m_monitorStopEvent == nullptr) {
            return;
        }

        HANDLE handles[2] = {m_processHandle, m_monitorStopEvent};
        const auto result = WaitForMultipleObjects(2, handles, FALSE, INFINITE);
        if (result == WAIT_OBJECT_0) {
            RequestStop(StopReason::TargetExited);
        }
    });
}

void Recorder::StopProcessMonitor()
{
    if (m_monitorStopEvent != nullptr) {
        SetEvent(m_monitorStopEvent);
    }
    if (m_processMonitorThread.joinable()) {
        m_processMonitorThread.join();
    }
}

}  // namespace corebs