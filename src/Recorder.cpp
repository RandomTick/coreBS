#include "Recorder.h"

#include "SignalHandler.h"
#include "Utils.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <thread>
#include <vector>

namespace corebs {

namespace {

constexpr uint32_t kCanvasWidth = 1920;
constexpr uint32_t kCanvasHeight = 1080;
constexpr auto kPollInterval = std::chrono::milliseconds(500);
constexpr auto kIdleSleep = std::chrono::milliseconds(2);
constexpr uint32_t kGameRegionX = 0;
constexpr uint32_t kGameRegionY = 0;
constexpr uint32_t kGameRegionWidth = 1500;
constexpr uint32_t kGameRegionHeight = kCanvasHeight;
constexpr uint32_t kLiveSplitRegionX = 1500;
constexpr uint32_t kLiveSplitRegionY = 0;
constexpr uint32_t kLiveSplitRegionWidth = 420;
constexpr uint32_t kLiveSplitRegionHeight = kCanvasHeight;

std::wstring StopReasonToString(Recorder::StopReason reason)
{
    switch (reason) {
    case Recorder::StopReason::CtrlC:
        return L"user interrupted with Ctrl+C";
    case Recorder::StopReason::TargetExited:
        return L"target exited";
    case Recorder::StopReason::WindowLost:
        return L"target window became unavailable";
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

void RemoveFileIfPresent(const std::filesystem::path& path)
{
    if (path.empty()) {
        return;
    }

    constexpr int kMaxAttempts = 10;
    for (int attempt = 0; attempt < kMaxAttempts; ++attempt) {
        std::error_code removeError;
        const bool removed = std::filesystem::remove(path, removeError);
        if (removed || !std::filesystem::exists(path)) {
            return;
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    if (std::filesystem::exists(path)) {
        utils::LogWarning(L"Failed to delete temporary file: " + path.wstring());
    }
}

bool ContainsWindow(const std::vector<WindowInfo>& windows, HWND hwnd)
{
    return std::any_of(windows.begin(), windows.end(), [hwnd](const WindowInfo& window) {
        return window.hwnd == hwnd;
    });
}

std::wstring FormatSeconds(double seconds)
{
    std::wstringstream stream;
    stream << std::fixed << std::setprecision(3) << seconds;
    return stream.str();
}

uint64_t AlignDown(uint64_t value, uint16_t alignment)
{
    if (alignment == 0) {
        return value;
    }
    return value - (value % alignment);
}

RECT ComputeAnchoredLayoutRect(
    uint32_t sourceWidth,
    uint32_t sourceHeight,
    uint32_t originX,
    uint32_t originY,
    uint32_t maxWidth,
    uint32_t maxHeight,
    bool allowUpscale)
{
    RECT result{};
    result.left = static_cast<LONG>(originX);
    result.top = static_cast<LONG>(originY);
    result.right = static_cast<LONG>(originX);
    result.bottom = static_cast<LONG>(originY);

    if (sourceWidth == 0 || sourceHeight == 0 || maxWidth == 0 || maxHeight == 0) {
        return result;
    }

    const double widthScale = static_cast<double>(maxWidth) / static_cast<double>(sourceWidth);
    const double heightScale = static_cast<double>(maxHeight) / static_cast<double>(sourceHeight);
    const double scale = allowUpscale ? std::min(widthScale, heightScale) : std::min(1.0, std::min(widthScale, heightScale));

    const LONG drawWidth = std::max<LONG>(1, static_cast<LONG>(sourceWidth * scale));
    const LONG drawHeight = std::max<LONG>(1, static_cast<LONG>(sourceHeight * scale));
    result.right = result.left + drawWidth;
    result.bottom = result.top + drawHeight;
    return result;
}

void FlipCanvasVertically(std::vector<std::byte>& canvas, uint32_t width, uint32_t height)
{
    const size_t rowBytes = static_cast<size_t>(width) * 4ULL;
    std::vector<std::byte> swapRow(rowBytes);

    for (uint32_t y = 0; y < height / 2; ++y) {
        auto* topRow = canvas.data() + static_cast<size_t>(y) * rowBytes;
        auto* bottomRow = canvas.data() + static_cast<size_t>(height - 1 - y) * rowBytes;
        memcpy(swapRow.data(), topRow, rowBytes);
        memcpy(topRow, bottomRow, rowBytes);
        memcpy(bottomRow, swapRow.data(), rowBytes);
    }
}
void BlitSnapshot(
    const WindowCaptureSource::Snapshot& snapshot,
    std::vector<std::byte>& canvas,
    uint32_t canvasWidth,
    uint32_t originX,
    uint32_t originY,
    uint32_t maxWidth,
    uint32_t maxHeight,
    bool allowUpscale)
{
    if (!snapshot.hasFrame || snapshot.width == 0 || snapshot.height == 0 || snapshot.pixels.empty()) {
        return;
    }

    const RECT target = ComputeAnchoredLayoutRect(snapshot.width, snapshot.height, originX, originY, maxWidth, maxHeight, allowUpscale);
    const uint32_t targetWidth = static_cast<uint32_t>(std::max<LONG>(1, target.right - target.left));
    const uint32_t targetHeight = static_cast<uint32_t>(std::max<LONG>(1, target.bottom - target.top));

    for (uint32_t y = 0; y < targetHeight; ++y) {
        const uint32_t sourceY = std::min<uint32_t>(snapshot.height - 1, static_cast<uint32_t>((static_cast<uint64_t>(y) * snapshot.height) / targetHeight));
        const auto* sourceRow = snapshot.pixels.data() + static_cast<size_t>(sourceY) * static_cast<size_t>(snapshot.width) * 4ULL;
        auto* destinationRow = canvas.data() +
            (static_cast<size_t>(target.top + static_cast<LONG>(y)) * static_cast<size_t>(canvasWidth) + static_cast<size_t>(target.left)) * 4ULL;

        for (uint32_t x = 0; x < targetWidth; ++x) {
            const uint32_t sourceX = std::min<uint32_t>(snapshot.width - 1, static_cast<uint32_t>((static_cast<uint64_t>(x) * snapshot.width) / targetWidth));
            const auto* sourcePixel = sourceRow + static_cast<size_t>(sourceX) * 4ULL;
            auto* destinationPixel = destinationRow + static_cast<size_t>(x) * 4ULL;
            memcpy(destinationPixel, sourcePixel, 4ULL);
        }
    }
}

}  // namespace

class SessionAudioWriter {
public:
    explicit SessionAudioWriter(std::filesystem::path outputPath)
        : m_outputPath(std::move(outputPath))
    {
    }

    void Initialize(const WAVEFORMATEX& format)
    {
        std::lock_guard<std::mutex> lock(m_mutex);

        const size_t formatSize = (format.wFormatTag == WAVE_FORMAT_PCM)
            ? static_cast<size_t>(sizeof(PCMWAVEFORMAT))
            : static_cast<size_t>(sizeof(WAVEFORMATEX) + format.cbSize);

        if (m_initialized) {
            if (m_formatBlob.size() != formatSize || memcmp(m_formatBlob.data(), &format, formatSize) != 0) {
                m_formatMismatch = true;
            }
            return;
        }

        EnsureParentDirectoryExists(m_outputPath);
        m_stream.open(m_outputPath, std::ios::binary | std::ios::trunc);
        if (!m_stream.is_open()) {
            utils::Fail(L"Failed to open audio temp file: " + m_outputPath.wstring());
        }

        m_formatBlob.resize(formatSize);
        memcpy(m_formatBlob.data(), &format, formatSize);
        m_blockAlign = format.nBlockAlign;
        m_bytesPerSecond = format.nAvgBytesPerSec;
        if (m_blockAlign == 0 || m_bytesPerSecond == 0) {
            utils::Fail(L"Audio mix format is invalid for WAV output.");
        }

        WriteHeaderLocked();
        m_initialized = true;
    }

    void BeginSpan(uint64_t relativeStartHns)
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (!m_initialized || m_formatMismatch) {
            return;
        }

        WriteSilenceUntilLocked(relativeStartHns);
        m_spanActive = true;
    }

    void AppendChunk(const BYTE* data, size_t sizeBytes, uint64_t relativePositionHns)
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (!m_initialized || m_formatMismatch || !m_spanActive || sizeBytes == 0) {
            return;
        }

        size_t alignedBytes = sizeBytes;
        if (m_blockAlign != 0) {
            alignedBytes -= alignedBytes % m_blockAlign;
        }
        if (alignedBytes == 0) {
            return;
        }

        WriteSilenceUntilLocked(relativePositionHns);
        WriteBytesLocked(reinterpret_cast<const char*>(data), alignedBytes);
    }

    void EndSpan()
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_spanActive = false;
    }

    void Finalize(uint64_t stopHns)
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (!m_initialized) {
            return;
        }

        m_spanActive = false;
        WriteSilenceUntilLocked(stopHns);
        RewriteHeaderLocked();
        m_stream.close();
    }

