#!/usr/bin/env bash
#
# Configures pixet on macOS. Counterpart to configure.ps1.
#
# Far shorter than the Windows version, and that's the whole point: ~80% of
# configure.ps1 is MSVC dev-shell plumbing (vswhere, Enter-VsDevShell, merging the
# registry PATH back in) with no macOS analogue at all - Apple clang from the Command
# Line Tools is already on PATH with the correct sysroot. What genuinely carries over is
# only the ordering: vcpkg's dependency install happens during *this* step (configure),
# not during build, so the binary cache has to be set up before `cmake --preset` runs.
#
# Usage: ./scripts/configure.sh [mac-debug|mac-release]
#
# A cold run builds every vcpkg dependency from source (sqlite3, libjpeg-turbo, libpng,
# tiff, webp, avif, libraw, libheif+libde265, ffmpeg) - budget ~30 minutes and ~10-15GB
# of transient space under vcpkg/buildtrees, which vcpkg does not clean up on its own.

set -euo pipefail

PRESET="${1:-mac-debug}"

case "$PRESET" in
    mac-debug|mac-release) ;;
    debug|release)
        echo "error: '$PRESET' is a Windows preset - it's condition-gated to Windows in" >&2
        echo "       CMakePresets.json and won't resolve here. Use mac-debug or mac-release." >&2
        exit 2
        ;;
    *)
        echo "usage: $0 [mac-debug|mac-release]" >&2
        exit 2
        ;;
esac

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

# Fail with something readable rather than letting CMake report a 200-line
# "Could not find a package configuration file provided by Qt6" wall. The path here has
# to stay in sync with mac-base's CMAKE_PREFIX_PATH in CMakePresets.json.
QT_DIR="$HOME/Qt/6.8.3/macos"
if [ ! -f "$QT_DIR/lib/cmake/Qt6/Qt6Config.cmake" ]; then
    echo "error: no Qt6 at $QT_DIR" >&2
    echo "       Install it with:" >&2
    echo "         aqt install-qt mac desktop 6.8.3 clang_64 --outputdir ~/Qt" >&2
    echo "       If your Qt lives elsewhere or is a different version, do NOT edit the" >&2
    echo "       committed CMakePresets.json - create a gitignored CMakeUserPresets.json" >&2
    echo "       inheriting from mac-debug and override CMAKE_PREFIX_PATH there." >&2
    exit 1
fi

if [ ! -x "$REPO_ROOT/vcpkg/vcpkg" ]; then
    echo "error: vcpkg isn't bootstrapped. Run:" >&2
    echo "         git submodule update --init" >&2
    echo "         ./vcpkg/bootstrap-vcpkg.sh -disableMetrics" >&2
    exit 1
fi

# shellcheck source=scripts/vcpkg-cache-env.sh
. "$(dirname "${BASH_SOURCE[0]}")/vcpkg-cache-env.sh"

cd "$REPO_ROOT"
exec cmake --preset "$PRESET"
