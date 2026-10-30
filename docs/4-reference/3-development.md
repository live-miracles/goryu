# Development

## Build

```bash
bash scripts/build-local.sh
./objs/goryu --version
```

The local build compiles libsrt 1.5.6 with bonding in Docker and produces
`./objs/goryu` and its libraries in `./objs/lib`.

## Run and test

```bash
bash scripts/run.sh          # runs ./objs/goryu with ./goryu.json
bash scripts/format.sh       # format src/ and tests/ (clang-format)
bash scripts/format.sh check # check only, as CI does
bash scripts/test-unit.sh    # unit tests (gcc with sanitizers; no Docker)
bash scripts/smoke-test.sh   # build, start with goryu.json, check /status
bash scripts/test-e2e.sh     # end-to-end scenarios against ./objs/goryu
bash scripts/test-docker.sh  # the image and compose file in docker/ (DEMO=1 adds a bonded stream)
```

`run.sh` honours `GORYU_PATH`, `GORYU_LIB_DIR` and `GORYU_CONFIG_PATH`. The smoke
test checks that a call with no arguments prints the usage text and fails, that
the process starts from `goryu.json`, and that the status endpoint answers with
a `streamStates` field and the pid of the process it started.

### Tests

- **Unit tests** (`tests/unit/`) cover the code that needs no libsrt: config and
  URI parsing, the passphrase rules and redaction, the retry backoff, the JSON
  writers, the deadline-bound send (over a socketpair), the telemetry rules (a libsrt
  reading is a struct), the status JSON with every null rule, the stream state
  table (its log and clock stubbed), a session's place in the relay (the tracker: the
  duplicate wait, takeover and release against a fake clock and a recorded `srt_close`,
  built with a stand-in `srt/srt.h` from `tests/unit/fake/`), a session's connection to
  the downstream (the output: connect, the whole retry schedule and the healthy period
  over a scripted transport and the same fake clock), and the command line (every
  way it is refused). Each test is built with the release's `gcc -Wall -Wextra` plus `-Werror`,
  under AddressSanitizer and UndefinedBehaviorSanitizer. A new check
  goes in the matching `tests/unit/test_*.c`.
- **End-to-end tests** (`scripts/test-e2e.sh`) start the relay, a downstream and
  ffmpeg publishers on spare ports (`E2E_BASE_PORT`, default 24000) and a private
  work dir, so a running relay is not touched. They need `ffmpeg` with SRT, `curl`,
  `jq` and `ss` (plus Docker for the bonded caller and the closing sink), and take about three minutes.
  A scenario with a failed check prints the relay's last 40 log lines, the tails of the
  publisher, downstream sink and bonded caller logs, and the status
  API response, because the work dir is gone once CI ends. Pass scenario names to run some: `cli bad_config uri_mode forward downstream_down
  downstream_drops duplicate takeover two_streams passphrase sigterm shutdown_silent churn bonded
  bonded_legs bonded_flap bonded_backup bonded_duplicate bonded_takeover bonded_passphrase bonded_shutdown
  bonded_abrupt_end unknown_key status_idle_client`.
