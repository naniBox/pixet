<#
.SYNOPSIS
    End-to-end Windows check: debug build + tests, then a release build + tests +
    installer - the full sequence this project's own workflow runs by hand after any
    real change, chained into one script instead of re-typing it each time.

.DESCRIPTION
    Nothing here is new - it just calls the existing scripts (configure.ps1,
    build.ps1, deploy-windows.ps1) and pixet_tests.exe in order, stopping at the
    first failure. deploy-windows.ps1 already configures and builds release itself
    (with PIXET_DEBUG_MENU=OFF, the actual shipped setting) and produces the
    installer, so this doesn't duplicate that - it just also runs the release
    pixet_tests.exe once that build exists, and a debug build + test pass first
    since that's the faster loop to fail in.

.EXAMPLE
    ./scripts/win-e2e.ps1
#>

$ErrorActionPreference = "Stop"
$repoRoot = Split-Path -Parent $PSScriptRoot

Push-Location $repoRoot
try {
    Write-Output "==> Configuring debug"
    & "$PSScriptRoot\configure.ps1" -Preset debug
    if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

    Write-Output "==> Building debug"
    & "$PSScriptRoot\build.ps1" -Preset debug
    if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

    Write-Output "==> Running tests (debug)"
    & "$repoRoot\build\debug\tests\pixet_tests.exe"
    if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

    Write-Output "==> Building release + installer"
    & "$PSScriptRoot\deploy-windows.ps1"
    if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

    Write-Output "==> Running tests (release)"
    & "$repoRoot\build\release\tests\pixet_tests.exe"
    if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

    Write-Output ""
    Write-Output "==> All green: debug build+tests, release build+tests, installer."
} finally {
    Pop-Location
}