    bool HasOutput() const
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_initialized && m_dataBytes > 0;
    }

    bool HasFormatMismatch() const
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_formatMismatch;
    }

private:
    void WriteHeaderLocked()
    {
        const uint32_t formatSize = static_cast<uint32_t>(m_formatBlob.size());
        const uint32_t riffSize = 4U + 8U + formatSize + 8U;
        m_stream.write("RIFF", 4);
        m_stream.write(reinterpret_cast<const char*>(&riffSize), sizeof(riffSize));
        m_stream.write("WAVE", 4);
        m_stream.write("fmt ", 4);
        m_stream.write(reinterpret_cast<const char*>(&formatSize), sizeof(formatSize));
        m_stream.write(reinterpret_cast<const char*>(m_formatBlob.data()), static_cast<std::streamsize>(m_formatBlob.size()));
        m_stream.write("data", 4);
        const uint32_t zero = 0;
        m_stream.write(reinterpret_cast<const char*>(&zero), sizeof(zero));
    }

    void RewriteHeaderLocked()
    {
        const uint32_t formatSize = static_cast<uint32_t>(m_formatBlob.size());
        const uint32_t dataSize = static_cast<uint32_t>(std::min<uint64_t>(m_dataBytes, 0xFFFFFFFFULL));
        const uint32_t riffSize = 4U + 8U + formatSize + 8U + dataSize;

        m_stream.seekp(4, std::ios::beg);
        m_stream.write(reinterpret_cast<const char*>(&riffSize), sizeof(riffSize));
        m_stream.seekp(24 + formatSize, std::ios::beg);
        m_stream.write(reinterpret_cast<const char*>(&dataSize), sizeof(dataSize));
        m_stream.flush();
    }

    void WriteSilenceUntilLocked(uint64_t offsetHns)
    {
        const uint64_t targetBytes = AlignDown((offsetHns * static_cast<uint64_t>(m_bytesPerSecond)) / 10000000ULL, m_blockAlign);
        if (targetBytes <= m_dataBytes) {
            return;
        }

        std::array<char, 4096> zeros{};
        uint64_t remaining = targetBytes - m_dataBytes;
        while (remaining > 0) {
            size_t chunkBytes = static_cast<size_t>(std::min<uint64_t>(remaining, zeros.size()));
            if (m_blockAlign != 0) {
                chunkBytes -= chunkBytes % m_blockAlign;
            }
            if (chunkBytes == 0) {
                chunkBytes = static_cast<size_t>(std::min<uint64_t>(remaining, static_cast<uint64_t>(m_blockAlign)));
            }
            WriteBytesLocked(zeros.data(), chunkBytes);
            remaining -= chunkBytes;
        }
    }

    void WriteBytesLocked(const char* data, size_t sizeBytes)
    {
        m_stream.write(data, static_cast<std::streamsize>(sizeBytes));
        if (!m_stream.good()) {
            utils::Fail(L"Failed while writing audio temp file: " + m_outputPath.wstring());
        }
        m_dataBytes += sizeBytes;
    }

    std::filesystem::path m_outputPath;
    mutable std::mutex m_mutex;
    std::ofstream m_stream;
    std::vector<std::byte> m_formatBlob;
    uint16_t m_blockAlign = 0;
    uint32_t m_bytesPerSecond = 0;
    uint64_t m_dataBytes = 0;
    bool m_initialized = false;
    bool m_formatMismatch = false;
    bool m_spanActive = false;
};

