#include "ProcessFinder.h"

#include "Utils.h"

#include <tlhelp32.h>

namespace corebs {

std::vector<ProcessInfo> ProcessFinder::EnumerateProcesses()
{
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE) {
        utils::Fail(L"CreateToolhelp32Snapshot failed: " + utils::FormatWindowsError(GetLastError()));
    }

    std::vector<ProcessInfo> result;
    PROCESSENTRY32W entry{};
    entry.dwSize = sizeof(entry);

    if (Process32FirstW(snapshot, &entry) != FALSE) {
        do {
            result.push_back(ProcessInfo{entry.th32ProcessID, entry.szExeFile});
        } while (Process32NextW(snapshot, &entry) != FALSE);
    }

    CloseHandle(snapshot);
    return result;
}

std::vector<ProcessInfo> ProcessFinder::FindByExeName(const std::wstring& exeName)
{
    const auto expected = utils::ToLower(exeName);
    std::vector<ProcessInfo> matches;
    for (const auto& process : EnumerateProcesses()) {
        if (utils::ToLower(process.exeName) == expected) {
            matches.push_back(process);
        }
    }
    return matches;
}

std::optional<ProcessInfo> ProcessFinder::FindByPid(DWORD pid)
{
    for (const auto& process : EnumerateProcesses()) {
        if (process.pid == pid) {
            return process;
        }
    }
    return std::nullopt;
}

HANDLE ProcessFinder::OpenProcessForMonitoring(DWORD pid)
{
    HANDLE process = OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (process == nullptr) {
        utils::Fail(L"OpenProcess failed for PID " + std::to_wstring(pid) + L": " + utils::FormatWindowsError(GetLastError()));
    }
    return process;
}

}  // namespace corebs