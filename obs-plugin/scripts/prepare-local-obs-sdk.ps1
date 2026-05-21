param(
    [string]$ObsInstallRoot = "C:\Program Files\obs-studio",
    [string]$ObsSourceRoot = "$(Split-Path (Split-Path $PSScriptRoot -Parent) -Parent)\vendor\obs-studio",
    [string]$ObsSdkRoot = "$(Split-Path (Split-Path $PSScriptRoot -Parent) -Parent)\vendor\obs-sdk"
)

$ErrorActionPreference = 'Stop'

$vswhere = 'C:\Program Files (x86)\Microsoft Visual Studio\Installer\vswhere.exe'
if (-not (Test-Path $vswhere)) {
    throw "vswhere.exe not found at $vswhere"
}

$vsPath = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (-not $vsPath) {
    throw 'Visual Studio with C++ tools was not found.'
}

$toolRoot = Join-Path $vsPath 'VC\Tools\MSVC'
$msvcVersionDir = Get-ChildItem $toolRoot -Directory | Sort-Object Name -Descending | Select-Object -First 1
if (-not $msvcVersionDir) {
    throw "No MSVC toolset found under $toolRoot"
}

$dumpbin = Join-Path $msvcVersionDir.FullName 'bin\Hostx64\x64\dumpbin.exe'
$libexe = Join-Path $msvcVersionDir.FullName 'bin\Hostx64\x64\lib.exe'
$obsExe = Join-Path $ObsInstallRoot 'bin\64bit\obs64.exe'
$obsDll = Join-Path $ObsInstallRoot 'bin\64bit\obs.dll'

if (-not (Test-Path $dumpbin)) {
    throw "dumpbin.exe not found at $dumpbin"
}
if (-not (Test-Path $libexe)) {
    throw "lib.exe not found at $libexe"
}
if (-not (Test-Path $obsExe)) {
    throw "OBS executable not found at $obsExe"
}
if (-not (Test-Path $obsDll)) {
    throw "OBS runtime library not found at $obsDll"
}

$obsVersion = (Get-Item $obsExe).VersionInfo.ProductVersion
$headerProbe = Join-Path $ObsSourceRoot 'libobs\obs-module.h'
if (-not (Test-Path $headerProbe)) {
    Write-Host "OBS headers are missing at $ObsSourceRoot" -ForegroundColor Yellow
    Write-Host "Clone the matching OBS source tree first, for example:" -ForegroundColor Yellow
    Write-Host "  git clone --depth 1 --branch $obsVersion https://github.com/obsproject/obs-studio.git vendor\\obs-studio" -ForegroundColor Yellow
    exit 1
}

$outDir = Join-Path $ObsSdkRoot 'lib'
New-Item -ItemType Directory -Force -Path $outDir | Out-Null

$dump = & $dumpbin /exports $obsDll
if ($LASTEXITCODE -ne 0) {
    throw 'dumpbin failed while reading obs.dll exports.'
}

$exports = $dump |
    Where-Object { $_ -match '^\s+\d+\s+[0-9A-F]+\s+[0-9A-F]+\s+([^\s=]+)' } |
    ForEach-Object {
        if ($_ -match '^\s+\d+\s+[0-9A-F]+\s+[0-9A-F]+\s+([^\s=]+)') {
            $matches[1]
        }
    }

if (-not $exports -or $exports.Count -eq 0) {
    throw 'No exports were parsed from obs.dll.'
}

$defPath = Join-Path $outDir 'obs.def'
$libPath = Join-Path $outDir 'obs.lib'
@('LIBRARY obs.dll', 'EXPORTS') + $exports | Set-Content -Path $defPath -Encoding ascii

& $libexe /def:$defPath /machine:x64 /out:$libPath | Out-Host
if ($LASTEXITCODE -ne 0) {
    throw 'lib.exe failed while generating obs.lib.'
}

Write-Host ''
Write-Host "Prepared local OBS SDK files:" -ForegroundColor Green
Write-Host "  Headers: $ObsSourceRoot\\libobs"
Write-Host "  Import lib: $libPath"
Write-Host ''
Write-Host 'Next steps:' -ForegroundColor Green
Write-Host '  cmake -S . -B build -G "Visual Studio 17 2022" -A x64'
Write-Host '  cmake --build build --config Release'
Write-Host '  cmake --install build --config Release --prefix build\\obs-package'
