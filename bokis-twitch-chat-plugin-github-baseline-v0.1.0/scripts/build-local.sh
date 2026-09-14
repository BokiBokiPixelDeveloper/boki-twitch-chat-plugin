#!/usr/bin/env bash
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"
cmake --preset linux-x86_64
cmake --build --preset linux-x86_64
printf '\nBuild complete: %s\n' "$ROOT/build/linux-x86_64/bokis-twitch-chat-plugin.so"
