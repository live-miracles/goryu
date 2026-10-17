#!/usr/bin/env bash
set -euo pipefail

REPO_DIR="$(cd "$(dirname "$0")/.." && pwd)"
BIN="${GORYU_PATH:-$REPO_DIR/objs/goryu}"
LIB_DIR="${GORYU_LIB_DIR:-$REPO_DIR/objs/lib}"
CONFIG_PATH="${GORYU_CONFIG_PATH:-$REPO_DIR/goryu.json}"

if [[ ! -x "$BIN" ]]; then
    echo "ERROR: goryu not found or not executable: $BIN" >&2
    echo "Run: bash scripts/build-local.sh" >&2
    exit 1
fi

if [[ ! -f "$CONFIG_PATH" ]]; then
    echo "ERROR: config file not found: $CONFIG_PATH" >&2
    exit 1
fi

echo "Relay:  $BIN"
echo "Config: $CONFIG_PATH"
if [[ -d "$LIB_DIR" ]]; then
    export LD_LIBRARY_PATH="$LIB_DIR${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
fi
exec "$BIN" "$CONFIG_PATH"

