#pragma once

#include <windows.h>

#include <optional>
#include <string>
#include <vector>

namespace corebs {

struct WindowInfo {
    HWND hwnd = nullptr;
    DWORD pid = 0;
    RECT bounds{};
    std::wstring title;
};

class WindowFinder {
public:
    static std::vector<WindowInfo> EnumerateTopLevelWindows();
    static std::vector<WindowInfo> FindWindowsForPid(DWORD pid);
    static std::vector<WindowInfo> FindWindowsByTitle(const std::wstring& title);
    static std::optional<WindowInfo> FindBestWindowForPid(DWORD pid);

private:
    static bool IsCaptureCandidate(HWND hwnd);
    static long long ScoreWindow(const WindowInfo& window);
};

}  // namespace corebs