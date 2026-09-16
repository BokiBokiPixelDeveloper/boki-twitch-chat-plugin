#!/usr/bin/env bash
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
[[ $# -le 2 ]] || { echo "Usage: $0 [build-directory [output-directory]]" >&2; exit 1; }
VERSION="$(tr -d '[:space:]' < "$ROOT/VERSION")"
BUILD="$(realpath -m -- "${1:-$ROOT/build/linux-x86_64}")"
SO="$BUILD/bokis-twitch-chat-plugin.so"
HELPER="$BUILD/bokis-twitch-chat-updater"
OUT="$(realpath -m -- "${2:-$ROOT/release}")"
PKG="$OUT/bokis-twitch-chat-plugin-$VERSION-linux-x86_64"
[[ -f "$SO" ]] || { echo "Missing build: $SO" >&2; exit 1; }
[[ -x "$HELPER" ]] || { echo "Missing helper: $HELPER" >&2; exit 1; }
rm -rf "$PKG"
mkdir -p "$PKG/plugin/bin/64bit"
install -m 0755 "$SO" "$PKG/plugin/bin/64bit/bokis-twitch-chat-plugin.so"
install -m 0755 "$HELPER" "$PKG/plugin/bin/64bit/bokis-twitch-chat-updater"
cp "$ROOT/VERSION" "$PKG/VERSION"
cp "$ROOT/README.md" "$PKG/README.md"
cp "$ROOT/docs/INSTALL_LINUX.md" "$PKG/INSTALL.md"
install -m 0755 "$ROOT/scripts/linux-installer.sh" "$PKG/install.sh"
install -m 0755 "$ROOT/scripts/linux-installer.sh" "$PKG/plugin/uninstall.sh"
mkdir -p "$PKG/licenses"
cp "$ROOT/resources/fonts/OFL.txt" "$PKG/licenses/NotoColorEmoji-OFL.txt"
install -D -m 0644 "$ROOT/resources/fonts/OFL.txt" "$PKG/plugin/data/licenses/NotoColorEmoji-OFL.txt"
(
  cd "$PKG/plugin"
  find . -type f -printf '%P\0' | sort -z | xargs -0 sha256sum -- > "$PKG/SHA256SUMS"
)
mkdir -p "$OUT"

BINARY_ASSET="$OUT/bokis-twitch-chat-plugin-$VERSION-linux-x86_64.so"
install -m 0755 "$SO" "$BINARY_ASSET"
HELPER_ASSET="$OUT/bokis-twitch-chat-updater-$VERSION-linux-x86_64.bin"
install -m 0755 "$HELPER" "$HELPER_ASSET"
(
  cd "$OUT"
  sha256sum "$(basename "$BINARY_ASSET")" > "$BINARY_ASSET.sha256"
  sha256sum "$(basename "$HELPER_ASSET")" > "$HELPER_ASSET.sha256"
  rm -f "bokis-twitch-chat-plugin-$VERSION-linux-x86_64.zip"
  zip -qr "bokis-twitch-chat-plugin-$VERSION-linux-x86_64.zip" "$(basename "$PKG")"
  sha256sum "bokis-twitch-chat-plugin-$VERSION-linux-x86_64.zip" > "bokis-twitch-chat-plugin-$VERSION-linux-x86_64.zip.sha256"
)
echo "Packaged in: $OUT"
