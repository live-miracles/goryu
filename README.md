<img src=".github/assets/logo.png" alt="Goryu logo" width="96" align="right" />

# Goryu

_Goryu (合流, "confluence"): many network paths, one stream._

[![CI](https://github.com/live-miracles/goryu/actions/workflows/ci.yml/badge.svg)](https://github.com/live-miracles/goryu/actions/workflows/ci.yml)
[![License: MIT](https://img.shields.io/badge/license-MIT-green.svg)](LICENSE)

**A small SRT bonding relay for Linux.** An encoder sends one stream over several network links (two uplinks, two SIMs) as a bonded SRT group. Goryu accepts the group on one port, merges the legs into a single deduplicated stream, and forwards it to a downstream SRT server. An ordinary single-link SRT caller works on the same port.

Built on [libsrt](https://github.com/Haivision/srt) 1.5.6 socket-group bonding. It is one C binary with a six-field JSON config and a loopback JSON status API, and it is the bonded-SRT ingress for [Bunryu](https://github.com/live-miracles/bunryu).

```text
        encoder / publisher

   leg A: SRT caller + streamid
   leg B: SRT caller + same streamid
              |
              v
   +-----------------------------+
   | goryu listener              |
   | SRTO_GROUPCONNECT=1         |
   +-----------------------------+
              |
              | SRT group socket
              | libsrt merges and deduplicates legs
              v
   +-----------------------------+
   | session thread per streamid |
   | reads one MPEG-TS payload   |
   +-----------------------------+
              |
              +----> srt:// output, same streamid
```

## Features

- [Bonded and plain SRT](docs/1-getting-started/1-introduction.md) on one listener; the status API tells them apart.
- [Streamid passthrough](docs/2-usage/1-configuration.md#streamid-routing): one listener serves many streams, each forwarded under its own streamid.
- [Passphrase](docs/2-usage/1-configuration.md#passphrase) applied to the input and the downstream connection.
- [Per-leg telemetry](docs/4-reference/1-status-api.md): RTT, loss, latency, bandwidth and buffer occupancy for every leg, plus the output connection.
- [Built to stay up](docs/3-operations/1-reliability.md): bounded retry with backoff to the downstream, takeover of a stale publisher, and a hard cap on log volume.

## Quick start

```bash
curl -fsSL -o goryu.tar.gz https://github.com/live-miracles/goryu/releases/latest/download/goryu-linux-x86_64.tar.gz
mkdir goryu && tar -xzf goryu.tar.gz -C goryu && cd goryu
```

Create `goryu.json` (the downstream must be an SRT server that accepts the publish):

```json
{
  "input_host": "0.0.0.0",
  "input_port": 10081,
  "output_host": "127.0.0.1",
  "output_port": 10080,
  "status_port": 8081,
  "passphrase": ""
}
```

Run it, then check it is up:

```bash
LD_LIBRARY_PATH=lib ./bin/goryu goryu.json
curl http://127.0.0.1:8081/status
```

Point the encoder at `srt://<host>:10081` with a `streamid`. See [Install](docs/1-getting-started/2-install.md) for running it under systemd and the firewall port, and [Configuration](docs/2-usage/1-configuration.md) for every field.

## Documentation

- **Getting started:** [Introduction](docs/1-getting-started/1-introduction.md) · [Install](docs/1-getting-started/2-install.md)
- **Usage:** [Configuration](docs/2-usage/1-configuration.md)
- **Operations:** [Reliability](docs/3-operations/1-reliability.md)
- **Reference:** [Status API](docs/4-reference/1-status-api.md) · [Architecture](docs/4-reference/2-architecture.md) · [Development](docs/4-reference/3-development.md)

## License

[MIT](LICENSE). libsrt and OpenSSL are separate projects under their own licenses.
