---
name: run-goryu
description: Build Goryu locally and run an isolated instance (own ports, local downstream, ffmpeg publisher) to check forwarding, the status API, downstream retry and duplicate-publisher handling, or to run the smoke test. Use to verify a change to src/ or the scripts in a real process. Does not touch any deployed relay.
---

# Run Goryu locally

Paths are relative to the repo root. Everything below was run on a dev machine
with Docker, `ffmpeg` (system build, with `srt`), `curl` and `jq`.

## Build and smoke test

```bash
bash scripts/build-local.sh      # Docker build; writes objs/goryu and objs/lib/
LD_LIBRARY_PATH=objs/lib objs/goryu --version      # prints VERSION, e.g. v3.2.1
bash scripts/smoke-test.sh       # rebuilds, starts from ./goryu.json, checks /status
bash scripts/test-unit.sh        # unit tests, no Docker
bash scripts/test-e2e.sh         # end-to-end scenarios on spare ports (about a minute)
bash scripts/test-e2e.sh passphrase takeover      # or just some scenarios
bash scripts/format.sh check     # CI's format job (drop `check` to rewrite files)
```

`scripts/test-e2e.sh` automates most of what is described below and tests the binary
in `objs/`; point `GORYU_PATH`/`GORYU_LIB_DIR` at another build to compare, for
example the last release against your change. Use the manual steps to explore a
failure interactively.

- `objs/goryu` alone fails with `cannot open shared object file: libsrt.so.1.5`.
  Set `LD_LIBRARY_PATH=objs/lib` on the goryu command only; exporting it for the
  shell breaks `curl` and others (they load the bundled libcrypto).
- The smoke test uses the repo's `goryu.json` (ports 10081/10080/8081). Stop any
  real Goryu on those ports first, or it checks the wrong process and fails.
- Docker pulls the pinned base image and clones libsrt, so the first build needs
  the network. Later builds are cached.

## Isolated instance (plain SRT, end to end)

Use ports that aren't the defaults, and a work dir outside the repo (it holds a
config with whatever passphrase you pick).

```bash
W=$(mktemp -d)                                         # scratch dir, not in the repo
cat > $W/goryu.json <<'EOF'
{"input_host":"127.0.0.1","input_port":19081,"output_host":"127.0.0.1",
 "output_port":19080,"status_port":19082,"passphrase":""}
EOF

# 1. downstream: an SRT listener that accepts the publish
ffmpeg -hide_banner -loglevel error -i "srt://127.0.0.1:19080?mode=listener&transtype=live" \
  -c copy -f null - &
# 2. the relay
LD_LIBRARY_PATH=objs/lib objs/goryu $W/goryu.json > $W/goryu.log 2>&1 &
# 3. an encoder: a plain SRT caller with a streamid
ffmpeg -hide_banner -loglevel error -re -f lavfi -i testsrc=size=640x360:rate=25 \
  -f lavfi -i sine -c:v libx264 -preset ultrafast -c:a aac -f mpegts \
  "srt://127.0.0.1:19081?streamid=demo/key&transtype=live" &

sleep 5
curl -s http://127.0.0.1:19082/status | jq '.streamStates[] |
  {streamId, inputActive, outputConnected, retryFailures, forwardedBytes,
   bonded: .input.bonded, peer: .input.peer}'
cat $W/goryu.log
```

Expect `inputActive` and `outputConnected` true, `bonded` false, `forwardedBytes`
growing between two polls, and the log to show `Accepted plain SRT source`. When
the publisher stops, the log shows `Connection closed ... reason=input_disconnected`
and the stream leaves `activeStreamIds`. The downstream ffmpeg then exits with
`Input/output error`; that is its reaction to the publish ending, not a fault.

Clean up by killing the three jobs you started (`kill %1 %2 %3` in the same
shell) and `rm -rf $W`. Don't use `pkill -f` with a broad pattern; it can match
your own shell's command line.

## Failure cases worth running

All of these are automated in `scripts/test-e2e.sh`; the descriptions say what to
look for when you run one by hand. Compare against
[Reliability](../../docs/3-operations/1-reliability.md).

- **Downstream down** (skip step 1): the encoder stays connected, `outputConnected`
  is false, `retryFailures` climbs and the stream's `lastError` reads
  `Failed to publish to downstream output: srt_connect ... connection timed out;
  retrying in N ms`. The log has one `Output publish retry ... retry_ms=` line per
  attempt, with the delay doubling 1, 2, 4 s up to 16 s. Each connect attempt takes
  about 3 s. Start the downstream later and `outputConnected` turns true with
  `retryFailures` back at 0.
- **Duplicate publisher**: with one publisher active, start a second with the same
  `streamid`. After up to 5 s the log has
  `Duplicate publisher rejected for active streamid=...`; the first keeps running.
- **Stale takeover**: needs a publisher whose SRT connection is alive but sends
  nothing for 5 s, then a second one. **`kill -STOP` on the first ffmpeg does not
  do this**: libsrt drops a silent connection after 5 s on its own, so the relay
  just sees a closed session and no takeover happens. `scripts/test-e2e.sh` holds
  the connection open by remuxing a FIFO that has been fed one clip
  (`start_silent_pub`); copy that. The log then shows
  `Duplicate publisher takeover for stale streamid=...` and the old session closes.
- **Passphrase**: set the same `passphrase` (10 to 79 characters) in `goryu.json`,
  on the encoder and on the downstream listener. A wrong one is rejected inside
  libsrt and leaves no Goryu log line, by design. A passphrase outside 10 to 79
  characters makes the relay refuse to start. The passphrase must never appear in
  the relay's log.

## Bonded input

`ffmpeg` cannot open a socket group, so use `tests/e2e/bonded_caller.c`: it reads
MPEG-TS on stdin and sends it as a bonded group. `scripts/test-e2e.sh` builds it
into `objs/bonded-caller` (Docker, using the image `build-local.sh` made). Legs
bound to different loopback addresses look like two links:

```bash
ffmpeg -nostdin -loglevel error -re -f lavfi -i testsrc=size=160x90:rate=15 -t 30 \
  -c:v libx264 -preset ultrafast -tune zerolatency -f mpegts - |
LD_LIBRARY_PATH=objs/lib objs/bonded-caller --port 19081 --streamid demo/bond \
  --locals 127.0.0.1,127.0.0.2 --drop-leg 1 --drop-after 12
```

In `/status` you should see `bonded: true`, `peer: null`, two `legs[]` with their own
addresses, `latencyMs: 240` (derived from the legs) and group-level
`bandwidthMbps: null`. After 12 s one leg disappears from `legs[]` and
`forwardedBytes` keeps growing. Other options: `--add-local IP --add-after SEC` (a leg
joining later), `--flap-leg I --flap-interval S --flap-toggles N` (a link going up and
down), `--passphrase`, `--latency`. The relay log shows each change as
`Bonded leg joined|left streamid=... leg=<ip>:<port> legs=N`. To hold a group open without sending
media, feed the caller a FIFO that has had one clip written and is kept open (see
`start_silent_bonded` in the e2e script).

## What this does not cover

- Real network conditions on bonded links: loss, jitter, latency and a physical link
  failing. The simulated legs are loopback, so per-leg loss and retransmission are
  zero. A change to that handling needs real links.
- Load, long soak and a real downstream with a publish hook.
- The system `ffmpeg` here is 8.x. Bunryu notes that ffmpeg 8 stalls pulling from SRS
  over SRT; this local pair (ffmpeg to Goryu to ffmpeg) did not show that, but use
  a 7.x build if you test against SRS.
