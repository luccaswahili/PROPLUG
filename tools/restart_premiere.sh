#!/bin/sh
# Dev tool: save the open project through the panel's test agent, confirm the save, quit Premiere
# (SIGTERM, never -9), install the freshly built effect, and reopen the project.
#   tools/restart_premiere.sh /path/to/project.prproj
# Refuses to quit if the save can't be confirmed (project file mtime must move forward).
set -e
PROJ="${1:?usage: restart_premiere.sh project.prproj}"
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
B="${PLUGIN:-$ROOT/build-installer/cmake/src/premiere/LookMatch.plugin}"  # PLUGIN=... to install another build
I="/Library/Application Support/Adobe/Common/Plug-ins/7.0/MediaCore/LookMatch.plugin"
APP="Adobe Premiere Pro 2026"

if pgrep -qx "$APP"; then
  before=$(stat -f %m "$PROJ")
  sleep 1
  "$ROOT/tools/panel_agent.py" saveProject >/dev/null || { echo "save failed: not quitting"; exit 1; }
  sleep 2
  after=$(stat -f %m "$PROJ")
  [ "$after" -gt "$before" ] || { echo "save not confirmed (mtime $before -> $after): not quitting"; exit 1; }
  echo "saved ($before -> $after)"
  kill -TERM "$(pgrep -x "$APP")"
  for i in $(seq 1 60); do pgrep -qx "$APP" || break; sleep 2; done
  pgrep -qx "$APP" && { echo "Premiere did not exit"; exit 1; }
  sleep 3
fi
ditto "$B" "$I" && codesign -v "$I"
echo "installed $(shasum -a 256 "$I/Contents/MacOS/LookMatch" | cut -c1-16)"
: > "$HOME/Library/Logs/LookMatch.log"
open -a "$APP" "$PROJ"
for i in $(seq 1 40); do
  sleep 3
  "$ROOT/tools/panel_agent.py" ping >/dev/null 2>&1 && { echo "panel up after $((i * 3))s"; exit 0; }
done
echo "panel did not answer"; exit 1
