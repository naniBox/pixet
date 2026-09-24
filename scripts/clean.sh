#!/usr/bin/env bash
#
# Deletes build output. Counterpart to clean.ps1 - see its header for exactly what goes,
# what stays, and why: in short, everything under build/ except the finished installers,
# and never the vcpkg binary caches.
#
# Usage: ./scripts/clean.sh [--installers] [--vcpkg] [--dry-run]
#   --installers  also delete pixet-*-setup.exe / pixet-*-arm64.dmg in build/
#   --vcpkg       also delete vcpkg/buildtrees and vcpkg/packages (vcpkg's scratch space)
#   --dry-run     list what would be deleted, delete nothing (-n for short)
#
# No "is pixet still running from here?" check like clean.ps1 has: that exists because
# Windows refuses to delete an open file, and macOS doesn't - a running binary's file can
# be unlinked out from under it.

set -euo pipefail

usage() { sed -n '7,10p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'; }

INSTALLERS=0
VCPKG=0
DRY_RUN=0
for arg in "$@"; do
    case "$arg" in
        --installers) INSTALLERS=1 ;;
        --vcpkg) VCPKG=1 ;;
        -n|--dry-run) DRY_RUN=1 ;;
        -h|--help) usage; exit 0 ;;
        *) echo "error: unknown option '$arg'" >&2; usage >&2; exit 2 ;;
    esac
done

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="$REPO_ROOT/build"
INSTALLER_RE='^pixet-.+-(setup\.exe|arm64\.dmg)$'

targets=()
kept=()
if [ -d "$BUILD_DIR" ]; then
    shopt -s nullglob dotglob
    for path in "$BUILD_DIR"/*; do
        name="$(basename "$path")"
        if [ "$INSTALLERS" -eq 0 ] && [[ "$name" =~ $INSTALLER_RE ]]; then
            kept+=("$name")
        else
            targets+=("$path")
        fi
    done
    shopt -u nullglob dotglob
fi
if [ "$VCPKG" -eq 1 ]; then
    for name in buildtrees packages; do
        if [ -e "$REPO_ROOT/vcpkg/$name" ]; then targets+=("$REPO_ROOT/vcpkg/$name"); fi
    done
fi

if [ "${#targets[@]}" -eq 0 ]; then
    echo "Nothing to clean."
else
    for path in "${targets[@]}"; do
        relative="${path#"$REPO_ROOT"/}"
        if [ "$DRY_RUN" -eq 1 ]; then
            echo "would delete: $relative"
        else
            echo "==> Deleting $relative"
            rm -rf "$path"
        fi
    done
fi

if [ "${#kept[@]}" -gt 0 ]; then
    echo "Kept ${#kept[@]} installer(s) in build/ (--installers deletes them too):"
    printf '  %s\n' "${kept[@]}"
fi
if [ "$DRY_RUN" -eq 0 ] && [ "${#targets[@]}" -gt 0 ]; then
    echo
    echo "Next: ./scripts/configure.sh mac-debug (and/or mac-release), then build.sh."
fi
