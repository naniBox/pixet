<#
.SYNOPSIS
    Deletes build output: every preset's build tree and the deploy staging folders.

.DESCRIPTION
    Everything under build/ goes - every configure preset's tree (debug, release, and
    mac-debug/mac-release when this runs on a Mac checkout), any stray hand-made one (a
    debug2), and the deploy staging folders (win-deploy, dmg-stage) - except the finished
    installers, pixet-*-setup.exe and pixet-*-arm64.dmg, which stay unless -Installers is
    given. Those are the one thing in build/ a rebuild can't give back for an older version
    without checking out its tag, so losing them is opt-in.

    Deliberately never touches the vcpkg *binary* caches (%LOCALAPPDATA%\vcpkg\archives and
    the shared one on \\kioku - see vcpkg-cache-env.ps1). Each preset's vcpkg_installed/ goes
    with its tree, and those caches are what restore it on the next configure in seconds
    rather than the ~30 minutes a from-source rebuild of every dependency takes.

    -Vcpkg also deletes vcpkg/buildtrees and vcpkg/packages: vcpkg's scratch space from
    building dependencies from source, which it never cleans up itself (10-15GB after a
    cold build - see SETUP.md). vcpkg/downloads, the source archives, is left alone.

    Afterwards build.ps1 has no tree to build into - run configure.ps1 first.

.PARAMETER Installers
    Delete the installers in build/ as well.

.PARAMETER Vcpkg
    Delete vcpkg/buildtrees and vcpkg/packages as well.

.EXAMPLE
    ./scripts/clean.ps1 -WhatIf

    Lists what would be deleted, and deletes nothing.

.EXAMPLE
    ./scripts/clean.ps1 -Installers -Vcpkg
#>
[CmdletBinding(SupportsShouldProcess)]
param(
    [switch]$Installers,
    [switch]$Vcpkg
)

$ErrorActionPreference = "Stop"

$repoRoot = Split-Path -Parent $PSScriptRoot
$buildDir = Join-Path $repoRoot "build"
$installerPattern = '^pixet-.+-(setup\.exe|arm64\.dmg)$'

# A pixet.exe, pixet-index.exe or pixet_tests.exe still running out of a build tree holds
# its exe and DLLs open, and Windows won't delete an open file - so the clean would fail
# partway through, leaving a half-deleted tree that the next configure trips over. Checked
# up front instead. Filtered by path so an installed pixet (Program Files) doesn't count.
$running = Get-Process -Name pixet, pixet-index, pixet_tests -ErrorAction SilentlyContinue |
    Where-Object { $_.Path -and $_.Path.StartsWith("$buildDir\", [StringComparison]::OrdinalIgnoreCase) }
if ($running) {
    $running | ForEach-Object { Write-Output "  running: $($_.Path) (pid $($_.Id))" }
    Write-Error "Close the programs above first - their files can't be deleted while they run."
    exit 1
}

$targets = @()
$kept = @()
if (Test-Path -LiteralPath $buildDir) {
    foreach ($item in Get-ChildItem -LiteralPath $buildDir -Force) {
        if (-not $Installers -and $item.Name -match $installerPattern) { $kept += $item.Name }
        else { $targets += $item }
    }
}
if ($Vcpkg) {
    foreach ($name in "buildtrees", "packages") {
        $path = Join-Path $repoRoot "vcpkg\$name"
        if (Test-Path -LiteralPath $path) { $targets += Get-Item -LiteralPath $path -Force }
    }
}

if ($targets.Count -eq 0) {
    Write-Output "Nothing to clean."
} else {
    $failed = @()
    foreach ($target in $targets) {
        # Substring rather than [IO.Path]::GetRelativePath(), which Windows PowerShell 5.1
        # doesn't have - every target is under $repoRoot by construction.
        $relative = $target.FullName.Substring($repoRoot.Length + 1)
        # ShouldProcess is what makes -WhatIf print "What if: ..." here and skip the delete.
        if (-not $PSCmdlet.ShouldProcess($relative, "Delete")) { continue }
        Write-Output "==> Deleting $relative"
        try {
            Remove-Item -LiteralPath $target.FullName -Recurse -Force
        } catch {
            # Carries on with the rest rather than stopping at the first failure, so one
            # locked file costs one entry, not the whole clean.
            Write-Warning "Could not fully delete ${relative}: $($_.Exception.Message)"
            $failed += $relative
        }
    }
    if ($failed.Count -gt 0) {
        Write-Error "Failed to delete: $($failed -join ', ')"
        exit 1
    }
}

if ($kept.Count -gt 0) {
    Write-Output "Kept $($kept.Count) installer(s) in build/ (-Installers deletes them too):"
    $kept | ForEach-Object { Write-Output "  $_" }
}
if (-not $WhatIfPreference -and $targets.Count -gt 0) {
    Write-Output ""
    Write-Output "Next: ./scripts/configure.ps1 -Preset debug (and/or release), then build.ps1."
}
