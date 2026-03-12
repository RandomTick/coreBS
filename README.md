# coreBS

`coreBS` is a minimal Windows 11 command-line backup recorder for speedrunning. It is intentionally narrow in scope: capture one target game window, write a playable recording file, and stop cleanly on Ctrl+C or when the target process exits.

This first pass is a working video-only MVP:

- Window video capture uses `Windows.Graphics.Capture` with `IGraphicsCaptureItemInterop::CreateForWindow`.
- Output is a playable H.264 MP4 file.
- Audio capture is intentionally not in this first commit yet.

The follow-up pass adds process-tree application audio capture and FFmpeg-based muxing.

## Prerequisites

- Windows 11
- Visual Studio 2022 with the Desktop development with C++ workload
- Windows 10/11 SDK with `Windows.Graphics.Capture` support
- CMake 3.24 or newer

## Build in Visual Studio

1. Open a `x64 Native Tools Command Prompt for VS 2022`.
2. Configure the project:

```powershell
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
```

3. Build it:

```powershell
cmake --build build --config Release
```

The executable will be at `build\Release\coreBS.exe`.

## Build with CMake only

```powershell
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release
```

## CLI examples

Capture by exe name:

```powershell
coreBS.exe --exe LEGOStarWars.exe
```

Capture by PID:

```powershell
coreBS.exe --pid 1234 --fps 60 --out D:\Runs\backup.mp4
```

Capture by window title and hide the cursor:

```powershell
coreBS.exe --title "LEGO Star Wars - The Complete Saga" --no-cursor --verbose
```

## Current MVP behavior

- Exactly one of `--exe`, `--pid`, or `--title` must be provided.
- If `--exe` matches multiple processes, `coreBS` picks the process with the best visible top-level window.
- If the target window is minimized, hidden, tool-only, or zero-sized, startup fails with a clear message.
- Ctrl+C stops capture, finalizes the MP4, and returns success.
- If the target process exits, recording stops and finalizes automatically.
- If `--out` is omitted, a timestamped `.mp4` is created in the current working directory.

## Known limitations in the MVP commit

- Audio is not implemented in this first pass.
- The final file format is MP4 only in this commit.
- Resize handling keeps the initial output resolution and rescales later frames into it.
- The recorder prioritizes reliability and simplicity over perfect frame pacing.

## How to verify video capture

1. Start the game windowed or borderless on Windows 11.
2. Run `coreBS.exe --exe LEGOStarWars.exe --verbose`.
3. Confirm the console logs the selected PID, HWND, output path, video size, and fps.
4. Stop with Ctrl+C and open the resulting MP4 in a media player.

## Testing with OBS also running

`coreBS` does not use OBS hooks or OBS APIs. To test coexistence:

1. Start OBS separately.
2. Start a recording or preview in OBS if you want.
3. Run `coreBS` against the same game window.
4. Verify `coreBS` still captures and finalizes its own MP4 independently.