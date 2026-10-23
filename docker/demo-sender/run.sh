#!/bin/sh
# A test pattern as MPEG-TS into a bonded caller, one leg per local address. EXTRA is
# passed on to bonded-caller (for example "--drop-leg 1 --drop-after 20" to lose a leg).
set -eu
: "${PORT:=10081}" "${STREAMID:=publish:demo}" "${HOST:=127.0.0.1}" "${LOCALS:=127.0.0.1,127.0.0.2}" "${EXTRA:=}"
ffmpeg -nostdin -hide_banner -loglevel error -re \
    -f lavfi -i testsrc=size=1280x720:rate=25 -f lavfi -i sine=frequency=440 \
    -c:v libx264 -preset ultrafast -tune zerolatency -g 25 -b:v 2M -c:a aac -f mpegts - |
    bonded-caller --host "$HOST" --port "$PORT" --streamid "$STREAMID" --locals "$LOCALS" $EXTRA
