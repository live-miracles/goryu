# Agent Guide

## Project overview

Goryu is a small SRT bonding relay for Linux: one C binary (about 2,800 lines on
top of libsrt 1.5.6) with a JSON config of six fields (plus an optional destination passphrase) and a loopback JSON status
API. It accepts a bonded SRT socket group, or an ordinary single-link SRT
caller, on one listener, merges the legs into one deduplicated MPEG-TS stream and
forwards it to a downstream SRT server under the same `streamid`.

It is a standalone project, and it is also the bonded-SRT ingress of
[Bunryu](https://github.com/live-miracles/bunryu), which pins a release,
generates `goryu.json` and polls the status API. Read `docs/` before changing
behavior; `docs/4-reference/2-architecture.md` is the shortest tour. Use the terms
in `GLOSSARY.md` (a *session* holds a *stream*; they are not the same thing), and read
the decision records in `docs/adr/` that touch the area before you change it: each
names the alternatives that were rejected and why. A new decision that is hard to
reverse, surprising without context and a real trade-off gets the next number there.

## Compatibility contract

Bunryu is a consumer you cannot see from this repo, so these are interfaces:

- **Release asset name** `goryu-linux-x86_64.tar.gz` (unpacking to `bin/goryu`
  and `lib/`) and `checksums.sha256`. Bunryu's installer and Docker image
  download it by name.
- **Status API shape** (`docs/4-reference/1-status-api.md`). Bunryu's
  `src/services/srtRelay.ts` mirrors the field names and the `null` rules (for
  example, a bonded input leaves `bandwidthMbps`..`rcvBufMs` null and a failed
  leg disappears from `legs[]`). Adding a field is safe; anything else is a
  breaking change ([ADR 0003](docs/adr/0003-bunryu-compatibility-contract.md)).
- **The `goryu.json` keys** (six required, and the optional `output_passphrase`) and the
  two-URI command line.

## Highest priority: stability and reliability

Goryu sits in front of a live ingest path. It must run for weeks through bad
networks, a dead or rejecting downstream, and misbehaving encoders.

- One bad stream must not affect another or crash the process. A session failure
  ends that session, not the relay.
- Every retry, wait and buffer is bounded (backoff caps at 16 s, the takeover
  wait is 5 s, a status client gets 2 s, the session table has 256 entries). Don't add an unbounded loop,
  a tight retry or a growing allocation; no tight polling.
- The status thread never calls libsrt; session threads sample and the status
  thread copies ([ADR 0002](docs/adr/0002-status-thread-never-calls-libsrt.md)).
- The status port stays bound to `127.0.0.1`. It reports encoder addresses and has no
  authentication ([ADR 0013](docs/adr/0013-status-api-is-loopback-only-and-unauthenticated.md)).
- Shutdown must close sockets and join session threads (`on_signal` in
  `runtime.c`, `tracker_close_all`, `tracker_wait_all`); a change to session
  lifecycle is high-risk.
- Prefer small, reversible changes. No new dependencies; the build is one
  `gcc` line against libsrt, libssl and libcrypto.

## Logging

Every failure or recovery path should leave a line that explains it afterwards.

- Use `relay_logf` (timestamped); keep the existing `key=value` style
  (`streamid=`, `peer=`, `reason=`, `duration_ms=`).
- Bound anything that can repeat per packet or per failure. libsrt's own log is
  capped at 20 lines/s process-wide
  ([ADR 0007](docs/adr/0007-cap-libsrt-log-output-process-wide.md)). Don't raise
  libsrt's log level or add a per-packet log.
- Don't swallow an error: either log it or put it in `lastError`
  (process-level) or the stream's `lastError`, and say why it is tolerated.
- Logs are private and unredacted: streamids (often stream keys) and peer
  addresses appear in them. The passphrase must not: log a URI only through
  `redact_srt_uri()`, and never put a config value in an error message (config
  errors name the key and the passphrase's length, nothing more).

## Code map

Navigation only; correct an entry when a change makes it stale.

- `src/goryu.c` - entry point only: the listener, the accept loop and the shutdown
  order (`shutdown_relay`, the one place that order is written).
- `src/relay_options.c` - the command line (a config file or two URIs) and the
  `SRT_BONDING_STATUS_PORT` override turned into one options struct; pure, so a
  refusal is a message and is unit-tested.
- `src/session.c` - one session thread (`session_main`): streamid, the read/send
  loop (it wakes every `RECV_TIMEOUT_MS` so a closed session ends); `session_spawn`
  starts it.
- `src/output.c` - a session's connection to the downstream (`output_service`,
  `output_send`, `output_close`): connect, the backoff after every failed connect or
  send, the healthy period, and the tracker and stream telemetry kept in step. It
  reaches libsrt only through an `output_transport_t`; `src/srt_output.c` is the libsrt
  one (socket options, connect, reject reason, send). Unit-tested with a scripted
  transport and a fake clock.
- `src/tracker.c` - a session's place in the relay (slot table, `tracker_*`): its
  sockets (how one thread closes another's, how shutdown waits for all of them), the
  streamid it holds, the duplicate wait and takeover (`tracker_acquire_stream`), and
  `tracker_release`, the one call every session ends with. Unit-tested against a fake
  clock and a recorded `srt_close`, with `tests/unit/fake/srt/srt.h` standing in for libsrt.
- `src/stream_state.c` - the mutex-protected per-stream telemetry, at the session's
  slot (the same index as the tracker's), begun and ended by the tracker; the
  process `lastError`; `stream_state_snapshot`; `stream_state_sample_due`, the
  once-a-second sampling throttle. Its functions ignore a slot that holds no stream.
  Unit-tested with the log and clock stubbed.
- `src/stats.c` - the libsrt side of telemetry: reads counters inside
  `tracker_read_sockets` (the tracker lock held) into a plain `stats_sample_t` and applies no rule.
- `src/telemetry.c` - the rules that turn a sample into what the status API reports
  (which values are valid, totals only move forward, group latency, bonded leg
  join/leave); pure and unit-tested ([ADR 0014](docs/adr/0014-telemetry-rules-are-pure-and-libsrt-is-read-in-one-place.md)).
- `src/status_json.c`, `src/json_out.c` - the status JSON written from a snapshot it is
  handed (pure; the Bunryu null rules are pinned by `tests/unit/test_status_json.c`),
  and the JSON writers. `src/status_api.c` - the loopback HTTP thread around them.
- `src/timed_send.c` - send a buffer with one total deadline (the status reply;
  unit-tested over a socketpair).
- `src/relay_config.c/.h` - `goryu.json` parsing (a strict scanner, [ADR 0012](docs/adr/0012-goryu-json-is-read-by-a-strict-scanner.md)), the passphrase rule,
  `build_srt_uri`, `redact_srt_uri`, URI and query parsing (unit-tested).
- `src/srt_opts.c` (apply and check SRT options), `net.c` (addresses),
  `log.c` (timestamps, the libsrt log cap), `runtime.c` (run flag, signals,
  the monotonic `now_ms` for every decision, `wall_ms` for status timestamps),
  `backoff.c` (retry delays and one session's retry state, unit-tested),
  `ratelimit.c` (a window that lets a few events through and counts the rest,
  unit-tested; it bounds the bonded leg join/leave and the status-client log lines and
  libsrt's own log cap), `relay_common.h`
  (limits).
- `scripts/` - `build-local.sh` (Docker build into `objs/`), `build-release.sh`
  (archive + checksums into `build/`), `goryu.Dockerfile` (pinned base image and
  libsrt commit), `run.sh`, `smoke-test.sh`, `test-unit.sh`, `test-e2e.sh`,
  `test-docker.sh`, `format.sh`. `tests/unit/` holds the unit tests and `tests/e2e/` the bonded test caller.
- `docker/` - `Dockerfile` (runtime image that installs a published release; not the
  build image above), `compose.yaml` (MediaMTX + Goryu, and a bonded test sender under
  the `demo` profile), `goryu.json` (its example config), `README.dockerhub.md` (the Docker
  Hub description), `demo-sender/`.
- `.github/workflows/` - `ci.yml` (format, unit, smoke, end-to-end and Docker image tests), `release.yml`
  (tag must equal `VERSION`; builds, attests, publishes the release, then the Docker image).
  `.github/release-notes/<tag>.md` holds each release's hand-written notes.
- `docs/` - numbered section folders of numbered pages, indexed by
  `docs/README.md`; `README.md` is the short pitch and quick start. `docs/adr/` holds
  the numbered decision records, and `GLOSSARY.md` at the root the domain terms.

The code is C, built with `gcc -O2 -Wall -Wextra` (`scripts/goryu.Dockerfile`); keep
it warning-free. libsrt is C++ inside but `srt.h` is a plain C API, which is all
Goryu uses. Releases up to v3.2.1 compiled the same files with `g++` by habit
(inherited from the prototype); nothing needed it. Each header must include what it
uses, and the unit tests build the pure modules with `-Werror` under the sanitizers.

## Repo skills

Skills live in `skills/` and are shared with Claude Code and Codex through the
symlinks `.claude/skills` and `.agents/skills`; add new ones there, not in a tool
folder.

- `run-goryu` - build locally, run an isolated instance on spare ports with a
  local downstream and publisher, check `/status`, run the test scripts.
- `investigate-goryu` - read-only inspection of a deployed relay (status API,
  journald, service state), with the redaction rules for anything written down.

## Documentation is part of every change

When you change behavior, limits, the status API, configuration or an
operational guarantee, update every page that describes it in the same change.
Search `docs/` and `README.md` for the name of the thing.

- **Status API fields** live in `docs/4-reference/1-status-api.md`; keep its
  example and field notes equal to `status_json_write`.
- **Retry, takeover, logging and limits** live in
  `docs/3-operations/1-reliability.md` and the Limits table in
  `docs/1-getting-started/1-introduction.md`.
- **Config fields and command line** live in `docs/2-usage/1-configuration.md`.
- **Sender and server recipes** (what can send a bonded group, streamids and ports per
  downstream server, the tested versions) live in `docs/2-usage/2-sending-bonded-srt.md`
  and `docs/2-usage/3-downstream-servers.md`; re-run the test and update the versions
  when you claim a new server works.
- **Threads, session table, stats collection** live in
  `docs/4-reference/2-architecture.md`.
- **Build, CI, release and pinned inputs** live in
  `docs/4-reference/3-development.md`.
- **This guide's code map** when a code map entry goes stale.

Remove or correct text the change makes wrong; don't append. Keep the docs lean:
users get what a feature is and the steps to use it, developers get the real
problem that justifies a timeout, cap or backoff, with the incident or
measurement behind it (the log cap above is the model). A parameter with no
recorded reason gets questioned later. No changelog narration or text that
restates the code. Every new page must be linked from `docs/README.md`, and
relative links must resolve.

## Release notes

Every release gets `.github/release-notes/<tag>.md`, committed before the tag is
pushed; `release.yml` publishes it as the release body (a missing file falls back to
generated commit titles, which is not good enough). Write it for someone who runs
Goryu, not for a reviewer:

- One opening sentence on what changes for them, then bullets of user-visible changes:
  what it does and how to use it, not commit subjects or file names. Read the commits
  since the last tag, but describe behaviour.
- An **Upgrading** section that says whether the config, the status API or the log
  format changed, and that a restart drops active streams.
- An **Install** section (archive and checksum, the Docker one-liner) and the compare
  link at the end.
- Say only what the diff and the tests back up. No performance or compatibility claim
  that was not measured, and the same redaction rules as the docs (no streamids,
  passphrases or customer addresses).

Look at the newest file in that folder for the shape.

## Testing

Three layers, all run by CI (details in `docs/4-reference/3-development.md`):

- `bash scripts/test-unit.sh` - no Docker, a few seconds. Pure modules: config and
  URI parsing, passphrase rules, redaction, backoff, JSON writers, the deadline-bound send, the telemetry rules, the status JSON
  (every null rule), the stream state table, the command line.
- `bash scripts/smoke-test.sh` (needs Docker) - rebuilds, starts the binary from
  `goryu.json`, checks the usage failure, the status endpoint and the pid.
- `bash scripts/test-e2e.sh` (needs ffmpeg, jq, Docker; about three minutes) - the
  relay with real publishers and downstreams: forwarding, retry and recovery,
  duplicate rejection and stale takeover, passphrase handling, shutdown, bad config,
  connection churn, a downstream that accepts and then drops (`tests/e2e/closing_sink.c`),
  and bonded input via `tests/e2e/bonded_caller.c`.

Run all three for a code change. Rules:

- For a bug fix, write the failing test first and confirm it fails for the right
  reason (the passphrase bugs were pinned this way: red against the old binary,
  green after). Put the check in the unit tests when it is a pure function and in
  `test-e2e.sh` when it needs the process.
- A new behavior gets a scenario or unit check for the normal case and the failure
  case. A test that passes for the wrong reason is worse than none; see how the
  takeover scenario holds a connection open instead of `SIGSTOP`ping a publisher.
- For a change to session, retry or shutdown code, also try the failure by hand
  with `run-goryu` and say what you saw.
- Bonded input is simulated by `tests/e2e/bonded_caller.c` (legs on 127.0.0.1 and
  127.0.0.2). It covers legs, dedup, takeover and shutdown, but not packet loss,
  jitter or a real link failing: a change to loss, retransmission or latency
  handling still needs a test on real links, and you should say so if you could not.
- Reproduce a bug on both kinds of input. The first bonded-takeover test failed
  while the plain one passed, which is how a long-standing bug was found.

## Pre-commit checks

Before committing, review the complete diff for bugs, regressions, unsafe
assumptions and unintended changes. Run `bash scripts/format.sh` for any change to
`src/` or `tests/`, and the three test layers for every code change. A documentation-only change needs
no build; check by eye that relative links and anchors resolve.

Never run `git push`, create or push a tag, or open or merge a PR unless the user
explicitly asks for it in that request; committing stays local. A release is a
tag equal to `VERSION`; a published release is never overwritten, so fix forward
with a new version.

**This repository is public on GitHub.** Never commit real streamids that are
stream keys, SRT passphrases, downstream hostnames that identify a customer, or
encoder/customer IP addresses, in code, config, docs or examples (use
placeholders such as `camera1/main`, `10.0.1.20`, `<redacted-srt-passphrase>`).
The checked-in `goryu.json` must keep `"passphrase": ""`. Log and status excerpts
pasted into docs, commit messages or issues are the usual leak: re-read them line
by line. If a real secret is committed and pushed, treat it as compromised and
rotate it; a history rewrite alone does not undo the exposure.
