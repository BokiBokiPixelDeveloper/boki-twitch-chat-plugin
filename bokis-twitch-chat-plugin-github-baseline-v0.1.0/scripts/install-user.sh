#!/usr/bin/env bash
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
SO="$ROOT/build/linux-x86_64/bokis-twitch-chat-plugin.so"
DEST="$HOME/.config/obs-studio/plugins/bokis-twitch-chat-plugin/bin/64bit"
if [[ ! -f "$SO" ]]; then
  echo "Plugin not built. Run ./scripts/build-local.sh first." >&2
  exit 1
fi
mkdir -p "$DEST"
install -m 0755 "$SO" "$DEST/bokis-twitch-chat-plugin.so"
echo "Installed: $DEST/bokis-twitch-chat-plugin.so"
echo "Restart OBS completely."
