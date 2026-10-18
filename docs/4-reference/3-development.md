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
bash scripts/smoke-test.sh   # build, start with goryu.json, check /status
bash scripts/format.sh       # format the C sources (clang-format)
bash scripts/format.sh check # check only, as CI does
```

`run.sh` honours `GORYU_PATH`, `GORYU_LIB_DIR` and `GORYU_CONFIG_PATH`. The smoke
test checks that a call with no arguments prints the usage text and fails, that
the process starts from `goryu.json`, and that the status endpoint answers with
a `streamStates` field and the pid of the process it started.

## CI

`.github/workflows/ci.yml` runs on every push to `master` or `main` and on pull
requests:

- **Format check:** `clang-format` over the C sources.
- **Build and smoke test:** the Docker build, then `scripts/smoke-test.sh`.

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

The release workflow (`.github/workflows/release.yml`) fails if the tag and
`VERSION` differ, builds the archive with Docker, and creates the GitHub Release
with generated notes. If the release already exists it replaces the archive.

To build the archive locally:

```bash
bash scripts/build-release.sh   # writes build/goryu-linux-x86_64.tar.gz
```

To replace the asset on an existing release by hand:

```bash
gh release upload "$(cat VERSION)" build/goryu-linux-x86_64.tar.gz --clobber
```

Projects that pin a Goryu release (such as Bunryu's installer and Docker image)
download `goryu-linux-x86_64.tar.gz` from the release, so the asset name is part
of the interface; keep it stable.
