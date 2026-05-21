#include "game-capture-source.h"

#include "../../src/Utils.h"

#include <graphics/graphics.h>

#include <algorithm>
#include <cstdint>
#include <limits>
#include <string_view>

namespace corebs::obs_plugin {

namespace {

constexpr double kPollIntervalSeconds = 0.5;
constexpr double kFirstFrameWarningSeconds = 2.0;
constexpr const char* kModeExe = "exe";
constexpr const char* kModeTitle = "title";
constexpr const char* kModePid = "pid";

void LogObs(int level, std::wstring_view message)
{
    const auto utf8 = utils::WideToUtf8(message);
    blog(level, "[coreBS] %s", utf8.c_str());
}

const char* GetSourceName(void*)
{
    return "coreBS Game Capture";
}

void* CreateSource(obs_data_t* settings, obs_source_t* source)
{
    auto* instance = new GameCaptureSource(source);
    instance->Update(settings);
    return instance;
}

void DestroySource(void* data)
{
    delete static_cast<GameCaptureSource*>(data);
}

void UpdateSource(void* data, obs_data_t* settings)
{
    static_cast<GameCaptureSource*>(data)->Update(settings);
}

void TickSource(void* data, float seconds)
{
    static_cast<GameCaptureSource*>(data)->Tick(seconds);
}

void RenderSource(void* data, gs_effect_t* effect)
{
    static_cast<GameCaptureSource*>(data)->Render(effect);
}

uint32_t GetSourceWidth(void* data)
{
    return static_cast<GameCaptureSource*>(data)->GetWidth();
}

uint32_t GetSourceHeight(void* data)
{
    return static_cast<GameCaptureSource*>(data)->GetHeight();
}

void GetSourceDefaults(obs_data_t* settings)
{
    GameCaptureSource::GetDefaults(settings);
}

obs_properties_t* GetSourceProperties(void* data)
{
    (void)data;
    return GameCaptureSource::GetProperties();
}

GameCaptureSource::TargetMode ParseMode(std::string_view mode)
{
    if (mode == kModeTitle) {
        return GameCaptureSource::TargetMode::Title;
    }
    if (mode == kModePid) {
        return GameCaptureSource::TargetMode::Pid;
    }
    return GameCaptureSource::TargetMode::Exe;
}

}  // namespace

GameCaptureSource::GameCaptureSource(obs_source_t* source)
    : m_source(source)
{
}

GameCaptureSource::~GameCaptureSource()
{
    Detach();
    obs_enter_graphics();
    if (m_texture != nullptr) {
        gs_texture_destroy(m_texture);
        m_texture = nullptr;
    }
    obs_leave_graphics();
}

void GameCaptureSource::Update(obs_data_t* settings)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    ApplySettings(settings);
    Detach();
    m_lastPollQpc = 0;
    m_attachQpc = 0;
    m_loggedNoFrameWarning = false;
}

void GameCaptureSource::Tick(float)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    try {
        UpdateAttachment();
        UploadLatestFrame();
    } catch (const std::exception& ex) {
        LogObs(LOG_WARNING, L"OBS game capture source tick failed: " + utils::Utf8ToWide(ex.what()));
        Detach();
    }
}

void GameCaptureSource::Render(gs_effect_t* effect)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_texture == nullptr || m_width == 0 || m_height == 0 || effect == nullptr) {
        return;
    }

    gs_blend_state_push();
    gs_blend_function(GS_BLEND_ONE, GS_BLEND_INVSRCALPHA);

    gs_eparam_t* image = gs_effect_get_param_by_name(effect, "image");
    if (image != nullptr) {
        gs_effect_set_texture(image, m_texture);
    }

    gs_draw_sprite(m_texture, 0, m_width, m_height);

    gs_blend_state_pop();
}

uint32_t GameCaptureSource::GetWidth() const
{
    return m_width;
}

uint32_t GameCaptureSource::GetHeight() const
{
    return m_height;
}

void GameCaptureSource::GetDefaults(obs_data_t* settings)
{
    obs_data_set_default_string(settings, "target_mode", kModeExe);
    obs_data_set_default_bool(settings, "capture_cursor", false);
    obs_data_set_default_int(settings, "pid", 0);
}

