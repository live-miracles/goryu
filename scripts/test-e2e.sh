#!/usr/bin/env bash
# End-to-end tests for goryu. Starts its own relay, downstream and publishers on
# spare ports and a private work dir, so it does not touch a running relay.
#
# Plain SRT callers are ffmpeg. Bonded input is simulated with
# tests/e2e/bonded_caller.c: a libsrt socket-group caller whose legs use different
# loopback source addresses (127.0.0.1 and 127.0.0.2), which gives the relay two
# distinct peers on one machine. It is built once into objs/ with the image that
# scripts/build-local.sh makes; E2E_SKIP_BONDED=1 skips the bonded scenarios.
#
#   bash scripts/test-e2e.sh [scenario ...]    (default: all)
#
# Needs: ffmpeg (with SRT), curl, jq, ss, and Docker for the bonded caller.
# GORYU_PATH, GORYU_LIB_DIR and E2E_BASE_PORT override the binary, its libraries
# and the first port used.
set -uo pipefail

REPO_DIR="$(cd "$(dirname "$0")/.." && pwd)"
BIN="${GORYU_PATH:-$REPO_DIR/objs/goryu}"
LIB_DIR="${GORYU_LIB_DIR:-$REPO_DIR/objs/lib}"
BASE_PORT="${E2E_BASE_PORT:-24000}"
PASSPHRASE="e2e-passphrase-0123"
BONDED="$REPO_DIR/objs/bonded-caller"

for tool in ffmpeg curl jq ss; do
    command -v "$tool" >/dev/null 2>&1 || { echo "ERROR: $tool is required" >&2; exit 2; }
done
[[ -x "$BIN" ]] || { echo "ERROR: $BIN not found; run scripts/build-local.sh" >&2; exit 2; }

WORK="$(mktemp -d)"
PIDS=()
PASS=0
FAIL=0
FAILED=()

kill_pids() {
    { for p in ${PIDS[@]+"${PIDS[@]}"}; do kill -KILL "$p"; done; wait; } 2>/dev/null
    PIDS=()
}
cleanup() { kill_pids; rm -rf "$WORK"; }
trap cleanup EXIT

# --- helpers ----------------------------------------------------------------

check() { # check <description> <command...>
    local desc="$1"; shift
    if "$@" >/dev/null 2>&1; then
        PASS=$((PASS + 1)); echo "  PASS  $desc"
    else
        FAIL=$((FAIL + 1)); FAILED+=("$CURRENT: $desc"); echo "  FAIL  $desc"
    fi
}

wait_for() { # wait_for <timeout-seconds> <command...>
    local deadline=$((SECONDS + $1)); shift
    while ! "$@" >/dev/null 2>&1; do
        ((SECONDS >= deadline)) && return 1
        sleep 0.25
    done
}

ports() { # ports <scenario-number>: sets IN OUT STATUS
    IN=$((BASE_PORT + $1 * 10 + 1))
    OUT=$((BASE_PORT + $1 * 10 + 2))
    STATUS=$((BASE_PORT + $1 * 10 + 3))
}

write_config() { # write_config <file> <passphrase>
    printf '{"input_host":"127.0.0.1","input_port":%d,"output_host":"127.0.0.1","output_port":%d,"status_port":%d,"passphrase":"%s"}\n' \
        "$IN" "$OUT" "$STATUS" "$2" >"$1"
}

status() { curl -fsS --max-time 2 "http://127.0.0.1:$STATUS/status"; }
# stream_jq <streamid> <jq expression over the stream's entry>
stream_jq() { status | jq -er --arg s "$1" ".streamStates[] | select(.streamId==\$s) | $2"; }

start_goryu() { # start_goryu <config> <log>; sets GPID
    LD_LIBRARY_PATH="$LIB_DIR" "$BIN" "$1" >"$2" 2>&1 &
    GPID=$!
    PIDS+=("$GPID")
    wait_for 5 status
}

