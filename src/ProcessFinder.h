#pragma once

#include <windows.h>

#include <optional>
#include <string>
#include <vector>

namespace corebs {

struct ProcessInfo {
    DWORD pid = 0;
    std::wstring exeName;
};

class ProcessFinder {
public:
    static std::vector<ProcessInfo> EnumerateProcesses();
    static std::vector<ProcessInfo> FindByExeName(const std::wstring& exeName);
    static std::optional<ProcessInfo> FindByPid(DWORD pid);
    static HANDLE OpenProcessForMonitoring(DWORD pid);
};

}  // namespace corebs