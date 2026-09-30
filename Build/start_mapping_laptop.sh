#!/usr/bin/env bash
set -euo pipefail
repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$repo_root"
if [[ ! -x .venv/bin/python ]]; then
  python3 -m venv .venv
fi
if ! .venv/bin/python -c 'import flask, pycolmap' >/dev/null 2>&1; then
  .venv/bin/python -m pip install -r Mapping/requirements.txt
fi
exec .venv/bin/python -m Mapping.server "$@"
