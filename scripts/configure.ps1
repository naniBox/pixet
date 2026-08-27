<#
.SYNOPSIS
    Configures pixet with the MSVC environment and shared vcpkg cache set up first.

.DESCRIPTION
    See build.ps1 for why the VS dev shell needs to be entered explicitly rather than
    relying on VS Code's CMake Tools to do it. vcpkg's dependency install happens
    during *this* step (configure), not build - so the shared binary cache
    (vcpkg-cache-env.ps1) has to be set up before `cmake --preset` runs here, not just
    before `cmake --build`.

.EXAMPLE
    ./scripts/configure.ps1 -Preset release -DPIXET_DEBUG_MENU=OFF

    Extra arguments (anything not matching -Preset) pass straight through to
    `cmake --preset` - the counterpart to configure.sh's `"$@"` forwarding, used by
    deploy-windows.ps1 to configure a build without the &Debug menu.
#>
param(
    [Parameter(Mandatory = $true)]
    [ValidateSet("debug", "release")]
    [string]$Preset,

    [Parameter(ValueFromRemainingArguments = $true)]
    [string[]]$ExtraArgs
)

$ErrorActionPreference = "Stop"

# Enter-VsDevShell below runs VsDevCmd.bat internally, which calls `vswhere` by bare name and
# prints "'vswhere.exe' is not recognized as an internal or external command" when the VS
# Installer directory isn't on PATH - which it isn't by default. Nothing actually fails: the
# dev shell still initializes and the build is correct. But it prints two error lines at the
# top of every configure and every build, which reads exactly like the toolchain failing to
# start. Putting the directory on PATH for this process is what the message is asking for.
$vsInstallerDir = "C:\Program Files (x86)\Microsoft Visual Studio\Installer"
if (Test-Path $vsInstallerDir) { $env:Path = "$vsInstallerDir;$env:Path" }

$vswhere = "C:\Program Files (x86)\Microsoft Visual Studio\Installer\vswhere.exe"
$vsPath = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (-not $vsPath) {
    Write-Error "Could not locate a Visual Studio install with the C++ (VC.Tools.x86.x64) workload via vswhere."
    exit 1
}

Import-Module "$vsPath\Common7\Tools\Microsoft.VisualStudio.DevShell.dll"
Enter-VsDevShell -VsInstallPath $vsPath -SkipAutomaticLocation -DevCmdArguments "-arch=x64 -host_arch=x64" | Out-Null
$env:Path = $env:Path + ";" + [System.Environment]::GetEnvironmentVariable("Path", "Machine") + ";" + [System.Environment]::GetEnvironmentVariable("Path", "User")

$repoRoot = Split-Path -Parent $PSScriptRoot
. (Join-Path $PSScriptRoot "vcpkg-cache-env.ps1")

Push-Location $repoRoot
try {
    cmake --preset $Preset @ExtraArgs
    exit $LASTEXITCODE
} finally {
    Pop-Location
}
