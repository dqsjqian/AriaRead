#!/usr/bin/env bash
# Compatibility entry point. Build/dependency/runtime logic lives in tools/build.py.
set -euo pipefail
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
exec python3 "$SCRIPT_DIR/../tools/build.py" "$@"
