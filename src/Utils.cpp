#include "Utils.h"

#include <iomanip>
#include <iostream>
#include <mutex>
#include <sstream>
#include <vector>

namespace corebs::utils {

namespace {

std::mutex g_logMutex;
bool g_verboseLogging = false;

std::wstring BuildTimestamp()
{
    SYSTEMTIME localTime{};
    GetLocalTime(&localTime);

    std::wstringstream stream;
    stream << std::setfill(L'0')
           << localTime.wYear
           << std::setw(2) << localTime.wMonth
           << std::setw(2) << localTime.wDay
           << L"_"
           << std::setw(2) << localTime.wHour
           << std::setw(2) << localTime.wMinute
           << std::setw(2) << localTime.wSecond;
    return stream.str();
}

void LogLine(const wchar_t* prefix, const std::wstring& message)
{
    std::lock_guard<std::mutex> lock(g_logMutex);
    std::wcout << prefix << message << std::endl;
}

std::filesystem::path ResolveToolPath(const std::wstring& executable)
{
    const std::filesystem::path candidate(executable);
    if (candidate.has_parent_path() || candidate.is_absolute()) {
        return candidate;
    }

    const DWORD required = SearchPathW(nullptr, executable.c_str(), nullptr, 0, nullptr, nullptr);
    if (required == 0) {
        return {};
    }

    std::vector<wchar_t> buffer(static_cast<size_t>(required), L'\0');
    if (SearchPathW(nullptr, executable.c_str(), nullptr, required, buffer.data(), nullptr) == 0) {
        return {};
    }

    return std::filesystem::path(buffer.data());
}

}  // namespace

AppException::AppException(const std::string& message)
    : std::runtime_error(message)
{
}

void SetVerboseLogging(bool enabled)
{
    g_verboseLogging = enabled;
}

bool IsVerboseLoggingEnabled()
{
    return g_verboseLogging;
}

void LogInfo(const std::wstring& message)
{
    LogLine(L"[coreBS] ", message);
}

void LogWarning(const std::wstring& message)
{
    LogLine(L"[coreBS][warn] ", message);
}

void LogError(const std::wstring& message)
{
    LogLine(L"[coreBS][error] ", message);
}

void LogVerbose(const std::wstring& message)
{
    if (g_verboseLogging) {
        LogLine(L"[coreBS][verbose] ", message);
    }
}

[[noreturn]] void Fail(const std::wstring& message)
{
    throw AppException(WideToUtf8(message));
}

void ThrowIfFailed(HRESULT hr, const std::wstring& context)
{
    if (FAILED(hr)) {
        Fail(context + L": " + FormatHresult(hr));
    }
}

std::wstring Utf8ToWide(std::string_view text)
{
    if (text.empty()) {
        return {};
    }

    const auto size = MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
    std::wstring result(static_cast<size_t>(size), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), result.data(), size);
    return result;
}

std::string WideToUtf8(std::wstring_view text)
{
    if (text.empty()) {
        return {};
    }

    const auto size = WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
    std::string result(static_cast<size_t>(size), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), result.data(), size, nullptr, nullptr);
    return result;
}

std::wstring FormatWindowsError(DWORD error)
{
    LPWSTR buffer = nullptr;
    const DWORD flags = FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS;
    const auto length = FormatMessageW(flags, nullptr, error, 0, reinterpret_cast<LPWSTR>(&buffer), 0, nullptr);

    std::wstring result;
    if (length > 0 && buffer != nullptr) {
        result.assign(buffer, buffer + length);
        result = Trim(result);
    } else {
        std::wstringstream stream;
        stream << L"Win32 error " << error;
        result = stream.str();
    }

    if (buffer != nullptr) {
        LocalFree(buffer);
    }
    return result;
}

std::wstring FormatHresult(HRESULT hr)
{
    std::wstringstream stream;
    stream << L"0x" << std::hex << std::uppercase << static_cast<unsigned long>(hr) << std::dec
           << L" (" << FormatWindowsError(static_cast<DWORD>(hr)) << L")";
    return stream.str();
}

std::wstring QuoteCommandLineArg(std::wstring_view arg)
{
    if (arg.find_first_of(L" \t\"") == std::wstring_view::npos) {
        return std::wstring(arg);
    }

    std::wstring quoted = L"\"";
    for (const wchar_t ch : arg) {
        if (ch == L'\"') {
            quoted += L'\\';
        }
        quoted += ch;
    }
    quoted += L"\"";
    return quoted;
}

