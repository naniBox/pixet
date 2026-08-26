#!/usr/bin/env bash
#
# Builds a self-contained, signed pixet.app and wraps it in a DMG.
#
# Usage: ./scripts/deploy-mac.sh
#
# What "self-contained" costs here is small, and worth knowing why: the vcpkg triplet is
# static (see triplets/arm64-osx-pixet.cmake), so every codec - ffmpeg, libheif, libraw,
# libjpeg-turbo and the rest - is already inside the executable. Only Qt is dynamic, so
# macdeployqt copying the Qt frameworks and the Cocoa platform plugin is the entire
# deployment step. There is no equivalent of Windows' DLL-herding problem here.
#
# ---------------------------------------------------------------------------------------
# Signing
#
# Defaults to ad-hoc signing ("-"), which is enough to *run* locally but NOT enough to hand
# to someone else without friction: a downloaded DMG gets the com.apple.quarantine attribute,
# and Gatekeeper will refuse an app that isn't Developer-ID-signed and notarized. Recipients
# then have to launch it once, get refused, and go to System Settings > Privacy & Security >
# "Open Anyway" (the Control-click > Open bypass stopped working in macOS 15). Or, from a
# terminal:  xattr -dr com.apple.quarantine /Applications/pixet.app
#
# To produce something that just opens, with no warning, you need an Apple Developer Program
# membership ($99/year) and then only environment variables - no edits to this script:
#
#   PIXET_CODESIGN_IDENTITY="Developer ID Application: Your Name (TEAMID)" \
#   PIXET_NOTARY_PROFILE=my-notary-profile \
#     ./scripts/deploy-mac.sh
#
# (PIXET_NOTARY_PROFILE is a keychain profile you store beforehand with
#  `xcrun notarytool store-credentials`.)
#
# The hardened runtime is enabled only when a real identity is supplied, and that is not
# timidity - enabling it with an ad-hoc signature produces an app that cannot launch at all:
#
#   dyld: Library not loaded: @rpath/QtWidgets.framework/...
#         code signature ... not valid for use in process:
#         mapping process and mapped file (non-platform) have different Team IDs
#
# The hardened runtime turns on *library validation*, which only lets a process load libraries
# sharing its own Team ID. Ad-hoc signatures have no Team ID, so every embedded Qt framework
# gets rejected - found, then refused. With a Developer ID the whole bundle is signed by one
# team and it's a non-issue, which is why it's switched on exactly there and nowhere else.
# (The alternative, a com.apple.security.cs.disable-library-validation entitlement, would
# weaken the notarized build to fix the unsigned one. Wrong trade.)

set -euo pipefail

PRESET="mac-release"
IDENTITY="${PIXET_CODESIGN_IDENTITY:--}"
NOTARY_PROFILE="${PIXET_NOTARY_PROFILE:-}"

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$REPO_ROOT"

QT_DIR="$HOME/Qt/6.8.3/macos"
MACDEPLOYQT="$QT_DIR/bin/macdeployqt"
if [ ! -x "$MACDEPLOYQT" ]; then
    echo "error: macdeployqt not found at $MACDEPLOYQT" >&2
    exit 1
fi

APP="$REPO_ROOT/build/$PRESET/src/app/pixet.app"
STAGE="$REPO_ROOT/build/dmg-stage"

echo "==> Configuring and building ($PRESET)"
# PIXET_DEBUG_MENU=OFF is the point of building through this script rather than reusing an
# existing build tree: the &Debug menu ("Copy Grid Debug Info") is deliberately kept in local
# release builds - see MainWindow.h's comment asking for it permanently - but it has no
# business appearing in the menu bar of a build handed to someone else.
./scripts/configure.sh "$PRESET" -DPIXET_DEBUG_MENU=OFF
./scripts/build.sh "$PRESET"

if [ ! -d "$APP" ]; then
    echo "error: $APP was not produced by the build" >&2
    exit 1
fi

VERSION="$(plutil -extract CFBundleShortVersionString raw "$APP/Contents/Info.plist")"
echo "==> pixet $VERSION"

echo "==> Running macdeployqt (embedding Qt frameworks + the Cocoa plugin)"
# -always-overwrite so a re-run doesn't leave a half-updated Frameworks directory behind.
# Deliberately not passing -dmg: that would build the disk image *before* signing, and the
# app inside it has to be signed first.
"$MACDEPLOYQT" "$APP" -always-overwrite