start_sink() { # start_sink [passphrase]; sets SINK_PID
    local extra=""
    [[ -n "${1:-}" ]] && extra="&passphrase=$1&pbkeylen=16"
    ffmpeg -nostdin -hide_banner -loglevel error \
        -i "srt://127.0.0.1:$OUT?mode=listener&transtype=live$extra" -c copy -f null - \
        >>"$WORK/sink.log" 2>&1 &
    SINK_PID=$!
    PIDS+=("$SINK_PID")
}

start_pub() { # start_pub <streamid> [passphrase] [seconds]; sets PUB_PID
    local extra=""
    [[ -n "${2:-}" ]] && extra="&passphrase=$2&pbkeylen=16"
    ffmpeg -nostdin -hide_banner -loglevel error -re \
        -f lavfi -i testsrc=size=160x90:rate=15 -t "${3:-120}" \
        -c:v libx264 -preset ultrafast -tune zerolatency -f mpegts "srt://127.0.0.1:$IN?streamid=$1&transtype=live$extra" \
        >>"$WORK/pub.log" 2>&1 &
    PUB_PID=$!
    PIDS+=("$PUB_PID")
}

# A publisher whose SRT connection stays up while it sends no media: ffmpeg
# remuxing a FIFO that has been fed one short clip and then held open. This is
# what a wedged encoder looks like to the relay (a SIGSTOPped ffmpeg is not: its
# connection is dropped by libsrt's own 5 s idle timeout before the relay's stale
# check can see it).
ensure_seed() { # a 2 s clip of MPEG-TS
    [[ -s "$WORK/seed.ts" ]] ||
        ffmpeg -nostdin -hide_banner -loglevel error -f lavfi -i testsrc=size=160x90:rate=15 -t 2 \
            -c:v libx264 -preset ultrafast -f mpegts "$WORK/seed.ts"
}

start_silent_pub() { # start_silent_pub <streamid>; sets PUB_PID
    local fifo="$WORK/silent.fifo"
    ensure_seed || return 1
    rm -f "$fifo"; mkfifo "$fifo"
    ( cat "$WORK/seed.ts"; exec sleep 300 ) >"$fifo" 2>/dev/null &
    PIDS+=("$!")
    ffmpeg -nostdin -hide_banner -loglevel error -fflags nobuffer -analyzeduration 100000 -probesize 20000 \
        -f mpegts -i "$fifo" -c copy -f mpegts "srt://127.0.0.1:$IN?streamid=$1&transtype=live" \
        >>"$WORK/pub.log" 2>&1 &
    PUB_PID=$!
    PIDS+=("$PUB_PID")
}

forwarding() { # forwarding <streamid>: bytes keep growing
    local a b
    a="$(stream_jq "$1" .forwardedBytes)" || return 1
    sleep 2
    b="$(stream_jq "$1" .forwardedBytes)" || return 1
    ((b > a))
}

active() { stream_jq "$1" .inputActive | grep -qx true; }
connected() { stream_jq "$1" .outputConnected | grep -qx true; }
both_active() { active "$1" && active "$2"; }
active_and_connected() { active "$1" && connected "$1"; }
field_is() { [[ "$(stream_jq "$1" "$2")" == "$3" ]]; } # field_is <streamid> <jq> <expected>
field_ge() { (("$(stream_jq "$1" "$2")" >= $3)); }    # field_ge <streamid> <jq> <number>
field_has() { stream_jq "$1" "$2" | grep -q -- "$3"; } # field_has <streamid> <jq> <text>
status_has() { status | jq -e "$1"; }
status_pid_is() { [[ "$(status | jq .pid)" == "$1" ]]; }
no_stream() { [[ "$(status | jq -r --arg s "$1" '[.streamStates[] | select(.streamId==$s)] | length')" == 0 ]]; }
listening_on() { ss -Hltn "sport = :$STATUS" | grep -q "$1:$STATUS"; }
log_has() { grep -q -- "$1" "$2"; }
exited_within() { # exited_within <pid> <seconds>
    wait_for "$2" bash -c "! kill -0 $1 2>/dev/null"
}


# --- bonded input -----------------------------------------------------------

