#!/usr/bin/env bash
# End-to-end tests for goryu. Starts its own relay, downstream and publishers on
# spare ports and a private work dir, so it does not touch a running relay.
#
# Plain SRT callers are ffmpeg. Bonded input is simulated with
# tests/e2e/bonded_caller.c: a libsrt socket-group caller whose legs use different
# loopback source addresses (127.0.0.1 and 127.0.0.2), which gives the relay two
# distinct peers on one machine. It is built once into objs/ with the image that
# scripts/build-local.sh makes; E2E_SKIP_BONDED=1 skips the bonded scenarios.
# tests/e2e/closing_sink.c, a downstream that accepts the handshake and then drops
# the connection, is built the same way.
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
CLOSING_SINK="$REPO_DIR/objs/closing-sink"

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
        # The end-of-scenario dump comes after the session is gone, so the state a check saw
        # (a bonded leg still joining, say) is only visible here. The first failure is the
        # informative one; the later ones usually follow from it.
        if [[ -z "${SNAPSHOT_TAKEN:-}" ]]; then
            SNAPSHOT_TAKEN=1
            echo "  --- status when this check failed"
            { status 2>/dev/null | jq -c . 2>/dev/null | cut -c1-2000 | sed 's/^/  | /'; } || true
        fi
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

write_config() { # write_config <file> <passphrase> [<output_passphrase>]
    local extra=""
    (($# >= 3)) && extra=",\"output_passphrase\":\"$3\""
    printf '{"input_host":"127.0.0.1","input_port":%d,"output_host":"127.0.0.1","output_port":%d,"status_port":%d,"passphrase":"%s"%s}\n' \
        "$IN" "$OUT" "$STATUS" "$2" "$extra" >"$1"
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

# PUB_RATE=4M sends a 720p stream of that bitrate instead of the tiny default one: a
# real encoder sends a packet every few milliseconds, which the default does not.
start_pub() { # start_pub <streamid> [passphrase] [seconds]; sets PUB_PID
    local extra=""
    [[ -n "${2:-}" ]] && extra="&passphrase=$2&pbkeylen=16"
    local video=(-f lavfi -i testsrc=size=160x90:rate=15)
    # A keyframe every second: a downstream that joins mid-stream (after a retry) can
    # start decoding at once. With x264's default of 250 frames the ffmpeg sink gives
    # up probing after about 7 s and drops the connection.
    local encode=(-c:v libx264 -preset ultrafast -tune zerolatency -g 15)
    if [[ -n "${PUB_RATE:-}" ]]; then
        video=(-f lavfi -i testsrc2=size=1280x720:rate=30)
        encode=(-c:v libx264 -preset ultrafast -tune zerolatency -b:v "$PUB_RATE" -maxrate "$PUB_RATE" -bufsize 1M)
    fi
    ffmpeg -nostdin -hide_banner -loglevel error -re \
        "${video[@]}" -t "${3:-120}" \
        "${encode[@]}" -f mpegts "srt://127.0.0.1:$IN?streamid=$1&transtype=live$extra" \
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

# build_test_tool <name>: builds tests/e2e/<name with - as _>.c into objs/<name>
# with the image that scripts/build-local.sh makes.
build_test_tool() {
    local name="$1" source="$REPO_DIR/tests/e2e/${1//-/_}.c"
    [[ -x "$REPO_DIR/objs/$name" && "$REPO_DIR/objs/$name" -nt "$source" ]] && return 0
    command -v docker >/dev/null 2>&1 || { echo "  (docker is needed to build $name)"; return 1; }
    local image="goryu-dev:${SRT_TAG:-$(sed -n 's/^ARG SRT_TAG=//p' "$REPO_DIR/scripts/goryu.Dockerfile")}"
    docker image inspect "$image" >/dev/null 2>&1 || { echo "  (image $image is missing: run scripts/build-local.sh)"; return 1; }
    mkdir -p "$REPO_DIR/objs"
    docker run --rm --user "$(id -u):$(id -g)" -v "$REPO_DIR":/w -w /w "$image" \
        sh -c "gcc -O2 -Wall -Wextra -o objs/$name tests/e2e/${name//-/_}.c \$(pkg-config --cflags --libs srt) -lpthread" \
        >"$WORK/$name-build.log" 2>&1 || { cat "$WORK/$name-build.log"; return 1; }
}
build_bonded_caller() { build_test_tool bonded-caller; }

BOND_N=0
# start_bonded <streamid> [bonded_caller options...]: ffmpeg test pattern piped
# into a bonded caller; sets PUB_PID to the caller's pid. BOND_SECONDS bounds it.
# BOND_RATE=4M sends a 720p stream of that bitrate instead of the tiny default one: a
# main/backup group only settles on a main leg when there is real traffic to judge.
start_bonded() {
    local sid="$1"; shift
    local fifo="$WORK/bond$((++BOND_N)).fifo"
    mkfifo "$fifo"
    local video=(-f lavfi -i testsrc=size=160x90:rate=15)
    local encode=(-c:v libx264 -preset ultrafast -tune zerolatency)
    if [[ -n "${BOND_RATE:-}" ]]; then
        video=(-f lavfi -i testsrc2=size=1280x720:rate=30)
        encode=(-c:v libx264 -preset ultrafast -tune zerolatency -b:v "$BOND_RATE" -maxrate "$BOND_RATE" -bufsize 1M)
    fi
    ffmpeg -nostdin -hide_banner -loglevel error -re "${video[@]}" \
        -t "${BOND_SECONDS:-120}" "${encode[@]}" -f mpegts - \
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
    check "a directory as the config file exits 1" bash -c \
        "LD_LIBRARY_PATH='$LIB_DIR' '$BIN' '$WORK' >/dev/null 2>&1; [ \$? -eq 1 ]"
}

scenario_bad_config() {
    ports 1
    local name body
    local six="\"input_host\":\"127.0.0.1\",\"input_port\":$IN,\"output_host\":\"127.0.0.1\",\"output_port\":$OUT,\"status_port\":$STATUS,\"passphrase\":\"\""
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
a duplicate key|{$six,"input_host":"127.0.0.2"}
a nested object|{$six,"extra":{"output_passphrase":""}}
text after the object|{$six} {$six}
a missing comma|{"input_host":"127.0.0.1" "input_port":$IN,"output_host":"127.0.0.1","output_port":$OUT,"status_port":$STATUS,"passphrase":""}
EOF
}

# An optional key with a typo is ignored, which changes what the relay does (here the
# output would fall back to the input's passphrase), so the relay says so.
scenario_unknown_key() {
    ports 23
    printf '{"input_host":"127.0.0.1","input_port":%d,"output_host":"127.0.0.1","output_port":%d,"status_port":%d,"passphrase":"","outpt_passphrase":""}\n' \
        "$IN" "$OUT" "$STATUS" >"$WORK/c.json"
    start_goryu "$WORK/c.json" "$WORK/g.log"
    check "a config with an unknown key still starts" status
    check "the unknown key is named in the log" log_has \
        'ignoring 1 unknown key(s), the first is "outpt_passphrase"' "$WORK/g.log"
}

# The status port has no authentication, so a client that connects and sends nothing
# must not keep the endpoint, or shutdown, waiting.
idle_status_client() { # connects to the status port and sends nothing for 30 s
    sleep 30 3<>"/dev/tcp/127.0.0.1/$STATUS" &
    PIDS+=("$!")
    sleep 0.3
}

scenario_status_idle_client() {
    ports 24
    write_config "$WORK/c.json" ""
    start_goryu "$WORK/c.json" "$WORK/g.log"
    check "status answers" status
    idle_status_client
    check "status answers again within 8 s of a client that connects and sends nothing" \
        wait_for 8 status
    check "the idle client is dropped and logged" wait_for 5 log_has "Status client sent no request" "$WORK/g.log"
    idle_status_client
    kill -TERM "$GPID"
    check "SIGTERM with an idle status client attached exits within 6 s" exited_within "$GPID" 6
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
    check "a plain input reports belatedAvgMs" wait_for 5 field_is demo/key '.input.belatedAvgMs | type' number
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
    # Cleared once the downstream connection has stayed up for OUTPUT_HEALTHY_MS (10 s).
    check "retry counter resets once the downstream has stayed up" wait_for 15 field_is demo/down .retryFailures 0
    check "data flows after recovery" forwarding demo/down
}

# The downstream accepts the handshake, so every connect succeeds, and drops the
# connection 50 ms later, while the relay is already sending at a real encoder's rate. The retry delay has to
# keep growing across those cycles: a successful connect (or a few packets accepted
# into libsrt's send buffer before the drop) does not mean the downstream is healthy.
scenario_downstream_drops() {
    ports 22
    write_config "$WORK/c.json" ""
    start_goryu "$WORK/c.json" "$WORK/g.log"
    LD_LIBRARY_PATH="$LIB_DIR" "$CLOSING_SINK" --port "$OUT" --hold-ms 50 >"$WORK/closing-sink.log" 2>&1 &
    PIDS+=("$!")
    check "the closing sink is listening" wait_for 5 ss -Hlnu "sport = :$OUT"
    PUB_RATE=4M start_pub "demo/drop"
    check "the downstream accepted the relay's connection" wait_for 10 log_has "accepted" "$WORK/closing-sink.log"
    check "send failures are retried with the first delay" wait_for 15 log_has "Output send retry .* failures=1 retry_ms=1000" "$WORK/g.log"
    check "the delay doubles on the next failure" wait_for 15 log_has "Output send retry .* failures=2 retry_ms=2000" "$WORK/g.log"
    check "and again" wait_for 25 log_has "Output send retry .* failures=3 retry_ms=4000" "$WORK/g.log"
    check "the status shows the count" field_ge demo/drop .retryFailures 3
    check "the publisher stays connected" active demo/drop
    check "relay is still alive" kill -0 "$GPID"
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

    check "a status port variable that is not a port is ignored, and the log says so" bash -c \
        "SRT_BONDING_STATUS_PORT=nope LD_LIBRARY_PATH='$LIB_DIR' timeout 3 '$BIN' '$in_uri' '$out_uri' >'$WORK/u4.log' 2>&1; \
         grep -q 'SRT_BONDING_STATUS_PORT is not a port number' '$WORK/u4.log'"

    start_sink
    env "$env_status" LD_LIBRARY_PATH="$LIB_DIR" "$BIN" "$in_uri&passphrase=$PASSPHRASE" \
        "$out_uri&passphrase=$PASSPHRASE&pbkeylen=16" >"$WORK/g.log" 2>&1 &
    GPID=$!
    PIDS+=("$GPID")
    check "the relay starts from two URIs" wait_for 5 status
    check "the passphrase is not logged in URI mode either" bash -c "! grep -q '$PASSPHRASE' '$WORK/g.log'"
    check "the log shows the redacted output URI" log_has "passphrase=<redacted>" "$WORK/g.log"
}

scenario_output_passphrase() {
    ports 20
    local p1="$PASSPHRASE" p2="other-passphrase-4567"
    # output_passphrase_case <label> <input-pass> <output-pass or -> <sink-pass> <publisher-pass> <flows|blocked>
    # "-" leaves output_passphrase out of the config, so it defaults to the input's.
    output_passphrase_case() {
        local label="$1" in_pass="$2" out_pass="$3" sink_pass="$4" pub_pass="$5" expect="$6"
        kill_pids 2>/dev/null
        : >"$WORK/g.log"
        if [[ "$out_pass" == "-" ]]; then
            write_config "$WORK/c.json" "$in_pass"
        else
            write_config "$WORK/c.json" "$in_pass" "$out_pass"
        fi
        start_sink "$sink_pass"
        start_goryu "$WORK/c.json" "$WORK/g.log"
        start_pub "demo/op" "$pub_pass"
        if [[ "$expect" == flows ]]; then
            check "$label: the stream reaches the destination" wait_for 12 active_and_connected demo/op
            check "$label: bytes keep growing" forwarding demo/op
        else
            check "$label: the encoder is accepted" wait_for 10 active demo/op
            sleep 7
            check "$label: the destination is refused and retried" field_ge demo/op .retryFailures 1
            check "$label: and never connects" field_is demo/op .outputConnected false
        fi
        check "$label: neither passphrase is in the relay's log" bash -c \
            "! grep -qE '$p1|$p2' '$WORK/g.log'"
    }

    output_passphrase_case "encrypted input, plain destination" "$p1" "" "" "$p1" flows
    output_passphrase_case "a different passphrase on each side" "$p1" "$p2" "$p2" "$p1" flows
    output_passphrase_case "plain input, encrypted destination" "" "$p2" "$p2" "" flows
    output_passphrase_case "no output_passphrase: the destination uses the input's" "$p1" - "$p1" "$p1" flows
    output_passphrase_case "an empty output_passphrase does not fall back to the input's" "$p1" "" "$p1" "$p1" blocked
    output_passphrase_case "the destination uses its own passphrase, not the input's" "$p1" "$p2" "$p1" "$p1" blocked

    kill_pids 2>/dev/null
    write_config "$WORK/bad.json" "$p1" "short"
    check "a too-short output_passphrase is refused" bash -c \
        "LD_LIBRARY_PATH='$LIB_DIR' timeout 5 '$BIN' '$WORK/bad.json' >'$WORK/bad.log' 2>&1; [ \$? -eq 1 ]"
    check "the error names output_passphrase and not its value" bash -c \
        "grep -q 'output_passphrase' '$WORK/bad.log' && ! grep -qE 'short|$p1' '$WORK/bad.log'"
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
    check "each leg's arrival is logged" bash -c \
        "grep -q 'Bonded leg joined streamid=demo/bond leg=127.0.0.1:.* legs=' '$WORK/g.log' && grep -q 'Bonded leg joined streamid=demo/bond leg=127.0.0.2:.* legs=2' '$WORK/g.log'"
    check "both legs are running" field_is demo/bond '[.input.legs[].state] | unique | join(",")' running
    check "each leg has its own source address" field_is demo/bond '[.input.legs[].ip] | sort | join(",")' 127.0.0.1,127.0.0.2
    check "each leg has received data" field_is demo/bond '[.input.legs[].recvUniquePacketsTotal > 0] | all' true
    check "each leg reports its RTT" field_is demo/bond '[.input.legs[].rttMs | type] | unique | join(",")' number
    check "latency is the negotiated 240 ms, derived from the legs" field_is demo/bond .input.latencyMs 240
    check "group-level bandwidth is null, as documented" field_is demo/bond '.input.bandwidthMbps == null' true
    check "group-level belatedAvgMs is null, as documented" field_is demo/bond '.input.belatedAvgMs == null' true
    check "group-level loss is null, as documented" field_is demo/bond '.input.recvLossTotal == null' true
    check "bytes are forwarded and keep growing" forwarding demo/bond
    sleep 5
    check "the relay forwards each packet once, not once per leg" forwarded_once_not_per_leg demo/bond
    check "a leg that goes down disappears from legs[]" wait_for 15 leg_count_is demo/bond 1
    check "the remaining leg is the one that was not dropped" field_is demo/bond '.input.legs[0].ip' 127.0.0.1
    check "the lost leg is logged with its address and the legs left" wait_for 5 log_has "Bonded leg left streamid=demo/bond leg=127.0.0.2:[0-9]* legs=1" "$WORK/g.log"
    check "exactly two joins were logged, none for a leg that stayed" bash -c \
        "[ \$(grep -c 'Bonded leg joined streamid=demo/bond' '$WORK/g.log') -eq 2 ] && [ \$(grep -c 'Bonded leg left streamid=demo/bond' '$WORK/g.log') -eq 1 ]"
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
    check "the late leg's arrival is logged with the new leg count" wait_for 5 log_has "Bonded leg joined streamid=demo/legs leg=127.0.0.2:[0-9]* legs=2" "$WORK/g.log"
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

scenario_bonded_backup() {
    ports 21
    write_config "$WORK/c.json" ""
    start_sink
    start_goryu "$WORK/c.json" "$WORK/g.log"
    # 127.0.0.1 has the higher weight, so it is the main leg; it is closed at 24 s.
    BOND_SECONDS=45 BOND_RATE=4M start_bonded demo/bk --type backup \
        --locals 127.0.0.1,127.0.0.2 --weights 10,5 --drop-leg 0 --drop-after 24
    check "a main/backup group is accepted and forwarded" wait_for 12 active_and_connected demo/bk
    check "it is reported as bonded with both legs" wait_for 8 leg_count_is demo/bk 2
    leg_field() { stream_jq demo/bk "[.input.legs[] | select(.ip==\"$1\") | $2][0]"; }
    check "the main leg (higher weight) carries the stream" wait_for 15 bash -c \
        "$(declare -f stream_jq status leg_field); STATUS=$STATUS; [ \"\$(leg_field 127.0.0.1 .state)\" = running ]"
    check "the backup leg is idle" wait_for 15 bash -c \
        "$(declare -f stream_jq status leg_field); STATUS=$STATUS; [ \"\$(leg_field 127.0.0.2 .state)\" = idle ]"
    check "the idle backup carries no data" bash -c \
        "$(declare -f stream_jq status leg_field); STATUS=$STATUS; [ \"\$(leg_field 127.0.0.2 '.recvRateMbps < 0.2')\" = true ]"
    # A one-second rate sample can dip on a starved runner, so give it a few tries.
    check "the main leg carries the whole stream" wait_for 10 bash -c \
        "$(declare -f stream_jq status leg_field); STATUS=$STATUS; [ \"\$(leg_field 127.0.0.1 '.recvRateMbps > 2')\" = true ]"
    check "bytes keep growing" forwarding demo/bk
    check "the loss of the main leg is logged" wait_for 30 log_has "Bonded leg left streamid=demo/bk leg=127.0.0.1:[0-9]* legs=1" "$WORK/g.log"
    check "the backup leg takes over and is now running" wait_for 10 bash -c \
        "$(declare -f stream_jq status leg_field); STATUS=$STATUS; [ \"\$(leg_field 127.0.0.2 .state)\" = running ]"
    check "forwarding continues on the backup leg" forwarding demo/bk
    check "the downstream never reconnected" field_is demo/bk .retryFailures 0
    check "and is still connected" field_is demo/bk .outputConnected true
}

scenario_bonded_flap() {
    ports 19
    write_config "$WORK/c.json" ""
    start_sink
    start_goryu "$WORK/c.json" "$WORK/g.log"
    BOND_SECONDS=60 start_bonded demo/flap --locals 127.0.0.1,127.0.0.2 \
        --flap-leg 1 --flap-interval 1.2 --flap-toggles 18
    check "the flapping group is accepted" wait_for 10 active_and_connected demo/flap
    check "the relay keeps forwarding while a leg flaps" forwarding demo/flap
    # 2 joins at the start and about 18 changes: more than the 12 lines allowed per 30 s
    check "the flapping is summarised once its window ends" wait_for 50 log_has \
        "Bonded leg events suppressed streamid=demo/flap count=[0-9]*" "$WORK/g.log"
    check "no more than 12 leg lines were written in that window" bash -c \
        "[ \$(grep -cE 'Bonded leg (joined|left) streamid=demo/flap' '$WORK/g.log') -eq 12 ]"
    check "the relay is still running and the stream is up" active demo/flap
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

# libsrt takes a streamid of at most 512 bytes. The one in the downstream URI is what a
# publisher without a streamid gets, and a longer one used to be refused by libsrt and
# ignored by the relay, which then connected downstream with no streamid at all.
scenario_long_output_streamid() {
    ports 25
    local long_id
    long_id="$(printf 'x%.0s' $(seq 1 600))"
    local in_uri="srt://127.0.0.1:$IN?mode=listener&groupconnect=1&transtype=live"
    local out_uri="srt://127.0.0.1:$OUT?mode=caller&transtype=live&streamid=$long_id"
    start_sink
    SRT_BONDING_STATUS_PORT="$STATUS" LD_LIBRARY_PATH="$LIB_DIR" "$BIN" "$in_uri" "$out_uri" >"$WORK/g.log" 2>&1 &
    GPID=$!
    PIDS+=("$GPID")
    wait_for 5 status
    start_pub ""
    check "the publisher is accepted" wait_for 10 status_has '.streamStates[0].inputActive == true'
    check "the refused streamid is logged by name, not by value" wait_for 10 log_has "libsrt rejected option streamid" "$WORK/g.log"
    check "the rejection line does not carry the streamid" bash -c \
        "grep 'libsrt rejected option' '$WORK/g.log' | grep -qv 'xxxxxxxxxxxxxxxx'"
    check "the relay does not connect downstream without it" status_has '.streamStates[0].outputConnected == false'
    check "the failure is retried with backoff" wait_for 20 status_has '.streamStates[0].retryFailures >= 2'
    check "the stream's lastError says why" status_has '.streamStates[0].lastError | contains("libsrt rejected option streamid")'
    check "the relay is still alive" kill -0 "$GPID"
}

# glibc sizes a thread's stack from the stack limit of the process. The status reply
# used to build its snapshot of the whole session table (about 1.5 MB) on the status
# thread's stack, so under a 1 MB limit the first status request crashed the relay.
scenario_status_small_stack() {
    ports 26
    write_config "$WORK/c.json" ""
    (ulimit -s 1024; exec env LD_LIBRARY_PATH="$LIB_DIR" "$BIN" "$WORK/c.json" >"$WORK/g.log" 2>&1) &
    GPID=$!
    PIDS+=("$GPID")
    check "the status endpoint answers under a 1 MB stack limit" wait_for 5 status
    check "it answers again" status_has '.streamStates | length == 0'
    check "the relay is still alive" kill -0 "$GPID"
}

# --- run --------------------------------------------------------------------

ALL=(cli bad_config unknown_key uri_mode output_passphrase forward downstream_down downstream_drops duplicate takeover two_streams passphrase sigterm
    status_idle_client shutdown_silent churn bonded bonded_legs bonded_flap bonded_backup bonded_duplicate bonded_takeover bonded_passphrase bonded_shutdown
    bonded_abrupt_end long_output_streamid status_small_stack)
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
    if [[ "$name" == downstream_drops ]] && ! build_test_tool closing-sink; then
        echo "== $name"; FAIL=$((FAIL + 1)); FAILED+=("$name: the closing sink could not be built"); echo "  FAIL  closing sink is available"
        continue
    fi
    echo "== $name"
    : >"$WORK/sink.log"; : >"$WORK/pub.log"; : >"$WORK/bonded.log"
    failed_before=$FAIL
    SNAPSHOT_TAKEN=
    "scenario_$name"
    if ((FAIL > failed_before)); then
        # A check that failed in CI leaves nothing else behind: the work dir is removed on exit.
        echo "  --- relay log, last 40 lines"
        tail -n 40 "$WORK/g.log" 2>/dev/null | cut -c1-300 | sed 's/^/  | /'
        echo "  --- publisher log, last 20 lines"
        tail -n 20 "$WORK/pub.log" 2>/dev/null | cut -c1-300 | sed 's/^/  | /'
        echo "  --- downstream sink log, last 20 lines"
        tail -n 20 "$WORK/sink.log" 2>/dev/null | cut -c1-300 | sed 's/^/  | /'
        echo "  --- bonded caller log, last 20 lines"
        tail -n 20 "$WORK/bonded.log" 2>/dev/null | cut -c1-300 | sed 's/^/  | /'
        echo "  --- status"
        status 2>/dev/null | jq -c . 2>/dev/null | cut -c1-2000 | sed 's/^/  | /'
    fi
    kill_pids 2>/dev/null
done

echo
echo "passed: $PASS, failed: $FAIL"
if ((FAIL > 0)); then
    printf '  - %s\n' "${FAILED[@]}"
    exit 1
fi
