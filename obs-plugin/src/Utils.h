#pragma once

#include <windows.h>

#include <cstdint>
#include <stdexcept>
#include <string>
#include <string_view>

namespace corebs::utils {

class AppException final : public std::runtime_error {
public:
    explicit AppException(const std::string& message);
};

void LogError(const std::wstring& message);

[[noreturn]] void Fail(const std::wstring& message);
void ThrowIfFailed(HRESULT hr, const std::wstring& context);

std::wstring Utf8ToWide(std::string_view text);
std::string WideToUtf8(std::wstring_view text);
std::wstring FormatWindowsError(DWORD error);
std::wstring FormatHresult(HRESULT hr);
std::wstring ToLower(std::wstring_view text);
std::wstring Trim(std::wstring_view text);
std::wstring FormatPointer(const void* value);

int64_t QueryPerformanceCounterValue();
int64_t QueryPerformanceFrequencyValue();
int64_t SecondsToQpcTicks(double seconds);

}  // namespace corebs::utils
