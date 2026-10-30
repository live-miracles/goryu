#!/usr/bin/env bash
# Tests the Docker packaging in docker/: the runtime image and compose.yaml. The image
# installs a published release, so this checks the packaging, not the commit's source
# (the other test scripts do that).
#
#   bash scripts/test-docker.sh              image: version, user, status API, clean stop
#   DEMO=1 bash scripts/test-docker.sh       also compose: MediaMTX + Goryu + a bonded
#                                            test sender, read back from MediaMTX
#                                            (builds libsrt once, a few minutes)
#
# GORYU_VERSION pins the release the image installs (default: latest).
# DOCKER_TEST_PORT is the first of four spare ports on the host (default 24600).
set -euo pipefail

REPO_DIR="$(cd "$(dirname "$0")/.." && pwd)"
BASE="${DOCKER_TEST_PORT:-24600}"
IN_PORT=$BASE
OUT_PORT=$((BASE + 1))
STATUS_PORT=$((BASE + 2))
IMAGE="goryu-docker-test"
PROJECT="goryu-docker-test-$$"
WORK_DIR="$(mktemp -d)"
chmod 755 "$WORK_DIR"

compose=(docker compose -p "$PROJECT" -f "$REPO_DIR/docker/compose.yaml" -f "$WORK_DIR/override.yaml")

fail() {
    echo "FAIL: $*" >&2
    exit 1
}

cleanup() {
    docker rm -f "$PROJECT-relay" >/dev/null 2>&1 || true
    if [[ -f "$WORK_DIR/override.yaml" ]]; then
        "${compose[@]}" --profile demo down >/dev/null 2>&1 || true
    fi
    rm -rf "$WORK_DIR"
}
trap cleanup EXIT

status() { curl -s -m 3 "http://127.0.0.1:$STATUS_PORT/status"; }

wait_for() { # wait_for <seconds> <description> <command...>
    local limit=$1 what=$2 i
    shift 2
    for ((i = 0; i < limit * 2; i++)); do
        if "$@" >/dev/null 2>&1; then return 0; fi
        sleep 0.5
    done
    fail "timed out waiting for $what"
}

# The runtime image must use the base the release is built on, because the archive links
# against its glibc. The digest is written in both Dockerfiles (the image is built from
# docker/ alone, so it cannot read the other); CI and the release run this, so the two
# cannot drift apart unnoticed.
build_base="$(sed -n 's/^FROM \(ubuntu:[^ ]*\).*/\1/p' "$REPO_DIR/scripts/goryu.Dockerfile")"
image_base="$(sed -n 's/^ARG BASE=//p' "$REPO_DIR/docker/Dockerfile")"
[[ -n "$build_base" && "$build_base" == "$image_base" ]] ||
    fail "docker/Dockerfile's BASE ($image_base) is not the base scripts/goryu.Dockerfile builds on ($build_base)"

for port in "$IN_PORT" "$OUT_PORT" "$STATUS_PORT"; do
    if ss -H -ltun | awk '{print $5}' | grep -q ":$port$"; then
        fail "port $port is in use; set DOCKER_TEST_PORT"
    fi
done

cat >"$WORK_DIR/goryu.json" <<EOF
{"input_host":"0.0.0.0","input_port":$IN_PORT,"output_host":"127.0.0.1","output_port":$OUT_PORT,"status_port":$STATUS_PORT,"passphrase":""}
EOF
chmod 644 "$WORK_DIR/goryu.json"

echo "== build the image"
docker build -q --no-cache --build-arg "GORYU_VERSION=${GORYU_VERSION:-latest}" -t "$IMAGE" "$REPO_DIR/docker" >/dev/null

version="$(docker run --rm "$IMAGE" --version)"
echo "release in image: $version"
if [[ -n "${GORYU_VERSION:-}" && "${GORYU_VERSION}" != latest && "$version" != "$GORYU_VERSION" ]]; then
    fail "asked for $GORYU_VERSION, image has $version"
fi

[[ "$(docker run --rm --entrypoint id "$IMAGE" -u)" != 0 ]] || fail "the image runs as root"

if docker run --rm "$IMAGE" >/dev/null 2>&1; then
    fail "a missing config file should be refused"
fi

echo "== run it with the example config"
docker run -d --name "$PROJECT-relay" --network host \
    -v "$WORK_DIR/goryu.json:/etc/goryu/goryu.json:ro" "$IMAGE" >/dev/null
wait_for 10 "the status API" status
status | jq -e '.pid and (.streamStates | type == "array")' >/dev/null || fail "unexpected status response"

start=$(date +%s.%N)
docker stop "$PROJECT-relay" >/dev/null
elapsed=$(echo "$(date +%s.%N) - $start" | bc)
code="$(docker inspect -f '{{.State.ExitCode}}' "$PROJECT-relay")"
[[ "$code" == 0 ]] || fail "stop exited with $code, expected 0"
(($(echo "$elapsed < 5" | bc))) || fail "stop took ${elapsed}s: SIGTERM is not handled as PID 1"
echo "stopped cleanly in ${elapsed}s"

echo "== compose file"
cat >"$WORK_DIR/override.yaml" <<EOF
services:
  mediamtx:
    environment:
      MTX_SRTADDRESS: ":$OUT_PORT"
  goryu:
    volumes:
      - $WORK_DIR/goryu.json:/etc/goryu/goryu.json:ro
  sender:
    environment:
      PORT: "$IN_PORT"
EOF
"${compose[@]}" config -q || fail "compose.yaml is not valid"

if [[ "${DEMO:-0}" == 1 ]]; then
    command -v ffmpeg >/dev/null || fail "DEMO=1 needs ffmpeg on the host to read the stream back"
    echo "== MediaMTX + Goryu + bonded sender"
    "${compose[@]}" --profile demo up -d --build >/dev/null
    wait_for 40 "a bonded stream with two legs" \
        bash -c "curl -s -m 3 http://127.0.0.1:$STATUS_PORT/status | jq -e '.streamStates[0] | .input.bonded and (.input.legs | length == 2) and .outputConnected' >/dev/null"
    first="$(status | jq '.streamStates[0].forwardedBytes')"
    sleep 3
    second="$(status | jq '.streamStates[0].forwardedBytes')"
    ((second > first)) || fail "forwardedBytes did not grow ($first -> $second)"
    frames="$(timeout 20 ffmpeg -hide_banner -loglevel error -i "srt://127.0.0.1:$OUT_PORT?streamid=read:demo" \
        -t 3 -f null - -stats 2>&1 | tr '\r' '\n' | sed -n 's/.*frame= *\([0-9]*\).*/\1/p' | tail -1)"
    [[ -n "$frames" && "$frames" -gt 0 ]] || fail "could not read the stream back from MediaMTX"
    echo "bonded stream forwarded ($first -> $second bytes), $frames frames read back from MediaMTX"
fi

echo "PASS"
