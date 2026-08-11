#!/usr/bin/env bash
#
# Builds pixet on macOS. Counterpart to build.ps1.
#
# build.ps1 exists mostly to work around MSVC environment injection being unreliable
# under VS Code's CMake Tools (see its header - "Cannot open include file: 'type_traits'").
# None of that applies here, so this script is thin. It still sources the vcpkg cache
# env for the same reason build.ps1 does: editing CMakeLists.txt or vcpkg.json triggers
# an implicit reconfigure during build, which can pull in a vcpkg install, which wants
# the cache configured.
#
# Usage: ./scripts/build.sh [mac-debug|mac-release] [extra cmake --build args...]
#   e.g. ./scripts/build.sh mac-debug -j 4
#
# Defaults to a parallel build across all cores; pass -j N to override.

set -euo pipefail

PRESET="${1:-mac-debug}"
shift || true

case "$PRESET" in
    mac-debug|mac-release) ;;
    debug|release)
        echo "error: '$PRESET' is a Windows preset. Use mac-debug or mac-release." >&2
        exit 2
        ;;
    *)
        echo "usage: $0 [mac-debug|mac-release] [extra cmake --build args...]" >&2
        exit 2
        ;;
esac

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

if [ ! -d "$REPO_ROOT/build/$PRESET" ]; then
    echo "error: build/$PRESET doesn't exist - run ./scripts/configure.sh $PRESET first." >&2
    exit 1
fi

# shellcheck source=scripts/vcpkg-cache-env.sh
. "$(dirname "${BASH_SOURCE[0]}")/vcpkg-cache-env.sh"

cd "$REPO_ROOT"
if [ "$#" -eq 0 ]; then
    exec cmake --build "build/$PRESET" --parallel
fi
exec cmake --build "build/$PRESET" "$@"
