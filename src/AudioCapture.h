#pragma once

#include <windows.h>

#include <filesystem>

namespace corebs {

class AudioCapture {
public:
    struct StartOptions {
        DWORD targetPid = 0;
        std::filesystem::path outputPath;
        bool verbose = false;
        int64_t baseQpc = 0;
    };

    void Start(const StartOptions& options);
    void Stop();
};

}  // namespace corebs