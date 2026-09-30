#!/usr/bin/env bash
set -euo pipefail
repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$repo_root"
if [[ -x "$repo_root/../.venv/bin/python" ]]; then
  mapping_python="$repo_root/../.venv/bin/python"
elif [[ -x "$repo_root/.venv/bin/python" ]]; then
  mapping_python="$repo_root/.venv/bin/python"
else
  mapping_python=python3
fi
exec "$mapping_python" SensorRig/CV/pi_camera_stream.py --mapping-only "$@"