Recorder::Recorder() = default;


Recorder::~Recorder() = default;

int Recorder::Run(const Options& options)
{
    m_options = options;
    ValidateOptions();

    bool sawMatchingProcess = false;
    if (!m_options.waitForTarget) {
        const auto initialTarget = TryResolveCurrentTarget(sawMatchingProcess);
        if (!initialTarget) {
            if (m_options.pid) {
                utils::Fail(L"Target PID " + std::to_wstring(*m_options.pid) + L" is not available for capture.");
            }
            if (sawMatchingProcess) {
                utils::Fail(L"Target process was found, but no visible non-minimized top-level window is captureable yet.");
            }
            utils::Fail(L"Target is not available and --no-wait was specified.");
        }
    }

    m_sessionOutputPlan = ResolveSessionOutputPlan(options);
    m_state = State::Stopped;
    m_lastStateMessage.clear();
    m_stopRequested.store(false);
    m_stopReason = StopReason::None;
    m_sessionStartQpc = utils::QueryPerformanceCounterValue();
    m_stopRequestedQpc = 0;
    m_frameIntervalQpc = std::max<int64_t>(1, utils::QueryPerformanceFrequencyValue() / std::max(1, options.fps));
    m_nextFrameQpc = m_sessionStartQpc;
    m_lastGamePollQpc = 0;
    m_lastLiveSplitPollQpc = 0;
    m_sessionStarted = false;
    m_hasEverAttachedGame = false;
    m_liveSplitWaitingLogged = false;
    m_currentAudioAttachHns = 0;
    m_activeTarget.reset();
    m_liveSplitWindow.reset();
    ReleaseActiveGameResources();
    ReleaseLiveSplitResources();
    m_audioWriter = std::make_unique<SessionAudioWriter>(m_sessionOutputPlan.tempAudioPath);

    utils::LogInfo(L"Chosen output path: " + m_sessionOutputPlan.finalOutputPath.wstring());
    utils::LogInfo(L"Session canvas: " + utils::FormatSize(kCanvasWidth, kCanvasHeight));
    utils::LogInfo(L"FPS: " + std::to_wstring(options.fps));
    utils::LogInfo(std::wstring(L"Audio enabled: ") + (options.audioEnabled ? L"yes" : L"no"));
    utils::LogInfo(std::wstring(L"Wait mode: ") + (options.waitForTarget ? L"enabled" : L"disabled"));
    utils::LogInfo(L"LiveSplit title match: " + m_options.liveSplitTitle);

    SignalHandler::Install([this] { RequestStop(StopReason::CtrlC); });

    try {
        StartSession();

        while (!m_stopRequested.load()) {
            const int64_t nowQpc = utils::QueryPerformanceCounterValue();
            UpdateLiveSplitCapture(nowQpc);
            UpdateGameCapture(nowQpc);
            WritePendingFrames(nowQpc);

            if (m_stopRequested.load()) {
                break;
            }

            const int64_t sleepUntilQpc = std::min(m_nextFrameQpc, nowQpc + utils::SecondsToQpcTicks(0.02));
            const int64_t remainingQpc = sleepUntilQpc - utils::QueryPerformanceCounterValue();
            if (remainingQpc > 0) {
                const auto sleepMillis = std::chrono::milliseconds(std::max<int64_t>(1, (remainingQpc * 1000LL) / utils::QueryPerformanceFrequencyValue()));
                SleepWithStopCheck(std::min(kIdleSleep, sleepMillis));
            } else {
                std::this_thread::yield();
            }
        }

        const int64_t stopQpc = m_stopRequestedQpc != 0 ? m_stopRequestedQpc : utils::QueryPerformanceCounterValue();
        if (m_stopReason == StopReason::CtrlC) {
            utils::LogInfo(L"User interrupted with Ctrl+C.");
        }
        FinalizeSession(stopQpc);
    } catch (...) {
        SignalHandler::Uninstall();
        ReleaseActiveGameResources();
        ReleaseLiveSplitResources();
        if (m_sessionStarted) {
            m_videoCapture.Stop();
        }
        throw;
    }

    SignalHandler::Uninstall();
    TransitionTo(State::Stopped, L"Final shutdown complete.");
    return 0;
}

