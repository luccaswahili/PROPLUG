#!/bin/sh
# Package the Komodo UXP panel as a .ccx (and optionally install it).
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
rm -rf "$STAGE" && mkdir -p "$STAGE/lib" "$STAGE/icons" "$OUT"
cp "$SRC/manifest.json" "$SRC/index.html" "$SRC/main.js" "$SRC/styles.css" "$STAGE/"
cp "$SRC"/icons/*.svg "$STAGE/icons/"
for f in "$SRC"/lib/*.js; do
  # dev-only, left out of release: the test agent and the self-test it runs
  [ "$RELEASE" = 1 ] && case "$(basename "$f")" in agent.js|selftest.js) continue ;; esac
  cp "$f" "$STAGE/lib/"
done
# Release hardening: minify the panel JS so the shipped .ccx isn't plain readable source. Best-effort —
# if terser is unavailable or a file fails to parse after minifying, keep the original so the panel never
# ships broken. The licence check is native anyway; this only raises the bar on reading the panel.
if [ "$RELEASE" = 1 ]; then
  for f in "$STAGE/main.js" "$STAGE"/lib/*.js; do
    [ -f "$f" ] || continue
    if npx --yes terser "$f" -c -m -o "$f.min" >/dev/null 2>&1 && node --check "$f.min" >/dev/null 2>&1; then
      mv "$f.min" "$f"
    else
      rm -f "$f.min"
      echo "note: left $(basename "$f") unminified"
    fi
  done
fi

NAME="KomodoPanel-$VERSION$([ "$RELEASE" = 1 ] && echo "" || echo "-dev").ccx"
rm -f "$OUT/$NAME"
(cd "$STAGE" && zip -q -r "$OUT/$NAME" .)
echo "Built $OUT/$NAME"

if [ "$INSTALL" = 1 ]; then
  UPIA="/Library/Application Support/Adobe/Adobe Desktop Common/RemoteComponents/UPI/UnifiedPluginInstallerAgent/UnifiedPluginInstallerAgent.app/Contents/MacOS/UnifiedPluginInstallerAgent"
  "$UPIA" --remove LookMatch >/dev/null 2>&1 || true   # the panel's name before 1.0
  "$UPIA" --remove Komodo >/dev/null 2>&1 || true
  "$UPIA" --install "$OUT/$NAME" | grep -v '^Creative Cloud'
fi
