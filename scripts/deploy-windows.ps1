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
         macdeployqt) adds these plus the plugin folders for every linked module.

    windeployqt's default output is then trimmed to what pixet actually loads, the same
    way deploy-mac.sh trims the .app: 115MB staged down to 73MB, a 51MB installer down
    to 40MB. See the two prune steps below for what goes and why.

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
    # windeployqt's defaults are tuned for a Qt Quick app that might render through any
    # backend and load any image format. pixet is a Widgets app that decodes every image
    # with its own codecs (see QtInterop.h - not asking Qt to read image files is a
    # deliberate design choice), so most of what it deploys by default is dead weight.
    # These four flags skip the largest pieces at copy time rather than deleting them
    # afterwards; the plugin prune below handles what has no flag.
    #
    #   --release                 MSVC Release build - don't pull in Qt's debug DLLs.
    #   --no-translations         pixet ships no .qm files; skips ~a dozen copied
    #                             speculatively.
    #   --no-opengl-sw            opengl32sw.dll is Mesa's software GL rasterizer (20MB,
    #                             the single largest file windeployqt adds). It is a
    #                             fallback for machines with no usable GL driver, which
    #                             only matters to a Qt Quick / QOpenGLWidget app. pixet
    #                             links Qt6::Widgets alone (src/app/CMakeLists.txt) and
    #                             paints through the raster engine.
    #   --no-system-d3d-compiler  d3dcompiler_47.dll (4.7MB) - HLSL compiler for Qt RHI's
    #                             D3D11 backend. Same reasoning: nothing here uses RHI.
    #   --no-system-dxc-compiler  dxcompiler.dll + dxil.dll (13.5MB) - the newer DXC pair
    #                             for RHI's D3D12 backend. Same reasoning again.
    #
    # NOT passed: --no-compiler-runtime. That would drop vc_redist.x64.exe, which pixet.iss
    # requires (see its [Files]/[Run] sections) and which windeployqt is the only thing
    # staging - see the redist note in SETUP.md step 8.
    & $windeployqt --release --no-translations --no-opengl-sw --no-system-d3d-compiler --no-system-dxc-compiler "$stage\pixet.exe"
    if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

    Write-Output "==> Pruning Qt plugins this app never loads"
    # Direct counterpart to deploy-mac.sh's own plugin prune - see that script for the
    # longer version of this reasoning. windeployqt deploys the default plugin set for the
    # linked modules, which is the right default and wrong for this app:
    #
    #   imageformats/  qgif, qico, qjpeg, qsvg - every decode is pixet's own codec via
    #                  rgbImageToQImage, so these are loaded at startup and never used.
    #   iconengines/   qsvgicon - only reachable through QIcon loading an .svg; the app
    #                  icon is a .ico/.png resource (src/app/icons.qrc).
    #   generic/       qtuiotouchplugin - TUIO tablet/touch protocol over UDP.
    #   networkinformation/, tls/  - Qt6Network's plugins. pixet makes no network calls;
    #                  these are here only because windeployqt saw Qt6Network, which is
    #                  itself only here because these plugins link it.
    #
    # An allowlist rather than a delete-list, so a future Qt version adding new default
    # plugins doesn't silently re-inflate the installer. platforms/qwindows.dll is what
    # makes the app start at all; styles/qmodernwindowsstyle.dll is the native Windows 11
    # look, and dropping it visibly regresses the UI to Qt's Fusion fallback.
    $keepPluginDirs = @("platforms", "styles")
    foreach ($dir in Get-ChildItem -Path $stage -Directory) {
        if ($keepPluginDirs -contains $dir.Name) { continue }
        $kb = [math]::Round((Get-ChildItem $dir.FullName -Recurse -File | Measure-Object -Property Length -Sum).Sum / 1KB)
        Write-Output "    dropping $($dir.Name)\ ($kb KB)"
        Remove-Item -Recurse -Force $dir.FullName
    }

    Write-Output "==> Dropping Qt DLLs nothing left in the staging folder links against"
    # Then drop any Qt DLL nothing that survived the prune actually imports. Derived rather
    # than hardcoded, for the same reason deploy-mac.sh derives its dead-framework list:
    # Qt6Svg and Qt6Network are dead only as a *consequence* of the plugins removed above,
    # and naming them here would rot the moment that plugin set changed. Looped, because
    # removing one DLL can orphan another.
    #
    # Scoped to Qt6*.dll on purpose. The vcpkg DLLs alongside them were placed by vcpkg's
    # applocal step, which already derives them from import tables - re-deriving them here
    # would at best agree and at worst delete something reached via LoadLibrary.
    $dumpbin = (Get-Command dumpbin.exe -ErrorAction SilentlyContinue).Source
    if (-not $dumpbin) {
        # Should not happen: configure.ps1 entered the VS dev shell in this same process a
        # few steps ago. Fall back to locating it the way configure.ps1 locates the shell.
        $vswhere = "C:\Program Files (x86)\Microsoft Visual Studio\Installer\vswhere.exe"
        $vsPath = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
        $dumpbin = Get-ChildItem -Path "$vsPath\VC\Tools\MSVC" -Filter dumpbin.exe -Recurse -ErrorAction SilentlyContinue |
                   Where-Object { $_.FullName -like "*\Hostx64\x64\*" } |
                   Select-Object -First 1 -ExpandProperty FullName
    }
    if (-not $dumpbin) {
        Write-Error "dumpbin.exe not found - cannot determine which Qt DLLs are unused"
        exit 1
    }

    # One dumpbin pass over everything staged - the two executables, the surviving plugins,
    # and the Qt DLLs themselves (Qt6Widgets imports Qt6Gui, so inter-DLL edges count).
    # Read once into a map rather than per candidate: deleting a file never changes what
    # the *others* import, so the graph is fixed and re-running dumpbin per pass would just
    # be the same answer at ~10x the process spawns.
    $imports = @{}
    foreach ($bin in Get-ChildItem -Path $stage -Recurse -File -Include *.exe, *.dll) {
        # vc_redist.x64.exe is a self-extracting installer, not something that links Qt.
        if ($bin.Name -eq "vc_redist.x64.exe") { continue }
        $imports[$bin.FullName] = (& $dumpbin /nologo /dependents $bin.FullName 2>$null) -join "`n"
    }

    while ($true) {
        $dropped = $false
        foreach ($qtDll in Get-ChildItem -Path $stage -Filter "Qt6*.dll") {
            $used = $false
            foreach ($binPath in $imports.Keys) {
                # Skip the candidate itself, and anything already dropped by an earlier
                # pass - an orphan's own imports must not keep its dependencies alive.
                if ($binPath -eq $qtDll.FullName) { continue }
                if (-not (Test-Path $binPath)) { continue }
                if ($imports[$binPath] -match [regex]::Escape($qtDll.Name)) { $used = $true; break }
            }
            if (-not $used) {
                $kb = [math]::Round($qtDll.Length / 1KB)
                Write-Output "    dropping $($qtDll.Name) ($kb KB)"
                Remove-Item -Force $qtDll.FullName
                $dropped = $true
            }
        }
        if (-not $dropped) { break }
    }

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
