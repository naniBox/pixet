<#
.SYNOPSIS
    Builds pixet with the MSVC environment explicitly set up first.

.DESCRIPTION
    CMake's Ninja+MSVC generator does not bake the compiler's system include/lib paths
    into build.ninja - it relies on INCLUDE/LIB/PATH being set in the calling process's
    environment (normally via vcvarsall.bat / Enter-VsDevShell). VS Code's CMake Tools
    extension *can* inject this automatically, but only reliably does so for a build
    directory it configured itself - if the build directory was ever configured by
    running `cmake --preset` directly (from a terminal, or by another session), CMake
    Tools' F5 build task can silently run cl.exe with no INCLUDE set at all, failing
    with "Cannot open include file: 'type_traits'" the moment anything actually needs
    recompiling. This script sidesteps that inconsistency entirely by always entering
    the dev shell itself before building, regardless of what CMake Tools thinks it owns.
#>
param(
    [Parameter(Mandatory = $true)]
    [ValidateSet("debug", "release")]
    [string]$Preset
)

$ErrorActionPreference = "Stop"

$vswhere = "C:\Program Files (x86)\Microsoft Visual Studio\Installer\vswhere.exe"
$vsPath = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (-not $vsPath) {
    Write-Error "Could not locate a Visual Studio install with the C++ (VC.Tools.x86.x64) workload via vswhere."
    exit 1
}

Import-Module "$vsPath\Common7\Tools\Microsoft.VisualStudio.DevShell.dll"
Enter-VsDevShell -VsInstallPath $vsPath -SkipAutomaticLocation -DevCmdArguments "-arch=x64 -host_arch=x64" | Out-Null

# Enter-VsDevShell replaces $env:Path rather than extending it, dropping user-scope
# tool installs (cmake, ninja) - merge the registry PATH back in.
$env:Path = $env:Path + ";" + [System.Environment]::GetEnvironmentVariable("Path", "Machine") + ";" + [System.Environment]::GetEnvironmentVariable("Path", "User")

$repoRoot = Split-Path -Parent $PSScriptRoot
# CMake reruns vcpkg install as part of an implicit reconfigure if CMakeLists.txt/
# vcpkg.json changed since the last build - set up the shared cache in case that
# happens here rather than only in configure.ps1.
. (Join-Path $PSScriptRoot "vcpkg-cache-env.ps1")

Push-Location $repoRoot
try {
    cmake --build "build/$Preset"
    exit $LASTEXITCODE
} finally {
    Pop-Location
}
