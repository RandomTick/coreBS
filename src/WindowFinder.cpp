#include "WindowFinder.h"

#include "Utils.h"

#include <algorithm>

namespace corebs {

namespace {

std::wstring GetWindowTitle(HWND hwnd)
{
    const auto length = GetWindowTextLengthW(hwnd);
    if (length == 0) {
        return {};
    }

    std::wstring title(static_cast<size_t>(length) + 1, L'\0');
    const auto copied = GetWindowTextW(hwnd, title.data(), length + 1);
    title.resize(static_cast<size_t>(copied));
    return title;
}

long long WindowArea(const RECT& rect)
{
    const auto width = std::max<LONG>(0, rect.right - rect.left);
    const auto height = std::max<LONG>(0, rect.bottom - rect.top);
    return static_cast<long long>(width) * static_cast<long long>(height);
}

}  // namespace

std::vector<WindowInfo> WindowFinder::EnumerateTopLevelWindows()
{
    std::vector<WindowInfo> windows;

    EnumWindows(
        [](HWND hwnd, LPARAM context) -> BOOL {
            auto* list = reinterpret_cast<std::vector<WindowInfo>*>(context);
            if (!WindowFinder::IsCaptureCandidate(hwnd)) {
                return TRUE;
            }

            DWORD pid = 0;
            GetWindowThreadProcessId(hwnd, &pid);

            RECT bounds{};
            GetWindowRect(hwnd, &bounds);

            list->push_back(WindowInfo{
                hwnd,
                pid,
                bounds,
                GetWindowTitle(hwnd),
            });
            return TRUE;
        },
        reinterpret_cast<LPARAM>(&windows));

    return windows;
}

std::vector<WindowInfo> WindowFinder::FindWindowsForPid(DWORD pid)
{
    std::vector<WindowInfo> matches;
    for (const auto& window : EnumerateTopLevelWindows()) {
        if (window.pid == pid) {
            matches.push_back(window);
        }
    }
    std::sort(matches.begin(), matches.end(), [](const WindowInfo& left, const WindowInfo& right) {
        return ScoreWindow(left) > ScoreWindow(right);
    });
    return matches;
}

std::vector<WindowInfo> WindowFinder::FindWindowsByTitle(const std::wstring& title)
{
    const auto expected = utils::ToLower(title);
    std::vector<WindowInfo> matches;

    for (const auto& window : EnumerateTopLevelWindows()) {
        const auto current = utils::ToLower(window.title);
        if (current == expected || current.find(expected) != std::wstring::npos) {
            matches.push_back(window);
        }
    }

    std::sort(matches.begin(), matches.end(), [](const WindowInfo& left, const WindowInfo& right) {
        return ScoreWindow(left) > ScoreWindow(right);
    });
    return matches;
}

std::optional<WindowInfo> WindowFinder::FindBestWindowForPid(DWORD pid)
{
    const auto matches = FindWindowsForPid(pid);
    if (matches.empty()) {
        return std::nullopt;
    }
    return matches.front();
}

bool WindowFinder::IsCaptureCandidate(HWND hwnd)
{
    if (hwnd == nullptr) {
        return false;
    }
    if (GetAncestor(hwnd, GA_ROOT) != hwnd) {
        return false;
    }
    if (!IsWindowVisible(hwnd)) {
        return false;
    }
    if (IsIconic(hwnd)) {
        return false;
    }
    if ((GetWindowLongPtrW(hwnd, GWL_EXSTYLE) & WS_EX_TOOLWINDOW) != 0) {
        return false;
    }
    if (GetWindow(hwnd, GW_OWNER) != nullptr) {
        return false;
    }


    RECT bounds{};
    if (!GetWindowRect(hwnd, &bounds)) {
        return false;
    }
    if (bounds.right <= bounds.left || bounds.bottom <= bounds.top) {
        return false;
    }

    return true;
}

long long WindowFinder::ScoreWindow(const WindowInfo& window)
{
    long long score = WindowArea(window.bounds);
    if (!window.title.empty()) {
        score += 1000000;
    }
    return score;
}

}  // namespace corebs
