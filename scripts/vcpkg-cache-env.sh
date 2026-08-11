# Sets up vcpkg's binary cache for macOS builds.
#
# Sourced by configure.sh and build.sh - not meant to be run directly.
#
# This is the macOS counterpart to vcpkg-cache-env.ps1, and it deliberately does NOT
# mirror it. Two differences, both intentional:
#
# 1. No shared network tier by default. The Windows script adds
#    "files,\\kioku\talsit\code\pixet\vcpkg-cache,readwrite" as a second cache tier so
#    the two dev machines can skip each other's cold builds. That does nothing for a
#    Mac: a vcpkg binary cache entry's ABI hash includes the compiler and the triplet,
#    so nothing an MSVC/x64-windows build ever pushed to that share is a cache hit for
#    an Apple-clang/arm64-osx build. The share would only ever pay off between two
#    Macs. Set PIXET_VCPKG_CACHE_DIR to add it back (see below) if that day comes.
#
# 2. `default` resolves to ~/.cache/vcpkg/archives here, not %LOCALAPPDATA%\vcpkg\archives.
#
# The ",readwrite" on `default` is NOT optional/redundant, despite the vcpkg docs'
# prose ("By default, vcpkg will save builds to a local machine cache") reading like
# it's implied. vcpkg-cache-env.ps1's header records this being verified directly on
# Windows: bare `default` silently never *wrote* to the local cache, making every
# rebuild cold. Carried over here rather than re-derived, since it's a vcpkg behavior
# and not a platform one - if you're tempted to simplify this to "clear;default",
# read that comment first.

# Optional extra cache tier, e.g. a mounted SMB share:
#   PIXET_VCPKG_CACHE_DIR=/Volumes/talsit/code/pixet/vcpkg-cache-osx ./scripts/configure.sh mac-debug
# Skipped silently if it doesn't exist - unlike the Windows script, which creates the
# directory, because an unreachable mount point here means "the share isn't mounted",
# not "the folder needs making".
if [ -n "${PIXET_VCPKG_CACHE_DIR:-}" ] && [ -d "${PIXET_VCPKG_CACHE_DIR}" ]; then
    export VCPKG_BINARY_SOURCES="clear;default,readwrite;files,${PIXET_VCPKG_CACHE_DIR},readwrite"
    echo "vcpkg binary cache: local + ${PIXET_VCPKG_CACHE_DIR}"
else
    if [ -n "${PIXET_VCPKG_CACHE_DIR:-}" ]; then
        echo "note: PIXET_VCPKG_CACHE_DIR=${PIXET_VCPKG_CACHE_DIR} does not exist - using the local cache only" >&2
    fi
    export VCPKG_BINARY_SOURCES="clear;default,readwrite"
fi
