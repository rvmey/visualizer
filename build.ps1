<#
.SYNOPSIS
    Configures and builds MilkdropVisualizer with the vcpkg toolchain.

.PARAMETER Config
    Build configuration (Release or Debug). Default: Release.

.PARAMETER PresetsDir
    Directory of .milk presets to embed (sets MILKDROP_PRESETS_SOURCE_DIR).
    Omit to use the value in CMakeLists.txt / the existing CMake cache.

.PARAMETER Clean
    Delete the build directory first.

.PARAMETER Run
    Launch the executable after a successful build.

.EXAMPLE
    .\build.ps1
    .\build.ps1 -Config Debug -PresetsDir D:\presets -Run
#>
[CmdletBinding()]
param(
    [ValidateSet('Release', 'Debug')]
    [string]$Config = 'Release',
    [string]$PresetsDir,
    [switch]$Clean,
    [switch]$Run
)

$ErrorActionPreference = 'Stop'
$root = $PSScriptRoot
$buildDir = Join-Path $root 'build'

# vcpkg root: $env:VCPKG_ROOT, else C:\vcpkg
$vcpkgRoot = if ($env:VCPKG_ROOT) { $env:VCPKG_ROOT } else { 'C:\vcpkg' }
$toolchain = Join-Path $vcpkgRoot 'scripts\buildsystems\vcpkg.cmake'
if (-not (Test-Path $toolchain)) {
    throw "vcpkg toolchain not found at $toolchain. Set VCPKG_ROOT to your vcpkg checkout."
}

# cmake: PATH, then the copy vcpkg downloaded
$cmake = (Get-Command cmake -ErrorAction SilentlyContinue).Source
if (-not $cmake) {
    $cmake = Get-ChildItem (Join-Path $vcpkgRoot 'downloads\tools\cmake-*\**\bin\cmake.exe') -ErrorAction SilentlyContinue |
        Sort-Object FullName -Descending | Select-Object -First 1 -ExpandProperty FullName
}
if (-not $cmake) {
    throw 'cmake not found on PATH or under the vcpkg downloads folder.'
}

if ($PresetsDir -and -not (Test-Path $PresetsDir -PathType Container)) {
    throw "Presets directory not found: $PresetsDir"
}

if ($Clean -and (Test-Path $buildDir)) {
    Write-Host "Removing $buildDir"
    Remove-Item $buildDir -Recurse -Force
}

$configureArgs = @('-S', $root, '-B', $buildDir, "-DCMAKE_TOOLCHAIN_FILE=$toolchain")
if ($PresetsDir) {
    $configureArgs += "-DMILKDROP_PRESETS_SOURCE_DIR=$((Resolve-Path $PresetsDir).Path -replace '\\', '/')"
}

Write-Host "Using $cmake"
& $cmake @configureArgs
if ($LASTEXITCODE -ne 0) { throw "CMake configure failed (exit $LASTEXITCODE)" }

# The preset pack is built from this directory; fail early if it's missing.
$cachedPresets = (Select-String -Path (Join-Path $buildDir 'CMakeCache.txt') `
        -Pattern '^MILKDROP_PRESETS_SOURCE_DIR:[A-Z]+=(.*)$').Matches[0].Groups[1].Value
if (-not (Test-Path $cachedPresets -PathType Container)) {
    throw "Presets directory not found: $cachedPresets`nPass -PresetsDir <folder of .milk files>."
}

& $cmake --build $buildDir --config $Config --parallel
if ($LASTEXITCODE -ne 0) { throw "CMake build failed (exit $LASTEXITCODE)" }

$exe = Join-Path $buildDir "$Config\MilkdropVisualizer.exe"
Write-Host "Built: $exe"

if ($Run) {
    & $exe
}
