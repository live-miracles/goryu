# Install

Goryu runs as a single process. Each release publishes one archive,
`goryu-linux-x86_64.tar.gz`, containing the binary and the shared libraries it
needs (libsrt 1.5.6 built with bonding, OpenSSL's libcrypto, libstdc++,
libgcc), so it does not depend on the host's libsrt.

## Download

```bash
curl -fsSL -o goryu.tar.gz https://github.com/live-miracles/goryu/releases/latest/download/goryu-linux-x86_64.tar.gz
mkdir goryu && tar -xzf goryu.tar.gz -C goryu
```

Each release also publishes `checksums.sha256`. To verify a download, fetch it
next to the archive and run `sha256sum --check checksums.sha256`, or check the
build attestation with `gh attestation verify goryu.tar.gz --repo live-miracles/goryu`
(needs a recent GitHub CLI; it prints nothing and exits 0 when the check passes
outside a terminal, so use `--format json` to see the details).

The archive unpacks to `bin/goryu` and `lib/`. To pin a version, replace
`latest/download` with `download/vX.Y.Z`. Check what you have with:

```bash
LD_LIBRARY_PATH=goryu/lib goryu/bin/goryu --version
```

## Run

Write a [`goryu.json`](../2-usage/1-configuration.md), then:

```bash
LD_LIBRARY_PATH=goryu/lib goryu/bin/goryu goryu.json
curl http://127.0.0.1:8081/status
```

Set `LD_LIBRARY_PATH` on the goryu command only, as above. Exporting it for
the whole shell makes other tools (`curl`, for one) load the bundled
`libcrypto` and fail.

## systemd

A minimal unit, assuming the archive is unpacked to `/opt/goryu` and the config
is at `/etc/goryu/goryu.json`:

```ini
[Unit]
Description=Goryu SRT bonding relay
After=network-online.target
Wants=network-online.target

[Service]
Type=simple
Environment=LD_LIBRARY_PATH=/opt/goryu/lib
ExecStart=/opt/goryu/bin/goryu /etc/goryu/goryu.json
Restart=always
RestartSec=2
LimitNOFILE=1048576
NoNewPrivileges=true
PrivateTmp=true

[Install]
WantedBy=multi-user.target
```

Add `User=` and `Group=` to run it as an unprivileged account; the input port
is above 1024 and needs no special privileges. Goryu only reads its config at
startup, so restart the service after changing `goryu.json`.

## Firewall

Open the encoder-facing port (`input_port`) for **UDP**. The status port is
bound to `127.0.0.1` and must stay closed to the network.

## Docker

`docker/` holds a runtime image and a compose example. The image installs a published
release and checks it against `checksums.sha256`, runs as an unprivileged user, and
stops cleanly on `docker stop`. It is linux/amd64 only, like the release. From v3.4.0 each
release publishes it as `nandisha0/goryu` on Docker Hub and `ghcr.io/live-miracles/goryu`,
tagged `X.Y.Z`, `X.Y` and `latest`; pin an exact version for anything long-lived.

```bash
docker run -d --name goryu --network host --restart unless-stopped \
  -v "$PWD/goryu.json:/etc/goryu/goryu.json:ro" nandisha0/goryu:3.4.0
curl http://127.0.0.1:8081/status
```

To build it yourself instead (the image installs a published release):

```bash
docker build -t goryu docker                                             # newest release
docker build --build-arg GORYU_VERSION=v3.4.0 -t goryu:v3.4.0 docker     # a pinned one
```

Use host networking: the status API listens on `127.0.0.1` inside the container, so a
published port cannot reach it. The config must be readable by uid 10001 (`chmod 644`),
and is read only at startup, so `docker restart goryu` after changing it. The log is
the container's stdout (`docker logs goryu`). Open the input port for UDP as in
[Firewall](#firewall).

To see the whole path with no hardware, the compose file runs MediaMTX and Goryu, and
its `demo` profile adds a bonded test sender (a test pattern over two legs on
`127.0.0.1` and `127.0.0.2`):

```bash
docker compose -f docker/compose.yaml up -d --build
docker compose -f docker/compose.yaml --profile demo up -d --build sender
curl -s http://127.0.0.1:8081/status | jq '.streamStates[0] | {streamId, bonded: .input.bonded, legs: (.input.legs | length)}'
ffplay 'srt://127.0.0.1:8890?streamid=read:demo'
```

The first sender build compiles libsrt, which takes a few minutes. Stop everything with
`docker compose -f docker/compose.yaml --profile demo down`.

## Bunryu

If you use Bunryu you do not install Goryu by hand: Bunryu's installer
downloads a pinned release and sets up `goryu.service`, and its Docker image
bundles it.
