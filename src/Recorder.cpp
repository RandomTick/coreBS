#include "Recorder.h"

#include "SignalHandler.h"
#include "Utils.h"

#include <filesystem>
#include <iomanip>
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

void EnsureParentDirectoryExists(const std::filesystem::path& path)
{
    const auto parent = path.parent_path();
    if (parent.empty()) {
        return;
    }

    std::error_code error;
    std::filesystem::create_directories(parent, error);
    if (error) {
        utils::Fail(L"Failed to create output directory " + parent.wstring() + L": " + utils::Utf8ToWide(error.message()));
    }
}

std::wstring FormatSeconds(double seconds)
{
    std::wstringstream stream;
    stream << std::fixed << std::setprecision(6) << seconds;
    return stream.str();
}

void RemoveFileIfPresent(const std::filesystem::path& path)
{
    std::error_code error;
    std::filesystem::remove(path, error);
}

}  // namespace

int Recorder::Run(const Options& options)
{
    m_options = options;
    m_stopRequested.store(false);
    m_stopReason = StopReason::None;
    m_target = ResolveTarget(options);
    m_outputPlan = ResolveOutputPlan(options);
    m_processHandle = ProcessFinder::OpenProcessForMonitoring(m_target.process.pid);
    m_monitorStopEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (m_monitorStopEvent == nullptr) {
        utils::Fail(L"CreateEventW failed for process monitor shutdown: " + utils::FormatWindowsError(GetLastError()));
    }

    utils::LogInfo(L"Chosen PID: " + std::to_wstring(m_target.process.pid) + L" (" + m_target.process.exeName + L")");
    utils::LogInfo(L"Chosen HWND: " + utils::FormatPointer(m_target.window.hwnd));
    utils::LogInfo(L"Chosen output path: " + m_outputPlan.finalOutputPath.wstring());
    utils::LogInfo(L"Video size: " + utils::FormatSize(
        static_cast<unsigned int>(m_target.window.bounds.right - m_target.window.bounds.left),
        static_cast<unsigned int>(m_target.window.bounds.bottom - m_target.window.bounds.top)));
    utils::LogInfo(L"FPS: " + std::to_wstring(options.fps));
    utils::LogInfo(std::wstring(L"Audio enabled: ") + (options.audioEnabled ? L"yes" : L"no"));
    if (options.verbose) {
        utils::LogVerbose(L"Video intermediate path: " + m_outputPlan.videoPath.wstring());
        if (m_outputPlan.audioPath) {
            utils::LogVerbose(L"Audio intermediate path: " + m_outputPlan.audioPath->wstring());
        }
    }

    const auto baseQpc = utils::QueryPerformanceCounterValue();

    SignalHandler::Install([this] { RequestStop(StopReason::CtrlC); });
    StartProcessMonitor();

    bool videoStarted = false;
    bool audioStarted = false;
    try {
        m_videoCapture.Start(VideoCapture::StartOptions{
            m_target.window.hwnd,
            m_outputPlan.videoPath,
            options.fps,
            options.captureCursor,
            options.verbose,
            baseQpc,
        });
        videoStarted = true;

        if (options.audioEnabled && m_outputPlan.audioPath) {
            m_audioCapture.Start(AudioCapture::StartOptions{
                m_target.process.pid,
                *m_outputPlan.audioPath,
                options.verbose,
                baseQpc,
            });
            audioStarted = true;
        }
    } catch (...) {
        if (audioStarted) {
            m_audioCapture.Stop();
        }
        if (videoStarted) {
            m_videoCapture.Stop();
        }
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

    utils::LogInfo(L"Start reason: target capture started.");

    {
        std::unique_lock<std::mutex> lock(m_stopMutex);
        m_stopCondition.wait(lock, [this] { return m_stopRequested.load(); });
    }

    utils::LogInfo(L"Stop reason: " + StopReasonToString(m_stopReason));

    AudioCapture::Stats audioStats{};
    if (audioStarted) {
        m_audioCapture.Stop();
        audioStats = m_audioCapture.GetStats();
    }
    if (videoStarted) {
        m_videoCapture.Stop();
    }

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

    const auto videoStats = m_videoCapture.GetStats();
    if (options.verbose) {
        utils::LogVerbose(L"Video frames written: " + std::to_wstring(videoStats.writtenFrames));
        utils::LogVerbose(L"Video frames dropped: " + std::to_wstring(videoStats.droppedFrames));
        if (options.audioEnabled) {
            utils::LogVerbose(L"Audio bytes written: " + std::to_wstring(audioStats.bytesWritten));
            utils::LogVerbose(L"Audio discontinuities: " + std::to_wstring(audioStats.discontinuities));
        }
    }

    FinalizeOutputs(baseQpc, audioStats);
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

Recorder::OutputPlan Recorder::ResolveOutputPlan(const Options& options) const
{
    OutputPlan plan;
    plan.finalOutputPath = options.outputPath.value_or(utils::DefaultOutputPath(options.audioEnabled));
    if (!plan.finalOutputPath.has_extension()) {
        plan.finalOutputPath += options.audioEnabled ? L".mkv" : L".mp4";
    }

    EnsureParentDirectoryExists(plan.finalOutputPath);

    const auto extension = ExtensionLower(plan.finalOutputPath);
    if (!options.audioEnabled && extension == L".mp4") {
        plan.videoPath = plan.finalOutputPath;
        return plan;
    }

    plan.videoPath = utils::AppendSuffixToStem(utils::ReplaceExtension(plan.finalOutputPath, L".mp4"), L".video");
    EnsureParentDirectoryExists(plan.videoPath);

    if (options.audioEnabled) {
        plan.audioPath = utils::AppendSuffixToStem(utils::ReplaceExtension(plan.finalOutputPath, L".wav"), L".audio");
        EnsureParentDirectoryExists(*plan.audioPath);
    }

    plan.needsMux = plan.videoPath != plan.finalOutputPath || plan.audioPath.has_value();
    return plan;
}

void Recorder::FinalizeOutputs(int64_t baseQpc, const AudioCapture::Stats& audioStats) const
{
    if (!m_outputPlan.needsMux) {
        utils::LogInfo(L"Finalized output at " + m_outputPlan.finalOutputPath.wstring());
        return;
    }

    if (!utils::ToolExists(L"ffmpeg.exe")) {
        if (m_outputPlan.audioPath) {
            utils::LogWarning(L"FFmpeg was not found. Keeping separate files: " + m_outputPlan.videoPath.wstring() + L" and " + m_outputPlan.audioPath->wstring());
        } else {
            utils::LogWarning(L"FFmpeg was not found. Keeping video-only MP4 at " + m_outputPlan.videoPath.wstring());
        }
        return;
    }

    std::vector<std::wstring> arguments;
    arguments.push_back(L"-y");
    arguments.push_back(L"-i");
    arguments.push_back(m_outputPlan.videoPath.wstring());

    if (m_outputPlan.audioPath) {
        const double audioOffsetSeconds = static_cast<double>(audioStats.startQpc - baseQpc) / static_cast<double>(utils::QueryPerformanceFrequencyValue());
        if (audioOffsetSeconds > 0.0005) {
            arguments.push_back(L"-itsoffset");
            arguments.push_back(FormatSeconds(audioOffsetSeconds));
        }
        arguments.push_back(L"-i");
        arguments.push_back(m_outputPlan.audioPath->wstring());
        arguments.push_back(L"-map");
        arguments.push_back(L"0:v:0");
        arguments.push_back(L"-map");
        arguments.push_back(L"1:a:0");
        arguments.push_back(L"-c:v");
        arguments.push_back(L"copy");
        arguments.push_back(L"-c:a");
        arguments.push_back(L"aac");
        arguments.push_back(L"-b:a");
        arguments.push_back(L"192k");
        arguments.push_back(L"-shortest");
    } else {
        arguments.push_back(L"-c");
        arguments.push_back(L"copy");
    }

    if (ExtensionLower(m_outputPlan.finalOutputPath) == L".mp4") {
        arguments.push_back(L"-movflags");
        arguments.push_back(L"+faststart");
    }

    arguments.push_back(m_outputPlan.finalOutputPath.wstring());

    const auto exitCode = utils::RunProcess(L"ffmpeg.exe", arguments, m_options.verbose);
    if (exitCode != 0) {
        utils::LogWarning(L"FFmpeg muxing failed with exit code " + std::to_wstring(exitCode) + L". Keeping intermediate files.");
        return;
    }

    RemoveFileIfPresent(m_outputPlan.videoPath);
    if (m_outputPlan.audioPath) {
        RemoveFileIfPresent(*m_outputPlan.audioPath);
    }

    utils::LogInfo(L"Finalized output at " + m_outputPlan.finalOutputPath.wstring());
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