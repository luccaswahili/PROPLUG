#!/bin/bash
# Build LookMatch-<version>.pkg from a built LookMatch.plugin.
#   installer/macos/build_pkg.sh build-plugin/src/premiere/LookMatch.plugin 1.0.0 [ "Developer ID Installer: ..." ]
# Without a Developer ID the pkg is unsigned: fine for testing, Gatekeeper will warn other users.
set -euo pipefail
PLUGIN="${1:?path to LookMatch.plugin}"
VERSION="${2:-1.0.0}"
SIGN_ID="${3:-}"
HERE="$(cd "$(dirname "$0")" && pwd)"
OUT="$(pwd)/dist"
WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT

DEST="Library/Application Support/Adobe/Common/Plug-ins/7.0/MediaCore"
mkdir -p "$WORK/root/$DEST" "$WORK/scripts" "$OUT"
ditto "$PLUGIN" "$WORK/root/$DEST/LookMatch.plugin"
cp "$HERE/postinstall" "$WORK/scripts/postinstall"
chmod +x "$WORK/scripts/postinstall"

pkgbuild --root "$WORK/root" --scripts "$WORK/scripts" --identifier com.proplugs.lookmatch.plugin \
         --version "$VERSION" --install-location / "$WORK/LookMatch-plugin.pkg"
sed "s/version=\"1.0.0\"/version=\"$VERSION\"/" "$HERE/distribution.xml" > "$WORK/distribution.xml"
SIGN_ARGS=()
[ -n "$SIGN_ID" ] && SIGN_ARGS=(--sign "$SIGN_ID")
productbuild --distribution "$WORK/distribution.xml" --resources "$HERE" --package-path "$WORK" \
             "${SIGN_ARGS[@]}" "$OUT/LookMatch-$VERSION.pkg"
echo "built $OUT/LookMatch-$VERSION.pkg"
