# Goryu documentation

## Getting started

- [Introduction](1-getting-started/1-introduction.md): what it is, how bonded and plain SRT are handled, limits
- [Install](1-getting-started/2-install.md): release download, systemd, Docker, firewall port
- [Alternatives](1-getting-started/3-alternatives.md): native server bonding, srt-live-transmit (tested as a relay), SRTLA, vendor bonding, and when Goryu is the right tool

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

## Decisions

Why Goryu is built the way it is; each record names the alternatives that were rejected.

- [0001 Terminate the bonded group in Goryu](adr/0001-terminate-the-bonded-group-in-goryu.md)
- [0002 The status thread never calls libsrt](adr/0002-status-thread-never-calls-libsrt.md)
- [0003 The status API, config keys and release asset are a contract with Bunryu](adr/0003-bunryu-compatibility-contract.md)
- [0004 Clear the retry count only after the output stays up](adr/0004-clear-the-retry-count-only-after-the-output-stays-up.md)
- [0005 A duplicate publisher waits, then takes over a stale session](adr/0005-duplicate-publisher-waits-then-takes-over-a-stale-session.md)
- [0006 A refused SRT option stops the relay](adr/0006-a-refused-srt-option-stops-the-relay.md)
- [0007 Cap libsrt's log output process-wide](adr/0007-cap-libsrt-log-output-process-wide.md)
- [0008 The destination has its own optional passphrase](adr/0008-destination-has-its-own-optional-passphrase.md)
- [0009 Reproducible releases that are never overwritten](adr/0009-reproducible-releases-that-are-never-overwritten.md)
- [0010 The Docker image installs the published release](adr/0010-docker-image-installs-the-published-release.md)
- [0011 A socket is untracked before it is closed](adr/0011-a-socket-is-untracked-before-it-is-closed.md)
- [0012 goryu.json is read by a strict scanner](adr/0012-goryu-json-is-read-by-a-strict-scanner.md)
- [0013 The status API is loopback-only and unauthenticated](adr/0013-status-api-is-loopback-only-and-unauthenticated.md)