void Recorder::ValidateOptions() const
{
    int selectorCount = 0;
    selectorCount += m_options.exeName.has_value() ? 1 : 0;
    selectorCount += m_options.pid.has_value() ? 1 : 0;
    selectorCount += m_options.windowTitle.has_value() ? 1 : 0;

    if (selectorCount != 1) {
        utils::Fail(L"Exactly one of --exe, --pid, or --title must be provided.");
    }

    if (m_options.pid && m_options.waitForTarget) {
        utils::LogWarning(L"--pid identifies only the current process instance. Automatic reattach is intended for --exe or --title targets.");
    }
}

bool Recorder::UsesStableTargetIdentity() const
{
    return m_options.exeName.has_value() || m_options.windowTitle.has_value();
}

std::optional<Recorder::ResolvedTarget> Recorder::TryResolveCurrentTarget(bool& sawMatchingProcess) const
{
    sawMatchingProcess = false;

    if (m_options.pid) {
        const auto process = ProcessFinder::FindByPid(*m_options.pid);
        if (!process) {
            return std::nullopt;
        }

        sawMatchingProcess = true;
        const auto window = WindowFinder::FindBestWindowForPid(*m_options.pid);
        if (!window) {
            return std::nullopt;
        }

        return ResolvedTarget{*process, *window};
    }

    if (m_options.exeName) {
        const auto processes = ProcessFinder::FindByExeName(*m_options.exeName);
        if (processes.empty()) {
            return std::nullopt;
        }

        sawMatchingProcess = true;
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

        return bestMatch;
    }

    const auto windows = WindowFinder::FindWindowsByTitle(*m_options.windowTitle);
    if (windows.empty()) {
        return std::nullopt;
    }

    const auto process = ProcessFinder::FindByPid(windows.front().pid);
    if (!process) {
        return std::nullopt;
    }

    sawMatchingProcess = true;
    return ResolvedTarget{*process, windows.front()};
}

