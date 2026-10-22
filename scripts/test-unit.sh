#!/usr/bin/env bash
# Unit tests for the modules that do not need libsrt: config and URI parsing,
# the retry backoff and the status JSON writers. Needs only gcc.
#
# Built like the release (gcc, -Wall -Wextra, here with -Werror) under
# AddressSanitizer and UndefinedBehaviorSanitizer.
set -euo pipefail

REPO_DIR="$(cd "$(dirname "$0")/.." && pwd)"
OUT="$(mktemp -d)"
trap 'rm -rf "$OUT"' EXIT

SANITIZE=(-fsanitize=address,undefined -fno-sanitize-recover=undefined -g -O1)
WARN=(-Wall -Wextra -Werror)

declare -A SOURCES=(
    [test_relay_config]="src/relay_config.c"
    [test_backoff_json]="src/backoff.c src/json_out.c src/ratelimit.c"
)

status=0
for test in "${!SOURCES[@]}"; do
    bin="$OUT/$test"
    # shellcheck disable=SC2086
    (cd "$REPO_DIR" && gcc "${WARN[@]}" "${SANITIZE[@]}" "tests/unit/$test.c" ${SOURCES[$test]} -o "$bin")
    echo "== $test"
    "$bin" || status=1
done
exit $status
