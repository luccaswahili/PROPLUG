#!/bin/sh
# Package the LookMatch UXP panel as a .ccx (and optionally install it).
#   installer/mac/build_panel.sh [--release] [--install]
# --release leaves out the dev-only test agent (lib/agent.js) and the tests.
# --install installs with Adobe's UnifiedPluginInstallerAgent (no Creative Cloud login needed).
set -e
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
SRC="$ROOT/src/panel-uxp"
OUT="$ROOT/build-installer"
RELEASE=0; INSTALL=0
for a in "$@"; do case "$a" in --release) RELEASE=1 ;; --install) INSTALL=1 ;; esac; done

VERSION=$(sed -n 's/.*"version": *"\([^"]*\)".*/\1/p' "$SRC/manifest.json" | head -1)
grep -q "const VERSION = \"$VERSION\"" "$SRC/main.js" || { echo "main.js VERSION != manifest $VERSION"; exit 1; }
node "$SRC/test/run.js" >/dev/null || { node "$SRC/test/run.js"; exit 1; }

STAGE="$OUT/panel-stage"
rm -rf "$STAGE" && mkdir -p "$STAGE/lib" "$OUT"
cp "$SRC/manifest.json" "$SRC/index.html" "$SRC/main.js" "$SRC/styles.css" "$STAGE/"
for f in "$SRC"/lib/*.js; do
  [ "$RELEASE" = 1 ] && [ "$(basename "$f")" = agent.js ] && continue
  cp "$f" "$STAGE/lib/"
done
NAME="LookMatchPanel-$VERSION$([ "$RELEASE" = 1 ] && echo "" || echo "-dev").ccx"
rm -f "$OUT/$NAME"
(cd "$STAGE" && zip -q -r "$OUT/$NAME" .)
echo "Built $OUT/$NAME"

if [ "$INSTALL" = 1 ]; then
  UPIA="/Library/Application Support/Adobe/Adobe Desktop Common/RemoteComponents/UPI/UnifiedPluginInstallerAgent/UnifiedPluginInstallerAgent.app/Contents/MacOS/UnifiedPluginInstallerAgent"
  "$UPIA" --remove LookMatch >/dev/null 2>&1 || true
  "$UPIA" --install "$OUT/$NAME" | grep -v '^Creative Cloud'
fi
