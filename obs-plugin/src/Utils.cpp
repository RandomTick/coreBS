#include "Utils.h"

#include <iostream>
#include <mutex>
#include <sstream>

namespace corebs::utils {

namespace {

std::mutex g_logMutex;

void LogLine(const wchar_t* prefix, const std::wstring& message)
{
    std::lock_guard<std::mutex> lock(g_logMutex);
    std::wcout << prefix << message << std::endl;
}

}  // namespace

AppException::AppException(const std::string& message)
    : std::runtime_error(message)
{
}

void LogError(const std::wstring& message)
{
    LogLine(L"[coreBS][error] ", message);
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

}  // namespace corebs::utils