echo "==> Pruning Qt plugins this app never loads"
# macdeployqt deploys the default plugin set for the linked modules, which is the right
# default and wrong for this app: every image decoder is pixet's own (see QtInterop.h and
# main.cpp - avoiding a runtime dependency on Qt's image plugins is a deliberate design
# choice, not an accident), so the imageformats and iconengines plugins are loaded at startup
# and then never used for anything.
#
# An allowlist rather than a list of things to delete, so a future Qt version adding new
# default plugins doesn't silently re-inflate the bundle. Verified with QT_DEBUG_PLUGINS=1:
# libqcocoa (the platform plugin - without it the app cannot start) and libqmacstyle (the
# native look) are the only two that matter.
KEEP_PLUGIN_DIRS=(platforms styles)
for dir in "$APP"/Contents/PlugIns/*/; do
    [ -d "$dir" ] || continue
    name="$(basename "$dir")"
    keep=0
    for k in "${KEEP_PLUGIN_DIRS[@]}"; do [ "$name" = "$k" ] && keep=1; done
    if [ "$keep" -eq 0 ]; then
        echo "    dropping PlugIns/$name ($(du -sh "$dir" | cut -f1))"
        rm -rf "$dir"
    fi
done

# Then drop any framework nothing left in the bundle actually links against. Derived rather
# than hardcoded, because the answer changes as plugins come and go: QtSvg is here only to
# serve the two SVG plugins removed just above, so it becomes dead as a *consequence* of that
# and a hardcoded list would rot the moment the plugin set changed. Looped because removing
# one framework can orphan another. Note QtDBus correctly survives this - QtGui links it
# directly in the official macOS Qt build, so it isn't optional however unused it looks.
while :; do
    dropped=0
    for fw in "$APP"/Contents/Frameworks/*.framework; do
        [ -d "$fw" ] || continue
        name="$(basename "$fw" .framework)"
        users=0
        while IFS= read -r macho; do
            # Skip the framework's own binary: otool -L reports a library's own install name
            # (LC_ID_DYLIB), so every framework "references" itself and nothing would ever
            # look unused.
            case "$macho" in "$fw"/*) continue ;; esac
            if otool -L "$macho" 2>/dev/null | tail -n +2 | grep -q "${name}\.framework"; then
                users=1
                break
            fi
        done < <(find "$APP/Contents/MacOS" "$APP/Contents/PlugIns" "$APP/Contents/Frameworks" \
                      -type f \( -perm -u+x -o -name "*.dylib" \) 2>/dev/null)
        if [ "$users" -eq 0 ]; then
            echo "    dropping Frameworks/$name.framework ($(du -sh "$fw" | cut -f1))"
            rm -rf "$fw"
            dropped=1
        fi
    done
    [ "$dropped" -eq 0 ] && break
done

echo "==> Thinning universal Qt binaries to this app's architecture"
# Qt's official macOS binaries are universal (x86_64 + arm64). Our own binaries are not: the
# vcpkg triplet is arm64-osx, so the app cannot run on Intel at all - which makes the embedded
# x86_64 halves of Qt 23MB of code that can never execute. Roughly a quarter of the bundle.
#
# Derived from the app binary rather than hardcoded to arm64, and skipped entirely if the app
# is itself universal, so this stays correct if CMAKE_OSX_ARCHITECTURES is ever widened to
# build for Intel too (see the root CMakeLists - it defaults to the host precisely to keep that
# a one-line change). Must happen before signing: lipo rewrites the binary and invalidates any
# signature already on it.
APP_ARCHS="$(lipo -archs "$APP/Contents/MacOS/pixet")"
if [ "$(echo "$APP_ARCHS" | wc -w)" -eq 1 ]; then
    while IFS= read -r macho; do
        [ "$(lipo -archs "$macho" 2>/dev/null | wc -w)" -gt 1 ] || continue
        lipo -thin "$APP_ARCHS" "$macho" -output "$macho.thin" 2>/dev/null || continue
        mv "$macho.thin" "$macho"
    done < <(find "$APP/Contents/Frameworks" "$APP/Contents/PlugIns" \
                  -type f \( -perm -u+x -o -name "*.dylib" \) 2>/dev/null)
    echo "    thinned to $APP_ARCHS"
else
    echo "    app is universal ($APP_ARCHS) - leaving Qt as-is"
fi

echo "==> Adding pixet-index to the bundle"
# Shipped inside Contents/MacOS rather than loose next to the .app in the DMG, for two
# reasons: it travels with the app (drag-installing to /Applications takes it along, and it
# can't get separated from the GUI it shares a database format with), and it sits inside the
# code signature rather than beside it, which is what a future notarized build will require.
#
# It needs nothing else to run - the vcpkg triplet is static, so unlike the GUI this binary
# links only system frameworks and libc++. No Qt, no rpath, no bundle: it works from any
# directory, which is the whole point of the symlink suggested at the end of this script.
INDEX_BIN="$REPO_ROOT/build/$PRESET/src/index/pixet-index"
if [ ! -x "$INDEX_BIN" ]; then
    echo "error: $INDEX_BIN was not produced by the build" >&2
    exit 1
fi
cp "$INDEX_BIN" "$APP/Contents/MacOS/pixet-index"

# -x drops local symbols and keeps the global/dynamic ones, which is all a release binary
# needs. Worth 3MB on pixet-index alone. Deliberately here and not in the CMake release
# flags, so local builds stay debuggable in lldb - stripping is a distribution concern.
strip -x "$APP/Contents/MacOS/pixet"
strip -x "$APP/Contents/MacOS/pixet-index"

echo "==> Signing (identity: $IDENTITY)"
# --options runtime and --timestamp only apply to a real identity: see the header comment for
# why the hardened runtime breaks an ad-hoc-signed bundle outright, and --timestamp needs a
# trusted identity plus a network round-trip to Apple to mean anything.
SIGN_OPTS=(--force)
if [ "$IDENTITY" != "-" ]; then
    SIGN_OPTS+=(--timestamp --options runtime)
fi

# Sign inside-out: nested Mach-O files first, then each framework bundle, then the app.
# --deep is deprecated and documented as missing nested code, so this walks it explicitly.
while IFS= read -r binary; do
    codesign "${SIGN_OPTS[@]}" --sign "$IDENTITY" "$binary" 2>/dev/null || true
done < <(find "$APP/Contents/Frameworks" "$APP/Contents/PlugIns" -type f \( -name "*.dylib" -o -perm -u+x \) 2>/dev/null)

for fw in "$APP"/Contents/Frameworks/*.framework; do
    [ -d "$fw" ] || continue
    codesign "${SIGN_OPTS[@]}" --sign "$IDENTITY" "$fw"
done
# Signed explicitly, before the bundle. Signing the .app seals Contents/MacOS as part of the
# bundle, but a nested Mach-O executable needs its own signature to satisfy notarization -
# codesign --verify --deep --strict below is what would otherwise catch this later.
codesign "${SIGN_OPTS[@]}" --sign "$IDENTITY" "$APP/Contents/MacOS/pixet-index"
codesign "${SIGN_OPTS[@]}" --sign "$IDENTITY" "$APP"

echo "==> Verifying signature"
codesign --verify --deep --strict --verbose=2 "$APP"

echo "==> Verifying nothing still links against the build machine's Qt"
# Cheap guard against the classic "works only on the machine that built it" bundle. Not a
# substitute for the ~/Qt-moved-aside test printed at the end - that one also catches
# signature problems this can't see - but it catches a macdeployqt step that silently failed
# to rewrite an install name, without touching the user's Qt install.
LEAKED=0
while IFS= read -r macho; do
    if otool -L "$macho" 2>/dev/null | tail -n +2 | grep -q "$HOME/Qt"; then
        echo "  LEAK: $macho still references $HOME/Qt" >&2
        LEAKED=1
    fi
done < <(find "$APP/Contents/MacOS" "$APP/Contents/Frameworks" "$APP/Contents/PlugIns" \
              -type f \( -name "*.dylib" -o -perm -u+x \) 2>/dev/null)
if [ "$LEAKED" -ne 0 ]; then
    echo "error: the bundle is not self-contained - see the leaks above." >&2
    exit 1
fi
echo "  clean - all Qt references are @rpath/@executable_path relative"

echo "==> Building DMG"
rm -rf "$STAGE"
mkdir -p "$STAGE"
cp -R "$APP" "$STAGE/"
# The Applications symlink is what makes the DMG a drag-to-install window rather than
# something the recipient has to be told what to do with.
ln -s /Applications "$STAGE/Applications"

# Two separate icons, and they're set by completely different mechanisms:
#
#  1. The mounted *volume's* icon - what Finder shows in the sidebar and on the desktop when
#     the DMG is opened. Comes from a .VolumeIcon.icns at the volume root plus the volume
#     having its custom-icon flag set, which can only be done while it's mounted read-write.
#     Hence build UDRW first, flag it, then convert to the compressed read-only UDZO that
#     actually ships.
#  2. The .dmg *file's* own icon in Finder, before anyone opens it. That one lives in the
#     file's resource fork, so it needs the sips/DeRez/Rez dance below - setting
#     .VolumeIcon.icns does nothing for it.
#
# Doing only (1) is the common half-measure: the disk image still looks like a generic white
# drive until it's mounted, which is exactly when a recipient is deciding whether to trust it.
ICON="$REPO_ROOT/src/app/pixet.icns"
cp "$ICON" "$STAGE/.VolumeIcon.icns"

DMG="$REPO_ROOT/build/pixet-$VERSION-arm64.dmg"
RW_DMG="$REPO_ROOT/build/pixet-$VERSION-rw.dmg"
rm -f "$DMG" "$RW_DMG"

hdiutil create -volname "pixet $VERSION" -srcfolder "$STAGE" -ov -format UDRW "$RW_DMG" >/dev/null
# -nobrowse/-noautoopen so building a release doesn't throw a Finder window in your face.
MOUNT_POINT="$(hdiutil attach "$RW_DMG" -nobrowse -noautoopen | grep -Eo '/Volumes/.*$' | tail -1)"
if [ -n "$MOUNT_POINT" ]; then
    SetFile -a C "$MOUNT_POINT"          # kHasCustomIcon on the volume root
    hdiutil detach "$MOUNT_POINT" >/dev/null
fi
hdiutil convert "$RW_DMG" -format UDZO -o "$DMG" >/dev/null
rm -f "$RW_DMG"
rm -rf "$STAGE"

echo "==> Applying the app icon to the .dmg file itself"
# sips -i builds an icon resource *inside* the .icns; DeRez extracts it as a resource
# description; Rez appends that to the .dmg's resource fork; SetFile flags the file as having
# a custom icon. All four steps are required - any one alone does nothing visible.
ICON_TMP="$(mktemp -d)"
cp "$ICON" "$ICON_TMP/icon.icns"
sips -i "$ICON_TMP/icon.icns" >/dev/null 2>&1
DeRez -only icns "$ICON_TMP/icon.icns" > "$ICON_TMP/icon.rsrc" 2>/dev/null
Rez -append "$ICON_TMP/icon.rsrc" -o "$DMG" 2>/dev/null
SetFile -a C "$DMG"
rm -rf "$ICON_TMP"

if [ -n "$NOTARY_PROFILE" ]; then
    echo "==> Notarizing (profile: $NOTARY_PROFILE)"
    xcrun notarytool submit "$DMG" --keychain-profile "$NOTARY_PROFILE" --wait
    xcrun stapler staple "$DMG"
    echo "==> Notarized and stapled"
else
    echo
    echo "NOTE: not notarized (no PIXET_NOTARY_PROFILE set)."
    if [ "$IDENTITY" = "-" ]; then
        echo "      Ad-hoc signed, so anyone you send this to will hit Gatekeeper. Tell them:"
        echo "        open it once, let it be refused, then System Settings >"
        echo "        Privacy & Security > \"Open Anyway\"."
        echo "      Or:  xattr -dr com.apple.quarantine /Applications/pixet.app"
    fi
fi

echo
echo "==> Done: $DMG"
echo "    Size: $(du -h "$DMG" | cut -f1)  (app bundle: $(du -sh "$APP" | cut -f1))"
echo
echo "pixet-index ships inside the bundle and needs nothing alongside it. To run it by name:"
echo "    sudo ln -sf /Applications/pixet.app/Contents/MacOS/pixet-index /usr/local/bin/pixet-index"
echo "    pixet-index --help"
echo
echo "Worth actually testing before sending it anywhere - a bundle that works only because"
echo "your own ~/Qt exists is the classic failure here:"
echo "    mv ~/Qt ~/Qt.aside && open \"$APP\" && mv ~/Qt.aside ~/Qt"