build_bonded_caller() {
    local source="$REPO_DIR/tests/e2e/bonded_caller.c"
    [[ -x "$BONDED" && "$BONDED" -nt "$source" ]] && return 0
    command -v docker >/dev/null 2>&1 || { echo "  (docker is needed to build the bonded caller)"; return 1; }
    local image="goryu-dev:${SRT_TAG:-v1.5.6}"
    docker image inspect "$image" >/dev/null 2>&1 || { echo "  (image $image is missing: run scripts/build-local.sh)"; return 1; }
    mkdir -p "$REPO_DIR/objs"
    docker run --rm --user "$(id -u):$(id -g)" -v "$REPO_DIR":/w -w /w "$image" \
        sh -c 'gcc -O2 -Wall -Wextra -o objs/bonded-caller tests/e2e/bonded_caller.c $(pkg-config --cflags --libs srt) -lpthread' \
        >"$WORK/bonded-build.log" 2>&1 || { cat "$WORK/bonded-build.log"; return 1; }
}

BOND_N=0
# start_bonded <streamid> [bonded_caller options...]: ffmpeg test pattern piped
# into a bonded caller; sets PUB_PID to the caller's pid. BOND_SECONDS bounds it.
start_bonded() {
    local sid="$1"; shift
    local fifo="$WORK/bond$((++BOND_N)).fifo"
    mkfifo "$fifo"
    ffmpeg -nostdin -hide_banner -loglevel error -re -f lavfi -i testsrc=size=160x90:rate=15 \
        -t "${BOND_SECONDS:-120}" -c:v libx264 -preset ultrafast -tune zerolatency -f mpegts - \
        >"$fifo" 2>>"$WORK/pub.log" &
    PIDS+=("$!")
    LD_LIBRARY_PATH="$LIB_DIR" "$BONDED" --port "$IN" --streamid "$sid" "$@" <"$fifo" \
        >>"$WORK/bonded.log" 2>&1 &
    PUB_PID=$!
    PIDS+=("$PUB_PID")
}

# The bonded counterpart of start_silent_pub: the group stays connected, no media.
start_silent_bonded() {
    local sid="$1"; shift
    local fifo="$WORK/bond$((++BOND_N)).fifo"
    ensure_seed || return 1
    mkfifo "$fifo"
    ( cat "$WORK/seed.ts"; exec sleep 300 ) >"$fifo" 2>/dev/null &
    PIDS+=("$!")
    LD_LIBRARY_PATH="$LIB_DIR" "$BONDED" --port "$IN" --streamid "$sid" "$@" <"$fifo" \
        >>"$WORK/bonded.log" 2>&1 &
    PUB_PID=$!
    PIDS+=("$PUB_PID")
}

stream_count_is() { [[ "$(status | jq '.streamStates | length')" == "$1" ]]; }
# With two live legs every packet arrives twice; the relay must send it on once.
# The sampled leg counters lag the live forwarded count slightly, hence a wide band.
forwarded_once_not_per_leg() {
    local fwd total
    fwd="$(stream_jq "$1" .forwardedPackets)" || return 1
    total="$(legs_received "$1")" || return 1
    echo "forwarded=$fwd legs_total=$total" >&2
    ((fwd * 10 < total * 7 && fwd * 10 > total * 3))
}
leg_count_is() { field_is "$1" '.input.legs|length' "$2"; }
# Total packets the legs received, and the packets the relay forwarded.
legs_received() { stream_jq "$1" '[.input.legs[].recvPacketsTotal] | add'; }
open_fds() { ls "/proc/$1/fd" | wc -l; }
threads() { ls "/proc/$1/task" | wc -l; }

# --- scenarios --------------------------------------------------------------

scenario_cli() {
    ports 0
    check "--version prints a version and exits 0" bash -c \
        "LD_LIBRARY_PATH='$LIB_DIR' '$BIN' --version | grep -q ."
    check "no arguments exits 1 with usage" bash -c \
        "out=\$(LD_LIBRARY_PATH='$LIB_DIR' '$BIN' 2>&1); [ \$? -eq 1 ] && echo \"\$out\" | grep -q Usage:"
    check "missing config file exits 1" bash -c \
        "LD_LIBRARY_PATH='$LIB_DIR' '$BIN' '$WORK/nope.json' >/dev/null 2>&1; [ \$? -eq 1 ]"
}