obs_properties_t* GameCaptureSource::GetProperties()
{
    obs_properties_t* props = obs_properties_create();

    obs_property_t* mode = obs_properties_add_list(
        props,
        "target_mode",
        "Target Mode",
        OBS_COMBO_TYPE_LIST,
        OBS_COMBO_FORMAT_STRING);
    obs_property_list_add_string(mode, "Executable Name", kModeExe);
    obs_property_list_add_string(mode, "Window Title", kModeTitle);
    obs_property_list_add_string(mode, "Process ID", kModePid);

    obs_properties_add_text(props, "exe_name", "Executable Name", OBS_TEXT_DEFAULT);
    obs_properties_add_text(props, "window_title", "Window Title", OBS_TEXT_DEFAULT);
    obs_properties_add_int(props, "pid", "Process ID", 0, std::numeric_limits<int>::max(), 1);
    obs_properties_add_bool(props, "capture_cursor", "Capture Cursor");
    return props;
}

void GameCaptureSource::ApplySettings(obs_data_t* settings)
{
    const char* modeValue = obs_data_get_string(settings, "target_mode");
    m_selection.mode = ParseMode(modeValue != nullptr ? modeValue : "");
    m_selection.exeName = ReadWideString(settings, "exe_name");
    m_selection.windowTitle = ReadWideString(settings, "window_title");
    m_selection.captureCursor = obs_data_get_bool(settings, "capture_cursor");

    const int pidValue = static_cast<int>(obs_data_get_int(settings, "pid"));
    if (pidValue > 0) {
        m_selection.pid = static_cast<DWORD>(pidValue);
    } else {
        m_selection.pid.reset();
    }
}

void GameCaptureSource::UpdateAttachment()
{
    const int64_t nowQpc = utils::QueryPerformanceCounterValue();

    if (m_activeTarget.has_value()) {
        if (!ActiveWindowStillValid() || !m_capture.IsRunning()) {
            Detach();
        }
    }

    if (m_activeTarget.has_value()) {
        return;
    }

    if (!ShouldPoll(nowQpc)) {
        return;
    }

    bool sawMatchingProcess = false;
    const auto resolved = TryResolveTarget(sawMatchingProcess);
    (void)sawMatchingProcess;
    m_lastPollQpc = nowQpc;

    if (resolved.has_value()) {
        Attach(*resolved);
    }
}

void GameCaptureSource::Attach(const ResolvedTarget& target)
{
    m_capture.Start(WindowCaptureSource::StartOptions{
        target.window.hwnd,
        m_selection.captureCursor,
        true,
    });
    m_activeTarget = target;
    m_attachQpc = utils::QueryPerformanceCounterValue();
    m_loggedNoFrameWarning = false;
}

void GameCaptureSource::Detach()
{
    if (m_capture.IsRunning()) {
        m_capture.Stop();
    }
    m_activeTarget.reset();
    m_attachQpc = 0;
    m_loggedNoFrameWarning = false;
}

void GameCaptureSource::UploadLatestFrame()
{
    if (!m_capture.IsRunning()) {
        return;
    }

    const auto snapshot = m_capture.GetLatestSnapshot();
    if (!snapshot.hasFrame || snapshot.width == 0 || snapshot.height == 0 || snapshot.pixels.empty()) {
        if (m_activeTarget.has_value() && m_attachQpc != 0 && !m_loggedNoFrameWarning) {
            const int64_t nowQpc = utils::QueryPerformanceCounterValue();
            if ((nowQpc - m_attachQpc) >= utils::SecondsToQpcTicks(kFirstFrameWarningSeconds)) {
                std::wstring message =
                    L"Attached to OBS game capture target but still waiting for the first frame from HWND=" +
                    utils::FormatPointer(m_activeTarget->window.hwnd);
                LogObs(LOG_WARNING, message);
                m_loggedNoFrameWarning = true;
            }
        }
        return;
    }

    obs_enter_graphics();

    if (m_texture != nullptr && (m_width != snapshot.width || m_height != snapshot.height)) {
        gs_texture_destroy(m_texture);
        m_texture = nullptr;
    }

    if (m_texture == nullptr) {
        m_texture = gs_texture_create(snapshot.width, snapshot.height, GS_BGRA, 1, nullptr, GS_DYNAMIC);
    }

    if (m_texture != nullptr) {
        gs_texture_set_image(
            m_texture,
            reinterpret_cast<const uint8_t*>(snapshot.pixels.data()),
            snapshot.width * 4,
            false);
        m_width = snapshot.width;
        m_height = snapshot.height;
    }

    obs_leave_graphics();
}

