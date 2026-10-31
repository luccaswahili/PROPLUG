#!/bin/sh
# Build the plug-in, the release panel and the "Install Komodo" app, embed both, sign, and make a dmg.
#   installer/mac/build_app.sh /path/to/AfterEffectsSDK/Examples [/path/to/PremiereSDK/Examples]
# With the Premiere Pro SDK the plug-in also renders on the GPU (Metal) in Premiere.
# Output: build-installer/Komodo-Installer.dmg (contains "Install Komodo.app").
# Optional: CODESIGN_ID="Developer ID Application: ..." (otherwise ad-hoc signed).
set -e
SDK="${1:?usage: build_app.sh /path/to/AfterEffectsSDK/Examples [/path/to/PremiereSDK/Examples]}"
PR_SDK="${2:-}"
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
SRC="$ROOT/installer/mac/LookMatchInstaller"
OUT="$ROOT/build-installer"
APP="$OUT/app/Install Komodo.app"
SIGN="${CODESIGN_ID:--}"

cmake -S "$ROOT" -B "$OUT/cmake" -DLOOKMATCH_BUILD_PLUGIN=ON -DCMAKE_BUILD_TYPE=Release -DAE_SDK_DIR="$SDK" \
  -DLOOKMATCH_PR_SDK_DIR="$PR_SDK"
cmake --build "$OUT/cmake" --config Release -j

"$ROOT/installer/mac/build_panel.sh" --release
PANEL=$(ls -t "$OUT"/KomodoPanel-*.ccx | grep -v -- -dev.ccx | head -1)

rm -rf "$OUT/app" && mkdir -p "$APP/Contents/MacOS" "$APP/Contents/Resources"
cp "$SRC/Info.plist" "$APP/Contents/Info.plist"
for arch in arm64 x86_64; do
  xcrun swiftc -O -parse-as-library -target "$arch-apple-macos11.0" \
    "$SRC/InstallerApp.swift" -o "$OUT/app/installer-$arch"
done
lipo -create "$OUT/app/installer-arm64" "$OUT/app/installer-x86_64" -output "$APP/Contents/MacOS/Install Komodo"
rm "$OUT/app/installer-arm64" "$OUT/app/installer-x86_64"

# app icon: the Komodo mark, rendered by the installer itself, as an .icns
ICONSET="$OUT/app/AppIcon.iconset"
mkdir -p "$ICONSET"
LOOKMATCH_INSTALLER_ICON="$OUT/app/icon-1024.png" "$APP/Contents/MacOS/Install Komodo"
for s in 16 32 128 256 512; do
  sips -z $s $s "$OUT/app/icon-1024.png" --out "$ICONSET/icon_${s}x${s}.png" >/dev/null
  sips -z $((s * 2)) $((s * 2)) "$OUT/app/icon-1024.png" --out "$ICONSET/icon_${s}x${s}@2x.png" >/dev/null
done
iconutil -c icns "$ICONSET" -o "$APP/Contents/Resources/AppIcon.icns"
rm -rf "$ICONSET" "$OUT/app/icon-1024.png"

ditto "$OUT/cmake/src/premiere/LookMatch.plugin" "$APP/Contents/Resources/LookMatch.plugin"
codesign --force -s "$SIGN" "$APP/Contents/Resources/LookMatch.plugin"
cp "$PANEL" "$APP/Contents/Resources/"
codesign --force -s "$SIGN" "$APP"

# Not notarized without a Developer ID: say how to open it the first time.
cat > "$OUT/app/If it won't open.txt" <<'TXT'
If macOS says "Install Komodo" can't be opened, or that Apple can't check it for malicious software:

macOS 15 Sequoia and newer
1. Double-click "Install Komodo" and click Done (or OK) on the warning.
2. Open System Settings > Privacy & Security.
3. Scroll down to the message about "Install Komodo" and click Open Anyway.
4. Enter your Mac password, then click Open Anyway once more.

macOS 14 Sonoma and older
1. Right-click (or Control-click) "Install Komodo" and choose Open.
2. Click Open in the window that appears.

You only need to do this the first time.
TXT

rm -f "$OUT/Komodo-Installer.dmg"
hdiutil create -quiet -volname "Komodo" -srcfolder "$OUT/app" -fs HFS+ -format UDZO "$OUT/Komodo-Installer.dmg"
echo "Built $OUT/Komodo-Installer.dmg"