scenario_bad_config() {
    ports 1
    local name body
    while IFS='|' read -r name body; do
        printf '%s\n' "$body" >"$WORK/bad-$name.json"
        check "config with $name is refused (exit 1, never listens)" bash -c \
            "LD_LIBRARY_PATH='$LIB_DIR' timeout 5 '$BIN' '$WORK/bad-$name.json' >'$WORK/bad-$name.log' 2>&1; \
             [ \$? -eq 1 ] && ! grep -q Listening '$WORK/bad-$name.log'"
    done <<EOF
a missing key|{"input_host":"0.0.0.0","input_port":$IN}
port 0|{"input_host":"127.0.0.1","input_port":0,"output_host":"127.0.0.1","output_port":$OUT,"status_port":$STATUS,"passphrase":""}
port above 65535|{"input_host":"127.0.0.1","input_port":65536,"output_host":"127.0.0.1","output_port":$OUT,"status_port":$STATUS,"passphrase":""}
a port that wraps to a valid one|{"input_host":"127.0.0.1","input_port":$((IN + 4294967296)),"output_host":"127.0.0.1","output_port":$OUT,"status_port":$STATUS,"passphrase":""}
a passphrase shorter than 10 characters|{"input_host":"127.0.0.1","input_port":$IN,"output_host":"127.0.0.1","output_port":$OUT,"status_port":$STATUS,"passphrase":"short"}
a passphrase longer than 79 characters|{"input_host":"127.0.0.1","input_port":$IN,"output_host":"127.0.0.1","output_port":$OUT,"status_port":$STATUS,"passphrase":"$(printf 'p%.0s' $(seq 1 80))"}
EOF
}

scenario_forward() {
    ports 2
    write_config "$WORK/c.json" ""
    start_sink
    start_goryu "$WORK/c.json" "$WORK/g.log"
    check "relay starts and logs the listener" log_has "Listening on SRT" "$WORK/g.log"
    check "status port is bound to loopback only" listening_on 127.0.0.1
    start_pub "demo/key"
    check "publisher becomes active" wait_for 10 active demo/key
    check "downstream output connects" wait_for 10 connected demo/key
    check "bytes are forwarded and keep growing" forwarding demo/key
    check "plain input is reported as not bonded" field_is demo/key .input.bonded false
    check "plain input reports the encoder's address" field_is demo/key .input.peer.ip 127.0.0.1
    check "plain input has no legs" field_is demo/key '.input.legs|length' 0
    check "status is valid JSON with the documented top-level fields" status_has \
        '.pid and .startedAtMs and .updatedAtMs and (.activeStreamIds|type=="array") and (.streamStates|type=="array")'
    check "status pid is the relay's pid" status_pid_is "$GPID"
    kill "$PUB_PID" 2>/dev/null
    check "stream disappears after the publisher stops" wait_for 10 no_stream demo/key
    check "close is logged with a reason" log_has "Connection closed .* reason=input_disconnected" "$WORK/g.log"
}

scenario_downstream_down() {
    ports 3
    write_config "$WORK/c.json" ""
    start_goryu "$WORK/c.json" "$WORK/g.log"
    start_pub "demo/down"
    check "publisher stays connected with no downstream" wait_for 10 active demo/down
    check "retry failures climb" wait_for 20 field_ge demo/down .retryFailures 2
    check "output is reported disconnected" field_is demo/down .outputConnected false
    check "stream lastError explains the retry" field_has demo/down .lastError "retrying in"
    check "backoff is logged (1000 then 2000 ms)" bash -c \
        "grep -q 'failures=1 retry_ms=1000' '$WORK/g.log' && grep -q 'failures=2 retry_ms=2000' '$WORK/g.log'"
    check "relay is still alive" kill -0 "$GPID"
    start_sink
    check "recovers by itself once the downstream appears" wait_for 25 connected demo/down
    check "retry counter resets after recovery" field_is demo/down .retryFailures 0
    check "data flows after recovery" forwarding demo/down
}

