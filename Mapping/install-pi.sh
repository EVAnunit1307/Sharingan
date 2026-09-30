#!/usr/bin/env bash
# Run on the Pi after transferring/extracting the setup bundle. Does not start motors or kill processes.
set -euo pipefail
bundle_root="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
target_root="${1:-/home/evanl1307/HTN2026/Sharingan}"
if [[ ! -f "$target_root/SensorRig/CV/detector/person_detector.py" ]]; then
  echo "Existing SensorRig checkout not found at: $target_root" >&2
  echo "Pass the actual repository directory as the first argument." >&2
  exit 1
fi
if [[ "$(uname -s)" != Linux ]]; then
  echo 'Run this installer on the Raspberry Pi, not the laptop.' >&2
  exit 1
fi
backup_root="$target_root/Saved/MappingSetup/$(date -u +%Y%m%dT%H%M%SZ)-$$"
mkdir -p "$backup_root"
for relative in SensorRig/CV/camera_dashboard.py SensorRig/CV/mapping_capture.py Build/start_mapping_pi.sh Build/run_mapping_pi_background.sh Build/wallhack-mapping.service; do
  mkdir -p "$backup_root/$(dirname "$relative")" "$target_root/$(dirname "$relative")"
  if [[ -f "$target_root/$relative" ]]; then
    cp -p "$target_root/$relative" "$backup_root/$relative"
  fi
  cp "$bundle_root/$relative" "$target_root/$relative"
done
echo "Installed mapping capture. Previous files saved under $backup_root"
echo 'Stop the existing camera dashboard before starting the mapping camera.'
echo "Then, from $target_root, run: bash Build/start_mapping_pi.sh"
echo 'For recording immediately at launch, append --mapping-autostart.'
echo 'Optional boot service instructions are in MAPPING-README.md in this bundle.'
