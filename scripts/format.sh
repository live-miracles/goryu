#!/usr/bin/env bash
set -euo pipefail

REPO_DIR="$(cd "$(dirname "$0")/.." && pwd)"
MODE="${1:-write}"
# Releases of clang-format format the same code differently (18 and 21 disagreed on braces
# in a macro and failed CI), so a different major version is refused. CI installs 21.1.8.
CLANG_FORMAT_MAJOR=21

if [[ "$MODE" != "write" && "$MODE" != "check" ]]; then
    echo "Usage: $0 [write|check]" >&2
    exit 1
fi

if ! command -v clang-format >/dev/null 2>&1; then
    echo "ERROR: clang-format is required for code formatting." >&2
    exit 1
fi

version="$(clang-format --version)"
if [[ ! "$version" =~ version\ ${CLANG_FORMAT_MAJOR}\. ]]; then
    echo "ERROR: clang-format $CLANG_FORMAT_MAJOR is required, found: $version" >&2
    exit 1
fi

mapfile -t files < <(find "$REPO_DIR/src" "$REPO_DIR/tests" -type f \( -name '*.c' -o -name '*.h' \) | sort)

if [[ "$MODE" == "check" ]]; then
    clang-format --dry-run --Werror "${files[@]}"
else
    clang-format -i "${files[@]}"
fi