scenario_duplicate() {
    ports 4
    write_config "$WORK/c.json" ""
    start_sink
    start_goryu "$WORK/c.json" "$WORK/g.log"
    start_pub "demo/dup"
    wait_for 10 connected demo/dup
    start_pub "demo/dup" "" 15
    check "second publisher on an active streamid is rejected" wait_for 12 log_has "Duplicate publisher rejected for active streamid=demo/dup" "$WORK/g.log"
    check "the first publisher is unaffected" forwarding demo/dup
}

scenario_takeover() {
    ports 5
    write_config "$WORK/c.json" ""
    start_sink
    start_goryu "$WORK/c.json" "$WORK/g.log"
    start_silent_pub "demo/stale"
    check "the silent publisher is accepted and connected downstream" wait_for 10 active_and_connected demo/stale
    check "it forwards nothing" field_is demo/stale .forwardedBytes 0
    sleep 6 # its input has now been silent for longer than the 5 s staleness limit
    start_pub "demo/stale"
    check "the takeover of the stale publisher is logged" wait_for 15 log_has "Duplicate publisher takeover for stale streamid=demo/stale" "$WORK/g.log"
    check "the stale session is closed" wait_for 10 log_has "Connection closed .*streamid=demo/stale" "$WORK/g.log"
    check "the replacement is not rejected" bash -c "! grep -q 'Duplicate publisher rejected' '$WORK/g.log'"
    start_sink # the old downstream connection was closed with the stale session
    check "the replacement connects downstream and forwards" wait_for 25 active_and_connected demo/stale
    check "bytes keep growing" forwarding demo/stale
}

scenario_two_streams() {
    ports 6
    write_config "$WORK/c.json" ""
    start_sink
    start_goryu "$WORK/c.json" "$WORK/g.log"
    start_pub "demo/one"
    wait_for 10 connected demo/one
    # the single ffmpeg downstream takes one caller, so demo/two keeps retrying
    start_pub "demo/two"
    check "both streams are tracked independently" wait_for 10 both_active demo/one demo/two
    kill "$PUB_PID" 2>/dev/null
    check "stopping one stream removes only that one" wait_for 10 no_stream demo/two
    check "the other stream keeps forwarding" forwarding demo/one
}

scenario_passphrase() {
    ports 7
    write_config "$WORK/c.json" "$PASSPHRASE"
    start_sink "$PASSPHRASE"
    start_goryu "$WORK/c.json" "$WORK/g.log"
    check "the passphrase never appears in the relay's log" bash -c "! grep -q '$PASSPHRASE' '$WORK/g.log'"
    start_pub "demo/enc" "wrong-passphrase-99" 8
    sleep 4
    check "a caller with the wrong passphrase is not accepted" no_stream demo/enc
    check "no session is logged for the wrong passphrase" bash -c "! grep -q 'Accepted' '$WORK/g.log'"
    start_pub "demo/enc" "$PASSPHRASE"
    check "a caller with the right passphrase is accepted and forwarded" wait_for 10 connected demo/enc
    check "data flows over the encrypted path" forwarding demo/enc
    check "the passphrase never appears in the log afterwards either" bash -c "! grep -q '$PASSPHRASE' '$WORK/g.log'"
}

