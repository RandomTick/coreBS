# coreBS OBS Game Capture

This repository contains a Windows OBS source plugin for capturing a selected gameplay window through `Windows.Graphics.Capture`.

There is no standalone recorder here. OBS owns recording, audio mixing, scenes, preview, layout, and output settings; this plugin focuses on finding and feeding the gameplay window as a source.

## Scope

The source currently supports:

- target selection by executable name, window title, or process ID
- optional cursor capture
- client-area capture without the window border
- automatic reattach when the target window changes

It intentionally does not provide:

- process-loopback audio capture
- LiveSplit composition
- recording or encoding controls
- non-Windows capture paths

## Requirements

- Windows 11
- OBS Studio installed locally
- Visual Studio 2022 with the Desktop development with C++ workload
- Windows 10/11 SDK with `Windows.Graphics.Capture` support
- CMake 3.24 or newer

## Local OBS SDK Setup

The local build expects:

- OBS Studio headers in `vendor/obs-studio`
- an OBS import library in `vendor/obs-sdk/lib/obs.lib`

The helper script can create the import-library staging folder from the installed OBS runtime:

```powershell
powershell -ExecutionPolicy Bypass -File .\obs-plugin\scripts\prepare-local-obs-sdk.ps1
```

If matching OBS headers are missing, the script prints the `git clone` command for the OBS source checkout it expects.

`vendor/` is ignored because those files are local build inputs, not plugin source.

## Build

Configure and build the plugin from the repository root:

```powershell
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release
```

The build defaults to `vendor/obs-studio` and `vendor/obs-sdk/lib/obs.lib`. You can instead pass `OBS_INCLUDE_DIR` and `OBS_LIBRARY` explicitly if your OBS SDK files live elsewhere.

## Package For OBS

Create an installable folder layout in the workspace:

```powershell
cmake --install build --config Release --prefix build\obs-package
```

That produces:

- `build\obs-package\corebs-obs-game-capture\bin\64bit\corebs-obs-game-capture.dll`
- `build\obs-package\corebs-obs-game-capture\data\locale\en-US.ini`

For a system-wide OBS install on Windows, copy that plugin folder into:

```text
C:\ProgramData\obs-studio\plugins\
```

## Repository Layout

- `obs-plugin/src`: plugin entrypoint, OBS source implementation, and the Windows capture helpers it uses
- `obs-plugin/data`: OBS locale data
- `obs-plugin/scripts`: local OBS SDK preparation helpers

## Notes

- The source only attaches to visible, captureable top-level windows.
- A process ID is tied to one running instance; executable-name and title matching are more useful for restarts.
- The plugin uses OBS graphics upload APIs after capturing frames from the Windows capture surface.