Recorder::SessionOutputPlan Recorder::ResolveSessionOutputPlan(const Options& options) const
{
    SessionOutputPlan plan;
    plan.finalOutputPath = options.outputPath.value_or(utils::DefaultOutputPath(options.audioEnabled));
    if (!plan.finalOutputPath.has_extension()) {
        plan.finalOutputPath += options.audioEnabled ? L".mkv" : L".mp4";
    }

    const auto finalVideoPath = utils::ReplaceExtension(plan.finalOutputPath, L".mp4");
    plan.tempVideoPath = utils::AppendSuffixToStem(finalVideoPath, L".corebs_tmp_video");
    plan.tempAudioPath = utils::AppendSuffixToStem(utils::ReplaceExtension(plan.finalOutputPath, L".wav"), L".corebs_tmp_audio");
    EnsureParentDirectoryExists(plan.finalOutputPath);
    EnsureParentDirectoryExists(plan.tempVideoPath);
    EnsureParentDirectoryExists(plan.tempAudioPath);
    return plan;
}

void Recorder::StartSession()
{
    if (m_sessionStarted) {
        return;
    }

    m_videoCapture.Start(VideoCapture::StartOptions{
        m_sessionOutputPlan.tempVideoPath,
        kCanvasWidth,
        kCanvasHeight,
        m_options.fps,
        m_sessionStartQpc,
    });
    m_sessionStarted = true;
    utils::LogInfo(L"Session recording started.");
}

void Recorder::FinalizeSession(int64_t stopQpc)
{
    TransitionTo(State::Finalizing, L"Finalizing session output.");

    WritePendingFrames(stopQpc);
    if (m_nextFrameQpc <= stopQpc) {
        WriteFrameAt(stopQpc);
        m_nextFrameQpc = stopQpc + m_frameIntervalQpc;
    }

    ReleaseActiveGameResources();
    ReleaseLiveSplitResources();

    if (m_audioWriter) {
        m_audioWriter->Finalize(utils::QpcTicksToHundredsOfNanoseconds(stopQpc - m_sessionStartQpc));
    }

    if (m_sessionStarted) {
        m_videoCapture.Stop();
        m_sessionStarted = false;
    }

    const bool hasAudioOutput = m_options.audioEnabled && m_audioWriter && m_audioWriter->HasOutput();
    if (m_audioWriter && m_audioWriter->HasFormatMismatch()) {
        utils::LogWarning(L"Audio format changed between game instances. Additional audio after the format change was ignored.");
    }

    RemoveFileIfPresent(m_sessionOutputPlan.finalOutputPath);

    const auto finalExtension = ExtensionLower(m_sessionOutputPlan.finalOutputPath);
    if (!hasAudioOutput) {
        if (finalExtension == L".mp4") {
            std::error_code renameError;
            std::filesystem::rename(m_sessionOutputPlan.tempVideoPath, m_sessionOutputPlan.finalOutputPath, renameError);
            if (renameError) {
                utils::Fail(L"Failed to finalize output: " + utils::Utf8ToWide(renameError.message()));
            }
            utils::LogInfo(L"Finalized output at " + m_sessionOutputPlan.finalOutputPath.wstring());
            return;
        }

        if (!utils::ToolExists(L"ffmpeg.exe")) {
            utils::LogWarning(L"FFmpeg was not found. Keeping the temporary MP4 because the requested container needs a remux.");
            utils::LogWarning(L"Temporary video: " + m_sessionOutputPlan.tempVideoPath.wstring());
            return;
        }

        std::vector<std::wstring> arguments{
            L"-y",
            L"-i", m_sessionOutputPlan.tempVideoPath.wstring(),
            L"-map", L"0:v:0",
            L"-c", L"copy",
            m_sessionOutputPlan.finalOutputPath.wstring(),
        };

        const auto exitCode = utils::RunProcess(L"ffmpeg.exe", arguments, m_options.verbose);
        if (exitCode != 0) {
            utils::LogWarning(L"FFmpeg failed while remuxing the final video. Keeping the temporary MP4 instead.");
            utils::LogWarning(L"Temporary video: " + m_sessionOutputPlan.tempVideoPath.wstring());
            return;
        }

        RemoveFileIfPresent(m_sessionOutputPlan.tempVideoPath);
        utils::LogInfo(L"Finalized output at " + m_sessionOutputPlan.finalOutputPath.wstring());
        return;
    }

    if (!utils::ToolExists(L"ffmpeg.exe")) {
        utils::LogWarning(L"FFmpeg was not found. Keeping separate session files instead of producing one muxed output.");
        utils::LogWarning(L"Temporary video: " + m_sessionOutputPlan.tempVideoPath.wstring());
        utils::LogWarning(L"Temporary audio: " + m_sessionOutputPlan.tempAudioPath.wstring());
        return;
    }

    std::vector<std::wstring> arguments;
    arguments.push_back(L"-y");
    arguments.push_back(L"-i");
    arguments.push_back(m_sessionOutputPlan.tempVideoPath.wstring());
    if (m_options.audioSyncMs != 0) {
        arguments.push_back(L"-itsoffset");
        arguments.push_back(FormatSeconds(-static_cast<double>(m_options.audioSyncMs) / 1000.0));
    }
    arguments.push_back(L"-i");
    arguments.push_back(m_sessionOutputPlan.tempAudioPath.wstring());
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
    if (finalExtension == L".mp4") {
        arguments.push_back(L"-movflags");
        arguments.push_back(L"+faststart");
    }

    arguments.push_back(m_sessionOutputPlan.finalOutputPath.wstring());

    const auto exitCode = utils::RunProcess(L"ffmpeg.exe", arguments, m_options.verbose);
    if (exitCode != 0) {
        utils::LogWarning(L"FFmpeg failed while building the final session file. Keeping the temporary audio/video files for recovery.");
        utils::LogWarning(L"Temporary video: " + m_sessionOutputPlan.tempVideoPath.wstring());
        utils::LogWarning(L"Temporary audio: " + m_sessionOutputPlan.tempAudioPath.wstring());
        return;
    }

    RemoveFileIfPresent(m_sessionOutputPlan.tempVideoPath);
    RemoveFileIfPresent(m_sessionOutputPlan.tempAudioPath);
    utils::LogInfo(L"Finalized output at " + m_sessionOutputPlan.finalOutputPath.wstring());
}

