#!/bin/sh
# Build LookMatch.plugin and wrap it in LookMatch-Installer.pkg.
#   installer/mac/build_pkg.sh /path/to/AfterEffectsSDK/Examples
# Optional: SIGN_ID="Developer ID Installer: ..." to sign the pkg (otherwise unsigned).
set -e
SDK="${1:?usage: build_pkg.sh /path/to/AfterEffectsSDK/Examples}"
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
HERE="$ROOT/installer/mac"
OUT="$ROOT/build-installer"

cmake -S "$ROOT" -B "$OUT/cmake" -DLOOKMATCH_BUILD_PLUGIN=ON -DCMAKE_BUILD_TYPE=Release -DAE_SDK_DIR="$SDK"
cmake --build "$OUT/cmake" --config Release -j

rm -rf "$OUT/payload" && mkdir -p "$OUT/payload"
ditto "$OUT/cmake/src/premiere/LookMatch.plugin" "$OUT/payload/LookMatch.plugin"
codesign --force --deep -s "${CODESIGN_ID:--}" "$OUT/payload/LookMatch.plugin"

# Always install into the shared folder, never "relocate" onto a copy found elsewhere
pkgbuild --analyze --root "$OUT/payload" "$OUT/components.plist"
plutil -replace 0.BundleIsRelocatable -bool NO "$OUT/components.plist"

pkgbuild --root "$OUT/payload" --component-plist "$OUT/components.plist" \
  --install-location "/Library/Application Support/Adobe/Common/Plug-ins/7.0/MediaCore" \
  --identifier com.proplugs.lookmatch --version 1.0.0 \
  --scripts "$HERE/scripts" "$OUT/LookMatch-component.pkg"

set -- --distribution "$HERE/distribution.xml" --resources "$HERE/resources" --package-path "$OUT"
[ -n "$SIGN_ID" ] && set -- "$@" --sign "$SIGN_ID"
productbuild "$@" "$OUT/LookMatch-Installer.pkg"
echo "Built $OUT/LookMatch-Installer.pkg"
