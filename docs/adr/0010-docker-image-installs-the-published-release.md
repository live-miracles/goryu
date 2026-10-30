# The Docker image installs the published release instead of building from source

`docker/Dockerfile` downloads the release archive and checks it against `checksums.sha256`, so the image runs exactly the archive's binary and libraries and there is no second copy of the libsrt pins. The image is built by a job in `release.yml` that runs after the release job: the image needs the release to exist, and a release created with `GITHUB_TOKEN` does not trigger other workflows.

## Consequences

`scripts/test-docker.sh` tests the packaging of a release, not the commit's source. The image repeats the base image's digest because the archive links against that glibc, so the two Dockerfiles change together. If the image job fails the release stays published; re-run the job.