void Recorder::TransitionTo(State state, const std::wstring& message)
{
    if (m_state == state && m_lastStateMessage == message) {
        return;
    }

    m_state = state;
    m_lastStateMessage = message;
    if (!message.empty()) {
        utils::LogInfo(message);
    }
}

void Recorder::RequestStop(StopReason reason)
{
    if (!m_stopRequested.exchange(true)) {
        m_stopReason = reason;
        m_stopRequestedQpc = utils::QueryPerformanceCounterValue();
    }
}

void Recorder::SleepWithStopCheck(std::chrono::milliseconds duration) const
{
    constexpr auto kSlice = std::chrono::milliseconds(10);
    auto remaining = duration;
    while (remaining.count() > 0 && !m_stopRequested.load()) {
        const auto slice = remaining > kSlice ? kSlice : remaining;
        std::this_thread::sleep_for(slice);
        remaining -= slice;
    }
}

void Recorder::UpdateGameCapture(int64_t nowQpc)
{
    if (m_activeTarget) {
        if (m_activeProcessHandle != nullptr && WaitForSingleObject(m_activeProcessHandle, 0) == WAIT_OBJECT_0) {
            utils::LogInfo(L"Target exited.");
            DetachGameTarget(StopReason::TargetExited, UsesStableTargetIdentity());
            return;
        }

        const auto windows = WindowFinder::FindWindowsForPid(m_activeTarget->process.pid);
        if (windows.empty()) {
            DetachGameTarget(StopReason::WindowLost, true);
            return;
        }

        if (!ContainsWindow(windows, m_activeTarget->window.hwnd)) {
            const auto process = m_activeTarget->process;
            const auto replacementWindow = windows.front();
            utils::LogInfo(L"Active target window disappeared. Reattaching to the new HWND.");
            DetachGameTarget(StopReason::WindowLost, true);
            try {
                AttachGameTarget(ResolvedTarget{process, replacementWindow}, true);
            } catch (const std::exception& ex) {
                utils::LogWarning(L"Reattach attempt failed. Returning to wait mode: " + utils::Utf8ToWide(ex.what()));
            }
            return;
        }

        if (!m_gameCapture.IsRunning()) {
            utils::LogWarning(L"Game capture source stopped unexpectedly. Waiting to reattach.");
            DetachGameTarget(StopReason::WindowLost, true);
        }
        return;
    }

    if (m_lastGamePollQpc != 0 && nowQpc - m_lastGamePollQpc < utils::SecondsToQpcTicks(kPollInterval.count() / 1000.0)) {
        return;
    }
    m_lastGamePollQpc = nowQpc;

    bool sawMatchingProcess = false;
    const auto resolvedTarget = TryResolveCurrentTarget(sawMatchingProcess);
    if (resolvedTarget) {
        try {
            AttachGameTarget(*resolvedTarget, m_hasEverAttachedGame);
        } catch (const std::exception& ex) {
            utils::LogWarning(L"Found a matching game window, but attach failed. Will keep waiting: " + utils::Utf8ToWide(ex.what()));
            if (m_options.exeName) {
                TransitionTo(State::WaitingForTarget, L"Waiting for target process: " + *m_options.exeName);
            } else if (m_options.windowTitle) {
                TransitionTo(State::WaitingForWindow, L"Waiting for target window title: " + *m_options.windowTitle);
            } else if (m_options.pid) {
                TransitionTo(State::WaitingForTarget, L"Waiting for target PID: " + std::to_wstring(*m_options.pid));
            }
        }
        return;
    }

    if (m_options.pid) {
        TransitionTo(State::WaitingForTarget, L"Waiting for target PID: " + std::to_wstring(*m_options.pid));
    } else if (sawMatchingProcess) {
        TransitionTo(State::WaitingForWindow, L"Process found. Waiting for a visible top-level window.");
    } else if (m_options.exeName) {
        TransitionTo(State::WaitingForTarget, L"Waiting for target process: " + *m_options.exeName);
    } else {
        TransitionTo(State::WaitingForWindow, L"Waiting for target window title: " + *m_options.windowTitle);
    }
}

