<#
.SYNOPSIS
    Sets up a vcpkg binary cache shared between both dev machines via a network share.

.DESCRIPTION
    vcpkg's own binary caching (the default %LOCALAPPDATA%\vcpkg\archives) is what
    makes a *second* `cmake --preset` on the same machine take seconds instead of the
    ~28 minutes a genuinely cold vcpkg cache needs to build every dependency from
    source. That default cache is local to one machine, though - a fresh clone on the
    other machine starts cold. Pointing vcpkg's cache at a shared network location
    (\\kioku\talsit\code\pixet\vcpkg-cache) means whichever machine builds a dependency
    first saves the other machine from ever rebuilding it - not synced storage, so
    nothing is mirrored/copied without the user doing it deliberately.

    "clear;default;files,<path>,readwrite" - clear resets to a known state, `default`
    keeps using the normal local cache (so this machine's existing cache still applies),
    and the `files` source adds the shared folder as an additional read/write cache
    tier - vcpkg checks both, and writes newly-built packages to both.

    Dot-sourced by configure.ps1 and build.ps1 - not meant to be run directly.
#>

$vcpkgCacheDir = "\\kioku\talsit\code\pixet\vcpkg-cache"
if (-not (Test-Path $vcpkgCacheDir)) {
    New-Item -ItemType Directory -Force -Path $vcpkgCacheDir | Out-Null
}

$env:VCPKG_BINARY_SOURCES = "clear;default;files,$vcpkgCacheDir,readwrite"
