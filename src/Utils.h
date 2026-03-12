#pragma once

#include <windows.h>

#include <chrono>
#include <filesystem>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace corebs::utils {

class AppException final : public std::runtime_error {
public:
    explicit AppException(const std::string& message);
};

void SetVerboseLogging(bool enabled);
bool IsVerboseLoggingEnabled();

void LogInfo(const std::wstring& message);
void LogWarning(const std::wstring& message);
void LogError(const std::wstring& message);
void LogVerbose(const std::wstring& message);

[[noreturn]] void Fail(const std::wstring& message);
void ThrowIfFailed(HRESULT hr, const std::wstring& context);

std::wstring Utf8ToWide(std::string_view text);
std::string WideToUtf8(std::wstring_view text);
std::wstring FormatWindowsError(DWORD error);
std::wstring FormatHresult(HRESULT hr);
std::wstring QuoteCommandLineArg(std::wstring_view arg);
std::wstring ToLower(std::wstring_view text);
std::wstring Trim(std::wstring_view text);
std::wstring FormatPointer(const void* value);
std::wstring FormatSize(unsigned int width, unsigned int height);

std::wstring CurrentTimestampForFileName();
std::filesystem::path DefaultOutputPath(bool audioEnabled);
std::filesystem::path ReplaceExtension(const std::filesystem::path& path, const std::wstring& extensionWithDot);
std::filesystem::path AppendSuffixToStem(const std::filesystem::path& path, const std::wstring& suffix);

int64_t QueryPerformanceCounterValue();
int64_t QueryPerformanceFrequencyValue();
int64_t SecondsToQpcTicks(double seconds);
int64_t QpcTicksToHundredsOfNanoseconds(int64_t ticks);

bool FileExists(const std::filesystem::path& path);
bool ToolExists(const std::wstring& name);
DWORD RunProcess(const std::wstring& executable, const std::vector<std::wstring>& arguments, bool verbose);

}  // namespace corebs::utils