void Recorder::UpdateLiveSplitCapture(int64_t nowQpc)
{
    if (m_options.liveSplitTitle.empty()) {
        return;
    }

    if (m_lastLiveSplitPollQpc != 0 && nowQpc - m_lastLiveSplitPollQpc < utils::SecondsToQpcTicks(kPollInterval.count() / 1000.0)) {
        return;
    }
    m_lastLiveSplitPollQpc = nowQpc;

    const auto matches = WindowFinder::FindWindowsByTitle(m_options.liveSplitTitle);
    if (matches.empty()) {
        if (m_liveSplitWindow) {
            utils::LogInfo(L"LiveSplit window unavailable. Returning its panel to black.");
            ReleaseLiveSplitResources();
        } else if (!m_liveSplitWaitingLogged) {
            utils::LogInfo(L"Waiting for LiveSplit window: " + m_options.liveSplitTitle);
            m_liveSplitWaitingLogged = true;
        }
        return;
    }

    if (m_liveSplitWindow && ContainsWindow(matches, m_liveSplitWindow->hwnd) && m_liveSplitCapture.IsRunning()) {
        m_liveSplitWaitingLogged = false;
        return;
    }

    const auto& bestWindow = matches.front();
    m_liveSplitWaitingLogged = false;

    ReleaseLiveSplitResources();
    utils::LogInfo(L"LiveSplit window found: HWND " + utils::FormatPointer(bestWindow.hwnd));
    try {
        m_liveSplitCapture.Start(WindowCaptureSource::StartOptions{bestWindow.hwnd, false, false});
        m_liveSplitWindow = bestWindow;
        utils::LogInfo(L"Attached LiveSplit panel to HWND=" + utils::FormatPointer(bestWindow.hwnd));
    } catch (const std::exception& ex) {
        utils::LogWarning(L"Found a matching LiveSplit window, but attach failed. Will keep waiting: " + utils::Utf8ToWide(ex.what()));
    }
}

void Recorder::AttachGameTarget(const ResolvedTarget& target, bool isReattach)
{
    const int64_t attachQpc = utils::QueryPerformanceCounterValue();
    m_currentAudioAttachHns = static_cast<uint64_t>(utils::QpcTicksToHundredsOfNanoseconds(std::max<int64_t>(0, attachQpc - m_sessionStartQpc)));

    utils::LogInfo(L"Process found: PID " + std::to_wstring(target.process.pid) + L" (" + target.process.exeName + L")");
    utils::LogInfo(L"Window found: HWND " + utils::FormatPointer(target.window.hwnd));

    try {
        m_activeProcessHandle = ProcessFinder::OpenProcessForMonitoring(target.process.pid);
        m_gameCapture.Start(WindowCaptureSource::StartOptions{target.window.hwnd, m_options.captureCursor, true});

        if (m_options.audioEnabled) {
            m_audioCapture.Start(AudioCapture::StartOptions{
                target.process.pid,
                m_options.verbose,
                m_sessionStartQpc,
                attachQpc,
                [this](const WAVEFORMATEX& format) { OnAudioFormat(format); },
                [this](const BYTE* data, size_t sizeBytes, uint64_t qpcPositionHns, bool discontinuity) {
                    OnAudioChunk(data, sizeBytes, qpcPositionHns, discontinuity);
                },
            });
            m_gameAudioStarted = true;
        }

        m_activeTarget = target;
        m_hasEverAttachedGame = true;
        const auto attachMessage = (isReattach ? L"Reattached to new PID / HWND. " : L"Attached to PID / HWND. ") +
            std::wstring(L"PID=") + std::to_wstring(target.process.pid) + L", HWND=" + utils::FormatPointer(target.window.hwnd);
        utils::LogInfo(attachMessage);
        TransitionTo(State::Recording, L"Recording continues with the target attached.");
    } catch (...) {
        ReleaseActiveGameResources();
        throw;
    }
}

