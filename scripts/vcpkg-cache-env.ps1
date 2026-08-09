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

    "clear;default,readwrite;files,<path>,readwrite" - clear resets to a known state,
    `default` keeps using the normal local cache (so this machine's existing cache still
    applies), and the `files` source adds the shared folder as an additional cache tier -
    vcpkg checks both, and writes newly-built packages to both.

    The ",readwrite" on `default` is NOT optional/redundant, despite the vcpkg docs'
    prose ("By default, vcpkg will save builds to a local machine cache") reading like
    it's implied. Verified directly: `"clear;default"` alone silently never wrote to
    %LOCALAPPDATA%\vcpkg\archives on a fresh build in this vcpkg version (confirmed via
    `vcpkg install --debug`, which logs "Starting/Completed submission... to 1 binary
    cache(s)" only when an explicit ",readwrite" is present) - bare `default` is
    apparently read-only unless told otherwise, exactly like `files`. Original version
    of this script had this bug: it wrote to the network share but silently never
    refreshed the local cache on a rebuild.

    Dot-sourced by configure.ps1 and build.ps1 - not meant to be run directly.
#>

$vcpkgCacheDir = "\\kioku\talsit\code\pixet\vcpkg-cache"
if (-not (Test-Path $vcpkgCacheDir)) {
    New-Item -ItemType Directory -Force -Path $vcpkgCacheDir | Out-Null
}

$env:VCPKG_BINARY_SOURCES = "clear;default,readwrite;files,$vcpkgCacheDir,readwrite"
