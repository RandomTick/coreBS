# coreBS

`coreBS` is a minimal Windows 11 command-line backup recorder for speedrunning. It captures one target game window, captures only that process tree's audio, and writes a playable backup recording without depending on OBS hooks.

It is intentionally narrow in scope:

- No scenes
- No streaming
- No overlays
- No preview window
- No plugin system
- No settings UI

## What it uses

- Video: `Windows.Graphics.Capture` with `IGraphicsCaptureItemInterop::CreateForWindow`
- Audio: Windows process loopback capture via `ActivateAudioInterfaceAsync` and `AUDIOCLIENT_ACTIVATION_TYPE_PROCESS_LOOPBACK`
- Encoding: Media Foundation H.264 MP4 for video, WAV for audio, then optional FFmpeg muxing to the requested final container

## Prerequisites

- Windows 11
- Visual Studio 2022 with the Desktop development with C++ workload
- Windows 10/11 SDK with `Windows.Graphics.Capture` support
- CMake 3.24 or newer
- Optional but recommended: `ffmpeg.exe` on `PATH`

If FFmpeg is not available, `coreBS` still succeeds but keeps separate intermediate files when muxing would otherwise be required.

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

Capture by PID with explicit output:

```powershell
coreBS.exe --pid 1234 --fps 60 --out D:\Runs\backup.mkv
```

Capture by window title, disable cursor, and print verbose diagnostics:

```powershell
coreBS.exe --title "LEGO Star Wars - The Complete Saga" --no-cursor --verbose
```

Disable application audio on purpose:

```powershell
coreBS.exe --exe LEGOStarWars.exe --audio-off --out D:\Runs\video_only.mp4
```

## Behavior

- Exactly one of `--exe`, `--pid`, or `--title` must be provided.
- `coreBS` resolves the target PID first, then resolves the best visible top-level window for that PID.
- Invisible, minimized, tool, child, and zero-sized windows are rejected.
- Ctrl+C stops capture, finalizes output, and returns success.
- If the target process exits, recording stops and finalizes automatically.
- If `--out` is omitted, `coreBS` creates a timestamped output name in the current working directory.
- If audio is enabled, the default final container is `.mkv`.
- If audio is disabled, the default final container is `.mp4`.

## Output strategy

`coreBS` prefers reliability over elegance:

- Video is always written first as H.264 MP4 through Media Foundation.
- Audio is written as WAV through process loopback capture.
- If the requested final output needs muxing, `coreBS` invokes `ffmpeg.exe` as a post-step.
- If FFmpeg is missing or muxing fails, `coreBS` exits successfully with a warning and keeps the intermediate files.

Examples:

- `--audio-off --out backup.mp4`: writes `backup.mp4` directly.
- `--audio-off --out backup.mkv`: writes `backup.video.mp4` first, then remuxes to `backup.mkv` if FFmpeg exists.
- `--out backup.mkv`: writes `backup.video.mp4` and `backup.audio.wav`, then muxes them into `backup.mkv` if FFmpeg exists.

## How to verify that only the target app audio is captured

1. Start the target game and something unrelated that also makes noise, like a browser video or music player.
2. Run `coreBS.exe --exe LEGOStarWars.exe --out test.mkv --verbose`.
3. Let the game produce audio while the unrelated app also produces audio.
4. Stop the capture with Ctrl+C.
5. Play the result. The game audio should be present, and the unrelated app audio should be absent.

If FFmpeg was not available, inspect `test.audio.wav` and `test.video.mp4` separately instead.

## How to test while OBS is also running

`coreBS` does not use OBS APIs or OBS game hooks.

1. Start OBS.
2. Add any OBS capture source you want, including Game Capture.
3. Launch the game.
4. Start `coreBS` against the same target window.
5. Verify `coreBS` records and finalizes independently even if OBS Game Capture behaves differently.

## Known limitations and compromises

- Video output keeps the initial capture resolution and rescales later frames into that fixed size if the game window changes size.
- The current video path uses CPU readback from the capture texture before handing frames to Media Foundation. This keeps the code simple and reliable, but it is not the most efficient possible implementation.
- Audio is written as standard RIFF WAV before muxing, so extremely long captures can hit WAV size limits.
- If FFmpeg is missing, a requested `.mkv` or muxed `.mp4` final file is not produced automatically; the intermediate files are preserved instead.
- The recorder is Windows-only by design.