scenario_uri_mode() {
    ports 9
    local in_uri="srt://127.0.0.1:$IN?mode=listener&groupconnect=1&transtype=live&latency=240"
    local out_uri="srt://127.0.0.1:$OUT?mode=caller&transtype=live&latency=200"
    local env_status="SRT_BONDING_STATUS_PORT=$STATUS"

    check "a too-short passphrase in the input URI is refused" bash -c \
        "env $env_status LD_LIBRARY_PATH='$LIB_DIR' timeout 5 '$BIN' '$in_uri&passphrase=short' '$out_uri' >'$WORK/u1.log' 2>&1; \
         [ \$? -eq 1 ] && ! grep -q Listening '$WORK/u1.log'"
    check "a too-short passphrase in the output URI is refused" bash -c \
        "env $env_status LD_LIBRARY_PATH='$LIB_DIR' timeout 5 '$BIN' '$in_uri' '$out_uri&passphrase=short' >'$WORK/u2.log' 2>&1; \
         [ \$? -eq 1 ] && ! grep -q Listening '$WORK/u2.log'"
    check "a malformed URI is refused without logging its passphrase" bash -c \
        "env $env_status LD_LIBRARY_PATH='$LIB_DIR' timeout 5 '$BIN' 'srt://nohost?passphrase=$PASSPHRASE' '$out_uri' >'$WORK/u3.log' 2>&1; \
         [ \$? -eq 1 ] && grep -q 'Bad input URI' '$WORK/u3.log' && ! grep -q '$PASSPHRASE' '$WORK/u3.log'"
    check "a non-srt scheme is refused" bash -c \
        "env $env_status LD_LIBRARY_PATH='$LIB_DIR' timeout 5 '$BIN' 'udp://127.0.0.1:$IN' '$out_uri' >/dev/null 2>&1; [ \$? -eq 1 ]"

    start_sink
    env "$env_status" LD_LIBRARY_PATH="$LIB_DIR" "$BIN" "$in_uri&passphrase=$PASSPHRASE" \
        "$out_uri&passphrase=$PASSPHRASE&pbkeylen=16" >"$WORK/g.log" 2>&1 &
    GPID=$!
    PIDS+=("$GPID")
    check "the relay starts from two URIs" wait_for 5 status
    check "the passphrase is not logged in URI mode either" bash -c "! grep -q '$PASSPHRASE' '$WORK/g.log'"
    check "the log shows the redacted output URI" log_has "passphrase=<redacted>" "$WORK/g.log"
}

scenario_sigterm() {
    ports 8
    write_config "$WORK/c.json" ""
    start_sink
    start_goryu "$WORK/c.json" "$WORK/g.log"
    start_pub "demo/term"
    wait_for 10 connected demo/term
    kill -TERM "$GPID"
    check "SIGTERM with an active session exits within 6 s" exited_within "$GPID" 6
    local rc=0
    wait "$GPID" 2>/dev/null || rc=$?
    check "and exits with status 0" test "$rc" -eq 0
}


scenario_bonded() {
    ports 10
    write_config "$WORK/c.json" ""
    start_sink
    start_goryu "$WORK/c.json" "$WORK/g.log"
    BOND_SECONDS=60 start_bonded demo/bond --locals 127.0.0.1,127.0.0.2 --drop-leg 1 --drop-after 14
    check "a bonded group is accepted and logged as bonded" wait_for 10 log_has "Accepted bonded SRT source" "$WORK/g.log"
    check "downstream connects" wait_for 10 active_and_connected demo/bond
    check "the input is reported as bonded" field_is demo/bond .input.bonded true
    check "a bonded input has no single peer" field_is demo/bond '.input.peer == null' true
    check "both legs are listed" wait_for 8 leg_count_is demo/bond 2
    check "both legs are running" field_is demo/bond '[.input.legs[].state] | unique | join(",")' running
    check "each leg has its own source address" field_is demo/bond '[.input.legs[].ip] | sort | join(",")' 127.0.0.1,127.0.0.2
    check "each leg has received data" field_is demo/bond '[.input.legs[].recvUniquePacketsTotal > 0] | all' true
    check "each leg reports its RTT" field_is demo/bond '[.input.legs[].rttMs | type] | unique | join(",")' number
    check "latency is the negotiated 240 ms, derived from the legs" field_is demo/bond .input.latencyMs 240
    check "group-level bandwidth is null, as documented" field_is demo/bond '.input.bandwidthMbps == null' true
    check "group-level loss is null, as documented" field_is demo/bond '.input.recvLossTotal == null' true
    check "bytes are forwarded and keep growing" forwarding demo/bond
    sleep 5
    check "the relay forwards each packet once, not once per leg" forwarded_once_not_per_leg demo/bond
    check "a leg that goes down disappears from legs[]" wait_for 15 leg_count_is demo/bond 1
    check "the remaining leg is the one that was not dropped" field_is demo/bond '.input.legs[0].ip' 127.0.0.1
    check "the stream is still bonded and active" field_is demo/bond '[.input.bonded, .inputActive] | all' true
    check "forwarding continues on the surviving leg" forwarding demo/bond
    kill "$PUB_PID" 2>/dev/null
    check "the stream disappears when the group ends" wait_for 10 no_stream demo/bond
    check "the close is logged" wait_for 5 log_has "Connection closed .*streamid=demo/bond" "$WORK/g.log"
}

