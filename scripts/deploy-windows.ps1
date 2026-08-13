<#
.SYNOPSIS
    Builds a self-contained pixet release and wraps it in a Windows installer.

.DESCRIPTION
    Counterpart to scripts/deploy-mac.sh - see that script's header for the platform
    contrast. macOS only has Qt to bundle (its vcpkg triplet is static); Windows'
    x64-windows triplet is dynamic, so this has *two* sets of DLLs to carry:

      1. vcpkg's own (sqlite3, ffmpeg, libraw, libheif, ...) - already handled for us.
         vcpkg's own "z-applocal" step copies these next to pixet.exe as part of every
         normal build (see the LINK step output - `vcpkg.exe z-applocal ...`), so
         nothing new is needed for this half.
      2. Qt's - windeployqt.exe (Qt's own deployment tool, the Windows equivalent of
         macdeployqt) adds these plus the platform/imageformats/styles plugin folders.

    Both land in a clean staging folder (build/win-deploy) rather than the raw
    build/release/src/app output, so a re-run always starts from exactly what the
    current build produced - no leftover DLLs from a previous run's different Qt
    version, no risk of the installer accidentally picking up debug artifacts sitting
    in the same folder from local testing.

    Usage: ./scripts/deploy-windows.ps1
#>

$ErrorActionPreference = "Stop"

$repoRoot = Split-Path -Parent $PSScriptRoot
Push-Location $repoRoot
try {
    # VERSION here is the single source of truth (see CMakeLists.txt's own comment on
    # project(VERSION)) - read back with a plain regex rather than configuring first and
    # inspecting CMakeCache.txt, since this needs to run before configure below anyway
    # and there's no built artifact (no Info.plist equivalent) to extract it from
    # post-build the way deploy-mac.sh does.
    $projectLine = Select-String -Path "CMakeLists.txt" -Pattern 'project\(pixet VERSION ([0-9]+\.[0-9]+\.[0-9]+)'
    if (-not $projectLine) {
        Write-Error "Could not find 'project(pixet VERSION X.Y.Z ...)' in CMakeLists.txt"
        exit 1
    }
    $version = $projectLine.Matches[0].Groups[1].Value
    Write-Output "==> pixet $version"

    $qtBin = "C:\Qt\6.8.3\msvc2022_64\bin"
    $windeployqt = Join-Path $qtBin "windeployqt.exe"
    if (-not (Test-Path $windeployqt)) {
        Write-Error "windeployqt.exe not found at $windeployqt"
        exit 1
    }

    $iscc = "$env:LOCALAPPDATA\Programs\Inno Setup 6\ISCC.exe"
    if (-not (Test-Path $iscc)) {
        Write-Error "ISCC.exe (Inno Setup) not found at $iscc - install it: winget install JRSoftware.InnoSetup"
        exit 1
    }

    # PIXET_DEBUG_MENU=OFF is the point of building through this script rather than
    # reusing an existing build/release tree: the &Debug menu ("Copy Grid Debug Info")
    # is deliberately kept in local release builds - see MainWindow.h's comment asking
    # for it permanently - but it has no business appearing in an installer handed to
    # someone else. Same reasoning deploy-mac.sh's DMG build already applies.
    Write-Output "==> Configuring (release, PIXET_DEBUG_MENU=OFF)"
    & "$PSScriptRoot\configure.ps1" -Preset release -DPIXET_DEBUG_MENU=OFF
    if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

    Write-Output "==> Building (release)"
    & "$PSScriptRoot\build.ps1" -Preset release
    if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

    $builtExe = "$repoRoot\build\release\src\app\pixet.exe"
    if (-not (Test-Path $builtExe)) {
        Write-Error "$builtExe was not produced by the build"
        exit 1
    }
    $builtIndexExe = "$repoRoot\build\release\src\index\pixet-index.exe"
    if (-not (Test-Path $builtIndexExe)) {
        Write-Error "$builtIndexExe was not produced by the build"
        exit 1
    }

    $stage = "$repoRoot\build\win-deploy"
    Write-Output "==> Staging into $stage"
    if (Test-Path $stage) { Remove-Item -Recurse -Force $stage }
    New-Item -ItemType Directory -Path $stage | Out-Null
    # Everything vcpkg's applocal step already placed next to pixet.exe (the .exe
    # itself, plus every vcpkg-managed DLL) - just the flat files, not the .pdb/.lib
    # intermediates CMake also leaves in that folder.
    Copy-Item "$repoRoot\build\release\src\app\pixet.exe" $stage
    # pixet-index.exe (the standalone CLI indexer - useful on its own for pre-indexing
    # a library via Task Scheduler, or running --render-raws as a deliberate separate
    # pass without opening the GUI) links only pixet_core, not Qt - see
    # src/index/CMakeLists.txt - so it needs no DLLs beyond what's already staged
    # above for pixet.exe; nothing from windeployqt applies to it.
    Copy-Item $builtIndexExe $stage
    Copy-Item "$repoRoot\build\release\src\app\*.dll" $stage -ErrorAction SilentlyContinue

    Write-Output "==> Running windeployqt (adding Qt DLLs + platform plugin)"
    # --release: this is an MSVC Release build, don't pull in Qt's debug DLLs.
    # --no-translations: pixet has no translation files to bundle; skips ~a dozen .qm
    # files windeployqt would otherwise copy speculatively.
    & $windeployqt --release --no-translations "$stage\pixet.exe"
    if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

    # Cheap guard against the classic "works only on the machine that built it" mistake -
    # confirms windeployqt actually did something rather than silently no-op'ing (e.g.
    # because it couldn't find the Qt install), mirroring deploy-mac.sh's own "verify
    # nothing still links against the build machine's Qt" check.
    if (-not (Test-Path "$stage\Qt6Core.dll")) {
        Write-Error "windeployqt did not deposit Qt6Core.dll into $stage - deployment is incomplete"
        exit 1
    }
    if (-not (Test-Path "$stage\platforms\qwindows.dll")) {
        Write-Error "windeployqt did not deposit the platforms\qwindows.dll plugin into $stage - the exe won't start"
        exit 1
    }
    if (-not (Test-Path "$stage\pixet-index.exe")) {
        Write-Error "pixet-index.exe did not survive staging - deployment is incomplete"
        exit 1
    }
    Write-Output "  ok - $((Get-ChildItem $stage -Recurse -File).Count) files staged"

    Write-Output "==> Compiling installer (Inno Setup)"
    & $iscc "/DMyAppVersion=$version" "$PSScriptRoot\pixet.iss"
    if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

    $installer = "$repoRoot\build\pixet-$version-setup.exe"
    if (-not (Test-Path $installer)) {
        Write-Error "$installer was not produced by ISCC.exe"
        exit 1
    }

    Write-Output ""
    Write-Output "==> Done: $installer"
    Write-Output "    Size: $([math]::Round((Get-Item $installer).Length / 1MB, 1)) MiB"
} finally {
    Pop-Location
}