void Recorder::DetachGameTarget(StopReason reason, bool keepWaiting)
{
    ReleaseActiveGameResources();

    if (reason == StopReason::TargetExited) {
        utils::LogInfo(L"Returned to waiting mode after target exit.");
    } else if (reason == StopReason::WindowLost) {
        utils::LogInfo(L"Target window is unavailable. Returning to waiting mode.");
    } else {
        utils::LogInfo(L"Stopped active target capture: " + StopReasonToString(reason));
    }

    if (keepWaiting) {
        if (m_options.exeName) {
            TransitionTo(State::WaitingForTarget, L"Waiting for target process: " + *m_options.exeName);
        } else if (m_options.windowTitle) {
            TransitionTo(State::WaitingForWindow, L"Waiting for target window title: " + *m_options.windowTitle);
        } else if (m_options.pid) {
            TransitionTo(State::WaitingForTarget, L"Waiting for target PID: " + std::to_wstring(*m_options.pid));
        }
    }
}

void Recorder::ReleaseActiveGameResources()
{
    if (m_gameAudioStarted) {
        m_audioCapture.Stop();
        m_gameAudioStarted = false;
    }
    if (m_gameCapture.IsRunning()) {
        m_gameCapture.Stop();
    }
    if (m_activeProcessHandle != nullptr) {
        CloseHandle(m_activeProcessHandle);
        m_activeProcessHandle = nullptr;
    }
    m_currentAudioAttachHns = 0;
    m_activeTarget.reset();
}

void Recorder::ReleaseLiveSplitResources()
{
    if (m_liveSplitCapture.IsRunning()) {
        m_liveSplitCapture.Stop();
    }
    m_liveSplitWindow.reset();
}

void Recorder::OnAudioFormat(const WAVEFORMATEX& format)
{
    if (!m_audioWriter) {
        return;
    }

    m_audioWriter->Initialize(format);
    m_audioWriter->BeginSpan(m_currentAudioAttachHns);
    if (m_audioWriter->HasFormatMismatch()) {
        utils::LogWarning(L"A later game instance reported a different audio format. Additional audio will be ignored for this session.");
    }
}

void Recorder::OnAudioChunk(const BYTE* data, size_t sizeBytes, uint64_t qpcPositionHns, bool discontinuity)
{
    if (discontinuity && m_options.verbose) {
        utils::LogVerbose(L"Audio discontinuity detected.");
    }
    if (m_audioWriter) {
        m_audioWriter->AppendChunk(data, sizeBytes, qpcPositionHns);
    }
}

void Recorder::WritePendingFrames(int64_t nowQpc)
{
    if (m_frameIntervalQpc <= 0) {
        return;
    }

    constexpr uint64_t kMaxCatchUpFrames = 3;
    const int64_t maxLagQpc = m_frameIntervalQpc * static_cast<int64_t>(kMaxCatchUpFrames);
    if (nowQpc - m_nextFrameQpc > maxLagQpc) {
        const uint64_t droppedFrames = static_cast<uint64_t>((nowQpc - m_nextFrameQpc) / m_frameIntervalQpc);
        if (droppedFrames > 0) {
            m_droppedFrameCount += droppedFrames;
            if (m_options.verbose) {
                utils::LogVerbose(L"Dropping " + std::to_wstring(droppedFrames) + L" overdue video frames to keep the recorder responsive.");
            }
        }
        m_nextFrameQpc = nowQpc;
    }

    uint64_t framesWritten = 0;
    while (m_nextFrameQpc <= nowQpc && !m_stopRequested.load() && framesWritten < kMaxCatchUpFrames) {
        WriteFrameAt(m_nextFrameQpc);
        m_nextFrameQpc += m_frameIntervalQpc;
        ++framesWritten;
    }
}

void Recorder::WriteFrameAt(int64_t frameQpc)
{
    std::vector<std::byte> canvas(static_cast<size_t>(kCanvasWidth) * static_cast<size_t>(kCanvasHeight) * 4ULL);

    const auto gameSnapshot = m_gameCapture.GetLatestSnapshot();
    const auto liveSplitSnapshot = m_liveSplitCapture.GetLatestSnapshot();
    BlitSnapshot(gameSnapshot, canvas, kCanvasWidth, kGameRegionX, kGameRegionY, kGameRegionWidth, kGameRegionHeight, true);
    BlitSnapshot(liveSplitSnapshot, canvas, kCanvasWidth, kLiveSplitRegionX, kLiveSplitRegionY, kLiveSplitRegionWidth, kLiveSplitRegionHeight, false);

    FlipCanvasVertically(canvas, kCanvasWidth, kCanvasHeight);
    m_videoCapture.WriteFrame(canvas, frameQpc);
}

}  // namespace corebs