scenario_bonded_legs() {
    ports 11
    write_config "$WORK/c.json" ""
    start_sink
    start_goryu "$WORK/c.json" "$WORK/g.log"
    BOND_SECONDS=40 start_bonded demo/legs --locals 127.0.0.1 --add-local 127.0.0.2 --add-after 6
    check "a group with a single leg is already bonded" wait_for 10 field_is demo/legs .input.bonded true
    check "it starts with one leg" wait_for 5 leg_count_is demo/legs 1
    check "a leg that joins later is added to the same stream" wait_for 15 leg_count_is demo/legs 2
    check "there is still only one stream" stream_count_is 1
    check "the relay logged one accepted session per leg connection" bash -c "[ \$(grep -c 'Accepted bonded' '$WORK/g.log') -ge 1 ]"
    check "forwarding continues with the extra leg" forwarding demo/legs
}

scenario_bonded_duplicate() {
    ports 12
    write_config "$WORK/c.json" ""
    start_sink
    start_goryu "$WORK/c.json" "$WORK/g.log"
    BOND_SECONDS=40 start_bonded demo/dup
    wait_for 10 active_and_connected demo/dup
    start_pub "demo/dup" "" 15
    check "a plain publisher on an active bonded streamid is rejected" wait_for 12 log_has "Duplicate publisher rejected for active streamid=demo/dup" "$WORK/g.log"
    check "the bonded stream keeps forwarding" forwarding demo/dup
}

scenario_bonded_takeover() {
    ports 13
    write_config "$WORK/c.json" ""
    start_sink
    start_goryu "$WORK/c.json" "$WORK/g.log"
    start_silent_bonded demo/stale --locals 127.0.0.1,127.0.0.2
    check "the silent bonded publisher is accepted" wait_for 10 active_and_connected demo/stale
    check "its group has both legs" wait_for 8 leg_count_is demo/stale 2
    sleep 6
    BOND_SECONDS=40 start_bonded demo/stale
    check "the takeover of the stale bonded publisher is logged" wait_for 15 log_has "Duplicate publisher takeover for stale streamid=demo/stale" "$WORK/g.log"
    check "the stale group's session is closed" wait_for 10 log_has "Connection closed .*streamid=demo/stale" "$WORK/g.log"
    start_sink
    check "the replacement forwards" wait_for 25 active_and_connected demo/stale
    check "bytes keep growing" forwarding demo/stale
    check "the relay is still running" kill -0 "$GPID"
}

scenario_bonded_passphrase() {
    ports 14
    write_config "$WORK/c.json" "$PASSPHRASE"
    start_sink "$PASSPHRASE"
    start_goryu "$WORK/c.json" "$WORK/g.log"
    check "a bonded caller with the wrong passphrase cannot connect" bash -c \
        "LD_LIBRARY_PATH='$LIB_DIR' timeout 15 '$BONDED' --port $IN --streamid demo/enc --passphrase wrong-passphrase-99 </dev/null >/dev/null 2>&1; [ \$? -ne 0 ]"
    check "and no session was created for it" bash -c "! grep -q Accepted '$WORK/g.log'"
    BOND_SECONDS=30 start_bonded demo/enc --passphrase "$PASSPHRASE"
    check "a bonded caller with the right passphrase is accepted" wait_for 10 active_and_connected demo/enc
    check "data flows over the encrypted group" forwarding demo/enc
    check "the passphrase is not in the relay's log" bash -c "! grep -q '$PASSPHRASE' '$WORK/g.log'"
}

scenario_bonded_shutdown() {
    ports 15
    write_config "$WORK/c.json" ""
    start_sink
    start_goryu "$WORK/c.json" "$WORK/g.log"
    BOND_SECONDS=40 start_bonded demo/term
    wait_for 10 active_and_connected demo/term
    kill -TERM "$GPID"
    check "SIGTERM with an active bonded session exits within 6 s" exited_within "$GPID" 6
    local rc=0
    wait "$GPID" 2>/dev/null || rc=$?
    check "and exits with status 0" test "$rc" -eq 0
}

