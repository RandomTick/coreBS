# OBS Plugin Prototype

This folder is the start of the OBS pivot: a game-only visual capture source that reuses the `coreBS` window capture path.

Current scope:

- game video only
- no LiveSplit handling
- no process-loopback audio
- no fixed session layout

The goal is to let OBS handle:

- recording
- scene/layout composition
- preview
- audio mixing
- sync tweaks

while this plugin focuses only on the more reliable gameplay capture path.

## Local Dev Setup

This repo can now bootstrap a local OBS plugin build from:

- a matching OBS Studio source checkout at `vendor/obs-studio`
- a locally generated `obs.lib` import library at `vendor/obs-sdk/lib/obs.lib`

To regenerate the import library from your installed OBS runtime:

```powershell
powershell -ExecutionPolicy Bypass -File .\obs-plugin\scripts\prepare-local-obs-sdk.ps1
```

If the OBS headers are missing, the script prints the exact `git clone` command to fetch the matching OBS source tree.

## Build

Once the local OBS SDK files are present, configure and build with:

```powershell
cmake -S . -B build-obs -G "Visual Studio 17 2022" -A x64 -DCOREBS_BUILD_OBS_PLUGIN=ON
cmake --build build-obs --config Release
```

Because `obs-plugin/CMakeLists.txt` now defaults to the vendored paths, `OBS_INCLUDE_DIR` and `OBS_LIBRARY` no longer need to be passed manually when `vendor/obs-studio` and `vendor/obs-sdk/lib/obs.lib` exist.

## Package For OBS

Create a third-party plugin package in the workspace with:

```powershell
cmake --install build-obs --config Release --prefix build-obs\obs-package
```

That produces:

- `build-obs\obs-package\corebs-obs-game-capture\bin\64bit\corebs-obs-game-capture.dll`
- `build-obs\obs-package\corebs-obs-game-capture\data\locale\en-US.ini`

On newer OBS for Windows, third-party plugins should be copied into:

- `C:\ProgramData\obs-studio\plugins\corebs-obs-game-capture\bin\64bit\corebs-obs-game-capture.dll`
- `C:\ProgramData\obs-studio\plugins\corebs-obs-game-capture\data\locale\en-US.ini`

## Source Behavior

The experimental source currently aims to support:

- target by executable name
- target by window title
- target by PID
- optional cursor capture
- automatic reattach when the target window changes

## Why This Exists

The standalone recorder proved the visual capture method can work, but cross-machine timing and packaging are better solved by letting OBS own the recording pipeline.
