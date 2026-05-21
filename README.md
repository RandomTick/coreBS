# coreBS

`coreBS` is a minimal Windows 11 command-line backup recorder for speedrunning.

It records one continuous session from launch until Ctrl+C into a fixed 1920x1080 layout:

- black background
- target game on the left
- LiveSplit on the right
- only the game's process-tree audio

It is intentionally narrow in scope:

- no streaming
- no scene switching
- no overlays beyond the fixed layout
- no preview window
- no runtime plugin or settings system in the recorder
- no settings UI

## What it uses

- Video capture: `Windows.Graphics.Capture` with `IGraphicsCaptureItemInterop::CreateForWindow`
- Audio capture: Windows process loopback via `ActivateAudioInterfaceAsync` and `AUDIOCLIENT_ACTIVATION_TYPE_PROCESS_LOOPBACK`
- Video encoding: Media Foundation H.264 MP4
- Final muxing: FFmpeg as an external tool when audio or container remuxing is needed

## Layout

`coreBS` always records a 1920x1080 session canvas.

- the game is aspect-fit into a large left panel
- LiveSplit is aspect-fit into a tall right panel
- unused space stays black
- game window borders are cropped out by capturing the client area

There is no configurable layout yet. The goal is a reliable backup recording for a game plus timer without them overlapping.

## Prerequisites

- Windows 11
- Visual Studio 2022 with the Desktop development with C++ workload
- Windows 10/11 SDK with `Windows.Graphics.Capture` support
- CMake 3.24 or newer
- Recommended: `ffmpeg.exe` on `PATH`

FFmpeg is used only at finalization time:

- to mux the continuous MP4 video with the captured WAV audio
- or to remux the final file if you requested a non-MP4 container

If FFmpeg is missing, `coreBS` still exits successfully, but it keeps the temporary session files instead of producing the final muxed output.

## Build

```powershell
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release
```

The executable will be at `build\Release\coreBS.exe`.

## Optional OBS source plugin

The main recorder is standalone. `obs-plugin/` contains an experimental OBS source plugin that reuses the same window-capture helpers when OBS integration is useful for testing.

That plugin is opt-in through `-DCOREBS_BUILD_OBS_PLUGIN=ON` and has its own setup notes in `obs-plugin/README.md`. Local OBS source trees, SDK staging folders, and generated plugin builds stay outside the repository under ignored `vendor/` and `build-obs/` directories.

## CLI examples

Start `coreBS` before launching the game:

```powershell
coreBS.exe --exe LEGOStarWars.exe --fps 60
```

Target by window title instead:

```powershell
coreBS.exe --title "LEGO Star Wars - The Complete Saga" --verbose
```

Write to a specific file and override the LiveSplit title match:

```powershell
coreBS.exe --exe LEGOStarWars.exe --out D:\Runs\backup.mkv --livesplit-title "LiveSplit"
```

Disable target-process audio on purpose:

```powershell
coreBS.exe --exe LEGOStarWars.exe --audio-off --out D:\Runs\video_only.mp4
```

Fail immediately if the game is not available yet:

```powershell
coreBS.exe --exe LEGOStarWars.exe --no-wait
```

## Waiting mode and automatic attach

`coreBS` defaults to `--wait` mode.

That means you can start the recorder first, then launch the game later. `coreBS` will:

- start the session recording immediately
- keep writing the continuous timeline even while the game is absent
- wait for the target process or title match instead of failing immediately
- keep retrying if the process exists but the window is minimized or otherwise not captureable yet
- attach automatically once a visible top-level window is ready
- capture the game again after restarts when using `--exe` or `--title`

While the game is missing, the game panel stays black. LiveSplit can stay visible for the whole session if its window remains available.

## Stable target identity

For restart-tolerant workflows, prefer:

- `--exe LEGOStarWars.exe`
- `--title "LEGO Star Wars - The Complete Saga"`

`--pid` only refers to the current process instance, so it is not stable across restarts.

## Continuous output behavior

`coreBS` records one continuous session from startup to Ctrl+C.

That means:

- time before the game launches is preserved
- time while the game is closed between restarts is preserved
- LiveSplit can remain visible for the full session
- the final recording is one session file when muxing succeeds

Internally, `coreBS` writes a temporary session video MP4 and, when audio is enabled, a temporary session WAV. At shutdown it finalizes those files and muxes them into the requested output when FFmpeg is available.

## Example speedrun workflow

1. Start `coreBS.exe --exe LEGOStarWars.exe --fps 60 --out D:\Runs\session.mkv`.
2. Leave the recorder running.
3. Launch the game.
4. Keep LiveSplit open.
5. Play, reset, or relaunch the game as needed.
6. Press Ctrl+C when the run session is really over.
7. `coreBS` finalizes the continuous output and exits cleanly.

## Logging behavior

`coreBS` logs the main transitions, including:

- waiting for target
- process found
- window found
- attached to PID / HWND
- target exited
- returned to waiting mode
- reattached to new PID / HWND
- LiveSplit attach / loss
- user interrupted with Ctrl+C
- final shutdown complete

Use `--verbose` to also print audio discontinuities and external tool launch commands.

## How to verify that only the target app audio is captured

1. Start the target game and something unrelated that also makes sound, like a browser video or music player.
2. Run `coreBS.exe --exe LEGOStarWars.exe --out test.mkv --verbose`.
3. Let both apps produce audio.
4. Stop the session with Ctrl+C.
5. Play the result.

The game audio should be present. The unrelated app audio should be absent.

## Testing with OBS also running

`coreBS` does not use OBS hooks or OBS game capture APIs.

1. Start OBS.
2. Leave OBS open however you normally use it.
3. Start `coreBS`.
4. Launch the game and keep LiveSplit open.
5. Verify `coreBS` keeps producing its own independent backup recording.

## Known limitations and compromises

- The layout is currently fixed at 1920x1080.
- LiveSplit is matched by window title text, defaulting to `LiveSplit`.
- If LiveSplit is missing or minimized, its panel stays black.
- If the game is running but minimized, hidden, or otherwise not captureable, `coreBS` keeps waiting and the game panel stays black.
- Audio is captured only while the target game process is attached.
- If the game's audio format changes between separate launches in the same session, later audio is ignored and a warning is printed.
- Audio temp files use standard RIFF WAV sizing, so extremely long audio sessions can hit WAV size limits before final muxing.
- The current compositor uses CPU copies and simple scaling to keep the implementation small and reliable.
- FFmpeg is still needed for final audio muxing or non-MP4 final containers.
- The recorder is Windows-only by design.
