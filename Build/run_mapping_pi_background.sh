#!/usr/bin/env bash
# User-owned boot/standalone entry; flock prevents two copies of this launcher.
set -euo pipefail
repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
mkdir -p "$repo_root/Saved/MappingCapture"
exec /usr/bin/flock --nonblock "$repo_root/Saved/MappingCapture/camera.lock" \
  /bin/bash "$repo_root/Build/start_mapping_pi.sh" --mapping-autostart "$@" \
  >> "$repo_root/Saved/MappingCapture/service.log" 2>&1
