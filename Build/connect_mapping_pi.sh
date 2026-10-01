#!/usr/bin/env bash
# Run by the operator in their own terminal. Reuses one authenticated SSH session
# for setup; it does not save a password or install a new authorized key.
set -euo pipefail
# Pass the Pi's IP when hotspot mDNS is unavailable; verify the same trusted Pi key.
pi_host="${1:-larp-pi.local}"
pi_target="evanl1307@$pi_host"
repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
control_dir="$repo_root/Saved/MappingSetup/ssh"
mkdir -p "$control_dir"
chmod 700 "$control_dir"
control_socket="$control_dir/pi-control"
if ssh -S "$control_socket" -O check "$pi_target" 2>/dev/null; then
  echo 'Pi setup connection is already available. Tell Codex to continue.'
  exit 0
fi
echo 'Log in to the Pi below. Your password is entered in this terminal, not in chat.'
ssh -M -S "$control_socket" -o ControlPersist=20m -o StrictHostKeyChecking=yes \
  -o HostKeyAlias=larp-pi.local -o ConnectTimeout=8 -fN "$pi_target"
ssh -S "$control_socket" -O check "$pi_target"
echo 'Connected. Tell Codex "connected" so it can finish setup.'
echo 'This connection expires after 20 minutes idle; no password or new key is saved.'