bool GameCaptureSource::ShouldPoll(int64_t nowQpc) const
{
    if (m_lastPollQpc == 0) {
        return true;
    }
    return (nowQpc - m_lastPollQpc) >= utils::SecondsToQpcTicks(kPollIntervalSeconds);
}

bool GameCaptureSource::ActiveWindowStillValid() const
{
    if (!m_activeTarget.has_value()) {
        return false;
    }

    const auto windows = WindowFinder::FindWindowsForPid(m_activeTarget->process.pid);
    return std::any_of(windows.begin(), windows.end(), [this](const WindowInfo& window) {
        return window.hwnd == m_activeTarget->window.hwnd;
    });
}

std::optional<GameCaptureSource::ResolvedTarget> GameCaptureSource::TryResolveTarget(bool& sawMatchingProcess) const
{
    sawMatchingProcess = false;

    switch (m_selection.mode) {
    case TargetMode::Pid: {
        if (!m_selection.pid.has_value()) {
            return std::nullopt;
        }

        const auto process = ProcessFinder::FindByPid(*m_selection.pid);
        if (!process.has_value()) {
            return std::nullopt;
        }

        sawMatchingProcess = true;
        const auto window = WindowFinder::FindBestWindowForPid(*m_selection.pid);
        if (!window.has_value()) {
            return std::nullopt;
        }
        return ResolvedTarget{*process, *window};
    }
    case TargetMode::Title: {
        if (m_selection.windowTitle.empty()) {
            return std::nullopt;
        }

        const auto windows = WindowFinder::FindWindowsByTitle(m_selection.windowTitle);
        if (windows.empty()) {
            return std::nullopt;
        }

        const auto process = ProcessFinder::FindByPid(windows.front().pid);
        if (!process.has_value()) {
            return std::nullopt;
        }

        sawMatchingProcess = true;
        return ResolvedTarget{*process, windows.front()};
    }
    case TargetMode::Exe:
    default: {
        if (m_selection.exeName.empty()) {
            return std::nullopt;
        }

        const auto processes = ProcessFinder::FindByExeName(m_selection.exeName);
        if (processes.empty()) {
            return std::nullopt;
        }

        sawMatchingProcess = true;
        std::optional<ResolvedTarget> bestMatch;
        long long bestScore = -1;

        for (const auto& process : processes) {
            const auto window = WindowFinder::FindBestWindowForPid(process.pid);
            if (!window.has_value()) {
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
    }
}

std::wstring GameCaptureSource::ReadWideString(obs_data_t* settings, const char* key)
{
    const char* value = obs_data_get_string(settings, key);
    return value != nullptr ? utils::Utf8ToWide(value) : std::wstring{};
}

obs_source_info* GetGameCaptureSourceInfo()
{
    static obs_source_info info = {};
    static bool initialized = false;

    if (!initialized) {
        info.id = "corebs_game_capture";
        info.type = OBS_SOURCE_TYPE_INPUT;
        info.output_flags = OBS_SOURCE_VIDEO;
        info.get_name = GetSourceName;
        info.create = CreateSource;
        info.destroy = DestroySource;
        info.update = UpdateSource;
        info.get_width = GetSourceWidth;
        info.get_height = GetSourceHeight;
        info.get_defaults = GetSourceDefaults;
        info.get_properties = GetSourceProperties;
        info.video_tick = TickSource;
        info.video_render = RenderSource;
        initialized = true;
    }

    return &info;
}

}  // namespace corebs::obs_plugin

obs_source_info* corebs_get_game_capture_source_info()
{
    return corebs::obs_plugin::GetGameCaptureSourceInfo();
}
