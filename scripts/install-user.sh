#!/usr/bin/env bash
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
SO="$ROOT/build/linux-x86_64/bokis-twitch-chat-plugin.so"
HELPER="$ROOT/build/linux-x86_64/bokis-twitch-chat-updater"
DEST="${XDG_CONFIG_HOME:-$HOME/.config}/obs-studio/plugins/bokis-twitch-chat-plugin/bin/64bit"
if [[ ! -f "$SO" || ! -x "$HELPER" ]]; then
  echo "Plugin not built. Run ./scripts/build-local.sh first." >&2
  exit 1
fi
# This development installer must also refuse to overwrite a loaded plugin.
if pgrep -x obs >/dev/null; then
  echo "Close all OBS processes before installing." >&2
  exit 1
fi
mkdir -p "$DEST"
exec 9>"$DEST/bokis-twitch-chat-plugin.so.use.lock"
flock -n -x 9 || { echo "Plugin is in use." >&2; exit 1; }
install -m 0755 "$SO" "$DEST/bokis-twitch-chat-plugin.so"
install -m 0755 "$HELPER" "$DEST/bokis-twitch-chat-updater"
echo "Installed: $DEST/bokis-twitch-chat-plugin.so"
echo "Restart OBS completely."
