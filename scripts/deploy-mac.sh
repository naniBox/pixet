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
# "Open Anyway" (the old Control-click > Open bypass was removed in macOS 15). Or, from a
# terminal:  xattr -dr com.apple.quarantine /Applications/pixet.app
#
# To produce something that just opens, with no warning, you need an Apple Developer Program
# membership ($99/year) and then only environment variables - no edits to this script:
#
#   PIXET_CODESIGN_IDENTITY="Developer ID Application: Your Name (TEAMID)" \
#   PIXET_NOTARY_PROFILE=my-notary-profile \
#     ./scripts/deploy-mac.sh
#
# (PIXET_NOTARY_PROFILE is a keychain profile previously stored with
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
echo "    Size: $(du -h "$DMG" | cut -f1)"
echo
echo "Worth actually testing before sending it anywhere - a bundle that works only because"
echo "your own ~/Qt exists is the classic failure here:"
echo "    mv ~/Qt ~/Qt.aside && open \"$APP\" && mv ~/Qt.aside ~/Qt"
