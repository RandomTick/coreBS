#include "Recorder.h"
#include "Utils.h"

#include <mfapi.h>
#include <winrt/base.h>

#include <cstdlib>
#include <iostream>
#include <vector>

namespace {

void PrintUsage()
{
    std::wcout
        << L"coreBS - minimal Windows 11 speedrun backup recorder\n\n"
        << L"Usage:\n"
        << L"  coreBS.exe (--exe <name.exe> | --pid <pid> | --title <window title>) [options]\n\n"
        << L"Options:\n"
        << L"  --out <path>              Final output path for the session recording.\n"
        << L"  --fps <n>                 Target output rate. Default: 60.\n"
        << L"  --no-cursor               Disable cursor capture for the game source.\n"
        << L"  --audio-off               Disable target-process audio capture.\n"
        << L"  --livesplit-title <text>  Window title match for LiveSplit. Default: LiveSplit.\n"
        << L"  --audio-sync-ms <n>       Shift audio earlier by <n> ms. Default: 1000.\n"
        << L"  --wait                    Wait for the target if it is not available yet (default).\n"
        << L"  --no-wait                 Fail immediately if the target is not available at startup.\n"
        << L"  --verbose                 Print verbose logs.\n"
        << L"  --help                    Show this message.\n";
}

corebs::Recorder::Options ParseArguments(int argc, wchar_t** argv)
{
    corebs::Recorder::Options options;

    for (int index = 1; index < argc; ++index) {
        const std::wstring argument = argv[index];
        auto requireValue = [&](const wchar_t* flag) -> std::wstring {
            if (index + 1 >= argc) {
                corebs::utils::Fail(std::wstring(L"Missing value for ") + flag + L".");
            }
            ++index;
            return argv[index];
        };

        if (argument == L"--exe") {
            options.exeName = requireValue(L"--exe");
        } else if (argument == L"--pid") {
            options.pid = static_cast<DWORD>(std::stoul(requireValue(L"--pid")));
        } else if (argument == L"--title") {
            options.windowTitle = requireValue(L"--title");
        } else if (argument == L"--out") {
            options.outputPath = requireValue(L"--out");
        } else if (argument == L"--fps") {
            options.fps = std::stoi(requireValue(L"--fps"));
            if (options.fps <= 0 || options.fps > 240) {
                corebs::utils::Fail(L"--fps must be between 1 and 240.");
            }
        } else if (argument == L"--no-cursor") {
            options.captureCursor = false;
        } else if (argument == L"--audio-off") {
            options.audioEnabled = false;
        } else if (argument == L"--audio-sync-ms") {
            options.audioSyncMs = std::stoi(requireValue(L"--audio-sync-ms"));
            if (options.audioSyncMs < -1000 || options.audioSyncMs > 1000) {
                corebs::utils::Fail(L"--audio-sync-ms must be between -1000 and 1000.");
            }
        } else if (argument == L"--livesplit-title") {
            options.liveSplitTitle = requireValue(L"--livesplit-title");
        } else if (argument == L"--wait") {
            options.waitForTarget = true;
        } else if (argument == L"--no-wait") {
            options.waitForTarget = false;
        } else if (argument == L"--verbose") {
            options.verbose = true;
        } else if (argument == L"--help" || argument == L"-h" || argument == L"/?") {
            PrintUsage();
            std::exit(0);
        } else {
            corebs::utils::Fail(L"Unknown argument: " + argument);
        }
    }

    return options;
}

}  // namespace

int wmain(int argc, wchar_t** argv)
{
    bool mediaFoundationStarted = false;

    try {
        const auto options = ParseArguments(argc, argv);
        corebs::utils::SetVerboseLogging(options.verbose);

        winrt::init_apartment(winrt::apartment_type::multi_threaded);
        corebs::utils::ThrowIfFailed(MFStartup(MF_VERSION), L"MFStartup failed");
        mediaFoundationStarted = true;

        corebs::Recorder recorder;
        const int exitCode = recorder.Run(options);

        MFShutdown();
        return exitCode;
    } catch (const std::exception& ex) {
        if (mediaFoundationStarted) {
            MFShutdown();
        }
        corebs::utils::LogError(corebs::utils::Utf8ToWide(ex.what()));
    }

    return 1;
}