std::wstring ToLower(std::wstring_view text)
{
    std::wstring result(text);
    for (auto& ch : result) {
        ch = static_cast<wchar_t>(towlower(ch));
    }
    return result;
}

std::wstring Trim(std::wstring_view text)
{
    size_t start = 0;
    while (start < text.size() && iswspace(text[start]) != 0) {
        ++start;
    }

    size_t end = text.size();
    while (end > start && iswspace(text[end - 1]) != 0) {
        --end;
    }

    return std::wstring(text.substr(start, end - start));
}

std::wstring FormatPointer(const void* value)
{
    std::wstringstream stream;
    stream << L"0x" << std::hex << reinterpret_cast<uintptr_t>(value);
    return stream.str();
}

std::wstring FormatSize(unsigned int width, unsigned int height)
{
    std::wstringstream stream;
    stream << width << L"x" << height;
    return stream.str();
}

std::wstring CurrentTimestampForFileName()
{
    return BuildTimestamp();
}

std::filesystem::path DefaultOutputPath(bool audioEnabled)
{
    const auto extension = audioEnabled ? L".mkv" : L".mp4";
    return std::filesystem::current_path() / (L"coreBS_" + CurrentTimestampForFileName() + extension);
}

std::filesystem::path ReplaceExtension(const std::filesystem::path& path, const std::wstring& extensionWithDot)
{
    auto updated = path;
    updated.replace_extension(extensionWithDot);
    return updated;
}

std::filesystem::path AppendSuffixToStem(const std::filesystem::path& path, const std::wstring& suffix)
{
    const auto parent = path.parent_path();
    const auto stem = path.stem().wstring();
    const auto extension = path.extension().wstring();
    return parent / (stem + suffix + extension);
}

int64_t QueryPerformanceCounterValue()
{
    LARGE_INTEGER value{};
    QueryPerformanceCounter(&value);
    return value.QuadPart;
}

int64_t QueryPerformanceFrequencyValue()
{
    static const int64_t frequency = [] {
        LARGE_INTEGER value{};
        QueryPerformanceFrequency(&value);
        return value.QuadPart;
    }();
    return frequency;
}

int64_t SecondsToQpcTicks(double seconds)
{
    return static_cast<int64_t>(seconds * static_cast<double>(QueryPerformanceFrequencyValue()));
}

int64_t QpcTicksToHundredsOfNanoseconds(int64_t ticks)
{
    return (ticks * 10000000LL) / QueryPerformanceFrequencyValue();
}

bool FileExists(const std::filesystem::path& path)
{
    std::error_code error;
    return std::filesystem::exists(path, error);
}

bool ToolExists(const std::wstring& name)
{
    return !ResolveToolPath(name).empty();
}

DWORD RunProcess(const std::wstring& executable, const std::vector<std::wstring>& arguments, bool verbose)
{
    const auto resolvedExecutable = ResolveToolPath(executable);
    if (resolvedExecutable.empty()) {
        Fail(L"Failed to locate " + executable + L" on disk.");
    }

    std::wstring commandLine = QuoteCommandLineArg(resolvedExecutable.wstring());
    for (const auto& argument : arguments) {
        commandLine += L' ';
        commandLine += QuoteCommandLineArg(argument);
    }

    if (verbose) {
        LogVerbose(L"Launching external tool: " + commandLine);
    }

    STARTUPINFOW startupInfo{};
    startupInfo.cb = sizeof(startupInfo);

    PROCESS_INFORMATION processInfo{};
    std::vector<wchar_t> mutableCommand(commandLine.begin(), commandLine.end());
    mutableCommand.push_back(L'\0');

    if (!CreateProcessW(
            resolvedExecutable.c_str(),
            mutableCommand.data(),
            nullptr,
            nullptr,
            FALSE,
            0,
            nullptr,
            nullptr,
            &startupInfo,
            &processInfo)) {
        Fail(L"Failed to launch " + resolvedExecutable.wstring() + L": " + FormatWindowsError(GetLastError()));
    }

    WaitForSingleObject(processInfo.hProcess, INFINITE);

    DWORD exitCode = 0;
    GetExitCodeProcess(processInfo.hProcess, &exitCode);
    CloseHandle(processInfo.hThread);
    CloseHandle(processInfo.hProcess);
    return exitCode;
}

}  // namespace corebs::utils
