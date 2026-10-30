#!/usr/bin/env bash
# Unit tests for the modules that do not need libsrt: config and URI parsing,
# the command line, the retry backoff, the status JSON, the telemetry rules, the stream
# state table, a session's place in the relay (the tracker, against a fake clock and a
# recorded srt_close) and the deadline-bound send. Needs only gcc.
#
# Built like the release (gcc, -Wall -Wextra, here with -Werror) under
# AddressSanitizer and UndefinedBehaviorSanitizer.
set -euo pipefail

REPO_DIR="$(cd "$(dirname "$0")/.." && pwd)"
OUT="$(mktemp -d)"
trap 'rm -rf "$OUT"' EXIT

SANITIZE=(-fsanitize=address,undefined -fno-sanitize-recover=undefined -g -O1)
WARN=(-Wall -Wextra -Werror)
# tests/unit/fake/srt/srt.h stands in for libsrt in the module that only needs its types.
INCLUDES=(-Itests/unit/fake)

declare -A SOURCES=(
    [test_relay_config]="src/relay_config.c"
    [test_relay_options]="src/relay_options.c src/relay_config.c"
    [test_backoff_json]="src/backoff.c src/json_out.c src/ratelimit.c"
    [test_timed_send]="src/timed_send.c"
    [test_telemetry]="src/telemetry.c src/ratelimit.c"
    [test_status_json]="src/status_json.c src/json_out.c src/telemetry.c src/ratelimit.c"
    [test_stream_state]="src/stream_state.c src/telemetry.c src/ratelimit.c"
    [test_tracker]="src/tracker.c src/stream_state.c src/telemetry.c src/ratelimit.c"
)

status=0
for test in "${!SOURCES[@]}"; do
    bin="$OUT/$test"
    # shellcheck disable=SC2086
    (cd "$REPO_DIR" && gcc "${WARN[@]}" "${INCLUDES[@]}" "${SANITIZE[@]}" "tests/unit/$test.c" ${SOURCES[$test]} -o "$bin")
    echo "== $test"
    "$bin" || status=1
done
exit $status
