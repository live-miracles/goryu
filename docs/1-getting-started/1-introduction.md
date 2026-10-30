# Introduction

Goryu is a standalone SRT relay. It listens on one SRT port, accepts the
encoder's connection, and forwards the MPEG-TS payload to a downstream SRT
server, copying the caller's `streamid` onto the outgoing connection.

It exists because SRT _bonding_ (one stream sent over several links at once)
is implemented in libsrt as a socket group, and most SRT servers cannot accept a
group. Goryu terminates the group, so the server behind it only ever sees an
ordinary single SRT publish.

## Bonded and plain input

The listener is bound with `SRTO_GROUPCONNECT=1`, so it accepts both kinds of
caller:

- **Bonded.** The encoder connects two or more redundant legs (for example two
  NICs or uplinks) as one socket group, either a broadcast group (every leg carries
  the whole stream) or a main/backup group (one leg carries it, the others stand
  by); the encoder chooses. libsrt merges and deduplicates the legs
  before Goryu reads anything, so Goryu always reads one clean stream per group.
  A group with a single leg connected is still bonded.
- **Plain.** A caller using one ordinary SRT socket is accepted by the same
  listener and handled by the same session code, just without legs.

The [status API](../4-reference/1-status-api.md) reports which one a stream is
(`input.bonded`) and, for a plain input, the encoder's address (`input.peer`).

## Data path

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

## With Bunryu

[Bunryu](https://github.com/live-miracles/bunryu) runs Goryu as a shared relay
in front of SRS: encoders publish bonded SRT to Goryu's port (`10081`), and
Goryu forwards each stream to SRS's SRT port (`10080`). Bunryu generates
`goryu.json` and polls the status API for per-leg telemetry. Bunryu does not use
the optional [`output_passphrase`](../2-usage/1-configuration.md#destination-passphrase):
the SRS it forwards to shares the one passphrase, and Bunryu's installer rewrites
`goryu.json` from its own copy on every update, so a key added by hand would be lost.

## Limits

| Limit | Value |
|---|---:|
| Session table entries | 256 |
| Listen backlog | 64 |
| Time a status API client gets to send its request, and again to take the reply | 2 s each |
| Input listener address family | IPv4 |

Goryu forwards; it does not decode, transcode, or buffer beyond SRT's own
latency window, so CPU use is low and scales with total bitrate.
