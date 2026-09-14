#!/usr/bin/env bash
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
VERSION="$(tr -d '[:space:]' < "$ROOT/VERSION")"
SO="$ROOT/build/linux-x86_64/bokis-twitch-chat-plugin.so"
OUT="$ROOT/release"
PKG="$OUT/bokis-twitch-chat-plugin-$VERSION-linux-x86_64"
[[ -f "$SO" ]] || { echo "Missing build: $SO" >&2; exit 1; }
rm -rf "$PKG"
mkdir -p "$PKG/plugin/bin/64bit"
install -m 0755 "$SO" "$PKG/plugin/bin/64bit/bokis-twitch-chat-plugin.so"
cp "$ROOT/VERSION" "$PKG/VERSION"
cp "$ROOT/README.md" "$PKG/README.md"
mkdir -p "$OUT"

BINARY_ASSET="$OUT/bokis-twitch-chat-plugin-$VERSION-linux-x86_64.so"
install -m 0755 "$SO" "$BINARY_ASSET"
sha256sum "$BINARY_ASSET" > "$BINARY_ASSET.sha256"
(
  cd "$OUT"
  rm -f "bokis-twitch-chat-plugin-$VERSION-linux-x86_64.zip"
  zip -qr "bokis-twitch-chat-plugin-$VERSION-linux-x86_64.zip" "$(basename "$PKG")"
  sha256sum "bokis-twitch-chat-plugin-$VERSION-linux-x86_64.zip" > "bokis-twitch-chat-plugin-$VERSION-linux-x86_64.zip.sha256"
)
echo "Packaged in: $OUT"
