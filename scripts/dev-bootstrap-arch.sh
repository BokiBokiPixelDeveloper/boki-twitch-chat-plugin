#!/usr/bin/env bash
set -euo pipefail
sudo pacman -S --needed --noconfirm \
  base-devel cmake ninja obs-studio qt6-base qt6-websockets libwebp git python zip