- **Config and status edge cases.** `bad_config` refuses malformed files (a duplicate key,
  a nested object, text after the object, a missing comma) and `unknown_key` checks that a
  misspelled key starts the relay and is named in the log. `status_idle_client` opens a
  connection to the status port that sends nothing (bash's `/dev/tcp`) and checks that the
  endpoint answers again within 8 s and that `SIGTERM` still stops the relay within 6 s.
- **Bonded input is simulated.** ffmpeg cannot open a socket group, so
  `tests/e2e/bonded_caller.c` does: a small libsrt caller that reads MPEG-TS from
  stdin and sends it as a broadcast group (or a main/backup one, `--type backup
  --weights 10,5`) with one leg per `--locals` address. Legs
  bound to `127.0.0.1` and `127.0.0.2` look like two links to the relay, and the
  caller can drop a leg or add one later. The scenarios `bonded*` check the legs in
  the status API, deduplication (each packet forwarded once, not once per leg), a
  single-leg and a late-joining group, a dropped leg, duplicates and stale takeover,
  passphrase, SIGTERM, a vanished caller, a main/backup group (`bonded_backup`: the standby
  is idle and takes over when the main leg is closed) and a flapping leg (`bonded_flap`:
  its join and leave log lines are limited, then summarised); `churn` repeats connect/disconnect
  and checks that file descriptors and threads do not grow. The caller is built once
  into `objs/bonded-caller` with the Docker image `scripts/build-local.sh` makes;
  `E2E_SKIP_BONDED=1` skips these scenarios. What it cannot do is real network
  conditions: loss, jitter, latency or a physical link failing, so the per-leg loss
  and retransmission counters are only checked for presence, not for values.
- **A downstream that accepts and then drops** is `tests/e2e/closing_sink.c`: a libsrt
  listener that accepts the handshake and closes the connection 50 ms later, as a
  downstream does when a publish hook refuses the stream key. ffmpeg as a listener
  cannot do it. `downstream_drops` uses it with a 4 Mbps publisher (`PUB_RATE`) and checks that
  the retry delay keeps doubling: a quiet publisher would let a fix that is wrong for a
  real encoder pass, because it rarely has a packet to send inside those 50 ms. It is
  built into `objs/closing-sink` like the bonded caller.
- **Docker packaging** (`scripts/test-docker.sh`) builds `docker/Dockerfile`, which
  installs a published release, and checks the version, that it does not run as root,
  that a missing config is refused, that the status API answers, that `docker stop`
  ends it with exit 0 in under 5 s (it is PID 1, so this proves SIGTERM is handled), and
  that `compose.yaml` is valid. It tests the packaging of a release, not the commit's
  source. `DEMO=1` also runs the compose stack (which pulls the published `latest` image) with the
  bonded test sender and reads the stream back from MediaMTX; the sender build compiles
  libsrt once, so it is not in CI.
- The takeover scenario holds a publisher's SRT connection open without sending
  media (ffmpeg remuxing a FIFO). A `SIGSTOP`ped ffmpeg does not work: libsrt's own
  5 s idle timeout drops its connection before the relay's staleness check can run.

## CI

`.github/workflows/ci.yml` runs on every push to `master` or `main` and on pull
requests:

- **Format check:** `clang-format` over the C sources and tests.
- **Unit tests:** `scripts/test-unit.sh`.
- **Build, smoke and end-to-end tests:** the Docker build, `scripts/smoke-test.sh`,
  then `scripts/test-e2e.sh`.
- **Docker image:** `scripts/test-docker.sh`.

The release workflow (`release.yml`) runs that CI, builds and attests the archive,
publishes the GitHub Release, and then builds `docker/Dockerfile` for that tag and
pushes it to GHCR and, when the `DOCKERHUB_USERNAME` and `DOCKERHUB_TOKEN` secrets
are set, Docker Hub (`nandisha0/goryu`, description synced from
`docker/README.dockerhub.md`). The image job runs after the release because the image
installs it, and it checks that the pushed image reports the tag's version. If it
fails, the release stays published; re-run the job, since a published release is never
overwritten.

## Versioning

The version lives in the `VERSION` file (for example `v3.2.0`) and is embedded
into the binary at build time, so `goryu --version` prints it. Changing
`VERSION` after an archive was built does not change that archive.

## Releases

A release is created by pushing a tag equal to `VERSION`. To cut one, update
`VERSION`, write `.github/release-notes/<tag>.md` (see the newest one for the shape:
what changed for users, an Upgrading section, install lines and the compare link),
commit, then:

```bash
git tag v3.2.1
git push origin master v3.2.1
```

The release workflow (`.github/workflows/release.yml`) runs the CI jobs first,
then fails if the tag and `VERSION` differ, builds the archive with Docker,
attests its build provenance, and creates the GitHub Release with the notes file
(generated commit titles with a warning if the file is missing). The release
carries the archive and `checksums.sha256`.

A published release is never overwritten: if the release already exists the
workflow fails. Fix forward with a new version.

To build the archive locally:

```bash
bash scripts/build-release.sh   # writes build/goryu-linux-x86_64.tar.gz and build/checksums.sha256
```

### Pinned build inputs

`scripts/goryu.Dockerfile` pins the base image by digest and checks that the
libsrt tag resolves to a known commit, so a moved tag fails the build instead of
changing what ships. To update either one:

- Base image: `docker buildx imagetools inspect ubuntu:22.04`, then copy the
  top-level `Digest:` into the `FROM` line.
- libsrt: `git ls-remote https://github.com/Haivision/srt 'refs/tags/vX.Y.Z'`,
  then set `SRT_TAG` and `SRT_COMMIT` in the Dockerfile. To try another version
  locally without editing files, set both `SRT_TAG` and `SRT_COMMIT` in the
  environment.

The libsrt pin is written once: `scripts/build-local.sh`, `scripts/test-e2e.sh` and
`docker/demo-sender/Dockerfile` read the tag (and the demo sender the commit) out of
`scripts/goryu.Dockerfile`, so they follow without an edit.

`docker/Dockerfile` repeats the base image digest (it must match the one the release
is built on, because the archive links against that glibc), so change both together.
It is a copy because that image is built from `docker/` alone, which cannot read the
other file; `scripts/test-docker.sh` fails when the two differ, and CI and the release
run it.

Projects that pin a Goryu release (such as Bunryu's installer and Docker image)
download `goryu-linux-x86_64.tar.gz` from the release, so the asset name is part
of the interface; keep it stable.
