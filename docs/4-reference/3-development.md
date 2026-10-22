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
```

`run.sh` honours `GORYU_PATH`, `GORYU_LIB_DIR` and `GORYU_CONFIG_PATH`. The smoke
test checks that a call with no arguments prints the usage text and fails, that
the process starts from `goryu.json`, and that the status endpoint answers with
a `streamStates` field and the pid of the process it started.

### Tests

- **Unit tests** (`tests/unit/`) cover the code that needs no libsrt: config and
  URI parsing, the passphrase rules and redaction, the retry backoff and the JSON
  writers. Each test is built with the release's `gcc -Wall -Wextra` plus `-Werror`,
  under AddressSanitizer and UndefinedBehaviorSanitizer. A new check
  goes in the matching `tests/unit/test_*.c`.
- **End-to-end tests** (`scripts/test-e2e.sh`) start the relay, a downstream and
  ffmpeg publishers on spare ports (`E2E_BASE_PORT`, default 24000) and a private
  work dir, so a running relay is not touched. They need `ffmpeg` with SRT, `curl`,
  `jq` and `ss` (plus Docker for the bonded caller), and take about three minutes.
  Pass scenario names to run some: `cli bad_config uri_mode forward downstream_down
  duplicate takeover two_streams passphrase sigterm shutdown_silent churn bonded
  bonded_legs bonded_flap bonded_duplicate bonded_takeover bonded_passphrase bonded_shutdown
  bonded_abrupt_end`.
- **Bonded input is simulated.** ffmpeg cannot open a socket group, so
  `tests/e2e/bonded_caller.c` does: a small libsrt caller that reads MPEG-TS from
  stdin and sends it as a broadcast group with one leg per `--locals` address. Legs
  bound to `127.0.0.1` and `127.0.0.2` look like two links to the relay, and the
  caller can drop a leg or add one later. The scenarios `bonded*` check the legs in
  the status API, deduplication (each packet forwarded once, not once per leg), a
  single-leg and a late-joining group, a dropped leg, duplicates and stale takeover,
  passphrase, SIGTERM, a vanished caller and a flapping leg (`bonded_flap`: its join and
  leave log lines are limited, then summarised); `churn` repeats connect/disconnect
  and checks that file descriptors and threads do not grow. The caller is built once
  into `objs/bonded-caller` with the Docker image `scripts/build-local.sh` makes;
  `E2E_SKIP_BONDED=1` skips these scenarios. What it cannot do is real network
  conditions: loss, jitter, latency or a physical link failing, so the per-leg loss
  and retransmission counters are only checked for presence, not for values.
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

## Versioning

The version lives in the `VERSION` file (for example `v3.2.0`) and is embedded
into the binary at build time, so `goryu --version` prints it. Changing
`VERSION` after an archive was built does not change that archive.

## Releases

A release is created by pushing a tag equal to `VERSION`. To cut one, update
`VERSION`, commit, then:

```bash
git tag v3.2.1
git push origin master v3.2.1
```

The release workflow (`.github/workflows/release.yml`) runs the CI jobs first,
then fails if the tag and `VERSION` differ, builds the archive with Docker,
attests its build provenance, and creates the GitHub Release with generated
notes. The release carries the archive and `checksums.sha256`.

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
  then set `SRT_TAG` and `SRT_COMMIT` in the Dockerfile and `SRT_TAG` in
  `scripts/build-local.sh`. To try another version locally without editing
  files, set both `SRT_TAG` and `SRT_COMMIT` in the environment.

Projects that pin a Goryu release (such as Bunryu's installer and Docker image)
download `goryu-linux-x86_64.tar.gz` from the release, so the asset name is part
of the interface; keep it stable.
