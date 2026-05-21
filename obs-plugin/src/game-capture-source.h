#pragma once

#include "ProcessFinder.h"
#include "WindowCaptureSource.h"
#include "WindowFinder.h"

#include <obs-module.h>

#include <mutex>
#include <optional>
#include <string>

namespace corebs::obs_plugin {

class GameCaptureSource {
public:
    GameCaptureSource();
    ~GameCaptureSource();

    void Update(obs_data_t* settings);
    void Tick(float seconds);
    void Render(gs_effect_t* effect);
    uint32_t GetWidth() const;
    uint32_t GetHeight() const;

    static void GetDefaults(obs_data_t* settings);
    static obs_properties_t* GetProperties();

public:
    enum class TargetMode {
        Exe,
        Title,
        Pid,
    };

private:
    struct TargetSelection {
        TargetMode mode = TargetMode::Exe;
        std::wstring exeName;
        std::wstring windowTitle;
        std::optional<DWORD> pid;
        bool captureCursor = false;
    };

    struct ResolvedTarget {
        ProcessInfo process;
        WindowInfo window;
    };

    void ApplySettings(obs_data_t* settings);
    void UpdateAttachment();
    void Attach(const ResolvedTarget& target);
    void Detach();
    void UploadLatestFrame();
    bool ShouldPoll(int64_t nowQpc) const;
    bool ActiveWindowStillValid() const;
    std::optional<ResolvedTarget> TryResolveTarget() const;
    static std::wstring ReadWideString(obs_data_t* settings, const char* key);

    TargetSelection m_selection{};

    mutable std::mutex m_mutex;
    WindowCaptureSource m_capture;
    std::optional<ResolvedTarget> m_activeTarget;
    gs_texture_t* m_texture = nullptr;
    uint32_t m_width = 0;
    uint32_t m_height = 0;
    int64_t m_lastPollQpc = 0;
    int64_t m_attachQpc = 0;
    bool m_loggedNoFrameWarning = false;
};

obs_source_info* GetGameCaptureSourceInfo();

}  // namespace corebs::obs_plugin

obs_source_info* corebs_get_game_capture_source_info();
