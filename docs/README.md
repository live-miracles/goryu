# Goryu documentation

## Getting started

- [Introduction](1-getting-started/1-introduction.md): what it is, how bonded and plain SRT are handled, limits
- [Install](1-getting-started/2-install.md): release download, systemd, Docker, firewall port
- [Alternatives](1-getting-started/3-alternatives.md): native server bonding, SRTLA, vendor bonding, and when Goryu is the right tool

## Usage

- [Configuration](2-usage/1-configuration.md): command line, `goryu.json`, streamid routing, passphrase
- [Sending bonded SRT](2-usage/2-sending-bonded-srt.md): what a sender must do, what can send one, a test caller you can run today
- [Downstream servers](2-usage/3-downstream-servers.md): SRS, MediaMTX and Restreamer behind Goryu, with the streamid each expects

## Operations

- [Reliability](3-operations/1-reliability.md): retry, duplicate publishers, log cap, and what is and is not logged

## Reference

- [Status API](4-reference/1-status-api.md): the loopback JSON endpoint and every field in it
- [Architecture](4-reference/2-architecture.md): threads, session table, how stats are collected
- [Development](4-reference/3-development.md): build, tests, versioning and releases