scenario_bonded_abrupt_end() {
    ports 16
    write_config "$WORK/c.json" ""
    start_sink
    start_goryu "$WORK/c.json" "$WORK/g.log"
    BOND_SECONDS=40 start_bonded demo/kill
    wait_for 10 active_and_connected demo/kill
    kill -KILL "$PUB_PID"
    check "a bonded caller that vanishes (all legs silent) ends its session" wait_for 15 no_stream demo/kill
    check "and the close is logged" wait_for 5 log_has "Connection closed .*streamid=demo/kill" "$WORK/g.log"
    check "the relay is still running" kill -0 "$GPID"
}


scenario_shutdown_silent() {
    ports 18
    write_config "$WORK/c.json" ""
    start_sink
    start_goryu "$WORK/c.json" "$WORK/g.log"
    start_silent_pub "demo/plain-silent"
    wait_for 10 active demo/plain-silent
    start_silent_bonded "demo/bonded-silent" --locals 127.0.0.1,127.0.0.2
    wait_for 10 active demo/bonded-silent
    sleep 3 # past the seed clip: both publishers are now connected and silent
    kill -TERM "$GPID"
    check "SIGTERM with a silent plain and a silent bonded session exits within 6 s" exited_within "$GPID" 6
    local rc=0
    wait "$GPID" 2>/dev/null || rc=$?
    check "and exits with status 0" test "$rc" -eq 0
}

scenario_churn() {
    ports 17
    write_config "$WORK/c.json" ""
    start_sink
    start_goryu "$WORK/c.json" "$WORK/g.log"
    sleep 1
    local fds_before threads_before i
    fds_before="$(open_fds "$GPID")"; threads_before="$(threads "$GPID")"
    for i in 1 2 3 4 5 6; do
        BOND_SECONDS=3 start_bonded demo/churn
        wait_for 10 active demo/churn || break
        sleep 2
        kill "$PUB_PID" 2>/dev/null
        wait_for 15 no_stream demo/churn || break
    done
    for i in 1 2 3 4 5 6; do
        start_pub "demo/churnp" "" 3
        wait_for 10 active demo/churnp || break
        sleep 2
        kill "$PUB_PID" 2>/dev/null
        wait_for 15 no_stream demo/churnp || break
    done
    check "twelve connect/disconnect cycles leave no stream behind" stream_count_is 0
    sleep 3
    check "the relay is still running" kill -0 "$GPID"
    check "open file descriptors did not grow (before $fds_before, after $(open_fds "$GPID"))" \
        test "$(open_fds "$GPID")" -le $((fds_before + 3))
    check "threads did not grow (before $threads_before, after $(threads "$GPID"))" \
        test "$(threads "$GPID")" -le $((threads_before + 1))
}

# --- run --------------------------------------------------------------------

ALL=(cli bad_config uri_mode forward downstream_down duplicate takeover two_streams passphrase sigterm
    shutdown_silent churn bonded bonded_legs bonded_duplicate bonded_takeover bonded_passphrase bonded_shutdown
    bonded_abrupt_end)
SELECTED=("${@:-${ALL[@]}}")

for name in "${SELECTED[@]}"; do
    CURRENT="$name"
    if [[ "$name" == bonded* || "$name" == churn ]]; then
        if [[ "${E2E_SKIP_BONDED:-0}" == 1 ]]; then echo "== $name (skipped)"; continue; fi
        if ! build_bonded_caller; then
            echo "== $name"; FAIL=$((FAIL + 1)); FAILED+=("$name: the bonded caller could not be built"); echo "  FAIL  bonded caller is available"
            continue
        fi
    fi
    echo "== $name"
    : >"$WORK/sink.log"; : >"$WORK/pub.log"
    "scenario_$name"
    kill_pids 2>/dev/null
done

echo
echo "passed: $PASS, failed: $FAIL"
if ((FAIL > 0)); then
    printf '  - %s\n' "${FAILED[@]}"
    exit 1
fi
